/*
 * SPDX-License-Identifier: MIT
 *
 * mmWave Radar: the screen. The launcher's shared page header (cp0::ui::Page: Esc, title, status strip) over four pages
 * switched by a tab strip at the bottom: Radar (a top-down plot of up to three targets, or the "not found" help),
 * Table (the numbers of each target), Graph (distance and speed over the last 30 s) and Sensor (the connection, what
 * the scan found, the settings). The link to the sensor (core/link.hpp) is polled by an LVGL timer every 30 ms and
 * never blocks; the plot, the table and the graph are drawn by draw events (no widget per point), redrawn at most
 * 20 times a second and only when something changed.
 *
 * Keys: Left / Right or 1-4 switch pages, Space pauses (the display freezes, the link keeps reading), R rescans,
 * + / - change the plot range, Up / Down / Enter pick and change a setting on the Sensor page, Esc ends the app. Touch:
 * the tabs, the pause cell at the right end of the tab strip, the Rescan buttons, the setting rows. LVGL thread only.
 */
#pragma once

#include "link.hpp"
#include "settings.hpp"
#include "view.hpp"

#include "cp0_ui_roller.h"
#include "cp0_ui_widgets.hpp"
#include "lvgl/lvgl.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

struct key_item;

namespace mmwui {

constexpr int kMinW = 480, kMinH = 320;
constexpr int kPortraitW = 320, kPortraitH = 480;
constexpr const char *kVersion = "0.1.0";

enum class Tab { Radar = 0, Table, Graph, Sensor };

/* The sizes of this canvas: the toolkit's tokens, or on a narrow portrait canvas (320x480) the launcher's narrow
 * portrait numbers when the pinned toolkit does not have them yet (body 16 px, caption 14 px). */
struct Sizes {
    bool narrow = false;     /* narrow portrait: w < 480 and h > w */
    bool portrait = false;
    int body_px = 18, caption_px = 14;
    int space_s = 5, space_m = 10;
    int row = 40, target = 44;
    int tab_h = 44;
    int dot = 12;            /* a target's circle */
};

class RadarApp {
public:
    explicit RadarApp(lv_obj_t *screen);
    ~RadarApp();
    RadarApp(const RadarApp &) = delete;
    RadarApp &operator=(const RadarApp &) = delete;

private:
    struct Cell {
        lv_obj_t *obj = nullptr;
        lv_obj_t *label = nullptr;
        lv_obj_t *mark = nullptr;
    };
    struct InfoRow {
        lv_obj_t *row = nullptr, *key = nullptr, *value = nullptr;
        std::string shown;
    };
    struct SettingRow {
        lv_obj_t *row = nullptr, *key = nullptr, *value = nullptr, *chevron = nullptr;
    };

    /* building */
    void compute_sizes();
    void build_tabs();
    void build_view();
    void build_chips();
    void build_notfound();
    void build_sensor();
    lv_obj_t *make_label(lv_obj_t *parent, int px, std::uint32_t rgb, bool wrap);
    lv_obj_t *make_button(lv_obj_t *parent, const char *text);
    InfoRow make_info(lv_obj_t *parent, const char *key);
    lv_obj_t *make_heading(lv_obj_t *parent, const char *text);

    /* state */
    static void timer_cb(lv_timer_t *t);
    static void render_cb(lv_event_t *e);
    void tick();
    void set_tab(Tab t);
    void toggle_pause();
    void rescan();
    void change_range(int dir);
    void apply_setting(int index);
    void save_settings();
    void update_chips();
    void update_tabs();
    void update_notfound(bool force);
    void update_sensor(bool force);
    void update_view_visibility();
    void select_setting(int index);
    const mmw::SlotState &slot(int i) const;
    std::int64_t view_time() const;
    int effective_range() const;
    void set_info(InfoRow &r, const std::string &text, std::uint32_t rgb);

    /* drawing (radar_draw.cpp) */
    static void draw_cb(lv_event_t *e);
    void draw(lv_layer_t *layer);
    void draw_radar(lv_layer_t *layer, const lv_area_t &a);
    mmw::PlotGeom plot_geometry(bool local) const;
    void render_background();
    /* the part of the plot a target's drawing can cover (trail, circle, arrow, label): false when nothing is drawn */
    bool target_area(int i, const mmw::PlotGeom &g, lv_area_t &out) const;
    void invalidate_view();
    void draw_table(lv_layer_t *layer, const lv_area_t &a);
    void draw_graph(lv_layer_t *layer, const lv_area_t &a);
    const char *fmt(const char *format, ...);
    void text(lv_layer_t *layer, const char *s, int x, int y, int px, std::uint32_t rgb, lv_opa_t opa = LV_OPA_COVER);
    int text_w(const char *s, int px) const;
    void line(lv_layer_t *layer, int x1, int y1, int x2, int y2, int w, std::uint32_t rgb, lv_opa_t opa);
    void disc(lv_layer_t *layer, int cx, int cy, int r, std::uint32_t rgb, lv_opa_t opa);
    void rect(lv_layer_t *layer, int x, int y, int w, int h, std::uint32_t rgb, lv_opa_t opa, int radius = 0);

    /* input */
    static void key_cb(lv_event_t *e);
    static void keyboard_cb(lv_event_t *e);
    static void tab_cb(lv_event_t *e);
    static void pause_cb(lv_event_t *e);
    static void rescan_cb(lv_event_t *e);
    static void setting_cb(lv_event_t *e);
    void on_key(std::uint32_t key);
    void on_keyboard(const struct key_item &item);

    lv_obj_t *screen_ = nullptr;
    cp0_ui_roller_page_t page_layout_{};
    std::unique_ptr<cp0::ui::Page> page_;
    Sizes sz_;
    int body_y_ = 0, view_h_ = 0, w_ = 0, h_ = 0;

    lv_obj_t *surface_ = nullptr;   /* the body under the header: takes the keys */
    lv_obj_t *view_ = nullptr;      /* the plot / table / graph, drawn by draw_cb */
    lv_obj_t *bg_ = nullptr;        /* the radar's static background (a canvas under the view) */
    lv_draw_buf_t *bg_buf_ = nullptr;
    int bg_key_ = -1;
    lv_obj_t *tabbar_ = nullptr;
    Cell tabs_[4];
    Cell pause_;
    lv_obj_t *chip_ = nullptr, *chip_dot_ = nullptr, *chip_label_ = nullptr, *paused_chip_ = nullptr;
    std::string chip_shown_;

    lv_obj_t *nf_ = nullptr;        /* the "not found" help on the Radar page */
    lv_obj_t *nf_title_ = nullptr, *nf_status_ = nullptr, *nf_tried_ = nullptr, *nf_wiring_ = nullptr;
    lv_obj_t *nf_note_ = nullptr;
    std::string nf_shown_;

    int key_w_ = 100;               /* the key column of the Sensor page and the wiring table */
    lv_obj_t *sensor_ = nullptr;    /* the Sensor page: a scrolling column */
    InfoRow si_state_, si_port_, si_id_, si_baud_, si_fps_, si_good_, si_bad_, si_age_, si_model_;
    lv_obj_t *si_reports_ = nullptr;
    std::string si_reports_shown_;
    SettingRow settings_rows_[4];
    int setting_sel_ = -1;

    std::unique_ptr<mmw::Sys> sys_;
    std::unique_ptr<mmw::Link> link_;
    mmw::Settings settings_;
    std::string config_dir_;
    lv_timer_t *timer_ = nullptr;

    Tab tab_ = Tab::Radar;
    std::int64_t now_ = 0;          /* the time of the latest poll (monotonic ms) */
    bool paused_ = false;
    std::int64_t paused_at_ = 0;
    mmw::SlotState snap_[3];
    int auto_range_m_ = 4;

    /* change detection */
    std::uint32_t seen_frames_ = 0;
    mmw::LinkState seen_state_ = mmw::LinkState::Searching;
    std::int64_t seen_history_ = 0;
    unsigned seen_slots_ = 0;
    std::int64_t last_draw_ = 0, last_sensor_ = 0, last_nf_ = 0, last_chip_ = 0;
    bool dirty_ = true;
    bool full_redraw_ = true;       /* else only the targets' areas are redrawn */
    lv_area_t last_box_[3] = {};
    bool have_box_[3] = {};
    int label_w_max_ = 0;

    /* $MMWAVE_LOG=<file>: one diagnostic line a second (off when unset) */
    std::string diag_path_;
    std::int64_t diag_last_ = 0;
    unsigned diag_sent_ = 0;
    void diag_write(const std::string &text) const;

    /* $MMWAVE_TIMING=1: the render time of the screen, logged every 5 s (for the board test) */
    bool timing_ = false;
    std::int64_t render_start_ = 0, timing_since_ = 0, render_sum_ = 0, render_max_ = 0, last_render_ms_ = 0;
    int render_n_ = 0;

    /* text kept alive for the draw tasks of one frame (no allocation while drawing) */
    static constexpr int kTextSlots = 160;
    char texts_[kTextSlots][80];
    int text_n_ = 0;
};

} // namespace mmwui
