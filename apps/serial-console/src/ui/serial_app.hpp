/*
 * SPDX-License-Identifier: MIT
 *
 * Serial Console: the app's screen (LVGL thread only). One cp0::ui::Page (title, the shared status strip, the
 * on-screen Esc) and one cp0::ui::Roller whose rows change with the page, all laid out by cp0_lvgl's layout service:
 *
 *   Ports      a row per port found (the device's name, its tty at the right; the status line says
 *              "ttyUSB0 - 115200 recommended" for the highlighted one), refreshed every second (hot-plug: only the
 *              rows that changed are redrawn), a row per board in its USB boot loader (an RP2040 in BOOTSEL mode is a
 *              USB drive, not a serial port: the row says what to do), then "Show on-board UARTs" and, when the user
 *              is not in the dialout group, a row that opens the fix. No port: a card says what to do.
 *   Port       "What is connected" (Info rows: device, chip, maker, product, USB id and speed, driver, serial number,
 *              what it typically is), then "Identify what is connected": the adapter note (a bare USB-serial adapter
 *              says nothing about what is behind it), "Behind it" (the last result: opens its card), Listen, Reset and
 *              listen (a confirmation first), Ask the device (the Ask page); then Speed, Frame, Echo, Enter sends,
 *              Backspace sends, Log to file, Find the speed (a Busy row while it listens: "Trying 9600... (2 of 12)",
 *              Esc or a tap stops it), Send Enter while finding, and Connect (highlighted when the page opens). The
 *              status line explains the recommended settings (or what the highlighted probe does). Settings and the
 *              identification are kept per port in the settings file.
 *   Ask        the level 3 probes (MicroPython, AT, Marlin, GRBL, Arduino boot loader, ESP boot loader): each asks a
 *              confirmation that lists exactly what it sends; a Busy row while it runs (Esc stops it)
 *   Card       the result: Info rows (Device, Behind it, Chip, Firmware, Board, Details, Confidence, Found,
 *              Recommended) and the actions (Boot output, Evidence, Apply recommended settings, Run again, Copy summary
 *              to the log file); Boot output and Evidence open a Detail page (a TextBlock in the monospace face)
 *   Choice     the values of Speed, Frame, Enter sends or Backspace sends (Choice rows, the current one checked)
 *   Terminal   terminal_view.hpp
 * A refused open (no permission, busy) shows a Dialog with the exact fix. Esc / Left / the header's Esc go one level
 * up; at the ports page they end the app (cdk::request_quit).
 */
#pragma once

#include "console_core.hpp"
#include "demo.hpp"
#include "probe.hpp"
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
    enum class PortRow { Info, Behind, IdListen, IdReset, IdAsk, Speed, Frame, Echo, Ending, Backspace, Log, Find,
                         FindEnter, Connect };
    struct ListRow {
        enum Kind { Port, Boot, Onboard, Access } kind = Port;
        int port = -1; /* index in ports_ (Port) or boots_ (Boot) */
    };

    static void posted_cb(void *data);
    static void tick_cb(lv_timer_t *timer);
    void post(std::function<void()> fn);
    void tick();
    void focus(lv_obj_t *obj);
    void show_title();
    void back();

    /* ports page */
    std::vector<PortInfo> scan_all(bool with_onboard);
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

    /* "What is on the other end?" (identify.cpp of ui/) */
    void start_probe(Probe probe, bool confirmed);
    void check_probe();
    void probe_done(const ProbeProgress &p, Probe probe);
    void open_ask(int select);
    std::vector<cp0::ui::RowSpec> ask_rows();
    void activate_ask(int row);
    void update_ask_caption(bool force);
    void open_card(const Identity &id, Probe probe, int select_action);
    void show_card_rows(int select);
    void activate_card(int row);
    void update_card_caption(bool force);
    void open_detail(const std::string &title, const std::vector<std::string> &lines);
    void close_detail();
    void close_card();
    void show_confirm(const std::string &title, const std::string &text, const std::string &ok,
                      std::function<void()> on_ok);
    Identity port_identity() const;
    bool probe_running() const { return prober_ != nullptr; }

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
    std::unique_ptr<cp0::ui::TextBlock> detail_;
    lv_obj_t *card_ = nullptr;
    lv_timer_t *timer_ = nullptr;
    std::vector<std::function<void()>> posted_;
    bool post_armed_ = false;

    std::vector<PortInfo> ports_;
    std::vector<BootDrive> boots_;
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

    /* the identification */
    std::unique_ptr<Prober> prober_;
    Probe probe_ = Probe::Listen;      /* the running probe */
    std::string probe_shown_;          /* its Busy row's text */
    std::string probe_value_[2];       /* the Listen / Reset rows' last outcome ("Stopped", "Nothing heard") */
    std::string ask_value_;            /* the Ask page's last outcome, for ask_probe_ */
    Probe ask_probe_ = Probe::MicroPython;
    std::vector<Probe> ask_map_;
    Identity card_id_;
    Probe card_probe_ = Probe::Listen;
    std::vector<CardRow> card_rows_;
    std::string card_note_;            /* the card's status line after an action */
    bool card_note_warn_ = false;
};

} // namespace sc
