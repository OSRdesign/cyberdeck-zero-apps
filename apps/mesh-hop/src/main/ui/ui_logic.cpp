/*
 * SPDX-License-Identifier: MIT
 */

#include "ui_logic.hpp"

#include "client.hpp"
#include "protocol.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <sstream>

using namespace meshzero;

namespace meshhop {

/* ------------------------------------------------------------------ keys */

namespace {

enum : int {
    kEsc = 1, kBackspace = 14, kTab = 15, kEnter = 28, kLCtrl = 29, kLShift = 42, kRShift = 54, kKpAsterisk = 55, kLAlt = 56,
    kSpace = 57, kCaps = 58, kF1 = 59, kF10 = 68, kF11 = 87, kF12 = 88, kKpEnter = 96, kRCtrl = 97, kKpSlash = 98, kRAlt = 100,
    kHome = 102, kUp = 103, kPageUp = 104, kLeft = 105, kRight = 106, kEnd = 107, kDown = 108, kPageDown = 109, kDelete = 111,
};

struct KeyChars {
    const char *base, *shift, *altgr;
};

/* evdev codes 2..53 (the digit row to the slash key), 86 (the extra key next to left shift) is not mapped. */
KeyChars us_chars(int code)
{
    static const char *row_digit_b = "1234567890-=";
    static const char *row_digit_s = "!@#$%^&*()_+";
    static const char *row_q_b = "qwertyuiop[]";
    static const char *row_q_s = "QWERTYUIOP{}";
    static const char *row_a_b = "asdfghjkl;'`";
    static const char *row_a_s = "ASDFGHJKL:\"~";
    static const char *row_z_b = "zxcvbnm,./";
    static const char *row_z_s = "ZXCVBNM<>?";
    static thread_local char buf[3][2];
    auto one = [&](int slot, char c) { buf[slot][0] = c; buf[slot][1] = 0; return buf[slot]; };
    char b = 0, s = 0;
    if (code >= 2 && code <= 13) { b = row_digit_b[code - 2]; s = row_digit_s[code - 2]; }
    else if (code >= 16 && code <= 27) { b = row_q_b[code - 16]; s = row_q_s[code - 16]; }
    else if (code >= 30 && code <= 41) { b = row_a_b[code - 30]; s = row_a_s[code - 30]; }
    else if (code == 43) { b = '\\'; s = '|'; }
    else if (code >= 44 && code <= 53) { b = row_z_b[code - 44]; s = row_z_s[code - 44]; }
    else return {"", "", ""};
    return {one(0, b), one(1, s), ""};
}

KeyChars fr_chars(int code)
{
    switch (code) {
    case 2: return {"&", "1", ""};
    case 3: return {"\xC3\xA9", "2", "~"};                 // e acute
    case 4: return {"\"", "3", "#"};
    case 5: return {"'", "4", "{"};
    case 6: return {"(", "5", "["};
    case 7: return {"-", "6", "|"};
    case 8: return {"\xC3\xA8", "7", "`"};                 // e grave
    case 9: return {"_", "8", "\\"};
    case 10: return {"\xC3\xA7", "9", "^"};                // c cedilla
    case 11: return {"\xC3\xA0", "0", "@"};                // a grave
    case 12: return {")", "\xC2\xB0", "]"};                // degree
    case 13: return {"=", "+", "}"};
    case 16: return {"a", "A", ""};
    case 17: return {"z", "Z", ""};
    case 18: return {"e", "E", "\xE2\x82\xAC"};            // euro
    case 26: return {"^", "\xC2\xA8", ""};
    case 27: return {"$", "\xC2\xA3", "\xC2\xA4"};
    case 30: return {"q", "Q", ""};
    case 39: return {"m", "M", ""};
    case 40: return {"\xC3\xB9", "%", ""};                 // u grave
    case 41: return {"\xC2\xB2", "", ""};
    case 43: return {"*", "\xC2\xB5", ""};
    case 44: return {"w", "W", ""};
    case 50: return {",", "?", ""};
    case 51: return {";", ".", ""};
    case 52: return {":", "/", ""};
    case 53: return {"!", "\xC2\xA7", ""};
    default: return us_chars(code);                        // the letters that did not move, a few symbols
    }
}

} // namespace

Layout layout_from_name(const std::string &name)
{
    std::string n;
    for (char c : name) n += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (n.rfind("fr", 0) == 0) return Layout::FR;
    return Layout::US;
}

std::string xkb_layout_from_text(const std::string &text)
{
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        if (line.rfind("XKBLAYOUT=", 0) != 0) continue;
        std::string v = line.substr(10);
        v.erase(std::remove(v.begin(), v.end(), '"'), v.end());
        v.erase(std::remove(v.begin(), v.end(), '\''), v.end());
        return v;
    }
    return "";
}

Layout detect_layout()
{
    if (const char *e = std::getenv("MESHHOP_KEYMAP"); e && *e) return layout_from_name(e);
    std::ifstream f("/etc/default/keyboard");
    if (f) {
        std::stringstream ss;
        ss << f.rdbuf();
        return layout_from_name(xkb_layout_from_text(ss.str()));
    }
    return Layout::US;
}

void KeyTranslator::reset()
{
    lshift_ = rshift_ = lctrl_ = rctrl_ = lalt_ = ralt_ = false;
}

bool KeyTranslator::feed(int code, int value, KeyEvent &out)
{
    const bool down = value != 0;
    switch (code) {
    case kLShift: lshift_ = down; return false;
    case kRShift: rshift_ = down; return false;
    case kLCtrl: lctrl_ = down; return false;
    case kRCtrl: rctrl_ = down; return false;
    case kLAlt: lalt_ = down; return false;
    case kRAlt: ralt_ = down; return false;
    case kCaps:
        if (value == 1) caps_ = !caps_;
        return false;
    default: break;
    }
    if (value == 0) return false;
    out = KeyEvent{};
    out.code = code;
    out.repeat = value == 2;
    out.shift = lshift_ || rshift_;
    out.ctrl = lctrl_ || rctrl_;
    out.alt = lalt_;
    const bool altgr = ralt_;
    switch (code) {
    case kEsc: out.key = Key::Esc; return true;
    case kEnter:
    case kKpEnter: out.key = Key::Enter; return true;
    case kTab: out.key = out.shift ? Key::BackTab : Key::Tab; return true;
    case kBackspace: out.key = Key::Backspace; return true;
    case kDelete: out.key = Key::Delete; return true;
    case kUp: out.key = Key::Up; return true;
    case kDown: out.key = Key::Down; return true;
    case kLeft: out.key = Key::Left; return true;
    case kRight: out.key = Key::Right; return true;
    case kPageUp: out.key = Key::PageUp; return true;
    case kPageDown: out.key = Key::PageDown; return true;
    case kHome: out.key = Key::Home; return true;
    case kEnd: out.key = Key::End; return true;
    default: break;
    }
    if ((code >= kF1 && code <= kF10) || code == kF11 || code == kF12) {
        out.key = Key::Function;
        out.fn = code <= kF10 ? code - kF1 + 1 : code - kF11 + 11;
        return true;
    }
    out.key = Key::Char;
    if (code == kSpace) { out.text = " "; return true; }
    // keypad
    static const char *const kp = "789-456+1230.";
    if (code >= 71 && code <= 83) { out.text = std::string(1, kp[code - 71]); return true; }
    if (code == kKpAsterisk) { out.text = "*"; return true; }
    if (code == kKpSlash) { out.text = "/"; return true; }
    const KeyChars kc = layout_ == Layout::FR ? fr_chars(code) : us_chars(code);
    std::string t;
    if (altgr && kc.altgr[0]) t = kc.altgr;
    else {
        bool shift = out.shift;
        // caps lock only acts on letters
        const unsigned char b0 = static_cast<unsigned char>(kc.base[0]);
        if (caps_ && kc.base[1] == 0 && std::islower(b0)) shift = !shift;
        t = shift ? kc.shift : kc.base;
    }
    if (t.empty()) { out.key = Key::None; return false; }
    out.text = t;
    return true;
}

/* ------------------------------------------------------------------ text editors */

std::string filter_typed(EditMode mode, const std::string &cur, const std::string &add_in, size_t limit_bytes)
{
    std::string add = add_in;
    if (add.empty()) return "";
    for (unsigned char c : add)
        if (c < 0x20 || c == 0x7F) return "";
    switch (mode) {
    case EditMode::Number:
        if (add == ",") add = ".";
        if (!(add.size() == 1 && (std::isdigit(static_cast<unsigned char>(add[0])) || add[0] == '.' || add[0] == '-'))) return "";
        break;
    case EditMode::Clock:
        if (!(add.size() == 1 && (std::isdigit(static_cast<unsigned char>(add[0])) || add[0] == '-' || add[0] == ':' || add[0] == ' ')))
            return "";
        break;
    case EditMode::Channel:
        if (add.find(' ') != std::string::npos) return "";
        break;
    case EditMode::HexKey:
        if (!channel_key_char_ok(cur, add)) return "";
        break;
    default: break;
    }
    if (cur.size() + add.size() > limit_bytes) return "";
    return add;
}

/* ------------------------------------------------------------------ formatting */

std::string fmt_clock(uint32_t ts)
{
    if (ts < kMinPlausibleTime) return "--:--";
    const time_t t = static_cast<time_t>(ts);
    struct tm tmv;
    localtime_r(&t, &tmv);
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%02d:%02d", tmv.tm_hour, tmv.tm_min);
    return buf;
}

std::string fmt_age(uint32_t when)
{
    const uint32_t now = now_unix();
    if (when == 0 || now < kMinPlausibleTime || when < kMinPlausibleTime) return "-";
    if (when > now) return "now";
    return format_age(static_cast<int64_t>(now - when));
}

std::string fmt_age_coarse(uint32_t when)
{
    const uint32_t now = now_unix();
    if (when != 0 && now >= kMinPlausibleTime && when >= kMinPlausibleTime && (when > now || now - when < 60)) return "< 1 min";
    return fmt_age(when);
}

std::string fmt_freq(double mhz)
{
    char b[32];
    std::snprintf(b, sizeof(b), "%.3f MHz", mhz);
    return b;
}

std::string fmt_bw(double khz)
{
    char b[32];
    std::snprintf(b, sizeof(b), "%g kHz", khz);
    return b;
}

std::string fmt_snr(bool has, double snr)
{
    if (!has) return "-";
    char b[32];
    std::snprintf(b, sizeof(b), "%.1f dB", snr);
    return b;
}

std::string fmt_hops(int hops)
{
    if (hops < 0) return "flood";
    if (hops == 0) return "direct";
    return std::to_string(hops) + (hops == 1 ? " hop" : " hops");
}

std::string fmt_distance(double km)
{
    char b[32];
    if (km < 1.0) std::snprintf(b, sizeof(b), "%d m", static_cast<int>(std::lround(km * 1000.0)));
    else if (km < 100.0) std::snprintf(b, sizeof(b), "%.1f km", km);
    else std::snprintf(b, sizeof(b), "%.0f km", km);
    return b;
}

double distance_km(double lat1, double lon1, double lat2, double lon2)
{
    const double kR = 6371.0088, rad = 3.14159265358979323846 / 180.0;
    const double dlat = (lat2 - lat1) * rad, dlon = (lon2 - lon1) * rad;
    const double a = std::sin(dlat / 2) * std::sin(dlat / 2) +
                     std::cos(lat1 * rad) * std::cos(lat2 * rad) * std::sin(dlon / 2) * std::sin(dlon / 2);
    return 2 * kR * std::asin(std::min(1.0, std::sqrt(a)));
}

std::string truncate_ellipsis(const std::string &s, size_t max_chars)
{
    size_t chars = 0, i = 0;
    while (i < s.size()) {
        if ((static_cast<unsigned char>(s[i]) & 0xC0) != 0x80) {
            if (chars == max_chars) return s.substr(0, i) + "...";
            ++chars;
        }
        ++i;
    }
    return s;
}

/* ------------------------------------------------------------------ chats */

std::string fmt_count_badge(int n)
{
    if (n <= 0) return "";
    return n > 99 ? "99+" : std::to_string(n);
}

std::vector<ChatRow> build_chat_rows(const Model &model, const std::string &also_direct)
{
    std::vector<ChatRow> rows;
    auto convs = model.conversations();
    if (also_direct.rfind("d:", 0) == 0) {
        bool present = false;
        for (const ConvSummary &c : convs)
            if (c.key == also_direct) present = true;
        if (!present) {
            ConvSummary extra;
            extra.key = also_direct;
            extra.title = model.conv_title(also_direct);
            extra.muted = model.is_muted(also_direct);
            convs.push_back(extra);                 // after the channels, before the older directs: the list puts it first below
        }
    }
    bool any_channel = false;
    for (const ConvSummary &s : convs)
        if (s.channel) any_channel = true;
    ChatRow h;
    h.kind = ChatRow::Header;
    h.title = "CHANNELS";
    rows.push_back(h);
    (void)any_channel;
    for (const ConvSummary &s : convs) {
        if (!s.channel) continue;
        ChatRow r;
        r.kind = ChatRow::Channel;
        r.key = s.key;
        r.title = s.title;
        r.unread = s.unread;
        r.muted = s.muted;
        r.last_ts = s.last_ts;
        r.last_text = s.last_text;
        rows.push_back(r);
    }
    ChatRow add;
    add.kind = ChatRow::Add;
    add.key = "+";
    add.title = "+ Add channel";
    rows.push_back(add);
    bool header = false;
    std::stable_sort(convs.begin(), convs.end(), [&](const ConvSummary &a, const ConvSummary &b) {
        return (a.key == also_direct && b.key != also_direct);      // the chat just opened from Contacts first
    });
    for (const ConvSummary &s : convs) {
        if (s.channel) continue;
        if (!header) {
            ChatRow d;
            d.kind = ChatRow::Header;
            d.title = "DIRECT";
            rows.push_back(d);
            header = true;
        }
        ChatRow r;
        r.kind = ChatRow::Direct;
        r.key = s.key;
        r.title = s.title;
        r.unread = s.unread;
        r.muted = s.muted;
        r.last_ts = s.last_ts;
        r.last_text = s.last_text;
        rows.push_back(r);
    }
    return rows;
}

int first_selectable(const std::vector<ChatRow> &rows)
{
    for (size_t i = 0; i < rows.size(); ++i)
        if (rows[i].kind != ChatRow::Header) return static_cast<int>(i);
    return -1;
}

int step_selection(const std::vector<ChatRow> &rows, int current, int delta)
{
    if (rows.empty()) return -1;
    if (current < 0 || current >= static_cast<int>(rows.size()) || rows[static_cast<size_t>(current)].kind == ChatRow::Header)
        return first_selectable(rows);
    if (delta == 0) return current;
    const int step = delta > 0 ? 1 : -1;
    int n = std::abs(delta);
    int i = current;
    while (n > 0) {
        int j = i + step;
        while (j >= 0 && j < static_cast<int>(rows.size()) && rows[static_cast<size_t>(j)].kind == ChatRow::Header) j += step;
        if (j < 0 || j >= static_cast<int>(rows.size())) break;
        i = j;
        --n;
    }
    return i;
}

int step_selectable(const std::vector<bool> &ok, int current, int delta)
{
    const int n = static_cast<int>(ok.size());
    int first = -1;
    for (int i = 0; i < n; ++i)
        if (ok[static_cast<size_t>(i)]) { first = i; break; }
    if (first < 0) return -1;
    if (current < 0 || current >= n || !ok[static_cast<size_t>(current)]) return first;
    const int step = delta > 0 ? 1 : -1;
    int left = std::abs(delta), i = current;
    while (left > 0) {
        int j = i + step;
        while (j >= 0 && j < n && !ok[static_cast<size_t>(j)]) j += step;
        if (j < 0 || j >= n) break;
        i = j;
        --left;
    }
    return i;
}

int find_chat_row(const std::vector<ChatRow> &rows, const std::string &key)
{
    for (size_t i = 0; i < rows.size(); ++i)
        if (rows[i].kind != ChatRow::Header && rows[i].key == key) return static_cast<int>(i);
    return -1;
}

/* ------------------------------------------------------------------ contacts */

const char *sort_name(SortKey k)
{
    switch (k) {
    case SortKey::Heard: return "last heard";
    case SortKey::Name: return "name";
    case SortKey::Type: return "type";
    case SortKey::Snr: return "SNR";
    case SortKey::Hops: return "hops";
    case SortKey::Distance: return "distance";
    }
    return "";
}

SortKey next_sort(SortKey k)
{
    switch (k) {
    case SortKey::Heard: return SortKey::Name;
    case SortKey::Name: return SortKey::Type;
    case SortKey::Type: return SortKey::Snr;
    case SortKey::Snr: return SortKey::Hops;
    case SortKey::Hops: return SortKey::Distance;
    case SortKey::Distance: return SortKey::Heard;
    }
    return SortKey::Heard;
}

bool opens_chat(int type)
{
    return type == advtype::kChat || type == advtype::kNone;
}

const char *type_filter_name(TypeFilter t)
{
    switch (t) {
    case TypeFilter::All: return "All";
    case TypeFilter::Chat: return "Chat";
    case TypeFilter::Repeater: return "Repeater";
    case TypeFilter::Room: return "Room";
    case TypeFilter::Sensor: return "Sensor";
    }
    return "";
}

const char *age_filter_name(AgeFilter a)
{
    switch (a) {
    case AgeFilter::Any: return "Any time";
    case AgeFilter::Hour: return "1 h";
    case AgeFilter::Day: return "24 h";
    case AgeFilter::Week: return "7 d";
    }
    return "";
}

TypeFilter next_type_filter(TypeFilter t)
{
    return static_cast<TypeFilter>((static_cast<int>(t) + 1) % 5);
}

AgeFilter next_age_filter(AgeFilter a)
{
    return static_cast<AgeFilter>((static_cast<int>(a) + 1) % 4);
}

uint32_t age_filter_seconds(AgeFilter a)
{
    switch (a) {
    case AgeFilter::Any: return 0;
    case AgeFilter::Hour: return 3600;
    case AgeFilter::Day: return 86400;
    case AgeFilter::Week: return 7 * 86400;
    }
    return 0;
}

bool type_matches(TypeFilter f, int type)
{
    switch (f) {
    case TypeFilter::All: return true;
    case TypeFilter::Chat: return type == advtype::kChat || type == advtype::kNone;
    case TypeFilter::Repeater: return type == advtype::kRepeater;
    case TypeFilter::Room: return type == advtype::kRoom;
    case TypeFilter::Sensor: return type == advtype::kSensor;
    }
    return true;
}

bool passes_filter(const ContactRow &r, const ContactFilter &f, uint32_t now)
{
    if (!type_matches(f.type, r.type)) return false;
    const uint32_t limit = age_filter_seconds(f.age);
    if (limit == 0) return true;
    if (r.heard == 0 || r.heard < kMinPlausibleTime || now < kMinPlausibleTime) return false;
    return r.heard >= now || now - r.heard <= limit;       // a node heard "in the future" (clock skew) counts as just heard
}

void ContactView::tap_column(SortKey k)
{
    if (k == sort) {
        reverse = !reverse;
    } else {
        sort = k;
        reverse = false;
    }
}

void ContactView::cycle_sort()
{
    sort = next_sort(sort);
    reverse = false;
}

std::string ContactSnapshot::key_at(int index) const
{
    if (index < 0 || index >= static_cast<int>(rows.size())) return "";
    return rows[static_cast<size_t>(index)].key_hex;
}

int ContactSnapshot::index_of(const std::string &key_hex) const
{
    for (size_t i = 0; i < rows.size(); ++i)
        if (rows[i].key_hex == key_hex) return static_cast<int>(i);
    return -1;
}

ContactSnapshot make_contact_snapshot(const Model &model, const ContactView &view, uint32_t now)
{
    ContactSnapshot s;
    s.view = view;
    s.rows = build_contact_rows(model, view.sort, view.reverse, view.filter, now);
    return s;
}

std::vector<ContactRow> build_contact_rows(const Model &model, SortKey sort, bool reverse, const ContactFilter &filter, uint32_t now_in)
{
    const uint32_t now = now_in ? now_in : now_unix();
    std::vector<ContactRow> rows;
    const auto &self = model.self();
    const bool self_pos = self && !(self->lat == 0 && self->lon == 0);
    for (const ContactRec *r : model.contacts_sorted()) {
        ContactRow c;
        c.key_hex = r->key_hex();
        c.name = r->c.name.empty() ? to_hex(r->prefix()) : r->c.name;
        c.type = r->c.type;
        c.type_name = contact_type_name(r->c.type);
        c.heard = r->last_heard();
        c.has_snr = r->has_snr;
        c.snr = r->snr;
        c.hops = path_hops(r->c.out_path_len);
        c.has_pos = !(r->c.lat == 0 && r->c.lon == 0);
        c.lat = r->c.lat;
        c.lon = r->c.lon;
        if (c.has_pos && self_pos) {
            c.has_dist = true;
            c.dist_km = distance_km(self->lat, self->lon, c.lat, c.lon);
        }
        if (filter.active() && !passes_filter(c, filter, now)) continue;
        rows.push_back(c);
    }
    auto lower = [](const std::string &s) {
        std::string o = s;
        for (char &ch : o) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        return o;
    };
    // a total order for every key, unknown values last, ties by name then key (stable and reproducible)
    auto tie = [&](const ContactRow &a, const ContactRow &b) {
        const std::string la = lower(a.name), lb = lower(b.name);
        if (la != lb) return la < lb;
        return a.key_hex < b.key_hex;
    };
    std::stable_sort(rows.begin(), rows.end(), [&](const ContactRow &a, const ContactRow &b) {
        switch (sort) {
        case SortKey::Heard:
            if (a.heard != b.heard) return a.heard > b.heard;
            break;
        case SortKey::Name: {
            const std::string la = lower(a.name), lb = lower(b.name);
            if (la != lb) return la < lb;
            break;
        }
        case SortKey::Type:
            if (a.type != b.type) return a.type < b.type;
            break;
        case SortKey::Snr:
            if (a.has_snr != b.has_snr) return a.has_snr;
            if (a.has_snr && a.snr != b.snr) return a.snr > b.snr;
            break;
        case SortKey::Hops: {
            const int ha = a.hops < 0 ? 1000 : a.hops, hb = b.hops < 0 ? 1000 : b.hops;
            if (ha != hb) return ha < hb;
            break;
        }
        case SortKey::Distance:
            if (a.has_dist != b.has_dist) return a.has_dist;
            if (a.has_dist && a.dist_km != b.dist_km) return a.dist_km < b.dist_km;
            break;
        }
        return tie(a, b);
    });
    if (reverse) std::reverse(rows.begin(), rows.end());
    return rows;
}

/* ------------------------------------------------------------------ settings */

void adjust_setting(RadioSettings &e, int row, int delta)
{
    if (delta == 0) return;
    switch (row) {
    case kRowFreq: e.freq_mhz = std::clamp(std::round((e.freq_mhz + delta * 0.025) * 1000.0) / 1000.0, 137.0, 2500.0); break;
    case kRowBw: {
        const auto &bws = lora_bandwidths();
        int idx = 0;
        for (size_t i = 0; i < bws.size(); ++i)
            if (std::fabs(bws[i] - e.bw_khz) < 0.05) idx = static_cast<int>(i);
        idx = std::clamp(idx + delta, 0, static_cast<int>(bws.size()) - 1);
        e.bw_khz = bws[static_cast<size_t>(idx)];
        break;
    }
    case kRowSf: e.sf = std::clamp(e.sf + delta, 5, 12); break;
    case kRowCr: e.cr = std::clamp(e.cr + delta, 5, 8); break;
    case kRowTx: e.tx_power = std::clamp(e.tx_power + delta, -9, e.max_tx_power > 0 ? e.max_tx_power : 30); break;
    default: break;
    }
}

std::string apply_number(RadioSettings &s, int row, const std::string &text)
{
    char *end = nullptr;
    const double v = std::strtod(text.c_str(), &end);
    RadioSettings t = s;
    std::string bad;
    if (text.empty() || end == text.c_str() || *end != '\0') bad = "Type a number";
    else if (row == kRowFreq) t.freq_mhz = std::round(v * 1000.0) / 1000.0;
    else if (row == kRowBw) {
        bool found = false;
        for (double bw : lora_bandwidths())
            if (std::fabs(bw - v) < 0.05) {
                t.bw_khz = bw;
                found = true;
            }
        if (!found) bad = "Bandwidth: 7.8 10.4 15.6 20.8 31.25 41.7 62.5 125 250 500";
    } else if (row == kRowSf) t.sf = static_cast<int>(std::lround(v));
    else if (row == kRowCr) t.cr = static_cast<int>(std::lround(v));
    else if (row == kRowTx) t.tx_power = static_cast<int>(std::lround(v));
    else bad = "Not a number setting";
    if (bad.empty()) bad = validate_radio(t);
    if (!bad.empty()) return bad;
    s = t;
    return "";
}

std::vector<std::string> describe_radio_changes(const RadioSettings &cur, const RadioSettings &e)
{
    std::vector<std::string> out;
    char b[96];
    if (e.name != cur.name) out.push_back("Name " + (cur.name.empty() ? std::string("-") : cur.name) + " -> " + e.name);
    if (std::fabs(e.freq_mhz - cur.freq_mhz) >= 0.0005) {
        std::snprintf(b, sizeof(b), "Frequency %.3f -> %.3f MHz", cur.freq_mhz, e.freq_mhz);
        out.push_back(b);
    }
    if (std::fabs(e.bw_khz - cur.bw_khz) >= 0.0005) {
        std::snprintf(b, sizeof(b), "Bandwidth %g -> %g kHz", cur.bw_khz, e.bw_khz);
        out.push_back(b);
    }
    if (e.sf != cur.sf) out.push_back("Spreading factor SF" + std::to_string(cur.sf) + " -> SF" + std::to_string(e.sf));
    if (e.cr != cur.cr) out.push_back("Coding rate 4/" + std::to_string(cur.cr) + " -> 4/" + std::to_string(e.cr));
    if (e.tx_power != cur.tx_power) out.push_back("TX power " + std::to_string(cur.tx_power) + " -> " + std::to_string(e.tx_power) + " dBm");
    return out;
}

/* ---- the choice popup */

bool Choice::step(int delta)
{
    if (labels.empty()) return false;
    const int next = std::clamp(index + delta, 0, static_cast<int>(labels.size()) - 1);
    if (next == index) return false;
    index = next;
    return true;
}

const std::string &Choice::label() const
{
    static const std::string empty;
    if (index < 0 || index >= static_cast<int>(labels.size())) return empty;
    return labels[static_cast<size_t>(index)];
}

std::string Choice::detail() const
{
    if (index < 0 || index >= static_cast<int>(details.size())) return "";
    return details[static_cast<size_t>(index)];
}

std::string Choice::position() const
{
    return std::to_string(index + 1) + " / " + std::to_string(labels.size());
}

Choice make_choice(ChoiceField f, const RadioSettings &s, const RetrySettings &retry)
{
    Choice c;
    switch (f) {
    case ChoiceField::Bandwidth:
        c.title = "Bandwidth";
        for (double bw : lora_bandwidths()) c.labels.push_back(fmt_bw_label(bw));
        c.index = bandwidth_index(s.bw_khz);
        c.note = "Narrower reaches further and is slower. The whole mesh must use the same value.";
        break;
    case ChoiceField::Sf:
        c.title = "Spreading factor";
        for (int v : spreading_factors()) c.labels.push_back(fmt_sf_label(v));
        c.index = sf_index(s.sf);
        c.note = "Higher reaches further and is slower. The whole mesh must use the same value.";
        break;
    case ChoiceField::Cr:
        c.title = "Coding rate";
        for (int v : coding_rates()) c.labels.push_back(fmt_cr_label(v));
        c.index = cr_index(s.cr);
        c.note = "Higher repairs more errors and is slower.";
        break;
    case ChoiceField::Tx: {
        c.title = "TX power";
        const int hi = s.max_tx_power > 0 ? s.max_tx_power : 30;
        for (int v = -9; v <= hi; ++v) c.labels.push_back(std::to_string(v) + " dBm");
        c.index = std::clamp(s.tx_power + 9, 0, hi + 9);
        c.note = "Up to the maximum of this board.";
        break;
    }
    case ChoiceField::Preset: {
        c.title = "Radio preset";
        for (const RadioPreset &p : radio_presets()) {
            c.labels.push_back(p.name);
            c.details.push_back(preset_detail(p));
        }
        const int cur = find_preset(s.freq_mhz, s.bw_khz, s.sf, s.cr);
        c.index = cur >= 0 ? cur : 0;
        c.note = cur >= 0 ? "" : "The current settings match no preset.";
        c.note += std::string(c.note.empty() ? "" : "\n") + presets_origin_text() + ".";
        break;
    }
    case ChoiceField::RetryAttempts:
        c.title = "Message tries";
        for (int v = 1; v <= kMaxRetryAttempts; ++v) c.labels.push_back(v == 1 ? "1 try (no retry)" : std::to_string(v) + " tries");
        c.index = std::clamp(retry.attempts, 1, kMaxRetryAttempts) - 1;
        c.note = "How many times a direct message is sent while no acknowledgement comes back. Default 3.";
        break;
    case ChoiceField::ResetAfter: {
        c.title = "Forget the route";
        c.labels.push_back("Never");
        for (int v = 1; v < kMaxRetryAttempts; ++v) c.labels.push_back("Before try " + std::to_string(v + 1));
        c.index = std::clamp(retry.reset_after, 0, kMaxRetryAttempts - 1);
        c.note = "When the stored route fails, the route is forgotten and the message floods. Default: before try 3.";
        break;
    }
    }
    c.initial = c.index;
    return c;
}

void apply_choice(ChoiceField f, int index, RadioSettings &s, RetrySettings &retry)
{
    switch (f) {
    case ChoiceField::Bandwidth: {
        const auto &v = lora_bandwidths();
        if (index >= 0 && index < static_cast<int>(v.size())) s.bw_khz = v[static_cast<size_t>(index)];
        break;
    }
    case ChoiceField::Sf: {
        const auto &v = spreading_factors();
        if (index >= 0 && index < static_cast<int>(v.size())) s.sf = v[static_cast<size_t>(index)];
        break;
    }
    case ChoiceField::Cr: {
        const auto &v = coding_rates();
        if (index >= 0 && index < static_cast<int>(v.size())) s.cr = v[static_cast<size_t>(index)];
        break;
    }
    case ChoiceField::Tx: {
        const int hi = s.max_tx_power > 0 ? s.max_tx_power : 30;
        if (index >= 0 && index <= hi + 9) s.tx_power = index - 9;
        break;
    }
    case ChoiceField::Preset: apply_preset(s, index); break;
    case ChoiceField::RetryAttempts:
        retry.attempts = index + 1;
        retry = normalize_retry(retry);
        break;
    case ChoiceField::ResetAfter:
        retry.reset_after = index;
        retry = normalize_retry(retry);
        break;
    }
}

ChoiceResult choice_key(const KeyEvent &e, Choice &c)
{
    switch (e.key) {
    case Key::Left:
    case Key::Up: return c.step(-1) ? ChoiceResult::Moved : ChoiceResult::None;
    case Key::Right:
    case Key::Down: return c.step(1) ? ChoiceResult::Moved : ChoiceResult::None;
    case Key::PageUp: return c.step(-5) ? ChoiceResult::Moved : ChoiceResult::None;
    case Key::PageDown: return c.step(5) ? ChoiceResult::Moved : ChoiceResult::None;
    case Key::Home: return c.step(-1000) ? ChoiceResult::Moved : ChoiceResult::None;
    case Key::End: return c.step(1000) ? ChoiceResult::Moved : ChoiceResult::None;
    case Key::Enter: return e.repeat ? ChoiceResult::None : ChoiceResult::Accept;
    case Key::Esc: return e.repeat ? ChoiceResult::None : ChoiceResult::Cancel;
    default: return ChoiceResult::None;
    }
}

ConfirmResult confirm_key(const KeyEvent &e, int &focus)
{
    switch (e.key) {
    case Key::Left:
    case Key::Right:
    case Key::Up:
    case Key::Down:
    case Key::Tab:
    case Key::BackTab: {
        const int next = (e.key == Key::Left || e.key == Key::Up || e.key == Key::BackTab) ? 0 : 1;
        if (e.key == Key::Tab) focus = 1 - focus;
        else focus = next;
        return ConfirmResult::Moved;
    }
    case Key::Enter: return e.repeat ? ConfirmResult::None : (focus == 1 ? ConfirmResult::Yes : ConfirmResult::No);
    case Key::Esc: return e.repeat ? ConfirmResult::None : ConfirmResult::No;
    case Key::Char: {
        if (e.ctrl || e.alt || e.repeat || e.text.size() != 1) return ConfirmResult::None;
        const char ch = static_cast<char>(std::tolower(static_cast<unsigned char>(e.text[0])));
        if (ch == 'y') return ConfirmResult::Yes;
        if (ch == 'n') return ConfirmResult::No;
        return ConfirmResult::None;
    }
    default: return ConfirmResult::None;
    }
}

MenuResult menu_key(const KeyEvent &e, int &selected, int count)
{
    if (count <= 0) return e.key == Key::Esc && !e.repeat ? MenuResult::Cancel : MenuResult::None;
    switch (e.key) {
    case Key::Up:
    case Key::Left: {
        const int n = std::clamp(selected - 1, 0, count - 1);
        const bool moved = n != selected;
        selected = n;
        return moved ? MenuResult::Moved : MenuResult::None;
    }
    case Key::Down:
    case Key::Right: {
        const int n = std::clamp(selected + 1, 0, count - 1);
        const bool moved = n != selected;
        selected = n;
        return moved ? MenuResult::Moved : MenuResult::None;
    }
    case Key::Enter: return e.repeat ? MenuResult::None : MenuResult::Accept;
    case Key::Esc: return e.repeat ? MenuResult::None : MenuResult::Cancel;
    default: return MenuResult::None;
    }
}

/* ---- the Settings list */

std::vector<SRowSpec> settings_layout(const SettingsContext &ctx)
{
    std::vector<SRowSpec> rows;
    auto add = [&](SRowKind k, int arg = 0) { rows.push_back({k, arg}); };
    add(SRowKind::Header, 0);                       // BOARD
    add(SRowKind::Info, 0);                         // connection
    add(SRowKind::Info, 1);                         // firmware
    add(SRowKind::Info, 2);                         // battery
    add(SRowKind::Info, 3);                         // clock
    if (ctx.gps.available) add(SRowKind::BoardGps);
    else if (!ctx.gps.notice.empty()) add(SRowKind::GpsNotice);
    add(SRowKind::Header, 1);                       // RADIO
    add(SRowKind::Name);
    add(SRowKind::Preset);
    add(SRowKind::Freq);
    add(SRowKind::Bw);
    add(SRowKind::Sf);
    add(SRowKind::Cr);
    add(SRowKind::Tx);
    add(SRowKind::Header, 2);                       // MESSAGES
    add(SRowKind::RetryAttempts);
    add(SRowKind::ResetAfter);
    add(SRowKind::Header, 3);                       // CLOCK
    add(SRowKind::SyncClock);
    add(SRowKind::Header, 4);                       // CHANNELS
    for (int idx : ctx.channels)
        if (idx != 0) add(SRowKind::Channel, idx);  // the fixed Public channel is never listed
    if (ctx.channel_admin.available) add(SRowKind::AddChannel);
    else if (!ctx.channel_admin.notice.empty()) add(SRowKind::ChannelsNotice);
    if (ctx.connected && ctx.slot0_empty && ctx.channel_admin.available) add(SRowKind::AddPublic);
    add(SRowKind::Header, 6);                       // HISTORY (local messages only: contacts, channels and settings are never touched)
    add(SRowKind::HistoryAll);
    add(SRowKind::HistoryOlder);
    add(SRowKind::HistoryNote);
    add(SRowKind::Header, 5);                       // CHANGES
    add(SRowKind::Undo);                            // the last two rows
    add(SRowKind::Save);
    return rows;
}

bool settings_row_selectable(SRowKind k)
{
    return k != SRowKind::Header && k != SRowKind::Info && k != SRowKind::GpsNotice && k != SRowKind::ChannelsNotice && k != SRowKind::HistoryNote;
}

std::vector<ChannelEntry> build_channel_entries(const Model &model)
{
    std::vector<ChannelEntry> out;
    for (const ChannelRec &ch : model.channels()) {
        if (ch.empty || ch.idx == 0) continue;
        ChannelEntry e;
        e.idx = ch.idx;
        e.name = ch.name;
        e.kind = ch.kind();
        e.muted = model.is_muted(Model::conv_channel(ch.idx));
        e.has_key = ch.has_secret;
        out.push_back(e);
    }
    return out;
}

/* ------------------------------------------------------------------ history */

std::string fmt_message_count(size_t n)
{
    return std::to_string(n) + (n == 1 ? " message" : " messages");
}

ConvOptions conversation_options(const Model &model, const std::string &conv)
{
    ConvOptions o;
    const bool channel = conv.rfind("c:", 0) == 0;
    const bool muted = model.is_muted(conv);
    o.title = truncate_ellipsis(model.conv_title(conv), 24);
    if (channel) {
        o.labels = {muted ? "Unmute this channel" : "Mute this channel", "Delete messages"};
        o.actions = {muted ? ConvAction::Unmute : ConvAction::Mute, ConvAction::DeleteMessages};
    } else {
        o.labels = {muted ? "Unmute this contact" : "Mute this contact", "Delete conversation"};
        o.actions = {muted ? ConvAction::Unmute : ConvAction::Mute, ConvAction::DeleteConversation};
    }
    return o;
}

DeletePrompt delete_conversation_prompt(const Model &model, const std::string &conv)
{
    DeletePrompt p;
    const bool channel = conv.rfind("c:", 0) == 0;
    const std::string name = truncate_ellipsis(model.conv_title(conv), 28);
    p.count = model.message_count_in(conv);
    p.empty = p.count == 0;
    if (channel) {
        p.title = "Delete messages?";
        p.body = "Delete " + fmt_message_count(p.count) + " of " + name + " from this deck? The channel stays; only the history here is cleared. This cannot be undone.";
    } else {
        p.title = "Delete conversation?";
        p.body = "Delete " + fmt_message_count(p.count) + " with " + name + " from this deck? " + name +
                 " stays in Contacts; a new message starts a new conversation. This cannot be undone.";
    }
    return p;
}

DeletePrompt delete_all_prompt(const Model &model)
{
    DeletePrompt p;
    p.count = model.message_count();
    p.empty = p.count == 0;
    p.title = "Delete all messages?";
    p.body = "Delete all " + fmt_message_count(p.count) + " of every conversation from this deck? Contacts, channels and settings stay. This cannot be undone.";
    return p;
}

DeletePrompt delete_older_prompt(const Model &model, int days, uint32_t now)
{
    DeletePrompt p;
    const uint32_t cutoff = history_cutoff(now, days);
    p.count = cutoff ? model.count_older_than(cutoff) : 0;
    p.empty = p.count == 0;
    p.title = "Delete old messages?";
    p.body = "Delete " + fmt_message_count(p.count) + " older than " + std::to_string(days) +
             " days from this deck? Newer messages, contacts, channels and settings stay. This cannot be undone.";
    return p;
}

const std::vector<int> &history_day_options()
{
    static const std::vector<int> v = {7, 30, 90};
    return v;
}

Choice make_days_choice(int initial_days)
{
    Choice c;
    c.title = "Delete messages older than";
    int idx = 1;
    const auto &v = history_day_options();
    for (size_t i = 0; i < v.size(); ++i) {
        c.labels.push_back(std::to_string(v[i]) + " days");
        if (v[i] == initial_days) idx = static_cast<int>(i);
    }
    c.index = c.initial = idx;
    c.note = "You are asked to confirm, with the number of messages, before anything is deleted.";
    return c;
}

uint32_t history_cutoff(uint32_t now, int days)
{
    if (now < meshzero::kMinPlausibleTime || days <= 0) return 0;
    const uint64_t span = static_cast<uint64_t>(days) * 86400u;
    return now > span ? static_cast<uint32_t>(now - span) : 0;
}

void HoldTracker::begin(const std::string &target, int x, int y, uint64_t now)
{
    active_ = true;
    swallow_ = false;
    target_ = target;
    x_ = x;
    y_ = y;
    start_ = now;
}

void HoldTracker::move(int x, int y)
{
    if (!active_) return;
    if (std::abs(x - x_) > kSlop || std::abs(y - y_) > kSlop) active_ = false;
}

void HoldTracker::cancel()
{
    active_ = false;
}

double HoldTracker::progress(uint64_t now) const
{
    if (!active_) return 0;
    const double p = static_cast<double>(now - start_) / static_cast<double>(kFireMs);
    return p < 0 ? 0 : p > 1 ? 1 : p;
}

bool HoldTracker::poll(uint64_t now)
{
    if (!active_ || now - start_ < kFireMs) return false;
    active_ = false;
    swallow_ = true;
    return true;
}

bool HoldTracker::take_swallow()
{
    const bool s = swallow_;
    swallow_ = false;
    return s;
}

bool in_eu868(double mhz)
{
    return mhz >= 863.0 && mhz <= 870.0;
}

/* ------------------------------------------------------------------ navigation */

const char *tab_name(Tab t)
{
    switch (t) {
    case Tab::Chats: return "Chats";
    case Tab::Map: return "Map";
    case Tab::Contacts: return "Contacts";
    case Tab::Terminal: return "Terminal";
    case Tab::Settings: return "Settings";
    }
    return "";
}

Tab step_tab(Tab t, int delta)
{
    const int n = 5;
    const int i = (static_cast<int>(t) + delta % n + n) % n;
    return static_cast<Tab>(i);
}

BackAction back_action(const NavState &s)
{
    if (s.popup_open) return BackAction::ClosePopup;
    if (s.editor_open) return BackAction::CancelEditor;
    if (s.detail_open) return BackAction::CloseDetail;
    if (s.tab == Tab::Chats && s.compose_focus) return BackAction::FocusList;
    return BackAction::ExitHint;
}

/* ------------------------------------------------------------------ messages */

StatusText message_status(const Message &m, bool channel)
{
    StatusText s;
    if (m.dir != Dir::Out) return s;
    switch (m.state) {
    case MsgState::Pending: s.text = m.note.empty() ? "sending" : m.note; s.tone = Tone::Gold; break;
    case MsgState::Sent:
        if (channel) {
            s.text = m.note.empty() ? "sent" : "sent (unconfirmed)";
            s.tone = m.note.empty() ? Tone::Blue : Tone::Gold;
        } else {
            s.text = "sent, waiting for ack";
            s.tone = Tone::Blue;
        }
        break;
    case MsgState::Delivered:
        if (channel) { s.text = "sent"; s.tone = Tone::Blue; }       // a channel has no ack: never "delivered"
        else { s.text = "delivered"; s.tone = Tone::Green; }
        break;
    case MsgState::Failed: s.text = "failed: " + m.note; s.tone = Tone::Red; break;
    case MsgState::NoAck: s.text = "no ack"; s.tone = Tone::Red; break;
    default: break;
    }
    return s;
}

BoardLine board_line(const Client &c, const Model &model)
{
    BoardLine b;
    std::string model_name;
    if (const auto &dev = model.device()) model_name = dev->model;
    switch (c.link()) {
    case Link::Ready:
        b.text = "board: " + (model_name.empty() ? std::string("companion radio") : model_name) + "  OK";
        b.tone = Tone::Green;
        break;
    case Link::Handshake: b.text = "board: connecting"; b.tone = Tone::Gold; break;
    case Link::Searching: b.text = "board: not found"; b.tone = Tone::Muted; break;
    case Link::Denied: b.text = "board: no permission"; b.tone = Tone::Red; break;
    case Link::Busy: b.text = "board: port busy"; b.tone = Tone::Red; break;
    case Link::Error: b.text = "board: port error"; b.tone = Tone::Red; break;
    case Link::NotCompanion: b.text = "board: wrong firmware"; b.tone = Tone::Red; break;
    }
    return b;
}

} // namespace meshhop
