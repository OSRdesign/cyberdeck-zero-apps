/*
 * SPDX-License-Identifier: MIT
 *
 * Serial Console: the app's screen (LVGL thread only). One cp0::ui::Page (title, the shared status strip, the
 * on-screen Esc), a status band (two lines of CAPTION text) and, under it, one cp0::ui::CompactList whose rows change
 * with the page: Comfortable rows on Ports and Ask (a few rows that act), compact rows on Port and Card (long
 * tables). Top-aligned rows, scrolling only when the highlight reaches the bottom, a thin auto-hiding scrollbar,
 * headings that are not selectable. The list is the deck's "detail column" (the full width less a 40 px margin). The
 * Choice page keeps a cp0::ui::Roller (a short pick-one list with a check on the current value). All laid out by
 * cp0_lvgl's layout service:
 *
 *   Ports      a row per port found (the device's name, its tty at the right; the status band says
 *              "ttyUSB0 - 115200 recommended" for the highlighted one), refreshed every second (hot-plug: only the
 *              texts that changed are redrawn, the scroll and the highlight stay), a row per board in its USB boot
 *              loader (an RP2040 in BOOTSEL mode is a USB drive, not a serial port: the row says what to do), then
 *              "Show on-board UARTs" (On / Off) and, when the user is not in the dialout group, a row that opens the
 *              fix. No port: a card under the rows says what to do.
 *   Port       the section "What is connected" (device, chip, maker, product, USB id and speed, driver, serial number,
 *              what it typically is), the section "Identify what is connected": the adapter note (a bare USB-serial
 *              adapter says nothing about what is behind it; a long note takes two rows), "Behind it" (the last
 *              result: opens its card), Listen, Reset and listen (a confirmation first), Ask the device (the Ask
 *              page); the section "Settings": Speed, Frame, Local echo (On / Off), Enter sends, Backspace sends, Log
 *              to file, Find the speed (the row says "Trying 9600... (2 of 12)" with the value Stop while it listens;
 *              Esc or a tap stops it), Send Enter while finding, and Connect (highlighted when the page opens). The
 *              status band explains the recommended settings (or what the highlighted probe does). Settings and the
 *              identification are kept per port in the settings file.
 *   Ask        the level 3 probes (MicroPython, AT, Marlin, GRBL, Arduino boot loader, ESP boot loader): each asks a
 *              confirmation that lists exactly what it sends; the row says the progress with the value Stop while it
 *              runs (Esc stops it)
 *   Card       the result: plain rows (Device, Behind it, Chip, Firmware, Board, Details, Confidence, Found,
 *              Recommended) and the actions (Boot output, Evidence, Apply recommended settings, Run again, Copy summary
 *              to the log file); Boot output and Evidence open a Detail page (a TextBlock in the monospace face). A
 *              value too long for its row is cut with dots; the status band then says the whole of it.
 *   Choice     the values of Speed, Frame, Enter sends or Backspace sends (a Roller of Choice rows, the current one
 *              checked: a pick-one list, which CompactList has no check mark for)
 *   Terminal   terminal_view.hpp
 * A refused open (no permission, busy) shows a Dialog with the exact fix. Esc / Left / the header's Esc go one level
 * up; at the ports page they end the app (cdk::request_quit).
 */
#pragma once

#include "console_core.hpp"
#include "demo.hpp"
#include "probe.hpp"
#include "terminal_view.hpp"

#include "cp0_ui_list.h"
#include "cp0_ui_roller.h"
#include "cp0_ui_widgets.hpp"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace sc {

/* The list's row affordance (chevron, grey information, blue actions) of a row kind. */
using Aff = cp0::ui::RowAffordance;
inline Aff to_affordance(RowKind kind)
{
    switch (kind) {
    case RowKind::Open: return Aff::Open;
    case RowKind::Edit: return Aff::Edit;
    case RowKind::Action: return Aff::Action;
    case RowKind::Info: break;
    }
    return Aff::Info;
}

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
    enum class Density { Compact, Comfortable }; /* cp0_ui_list_density_t's values */
    struct PortsRow {
        enum Kind { Port, Boot, Onboard, Access } kind = Port;
        int port = -1; /* index in ports_ (Port) or boots_ (Boot) */
    };

    static void posted_cb(void *data);
    static void tick_cb(lv_timer_t *timer);
    void post(std::function<void()> fn);
    void tick();
    void focus(lv_obj_t *obj);
    lv_obj_t *focus_target() const;
    void show_title();
    void back();

    /* the list, the status band, what the page shows */
    void build_list(Density density, bool wrap);
    void ensure_list(Density density, bool wrap);
    void make_roller();
    int sel() const { return list_ ? list_->selected() : roller_ ? roller_->selected() : -1; }
    void say(const std::string &text, cp0::ui::CaptionTone tone);
    void update_chrome();
    void dispatch_activate(int row);
    /* a row; a value too wide is cut (the whole "label: value" goes to full); with note, a label with no value is a
     * note: section rows (grey capitals, never selected), as many as its text needs (full is "" for each) */
    void add_rows(std::vector<cp0::ui::ListRow> &rows, std::vector<std::string> &full, Density density,
                  const std::string &label, const std::string &value, cp0::ui::ValueTone tone, bool note,
                  cp0::ui::RowAffordance aff = cp0::ui::RowAffordance::Unspecified) const;
    std::string fit_text(const std::string &text, const lv_font_t *font, int room) const;
    void show_empty_card(bool show);

    /* ports page */
    std::vector<PortInfo> scan_all(bool with_onboard);
    void scan();
    void show_ports(const std::string &select_id, bool select_toggle, bool keep = false);
    std::vector<cp0::ui::ListRow> ports_rows();
    void update_ports_caption(bool force);
    void activate_ports(int row);

    /* port page */
    void open_port_page(const PortInfo &port);
    std::vector<cp0::ui::ListRow> port_rows();
    void show_port_rows(int select, bool keep = false);
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
    std::vector<cp0::ui::ListRow> ask_rows();
    void activate_ask(int row);
    void update_ask_caption(bool force);
    void open_card(const Identity &id, Probe probe, int select_action);
    void show_card_rows(int select);
    int card_list_row(int card_index) const;
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
    std::unique_ptr<cp0::ui::CompactList> list_;
    std::unique_ptr<cp0::ui::Roller> roller_;    /* the Choice page only */
    std::unique_ptr<cp0::ui::Dialog> dialog_;
    std::unique_ptr<TerminalView> term_;
    std::unique_ptr<cp0::ui::TextBlock> detail_;
    lv_obj_t *card_ = nullptr;                   /* the empty ports page's card */
    lv_obj_t *band_ = nullptr;                   /* the status band over the list */
    Density list_density_ = Density::Comfortable;
    bool list_wrap_ = true;
    cp0_ui_list_layout_t lay_[2]{};              /* the list's layout per Density (the same column, top and height) */
    const lv_font_t *label_font_[2] = {nullptr, nullptr};
    const lv_font_t *value_font_[2] = {nullptr, nullptr};
    const lv_font_t *note_font_[2] = {nullptr, nullptr};   /* the section rows' font (a note is a section: grey, not selected) */
    lv_timer_t *timer_ = nullptr;
    std::vector<std::function<void()>> posted_;
    bool post_armed_ = false;

    std::vector<PortInfo> ports_;
    std::vector<BootDrive> boots_;
    std::vector<PortsRow> ports_map_;
    std::int64_t next_scan_ = 0;
    int caption_row_ = -2;

    PortInfo port_;
    bool port_present_ = true;
    PortSettings settings_;
    std::vector<PortRow> port_map_;              /* the port page's list rows (a section: Info) */
    std::vector<std::string> row_full_;          /* the shown page's rows: the whole "label: value" of a cut row, else "" */
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
    std::vector<int> card_map_;        /* the card's list rows: the index in card_rows_ (-1: a continuation line) */
    std::string card_note_;            /* the card's status line after an action */
    bool card_note_warn_ = false;
};

} // namespace sc
