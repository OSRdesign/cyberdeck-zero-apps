/*
 * SPDX-License-Identifier: MIT
 *
 * Mesh Hop: the full-screen 640x480 MeshCore messenger. The screens (Chats, Map, Contacts, Terminal, Settings), the popups, the
 * status footer and the text editor, written with LVGL over the core/ layer (client, model, store). Input comes from the Platform
 * as KeyEvents and LVGL pointer events; nothing here talks to the radio except through meshzero::Client.
 */

#pragma once

#include "client.hpp"
#include "deck_clock.hpp"
#include "log.hpp"
#include "model.hpp"
#include "platform.hpp"
#include "preset_feed.hpp"
#include "serial_transport.hpp"
#include "store.hpp"
#include "ui_logic.hpp"

#include <functional>
#include <memory>
#include <string>
#include <vector>

extern "C" {
#include "cp0_statusbar.h"
}

namespace meshhop {

struct Cell {
    std::string text;
    uint32_t color = 0xF4F4F5;
    int x = 0, w = 100;
    lv_text_align_t align = LV_TEXT_ALIGN_LEFT;
    const lv_font_t *font = nullptr;
    int y = 0;
    uint32_t box = 0;                    // not 0: drawn as a button (a rounded box of this colour) the size of the cell
};

struct RowSpec {
    int h = 40;
    bool selectable = true;
    bool header = false;
    int badge = 0;                       // unread count (0: none)
    std::string id;                      // what the row stands for (a contact key, a conversation, a settings row): taps resolve it from here
    std::vector<Cell> cells;
};

/* A scrollable list of rows (touch: drag to scroll, tap a row; keys: the selection). It is virtual: the data of every row is kept,
 * but only the rows in view (plus a margin) exist as widgets, in a small pool that is re-used while scrolling. A key press that moves
 * the selection only re-binds two rows, so 161 contacts move as fast as 10. */
class RowList {
public:
    struct Config {
        int max_cells = 6;               // the most cells of a row
        int max_boxes = 0;               // the most boxed (button-like) cells of a row
        int min_row_h = 40;              // the smallest row height: sizes the pool
    };
    void create(lv_obj_t *parent, int x, int y, int w, int h, const Config &cfg);
    /* Replaces all rows. Keeps the scroll position. */
    void set(std::vector<RowSpec> rows, int selected, bool focused);
    /* Moves the selection only (cheap). scroll: bring the selected row into view. */
    void set_selected(int selected, bool focused, bool scroll);
    void scroll_to_selected();
    void scroll_to_top();
    lv_obj_t *obj() const { return cont_; }
    /* Forgets the widgets (their parent was cleaned): the list can be create()d again, for a list inside a popup. */
    void reset();
    int selected() const { return selected_; }
    int count() const { return static_cast<int>(rows_.size()); }
    const std::string &id_at(int index) const;
    void update_window();                // from the scroll event
    int bound_rows() const;              // widgets in use (tests, diagnostics)

private:
    struct Slot {
        lv_obj_t *row = nullptr, *bar = nullptr, *pill = nullptr, *pill_label = nullptr;
        std::vector<lv_obj_t *> labels, boxes;
        int index = -1;
    };
    void bind(Slot &s, int index);
    void hide(Slot &s);

    lv_obj_t *cont_ = nullptr, *spacer_ = nullptr;
    Config cfg_;
    int w_ = 0, h_ = 0;
    std::vector<Slot> pool_;
    std::vector<RowSpec> rows_;
    std::vector<int> off_;
    int total_ = 0;
    int selected_ = -1;
    bool focused_ = true;
};

class App {
public:
    explicit App(Platform &plat);
    ~App();
    App(const App &) = delete;
    App &operator=(const App &) = delete;

    /* From the main loop, every few milliseconds: polls the radio client, refreshes what changed. */
    void tick();
    void on_key(const KeyEvent &e);
    /* A short Esc: Back. Never quits. */
    void back();

    // for the scripted tests and the screenshots
    Tab tab() const { return nav_.tab; }
    meshzero::Client &client() { return *client_; }
    meshzero::Model &model() { return model_; }
    void select_tab(Tab t);
    /* One line with the state of the app (scripted tests). */
    std::string debug_state() const;

    // LVGL callbacks (public for the static trampolines)
    void button_clicked(int tag);
    void row_clicked(RowList *list, int index, int x);
    /* A press on a name that can be held for 3 s (header title or a row of the chat list): what = 0 pressed, 1 moved, 2 ended. */
    void hold_event(int what, RowList *list, int index, int x, int y);

private:
    enum class EditKind { Name, Number, Channel, Clock, PrivName, PrivKey, RandName, GroupNew, GroupRename, DateRange };

    struct Popup {
        enum Kind { None, Choice, Confirm, Menu, Info, List, Progress } kind = None;
        std::string title, body;
        meshhop::Choice choice;
        std::vector<std::string> items;
        int focus = 0;                   // Confirm: 0 = No, 1 = Yes. Menu, List: the selected item
        std::string yes_text = "Yes", no_text = "No", ok_text = "OK";
        std::function<void(int)> on_accept;   // Choice: the index. Menu, List: the item
        std::function<void()> on_yes;
        std::function<void()> on_cancel;
        uint64_t opened_ms = 0;
        uint32_t yes_delay_ms = 0;       // Confirm: the Yes button only works after this long (the second box of a factory reset)
        bool danger = false;             // Confirm: the Yes button is red (it erases something)
    };

    // construction
    void load_fonts();
    void build_screen();
    void build_chats();
    void build_contacts();
    void build_nearby();
    void build_search();
    void build_stats();
    void build_settings();
    void build_placeholder(lv_obj_t **page, const char *title, const char *text);
    void build_editor();
    void build_popup();
    void build_footer();
    lv_obj_t *make_button(lv_obj_t *parent, int x, int y, int w, int h, const char *text, int tag, lv_obj_t **label_out = nullptr);
    lv_obj_t *make_label(lv_obj_t *parent, const lv_font_t *font, uint32_t color, int x, int y, int w, int h,
                         lv_text_align_t align = LV_TEXT_ALIGN_LEFT);

    // rendering
    void render();
    void render_tabs();
    void render_chats();
    void render_chat_messages(bool force);
    void render_contacts();
    void render_contact_toolbar();
    void render_detail();
    void render_nearby();
    void render_search();
    void render_stats();
    void render_settings();
    void render_footer();
    void update_statusbar(bool force);
    void update_compose_style();
    void set_page_visible();

    // state changes
    void invalidate() { dirty_ = true; }
    void notice(const std::string &text, bool ok);
    bool keyboard_ready() const;
    void select_chat_row(int index);
    void open_conversation(const std::string &conv, bool focus_compose);
    void toggle_mute(const std::string &conv);
    void open_conversation_options(const std::string &conv);
    void confirm_delete_conversation(const std::string &conv);
    void confirm_delete_all();
    void choose_delete_older();
    void confirm_delete_older(int days);
    void after_deletion();
    void on_board_changed();             // another board (identity) is connected: its own data is loaded, the selections of the old one go
    void confirm_forget_board();
    void confirm_forget_others();
    void refresh_other_boards(bool force);
    void presets_tick(uint64_t now);
    void hold_tick(uint64_t now);
    void send_message();
    void open_detail(const std::string &key_hex);
    void activate_contact(const std::string &key_hex);
    void move_contact_selection(int delta, bool absolute);
    void change_contact_view(const ContactView &next);
    void apply_contact_view();
    void show_loading(bool on);
    void rebuild_contact_snapshot();
    void build_contact_specs();          // the rows of the table from the current snapshot and selection (no new sort)
    void settings_activate(int index, int x);
    void settings_step(int index, int delta);
    void settings_save();
    void settings_undo();
    void request_save();                 // the Yes / No box before the settings go to the radio
    void request_undo();
    void settings_choice(ChoiceField f);
    void channel_menu(int idx);
    void show_channel_key(int idx);
    void confirm_remove_channel(int idx);
    void add_channel_menu();
    int max_compose_bytes() const;

    // phase 2: contacts
    void enter_select_mode();
    void leave_select_mode();
    void toggle_selected(const std::string &key_hex);
    void open_select_menu();
    void confirm_bulk_delete();
    void open_group_filter();
    void open_group_assign();
    void group_pick(GroupAction a);
    void ask_new_group(const std::vector<std::string> &keys);
    void group_toggle_for(const std::string &key_hex);      // the contact detail: pick the groups of one contact
    void open_contact_info(const std::string &key_hex);
    void delete_detail_contact();
    void open_nearby();
    void close_nearby();
    void nearby_activate(int index, int x);
    void nearby_ignore(const std::string &key_hex);
    void key_nearby(const KeyEvent &e);
    void key_detail(const KeyEvent &e);
    // phase 2: message search
    void open_search();
    void close_search();
    void refresh_search_results(bool to_top = true);
    void search_open_hit(int index);
    void search_pick_conv();
    void search_pick_date();
    void search_edit(const KeyEvent &e);
    void key_search(const KeyEvent &e);
    // phase 2: statistics and the board
    void open_stats();
    void close_stats();
    void key_stats(const KeyEvent &e);
    void autoadd_menu();
    void ask_reboot();
    void ask_factory_reset();
    void ask_repeat(bool on);
    void tick_phase2(uint64_t now);

    // popups
    void open_choice(ChoiceField f);
    void open_confirm(const std::string &title, const std::string &body, std::function<void()> on_yes, const std::string &yes = "Yes",
                      const std::string &no = "No", uint32_t yes_delay_ms = 0, bool danger = false);
    void open_menu(const std::string &title, const std::vector<std::string> &items, std::function<void(int)> on_pick);
    void open_list(const std::string &title, const std::vector<std::string> &items, int focus, std::function<void(int)> on_pick);
    void open_progress(const std::string &title);
    void open_info(const std::string &title, const std::string &body, const std::string &small);
    void popup_begin(Popup p);
    void popup_build();                  // the widgets of the pending popup (from tick, never from an event of a popup button)
    void popup_refresh();                // the values, arrows and focus of the open popup
    void popup_key(const KeyEvent &e);
    void popup_click(int tag);
    void popup_accept();
    void popup_cancel();
    void popup_yes();
    void close_popup();

    // editor
    void open_editor(EditKind kind, const std::string &title, const std::string &hint, const std::string &initial, int row = 0);
    void close_editor(bool accept);
    void editor_key(const KeyEvent &e);
    void editor_error(const std::string &text);
    void editor_note(const std::string &text);

    // keys per screen
    void key_chats(const KeyEvent &e);
    void key_contacts(const KeyEvent &e);
    void key_settings(const KeyEvent &e);
    void compose_edit(const KeyEvent &e);
    void scroll_messages(int dy);

    // core
    Platform &plat_;
    meshzero::Model model_;
    std::unique_ptr<meshzero::Store> store_;
    std::unique_ptr<meshzero::FileLog> log_;
    meshzero::SerialTransport serial_;
    meshzero::DeckClockProbe deck_clock_;
    std::unique_ptr<meshzero::Client> client_;
    std::unique_ptr<meshzero::PresetFetcher> fetcher_;

    // the radio preset list refresh (A): one background download per launch (and one retry 60 s after a failure), started once the deck is online
    std::unique_ptr<meshzero::PresetSchedule> preset_sched_;
    std::vector<meshzero::RadioPreset> preset_pending_;       // a fetched list waiting for the preset popup to be closed
    bool preset_pending_ready_ = false;
    std::string preset_pending_date_;

    // a touch held on a name (B)
    HoldTracker hold_;
    int hold_scroll_y_ = 0;
    uint64_t preset_poll_ms_ = 0;
    int days_choice_ = 30;

    // fonts
    const lv_font_t *f_small_ = nullptr, *f_ui_ = nullptr, *f_big_ = nullptr;     // Montserrat 14 / 16 / 18
    const lv_font_t *f_value_ = nullptr, *f_value_small_ = nullptr;               // Montserrat 28 / 22: the value of a choice popup
    const lv_font_t *f_text_ = nullptr, *f_text_small_ = nullptr;                 // message text (DejaVu when available)
    bool dejavu_ = false;

    // state
    NavState nav_;
    bool dirty_ = true;
    uint64_t start_ms_ = 0, last_poll_ms_ = 0, last_render_ms_ = 0, last_bar_ms_ = 0, kbd_check_ms_ = 0, prompt_ms_ = 0;
    uint32_t last_revision_ = 0, last_link_ = 0xFFFF, last_notice_id_ = 0;
    std::string notice_text_;
    bool notice_ok_ = true;
    uint64_t notice_ms_ = 0;
    bool kbd_ok_ = true;
    uint32_t chat_sig_ = 0, settings_sig_ = 0;                       // what the lists were last built from
    uint32_t seen_epoch_ = 0;                                        // the board change the UI has handled (Model::board_epoch)
    size_t other_boards_n_ = 0;                                      // saved data of boards that are not connected (cached, read from the disk)
    uint64_t other_boards_bytes_ = 0, other_cache_ms_ = 0;

    // chats
    std::vector<ChatRow> chat_rows_;
    std::string chat_sel_key_;
    int chat_sel_ = -1;
    std::string conv_;                   // the conversation shown on the right
    std::string built_conv_;
    uint32_t built_sig_ = 0;

    // contacts
    ContactView cview_;                  // what the user chose: kept while the screen is left and re-entered
    ContactSnapshot csnap_;              // the rows as shown (taps resolve from here)
    bool csnap_valid_ = false;
    uint32_t csnap_rev_ = 0, csnap_minute_ = 0;
    int contact_sel_ = 0;
    std::string contact_sel_key_;
    std::string detail_key_;
    bool view_pending_ = false;          // a sort or filter change waits behind the "Loading..." popup
    ContactView view_next_;
    uint64_t view_ms_ = 0;
    ContactSelection csel_;              // the marked contacts of the select mode
    uint32_t bulk_seen_id_ = 0;          // the bulk delete whose end was handled

    // nearby (C4)
    bool nearby_show_ignored_ = false;
    std::vector<NearbyRow> nearby_rows_;
    int nearby_sel_ = 0;
    std::string nearby_sel_key_;
    uint32_t nearby_sig_ = 0, discover_seen_id_ = 0;

    // search (M7)
    SearchState search_;
    std::vector<SearchRow> search_rows_;
    size_t search_total_ = 0;
    int search_sel_ = 0;
    uint32_t search_sig_ = 0;
    uint32_t focus_seq_ = 0;             // a message found by the search: the conversation is shown around it
    std::string focus_conv_;

    // statistics (D9)
    bool stats_auto_ = true;
    uint64_t stats_ms_ = 0, self_refresh_ms_ = 0;
    uint32_t stats_sig_ = 0;

    // settings
    meshzero::RadioSettings edit_;
    bool dirty_radio_ = false;
    bool holding_ = false;
    uint64_t hold_ms_ = 0;
    std::vector<SRowSpec> srows_;
    int settings_sel_ = -1;
    std::vector<ChannelEntry> settings_channels_;
    ChoiceField choice_field_ = ChoiceField::Bandwidth;

    // editor
    bool editor_open_ = false;
    EditKind edit_kind_ = EditKind::Name;
    EditMode edit_mode_ = EditMode::Free;
    int edit_row_ = 0;
    size_t edit_limit_ = 31;
    uint64_t edit_open_ms_ = 0;
    bool edit_fresh_ = false;            // numbers and the clock: the first typed character replaces the old value
    std::string pending_name_;           // the name typed for a private channel while its key is asked
    bool chain_priv_key_ = false;
    std::vector<std::string> group_keys_;  // contacts to put into the group being created
    std::string group_target_;             // the group being renamed
    int group_focus_ = 0;

    // a new private channel whose key is shown once the board has it
    bool share_pending_ = false;
    std::string share_name_;
    meshzero::ChannelSecret share_key_{};
    uint64_t share_deadline_ = 0;

    // popup
    Popup pop_;
    bool pop_dirty_ = false;             // the widgets of pop_ are built in the next tick
    bool pop_built_ = false;

    // widgets
    lv_obj_t *screen_ = nullptr;
    lv_obj_t *tab_btn_[5] = {};
    lv_obj_t *tab_label_[5] = {};
    lv_obj_t *tab_bar_line_[5] = {};
    lv_obj_t *tab_badge_ = nullptr, *tab_badge_label_ = nullptr;
    lv_obj_t *page_[5] = {};
    lv_obj_t *bar_img_ = nullptr;
    // chats
    RowList chat_list_;
    lv_obj_t *chat_title_ = nullptr, *chat_info_ = nullptr, *msgs_ = nullptr, *compose_ = nullptr;
    lv_obj_t *title_hold_ = nullptr;
    lv_obj_t *send_btn_ = nullptr, *send_label_ = nullptr;
    lv_obj_t *search_btn_ = nullptr, *details_btn_ = nullptr;
    // contacts
    RowList contact_list_;
    lv_obj_t *contacts_count_ = nullptr, *sort_label_ = nullptr;
    lv_obj_t *col_hdr_[6] = {};
    lv_obj_t *type_chip_[5] = {}, *type_chip_label_[5] = {}, *age_chip_[4] = {}, *age_chip_label_[4] = {};
    lv_obj_t *detail_ = nullptr, *detail_title_ = nullptr, *detail_val_[10] = {}, *detail_msg_btn_ = nullptr;
    lv_obj_t *loading_ = nullptr;
    lv_obj_t *tb_normal_ = nullptr, *tb_select_ = nullptr;                 // the two toolbars (normal, select mode)
    lv_obj_t *nearby_btn_ = nullptr, *nearby_btn_label_ = nullptr, *group_btn_label_ = nullptr, *del_btn_ = nullptr, *del_btn_label_ = nullptr, *sel_count_ = nullptr;
    // nearby panel
    lv_obj_t *nearby_ = nullptr, *nearby_title_ = nullptr, *nearby_empty_ = nullptr, *nearby_ign_label_ = nullptr, *nearby_scan_label_ = nullptr;
    RowList nearby_list_;
    // search panel (over the Chats page)
    lv_obj_t *search_panel_ = nullptr, *sr_query_ = nullptr, *sr_conv_label_ = nullptr, *sr_date_label_ = nullptr, *sr_dir_label_ = nullptr, *sr_count_ = nullptr;
    RowList search_list_;
    // statistics panel (over the Settings page)
    lv_obj_t *stats_panel_ = nullptr, *st_label_[2][10] = {}, *st_value_[2][10] = {}, *st_updated_ = nullptr, *st_auto_label_ = nullptr, *st_note_ = nullptr;
    // settings
    RowList settings_list_;
    // popup list and progress
    RowList pop_list_;
    lv_obj_t *pop_bar_ = nullptr, *pop_bar_fill_ = nullptr, *pop_progress_ = nullptr;
    // editor
    lv_obj_t *ed_overlay_ = nullptr, *ed_title_ = nullptr, *ed_hint_ = nullptr, *ed_ta_ = nullptr, *ed_err_ = nullptr;
    // popup
    lv_obj_t *pop_overlay_ = nullptr, *pop_panel_ = nullptr;
    lv_obj_t *pop_value_ = nullptr, *pop_detail_ = nullptr, *pop_pos_ = nullptr, *pop_left_ = nullptr, *pop_right_ = nullptr;
    lv_obj_t *pop_yes_ = nullptr, *pop_no_ = nullptr;
    lv_obj_t *hold_box_ = nullptr, *hold_bar_ = nullptr, *hold_label_ = nullptr;
    std::vector<lv_obj_t *> pop_items_;
    // footer
    lv_obj_t *foot_left_ = nullptr, *foot_right_ = nullptr, *foot_dot_ = nullptr;

    // top bar
    cp0_statusbar_t *bar_ = nullptr;
    std::vector<uint32_t> bar_px_;
    lv_image_dsc_t bar_dsc_{};
};

} // namespace meshhop
