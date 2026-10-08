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

private:
    enum class EditKind { Name, Number, Channel, Clock, PrivName, PrivKey, RandName };

    struct Popup {
        enum Kind { None, Choice, Confirm, Menu, Info } kind = None;
        std::string title, body;
        meshhop::Choice choice;
        std::vector<std::string> items;
        int focus = 0;                   // Confirm: 0 = No, 1 = Yes. Menu: the selected item
        std::string yes_text = "Yes", no_text = "No", ok_text = "OK";
        std::function<void(int)> on_accept;   // Choice: the index. Menu: the item
        std::function<void()> on_yes;
        std::function<void()> on_cancel;
        uint64_t opened_ms = 0;
    };

    // construction
    void load_fonts();
    void build_screen();
    void build_chats();
    void build_contacts();
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
    void send_message();
    void open_detail(const std::string &key_hex);
    void activate_contact(const std::string &key_hex);
    void move_contact_selection(int delta, bool absolute);
    void change_contact_view(const ContactView &next);
    void apply_contact_view();
    void show_loading(bool on);
    void rebuild_contact_snapshot();
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

    // popups
    void open_choice(ChoiceField f);
    void open_confirm(const std::string &title, const std::string &body, std::function<void()> on_yes, const std::string &yes = "Yes",
                      const std::string &no = "No");
    void open_menu(const std::string &title, const std::vector<std::string> &items, std::function<void(int)> on_pick);
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
    lv_obj_t *send_btn_ = nullptr, *mute_btn_ = nullptr, *mute_label_ = nullptr, *send_label_ = nullptr;
    // contacts
    RowList contact_list_;
    lv_obj_t *contacts_count_ = nullptr, *sort_label_ = nullptr;
    lv_obj_t *col_hdr_[6] = {};
    lv_obj_t *type_chip_[5] = {}, *type_chip_label_[5] = {}, *age_chip_[4] = {}, *age_chip_label_[4] = {};
    lv_obj_t *detail_ = nullptr, *detail_title_ = nullptr, *detail_val_[8] = {}, *detail_msg_btn_ = nullptr;
    lv_obj_t *loading_ = nullptr;
    // settings
    RowList settings_list_;
    // editor
    lv_obj_t *ed_overlay_ = nullptr, *ed_title_ = nullptr, *ed_hint_ = nullptr, *ed_ta_ = nullptr, *ed_err_ = nullptr;
    // popup
    lv_obj_t *pop_overlay_ = nullptr, *pop_panel_ = nullptr;
    lv_obj_t *pop_value_ = nullptr, *pop_detail_ = nullptr, *pop_pos_ = nullptr, *pop_left_ = nullptr, *pop_right_ = nullptr;
    lv_obj_t *pop_yes_ = nullptr, *pop_no_ = nullptr;
    std::vector<lv_obj_t *> pop_items_;
    // footer
    lv_obj_t *foot_left_ = nullptr, *foot_right_ = nullptr, *foot_dot_ = nullptr;

    // top bar
    cp0_statusbar_t *bar_ = nullptr;
    std::vector<uint32_t> bar_px_;
    lv_image_dsc_t bar_dsc_{};
};

} // namespace meshhop
