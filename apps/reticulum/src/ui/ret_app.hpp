/*
 * SPDX-License-Identifier: MIT
 *
 * The Reticulum app: Connections and Status (phase P1b). One cp0::ui::Page (title, the shared status strip, the
 * on-screen Esc) and one body at a time under it, all laid out by cp0_lvgl's layout service (no geometry of its own):
 *
 *   Home          Connections, Status, Settings; the service line and the hint around the list
 *   Connections   one row per configured connection, "Add a connection"
 *   Connection    Switch on, Allow sending, the options of its kind (generated from the bridge's iface.types), Remove
 *   Add a connection  kind list (Wi-Fi / LAN, Internet, LoRa radio, More...) and a short form; Save creates it OFF
 *   Status        this deck, the connections, who was heard recently (read-only)
 *   Settings      Keep running, Name, My address, Stop the service
 *
 * Everything slow runs off the LVGL thread: the bridge socket on the Link's worker thread, systemctl through
 * async_run; both deliver on the LVGL thread. LVGL thread only (the callbacks arrive there).
 *
 * Nothing transmits without the user: a new connection is created off and listen-only, the Link refuses a
 * transmit-enabling request that does not carry the user's action, and the only such request is the confirmation of
 * the "Allow sending" dialog.
 */
#pragma once

#include "async_run.hpp"
#include "link.hpp"
#include "model.hpp"

#include "cp0_ui_roller.h"
#include "cp0_ui_widgets.hpp"

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace retui {

class App {
public:
    explicit App(lv_obj_t *screen);
    ~App();
    App(const App &) = delete;
    App &operator=(const App &) = delete;

private:
    enum class Svc { Checking, Starting, Up, Failed, Stopped };

    /* what a text page is for */
    enum class Edit { None, Option, Name, DraftName, DisplayName };
    struct Choice {
        std::string label, value;
        bool current = false;
        bool info = false; /* a line that is not a choice ("No port found") */
    };

    static void tick_cb(lv_timer_t *timer);
    static void posted_cb(void *data);
    void post(std::function<void()> fn);
    void tick();
    void focus(lv_obj_t *obj);
    std::int64_t now_ms() const;

    /* service */
    void begin_service();
    void start_service();
    void service_up();
    void service_failed(const std::string &why, Svc as = Svc::Failed);
    void probe();
    void send_poll();
    void check_keep_running();
    void set_keep_running(bool on);
    void stop_service();
    void on_link_lost(const ret::Response &r);

    /* navigation and frames */
    void render(int selected = 0);
    void open(ret::View v);
    void push_view(ret::View v);
    void open_connection(const std::string &name);
    void open_address();
    void back();
    void refresh_view();
    void set_title();
    void clear_view();
    cp0::ui::CompactListEvents list_events();
    void frame(const std::string &top, ret::Tone top_tone, const std::string &foot, bool read_only,
               const std::vector<cp0::ui::ListRow> &rows, int selected);
    void set_top(const std::string &text, ret::Tone tone);
    int line_h(int px) const;

    /* rows */
    std::vector<cp0::ui::ListRow> rows_home() const;
    std::vector<cp0::ui::ListRow> rows_connections() const;
    std::vector<cp0::ui::ListRow> rows_connection() const;
    std::vector<cp0::ui::ListRow> rows_add_kind(bool more) const;
    std::vector<cp0::ui::ListRow> rows_form() const;
    std::vector<cp0::ui::ListRow> rows_status() const;
    std::vector<cp0::ui::ListRow> rows_settings() const;
    std::vector<cp0::ui::ListRow> rows_choice() const;
    void add_option_rows(std::vector<cp0::ui::ListRow> &rows, std::size_t from, std::size_t to) const;
    std::string home_top(ret::Tone *tone) const;
    std::string connection_top(ret::Tone *tone) const;
    const ret::Iface *current_iface() const;
    const ret::OptionDef *option_def(std::intptr_t id) const;

    /* actions */
    void activate(std::intptr_t id);
    void activate_home(std::intptr_t id);
    void activate_connections(std::intptr_t id);
    void activate_connection(std::intptr_t id);
    void activate_add_kind(std::intptr_t id);
    void activate_form(std::intptr_t id);
    void activate_status(std::intptr_t id);
    void activate_settings(std::intptr_t id);
    void activate_choice(std::intptr_t id);
    void toggle_enabled();
    void toggle_sending();
    void allow_sending();
    void confirm_remove();
    void begin_form(const std::string &type);
    void save_draft();
    void edit_option(const ret::OptionDef &def);
    void open_choice(const ret::OptionDef &def);
    void open_text(Edit what, const std::string &title, const std::string &label, const std::string &initial,
                   cp0::ui::TextType type, std::size_t max_bytes, const std::string &hint);
    void submit_text(const std::string &text);
    void text_error(const std::string &message);
    void apply_option(const std::string &value, bool clear);
    void apply_choice(const Choice &c);
    void request_ifaces(std::function<void()> then = nullptr);
    void request_status();
    void request_failed(const ret::Response &r, const std::string &title);

    /* dialogs */
    void confirm(const std::string &title, const std::string &text, const std::string &yes, std::function<void()> on_yes);
    void notice(const std::string &title, const std::string &text);
    void close_dialog();

    cp0_ui_roller_page_t layout_{};
    std::unique_ptr<cp0::ui::Page> page_;
    std::unique_ptr<cp0::ui::CompactList> list_;
    std::unique_ptr<cp0::ui::TextField> field_;
    std::unique_ptr<cp0::ui::TextBlock> text_;
    std::unique_ptr<cp0::ui::Dialog> dialog_;
    lv_obj_t *top_ = nullptr;
    lv_obj_t *foot_ = nullptr;
    lv_obj_t *form_status_ = nullptr;
    lv_timer_t *timer_ = nullptr;
    std::vector<std::function<void()>> posted_;
    bool post_armed_ = false;

    /* the bridge */
    std::unique_ptr<ret::Link> link_;
    Svc svc_ = Svc::Checking;
    std::string svc_why_;
    std::int64_t svc_since_ms_ = 0;
    std::int64_t last_probe_ms_ = 0;
    std::int64_t last_poll_ms_ = 0;
    bool start_cmd_done_ = false;
    bool probe_busy_ = false;
    bool poll_busy_ = false;
    bool busy_ = false; /* a user action waits for the bridge: taps are ignored */
    int rendered_svc_ = -1;
    cdk::async_run::JobId start_job_ = 0;
    cdk::async_run::JobId unit_job_ = 0;
    std::string systemctl_bin_;

    /* what it answered */
    ret::Catalog catalog_;
    std::vector<ret::Iface> ifaces_;
    ret::Deck deck_;
    ret::HeardList heard_;
    bool keep_known_ = false;
    bool keep_running_ = false;
    bool keep_busy_ = false;

    /* navigation */
    ret::Nav nav_;
    std::vector<int> saved_sel_;
    std::string cur_name_; /* the connection page's connection */

    /* the form (the connection page's options, or the draft of a new connection) */
    std::vector<ret::OptionDef> form_opts_;
    std::size_t form_type_n_ = 0; /* the first form_type_n_ belong to the kind, the rest are the common ones */
    std::string draft_type_, draft_name_;
    std::map<std::string, std::string> draft_vals_;
    bool drafting_ = false; /* the form edits the draft, not a connection */

    /* one editor at a time */
    Edit edit_ = Edit::None;
    ret::OptionDef edit_def_;
    std::vector<Choice> choices_;
    std::string edit_title_;
    bool edit_secret_ = false;
};

} // namespace retui
