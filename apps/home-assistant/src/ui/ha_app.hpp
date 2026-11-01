/*
 * SPDX-License-Identifier: MIT
 *
 * Home Assistant app: the pages (design 052a, phase 1). LVGL thread only.
 *
 *   Connect   (no sign-in yet, or after Sign out) "Scan with your phone to connect to Home Assistant", the QR code
 *             (left in landscape, on top in portrait), the address line, the Server address row (opens its field:
 *             the fallback keyboard only when no keyboard is connected, the shared rule), the status line ("Waiting
 *             for the phone...", "Connected as <user>", or which step failed). Esc quits.
 *   Home      FAVOURITES (starred on the deck) then ROOMS (Home Assistant's areas, with their count), then Settings.
 *   Room      the room's entities. Enter or a tap on the label side acts (toggle, run, the lock's confirmation, the
 *             cover's page); Right or a tap on the value side opens the entity page. Typing filters the room (a
 *             physical keyboard only: no on-screen keyboard for the filter).
 *   Entity    the state and read-only attributes (Roller rows), the brightness slider of a dimmable light (Left / Right
 *             in steps of 10 %, or a drag), Open / Close for a cover, the favourite row.
 *   Settings  Server address and Signed in as (read only), Sign out (asks first; the server revokes the token).
 *   Locks and garage-type covers always ask ("Unlock Front door?" Yes / Cancel).
 *
 * The status line under the title: Connected (green), Connecting / Reconnecting (amber), Offline (red), with the time of
 * the last update while not connected; the rows keep their last values in grey. Data comes from a Backend (the
 * connection thread, or the simulated Home Assistant with HA_DEMO=<world> in the render harness), polled every 200 ms;
 * the lists are rebuilt at most about twice a second (set_rows keeps the scroll and the highlight).
 *
 * Keys: the app takes every key itself (the keypad path is intercepted, as Mesh Hop does): the arrows, Enter, Esc,
 * Backspace, Home / End and Page Up / Down drive the focused list, roller or dialog; letters filter a room.
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
constexpr const char *kVersion = "0.1.0";

class HaApp {
public:
    explicit HaApp(lv_obj_t *screen);
    ~HaApp();
    HaApp(const HaApp &) = delete;
    HaApp &operator=(const HaApp &) = delete;

private:
    enum class Screen { None, Connect, ServerEdit, Home, Room, Entity, Settings };
    struct Target {
        enum class Kind { None, Entity, Room, Settings, ServerAddress, SignOut };
        Kind kind = Kind::None;
        std::string id;
        ha::RowAction action = ha::RowAction::Info;
    };
    enum class RollerRow { State, Info, Slider, Open, Close, Favourite };

    /* pages */
    void show(Screen s);
    void clear_page();
    void build_list(bool caption_room);
    void build_connect();
    void build_server_edit();
    void build_entity();
    void set_title();

    /* data */
    void tick();
    void refresh(bool force);
    void refresh_connect();
    void refresh_list();
    void refresh_entity(bool rebuild);
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
    void roller_activate(int row);
    void roller_slide(int row, int value);
    void back();
    void open_entity(const std::string &id);
    void confirm(const std::string &question, std::function<void()> yes);
    void close_dialog();
    void post(std::function<void()> fn);
    static void posted_cb(void *data);
    void focus(lv_obj_t *obj);
    const ha::EntityRow *find_row(const std::string &id) const;

    lv_obj_t *screen_ = nullptr;
    cp0_ui_roller_page_t layout_{};
    std::unique_ptr<cp0::ui::Page> page_;
    std::unique_ptr<cp0::ui::CompactList> list_;
    std::unique_ptr<cp0::ui::Roller> roller_;
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

    Screen screen_kind_ = Screen::None;
    Screen entity_from_ = Screen::Home;
    lv_obj_t *focus_obj_ = nullptr;
    int list_x_ = 0, list_w_ = 0;
    std::vector<Target> targets_;
    std::vector<RollerRow> roller_kinds_;
    std::vector<cp0::ui::RowSpec> roller_specs_;
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

    /* the brightness slider: the last value sent, debounced; a tap's x on the slider row */
    int roller_tap_x_ = -1;
    lv_obj_t *slider_obj_ = nullptr;
    lv_timer_t *slide_timer_ = nullptr;
    int slide_value_ = -1;
    std::uint32_t slid_at_ = 0;

    lv_timer_t *timer_ = nullptr;
    int saved_intercept_ = 0;
    bool key_activation_ = false;
    std::uint32_t last_key_ = 0;
    std::vector<std::function<void()>> posted_;
    bool post_armed_ = false;
};

} // namespace haui
