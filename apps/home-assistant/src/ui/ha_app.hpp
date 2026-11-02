/*
 * SPDX-License-Identifier: MIT
 *
 * Home Assistant app: the pages (design 052a phase 1, study 053 P1: the tile dashboard, P2: the owner's stored
 * dashboard drawn natively). LVGL thread only.
 *
 *   Connect    (no sign-in yet, or after Sign out) "Scan with your phone to connect to Home Assistant", the QR code
 *              (left in landscape, on top in portrait), the address line, the Server address row (opens its field:
 *              the fallback keyboard only when no keyboard is connected, the shared rule), the status line ("Waiting
 *              for the phone...", "Connected as <user>", or which step failed). Esc quits.
 *   Dashboard  under the shared header, one 7 mm tab bar: the stored dashboard's views first (0.3.0: their titles, or
 *              their icons when a title does not fit), then Favourites, one tab per Home Assistant area, All, scrolling
 *              sideways; the connection at its right end as a dot and a word. Favourites and the areas are grids of
 *              tiles (core/tiles.hpp: columns from the width in mm, one 7 mm row each): the left 7 x 7 mm square is the
 *              icon (an MDI glyph in a circle tinted with the state colour when active) and does the tile's action
 *              (toggle, run, lock / unlock and garage-type covers after a confirmation); the name and state word open
 *              the entity page (the small chevron says so). A dimmable light gets a full-width slider row under its
 *              tile. All is 0.1.0's list (favourites, rooms, Settings). A view of the stored dashboard (ha_dash.cpp)
 *              is its cards in masonry columns (clamp(floor(width_mm / 50), 1, 4), each card into the shortest
 *              column): entities (7 mm rows: icon, name, state word or a switch; sections, dividers, other row types
 *              greyed; a header toggle for lights, switches, fans and input booleans), sensor (the value and a line of
 *              its history), weather-forecast (now and five days), any other card greyed: "Not supported on this
 *              screen". A row's icon or switch acts, its name opens the entity page (or follows its tap_action).
 *   Room       (from All) the room's entities, a keyboard filter.
 *   Entity     (study 053 section 7.6) the tile's icon, word and "room - changed HH:MM" at the top, then 7 mm rows
 *              straight under it, the toolkit's list look (cp0_ui_list.h: highlight bar, action dots, grey values):
 *              read-only attributes, the brightness slider row (Left / Right 10 %, a drag), the action row (Turn on /
 *              off, Activate, Run, Lock / Unlock, a cover's Open and Close), the favourite row. Up / Down move, Enter
 *              (or Right) acts, Esc (or Left) goes back; a tap selects a row, a tap on the selected row acts.
 *   Settings   Server address and Signed in as (read only), Dashboard (0.3.0: Automatic, a dashboard, Off; Enter
 *              moves to the next), the keys of the tiles, Sign out (asks first).
 *
 * Keys on the dashboard: arrows move a 2-D focus (Up / Down by row, Left / Right by column; Up from the first row to
 * the tab bar, where Left / Right switch views), Enter does the tile's action, Ctrl+O opens its page, Page Up / Down
 * switch views anywhere, Esc quits. Left / Right on a slider row step it by 10 %. The focus (the launcher's highlight
 * colour and an outline) shows from the first key press; a tap hides it again (touch-only boards show no focus).
 * Touch: a tap on the icon acts, on the name opens the page, a drag scrolls, a sideways swipe switches views.
 *
 * Data comes from a Backend (the connection thread, or the simulated Home Assistant with HA_DEMO=<world> in the render
 * harness), polled every 200 ms; the pages are refreshed at most twice a second, a tile only when what it shows changed.
 */
#pragma once

#include "view.hpp"

#include "cp0_ui_widgets.hpp"
#include "lvgl/lvgl.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

struct key_item;

namespace haui {

/* = X-MinSize / X-Portrait of root/usr/share/APPLaunch/applications/home-assistant.desktop (tests/test_package.cpp) */
constexpr int kMinW = 480, kMinH = 320;
constexpr int kPortraitW = 480, kPortraitH = 640;
constexpr const char *kVersion = "0.3.0";

class HaApp {
public:
    explicit HaApp(lv_obj_t *screen);
    ~HaApp();
    HaApp(const HaApp &) = delete;
    HaApp &operator=(const HaApp &) = delete;

private:
    enum class Screen { None, Connect, ServerEdit, Dashboard, Room, Entity, Settings };
    struct Target {
        enum class Kind { None, Entity, Room, Settings, ServerAddress, SignOut, Dashboard };
        Kind kind = Kind::None;
        std::string id;
        ha::RowAction action = ha::RowAction::Info;
    };
    /* a row of the entity page */
    struct DetailRow {
        enum class Kind { Info, Slider, Action };
        enum class Does { None, Brightness, Act, Open, Close, Favourite };
        Kind kind = Kind::Info;
        Does does = Does::None;
        std::string label, value;
        bool running = false;  /* Action: the dot in the accent blue (a command waits) */
        int slider_value = -1; /* Slider: 0..100 shown */
        lv_obj_t *row = nullptr, *label_obj = nullptr, *value_obj = nullptr, *dot = nullptr;
        lv_obj_t *track = nullptr, *fill = nullptr, *knob = nullptr;
        int x0 = 0, w = 0; /* Slider: the track in the row */
        const lv_font_t *font = nullptr, *bold = nullptr; /* the label's faces (bold: selected) */
    };

    /* one tile of the grid, and its slider row when it has one */
    struct TileUi {
        std::string id;
        lv_obj_t *tile = nullptr, *icon = nullptr, *circle = nullptr, *glyph = nullptr;
        lv_obj_t *text = nullptr, *name = nullptr, *word = nullptr, *chevron = nullptr;
        ha::EntityRow shown;
        bool drawn = false;
        int slider = -1; /* index in sliders_ */
    };
    struct SliderUi {
        std::string id;
        int tile = -1;
        lv_obj_t *row = nullptr, *name = nullptr, *track = nullptr, *fill = nullptr, *knob = nullptr, *pct = nullptr;
        int x0 = 0, w = 0; /* the track in the row */
        int value = -1;    /* shown */
        bool on = false;
    };
    enum class Zone { Bar, Grid };

    /* the stored dashboard's view (ha_dash.cpp): its cards, their rows, the touch zones and the keyboard's stops */
    struct DashRowUi {
        int card = -1;
        lv_obj_t *box = nullptr, *glyph = nullptr, *name = nullptr, *value = nullptr, *sw = nullptr;
        int name_x = 0, name_right = 0; /* the name's left and the value's right edge, in their parent */
        ha::DashRowView shown;
        bool drawn = false;
    };
    struct DashCardUi {
        lv_obj_t *box = nullptr, *title = nullptr, *hdr = nullptr, *hdr_sw = nullptr, *hdr_value = nullptr;
        lv_obj_t *glyph = nullptr, *name = nullptr, *value = nullptr, *unit = nullptr, *cond = nullptr, *chart = nullptr;
        lv_chart_series_t *series = nullptr;
        std::vector<lv_obj_t *> days; /* per forecast day: its name, glyph, high, low */
        int first_row = 0, rows = 0;  /* its rows in dash_rows_ */
        int col = 0, x = 0, y = 0, w = 0, h = 0;
        int hdr_key = -1; /* the header switch as drawn: on x 2 + grey */
        ha::DashCardView shown;
        bool drawn = false;
    };
    enum class DashZone { Icon, Name, Switch, Card, Header };
    struct DashStop {   /* a stop of the keyboard's focus */
        int card = -1;
        int row = -1;   /* its index in dash_rows_; -1: the card's header toggle; -2: the card itself (a sensor) */
        lv_obj_t *obj = nullptr;
    };

    /* pages */
    void show(Screen s);
    void clear_page();
    void build_list(bool caption_room);
    void build_connect();
    void build_server_edit();
    void build_entity();
    void set_title();

    /* the dashboard */
    void build_dashboard();
    void build_tab_bar();
    void build_content();
    void clear_content();
    void refresh_dashboard();
    void refresh_bar();  /* the tabs again when the areas changed, then style_bar() */
    void style_bar();    /* the active tab, the bar's focus, the connection word */
    int m_radius() const;
    void refresh_grid(const std::vector<ha::EntityRow> &rows, bool loading);
    void build_grid(const std::vector<ha::EntityRow> &rows);
    void draw_tile(TileUi &t, const ha::EntityRow &row, bool force);
    void draw_slider(SliderUi &s, int value, bool on, bool force);
    void style_focus();
    void focus_tile_into_view();
    void switch_tab(int index);
    const ha::Tab *current_tab() const;
    const std::vector<ha::EntityRow> *grid_rows_source(bool &loading) const;
    void dashboard_key(ha::NavKey k);
    void act_on_tile(const ha::EntityRow &row);
    void slider_step(int slider, int delta);
    void slider_set(int slider, int value);
    void slider_pointer(int slider, lv_event_code_t code);
    bool swipe(int dx, int dy);
    void show_toast(const std::string &text, bool error);
    static void tab_event_cb(lv_event_t *e);
    static void zone_event_cb(lv_event_t *e);
    static void slider_event_cb(lv_event_t *e);
    static void grid_event_cb(lv_event_t *e);

    /* the stored dashboard's view (ha_dash.cpp) */
    bool on_dash_tab() const;
    void refresh_dash_view();
    void build_dash_view();
    void clear_dash();
    int build_entities_card(DashCardUi &c, const ha::DashCardView &v, int card);
    int build_sensor_card(DashCardUi &c, const ha::DashCardView &v, int card);
    int build_weather_card(DashCardUi &c, const ha::DashCardView &v);
    int build_unsupported_card(DashCardUi &c, const ha::DashCardView &v);
    lv_obj_t *dash_zone(lv_obj_t *parent, int x, int y, int w, int h, int card, int row, DashZone zone);
    lv_obj_t *dash_switch(lv_obj_t *parent, int zone_x, int zone_w, int row_h);
    void draw_dash_row(DashRowUi &r, const ha::DashRowView &want, bool force);
    void draw_dash_card(DashCardUi &c, const ha::DashCardView &want, bool force);
    void draw_switch(lv_obj_t *sw, bool on, bool grey);
    void dash_tap(int card, int row, DashZone zone);
    void act_on_dash_row(const ha::DashRowView &r);
    void dash_header(int card);
    void dash_key(ha::NavKey k);
    void style_dash_focus();
    static void dash_event_cb(lv_event_t *e);
    std::string dash_setting_value();

    /* data */
    void tick();
    void refresh(bool force);
    void refresh_connect();
    void refresh_list();
    void refresh_entity(bool rebuild);
    void refresh_hero();
    void build_detail_rows(const std::vector<DetailRow> &rows);
    void draw_detail_row(DetailRow &r, const DetailRow &want, bool force);
    void draw_detail_slider(DetailRow &r, int value);
    void select_detail(int index, bool scroll);
    void detail_key(std::uint32_t lv_key);
    void detail_activate(int index, int tap_x);
    void detail_pointer(int index, lv_event_code_t code);
    int detail_step(int from, int delta) const;
    static void detail_event_cb(lv_event_t *e);
    void update_caption();
    std::vector<cp0::ui::ListRow> home_rows();
    std::vector<cp0::ui::ListRow> room_rows();
    std::vector<cp0::ui::ListRow> settings_rows();
    cp0::ui::ListRow entity_row(const ha::EntityRow &e);
    std::intptr_t add_target(Target t);
    void render_qr(const std::string &url, int box);
    void submit_server(const std::string &text);

    /* input */
    static void keyboard_cb(lv_event_t *event);
    void on_key(const key_item *item);
    void forward_key(std::uint32_t lv_key);
    void list_activate(std::intptr_t id, bool by_key, std::uint32_t key, int x);
    void act_on_entity(const ha::EntityRow &row);
    void back();
    void open_entity(const std::string &id);
    void confirm(const std::string &question, std::function<void()> yes);
    void close_dialog();
    void post(std::function<void()> fn);
    static void posted_cb(void *data);
    void focus(lv_obj_t *obj);
    const ha::EntityRow *find_row(const std::string &id) const;
    void send_brightness(const std::string &id, int value);

    lv_obj_t *screen_ = nullptr;
    cp0_ui_roller_page_t layout_{};
    std::unique_ptr<cp0::ui::Page> page_;
    std::unique_ptr<cp0::ui::CompactList> list_;
    std::unique_ptr<cp0::ui::Dialog> dialog_;
    std::unique_ptr<cp0::ui::TextField> field_;
    std::unique_ptr<ha::Backend> backend_;

    /* the Connect page */
    lv_obj_t *connect_ = nullptr;
    lv_obj_t *qr_box_ = nullptr;
    lv_obj_t *qr_img_ = nullptr;
    lv_obj_t *connect_text_ = nullptr, *connect_addr_ = nullptr, *connect_status_ = nullptr;
    std::vector<std::uint8_t> qr_pixels_;
    lv_image_dsc_t qr_dsc_{};
    std::string qr_shown_;
    int qr_box_px_ = 0;
    std::uint32_t connected_at_ = 0; /* when the Connect page saw the sign-in finish */
    std::uint32_t named_at_ = 0;     /* when it showed "Connected as <user>" */
    lv_obj_t *field_hint_ = nullptr; /* the line under the server address field */

    /* the dashboard */
    lv_obj_t *bar_ = nullptr, *tabs_box_ = nullptr, *conn_dot_ = nullptr, *conn_word_ = nullptr;
    std::vector<lv_obj_t *> tab_btns_;
    std::vector<ha::Tab> tabs_;
    std::string tab_key_;          /* "" until the first tab is shown: the first tab */
    std::vector<bool> tab_icon_;   /* the tab shows its view's icon (its title does not fit) */
    std::string bar_conn_;
    lv_obj_t *grid_ = nullptr, *grid_note_ = nullptr;
    std::vector<TileUi> tiles_;
    std::vector<SliderUi> sliders_;
    std::vector<ha::GridRow> rows_;
    std::vector<std::string> grid_ids_;
    std::vector<bool> grid_dims_;
    int grid_cols_ = 2;
    ha::Focus focus_;
    bool focus_visible_ = false;
    Zone zone_ = Zone::Grid;
    int focus_drawn_ = -1, slider_focus_drawn_ = -1;
    bool bar_focus_drawn_ = false;
    int bar_styled_tab_ = -1;
    std::string hero_shown_; /* what the entity page's top shows (redrawn only when it changes) */
    lv_point_t press_at_{};
    int content_y_ = 0;
    lv_obj_t *toast_ = nullptr;
    std::uint32_t toast_until_ = 0;
    /* the stored dashboard's view */
    std::vector<DashCardUi> dcards_;
    std::vector<DashRowUi> dash_rows_;
    std::vector<DashStop> dstops_;
    int dstop_ = 0, dstop_drawn_ = -1;
    std::string dash_sig_;
    int dash_shown_ = -1;     /* the dashboard view under the entity page (kept subscribed) */
    bool tab_chosen_ = false; /* the user moved to a tab: the dashboard's first view no longer takes over */
    const lv_font_t *big_font_ = nullptr;
    int cpad_ = 0, sw_h_ = 0, sw_w_ = 0;
    /* fonts and sizes of the tiles, from the layout service (built once per page) */
    const lv_font_t *name_font_ = nullptr, *word_font_ = nullptr, *tab_font_ = nullptr, *tab_bold_ = nullptr;
    const lv_font_t *glyph_font_ = nullptr, *hero_glyph_font_ = nullptr;
    int row_h_ = 0, gap_ = 0, margin_ = 0, tile_w_ = 0, circle_ = 0;
    std::string chevron_text_;

    /* the entity page: its top, then its rows (the list's layout and faces) */
    lv_obj_t *hero_ = nullptr, *hero_circle_ = nullptr, *hero_glyph_ = nullptr, *hero_word_ = nullptr,
             *hero_meta_ = nullptr;
    lv_obj_t *dbox_ = nullptr;
    std::vector<DetailRow> drows_;
    int dsel_ = -1;
    cp0_ui_list_layout_t dl_{};
    const lv_font_t *dlabel_font_ = nullptr, *dlabel_bold_ = nullptr, *dvalue_font_ = nullptr;

    Screen screen_kind_ = Screen::None;
    Screen entity_from_ = Screen::Dashboard;
    lv_obj_t *focus_obj_ = nullptr;
    int list_x_ = 0, list_w_ = 0;
    std::vector<Target> targets_;
    std::string room_id_;
    bool room_shown_ = false;
    std::string entity_id_;
    std::string filter_;

    ha::View view_;
    std::uint64_t view_version_ = 0;
    std::uint32_t last_rebuild_ = 0;
    bool dirty_ = false;
    std::uint64_t message_seen_ = 0;
    std::uint32_t message_until_ = 0;
    std::string message_;
    bool message_error_ = false;

    /* the brightness sliders (entity page and tiles): the last value set, debounced */
    lv_timer_t *slide_timer_ = nullptr;
    std::string slide_id_;
    int slide_value_ = -1;
    std::uint32_t slid_at_ = 0;
    bool dragging_ = false;

    lv_timer_t *timer_ = nullptr;
    int saved_intercept_ = 0;
    bool key_activation_ = false;
    std::uint32_t last_key_ = 0;
    std::vector<std::function<void()>> posted_;
    bool post_armed_ = false;
};

} // namespace haui
