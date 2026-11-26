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
#include "chat.hpp"
#include "link.hpp"
#include "model.hpp"
#include "rich_list.hpp"

#include "cp0_ui_roller.h"
#include "cp0_ui_widgets.hpp"

#include <cstdint>
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
    enum class Edit { None, Option, Name, DraftName, DisplayName, ContactName, ContactAddress, RenameContact };
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
    /* the same frame around a RichList (two-line rows, text blocks) */
    void frame_rich(const std::string &foot, const std::vector<RichRow> &rows, int selected);
    cp0_ui_list_layout_t prepare_frame(const std::string &top, ret::Tone top_tone, const std::string &foot, bool read_only);
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
    std::string home_foot() const; /* the hint under Home: true whether or not a connection is on */
    std::string connection_top(ret::Tone *tone) const;
    const ret::Iface *current_iface() const;
    int total_unread() const;
    const ret::OptionDef *option_def(std::intptr_t id) const;

    /* P2: contacts, messages, heard, my address (ret_chat.cpp) */
    std::vector<cp0::ui::ListRow> rows_contacts() const;
    std::vector<cp0::ui::ListRow> rows_contact() const;
    std::vector<cp0::ui::ListRow> rows_add_contact() const;
    std::vector<cp0::ui::ListRow> rows_conv_options() const;
    std::vector<RichRow> rich_messages() const;
    std::vector<RichRow> rich_heard() const;
    std::vector<RichRow> rich_address() const;
    std::vector<RichRow> rich_announce_blocked() const;
    const ret::Contact *current_contact() const;
    const ret::Conversation *current_conversation() const;
    std::string current_name() const;
    std::string clip_conv_title(const std::string &name) const;
    bool title_fits(const std::string &text) const;
    std::string footer_for(ret::View v) const;
    void activate_rich(std::intptr_t id);
    void activate_contacts(std::intptr_t id);
    void activate_contact(std::intptr_t id);
    void activate_add_contact(std::intptr_t id);
    void activate_conv_options(std::intptr_t id);
    void activate_heard(std::intptr_t id);
    void open_contact(const std::string &address);
    void open_conversation(const std::string &address);
    void open_contacts(bool pick);
    void begin_add_contact();
    void save_contact();
    void add_person(const ret::Person &p);
    void confirm_remove_contact();
    void confirm_delete_conversation();
    void confirm_announce();
    void start_announce();
    void do_announce();   /* the one request that sends an announcement: only from the dialog's primary button */
    void copy_address();
    void paste_address();
    void set_alerts(bool on);
    void request_contacts();
    void request_convs();
    void request_people();
    void request_limits();
    void request_all_chat();
    void handle_chat_event(const ret::json::Value &ev);
    void chat_changed();           /* the data under the visible page changed */
    void leave_deleted_conversation();
    void mark_read(const std::string &address);

    /* the conversation page (ret_convo.cpp) */
    void build_conversation();
    void teardown_conversation();
    void load_history();
    void show_messages(bool force = false); /* rebuild the bubbles from msgs_ when they changed */
    void layout_conversation();
    void make_more();
    void update_strip();
    void regroup();                /* the input group: bubbles, Retry buttons, the open-Connections row, field, Send, More */
    void update_compose();         /* the counter, the warning, Send */
    void send_message();           /* Send: the user's action */
    void retry_message(std::int64_t id);
    void retry_all_failed();
    bool compose_blocked() const;
    bool fire_ok(); /* one press makes both a key event and a click on a focused button: do the action once */
    void open_conv_options();
    void focus_compose();

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
    void request_notices(); /* things the service wants the user told once (a connection it switched off) */
    void switch_on_asking(); /* a kind with no listen-only state: asks, then switches on and allows sending */

    /* dialogs */
    void confirm(const std::string &title, const std::string &text, const std::string &yes, std::function<void()> on_yes);
    void notice(const std::string &title, const std::string &text);
    void close_dialog();
    void release_dialog_keys();
    void arm_dialog(std::size_t first); /* the dialog's buttons take the keys: Left / Right / Tab choose, Enter presses, Esc cancels */

    cp0_ui_roller_page_t layout_{};
    std::unique_ptr<cp0::ui::Page> page_;
    std::unique_ptr<cp0::ui::CompactList> list_;
    std::unique_ptr<cp0::ui::TextField> field_;
    std::unique_ptr<cp0::ui::TextBlock> text_;
    std::unique_ptr<RichList> rich_;
    std::unique_ptr<cp0::ui::Dialog> dialog_;
    std::vector<lv_obj_t *> dialog_buttons_; /* in the order of the spec: Cancel first, then the primary one */
    lv_group_t *dialog_group_ = nullptr, *saved_group_ = nullptr; /* the dialog's own input group while it is open */
    lv_indev_t *dialog_indev_ = nullptr;
    lv_timer_t *dialog_timer_ = nullptr; /* waits for the key that opened the dialog to come up, then gives the buttons the keyboard */
    std::size_t dialog_first_ = 0;
    std::uint32_t dialog_opened_tick_ = 0;
    bool dialog_done_ = false;               /* one button, once: a key press can also arrive as a click */
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
    bool ifaces_known_ = false; /* the service has answered `ifaces` at least once */
    ret::Deck deck_;
    ret::HeardList heard_;
    bool keep_known_ = false;
    bool keep_running_ = false;
    bool keep_busy_ = false;

    /* contacts and messages (P2) */
    ret::Limits limits_;
    std::vector<ret::Contact> contacts_;
    std::vector<ret::Conversation> convs_;
    std::vector<ret::Person> people_;
    bool contacts_known_ = false, convs_known_ = false, people_known_ = false;
    int unread_seen_ = -1;
    bool convs_dirty_ = false, contacts_dirty_ = false;
    bool convs_busy_ = false, contacts_busy_ = false;
    std::int64_t last_convs_ms_ = 0;
    std::string clip_;               /* what Copy address copied (and the system clipboard, when there is one) */
    bool pick_ = false;              /* the Contacts page chooses who to write to */
    std::string cur_addr_;           /* the open contact or conversation */
    std::string draft_text_, draft_for_; /* what was typed and not sent, kept while the page is left (More, Connections) */
    std::string conv_title_clipped_; /* the conversation's name as the header has room for it */
    std::vector<ret::Message> msgs_; /* the open conversation, oldest first */
    bool msgs_more_ = false;
    std::string draft_cname_, draft_caddr_; /* Add a contact */
    bool send_blocked_ = false;      /* the bridge said no_transmit */
    std::int64_t last_fire_ms_ = -100000;

    struct Convo {
        lv_obj_t *root = nullptr;    /* the area under the header */
        lv_obj_t *msgs = nullptr;    /* the scrolling bubbles (the page's focus object) */
        lv_obj_t *warn = nullptr;    /* "Long messages are slow over radio" */
        lv_obj_t *panel = nullptr;   /* blocked: the amber note */
        lv_obj_t *note = nullptr;    /* blocked, no room for the panel: its first sentence on one line */
        lv_obj_t *strip = nullptr;   /* the latest message on one line, instead of the bubbles (keyboard up) */
        lv_obj_t *strip_text = nullptr, *strip_status = nullptr;
        lv_obj_t *goto_conn = nullptr; /* blocked: Open Connections */
        lv_obj_t *send = nullptr;
        lv_obj_t *send_label = nullptr;
        lv_obj_t *counter = nullptr;
        lv_obj_t *more = nullptr;    /* in the header */
        std::vector<lv_obj_t *> retries;
        std::unique_ptr<cp0::ui::TextField> field;
        int inset_listener = 0;
        int osk_inset = 0;
        bool sending = false;
        bool built = false;
        std::string addr; /* whose conversation this is */
        std::string sig;  /* what the bubbles show now: they are rebuilt only when it changes (a rebuild moves the focus) */
    } convo_;

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
    bool field_error_ = false; /* the text page's hint line shows an error */
};

} // namespace retui
