/*
 * SPDX-License-Identifier: MIT
 *
 * Mesh Hop: the full-screen MeshCore messenger. The shell (page header with the Esc button and the status strip, the
 * caption line with the board state and the notices, the key hint line, the dialogs) is built from the launcher's widgets
 * (cp0::ui::Page, Dialog) and follows the screen. So do the pages of phase P3 (Contacts with a contact's details and
 * Nearby, Settings with the statistics and the packet log, the message search): they fill the width of the screen with the
 * margins of the page column (ui_pages.hpp page_column), every size from the layout service's tokens. Only Chats is still
 * laid out for a 640 px wide column, the stage (stage.hpp), centred under the shell (phase P4). Written with LVGL over the
 * core/ layer (client, model, store). Input comes from the Platform as KeyEvents and LVGL pointer events; nothing here
 * talks to the radio except through meshzero::Client.
 */

#pragma once

#include "client.hpp"
#include "deck_clock.hpp"
#include "log.hpp"
#include "model.hpp"
#include "preset_feed.hpp"
#include "serial_transport.hpp"
#include "stage.hpp"
#include "store.hpp"
#include "ui_logic.hpp"
#include "ui_pages.hpp"

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "cp0_ui_widgets.hpp"

namespace meshhop {

struct Cell {
    std::string text;
    uint32_t color = 0xF4F4F5;
    int x = 0, w = 100;
    lv_text_align_t align = LV_TEXT_ALIGN_LEFT;
    const lv_font_t *font = nullptr;
    int y = 0;
    uint32_t box = 0;                    // not 0: drawn as a button (a rounded box of this colour) the size of the cell
    int lines = 1;                       // more than 1: the text wraps over that many lines (cut with dots otherwise)
};

struct RowSpec {
    int h = 40;
    bool selectable = true;
    bool header = false;
    int badge = 0;                       // unread count (0: none)
    std::string id;                      // what the row stands for (a contact key, a conversation, a settings row): taps resolve it from here
    std::vector<Cell> cells;
    Aff aff = Aff::None;                 // a list with markers (Config::marks): the chevron (Open, Edit) or the dot (Action, Running) at the right
};

/* A scrollable list of rows (touch: drag to scroll, tap a row; keys: the selection). It is virtual: the data of every row is kept,
 * but only the rows in view (plus a margin) exist as widgets, in a small pool that is re-used while scrolling. A key press that moves
 * the selection only re-binds two rows, so 161 contacts move as fast as 10. With `flat` it looks like the launcher's CompactList (the
 * same highlight bar, no separators, no accent bar) and, with `marks`, draws the row kinds' markers of cp0_ui_list.h (a chevron or a
 * dot in the slot at the right edge of mark_layout's inset) from RowSpec::aff: a list of rows of any height and any cells that reads
 * like a CompactList. */
class RowList {
public:
    struct Config {
        int max_cells = 6;               // the most cells of a row
        int max_boxes = 0;               // the most boxed (button-like) cells of a row
        int min_row_h = 40;              // the smallest row height: sizes the pool
        bool flat = false;               // the CompactList look
        bool marks = false;              // draw RowSpec::aff with mark_layout's sizes (cp0_ui_list_layout of this list's area)
        cp0_ui_list_layout_t mark_layout{};
    };
    void create(lv_obj_t *parent, int x, int y, int w, int h, const Config &cfg);
    /* A new height (the room under the shell changed). The pool of widgets was sized at create() for its h. */
    void resize(int h);
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
    int width() const { return w_; }
    void set_y(int y);
    void scroll_by(int dy);              // a read-only list: the keys scroll it

private:
    struct Slot {
        lv_obj_t *row = nullptr, *bar = nullptr, *pill = nullptr, *pill_label = nullptr;
        lv_obj_t *chevron = nullptr, *dot = nullptr;
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
    int chev_ink_top_ = 0, chev_ink_h_ = 0, chev_line_h_ = 0;
};

/* A short row of large cells above a list (Net Toolbox's ActionBar): each cell is an action, a tap acts at once (the cells are the
 * ROW token high, the launcher's rule for a control that acts: 7 mm); a second, smaller line under the label (a value) is optional.
 * The keys of the page reach the actions through their letters. The cells' click events carry the App's button tags. */
class ActionBar {
public:
    struct CellSpec {
        int tag = 0;
        std::string label, value;
    };
    void create(lv_obj_t *parent, int x, int y, int w, int h, const lv_font_t *label_font, const lv_font_t *value_font);
    /* New cells: rebuilt when the tags changed, else only the texts that changed. Never from inside a cell's own event. */
    void set_cells(const std::vector<CellSpec> &cells);
    void set_y(int y);
    lv_obj_t *root() const { return root_; }
    int height() const { return h_; }

private:
    struct Cell {
        CellSpec spec;
        lv_obj_t *obj = nullptr, *label = nullptr, *value = nullptr;
    };
    void place(Cell &c, int index, int n);

    lv_obj_t *root_ = nullptr;
    std::vector<Cell> cells_;
    const lv_font_t *label_font_ = nullptr, *value_font_ = nullptr;
    int w_ = 0, h_ = 0;
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
    /* A short Esc: Back. Never quits (the header's Esc button asks "Quit Mesh Hop?" at the root: header_back()). */
    void back();

    // for the scripted tests and the screenshots
    Tab tab() const { return nav_.tab; }
    meshzero::Client &client() { return *client_; }
    meshzero::Model &model() { return model_; }
    void select_tab(Tab t);
    /* The screen place of the (0, 0) of the 640x480 layout the scripted flows (tests/flows) give their coordinates in:
     * the page bodies sit 56 px under the top of that layout. */
    lv_point_t script_origin() const;
    /* One line with the state of the app (scripted tests). */
    std::string debug_state() const;
    /* The containers of the page bodies (the stage, the full width, Home's list) and the key hint line (scripted layout checks:
     * nothing of a body reaches under the hints). */
    void body_lists(std::vector<lv_obj_t *> &roots, lv_obj_t *&hint) const;

    // LVGL callbacks (public for the static trampolines)
    void button_clicked(int tag);
    void row_clicked(RowList *list, int index, int x);
    /* A row of a menu or a pick list in a dialog was chosen by a tap. */
    void popup_pick(int index);
    /* A press on a name that can be held for 3 s (header title or a row of the chat list): what = 0 pressed, 1 moved, 2 ended. */
    void hold_event(int what, RowList *list, int index, int x, int y);

private:
    enum class EditKind { Name, Number, Channel, Clock, PrivName, PrivKey, RandName, GroupNew, GroupRename, DateRange, Position };

    struct Popup {
        enum Kind { None, Choice, Confirm, Menu, Info, List, Progress } kind = None;
        std::string title, body;
        meshhop::Choice choice;
        std::vector<std::string> items;
        std::vector<std::string> values;     // Menu, List: a value at the right of the item (grey), "" for none
        std::vector<Aff> affs;               // Menu: the marker of each item (default: the action dot); List: none
        int focus = 0;                   // Confirm: 0 = No, 1 = Yes. Menu, List: the selected item
        std::string yes_text = "Yes", no_text = "No", ok_text = "OK";
        std::function<void(int)> on_accept;   // Choice: the index. Menu, List: the item
        std::function<void()> on_yes;
        std::function<void()> on_cancel;
        uint64_t opened_ms = 0;
        uint32_t yes_delay_ms = 0;       // Confirm: the Yes button only works after this long (the second box of a factory reset)
        bool danger = false;             // Confirm: the Yes button is red (it erases something)
    };

    /* The page column and the tokens the pages are laid out with (page_column in ui_pages.hpp): x / cw the edge of the texts
     * and of the buttons, lx / lw a list's area (the column and SPACE_L on each side), h the room under the caption line. */
    struct Geo {
        int w = 0, x = 0, cw = 0, lx = 0, lw = 0, inset = 0, s = 0, m = 0, h = 0;
        int row = 0, list_row = 0, target = 0;
        int bar = 0;                     // an action bar: ROW, or more when a label and a value line need it (a low density panel)
        int field = 0;                   // a one-line field: LIST_ROW, or more for the BODY text
        bool wide = false;               // the Wide and Large classes (800x480, 1024x600, 1280x720): more on a line
    };
    Geo geo() const;

    // construction
    void load_fonts();
    void build_screen();
    void build_chats();
    void build_contacts();
    void build_nearby();
    void build_search();
    void build_stats();
    void build_packet_log();
    void build_settings();
    void build_placeholder(lv_obj_t **page, const char *title, const char *text);
    void build_popup();
    void build_hint_line();
    void build_home();
    void render_home();
    void key_home(const KeyEvent &e);
    void open_page(Tab t);
    void go_home();
    void build_stage();
    void build_shell();
    void layout_body();                  // sizes and places what depends on the height under the shell (page_h_, panel_h_)
    void layout_wide();                  // ... for the pages of the full width
    cp0_ui_list_layout_t settings_list_layout(int y, int h) const;
    void update_title();
    void update_caption();
    void header_back();                  // the Esc button of the header: Back, and at the root the Quit question
    void ask_quit();
    void post(std::function<void()> fn);
    void run_posted();
    lv_obj_t *make_button(lv_obj_t *parent, int x, int y, int w, int h, const char *text, int tag, lv_obj_t **label_out = nullptr);
    lv_obj_t *make_label(lv_obj_t *parent, const lv_font_t *font, uint32_t color, int x, int y, int w, int h,
                         lv_text_align_t align = LV_TEXT_ALIGN_LEFT);

    // rendering
    void render();
    void render_chats();
    void render_chat_messages(bool force);
    void render_contacts();
    void render_contacts_bar();
    void render_detail();
    void render_nearby();
    void render_search();
    void render_stats();
    void render_packet_log();
    void render_settings();
    void render_footer();                // the caption line and the key hints
    bool page_caption(std::string &text) const;   // the grey line under the title a page shows instead of the board line (false: none)
    std::string hint_text() const;
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
    void settings_activate(int index);
    void settings_select(int index, bool scroll);
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
    void nearby_activate(int index);
    void nearby_add_selected();
    void open_filter_dialog();
    void open_sort_dialog();
    void open_more_menu();
    void open_send_advert();             // More > Send advert: the arrow choice of the scope, OK sends
    void send_advert(bool flood);        // (and remembers the scope for the next choice)
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
    // phase 3: the packet log (D8)
    void open_packet_log();
    void close_packet_log();
    void key_packet_log(const KeyEvent &e);
    void packet_log_toggle();
    void packet_log_clear();
    void packet_log_select(int64_t abs_index);

    // popups
    void open_choice(ChoiceField f);
    void open_confirm(const std::string &title, const std::string &body, std::function<void()> on_yes, const std::string &yes = "Yes",
                      const std::string &no = "No", uint32_t yes_delay_ms = 0, bool danger = false);
    void open_menu(const std::string &title, const std::vector<std::string> &items, std::function<void(int)> on_pick,
                   const std::vector<std::string> &values = {}, const std::vector<Aff> &affs = {});
    void open_list(const std::string &title, const std::vector<std::string> &items, int focus, std::function<void(int)> on_pick,
                   const std::vector<std::string> &values = {}, const std::vector<Aff> &affs = {});
    void add_scroll_cue(cp0::ui::Dialog &d);   // a dialog's body taller than its room: the list scrollbar at its right
    void open_progress(const std::string &title);
    lv_obj_t *dialog_button(cp0::ui::Dialog &d, size_t index) const;
    lv_obj_t *dialog_label(cp0::ui::Dialog &d, const std::string &text, bool small, uint32_t color);
    lv_obj_t *dialog_row(cp0::ui::Dialog &d, const std::string &text, const std::string &value, Aff aff, int tag);
    lv_obj_t *make_field(lv_obj_t *parent, int tag, size_t max_len);
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
    // the Position box (D6): an editor with two fields (latitude, longitude), the board GPS switch and Clear. It counts as the open
    // editor (editor_open_, edit_kind_ == EditKind::Position), so every guard of the editor holds for it too.
    void open_position_box();
    void close_position_box(bool apply);
    void position_clear();
    void position_key(const KeyEvent &e);
    void position_layout(bool gps);
    void position_refresh();
    void position_focus(int focus, bool scroll = true);
    void position_error(const std::string &text, bool error);

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

    // fonts: the page bodies (the stage) keep their own sizes; the shell and the dialogs take the token sizes
    const lv_font_t *f_small_ = nullptr, *f_ui_ = nullptr, *f_big_ = nullptr;     // Montserrat 14 / 16 / 18
    const lv_font_t *f_value_small_ = nullptr;                                    // Montserrat 22: the "Loading..." box
    const lv_font_t *f_text_ = nullptr, *f_text_small_ = nullptr;                 // message text (DejaVu when available)
    const lv_font_t *f_body_ = nullptr, *f_caption_ = nullptr, *f_title_ = nullptr; // the BODY / CAPTION / TITLE text sizes (tokens)
    const lv_font_t *f_field_ = nullptr;                                          // the text of a dialog's field (DejaVu at the BODY size when available)
    const lv_font_t *f_value_ = nullptr;                                          // 90 % of BODY: the values and the tables of the pages
    const lv_font_t *f_mono_ = nullptr;                                           // the packet bytes: DejaVu at the CAPTION size when available
    int label_px_ = 0, value_px_ = 0;                                             // the labels and values of Settings and Statistics
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

    // a contact's details
    int detail_sel_ = 0;
    uint32_t detail_sig_ = 0;

    // statistics (D9)
    bool stats_auto_ = true;
    uint32_t stats_rows_sig_ = 0;
    int stats_at_ = 0;                   // the row the keys scrolled the figures to
    uint64_t stats_ms_ = 0, self_refresh_ms_ = 0;
    uint32_t stats_sig_ = 0;

    // the packet log (D8): the rows are the client's ring, oldest first; the selection is kept as an absolute number (dropped + index)
    // so that it stays on the same packet while the oldest ones fall out. -1 = follow the newest.
    int64_t log_sel_abs_ = -1;
    uint32_t log_rev_ = 0;
    uint64_t log_built_sel_ = ~0ull;

    // settings
    meshzero::RadioSettings edit_;
    bool dirty_radio_ = false;
    bool holding_ = false;
    uint64_t hold_ms_ = 0;
    std::vector<SRowSpec> srows_;
    int settings_sel_ = -1;
    uint32_t settings_builds_ = 0, contacts_builds_ = 0;
    bool last_advert_flood_ = true;      // the scope of the last advert sent by hand: where Send advert starts   // the lists built again (scripted tests: a refresh happened)
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
    // the Position box
    enum PosFocus { kPosLat = 0, kPosLon, kPosGps, kPosClear };
    int pos_focus_ = kPosLat;
    std::string pos_err_text_;             // the status line of the Position box (shown by the dialog's message line)
    bool pos_fresh_[2] = {false, false};   // the prefilled value is replaced by the first typed character (like the number editor)
    bool pos_gps_shown_ = false;

    // a new private channel whose key is shown once the board has it
    bool share_pending_ = false;
    std::string share_name_;
    meshzero::ChannelSecret share_key_{};
    uint64_t share_deadline_ = 0;

    // popup
    Popup pop_;
    bool pop_dirty_ = false;             // the widgets of pop_ are built in the next tick
    bool pop_built_ = false;

    // the shell: the page (header, caption), the tab row, the key hint line, the stage with the page bodies
    cp0_ui_roller_page_t layout_{};
    std::unique_ptr<cp0::ui::Page> shell_;
    int tabs_y_ = 0, tab_h_ = 0, hint_h_ = 0, stage_x_ = 0;   // tabs_y_: the top of the stage and of Home, under the caption line (no tab row: tab_h_ is 0)
    bool at_home_ = true;                // Home: the main menu (Chats, Contacts, Settings); a page is open otherwise
    std::unique_ptr<cp0::ui::CompactList> home_list_;
    lv_obj_t *wide_ = nullptr;           // the pages of the full width (P3), under the caption line, as high as the stage
    lv_obj_t *cap_alt_ = nullptr;        // a page's own grey line in the caption's place (the filter summary of Contacts...)
    int panel_h_ = 400, page_h_ = 320;   // the height of a sub-page (it covers the tab row) and of a main page, in the stage
    std::string title_key_, cap_text_;
    int cap_tone_ = -1;
    std::vector<std::function<void()>> posted_;
    lv_obj_t *hint_box_ = nullptr, *hint_label_ = nullptr;

    // widgets
    lv_obj_t *screen_ = nullptr;          // the stage
    lv_obj_t *page_[5] = {};
    // chats
    RowList chat_list_;
    lv_obj_t *chat_title_ = nullptr, *chat_info_ = nullptr, *msgs_ = nullptr, *compose_ = nullptr;
    lv_obj_t *title_hold_ = nullptr, *chat_div_ = nullptr, *chat_line2_ = nullptr;
    lv_obj_t *send_btn_ = nullptr, *send_label_ = nullptr;
    lv_obj_t *search_btn_ = nullptr, *details_btn_ = nullptr, *options_btn_ = nullptr;
    // contacts: the action bar (Filter, Sort, More; Done, Select..., Delete, Group... in the select mode) and the two-line rows
    ActionBar contacts_bar_;
    RowList contact_list_;
    lv_obj_t *loading_ = nullptr;
    // a contact's details: its actions (Message, Groups, Delete) and its facts
    lv_obj_t *detail_ = nullptr;
    ActionBar detail_bar_;
    RowList detail_list_;
    // nearby panel
    lv_obj_t *nearby_ = nullptr, *nearby_empty_ = nullptr;
    ActionBar nearby_bar_;
    RowList nearby_list_;
    // search panel (over the Chats page)
    lv_obj_t *search_panel_ = nullptr, *sr_query_ = nullptr;
    ActionBar search_bar_;
    RowList search_list_;
    // statistics panel (over the Settings page)
    lv_obj_t *stats_panel_ = nullptr;
    ActionBar stats_bar_;
    std::unique_ptr<cp0::ui::CompactList> stats_list_;
    // packet log panel (over the Settings page): the table (its columns chosen by the width) and the selected packet's bytes,
    // under it, or at its right on the Wide and Large screens
    lv_obj_t *log_panel_ = nullptr, *log_head_ = nullptr, *log_detail_ = nullptr, *log_hex_ = nullptr, *log_div_ = nullptr;
    ActionBar log_bar_;
    RowList log_list_;
    std::vector<int> log_cols_, log_col_x_, log_col_w_;       // the packet columns shown (PacketRow fields 0..7), their x and width
    bool log_side_ = false;                                    // the bytes at the right of the table
    // settings
    RowList settings_list_;              // the virtual list in the CompactList look: notes that wrap, read-outs the cursor skips
    // dialogs (cp0::ui::Dialog): the popups, and the editor / Position box
    std::unique_ptr<cp0::ui::Dialog> dialog_, ed_dialog_;
    // popup
    lv_obj_t *pop_bar_ = nullptr, *pop_bar_fill_ = nullptr, *pop_progress_ = nullptr;
    lv_obj_t *pop_value_ = nullptr, *pop_detail_ = nullptr, *pop_pos_ = nullptr, *pop_left_ = nullptr, *pop_right_ = nullptr;
    lv_obj_t *pop_yes_ = nullptr, *pop_no_ = nullptr;
    std::vector<lv_obj_t *> pop_items_;
    int pop_bar_w_ = 0;
    // editor
    lv_obj_t *ed_ta_ = nullptr;
    // the Position box
    lv_obj_t *pos_now_ = nullptr, *pos_gps_ = nullptr, *pos_gps_label_ = nullptr;
    lv_obj_t *pos_label_[2] = {}, *pos_ta_[2] = {}, *pos_hint_ = nullptr;
    lv_obj_t *pos_clear_ = nullptr;
    lv_obj_t *hold_box_ = nullptr, *hold_bar_ = nullptr, *hold_label_ = nullptr;
    lv_obj_t *loading_label_ = nullptr;
};

} // namespace meshhop
