/*
 * SPDX-License-Identifier: MIT
 *
 * RSS Reader: Add a feed. An address field (typed with a keyboard, or with the launcher's on-screen keyboard while none
 * is connected: the shared rule), Find feed (Enter), and the answer: "Found: <title> · N articles" with Pin and Cancel,
 * or why nothing was found in plain words. Finding runs on a worker thread (Refresher::add_custom_feed: the address is
 * fetched; a page's feed is discovered), Pin adds it to the favorites with the articles already fetched
 * (Refresher::pin_custom_feed). With the on-screen keyboard up at 640x480 only the field and Find feed fit above it.
 */
#include "rss_app.hpp"

#include "app_style.hpp"
#include "hooks.hpp"
#include "util.hpp"

#include <algorithm>

using namespace rssreader;

namespace rssui {

namespace {
constexpr std::size_t kMaxAddress = 400;
}

void App::build_add()
{
    lv_obj_t *p = make_page();
    pages_[PageId::Add] = p;
    const Geo g = geo();
    add_ta_ = make_text_field(p, kTagAddField, kMaxAddress, g.row, "example.com, or a feed's address");
    add_find_ = make_button(p, "Find feed", kTagAddFind, true);
    add_status_ = lv_label_create(p);
    lv_label_set_long_mode(add_status_, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_text_font(add_status_, f_body_, 0);
    lv_obj_remove_flag(add_status_, LV_OBJ_FLAG_CLICKABLE);
    lv_label_set_text(add_status_, "");
    add_pin_ = make_button(p, "Pin", kTagAddPin, true);
    add_cancel_ = make_button(p, "Cancel", kTagAddCancel, false);
    layout_add();
}

/* The places of the page's parts for the room there is (the on-screen keyboard shortens it): what does not fit is hidden,
 * never drawn under the keyboard. */
void App::layout_add()
{
    if (!add_ta_) return;
    const Geo g = geo();
    const int find_w = std::max(g.row, text_w("Find feed", f_body_) + 2 * g.m);
    const int y0 = g.s / 2;
    lv_obj_set_pos(add_ta_, g.x, y0);
    lv_obj_set_size(add_ta_, std::max(g.row, g.cw - find_w - g.s), g.row);
    lv_obj_set_pos(add_find_, g.x + g.cw - find_w, y0);
    lv_obj_set_size(add_find_, find_w, g.row);
    const int sy = y0 + g.row + g.m;
    lv_obj_set_pos(add_status_, g.x, sy);
    lv_obj_set_width(add_status_, g.cw);
    lv_obj_update_layout(add_status_);
    const int sh = std::max(line_h(f_body_), static_cast<int>(lv_obj_get_height(add_status_)));
    const int by = sy + sh + g.m;
    const int pin_w = std::max(g.row * 3 / 2, text_w("Pin", f_body_) + 2 * g.m);
    const int cancel_w = std::max(g.row * 3 / 2, text_w("Cancel", f_body_) + 2 * g.m);
    lv_obj_set_pos(add_pin_, g.x, by);
    lv_obj_set_size(add_pin_, pin_w, g.row);
    lv_obj_set_pos(add_cancel_, g.x + pin_w + g.m, by);
    lv_obj_set_size(add_cancel_, cancel_w, g.row);
    const bool status_fits = sy + sh <= panel_h_;
    const bool buttons_fit = by + g.row <= panel_h_;
    set_hidden(add_status_, !status_fits || lv_label_get_text(add_status_)[0] == 0);
    set_hidden(add_pin_, !(add_state_ == AddState::Found && buttons_fit));
    set_hidden(add_cancel_, !(add_state_ == AddState::Found && buttons_fit));
}

void App::add_reset()
{
    if (add_job_) add_job_->cancel.store(true);
    add_job_.reset();
    add_state_ = AddState::Idle;
    add_message_.clear();
    add_found_ = FoundFeed();
    add_typing_ = true;                            // the page is for typing: the field has the keys at once
    add_focus_ = 0;
    lv_textarea_set_text(add_ta_, "");
}

void App::render_add()
{
    set_caption("Type the address of a site or of its feed");
    std::string status;
    std::uint32_t color = kMuted;
    switch (add_state_) {
    case AddState::Idle: status = "A site's address is enough: its feed is found for you."; break;
    case AddState::Working: status = "Looking for a feed\xE2\x80\xA6"; break;
    case AddState::Found:
        status = "Found: " + add_found_.title + "  \xC2\xB7  " + plural(add_found_.article_count, "article", "articles");
        color = kOk;
        break;
    case AddState::Failed:
        status = add_message_;
        color = kWarn;
        break;
    case AddState::Pinned:
        status = add_message_;
        color = kOk;
        break;
    }
    if (status != lv_label_get_text(add_status_)) lv_label_set_text(add_status_, status.c_str());
    set_color(add_status_, color);
    lv_label_set_text(lv_obj_get_child(add_find_, 0), add_state_ == AddState::Working ? "Stop" : "Find feed");
    for (int i = 0; i < 2; ++i) {
        lv_obj_t *b = i == 0 ? add_pin_ : add_cancel_;
        lv_obj_set_style_border_width(b, add_state_ == AddState::Found && add_focus_ == i ? 3 : 0, 0);
    }
    layout_add();
}

void App::add_find()
{
    if (add_state_ == AddState::Working) {             // Stop
        if (add_job_) add_job_->cancel.store(true);
        return;
    }
    const std::string text = trim(lv_textarea_get_text(add_ta_));
    if (text.empty()) {
        add_state_ = AddState::Failed;
        add_message_ = "Type an address first.";
        invalidate();
        return;
    }
    add_state_ = AddState::Working;
    add_typing_ = false;                               // the keyboard goes: the answer shows under the field
    HttpClient *http = http_.get();
    Store *store = &store_;
    add_job_ = jobs_.start([this, text, http, store](Jobs::Token &tok) {
        Refresher r(*store, *http, [] { return now_epoch(); });
        auto res = std::make_shared<AddResult>(r.add_custom_feed(text, &tok.cancel));
        jobs_.post([this, res] {
            add_job_.reset();
            if (res->status == AddResult::Status::Ok) {
                add_found_ = res->found;
                add_state_ = AddState::Found;
                add_focus_ = 0;
            } else {
                add_state_ = AddState::Failed;
                add_message_ = add_failure_text(*res);
            }
            invalidate();
        });
    });
    invalidate();
}

void App::add_pin()
{
    if (add_state_ != AddState::Found) return;
    Refresher r(store_, *http_, [] { return now_epoch(); });
    const StoreError e = r.pin_custom_feed(add_found_);
    if (e == StoreError::Ok) {
        add_state_ = AddState::Pinned;
        add_message_ = "Added to your favorites: " + add_found_.title;
        notice("Pinned " + add_found_.title);
        lv_textarea_set_text(add_ta_, "");
        add_typing_ = false;                       // the keyboard goes: the answer shows; a tap on the field (or a key) types again
    } else {
        add_state_ = AddState::Failed;
        add_message_ = e == StoreError::Duplicate ? "That feed is already in your favorites." : "Could not save the feed (is the storage full?).";
    }
    invalidate();
}

void App::key_add(const KeyEvent &e)
{
    if (add_state_ == AddState::Working) {
        if (e.key == Key::Enter && !e.repeat) add_find();       // Enter on "Stop"
        return;
    }
    if (add_state_ == AddState::Found) {
        switch (e.key) {
        case Key::Left:
        case Key::Right:
        case Key::Tab:
        case Key::BackTab:
            add_focus_ = 1 - add_focus_;
            invalidate();
            return;
        case Key::Enter:
            if (e.repeat) return;
            if (add_focus_ == 0) add_pin();
            else button_clicked(kTagAddCancel);
            return;
        default:
            if (!is_typing(e) && e.key != Key::Backspace) return;
            add_state_ = AddState::Idle;                         // typing again: back to the address
            break;
        }
    }
    if (e.key == Key::Enter) {
        if (!e.repeat) add_find();
        return;
    }
    if (is_typing(e) || e.key == Key::Backspace || e.key == Key::Delete || e.key == Key::Left || e.key == Key::Right ||
        e.key == Key::Home || e.key == Key::End) {
        add_typing_ = true;
        if (add_state_ == AddState::Failed || add_state_ == AddState::Pinned) add_state_ = AddState::Idle;
        field_key(add_ta_, e, kMaxAddress);
        invalidate();
    }
}

} // namespace rssui
