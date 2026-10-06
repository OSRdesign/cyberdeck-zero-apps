/*
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "client.hpp"
#include "deck_clock.hpp"
#include "model.hpp"
#include "serial_transport.hpp"
#include "sha256.hpp"
#include "input_state.hpp"
#include "store.hpp"
#include "ui_app_page.hpp"

#include <array>
#include <memory>
#include <string>
#include <vector>

/* True once the page asked to quit (Esc on a top-level screen); main() polls it through should_quit. */
bool meshzero_quit_requested();

// MeshZero messenger: Status, Chats, Contacts and Settings tabs, a Chat view (direct or channel), a Contact
// detail view and a text editor (message, node name, frequency) with an on-screen keyboard.
class UIMeshZeroPage : public AppPage
{
public:
    UIMeshZeroPage();
    ~UIMeshZeroPage() override;

private:
    enum class View { Home, Chats, Contacts, Settings, Chat, Detail, Edit };
    enum class EditKind { Message, Name, Number, Channel, Clock };
    struct Item {
        std::string mark, left, mid, right;
        uint32_t left_color = 0, mid_color = 0, right_color = 0, mark_color = 0;
    };
    static constexpr int kRows = 6;
    static constexpr int kSettingsRows = 11;

    // construction
    void build_header();
    void build_home();
    void build_list();
    void build_chat();
    void build_detail();
    void build_edit();

    // state
    void set_view(View v);
    bool is_tab(View v) const { return v == View::Home || v == View::Chats || v == View::Contacts || v == View::Settings; }
    void next_tab(int delta);
    void tick();
    void refresh();
    void back();
    void activate();
    void move_selection(int delta);
    void open_chat(const std::string &conv);
    void open_detail(const std::string &key_hex);
    void start_edit(EditKind kind, const std::string &initial, const std::string &title);
    void finish_edit(bool accept);
    void enter_text_mode();
    void leave_text_mode();

    // list content
    void build_items();
    void render_list();
    void render_home();
    void render_chat();
    void render_detail();
    void render_footer();
    void settings_adjust(int row, int delta);
    void settings_activate(int row);
    int settings_row_count() const;
    void send_current_edit();
    void chat_scroll(int dy);
    void request_remove();
    bool keyboard_ready();
    void notice(const std::string &text, bool ok);
    bool conv_removable() const;
    void update_edit_title();
    std::string target_title() const;

    // input
    void handle_key(lv_event_t *event);
    void handle_raw_key(lv_event_t *event);
    void handle_text_key(const struct key_item *item);
    static void key_event_cb(lv_event_t *event);
    static void keyboard_event_cb(lv_event_t *event);
    static void row_event_cb(lv_event_t *event);
    static void button_event_cb(lv_event_t *event);
    static void timer_cb(lv_timer_t *timer);

    // core
    meshzero::Model model_;
    std::unique_ptr<meshzero::Store> store_;
    meshzero::SerialTransport serial_;
    meshzero::DeckClockProbe deck_clock_;      // before client_: the client's callbacks use it
    std::unique_ptr<meshzero::Client> client_;

    View view_ = View::Home;
    View tab_ = View::Home;
    std::string conv_;                 // the open conversation
    std::string detail_key_;           // the contact shown in Detail
    EditKind edit_kind_ = EditKind::Message;
    int num_row_ = 0;                  // Settings row being typed (1 frequency, 2 bandwidth, 3 SF, 4 CR, 5 TX power)
    std::string edit_title_;
    uint32_t last_nav_tick_ = 0;       // a tap may also arrive as the launcher's tap=Enter gesture: act once
    uint32_t kbd_check_tick_ = 0;
    uint32_t edit_open_tick_ = 0;      // typed editors ignore an Enter that arrives right after they opened
    uint32_t prompt_tick_ = 0;
    bool kbd_present_ = true;
    meshzero::RadioSettings edit_;     // settings being edited
    bool dirty_ = false;
    uint32_t hold_tick_ = 0;           // keep the edited values on screen until the radio confirmed
    bool holding_ = false;
    std::vector<Item> items_;
    std::vector<std::string> item_keys_;       // conversation key / contact key per list row
    int selected_ = 0, offset_ = 0;
    uint32_t last_revision_ = 0, last_link_ = 0xFFFF, last_notice_ = 0, last_tick_ = 0, notice_tick_ = 0;
    std::string notice_text_;
    bool notice_ok_ = true;
    uint32_t start_tick_ = 0;
    int press_y_ = 0;
    bool swiped_ = false, valid_press_ = false;
    float press_fx_ = 0;
    bool text_mode_ = false;
    int saved_context_ = 0, saved_intercept_ = 0;
    size_t edit_limit_ = 133;

    // widgets
    lv_obj_t *root_ = nullptr;
    std::array<lv_obj_t *, 4> tab_box_{};
    std::array<lv_obj_t *, 4> tab_label_{};
    lv_obj_t *status_ = nullptr;
    lv_obj_t *title_ = nullptr;
    lv_obj_t *remove_btn_ = nullptr, *remove_label_ = nullptr;
    uint32_t remove_armed_ = 0;
    bool remove_pending_ = false;
    lv_obj_t *back_label_ = nullptr;
    lv_obj_t *back_btn_ = nullptr, *action_btn_ = nullptr, *action_label_ = nullptr;
    lv_obj_t *footer_ = nullptr;

    lv_obj_t *home_box_ = nullptr;
    std::array<lv_obj_t *, 7> home_key_{}, home_val_{};
    lv_obj_t *advert_flood_ = nullptr, *advert_direct_ = nullptr;

    lv_obj_t *list_box_ = nullptr;
    std::array<lv_obj_t *, 3> col_hdr_{};
    std::array<lv_obj_t *, kRows> rows_{}, row_mark_{}, row_left_{}, row_mid_{}, row_right_{};
    lv_obj_t *empty_msg_ = nullptr;

    lv_obj_t *chat_box_ = nullptr;
    uint32_t chat_seq_ = 0;            // revision the chat box was built for
    std::string chat_conv_built_;

    lv_obj_t *det_box_ = nullptr;
    std::array<lv_obj_t *, 7> det_key_{}, det_val_{};

    lv_obj_t *edit_box_ = nullptr;
    lv_obj_t *edit_ta_ = nullptr;

    lv_event_dsc_t *keyboard_dsc_ = nullptr;
    lv_obj_t *keyboard_root_ = nullptr;
    lv_timer_t *timer_ = nullptr;
};
