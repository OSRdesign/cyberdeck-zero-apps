/*
 * SPDX-License-Identifier: MIT
 *
 * viz1090 intro screen (/opt/viz1090/vizsetup), a native app on the launcher's cp0_lvgl. run_viz1090.sh runs it before
 * the map: exit status 0 = start viz1090 (the position is saved), anything else = back to the launcher.
 *
 * One cp0::ui::Page (title, the shared status strip, the on-screen Esc) and, under it, the pages (LVGL thread only; every
 * size from the layout service's tokens and the widgets, nothing in pixels):
 *
 *   viz1090    a status line, the action bar with Start (TARGET high), then a CompactList (Comfortable rows):
 *              SDR receiver   Found / Not found (a check / a cross), checked again every second (hot-plug)
 *              GPS receiver   None detected / Found on /dev/ttyACM0, waiting for a fix / Fix: 48.8566, 2.3522;
 *                             Enter or a tap on it with a fix takes the GPS position
 *              Position       CDG  49.0097, 2.5478 (or Not set); Enter or a tap opens the Position page
 *              Start saves the position (~/.config/cardputerzero/viz1090.conf) and ends the app with 0; without a usable
 *              position it says why in the status line and stays.
 *   Position   the text page: one TextField "Nearest airport code" (letters, upper case, at most 4) with its line under
 *              it (found: CDG / not in the list); Enter on a known code sets the position and goes back. Under them a
 *              CompactList: Latitude, Longitude (each opens its own one-field page) and RECENT, the last 5 places used
 *              (a tap or Enter sets one and goes back): a place can be chosen without typing. While the on-screen
 *              keyboard is up the list is hidden, so the page is the header, the field and its line.
 *   Latitude / Longitude   one TextField (digits, '-', '.'), the range under it; Enter applies and goes back.
 *
 * The on-screen keyboard: only by the shared rule (_common/osk_policy: cp0_app_osk_allowed on input.state), shown by the
 * TextField while it edits; never summoned by hand. On the Position page the field starts editing at once when the
 * rule keeps the keyboard away (a physical keyboard types at once); when the keyboard may show, the page opens on its
 * list (the field, the rows and Recent all in view) and a tap on the field (or Up / Tab) starts editing.
 *
 * Esc / Left / the header's Esc: one page up; at the root they end the app with 1 (cancelled).
 */
#pragma once

#include "action_bar.hpp"
#include "model.hpp"

#include "cp0_ui_list.h"
#include "cp0_ui_roller.h"
#include "cp0_ui_widgets.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace viz {

/* Start saved the position: the process ends with 0 (run_viz1090.sh starts the map). Any other end is 1. */
bool start_confirmed();

class VizApp {
public:
    explicit VizApp(lv_obj_t *screen);
    ~VizApp();
    VizApp(const VizApp &) = delete;
    VizApp &operator=(const VizApp &) = delete;

private:
    enum RowId : std::intptr_t { kSdr = 1, kGps, kPosition, kLat = 10, kLon, kRecent = 100 /* + index */ };
    enum CellId { kStart = 1 };

    /* the column the pages are built in, from the layout service */
    struct Geo {
        int x = 0, w = 0;        /* the column */
        int s = 0, inset = 0;    /* SPACE_S, SPACE_L */
        int lh = 0;              /* a line of CAPTION text */
        int top = 0, bottom = 0; /* the body under the header, less SPACE_S at the top */
        int target = 0;          /* TARGET token: the action bar */
    };

    static void posted_cb(void *data);
    static void tick_cb(lv_timer_t *timer);
    static void list_key_cb(lv_event_t *event);
    void post(std::function<void()> fn);
    void focus(lv_obj_t *obj);
    void tick();
    Geo geo() const;
    std::int64_t now_ms() const;

    void show(int select);
    void teardown();
    void back();

    /* root */
    void build_root(int select);
    std::vector<cp0::ui::ListRow> root_rows() const;
    void refresh_root(bool force);
    void activate_root(std::intptr_t id);
    void start();
    void show_band();
    void note(const Band &band);

    /* Position, Latitude, Longitude */
    void build_position(int select);
    std::vector<cp0::ui::ListRow> position_rows() const;
    void activate_position(std::intptr_t id);
    void code_changed(const std::string &text);
    void code_submit(const std::string &text);
    void build_degrees(bool latitude);
    void degrees_submit(bool latitude, const std::string &text);
    void build_field_page(const std::string &label, const std::string &text, const std::string &placeholder,
                          std::size_t max_bytes, std::function<void(const std::string &)> changed,
                          std::function<void(const std::string &)> submit);
    void set_hint(const Band &band);
    void pick(const Place &place); /* a place chosen on the Position page: set, back to the root */
    void on_inset(int inset);

    /* lists */
    void build_list(int y, int h, std::vector<cp0::ui::ListRow> rows, int select);
    cp0::ui::ListRow list_row(const RowText &text, std::intptr_t id) const;
    std::string fit_value(const RowText &text) const;
    void prepare_fonts();

    cp0_ui_roller_page_t layout_{};
    cp0_ui_list_layout_t list_layout_{}; /* the root's list (for the fonts and widths of fit_value) */
    std::unique_ptr<cp0::ui::Page> page_;
    std::unique_ptr<cp0::ui::CompactList> list_;
    std::unique_ptr<ui::ActionBar> bar_;
    std::unique_ptr<cp0::ui::TextField> field_;
    lv_obj_t *band_ = nullptr;
    lv_obj_t *hint_ = nullptr;
    lv_timer_t *timer_ = nullptr;
    std::vector<std::function<void()>> posted_;
    bool post_armed_ = false;
    int inset_listener_ = 0;
    std::function<void()> list_up_;  /* Up on the list's first row, Tab: back to the bar / the field */

    Nav nav_;
    Setup setup_;
    std::vector<Place> recent_;
    AirportList airports_;
    std::unique_ptr<GpsWatcher> gps_;
    GpsSnapshot gps_shown_;
    std::string conf_path_;
    std::string usb_dir_;
    std::string sdr_;
    bool installed_ = false;
    std::int64_t next_sdr_ms_ = 0;
    Band note_;
    bool noted_ = false;
    std::string code_typed_;
};

} // namespace viz
