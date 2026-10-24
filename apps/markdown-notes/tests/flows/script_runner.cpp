/*
 * SPDX-License-Identifier: MIT
 *
 * markdown-notes-script: Markdown Notes' scripted runner (the .script files of tests/flows) on the shared headless
 * harness's platform (apps/_common/headless: a plain LVGL display at any size, the layout service's metrics, cp0_lvgl's
 * keyboard queue and bridge, a pointer, the layout checks), with the app started by its own AppSpec (cdk::start, as on
 * a board). RSS Reader's runner (rss-reader/tests/flows/script_runner.cpp) with its fake network swapped for a vault:
 * notes are seeded from tests/data before the app starts and changed "on disk" while it runs (another device's sync),
 * and files are checked. The clock is fixed (NOTES_TEST_NOW) so dates, ages and stamps do not move. Built by
 * tests/flows/runner.mk (build.sh host).
 *
 *   markdown-notes-script --script FILE [--shot-dir DIR] [--size WxH] [--ppmm N] [--resources DIR]
 *   environment: NOTES_VAULT (the vault), XDG_CONFIG_HOME (the settings), NOTES_TEST_DATA (tests/data), NOTES_TEST_NOW
 *   (epoch seconds), CDK_HEADLESS_RESOURCES (the launcher's resources: fonts)
 *
 * Script (one command per line, # starts a comment; "quoted words"):
 *   seed vault                       the fixture vault tests/data/vault, ages from tests/data/vault.ages
 *   seed note <rel> <fixture>        tests/data/notes/<fixture> as <rel> in the vault
 *   seed text <rel> "<text>"         a note of that text (\n, \t and \xHH understood)
 *   seed big <rel> <KB>              a note of about that many KB (headings, paragraphs, tasks)
 *   seed age <rel> <seconds>         its mtime that long before NOTES_TEST_NOW
 *   seed setting <key> <value>       settings.conf (text_size, open_in)
 *                                    seeds run before the app starts, in the order written
 *   (a label: its exact text, or ^ and the start of it)
 *   wait <ms>   key <name>   type <text>   tap "label" [last]   tapat <fx> <fy>   tapfield <n>   tapkey "label"
 *   tapspan "text"   taptask <n>   hold "label" <ms> [shot]   holdat <fx> <fy> <ms> [shot]   drag <fx1> <fy1> <fx2> <fy2>
 *                         (fractions of the screen; a hold's shot is taken before the finger lifts)
 *   disk write <rel> <fixture> | disk text <rel> "<text>" | disk remove <rel>   the file changed by someone else
 *   keyboard on|off [gone S] [btgone S]   input.state (as the launcher writes it, read by the app's real policy): a
 *                                    keyboard there or not; a keyboard (gone: the 2 s grace) or a Bluetooth keyboard
 *                                    (btgone: asleep for 600 s) that went away S seconds ago on the monotonic clock
 *   input "<text>"                   input.state exactly as given (\n understood; gen= becomes the next generation)
 *   seed input "<text>"              the first input.state (default: one keyboard there)
 *   shot <name>   check <name>   dump   mark   say <text>
 *   expect text "T" | notext "T" | has "T" (a visible label holding T) | osk on|off | quit | noquit | state KEY VALUE | same KEY | changed KEY
 *          | osk_allowed yes|no      (the app's on-screen keyboard policy, asked now)
 *          | file <rel> contains "T" | file <rel> lacks "T" | file <rel> exists | file <rel> missing
 *          | copy <rel> contains "T"   (a conflict copy "<name> (deck ...).md" beside <rel> holds it)
 *          | span "T"                 (a span of the preview with exactly that text is in view)
 *          | key "T" toolbar          (the button labelled T is exactly the 9 mm toolbar key high)
 *   key names: esc enter tab backtab up down left right pgup pgdn home end bksp del space, ctrl-<letter>, ctrl-enter,
 *              ctrl-home, ctrl-end; tapkey "<enter>" "<bksp>" "<space>" for the on-screen keyboard's unlabelled keys
 */

#include "app.hpp"
#include "headless.hpp"
#include "hooks.hpp"
#include "session.hpp"

#include "fsutil.hpp"
#include "osk_policy.hpp"
#include "settings.hpp"

#include "cp0_ui_metrics_lvgl.h"
#include "cp0_ui_roller.h"
#include "cp0_ui_widgets.hpp"
#include "keyboard_input.h"
#include "lvgl/src/misc/lv_event_private.h"

#include <linux/input-event-codes.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <functional>
#include <map>
#include <sstream>
#include <string>
#include <vector>

using namespace notesui;

namespace {

int g_w = 640, g_h = 480;
std::string g_runtime, g_vault, g_data;
std::int64_t g_now = 0;
unsigned g_input_gen = 0;
std::atomic<bool> g_quit{false};

/* ------------------------------------------------------------------ helpers */

/* input.state as the launcher writes it (APP-CONTRACT section 10: a temp file, then a rename; a new gen), read by the
 * app's real policy (_common osk_policy: stat at most once a second, cp0_app_osk_allowed on the monotonic clock). A
 * "gen=" line of the text is replaced by the next generation. */
void write_input_text(std::string text)
{
    if (g_runtime.empty()) return;
    const std::string gen = "gen=" + std::to_string(++g_input_gen);
    const std::size_t at = text.find("gen=");
    if (at != std::string::npos) text.replace(at, text.find('\n', at) == std::string::npos ? std::string::npos : text.find('\n', at) - at, gen);
    else text += gen + "\n";
    const std::string path = g_runtime + "/input.state", tmp = path + ".tmp";
    {
        std::ofstream f(tmp);
        f << text;
    }
    std::rename(tmp.c_str(), path.c_str());
}

std::int64_t mono_s()
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<std::int64_t>(ts.tv_sec);
}

/* keyboards present (or not); a keyboard / a Bluetooth keyboard that went away `kb_ago` / `bt_ago` seconds ago (-1:
 * never). */
void write_input_state(int keyboards, long kb_ago = -1, long bt_ago = -1)
{
    const std::int64_t now = mono_s();
    const long ago = std::max(kb_ago, bt_ago);
    std::string t = "v=1\nkeyboards=" + std::to_string(keyboards) + "\nlast_seen=" +
                    std::to_string(ago >= 0 ? static_cast<long long>(std::time(nullptr) - ago) : 0LL) + "\nbt_keyboards=0\nkb_gone=" +
                    std::to_string(kb_ago >= 0 ? now - kb_ago : 0) + "\nbt_gone=" + std::to_string(bt_ago >= 0 ? now - bt_ago : 0) +
                    "\nvkb=auto\npointer=off\ngen=0\n";
    write_input_text(t);
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
        {"bksp", KEY_BACKSPACE, 0}, {"del", KEY_DELETE, 0}, {"ctrl-enter", KEY_ENTER, KBD_MOD_CTRL},
        {"ctrl-home", KEY_HOME, KBD_MOD_CTRL}, {"ctrl-end", KEY_END, KBD_MOD_CTRL}};
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

/* "\n", "\t" and "\xHH" (any byte: a file that is not UTF-8) in a script's text. */
std::string unescape(const std::string &s)
{
    std::string out;
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\\' && i + 1 < s.size()) {
            const char n = s[i + 1];
            if (n == 'n') { out += '\n'; ++i; continue; }
            if (n == 't') { out += '\t'; ++i; continue; }
            if (n == '\\') { out += '\\'; ++i; continue; }
            if (n == 'x' && i + 3 < s.size() && std::isxdigit(static_cast<unsigned char>(s[i + 2])) &&
                std::isxdigit(static_cast<unsigned char>(s[i + 3]))) {
                out += static_cast<char>(std::stoi(s.substr(i + 2, 2), nullptr, 16));
                i += 3;
                continue;
            }
        }
        out += s[i];
    }
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
        const uint32_t code = dsc ? (dsc->filter & ~static_cast<uint32_t>(LV_EVENT_PREPROCESS)) : 0;
        if (dsc && (code == LV_EVENT_CLICKED || code == LV_EVENT_PRESSED || code == LV_EVENT_ALL)) return true;
    }
    return false;
}

struct ShellStats {
    int controls = 0, failures = 0, min_row = 1 << 20, min_button = 1 << 20, min_header = 1 << 20;
};

/* RSS Reader's rule for every control: the header's Esc the header rule, everything else (rows, buttons, fields, the
 * dialogs' rows and buttons) at least ROW (7 mm). A control with a pressable child counts as that child. */
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

/* The first visible label whose text holds `part` (the caption's hint at its end). */
lv_obj_t *find_label_holding(lv_obj_t *obj, const std::string &part)
{
    if (!obj || lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN)) return nullptr;
    if (lv_obj_check_type(obj, &lv_label_class)) {
        const char *t = lv_label_get_text(obj);
        const lv_area_t a = headless::visible_area(obj);
        if (t && std::string(t).find(part) != std::string::npos && a.x2 >= a.x1 && a.y2 >= a.y1) return obj;
    }
    for (uint32_t i = 0; i < lv_obj_get_child_count(obj); ++i)
        if (lv_obj_t *f = find_label_holding(lv_obj_get_child(obj, static_cast<int32_t>(i)), part)) return f;
    return nullptr;
}

lv_obj_t *find_target(const std::string &w)
{
    if (!w.empty() && w[0] == '^') return find_label_prefix(lv_screen_active(), w.substr(1));
    return headless::find_label(w, false);
}

/* The span with exactly this text, in a visible spangroup: its group and the centre of its first line on the screen. */
bool find_span(lv_obj_t *obj, const std::string &text, int &x, int &y)
{
    if (!obj || lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN)) return false;
    if (lv_obj_check_type(obj, &lv_spangroup_class)) {
        const lv_area_t va = headless::visible_area(obj);
        if (va.x2 >= va.x1 && va.y2 >= va.y1) {
            const uint32_t n = lv_spangroup_get_span_count(obj);
            for (uint32_t i = 0; i < n; ++i) {
                lv_span_t *sp = lv_spangroup_get_child(obj, static_cast<int32_t>(i));
                const char *t = sp ? lv_span_get_text(sp) : nullptr;
                if (!t || text != t) continue;
                const lv_span_coords_t c = lv_spangroup_get_span_coords(obj, sp);
                lv_area_t oc;
                lv_obj_get_coords(obj, &oc);
                x = oc.x1 + (c.heading.x1 + c.heading.x2) / 2;
                y = oc.y1 + (c.heading.y1 + c.heading.y2) / 2;
                if (y >= va.y1 && y <= va.y2) return true;
            }
        }
    }
    for (uint32_t i = 0; i < lv_obj_get_child_count(obj); ++i)
        if (find_span(lv_obj_get_child(obj, static_cast<int32_t>(i)), text, x, y)) return true;
    return false;
}

/* The on-screen keyboard's space bar: the visible clickable key whose label is empty (it has no label text). */
lv_obj_t *find_blank_key(lv_obj_t *obj)
{
    if (!obj || lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN)) return nullptr;
    if (lv_obj_check_type(obj, &lv_label_class)) {
        const char *t = lv_label_get_text(obj);
        lv_obj_t *p = lv_obj_get_parent(obj);
        if (t && !*t && p && lv_obj_has_flag(p, LV_OBJ_FLAG_CLICKABLE)) {
            const lv_area_t a = headless::visible_area(p);
            if (a.x2 > a.x1 && a.y2 > a.y1) return obj;
        }
    }
    for (uint32_t i = 0; i < lv_obj_get_child_count(obj); ++i)
        if (lv_obj_t *f = find_blank_key(lv_obj_get_child(obj, static_cast<int32_t>(i)))) return f;
    return nullptr;
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
        std::fprintf(stderr, "markdown-notes-script: cannot write %s\n", path.c_str());
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

std::string read_all(const std::string &path)
{
    std::string s;
    mdnotes::read_file(path, s, 8u * 1024 * 1024);
    return s;
}

bool write_now(const std::string &path, const std::string &bytes)
{
    mdnotes::make_dirs(mdnotes::dir_name(path));
    return mdnotes::write_file_atomic(path, bytes);
}

void set_age(const std::string &path, std::int64_t seconds_ago)
{
    struct timespec ts[2];
    ts[0].tv_sec = static_cast<time_t>(g_now - seconds_ago);
    ts[0].tv_nsec = 0;
    ts[1] = ts[0];
    ::utimensat(AT_FDCWD, path.c_str(), ts, 0);
}

void copy_tree(const std::string &from, const std::string &to)
{
    std::vector<mdnotes::DirEntry> entries;
    if (!mdnotes::list_dir(from, entries)) return;
    mdnotes::make_dirs(to);
    for (const mdnotes::DirEntry &e : entries) {
        if (e.is_dir) copy_tree(from + "/" + e.name, to + "/" + e.name);
        else if (e.is_reg) write_now(to + "/" + e.name, read_all(from + "/" + e.name));
    }
}

/* A note of about kb KB: headings, paragraphs with emphasis and links, task lists (the board's lag test uses the same
 * shape: tests/board/big-note.sh). */
std::string big_note(int kb)
{
    std::string s = "# A big note\n\n";
    int n = 0;
    while (static_cast<int>(s.size()) < kb * 1024) {
        ++n;
        s += "## Section " + std::to_string(n) + "\n\n";
        s += "Some **bold** text, some *italic* text and a [[Link " + std::to_string(n) +
             "]] in a paragraph that goes on for a while so that it wraps over a few lines on the deck's screen.\n\n";
        s += "- [ ] task " + std::to_string(n) + " one\n- [x] task " + std::to_string(n) + " two\n- a plain item\n\n";
    }
    return s;
}

/* ------------------------------------------------------------------ seeding (before the app starts) */

int seed(const std::vector<std::string> &lines)
{
    int failures = 0;
    for (const std::string &line : lines) {
        const std::vector<std::string> w = words_of(line);
        if (w.size() < 2) continue;
        const std::string &what = w[1];
        bool ok = true;
        if (what == "vault") {
            copy_tree(g_data + "/vault", g_vault);
            std::ifstream ages(g_data + "/vault.ages");
            std::string rel;
            long secs = 0;
            std::string l;
            while (std::getline(ages, l)) {
                if (l.empty() || l[0] == '#') continue;
                const std::size_t sp = l.rfind(' ');
                if (sp == std::string::npos) continue;
                rel = l.substr(0, sp);
                secs = std::atol(l.c_str() + sp + 1);
                set_age(g_vault + "/" + rel, secs);
            }
        } else if (what == "note" && w.size() > 3) {
            ok = write_now(g_vault + "/" + w[2], read_all(g_data + "/notes/" + w[3]));
            set_age(g_vault + "/" + w[2], 3600);
        } else if (what == "text" && w.size() > 3) {
            ok = write_now(g_vault + "/" + w[2], unescape(w[3]));
            set_age(g_vault + "/" + w[2], 600);
        } else if (what == "big" && w.size() > 3) {
            ok = write_now(g_vault + "/" + w[2], big_note(std::atoi(w[3].c_str())));
        } else if (what == "age" && w.size() > 3) {
            set_age(g_vault + "/" + w[2], std::atol(w[3].c_str()));
        } else if (what == "input") {                   // the first input.state: written below, before the app starts
            ok = w.size() > 2;
        } else if (what == "setting" && w.size() > 3) {
            const std::string path = mdnotes::settings_path(mdnotes::home_dir());
            mdnotes::Settings s = mdnotes::load_settings(path);
            if (w[2] == "open_in") s.open_in = w[3] == "editor" ? mdnotes::OpenIn::Editor : mdnotes::OpenIn::Preview;
            else if (w[2] == "text_size")
                s.text_size = w[3] == "small" ? mdnotes::TextSize::Small : w[3] == "large" ? mdnotes::TextSize::Large : mdnotes::TextSize::Medium;
            else ok = false;
            ok = ok && mdnotes::save_settings(path, s);
        } else {
            ok = false;
        }
        std::printf("seed %s: %s\n", line.c_str(), ok ? "ok" : "FAILED");
        if (!ok) ++failures;
    }
    return failures;
}

/* The conflict copies beside rel: "<stem> (deck ...).md". */
std::vector<std::string> find_copies(const std::string &rel)
{
    const std::string dir = mdnotes::path_join(g_vault, mdnotes::dir_name(rel));
    std::string stem = mdnotes::base_name(rel);
    if (stem.size() > 3) stem.resize(stem.size() - 3);
    std::vector<mdnotes::DirEntry> entries;
    mdnotes::list_dir(dir, entries);
    std::vector<std::string> out;
    for (const mdnotes::DirEntry &e : entries)
        if (e.name.rfind(stem + " (deck ", 0) == 0) out.push_back(dir + "/" + e.name);
    return out;
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
                std::printf("script:%d: FAIL unknown key %s\n", lineno, name.c_str());
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
            const std::string wanted = w[0] == "<right>" ? LV_SYMBOL_RIGHT : w[0] == "<left>" ? LV_SYMBOL_LEFT
                                       : w[0] == "<enter>" ? LV_SYMBOL_NEW_LINE : w[0] == "<bksp>" ? LV_SYMBOL_BACKSPACE
                                       : w[0] == "<space>" ? std::string() : w[0];
            lv_obj_t *label = cmd == "tapkey" ? (wanted.empty() ? find_blank_key(lv_display_get_layer_top(lv_display_get_default())) : headless::find_label(wanted, true))
                              : w.size() == 2 ? find_last_label(lv_screen_active(), wanted) : find_target(wanted);
            if (!label) {
                std::printf("script:%d: FAIL nothing labelled \"%s\" to tap\n", lineno, w[0].c_str());
                ++failures;
                continue;
            }
            const lv_area_t a = headless::visible_area(headless::clickable_of(label));
            tap_screen((a.x1 + a.x2) / 2, (a.y1 + a.y2) / 2);
        } else if (cmd == "tapspan") {
            std::string rest;
            std::getline(ss, rest);
            const std::vector<std::string> w = words_of(rest);
            int x = 0, y = 0;
            if (w.empty() || !find_span(lv_screen_active(), w[0], x, y)) {
                std::printf("script:%d: FAIL no span \"%s\" in view to tap\n", lineno, w.empty() ? "" : w[0].c_str());
                ++failures;
                continue;
            }
            tap_screen(x, y);
        } else if (cmd == "taptask") {
            int n = 0;
            ss >> n;
            lv_obj_t *t = session ? session->app().task_box(n) : nullptr;
            const lv_area_t a = t ? headless::visible_area(t) : lv_area_t{0, 0, -1, -1};
            if (!t || a.x2 < a.x1 || a.y2 < a.y1) {
                std::printf("script:%d: FAIL no task box %d in view to tap\n", lineno, n);
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
            } else {                                   // holdat <fx> <fy> <ms> [shot]: fractions of the screen
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
        } else if (cmd == "disk") {
            std::string rest;
            std::getline(ss, rest);
            const std::vector<std::string> w = words_of(rest);
            bool ok = w.size() >= 2;
            if (ok && w[0] == "write" && w.size() > 2) ok = write_now(g_vault + "/" + w[1], read_all(g_data + "/notes/" + w[2]));
            else if (ok && w[0] == "text" && w.size() > 2) ok = write_now(g_vault + "/" + w[1], unescape(w[2]));
            else if (ok && w[0] == "remove") ok = ::unlink((g_vault + "/" + w[1]).c_str()) == 0;
            else ok = false;
            std::printf("disk %s: %s\n", rest.c_str(), ok ? "ok" : "FAIL");
            if (!ok) ++failures;
        } else if (cmd == "keyboard") {
            std::string state, word;
            long kb_ago = -1, bt_ago = -1, n = 0;
            ss >> state;
            while (ss >> word >> n) {
                if (word == "gone") kb_ago = n;              // any keyboard (USB): the 2 s grace
                else if (word == "btgone") kb_ago = bt_ago = n;   // a Bluetooth one: asleep for 600 s
            }
            write_input_state(state == "on" ? 1 : 0, kb_ago, bt_ago);
            headless::run_for(1300);
        } else if (cmd == "input") {
            std::string rest;
            std::getline(ss, rest);
            const std::vector<std::string> w = words_of(rest);
            write_input_text(w.empty() ? std::string() : unescape(w[0]));
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
            std::string name;
            ss >> name;
            headless::run_for(250);
            headless::CheckResult r = headless::check_layout(!session);
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
            else if (what == "has") ok = find_label_holding(lv_screen_active(), arg) != nullptr;
            else if (what == "notext") ok = find_target(arg) == nullptr;
            else if (what == "osk") ok = cp0::ui::osk::visible() == (arg == "on");
            else if (what == "osk_allowed") {              // the app's real policy, asked now
                cdk::osk_policy::Policy *p = cdk::osk_policy::installed();
                struct timespec ts;
                clock_gettime(CLOCK_MONOTONIC, &ts);
                const bool allowed = p && p->allowed(static_cast<std::int64_t>(ts.tv_sec) * 1000 + ts.tv_nsec / 1000000);
                ok = allowed == (arg == "yes");
                detail = std::string(" (the policy says ") + (allowed ? "yes" : "no") + ", monotonic " + std::to_string(mono_s()) + " s)";
            }
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
            } else if (what == "file" && w.size() > 2) {
                const std::string path = g_vault + "/" + arg;
                const bool exists = mdnotes::stat_path(path).exists;
                const std::string mode = w[2], want = w.size() > 3 ? unescape(w[3]) : std::string();
                if (mode == "exists") ok = exists;
                else if (mode == "missing") ok = !exists;
                else if (mode == "contains") ok = exists && read_all(path).find(want) != std::string::npos;
                else if (mode == "lacks") ok = exists && read_all(path).find(want) == std::string::npos;
                else ok = false;
                detail = " " + mode + (want.empty() ? std::string() : " \"" + w[3] + "\"");
                if (!ok && exists) std::printf("  the file holds: [%s]\n", read_all(path).substr(0, 400).c_str());
            } else if (what == "copy" && w.size() > 3) {
                const std::string want = unescape(w[3]);
                std::string hit;
                const std::vector<std::string> copies = find_copies(arg);
                for (const std::string &c : copies)
                    if (read_all(c).find(want) != std::string::npos) hit = c;
                ok = !hit.empty();
                detail = " " + (hit.empty() ? std::to_string(copies.size()) + " copies" : mdnotes::base_name(hit)) + " contains \"" + w[3] + "\"";
            } else if (what == "span") {
                int x = 0, y = 0;
                ok = find_span(lv_screen_active(), arg, x, y);
            } else if (what == "key" && w.size() > 2) {
                lv_obj_t *label = find_target(arg);
                lv_obj_t *b = label ? headless::clickable_of(label) : nullptr;
                const int want = cp0_ui_toolbar_key_px(cp0_ui_metrics_get()->ppmm_x100);
                const int h = b ? static_cast<int>(lv_obj_get_height(b)) : 0;
                ok = b && lv_obj_check_type(b, &lv_button_class) && h == want && lv_obj_get_width(b) >= want;
                detail = " " + w[2] + " (" + std::to_string(h) + " px, want " + std::to_string(want) + ")";
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
    if (w == 1280 && h == 800) return 860;       // the FZ-M1 tablet (8.6 px/mm)
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
            std::fprintf(stderr, "markdown-notes-script: cannot read %s\n", script.c_str());
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

    const char *vault = std::getenv("NOTES_VAULT");
    const char *data = std::getenv("NOTES_TEST_DATA");
    if (!vault || !*vault || !data || !*data) {
        std::fprintf(stderr, "markdown-notes-script: NOTES_VAULT and NOTES_TEST_DATA are needed\n");
        return 2;
    }
    g_vault = vault;
    g_data = data;
    mkdirs(g_vault);
    g_now = std::time(nullptr);
    if (const char *n = std::getenv("NOTES_TEST_NOW"); n && *n) {
        g_now = std::atoll(n);
        const std::int64_t fixed = g_now;
        const auto t0 = std::chrono::steady_clock::now();
        hooks().now = [fixed, t0] {          // the fixed time, moving on with the run
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
    std::string first_input;
    for (const std::string &l : lines)
        if (l.rfind("seed input ", 0) == 0) {
            const std::vector<std::string> w = words_of(l);
            if (w.size() > 2) first_input = unescape(w[2]);
        }
    if (first_input.empty()) write_input_state(1);
    else write_input_text(first_input);
    const std::string size = std::to_string(g_w) + "x" + std::to_string(g_h);
    setenv("APPLAUNCH_CONTRACT", "1", 1);
    setenv("APPLAUNCH_APP_MODE", "fullscreen", 1);
    setenv("APPLAUNCH_WINDOW", ("0,0," + std::to_string(g_w) + "," + std::to_string(g_h)).c_str(), 1);
    setenv("APPLAUNCH_CANVAS", size.c_str(), 1);
    setenv("APPLAUNCH_CANVAS_SCALE", "1", 1);
    if (!headless::platform_init(g_w, g_h, ppmm > 0 ? ppmm : ppmm_for(g_w, g_h), resources)) {
        std::fprintf(stderr, "markdown-notes-script: platform init failed\n");
        return 1;
    }
    cdk::start(cdk::app_spec(), lv_screen_active());
    failures += run_script(lines, shot_dir);
    cdk::stop();
    std::printf("== %s at %s: %d failure(s)\n", script.c_str(), size.c_str(), failures);
    return failures ? 1 : 0;
}
