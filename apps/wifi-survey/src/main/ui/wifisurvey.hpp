/*
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "esc_policy.hpp"
#include "ui_app_page.hpp"
#include "wifiscan.hpp"

#include <array>
#include <string>
#include <vector>

/* True once the page asked to quit (never from a short Esc now; kept for main()'s should_quit hook). */
bool wifisurvey_quit_requested();

// Lists the Wi-Fi networks around the deck (Networks), shows how crowded each channel is (Channels) and the
// details of one network (Detail). Passive: it only reads NetworkManager's scan results.
class UIWifiSurveyPage : public AppPage
{
public:
    UIWifiSurveyPage();
    ~UIWifiSurveyPage() override;

private:
    using View = wifisurvey::View;
    static constexpr uint32_t kHintMs = 2500;
    bool hint_on_ = false;
    uint32_t hint_tick_ = 0;   // lv_tick_get() when the Esc hint was shown
    void show_footer();
    static constexpr int kRows = 6;
    static constexpr int kSlots = 13;        // channel columns per band

    void build_networks();
    void build_channels();
    void build_detail();
    void set_view(View view);
    void toggle_tab(int delta);
    void request_scan();
    void poll();
    void apply_snapshot();
    void move_selection(int delta);
    void activate();
    void back();
    void refresh();
    void render_networks();
    void render_channels();
    void render_detail();
    void select_bssid(const std::string &bssid);
    int selected_index() const;
    std::string message() const;
    void handle_key(lv_event_t *event);
    void handle_raw_key(lv_event_t *event);

    static void key_event_cb(lv_event_t *event);
    static void keyboard_event_cb(lv_event_t *event);
    static void row_event_cb(lv_event_t *event);
    static void tab_event_cb(lv_event_t *event);
    static void timer_cb(lv_timer_t *timer);

    wifisurvey::Scanner scanner_;
    wifisurvey::Snapshot snap_;
    std::vector<wifisurvey::AccessPoint> aps_;   // sorted by signal
    wifisurvey::AccessPoint detail_ap_;          // last known data of the network shown in Detail
    bool detail_present_ = false;
    View view_ = View::Networks;
    View tab_ = View::Networks;                  // screen to return to from Detail
    std::string selected_bssid_;
    int selected_ = 0;
    int offset_ = 0;
    uint32_t last_scan_start_ = 0;               // lv_tick_get() when the last scan was requested
    uint32_t raw_r_tick_ = 0;                    // an evdev "R" press was seen (see handle_raw_key)
    bool raw_r_ = false;
    int press_y_ = 0;
    bool swiped_ = false;
    bool valid_press_ = false;                   // the current touch started after the start-up guard
    uint32_t start_tick_ = 0;                    // lv_tick_get() at construction

    lv_obj_t *root_ = nullptr;
    lv_obj_t *tab_net_ = nullptr;
    lv_obj_t *tab_ch_ = nullptr;
    lv_obj_t *tab_net_label_ = nullptr;
    lv_obj_t *tab_ch_label_ = nullptr;
    lv_obj_t *title_ = nullptr;                  // Detail: network name in the header
    lv_obj_t *status_ = nullptr;
    lv_obj_t *footer_ = nullptr;
    lv_obj_t *msg_ = nullptr;

    lv_obj_t *net_box_ = nullptr;
    std::array<lv_obj_t *, kRows> rows_{};
    std::array<lv_obj_t *, kRows> row_mark_{};
    std::array<lv_obj_t *, kRows> row_ssid_{};
    std::array<lv_obj_t *, kRows> row_track_{};
    std::array<lv_obj_t *, kRows> row_fill_{};
    std::array<lv_obj_t *, kRows> row_dbm_{};
    std::array<lv_obj_t *, kRows> row_chan_{};
    std::array<lv_obj_t *, kRows> row_band_{};
    std::array<lv_obj_t *, kRows> row_sec_{};

    lv_obj_t *ch_box_ = nullptr;
    lv_obj_t *ch_title_[2] = {};
    std::array<lv_obj_t *, kSlots> bar24_{};
    std::array<lv_obj_t *, kSlots> cnt24_{};
    std::array<lv_obj_t *, kSlots> lab24_{};
    std::array<lv_obj_t *, kSlots> bar5_{};
    std::array<lv_obj_t *, kSlots> lab5_{};

    lv_obj_t *det_box_ = nullptr;
    static constexpr int kDetailRows = 7;
    std::array<lv_obj_t *, kDetailRows> det_key_{};
    std::array<lv_obj_t *, kDetailRows> det_val_{};

    lv_event_dsc_t *keyboard_dsc_ = nullptr;
    lv_obj_t *keyboard_root_ = nullptr;
    lv_timer_t *timer_ = nullptr;
};
