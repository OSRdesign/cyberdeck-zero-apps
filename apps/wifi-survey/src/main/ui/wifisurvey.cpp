/*
 * SPDX-License-Identifier: MIT
 */

#include "wifisurvey.hpp"

#include "cp0_keyboard_navigation_contract.h"
#include "input_keys.h"
#include "keyboard_input.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <utility>

namespace {

constexpr uint32_t kBackground = 0x101214;
constexpr uint32_t kSelected = 0x2A2F35;
constexpr uint32_t kTrack = 0x24292E;
constexpr uint32_t kText = 0xF4F4F5;
constexpr uint32_t kMuted = 0x8A929B;
constexpr uint32_t kGold = 0xF0B400;
constexpr uint32_t kGreen = 0x33CC33;
constexpr uint32_t kBlue = 0x3B9DFF;
constexpr uint32_t kRed = 0xE5604D;

constexpr int kWidth = 320;
constexpr int kContentH = 150;
constexpr int kHeaderH = 18;
constexpr int kFooterH = 16;
constexpr int kRowH = 17;
constexpr int kColHdrH = 14;   // column titles above the rows (the unit "dBm" lives here, not in every row)
constexpr int kTrackW = 20;    // signal bar width
constexpr int kScanEveryMs = 5000;
constexpr uint32_t kStartGuardMs = 700;   // ignore Enter / taps this long after start (the one that launched us)

// 2.4 GHz chart (13 columns of 24 px)
constexpr int kB24Pitch = 24;
constexpr int kB24Base = 68;       // y of the baseline
constexpr int kB24Max = 22;        // tallest bar
// 5 GHz chart
constexpr int kB5Base = 119;
constexpr int kB5Max = 18;

std::atomic<bool> g_quit{false};

lv_obj_t *make_box(lv_obj_t *parent, int x, int y, int w, int h)
{
    lv_obj_t *box = lv_obj_create(parent);
    lv_obj_remove_style_all(box);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(box, x, y);
    lv_obj_set_size(box, w, h);
    return box;
}

lv_obj_t *make_label(lv_obj_t *parent, const lv_font_t *font, uint32_t color, int x, int y, int w, int h,
                     lv_text_align_t align = LV_TEXT_ALIGN_LEFT)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
    lv_obj_set_style_text_align(label, align, 0);
    lv_label_set_text(label, "");
    lv_obj_set_pos(label, x, y);
    lv_obj_set_size(label, w, h);                       // fixed size: "dots" mode wraps when the height is automatic
    lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_DOTS);
    return label;
}

void set_hidden(lv_obj_t *obj, bool hidden)
{
    if (hidden) lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_remove_flag(obj, LV_OBJ_FLAG_HIDDEN);
}

uint32_t signal_color(int pct)
{
    return pct >= 60 ? kGreen : (pct >= 35 ? kGold : kRed);
}

const char *band_text(int band)
{
    return band == 0 ? "2.4 GHz" : (band == 1 ? "5 GHz" : "6 GHz");
}

/* The 2.4 GHz channel (1..13) that interferes least: channels closer than 5 apart overlap, so a network
 * counts for the channel it is on and, less, for its neighbours. Ties go to 1, 6, 11, then the lowest. */
int least_crowded_24(const std::vector<wifisurvey::AccessPoint> &aps)
{
    int best = 0, best_score = 0;
    for (int ch = 1; ch <= 13; ++ch) {
        int score = 0;
        for (const auto &ap : aps)
            if (ap.band() == 0) score += std::max(0, 5 - std::abs(ap.channel - ch));
        const bool preferred = ch == 1 || ch == 6 || ch == 11;
        const bool best_preferred = best == 1 || best == 6 || best == 11;
        if (best == 0 || score < best_score || (score == best_score && preferred && !best_preferred)) {
            best = ch;
            best_score = score;
        }
    }
    return best;
}

} // namespace

bool wifisurvey_quit_requested()
{
    return g_quit.load();
}

UIWifiSurveyPage::UIWifiSurveyPage()
{
    set_page_title("Wi-Fi Survey");

    root_ = make_box(ui_APP_Container, 0, 0, kWidth, kContentH);
    lv_obj_set_style_bg_color(root_, lv_color_hex(kBackground), 0);
    lv_obj_set_style_bg_opa(root_, LV_OPA_COVER, 0);

    // header: two tabs on the left (touch targets), the scan status on the right (tap = refresh)
    tab_net_ = make_box(root_, 0, 0, 84, kHeaderH);
    lv_obj_add_flag(tab_net_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(tab_net_, tab_event_cb, LV_EVENT_CLICKED, this);
    tab_net_label_ = make_label(tab_net_, &lv_font_montserrat_14, kGold, 6, 1, 76, 16);
    lv_label_set_text(tab_net_label_, "Networks");
    tab_ch_ = make_box(root_, 84, 0, 84, kHeaderH);
    lv_obj_add_flag(tab_ch_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(tab_ch_, tab_event_cb, LV_EVENT_CLICKED, this);
    tab_ch_label_ = make_label(tab_ch_, &lv_font_montserrat_14, kMuted, 6, 1, 76, 16);
    lv_label_set_text(tab_ch_label_, "Channels");
    title_ = make_label(root_, &lv_font_montserrat_14, kGold, 6, 1, 200, 16);

    status_ = make_label(root_, &lv_font_montserrat_12, kGreen, kWidth - 150, 0, 146, kHeaderH, LV_TEXT_ALIGN_RIGHT);
    lv_obj_set_style_pad_top(status_, 3, 0);
    lv_obj_add_flag(status_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(status_, tab_event_cb, LV_EVENT_CLICKED, this);

    build_networks();
    build_channels();
    build_detail();

    msg_ = make_label(root_, &lv_font_montserrat_14, kMuted, 10, 50, kWidth - 20, 60, LV_TEXT_ALIGN_CENTER);
    lv_label_set_long_mode(msg_, LV_LABEL_LONG_MODE_WRAP);

    footer_ = make_label(root_, &lv_font_montserrat_12, kMuted, 6, kContentH - kFooterH + 1, kWidth - 8, 14);

    lv_obj_add_event_cb(root_screen_, key_event_cb, LV_EVENT_KEY, this);
    // The raw evdev key (needed for "R": its code equals LV_KEY_RIGHT once converted). The keyboard may be asleep
    // and absent; nothing here depends on it, touch does everything.
    // Must be the page's own screen: it is only loaded (made active) after this constructor returns, so
    // lv_screen_active() here is a different screen and the raw R event never arrived (R acted as Right).
    // LV_EVENT_KEYBOARD is sent to the active screen, which will be root_screen_.
    keyboard_root_ = root_screen_;
    if (keyboard_root_ && LV_EVENT_KEYBOARD != 0)
        keyboard_dsc_ = lv_obj_add_event_cb(keyboard_root_, &UIWifiSurveyPage::keyboard_event_cb,
                                            static_cast<lv_event_code_t>(LV_EVENT_KEYBOARD), this);

    start_tick_ = lv_tick_get();
    timer_ = lv_timer_create(timer_cb, 250, this);
    set_view(View::Networks);
    request_scan();
    poll();
}

UIWifiSurveyPage::~UIWifiSurveyPage()
{
    if (timer_) lv_timer_delete(timer_);
    if (keyboard_root_ && keyboard_dsc_) lv_obj_remove_event_dsc(keyboard_root_, keyboard_dsc_);
}

/* ------------------------------------------------------------ construction */

void UIWifiSurveyPage::build_networks()
{
    net_box_ = make_box(root_, 0, kHeaderH, kWidth, kColHdrH + kRows * kRowH);
    lv_obj_t *h = make_label(net_box_, &lv_font_montserrat_12, kMuted, 14, 0, 96, kColHdrH);
    lv_label_set_text(h, "Network");
    h = make_label(net_box_, &lv_font_montserrat_12, kMuted, 144, 0, 34, kColHdrH, LV_TEXT_ALIGN_RIGHT);
    lv_label_set_text(h, "dBm");
    h = make_label(net_box_, &lv_font_montserrat_12, kMuted, 180, 0, 26, kColHdrH, LV_TEXT_ALIGN_RIGHT);
    lv_label_set_text(h, "Ch");
    h = make_label(net_box_, &lv_font_montserrat_12, kMuted, 212, 0, 46, kColHdrH);
    lv_label_set_text(h, "Band");
    h = make_label(net_box_, &lv_font_montserrat_12, kMuted, 262, 0, 56, kColHdrH);
    lv_label_set_text(h, "Security");
    for (int i = 0; i < kRows; ++i) {
        lv_obj_t *row = make_box(net_box_, 0, kColHdrH + i * kRowH, kWidth, kRowH);
        rows_[i] = row;
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_bg_color(row, lv_color_hex(kSelected), 0);
        lv_obj_set_user_data(row, reinterpret_cast<void *>(static_cast<intptr_t>(i)));
        lv_obj_add_event_cb(row, row_event_cb, LV_EVENT_ALL, this);

        row_mark_[i] = make_label(row, &lv_font_montserrat_14, kGreen, 2, 1, 14, 16);
        row_ssid_[i] = make_label(row, &lv_font_montserrat_14, kText, 14, 0, 106, 16);
        row_track_[i] = make_box(row, 122, 6, kTrackW, 5);
        lv_obj_set_style_bg_color(row_track_[i], lv_color_hex(kTrack), 0);
        lv_obj_set_style_bg_opa(row_track_[i], LV_OPA_COVER, 0);
        row_fill_[i] = make_box(row_track_[i], 0, 0, kTrackW / 2, 5);
        lv_obj_set_style_bg_opa(row_fill_[i], LV_OPA_COVER, 0);
        row_dbm_[i] = make_label(row, &lv_font_montserrat_12, kMuted, 144, 2, 34, 14, LV_TEXT_ALIGN_RIGHT);
        row_chan_[i] = make_label(row, &lv_font_montserrat_12, kText, 180, 2, 26, 14, LV_TEXT_ALIGN_RIGHT);
        row_band_[i] = make_label(row, &lv_font_montserrat_12, kMuted, 212, 2, 46, 14);
        row_sec_[i] = make_label(row, &lv_font_montserrat_12, kMuted, 262, 2, 56, 14);
    }
}

void UIWifiSurveyPage::build_channels()
{
    ch_box_ = make_box(root_, 0, kHeaderH, kWidth, kContentH - kHeaderH - kFooterH);
    // child coordinates are relative to ch_box_, whose origin is y = kHeaderH of root_: subtract it from the plan
    const int oy = kHeaderH;
    ch_title_[0] = make_label(ch_box_, &lv_font_montserrat_12, kMuted, 6, 19 - oy, kWidth - 10, 14);
    ch_title_[1] = make_label(ch_box_, &lv_font_montserrat_12, kMuted, 6, 85 - oy, kWidth - 10, 14);
    for (int i = 0; i < kSlots; ++i) {
        bar24_[i] = make_box(ch_box_, 4 + i * kB24Pitch + 3, kB24Base - oy - 2, 18, 2);
        lv_obj_set_style_bg_opa(bar24_[i], LV_OPA_COVER, 0);
        cnt24_[i] = make_label(ch_box_, &lv_font_montserrat_12, kText, 4 + i * kB24Pitch, kB24Base - oy - 16, kB24Pitch,
                               14, LV_TEXT_ALIGN_CENTER);
        lab24_[i] = make_label(ch_box_, &lv_font_montserrat_12, kMuted, 4 + i * kB24Pitch, kB24Base - oy + 1, kB24Pitch,
                               14, LV_TEXT_ALIGN_CENTER);
        bar5_[i] = make_box(ch_box_, 0, kB5Base - oy - 2, 18, 2);
        lv_obj_set_style_bg_opa(bar5_[i], LV_OPA_COVER, 0);
        lab5_[i] = make_label(ch_box_, &lv_font_montserrat_12, kMuted, 0, kB5Base - oy + 1, kB24Pitch, 14,
                              LV_TEXT_ALIGN_CENTER);
    }
}

void UIWifiSurveyPage::build_detail()
{
    det_box_ = make_box(root_, 0, kHeaderH, kWidth, kContentH - kHeaderH - kFooterH);
    static const char *const keys[kDetailRows] = {"BSSID", "Vendor", "Signal", "Channel", "Security", "Rate", "Status"};
    for (int i = 0; i < kDetailRows; ++i) {
        det_key_[i] = make_label(det_box_, &lv_font_montserrat_12, kMuted, 6, i * 16 + 2, 70, 14);
        lv_label_set_text(det_key_[i], keys[i]);
        det_val_[i] = make_label(det_box_, &lv_font_montserrat_14, kText, 78, i * 16, kWidth - 82, 16);
    }
}

/* ------------------------------------------------------------ state */

void UIWifiSurveyPage::set_view(View view)
{
    view_ = view;
    if (view != View::Detail) tab_ = view;
    const bool detail = view == View::Detail;
    set_hidden(net_box_, view != View::Networks);
    set_hidden(ch_box_, view != View::Channels);
    set_hidden(det_box_, !detail);
    set_hidden(tab_net_, detail);
    set_hidden(tab_ch_, detail);
    set_hidden(title_, !detail);
    lv_obj_set_style_text_color(tab_net_label_, lv_color_hex(view == View::Networks ? kGold : kMuted), 0);
    lv_obj_set_style_text_color(tab_ch_label_, lv_color_hex(view == View::Channels ? kGold : kMuted), 0);
    hint_on_ = false;
    show_footer();
    refresh();
}

void UIWifiSurveyPage::show_footer()
{
    const char *footer = hint_on_ ? wifisurvey::kEscHint
                         : view_ == View::Networks ? "Enter: detail   Tab: channels   R: refresh   Hold Esc: exit"
                         : view_ == View::Channels ? "Tab: networks   R: refresh   Hold Esc: exit"
                                                   : "Left/Right: prev/next   R: refresh   Esc: back";
    lv_label_set_text(footer_, footer);
}

void UIWifiSurveyPage::toggle_tab(int delta)
{
    (void)delta;   // two tabs: any direction switches
    if (view_ == View::Networks) set_view(View::Channels);
    else if (view_ == View::Channels) set_view(View::Networks);
}

void UIWifiSurveyPage::request_scan()
{
    scanner_.request();
    last_scan_start_ = lv_tick_get();
}

void UIWifiSurveyPage::apply_snapshot()
{
    aps_ = snap_.aps;
    std::sort(aps_.begin(), aps_.end(), [](const wifisurvey::AccessPoint &a, const wifisurvey::AccessPoint &b) {
        if (a.signal_pct != b.signal_pct) return a.signal_pct > b.signal_pct;
        if (a.ssid != b.ssid) return a.ssid < b.ssid;
        return a.bssid < b.bssid;
    });
    // keep the selection on the same access point when the order changes
    const int idx = selected_index();
    if (idx >= 0) {
        selected_ = idx;
    } else {
        selected_ = std::clamp(selected_, 0, std::max(0, static_cast<int>(aps_.size()) - 1));
        selected_bssid_ = aps_.empty() ? "" : aps_[static_cast<size_t>(selected_)].bssid;
    }
    if (selected_ < offset_) offset_ = selected_;
    if (selected_ >= offset_ + kRows) offset_ = selected_ - kRows + 1;
    offset_ = std::max(0, std::min(offset_, std::max(0, static_cast<int>(aps_.size()) - kRows)));

    if (view_ == View::Detail) {
        detail_present_ = false;
        for (const auto &ap : aps_)
            if (ap.bssid == detail_ap_.bssid) {
                detail_ap_ = ap;
                detail_present_ = true;
                break;
            }
    }
}

int UIWifiSurveyPage::selected_index() const
{
    for (size_t i = 0; i < aps_.size(); ++i)
        if (aps_[i].bssid == selected_bssid_) return static_cast<int>(i);
    return -1;
}

void UIWifiSurveyPage::select_bssid(const std::string &bssid)
{
    selected_bssid_ = bssid;
}

void UIWifiSurveyPage::poll()
{
    static uint32_t done_tick = 0;
    const wifisurvey::Snapshot s = scanner_.snapshot();
    const bool changed = s.generation != snap_.generation || s.scanning != snap_.scanning;
    const bool fresh = s.generation != snap_.generation;
    snap_ = s;
    if (hint_on_ && lv_tick_elaps(hint_tick_) >= kHintMs) {
        hint_on_ = false;
        show_footer();
    }
    if (fresh) {
        apply_snapshot();
        done_tick = lv_tick_get();
    }
    if (!s.scanning && lv_tick_elaps(done_tick) >= static_cast<uint32_t>(kScanEveryMs) &&
        lv_tick_elaps(last_scan_start_) >= static_cast<uint32_t>(kScanEveryMs))
        request_scan();
    if (changed) refresh();
}

void UIWifiSurveyPage::move_selection(int delta)
{
    if (aps_.empty()) return;
    if (view_ == View::Detail) {
        // step from the network the detail shows (tracked by BSSID), not from a stale list position
        for (size_t i = 0; i < aps_.size(); ++i)
            if (aps_[i].bssid == detail_ap_.bssid) {
                selected_ = static_cast<int>(i);
                break;
            }
    }
    selected_ = std::clamp(selected_ + delta, 0, static_cast<int>(aps_.size()) - 1);
    selected_bssid_ = aps_[static_cast<size_t>(selected_)].bssid;
    if (selected_ < offset_) offset_ = selected_;
    if (selected_ >= offset_ + kRows) offset_ = selected_ - kRows + 1;
    if (view_ == View::Detail) {
        detail_ap_ = aps_[static_cast<size_t>(selected_)];
        detail_present_ = true;
    }
    refresh();
}

void UIWifiSurveyPage::activate()
{
    if (view_ != View::Networks || aps_.empty()) return;
    // the Enter / tap that started the app can still be in flight: it must not open a detail by itself
    if (lv_tick_elaps(start_tick_) < kStartGuardMs) return;
    detail_ap_ = aps_[static_cast<size_t>(selected_)];
    detail_present_ = true;
    set_view(View::Detail);
}

void UIWifiSurveyPage::back()
{
    // short Esc is Back only, never quit (hold Esc 3 s is handled by the launcher)
    const wifisurvey::EscResult r = wifisurvey::esc_short_press(view_, tab_);
    if (r.hint) {
        hint_on_ = true;
        hint_tick_ = lv_tick_get();
        show_footer();
    } else {
        set_view(r.next);
    }
}

/* ------------------------------------------------------------ rendering */

std::string UIWifiSurveyPage::message() const
{
    if (!aps_.empty()) return "";
    switch (snap_.state) {
    case wifisurvey::State::Idle: return "Scanning...";
    case wifisurvey::State::Ok: return snap_.scanning ? "Scanning..." : "No networks found";
    default: return snap_.note;
    }
}

void UIWifiSurveyPage::refresh()
{
    char text[96];
    const bool error = snap_.state != wifisurvey::State::Ok && snap_.state != wifisurvey::State::Idle;
    if (snap_.scanning && !aps_.empty()) std::snprintf(text, sizeof(text), "%zu networks  scanning", aps_.size());
    else if (snap_.scanning) std::snprintf(text, sizeof(text), "scanning");
    else if (error) std::snprintf(text, sizeof(text), "tap to retry");
    else if (snap_.cached) std::snprintf(text, sizeof(text), "%zu networks (old)", aps_.size());
    else std::snprintf(text, sizeof(text), "%zu networks", aps_.size());
    lv_label_set_text(status_, text);
    lv_obj_set_style_text_color(status_, lv_color_hex(error ? kRed : (snap_.scanning ? kGold : kGreen)), 0);

    const std::string msg = view_ == View::Detail ? "" : message();
    lv_label_set_text(msg_, msg.c_str());
    lv_obj_set_style_text_color(msg_, lv_color_hex(error ? kRed : kMuted), 0);
    set_hidden(msg_, msg.empty());

    if (view_ == View::Networks) render_networks();
    else if (view_ == View::Channels) render_channels();
    else render_detail();
}

void UIWifiSurveyPage::render_networks()
{
    for (int i = 0; i < kRows; ++i) {
        const int index = offset_ + i;
        if (index >= static_cast<int>(aps_.size())) {
            set_hidden(rows_[i], true);
            continue;
        }
        set_hidden(rows_[i], false);
        const wifisurvey::AccessPoint &ap = aps_[static_cast<size_t>(index)];
        lv_obj_set_style_bg_opa(rows_[i], index == selected_ ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
        lv_label_set_text(row_mark_[i], ap.connected ? LV_SYMBOL_OK : "");
        lv_label_set_text(row_ssid_[i], ap.hidden() ? "hidden" : ap.ssid.c_str());
        lv_obj_set_style_text_color(row_ssid_[i], lv_color_hex(ap.connected ? kGreen : (ap.hidden() ? kMuted : kText)), 0);
        lv_obj_set_width(row_fill_[i], std::max(1, ap.signal_pct * kTrackW / 100));
        lv_obj_set_style_bg_color(row_fill_[i], lv_color_hex(signal_color(ap.signal_pct)), 0);
        char text[32];
        std::snprintf(text, sizeof(text), "%d", ap.dbm());   // the unit is in the column title
        lv_label_set_text(row_dbm_[i], text);
        std::snprintf(text, sizeof(text), "%d", ap.channel);
        lv_label_set_text(row_chan_[i], text);
        lv_label_set_text(row_band_[i], band_text(ap.band()));
        const std::string sec = ap.security_short();
        lv_label_set_text(row_sec_[i], sec.c_str());
        lv_obj_set_style_text_color(row_sec_[i], lv_color_hex(sec == "Open" || sec == "WEP" ? kRed : kMuted), 0);
    }
}

void UIWifiSurveyPage::render_channels()
{
    char text[96];
    const int oy = kHeaderH;

    // ---- 2.4 GHz
    int count24[14] = {};
    int total24 = 0;
    for (const auto &ap : aps_)
        if (ap.band() == 0 && ap.channel >= 1 && ap.channel <= 13) {
            ++count24[ap.channel];
            ++total24;
        }
    const int best24 = aps_.empty() ? 0 : least_crowded_24(aps_);
    if (aps_.empty()) std::snprintf(text, sizeof(text), "2.4 GHz");
    else std::snprintf(text, sizeof(text), "2.4 GHz   %d networks   least crowded: channel %d", total24, best24);
    lv_label_set_text(ch_title_[0], text);
    int max24 = 1;
    for (int ch = 1; ch <= 13; ++ch) max24 = std::max(max24, count24[ch]);
    const int unit24 = std::max(1, std::min(6, kB24Max / max24));
    for (int ch = 1; ch <= 13; ++ch) {
        const int i = ch - 1;
        const int n = count24[ch];
        const int h = n > 0 ? std::min(kB24Max, n * unit24) : 2;
        const bool best = ch == best24;
        lv_obj_set_size(bar24_[i], 18, h);
        lv_obj_set_y(bar24_[i], kB24Base - oy - h);
        lv_obj_set_style_bg_color(bar24_[i], lv_color_hex(best ? kGreen : (n > 0 ? kBlue : kTrack)), 0);
        std::snprintf(text, sizeof(text), "%d", n);
        lv_label_set_text(cnt24_[i], n > 0 ? text : "");
        lv_obj_set_y(cnt24_[i], kB24Base - oy - h - 15);
        std::snprintf(text, sizeof(text), "%d", ch);
        lv_label_set_text(lab24_[i], text);
        lv_obj_set_style_text_color(lab24_[i], lv_color_hex(best ? kGreen : kMuted), 0);
    }

    // ---- 5 GHz: the channels seen plus the usual ones (UNII-1 and UNII-3)
    std::map<int, int> count5;
    int total5 = 0;
    for (const auto &ap : aps_)
        if (ap.band() == 1) {
            ++count5[ap.channel];
            ++total5;
        }
    std::vector<int> chans;
    if (total5 > 0) {
        std::map<int, int> all = count5;
        for (int ch : {36, 40, 44, 48, 149, 153, 157, 161, 165}) all.emplace(ch, 0);
        if (static_cast<int>(all.size()) > kSlots) all = count5;
        for (const auto &kv : all) {
            if (static_cast<int>(chans.size()) >= kSlots) break;
            chans.push_back(kv.first);
        }
    }
    int best5 = 0, max5 = 1;
    for (int ch : chans) {
        const int n = count5.count(ch) ? count5[ch] : 0;
        max5 = std::max(max5, n);
        if (best5 == 0 || n < (count5.count(best5) ? count5[best5] : 0)) best5 = ch;
    }
    if (chans.empty()) std::snprintf(text, sizeof(text), "5 GHz   no networks seen (the adapter may be 2.4 GHz only)");
    else std::snprintf(text, sizeof(text), "5 GHz   %d networks   least crowded: channel %d", total5, best5);
    lv_label_set_text(ch_title_[1], text);
    const int n5 = static_cast<int>(chans.size());
    const int pitch = n5 > 0 ? std::min(kB24Pitch, (kWidth - 8) / n5) : kB24Pitch;
    const int bar_w = std::max(6, pitch - 6);
    const int unit5 = std::max(1, std::min(6, kB5Max / max5));
    for (int i = 0; i < kSlots; ++i) {
        if (i >= n5) {
            set_hidden(bar5_[i], true);
            set_hidden(lab5_[i], true);
            continue;
        }
        set_hidden(bar5_[i], false);
        set_hidden(lab5_[i], false);
        const int ch = chans[static_cast<size_t>(i)];
        const int n = count5.count(ch) ? count5[ch] : 0;
        const int h = n > 0 ? std::min(kB5Max, n * unit5) : 2;
        const bool best = ch == best5;
        lv_obj_set_size(bar5_[i], bar_w, h);
        lv_obj_set_pos(bar5_[i], 4 + i * pitch + (pitch - bar_w) / 2, kB5Base - oy - h);
        lv_obj_set_style_bg_color(bar5_[i], lv_color_hex(best ? kGreen : (n > 0 ? kBlue : kTrack)), 0);
        std::snprintf(text, sizeof(text), "%d", ch);
        lv_label_set_text(lab5_[i], text);
        lv_obj_set_pos(lab5_[i], 4 + i * pitch, kB5Base - oy + 1);
        lv_obj_set_width(lab5_[i], pitch);
        lv_obj_set_style_text_color(lab5_[i], lv_color_hex(best ? kGreen : kMuted), 0);
    }
}

void UIWifiSurveyPage::render_detail()
{
    const wifisurvey::AccessPoint &ap = detail_ap_;
    char text[128];
    std::string title = ap.hidden() ? "(hidden network)" : ap.ssid;
    if (!detail_present_) title += "  - gone";
    lv_label_set_text(title_, title.c_str());
    lv_obj_set_style_text_color(title_, lv_color_hex(detail_present_ ? kGold : kRed), 0);

    lv_label_set_text(det_val_[0], ap.bssid.c_str());
    lv_label_set_text(det_val_[1], ap.vendor.empty() ? "unknown" : ap.vendor.c_str());
    std::snprintf(text, sizeof(text), "%d dBm (about), %d%%", ap.dbm(), ap.signal_pct);
    lv_label_set_text(det_val_[2], text);
    lv_obj_set_style_text_color(det_val_[2], lv_color_hex(signal_color(ap.signal_pct)), 0);
    std::snprintf(text, sizeof(text), "%d   (%d MHz, %s GHz)", ap.channel, ap.freq_mhz,
                  ap.band() == 0 ? "2.4" : (ap.band() == 1 ? "5" : "6"));
    lv_label_set_text(det_val_[3], text);
    lv_label_set_text(det_val_[4], ap.security.empty() || ap.security == "--" ? "Open (no encryption)" : ap.security.c_str());
    std::snprintf(text, sizeof(text), "%d Mbit/s", ap.rate_mbps);
    lv_label_set_text(det_val_[5], text);
    std::snprintf(text, sizeof(text), "%s%s%s", ap.infra ? "Access point" : "Ad-hoc",
                  ap.connected && detail_present_ ? ", connected" : "",
                  detail_present_ ? "" : "  - gone (last values)");
    lv_label_set_text(det_val_[6], text);
    lv_obj_set_style_text_color(det_val_[6], lv_color_hex(ap.connected ? kGreen : (detail_present_ ? kText : kRed)), 0);
}

/* ------------------------------------------------------------ input */

void UIWifiSurveyPage::handle_key(lv_event_t *event)
{
    uint32_t key = lv_event_get_key(event);
    // "R" arrives as KEY_R (19) which equals LV_KEY_RIGHT: the raw handler flags a real R press just before.
    // Window not cleared on use: key repeat of a held R produces several LV_KEY_RIGHT events.
    if (key == KEY_R && raw_r_ && lv_tick_elaps(raw_r_tick_) < 300) return;
    key = cp0_keyboard_navigation_alias(key);
    if (key == LV_KEY_UP || key == KEY_UP) move_selection(-1);
    else if (key == LV_KEY_DOWN || key == KEY_DOWN) move_selection(1);
    else if (key == LV_KEY_RIGHT || key == KEY_RIGHT || key == LV_KEY_LEFT || key == KEY_LEFT) {
        const bool right = key == LV_KEY_RIGHT || key == KEY_RIGHT;
        if (view_ == View::Detail) move_selection(right ? 1 : -1);
        else toggle_tab(right ? 1 : -1);
    } else if (key == LV_KEY_ENTER || key == KEY_ENTER) activate();
    else if (key == LV_KEY_ESC || key == KEY_ESC) back();
    else if (key == KEY_TAB || key == '\t') toggle_tab(1);
    else if (key == 'r' || key == 'R') {
        request_scan();
        refresh();
    }
}

void UIWifiSurveyPage::handle_raw_key(lv_event_t *event)
{
    const auto *item = static_cast<const struct key_item *>(lv_event_get_param(event));
    if (!item || item->key_code != KEY_R) return;
    raw_r_ = true;                       // press, repeat and release all keep the window open
    raw_r_tick_ = lv_tick_get();
    if (item->key_state != KBD_KEY_PRESSED) return;
    request_scan();
    refresh();
}

void UIWifiSurveyPage::key_event_cb(lv_event_t *event)
{
    static_cast<UIWifiSurveyPage *>(lv_event_get_user_data(event))->handle_key(event);
}

void UIWifiSurveyPage::keyboard_event_cb(lv_event_t *event)
{
    static_cast<UIWifiSurveyPage *>(lv_event_get_user_data(event))->handle_raw_key(event);
}

void UIWifiSurveyPage::row_event_cb(lv_event_t *event)
{
    auto *self = static_cast<UIWifiSurveyPage *>(lv_event_get_user_data(event));
    const lv_event_code_t code = lv_event_get_code(event);
    lv_indev_t *indev = lv_event_get_indev(event);
    lv_point_t point = {0, 0};
    if (indev) lv_indev_get_point(indev, &point);
    if (code == LV_EVENT_PRESSED) {
        self->press_y_ = point.y;
        self->swiped_ = false;
        self->valid_press_ = lv_tick_elaps(self->start_tick_) >= kStartGuardMs;
    } else if (code == LV_EVENT_RELEASED) {
        const int dy = point.y - self->press_y_;
        if (std::abs(dy) >= 10) {   // a vertical drag scrolls the list
            self->swiped_ = true;
            const int rows = std::max(1, std::abs(dy) / kRowH);
            const int max_offset = std::max(0, static_cast<int>(self->aps_.size()) - kRows);
            self->offset_ = std::clamp(self->offset_ + (dy < 0 ? rows : -rows), 0, max_offset);
            self->refresh();
        }
    } else if (code == LV_EVENT_CLICKED) {
        if (self->swiped_ || !self->valid_press_) {
            self->swiped_ = false;
            return;
        }
        const int row = static_cast<int>(reinterpret_cast<intptr_t>(lv_obj_get_user_data(lv_event_get_target_obj(event))));
        const int index = self->offset_ + row;
        if (index >= static_cast<int>(self->aps_.size())) return;
        self->selected_ = index;
        self->selected_bssid_ = self->aps_[static_cast<size_t>(index)].bssid;
        self->activate();
        self->refresh();
    }
}

void UIWifiSurveyPage::tab_event_cb(lv_event_t *event)
{
    auto *self = static_cast<UIWifiSurveyPage *>(lv_event_get_user_data(event));
    lv_obj_t *target = lv_event_get_target_obj(event);
    if (target == self->tab_net_) self->set_view(View::Networks);
    else if (target == self->tab_ch_) self->set_view(View::Channels);
    else if (target == self->status_) {
        self->request_scan();
        self->refresh();
    }
}

void UIWifiSurveyPage::timer_cb(lv_timer_t *timer)
{
    static_cast<UIWifiSurveyPage *>(lv_timer_get_user_data(timer))->poll();
}
