/*
 * SPDX-License-Identifier: MIT
 *
 * rss-reader-script: RSS Reader's scripted runner (the .script files of tests/flows) on the shared headless harness's
 * platform (apps/_common/headless: a plain LVGL display at any size, the layout service's metrics, cp0_lvgl's keyboard
 * queue and bridge, a pointer, the layout checks), with the app started by its own AppSpec (cdk::start, as on a board).
 * The network is a fake (no network in the tests): every request is answered from tests/data/ui by the routes file, or
 * fails the way the route says. The clock is fixed (RSS_TEST_NOW) so the ages in the shots do not move. Built by
 * tests/flows/runner.mk (build.sh host).
 *
 *   rss-reader-script --script FILE [--shot-dir DIR] [--size WxH] [--ppmm N] [--resources DIR]
 *   environment: RSSREADER_DATA (the store), HOME (exports, imports), RSS_FAKE_ROUTES (the routes file), RSS_FAKE_ROOT
 *   (its files' folder), RSS_TEST_NOW (epoch seconds), CDK_HEADLESS_RESOURCES (the launcher's resources: fonts)
 *
 * Script (one command per line, # starts a comment; "quoted words"):
 *   seed pin <catalog id> | seed refresh | seed read <feed id> <n oldest> | seed save <feed id> <index> | seed file <fixture> <name in HOME>
 *                         | seed text_size small|medium|large | seed refresh_on_open on|off | seed images always|off | seed cap <MB>
 *                         | seed filler <KB> (a saved article of that size) | seed hint seen (the options were opened before)
 *                         run before the app starts, in the order written (the store is seeded through the core's API)
 *   (a label: its exact text, or ^ and the start of it)
 *   wait <ms>   key <name>   type <text>   tap "label" [last]   tapat <fx> <fy>   tapfield <n>   tapkey "label"   tapimage <n>
 *   hold "label" <ms> [shot]   holdat <fx> <fy> <ms> [shot]   drag <fx1> <fy1> <fx2> <fy2>   (fractions of the screen)
 *   net online | offline | slow <ms>      the fake network: answers / every request fails with "no network" / each answer late
 *   keyboard on|off             input.state says a keyboard is there (no on-screen keyboard) or not
 *   shot <name>   check <name> [fullscreen]   dump   mark   say <text>   (fullscreen: the header rule is left out, for the
 *                         full-screen image that covers the header on purpose)
 *   expect text "T" | notext "T" | osk on|off | quit | noquit | state KEY VALUE | same KEY | changed KEY
 *   key names: esc enter tab backtab up down left right pgup pgdn home end bksp del space, ctrl-<letter>
 */

#include "app.hpp"
#include "headless.hpp"
#include "hooks.hpp"
#include "session.hpp"

#include "catalog.hpp"
#include "fsutil.hpp"
#include "refresh.hpp"
#include "store.hpp"

#include "cp0_ui_metrics_lvgl.h"
#include "cp0_ui_roller.h"
#include "cp0_ui_widgets.hpp"
#include "keyboard_input.h"
#include "lvgl/src/misc/lv_event_private.h"

#include <linux/input-event-codes.h>
#include <sys/stat.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

using namespace rssui;

namespace {

int g_w = 640, g_h = 480;
std::string g_runtime;
unsigned g_input_gen = 0;
std::atomic<bool> g_quit{false};

/* ------------------------------------------------------------------ the fake network */

class FakeNet {
public:
    void load(const std::string &routes, const std::string &root)
    {
        root_ = root;
        std::ifstream in(routes);
        std::string line;
        while (std::getline(in, line)) {
            if (line.empty() || line[0] == '#') continue;
            std::istringstream ss(line);
            std::string url, target;
            ss >> url >> target;
            if (!url.empty() && !target.empty()) routes_[url] = target;
        }
    }
    void set_offline(bool on) { offline_.store(on); }
    void set_delay(int ms) { delay_ms_.store(ms); }

    rssreader::HttpResponse get(const rssreader::HttpRequest &req)
    {
        rssreader::HttpResponse r;
        r.final_url = req.url;
        for (int t = 0; t < delay_ms_.load(); t += 20) {
            if (req.cancel && req.cancel->load()) {
                r.error = rssreader::FetchError::Cancelled;
                return r;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        if (req.cancel && req.cancel->load()) {
            r.error = rssreader::FetchError::Cancelled;
            return r;
        }
        std::lock_guard<std::mutex> g(mu_);
        ++requests_;
        if (offline_.load()) {
            r.error = rssreader::FetchError::Network;
            r.error_detail = "Could not resolve host";
            return r;
        }
        std::string target;
        for (const std::string &u : {req.url, req.url + "/", req.url.size() > 1 && req.url.back() == '/' ? req.url.substr(0, req.url.size() - 1) : req.url}) {
            auto it = routes_.find(u);
            if (it != routes_.end()) {
                target = it->second;
                break;
            }
        }
        if (target.empty() || target == "!404") {
            r.error = rssreader::FetchError::Http;
            r.status = 404;
            return r;
        }
        if (target == "!503") {
            r.error = rssreader::FetchError::Http;
            r.status = 503;
            return r;
        }
        if (target == "!network") {
            r.error = rssreader::FetchError::Network;
            r.error_detail = "Connection refused";
            return r;
        }
        if (target == "!timeout") {
            r.error = rssreader::FetchError::Timeout;
            return r;
        }
        if (target == "!tls") {
            r.error = rssreader::FetchError::Tls;
            r.error_detail = "certificate expired";
            return r;
        }
        if (!rssreader::read_file(root_ + "/" + target, r.body)) {
            r.error = rssreader::FetchError::Http;
            r.status = 404;
            return r;
        }
        r.status = 200;
        const std::string ext = target.substr(target.rfind('.') + 1);
        r.content_type = ext == "html" ? "text/html" : ext == "atom" ? "application/atom+xml" : ext == "png" ? "image/png"
                         : ext == "jpg" ? "image/jpeg" : ext == "gif" ? "image/gif" : ext == "webp" ? "image/webp" : "application/rss+xml";
        if (r.body.size() > req.max_bytes) {
            r.error = rssreader::FetchError::TooLarge;
            r.body.clear();
        }
        return r;
    }
    int requests() const { return requests_; }

private:
    std::mutex mu_;
    std::map<std::string, std::string> routes_;
    std::string root_;
    std::atomic<bool> offline_{false};
    std::atomic<int> delay_ms_{0};
    int requests_ = 0;
};

FakeNet g_net;

class FakeClient : public rssreader::HttpClient {
public:
    rssreader::HttpResponse get(const rssreader::HttpRequest &r) override { return g_net.get(r); }
};

/* ------------------------------------------------------------------ helpers */

void write_input_state(int keyboards)
{
    if (g_runtime.empty()) return;
    const std::string path = g_runtime + "/input.state", tmp = path + ".tmp";
    {
        std::ofstream f(tmp);
        f << "v=1\nkeyboards=" << keyboards << "\nlast_seen=0\nbt_keyboards=0\nkb_gone=0\nbt_gone=0\nvkb=auto\ngen=" << ++g_input_gen << "\n";
    }
    std::rename(tmp.c_str(), path.c_str());
}

void on_signal(int) { g_quit.store(true); }

struct KeySpec {
    uint32_t code = 0;
    uint32_t mods = 0;
    std::string utf8;
};

uint32_t letter_code(char c)
{
    static const uint32_t letters[26] = {KEY_A, KEY_B, KEY_C, KEY_D, KEY_E, KEY_F, KEY_G, KEY_H, KEY_I, KEY_J, KEY_K, KEY_L, KEY_M,
                                         KEY_N, KEY_O, KEY_P, KEY_Q, KEY_R, KEY_S, KEY_T, KEY_U, KEY_V, KEY_W, KEY_X, KEY_Y, KEY_Z};
    static const uint32_t digits[10] = {KEY_0, KEY_1, KEY_2, KEY_3, KEY_4, KEY_5, KEY_6, KEY_7, KEY_8, KEY_9};
    if (c >= 'a' && c <= 'z') return letters[c - 'a'];
    if (c >= 'A' && c <= 'Z') return letters[c - 'A'];
    if (c >= '0' && c <= '9') return digits[c - '0'];
    if (c == ' ') return KEY_SPACE;
    if (c == '.') return KEY_DOT;
    if (c == '/') return KEY_SLASH;
    if (c == '-') return KEY_MINUS;
    return 0;
}

bool key_by_name(const std::string &name, KeySpec &k)
{
    k = KeySpec{};
    static const struct { const char *n; uint32_t code; uint32_t mods; } names[] = {
        {"esc", KEY_ESC, 0}, {"enter", KEY_ENTER, 0}, {"tab", KEY_TAB, 0}, {"backtab", KEY_TAB, KBD_MOD_SHIFT},
        {"up", KEY_UP, 0}, {"down", KEY_DOWN, 0}, {"left", KEY_LEFT, 0}, {"right", KEY_RIGHT, 0},
        {"pgup", KEY_PAGEUP, 0}, {"pgdn", KEY_PAGEDOWN, 0}, {"home", KEY_HOME, 0}, {"end", KEY_END, 0},
        {"bksp", KEY_BACKSPACE, 0}, {"del", KEY_DELETE, 0}, {"f5", KEY_F5, 0}};
    for (const auto &n : names)
        if (name == n.n) {
            k.code = n.code;
            k.mods = n.mods;
            return true;
        }
    if (name == "space") {
        k.code = KEY_SPACE;
        k.utf8 = " ";
        return true;
    }
    if (name.rfind("ctrl-", 0) == 0 && name.size() == 6) {
        k.code = letter_code(name[5]);
        k.mods = KBD_MOD_CTRL;
        k.utf8 = std::string(1, name[5]);
        return k.code != 0;
    }
    return false;
}

void queue_key(const KeySpec &k)
{
    const char *t = k.utf8.empty() ? nullptr : k.utf8.c_str();
    cp0_keyboard_inject_key(k.code, KBD_KEY_PRESSED, k.mods, t);
    cp0_keyboard_inject_key(k.code, KBD_KEY_RELEASED, k.mods, t);
}

std::vector<std::string> words_of(const std::string &rest)
{
    std::vector<std::string> out;
    std::string cur;
    bool quoted = false, any = false;
    for (const char c : rest) {
        if (c == '"') {
            quoted = !quoted;
            any = true;
            continue;
        }
        if (!quoted && (c == ' ' || c == '\t')) {
            if (any) out.push_back(cur);
            cur.clear();
            any = false;
            continue;
        }
        cur += c;
        any = true;
    }
    if (any) out.push_back(cur);
    return out;
}

bool shown(lv_obj_t *obj)
{
    for (lv_obj_t *o = obj; o; o = lv_obj_get_parent(o))
        if (lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN) || lv_obj_get_style_opa(o, LV_PART_MAIN) == LV_OPA_TRANSP) return false;
    return true;
}

bool pressable(lv_obj_t *obj)
{
    if (!lv_obj_has_flag(obj, LV_OBJ_FLAG_CLICKABLE)) return false;
    if (lv_obj_check_type(obj, &lv_button_class)) return true;
    for (uint32_t i = 0; i < lv_obj_get_event_count(obj); ++i) {
        const lv_event_dsc_t *dsc = lv_obj_get_event_dsc(obj, i);
        if (dsc && (dsc->filter == LV_EVENT_CLICKED || dsc->filter == LV_EVENT_ALL)) return true;
    }
    return false;
}

struct ShellStats {
    int controls = 0, failures = 0, min_row = 1 << 20, min_button = 1 << 20, min_header = 1 << 20;
};

/* RSS Reader's rule for every control: the header's Esc the header rule, everything else (rows of the lists, buttons,
 * fields, the dialogs' rows and buttons) at least ROW (7 mm): the brief's minimum, stricter than the shared check's 5 mm
 * for a row of a scrolling list. A control with a pressable child counts as that child. */
void shell_walk(lv_obj_t *obj, const cp0_ui_metrics_t *m, int header_rule, int header_h, ShellStats &st)
{
    if (!obj) return;
    if (lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN) || lv_obj_get_style_opa(obj, LV_PART_MAIN) == LV_OPA_TRANSP) return;
    for (uint32_t i = 0; i < lv_obj_get_child_count(obj); ++i) shell_walk(lv_obj_get_child(obj, static_cast<int32_t>(i)), m, header_rule, header_h, st);
    if (!pressable(obj) || !shown(obj)) return;
    for (uint32_t i = 0; i < lv_obj_get_child_count(obj); ++i) {
        lv_obj_t *c = lv_obj_get_child(obj, static_cast<int32_t>(i));
        if (shown(c) && pressable(c)) return;
    }
    const lv_area_t va = headless::visible_area(obj);
    if (va.x2 < va.x1 || va.y2 < va.y1) return;          // scrolled out of its list
    lv_area_t a;
    lv_obj_get_coords(obj, &a);
    const int h = a.y2 - a.y1 + 1;
    ++st.controls;
    if (a.y2 < header_h) {
        st.min_header = std::min(st.min_header, h);
        if (h < header_rule) ++st.failures;
        return;
    }
    const bool button = lv_obj_check_type(obj, &lv_button_class);
    if (button) st.min_button = std::min(st.min_button, h);
    else st.min_row = std::min(st.min_row, h);
    if (h < m->tok[CP0_TOK_ROW_H]) {
        ++st.failures;
        std::printf("  under ROW (7 mm): %s %dx%d at (%d,%d)\n", button ? "button" : "row", static_cast<int>(a.x2 - a.x1 + 1), h,
                    static_cast<int>(a.x1), static_cast<int>(a.y1));
    }
}

lv_obj_t *find_last_label(lv_obj_t *obj, const std::string &text)
{
    if (!obj || lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN)) return nullptr;
    lv_obj_t *found = nullptr;
    if (lv_obj_check_type(obj, &lv_label_class)) {
        const char *t = lv_label_get_text(obj);
        const lv_area_t a = headless::visible_area(obj);
        if (t && text == t && a.x2 >= a.x1 && a.y2 >= a.y1) found = obj;
    }
    for (uint32_t i = 0; i < lv_obj_get_child_count(obj); ++i)
        if (lv_obj_t *f = find_last_label(lv_obj_get_child(obj, static_cast<int32_t>(i)), text)) found = f;
    return found;
}

/* The first visible label whose text starts with `prefix` (a long title cut with dots on the screen). */
lv_obj_t *find_label_prefix(lv_obj_t *obj, const std::string &prefix)
{
    if (!obj || lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN)) return nullptr;
    if (lv_obj_check_type(obj, &lv_label_class)) {
        const char *t = lv_label_get_text(obj);
        const lv_area_t a = headless::visible_area(obj);
        if (t && std::string(t).rfind(prefix, 0) == 0 && a.x2 >= a.x1 && a.y2 >= a.y1) return obj;
    }
    for (uint32_t i = 0; i < lv_obj_get_child_count(obj); ++i)
        if (lv_obj_t *f = find_label_prefix(lv_obj_get_child(obj, static_cast<int32_t>(i)), prefix)) return f;
    return nullptr;
}

/* A label by its exact text, or by the start of it ("^Your phone"). */
lv_obj_t *find_target(const std::string &w)
{
    if (!w.empty() && w[0] == '^') return find_label_prefix(lv_screen_active(), w.substr(1));
    return headless::find_label(w, false);
}

std::string state_value(const std::string &state, const std::string &key)
{
    const std::string pat = " " + key + "=";
    size_t at = state.find(pat);
    if (at == std::string::npos) return "<none>";
    at += pat.size();
    if (at < state.size() && state[at] == '\'') {
        const size_t end = state.find('\'', at + 1);
        return state.substr(at + 1, end == std::string::npos ? std::string::npos : end - at - 1);
    }
    const size_t end = state.find(' ', at);
    return state.substr(at, end == std::string::npos ? std::string::npos : end - at);
}

bool save_shot(const std::string &dir, const std::string &name)
{
    const std::string path = (dir.empty() ? std::string(".") : dir) + "/" + name + ".png";
    if (!headless::write_png(path, headless::screenshot())) {
        std::fprintf(stderr, "rss-reader-script: cannot write %s\n", path.c_str());
        return false;
    }
    std::printf("shot %s\n", path.c_str());
    return true;
}

void tap_screen(int x, int y)
{
    headless::platform_touch(true, x, y);
    headless::run_for(80);
    headless::platform_touch(false, x, y);
    headless::run_for(120);
}

/* ------------------------------------------------------------------ seeding (before the app starts) */

int seed(const std::vector<std::string> &lines)
{
    int failures = 0;
    rssreader::Store store(rssreader::Store::default_root());
    rssreader::Catalog catalog;
    const char *cat = std::getenv("RSSREADER_CATALOG");
    catalog.load_file(cat && *cat ? cat : std::string(CDK_APP_DIR) + "/data/catalog.json");
    FakeClient client;
    for (const std::string &line : lines) {
        const std::vector<std::string> w = words_of(line);
        if (w.size() < 2) continue;
        const std::string &what = w[1];
        bool ok = true;
        if (what == "pin" && w.size() > 2) {
            const rssreader::CatalogFeed *f = catalog.find(w[2]);
            ok = f && store.add_subscription(rssreader::make_subscription(*f, now_epoch())) == rssreader::StoreError::Ok;
        } else if (what == "refresh") {
            rssreader::Refresher r(store, client, [] { return now_epoch(); });
            r.refresh_all(store.subscriptions());
        } else if (what == "read" && w.size() > 3) {
            std::vector<rssreader::StoredArticle> a = store.load_articles(w[2]);
            const int n = std::atoi(w[3].c_str());
            for (int i = 0; i < n && i < static_cast<int>(a.size()); ++i) store.set_read(w[2], a[a.size() - 1 - static_cast<std::size_t>(i)].article.id, true);
        } else if (what == "save" && w.size() > 3) {
            std::vector<rssreader::StoredArticle> a = store.load_articles(w[2]);
            const std::size_t i = static_cast<std::size_t>(std::atoi(w[3].c_str()));
            rssreader::Subscription s;
            store.find_subscription(w[2], s);
            ok = i < a.size() && store.save_article(a[i].article, w[2], s.name, {}, now_epoch() - 3600) == rssreader::StoreError::Ok;
        } else if (what == "file" && w.size() > 3) {
            std::string body;
            const char *root = std::getenv("RSS_FAKE_ROOT");
            const char *home = std::getenv("HOME");
            ok = root && home && rssreader::read_file(std::string(root) + "/" + w[2], body) &&
                 rssreader::write_file_atomic(std::string(home) + "/" + w[3], body);
        } else if (what == "text_size" && w.size() > 2) {
            rssreader::Settings s = store.settings();
            s.text_size = w[2] == "small" ? rssreader::TextSize::Small : w[2] == "large" ? rssreader::TextSize::Large : rssreader::TextSize::Medium;
            ok = store.set_settings(s);
        } else if (what == "refresh_on_open" && w.size() > 2) {
            rssreader::Settings s = store.settings();
            s.refresh_on_open = w[2] == "on";
            ok = store.set_settings(s);
        } else if (what == "images" && w.size() > 2) {
            rssreader::Settings s = store.settings();
            s.image_download = w[2] == "off" ? rssreader::ImageDownload::Off : rssreader::ImageDownload::Always;
            ok = store.set_settings(s);
        } else if (what == "cap" && w.size() > 2) {
            rssreader::Settings s = store.settings();
            s.storage_cap_mb = std::atoi(w[2].c_str());
            ok = store.set_settings(s);
        } else if (what == "hint" && w.size() > 2) {
            rssreader::Settings s = store.settings();
            s.options_seen = w[2] == "seen";
            ok = store.set_settings(s);
        } else if (what == "filler" && w.size() > 2) {       // a saved article of about that many KB (its "image" is filler bytes)
            rssreader::Article a;
            a.id = "filler0000000000";
            a.title = "An old saved article that fills the storage";
            a.summary_html = "<p>Filler.</p>";
            rssreader::SavedImage im;
            im.url = "https://example.com/filler.jpg";
            im.content_type = "image/jpeg";
            im.bytes.assign(static_cast<std::size_t>(std::atoi(w[2].c_str())) * 1024, 'x');
            ok = store.save_article(a, "us-ars-technica", "Ars Technica", {im}, now_epoch() - 86400) == rssreader::StoreError::Ok;
        } else {
            ok = false;
        }
        std::printf("seed %s: %s\n", line.c_str(), ok ? "ok" : "FAILED");
        if (!ok) ++failures;
    }
    return failures;
}

/* ------------------------------------------------------------------ the script */

int run_script(const std::vector<std::string> &lines, const std::string &shot_dir)
{
    std::string marked;
    int lineno = 0, failures = 0;
    for (std::string line : lines) {
        ++lineno;
        if (g_quit.load()) break;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line[0] == '#' || line.rfind("seed ", 0) == 0) continue;
        std::istringstream ss(line);
        std::string cmd;
        ss >> cmd;
        Session *session = running_session();
        if (cdk::quit_requested() && cmd != "expect" && cmd != "say") continue;
        if (cmd == "wait") {
            int ms = 0;
            ss >> ms;
            headless::run_for(ms);
        } else if (cmd == "key") {
            std::string name;
            ss >> name;
            KeySpec k;
            if (!key_by_name(name, k)) {
                std::fprintf(stderr, "script:%d: unknown key %s\n", lineno, name.c_str());
                ++failures;
            } else {
                queue_key(k);
            }
            headless::run_for(80);
        } else if (cmd == "type") {
            std::string text;
            std::getline(ss, text);
            if (!text.empty() && text[0] == ' ') text.erase(0, 1);
            for (size_t i = 0; i < text.size();) {
                size_t n = 1;
                const unsigned char c = static_cast<unsigned char>(text[i]);
                if (c >= 0xF0) n = 4;
                else if (c >= 0xE0) n = 3;
                else if (c >= 0xC0) n = 2;
                KeySpec k;
                k.utf8 = text.substr(i, n);
                k.code = n == 1 ? letter_code(text[i]) : 0;
                queue_key(k);
                i += n;
            }
            headless::run_for(80);
        } else if (cmd == "tap" || cmd == "tapkey") {
            std::string rest;
            std::getline(ss, rest);
            const std::vector<std::string> w = words_of(rest);
            if (w.empty()) {
                ++failures;
                continue;
            }
            const std::string wanted = w[0] == "<right>" ? LV_SYMBOL_RIGHT : w[0] == "<left>" ? LV_SYMBOL_LEFT : w[0];
            lv_obj_t *label = cmd == "tapkey" ? headless::find_label(wanted, true)
                              : w.size() == 2 ? find_last_label(lv_screen_active(), wanted) : find_target(wanted);
            if (!label) {
                std::fprintf(stderr, "script:%d: nothing labelled \"%s\" to tap\n", lineno, w[0].c_str());
                std::printf("script:%d: FAIL nothing labelled \"%s\" to tap\n", lineno, w[0].c_str());
                ++failures;
                continue;
            }
            const lv_area_t a = headless::visible_area(headless::clickable_of(label));
            tap_screen((a.x1 + a.x2) / 2, (a.y1 + a.y2) / 2);
        } else if (cmd == "tapimage") {                  // the n-th image tile of the reader (0 first)
            int n = 0;
            ss >> n;
            lv_obj_t *t = session ? session->app().image_tile(n) : nullptr;
            const lv_area_t a = t ? headless::visible_area(t) : lv_area_t{0, 0, -1, -1};
            if (!t || a.x2 < a.x1 || a.y2 < a.y1) {
                std::printf("script:%d: FAIL no image %d in view to tap\n", lineno, n);
                ++failures;
                continue;
            }
            tap_screen((a.x1 + a.x2) / 2, (a.y1 + a.y2) / 2);
        } else if (cmd == "tapat") {
            double fx = 0, fy = 0;
            ss >> fx >> fy;
            tap_screen(static_cast<int>(fx * g_w), static_cast<int>(fy * g_h));
        } else if (cmd == "tapfield") {
            int n = 1;
            ss >> n;
            std::vector<lv_obj_t *> fields;
            std::function<void(lv_obj_t *)> walk = [&](lv_obj_t *o) {
                if (!o || !shown(o)) return;
                if (lv_obj_check_type(o, &lv_textarea_class)) {
                    const lv_area_t a = headless::visible_area(o);
                    if (a.x2 >= a.x1 && a.y2 >= a.y1) fields.push_back(o);
                    return;
                }
                for (uint32_t i = 0; i < lv_obj_get_child_count(o); ++i) walk(lv_obj_get_child(o, static_cast<int32_t>(i)));
            };
            walk(lv_screen_active());
            if (n < 1 || n > static_cast<int>(fields.size())) {
                std::printf("script:%d: FAIL no text field %d to tap\n", lineno, n);
                ++failures;
                continue;
            }
            const lv_area_t a = headless::visible_area(fields[static_cast<size_t>(n - 1)]);
            tap_screen((a.x1 + a.x2) / 2, (a.y1 + a.y2) / 2);
        } else if (cmd == "hold" || cmd == "holdat") {
            std::string rest;
            std::getline(ss, rest);
            const std::vector<std::string> w = words_of(rest);
            int x = 0, y = 0, ms = 0;
            std::string shot;
            if (cmd == "hold") {
                lv_obj_t *label = w.empty() ? nullptr : find_target(w[0]);
                if (!label) {
                    std::printf("script:%d: FAIL nothing labelled \"%s\" to hold\n", lineno, w.empty() ? "" : w[0].c_str());
                    ++failures;
                    continue;
                }
                const lv_area_t a = headless::visible_area(headless::clickable_of(label));
                x = (a.x1 + a.x2) / 2;
                y = (a.y1 + a.y2) / 2;
                ms = w.size() > 1 ? std::atoi(w[1].c_str()) : 0;
                if (w.size() > 2) shot = w[2];
            } else {
                x = static_cast<int>((w.size() > 0 ? std::atof(w[0].c_str()) : 0) * g_w);
                y = static_cast<int>((w.size() > 1 ? std::atof(w[1].c_str()) : 0) * g_h);
                ms = w.size() > 2 ? std::atoi(w[2].c_str()) : 0;
                if (w.size() > 3) shot = w[3];
            }
            headless::platform_touch(true, x, y);
            headless::run_for(ms);
            if (!shot.empty() && !save_shot(shot_dir, shot)) ++failures;
            headless::platform_touch(false, x, y);
            headless::run_for(200);
        } else if (cmd == "drag") {
            double f[4] = {0, 0, 0, 0};
            ss >> f[0] >> f[1] >> f[2] >> f[3];
            const int x1 = static_cast<int>(f[0] * g_w), y1 = static_cast<int>(f[1] * g_h), x2 = static_cast<int>(f[2] * g_w),
                      y2 = static_cast<int>(f[3] * g_h);
            headless::platform_touch(true, x1, y1);
            headless::run_for(60);
            for (int i = 1; i <= 10; ++i) {
                headless::platform_touch(true, x1 + (x2 - x1) * i / 10, y1 + (y2 - y1) * i / 10);
                headless::run_for(30);
            }
            headless::platform_touch(false, x2, y2);
            headless::run_for(400);
        } else if (cmd == "net") {
            std::string mode;
            int ms = 0;
            ss >> mode >> ms;
            if (mode == "offline") g_net.set_offline(true);
            else if (mode == "online") g_net.set_offline(false);
            else if (mode == "slow") g_net.set_delay(ms);
            else {
                std::printf("script:%d: FAIL net online|offline|slow <ms>\n", lineno);
                ++failures;
            }
        } else if (cmd == "keyboard") {
            std::string state;
            ss >> state;
            write_input_state(state == "on" ? 1 : 0);
            headless::run_for(1300);
        } else if (cmd == "shot") {
            std::string name;
            ss >> name;
            headless::run_for(150);
            if (!save_shot(shot_dir, name)) ++failures;
        } else if (cmd == "mark") {
            headless::run_for(60);
            marked = session ? session->app().debug_state() : std::string();
        } else if (cmd == "dump") {
            headless::run_for(60);
            std::printf("%s\n", session ? session->app().debug_state().c_str() : "state none");
        } else if (cmd == "say") {
            std::string text;
            std::getline(ss, text);
            std::printf("%s\n", text.c_str() + (text.empty() ? 0 : 1));
        } else if (cmd == "check") {
            std::string name, mode;
            ss >> name >> mode;
            headless::run_for(250);
            headless::CheckResult r = headless::check_layout(!session);
            if (mode == "fullscreen") {
                // the full-screen image covers the header on purpose (the design: no header there); every other rule holds
                std::vector<std::string> kept;
                for (const std::string &f : r.failures)
                    if (f.rfind("header: ", 0) != 0) kept.push_back(f);
                    else std::printf("check %s note (full-screen layer) %s\n", name.c_str(), f.c_str());
                r.failures = kept;
                r.notes.push_back("full-screen layer: the header rule does not apply");
            }
            for (const std::string &f : r.failures) std::printf("check %s FAIL %s\n", name.c_str(), f.c_str());
            ShellStats sh;
            if (session) {
                const cp0_ui_metrics_t *m = cp0_ui_metrics_get();
                cp0_ui_roller_page_t page{};
                cp0_ui_roller_page(m, cp0::ui::status_strip_left(), &page);
                const int header_rule = std::min(m->tok[CP0_TOK_TARGET], page.header_h - 2 * m->shell.status_top);
                shell_walk(lv_screen_active(), m, header_rule, page.header_h, sh);
            }
            std::printf("check %s %s: %d finding(s) of the shared layout checks", name.c_str(), r.failures.empty() ? "PASS" : "FAIL",
                        static_cast<int>(r.failures.size()));
            for (const std::string &n : r.notes) std::printf(" | %s", n.c_str());
            std::printf("\n");
            std::printf("shell %s %s: %d controls, %d under 7 mm (rows min %d, buttons min %d, header min %d)\n", name.c_str(),
                        sh.failures ? "FAIL" : "PASS", sh.controls, sh.failures, sh.min_row == (1 << 20) ? 0 : sh.min_row,
                        sh.min_button == (1 << 20) ? 0 : sh.min_button, sh.min_header == (1 << 20) ? 0 : sh.min_header);
            failures += static_cast<int>(r.failures.size()) + sh.failures;
        } else if (cmd == "expect") {
            std::string rest;
            std::getline(ss, rest);
            const std::vector<std::string> w = words_of(rest);
            const std::string what = w.empty() ? std::string() : w[0], arg = w.size() > 1 ? w[1] : std::string();
            headless::run_for(80);
            bool ok = true;
            std::string detail;
            if (what == "text") ok = find_target(arg) != nullptr;
            else if (what == "notext") ok = find_target(arg) == nullptr;
            else if (what == "osk") ok = cp0::ui::osk::visible() == (arg == "on");
            else if (what == "quit") ok = cdk::quit_requested();
            else if (what == "noquit") ok = !cdk::quit_requested();
            else if (what == "state" && session) {
                const std::string want = w.size() > 2 ? w[2] : std::string();
                const std::string now = state_value(session->app().debug_state(), arg);
                ok = now == want;
                detail = " = " + want + " (is " + now + ")";
            } else if ((what == "same" || what == "changed") && session) {
                const std::string was = state_value(marked, arg), now = state_value(session->app().debug_state(), arg);
                ok = what == "same" ? was == now : was != now;
                detail = " (" + was + " -> " + now + ")";
            } else {
                ok = false;
            }
            std::printf("expect %s \"%s\"%s: %s\n", what.c_str(), arg.c_str(), detail.c_str(), ok ? "PASS" : "FAIL");
            if (!ok) ++failures;
        } else {
            std::printf("script:%d: FAIL unknown command %s\n", lineno, cmd.c_str());
            ++failures;
        }
        std::fflush(stdout);
    }
    return failures;
}

int ppmm_for(int w, int h)
{
    if (w == 800 && h == 480) return 920;
    if (w == 720 && h == 720) return 1000;
    if (w == 1280 && h == 720) return 1160;
    return 1130;
}

void mkdirs(const std::string &path)
{
    std::string cur;
    std::stringstream ss(path);
    std::string part;
    if (!path.empty() && path[0] == '/') cur = "/";
    while (std::getline(ss, part, '/')) {
        if (part.empty()) continue;
        cur += part + "/";
        mkdir(cur.c_str(), 0755);
    }
}

} // namespace

int main(int argc, char *argv[])
{
    std::string script, shot_dir, resources;
    int ppmm = 0;
    if (const char *e = std::getenv("CDK_HEADLESS_RESOURCES")) resources = e;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--script" && i + 1 < argc) script = argv[++i];
        else if (a == "--shot-dir" && i + 1 < argc) shot_dir = argv[++i];
        else if (a == "--resources" && i + 1 < argc) resources = argv[++i];
        else if (a == "--ppmm" && i + 1 < argc) ppmm = std::atoi(argv[++i]);
        else if (a == "--size" && i + 1 < argc) {
            if (std::sscanf(argv[++i], "%dx%d", &g_w, &g_h) != 2 || g_w <= 0 || g_h <= 0) return 2;
        } else {
            std::fprintf(stderr, "usage: %s --script file [--shot-dir dir] [--size WxH] [--ppmm N] [--resources dir]\n", argv[0]);
            return 2;
        }
    }
    std::vector<std::string> lines;
    {
        std::ifstream in(script);
        if (!in) {
            std::fprintf(stderr, "rss-reader-script: cannot read %s\n", script.c_str());
            return 2;
        }
        std::string line;
        while (std::getline(in, line)) lines.push_back(line);
    }
    struct sigaction sa;
    std::memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_signal;
    sigaction(SIGTERM, &sa, nullptr);
    sigaction(SIGINT, &sa, nullptr);

    // the fake network and the fixed clock, before anything uses them
    if (const char *r = std::getenv("RSS_FAKE_ROUTES"); r && *r) g_net.load(r, std::getenv("RSS_FAKE_ROOT") ? std::getenv("RSS_FAKE_ROOT") : ".");
    hooks().make_http = [] { return std::unique_ptr<rssreader::HttpClient>(new FakeClient()); };
    if (const char *n = std::getenv("RSS_TEST_NOW"); n && *n) {
        const std::int64_t fixed = std::atoll(n);
        const auto t0 = std::chrono::steady_clock::now();
        hooks().now = [fixed, t0] {          // the fixed time, moving on with the run (a refresh takes "now")
            return fixed + std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - t0).count();
        };
    }
    std::vector<std::string> seeds;
    for (const std::string &l : lines)
        if (l.rfind("seed ", 0) == 0) seeds.push_back(l);
    int failures = seed(seeds);

    const std::string runtime = (shot_dir.empty() ? std::string("/tmp") : shot_dir) + "/.runtime/applaunch";
    mkdirs(runtime);
    if (!shot_dir.empty()) mkdirs(shot_dir);
    setenv("APPLAUNCH_STATE_DIR", runtime.c_str(), 1);
    unsetenv("APPLAUNCH_OSK");
    g_runtime = runtime;
    write_input_state(1);
    const std::string size = std::to_string(g_w) + "x" + std::to_string(g_h);
    setenv("APPLAUNCH_CONTRACT", "1", 1);
    setenv("APPLAUNCH_APP_MODE", "fullscreen", 1);
    setenv("APPLAUNCH_WINDOW", ("0,0," + std::to_string(g_w) + "," + std::to_string(g_h)).c_str(), 1);
    setenv("APPLAUNCH_CANVAS", size.c_str(), 1);
    setenv("APPLAUNCH_CANVAS_SCALE", "1", 1);
    if (!headless::platform_init(g_w, g_h, ppmm > 0 ? ppmm : ppmm_for(g_w, g_h), resources)) {
        std::fprintf(stderr, "rss-reader-script: platform init failed\n");
        return 1;
    }
    cdk::start(cdk::app_spec(), lv_screen_active());
    failures += run_script(lines, shot_dir);
    cdk::stop();
    std::printf("== %s at %s: %d failure(s), %d fake requests\n", script.c_str(), size.c_str(), failures, g_net.requests());
    return failures ? 1 : 0;
}
