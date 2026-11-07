/*
 * SPDX-License-Identifier: MIT
 *
 * Deck Atlas: the screen. The launcher's shared page header (cp0::ui::Page: Esc, title, status strip) over three tabs
 * switched by a tab strip at the bottom (the mmWave Radar pattern):
 *   Map       the engine's map widget (cp0_map_widget.h) over the whole area: drag and arrow keys pan, its + / - pad,
 *             the + and - keys and a double tap zoom, its scale bar and attribution in the corners, its state strip
 *             for online sources (offline, needs a key, cap reached...); "No map here" when no imported file covers
 *             the view. The view (centre, zoom) is remembered in settings.conf.
 *   Sources   "Map shows" (all map files, one file, or an online source), the imported map files (a row opens its
 *             page: path, size, zooms, tiles, bounds, Show on map, Remove), "Import a map" (the file browser, then the
 *             import page with its progress bar, rate and Cancel), the three online sources of the engine (a tap on
 *             the row switches it: the key page first for a keyed source without key, the terms dialog the first
 *             time) and their key rows.
 *   Settings  Units, Display scale, Cache size, Offline mode, Clear saved tiles (asks first), the saved tiles' usage,
 *             About (version, the map data attributions, credits).
 * Sub-pages (a file's page, the browser, the import, a key, the "Map shows" choice, About) take the whole body; Esc or
 * the header's Esc goes back; at a tab a short Esc ends the app. Tab / Shift+Tab cycle the tabs.
 *
 * Threads: everything slow runs on one worker (worker.hpp); the map widget has its own tile thread. LVGL thread only.
 */
#pragma once

#include "attribution.hpp"
#include "follow.hpp"
#include "gps_link.hpp"
#include "position_state.hpp"
#include "geo.hpp"
#include "import_model.hpp"
#include "settings.hpp"
#include "source_model.hpp"
#include "browser.hpp"
#include "worker.hpp"

#include "cp0_map.h"
#include "cp0_ui_roller.h"
#include "cp0_ui_widgets.hpp"
#include "lvgl/lvgl.h"

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

struct key_item;

namespace atlasui {

/* = X-MinSize / X-Portrait of root/usr/share/APPLaunch/applications/deck-atlas.desktop (tests/test_package.cpp) */
constexpr int kMinW = 480, kMinH = 320;
constexpr int kPortraitW = 320, kPortraitH = 480;
constexpr const char *kVersion = "0.2.0";

enum class Tab { Map = 0, Sources, Settings };
enum class Sub { None, FileDetail, Browser, Import, Key, Shown, About };

/* One imported archive as the pages show it (the engine's listing). */
struct ArchiveItem {
    std::string id, title, path, attribution, imported, source_path;
    int state = 0;
    int mode = 0;               /* cp0_map_import_mode_t */
    int minzoom = -1, maxzoom = -1, tile_px = 0;
    bool has_bounds = false;
    atlas::Box bounds;
    std::uint64_t bytes = 0, tiles = 0;
    bool has_zoom_extents = false; /* the record keeps every zoom's tile rectangle: the bounds are the union of all levels */
};

/* One online source. */
struct OnlineItem {
    atlas::SourceFacts facts;
    std::string licence, note, attribution;
    bool shipped = false;
};

class AtlasApp {
public:
    explicit AtlasApp(lv_obj_t *screen);
    ~AtlasApp();
    AtlasApp(const AtlasApp &) = delete;
    AtlasApp &operator=(const AtlasApp &) = delete;

private:
    /* ---- shell (atlas_app.cpp) */
    void compute_sizes();
    void open_store();
    void build_tabs();
    void build_map();
    void set_tab(Tab t);
    void update_tabs();
    void show_tabs(bool shown);
    void back();
    void close_sub();
    void focus(lv_obj_t *obj);
    void post(std::function<void()> fn);
    static void posted_cb(void *data);
    static void timer_cb(lv_timer_t *t);
    void tick();
    void save_settings();
    void set_title();
    void make_list(bool with_tabs, cp0::ui::CompactListEvents ev);
    void clear_page_objects();
    lv_obj_t *make_label(lv_obj_t *parent, int px, std::uint32_t rgb, bool wrap);
    lv_obj_t *make_button(lv_obj_t *parent, const char *text, lv_event_cb_t cb);
    void confirm(const std::string &title, const std::string &text, const std::string &yes, std::function<void()> on_yes);
    void message(const std::string &title, const std::string &text);
    void close_dialog();
    static void tab_cb(lv_event_t *e);
    static void keyboard_cb(lv_event_t *e);
    static void map_key_cb(lv_event_t *e);
    void on_keyboard(const key_item &item);
    static void log_cb(void *user, const char *line);

    /* ---- map (atlas_app.cpp) */
    void apply_map_source();
    void first_view();
    void fit_archive(const ArchiveItem &a);
    static void view_cb(void *user, double lat, double lon, int zoom);
    void update_chip();
    void build_gps_layer();      /* the position dot's layer, over the map (atlas_gps.cpp) */
    void build_gps_controls();   /* the status chip and the follow button */
    void update_attribution();   /* the plate over a credit the engine would cut (core/attribution.hpp) */
    std::string engine_attribution() const;
    void show_on_map(const std::string &id);
    void store_changed();     /* an archive came or went: the widget's limits, attribution and "*" tiles refreshed */

    /* ---- GPS (atlas_gps.cpp) */
    void gps_start();
    void gps_stop();
    void gps_apply_settings();             /* the Settings rows changed: configure the reader, show or hide the overlays */
    void gps_tick(std::int64_t now);
    void gps_refresh_ports();
    void gps_layout();                     /* the chip and the follow button, laid out from the engine's controls */
    void gps_show_overlays();              /* chip text and colour, button visibility, a redraw of the dot */
    void set_follow(bool on);
    void on_follow_off();
    void recentre_on(double lat, double lon);
    bool gps_key(std::uint32_t key_code, char ch);
    void draw_gps(lv_layer_t *layer, const lv_area_t &area);
    void draw_follow_button(lv_layer_t *layer, const lv_area_t &area);
    static void gps_layer_cb(lv_event_t *e);
    static void follow_draw_cb(lv_event_t *e);
    static void follow_click_cb(lv_event_t *e);
    atlas::AttrInput overlay_input() const;

    /* ---- store data (atlas_sources.cpp) */
    void refresh_store(std::function<void()> then = nullptr);
    const ArchiveItem *find_archive(const std::string &id) const;
    const OnlineItem *find_online(const std::string &id) const;
    std::string shown_title() const;
    std::string active_attribution() const;

    /* ---- Sources tab and its pages (atlas_sources.cpp) */
    void build_sources();
    std::vector<cp0::ui::ListRow> sources_rows() const;
    void sources_activate(std::intptr_t id);
    void open_file_detail(const std::string &id);
    std::vector<cp0::ui::ListRow> detail_rows() const;
    void detail_activate(std::intptr_t id);
    void confirm_remove(const std::string &id);
    void open_shown();
    void shown_activate(std::intptr_t id);
    void set_shown(const std::string &id, bool fit);
    void toggle_source(const std::string &id);
    void ask_terms(const std::string &id);
    void enable_source(const std::string &id, bool on);
    void open_key(const std::string &id, bool then_enable);
    void submit_key(const std::string &text);
    void remove_key();
    void set_key_status(const std::string &text, bool error);
    void refresh_sources_rows();

    /* ---- import (atlas_import.cpp) */
    void open_browser(const std::string &path); /* "" = the places to start from */
    void browser_back();
    void browser_loaded(const std::string &path, std::vector<atlas::BrowserRow> rows, const std::string &error);
    void browser_activate(std::intptr_t id);
    void confirm_import(const atlas::BrowserRow &row);
    void start_import(const std::string &path, atlas::ImportKind kind);
    void build_import_page();
    void update_import_page(bool force);
    void finish_import(int rc, const std::string &id, const std::string &reason);
    static void import_progress_cb(void *user, const cp0_map_progress_t *p);
    static void import_cancel_cb(lv_event_t *e);
    static void import_show_cb(lv_event_t *e);
    static void import_done_cb(lv_event_t *e);

    /* ---- Settings tab (atlas_settings.cpp) */
    void build_settings();
    std::vector<cp0::ui::ListRow> settings_rows() const;
    void settings_activate(std::intptr_t id);
    void refresh_stats();
    void confirm_clear();
    void open_about();
    std::string about_text() const;

    lv_obj_t *screen_ = nullptr;
    cp0_ui_roller_page_t layout_{};
    std::unique_ptr<cp0::ui::Page> page_;
    std::unique_ptr<cp0::ui::CompactList> list_;
    std::unique_ptr<cp0::ui::Dialog> dialog_;
    std::unique_ptr<cp0::ui::TextField> field_;
    std::unique_ptr<cp0::ui::TextBlock> text_;
    std::unique_ptr<Worker> worker_;

    /* sizes from the toolkit's tokens */
    int w_ = 0, h_ = 0, body_y_ = 0, view_h_ = 0, tab_h_ = 0;
    int space_s_ = 8, space_m_ = 16, row_ = 40, target_ = 44, body_px_ = 18, caption_px_ = 14;
    bool narrow_ = false;

    lv_obj_t *map_box_ = nullptr, *map_ = nullptr, *chip_ = nullptr, *chip_label_ = nullptr;
    lv_obj_t *attr_plate_ = nullptr, *attr_label_ = nullptr;
    std::string attr_key_;
    int attr_row_top_ = -1;      /* atlas::AttrPlan::row_top of the credit now shown */

    /* GPS: the reader (a fake port in demonstration mode), position.state, follow mode, what the overlays show now */
    std::unique_ptr<atlas::GpsPortSys> gps_ports_;
    std::unique_ptr<atlas::GpsLink> gps_;
    std::unique_ptr<atlas::PositionStateWriter> pos_writer_;
    atlas::FollowLogic follow_;
    atlas::GpsStatus gps_status_;
    std::string gps_chip_text_, gps_source_;
    std::vector<std::string> gps_present_;   /* the serial ports present, for the Port row */
    lv_obj_t *gps_layer_ = nullptr, *gps_chip_ = nullptr, *gps_chip_label_ = nullptr, *follow_btn_ = nullptr;
    std::int64_t gps_ports_at_ = 0;
    lv_obj_t *tabbar_ = nullptr;
    struct Cell {
        lv_obj_t *obj = nullptr, *label = nullptr, *mark = nullptr;
    } tabs_[3];
    lv_obj_t *page_box_ = nullptr;   /* a sub-page's own objects (key page, import page) */
    lv_obj_t *key_status_ = nullptr;
    lv_obj_t *imp_title_ = nullptr, *imp_bar_ = nullptr, *imp_count_ = nullptr, *imp_rate_ = nullptr,
             *imp_result_ = nullptr, *imp_note_ = nullptr, *imp_cancel_ = nullptr, *imp_show_ = nullptr,
             *imp_done_ = nullptr;

    Tab tab_ = Tab::Map;
    Sub sub_ = Sub::None;
    lv_timer_t *timer_ = nullptr;
    std::vector<std::function<void()>> posted_;
    bool post_armed_ = false;

    /* the store and what it holds (refreshed by the worker) */
    cp0_map_store_t *store_ = nullptr;
    std::string store_dir_, store_error_;
    std::vector<ArchiveItem> archives_;
    std::vector<OnlineItem> online_;
    bool refreshing_ = false, refresh_again_ = false;
    std::vector<std::function<void()>> refresh_thens_; /* closures waiting for the next listing */
    std::int64_t last_refresh_ = 0;
    bool listed_once_ = false;

    /* the settings */
    atlas::Settings settings_;
    std::string config_dir_;
    bool demo_ = false;
    std::string demo_mode_;

    /* the map's view as last seen (polled: the widget has no view-change event) */
    bool view_dirty_ = false;
    std::int64_t view_saved_at_ = 0;
    std::string chip_shown_;
    bool widget_by_id_ = false;   /* the widget reads one archive or source by its id (not the every-archive lookup) */

    /* the sub-pages' state */
    std::string detail_id_;
    std::string key_id_;
    bool key_then_enable_ = false;
    std::string browser_path_;                 /* "" = the places to start from */
    std::vector<std::string> root_paths_;      /* those places' folders (Esc there goes back to the list of them) */
    std::vector<atlas::BrowserRow> browser_rows_;
    int browser_req_ = 0;                      /* the listing asked last (an older answer is dropped) */
    std::vector<std::string> shown_ids_;
    std::string home_, user_;
    int sources_sel_ = -1, settings_sel_ = -1;

    /* the import */
    atlas::ImportProgress import_;
    bool importing_ = false;
    volatile int import_cancel_ = 0;
    std::string import_id_;
    std::mutex prog_mu_;
    struct Prog {
        int phase = 0;
        std::uint64_t done = 0, total = 0, skipped = 0, resumed = 0;
        bool fresh = false;
    } prog_;
    std::string imp_shown_;
    std::int64_t imp_updated_ = 0;

    /* the cache statistics (Settings) */
    bool stats_known_ = false, stats_busy_ = false, clearing_ = false;
    std::uint64_t stats_disk_ = 0, stats_cap_ = 0, stats_tiles_ = 0;
    std::int64_t last_stats_ = 0;
};

} // namespace atlasui
