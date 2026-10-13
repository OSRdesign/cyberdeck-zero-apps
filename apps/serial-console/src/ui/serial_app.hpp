/*
 * SPDX-License-Identifier: MIT
 *
 * Serial Console: the app's screen (LVGL thread only). One cp0::ui::Page (title, the shared status strip, the
 * on-screen Esc) and one cp0::ui::Roller whose rows change with the page, all laid out by cp0_lvgl's layout service:
 *
 *   Ports      a row per port found (the device's name, its tty at the right; the status line says
 *              "ttyUSB0 - 115200 recommended" for the highlighted one), refreshed every second (hot-plug: only the
 *              rows that changed are redrawn), then "Show on-board UARTs" and, when the user is not in the dialout
 *              group, a row that opens the fix. No port: a card says what to do.
 *   Port       "What is connected" (Info rows: device, maker, USB id, driver, serial number, what it typically is),
 *              then Speed, Frame, Echo, Enter sends, Backspace sends, Log to file, Find the speed (a Busy row while it
 *              listens: "Trying 9600... (2 of 12)", Esc or a tap stops it), Send Enter while finding, and Connect
 *              (highlighted when the page opens). The status line explains the recommended settings. Settings are
 *              kept per port in the settings file.
 *   Choice     the values of Speed, Frame, Enter sends or Backspace sends (Choice rows, the current one checked)
 *   Terminal   terminal_view.hpp
 * A refused open (no permission, busy) shows a Dialog with the exact fix. Esc / Left / the header's Esc go one level
 * up; at the ports page they end the app (cdk::request_quit).
 */
#pragma once

#include "console_core.hpp"
#include "demo.hpp"
#include "terminal_view.hpp"

#include "cp0_ui_roller.h"
#include "cp0_ui_widgets.hpp"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace sc {

class SerialApp {
public:
    explicit SerialApp(lv_obj_t *screen);
    ~SerialApp();
    SerialApp(const SerialApp &) = delete;
    SerialApp &operator=(const SerialApp &) = delete;

private:
    enum class Choice { Speed, Frame, Ending, Backspace };
    enum class PortRow { Info, Speed, Frame, Echo, Ending, Backspace, Log, Find, FindEnter, Connect };
    struct ListRow {
        enum Kind { Port, Onboard, Access } kind = Port;
        int port = -1; /* index in ports_ */
    };

    static void posted_cb(void *data);
    static void tick_cb(lv_timer_t *timer);
    void post(std::function<void()> fn);
    void tick();
    void focus(lv_obj_t *obj);
    void show_title();
    void back();

    /* ports page */
    void scan();
    void show_ports(const std::string &select_id, bool select_toggle);
    std::vector<cp0::ui::RowSpec> ports_rows();
    void update_ports_caption(bool force);
    void show_card(bool show);
    void activate_ports(int row);

    /* port page */
    void open_port_page(const PortInfo &port);
    std::vector<cp0::ui::RowSpec> port_rows();
    void show_port_rows(int select);
    int port_row_index(PortRow which) const;
    void update_port_row(PortRow which);
    void update_port_caption(bool force);
    void activate_port(int row);
    void start_finder();
    void check_finder();
    void save_port();

    /* value lists */
    void open_choice(Choice which);
    void activate_choice(int row);

    /* terminal */
    void connect();
    void close_terminal();

    void show_problem(const Problem &problem);
    void close_dialog();
    GroupState groups() const;
    bool can_open(const PortInfo &port) const;
    int recommended() const { return port_.rec.baud; }

    cp0_ui_roller_page_t layout_{};
    Nav nav_;
    std::unique_ptr<demo::World> world_;
    Roots roots_;
    std::string config_path_, log_dir_;
    Config config_;

    std::unique_ptr<cp0::ui::Page> page_;
    std::unique_ptr<cp0::ui::Roller> roller_;
    std::unique_ptr<cp0::ui::Dialog> dialog_;
    std::unique_ptr<TerminalView> term_;
    lv_obj_t *card_ = nullptr;
    lv_timer_t *timer_ = nullptr;
    std::vector<std::function<void()>> posted_;
    bool post_armed_ = false;

    std::vector<PortInfo> ports_;
    std::vector<ListRow> list_;
    std::int64_t next_scan_ = 0;
    int caption_row_ = -2;

    PortInfo port_;
    bool port_present_ = true;
    PortSettings settings_;
    std::vector<PortRow> port_map_;
    std::unique_ptr<SpeedFinder> finder_;
    std::string finder_shown_;
    std::string find_value_;
    int find_rgb_ = -1;
    std::string port_note_;   /* the port page's status line after a detection ("" = the recommendation) */
    bool port_note_warn_ = false;
    Choice choice_ = Choice::Speed;
    std::vector<std::string> choice_values_;
};

} // namespace sc
