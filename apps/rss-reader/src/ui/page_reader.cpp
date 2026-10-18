/*
 * SPDX-License-Identifier: MIT
 *
 * RSS Reader: the reader (phase 2, minimal). The article's title, then its text as html_to_blocks gives it: headings,
 * paragraphs, list items, quotes (a bar at the left), preformatted text, rules, and each image as a one-line
 * "[image: alt]" placeholder (phase 3 decodes and shows them, with a full-screen view and Save for offline). The text
 * column takes the whole width of the screen less a SPACE_L margin on each side; the text size is the setting's. It
 * scrolls by a drag, the wheel (the toolkit's pointer navigation gives the arrows), Up / Down, Page Up / Page Down,
 * Home / End; Left / Right go to the previous / next article of the list. Opening an article marks it read.
 */
#include "rss_app.hpp"

#include "app_style.hpp"
#include "hooks.hpp"
#include "html_text.hpp"
#include "util.hpp"

#include "cp0_ui_metrics_lvgl.h"

#include <algorithm>
#include <cmath>

using namespace rssreader;

namespace rssui {

namespace {

void reader_event_cb(lv_event_t *e)
{
    auto *self = static_cast<App *>(lv_event_get_user_data(e));
    if (!self) return;
    lv_point_t p = {0, 0};
    if (lv_indev_t *indev = lv_event_get_indev(e)) lv_indev_get_point(indev, &p);
    switch (lv_event_get_code(e)) {
    case LV_EVENT_PRESSED: self->reader_pressed(0, p.x, p.y); break;
    case LV_EVENT_PRESSING: self->reader_pressed(1, p.x, p.y); break;
    case LV_EVENT_RELEASED:
    case LV_EVENT_PRESS_LOST: self->reader_pressed(2, p.x, p.y); break;
    default: break;
    }
}

} // namespace

void App::build_reader()
{
    lv_obj_t *p = make_page();
    pages_[PageId::Reader] = p;
    const Geo g = geo();
    reader_ = lv_obj_create(p);
    lv_obj_remove_style_all(reader_);
    lv_obj_set_pos(reader_, 0, 0);
    lv_obj_set_size(reader_, g.w, panel_h_);
    lv_obj_set_scroll_dir(reader_, LV_DIR_VER);
    lv_obj_remove_flag(reader_, LV_OBJ_FLAG_SCROLL_ELASTIC);
    lv_obj_remove_flag(reader_, LV_OBJ_FLAG_CLICK_FOCUSABLE);
    lv_obj_add_flag(reader_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_flex_flow(reader_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_hor(reader_, g.inset, 0);
    lv_obj_set_style_pad_top(reader_, g.s / 2, 0);
    lv_obj_set_style_pad_bottom(reader_, g.m * 2, 0);
    style_list_scrollbar(reader_);
    for (lv_event_code_t code : {LV_EVENT_PRESSED, LV_EVENT_PRESSING, LV_EVENT_RELEASED, LV_EVENT_PRESS_LOST})
        lv_obj_add_event_cb(reader_, reader_event_cb, code, this);
}

int App::reader_px() const
{
    const cp0_ui_metrics_t *m = cp0_ui_metrics_get();
    return reader_text_px(const_cast<rssreader::Store &>(store_).settings().text_size, m->text_px[CP0_TEXT_CAPTION], m->text_px[CP0_TEXT_BODY],
                          m->text_px[CP0_TEXT_HEADING], m->text_px[CP0_TEXT_TITLE], [](int px) { return cp0_ui_roller_font_px(px); });
}

void App::open_reader(ReaderSource src, int index)
{
    reader_src_ = std::move(src);
    reader_idx_ = -1;
    open(PageId::Reader);
    show_article(index);
}

/* The article at `index` of the source: shown from the top, marked read. */
void App::show_article(int index)
{
    if (index < 0 || index >= reader_src_.count()) return;
    reader_idx_ = index;
    if (reader_src_.saved) {
        const SavedSummary &s = reader_src_.saved_list[static_cast<std::size_t>(index)];
        SavedArticle sa;
        if (store_.load_saved(s.id, sa)) {
            reader_art_ = sa.article;
            reader_feed_name_ = sa.feed_name;
            Subscription sub;
            if (store_.find_subscription(sa.feed_id, sub)) store_.set_read(sa.feed_id, sa.article.id, true);
        } else {
            reader_art_ = Article();
            reader_art_.title = s.title;
            reader_feed_name_ = s.feed_name;
        }
    } else {
        const StoredArticle &a = (*reader_src_.arts)[static_cast<std::size_t>(index)];
        reader_art_ = a.article;
        reader_feed_name_ = reader_src_.feed_name;
        store_.set_read(reader_src_.feed_id, a.article.id, true);
        read_marks_.insert(reader_src_.feed_id + "\n" + a.article.id);
        // the list's copy (the same snapshot unless a refresh replaced it)
        if (reader_src_.feed_id == art_feed_.id)
            for (StoredArticle &x : *arts_)
                if (x.article.id == a.article.id) x.read = true;
    }
    render_reader_text();
    lv_obj_scroll_to_y(reader_, 0, LV_ANIM_OFF);
    invalidate();
}

/* The text of the article as labels in the reader's column (rebuilt for a new article or a new text size). */
void App::render_reader_text()
{
    lv_obj_clean(reader_);
    const Geo g = geo();
    const int px = reader_px();
    reader_built_px_ = px;
    const int tw = std::max(10, g.w - 2 * g.inset);
    auto snap = [](int v) { return cp0_ui_roller_font_px(v); };
    const lv_font_t *f_text = font("Montserrat-Medium.ttf", px);
    const lv_font_t *f_bold = font("Montserrat-Bold.ttf", px);
    const lv_font_t *f_h1 = font("Montserrat-Bold.ttf", snap(px * 5 / 4));
    const lv_font_t *f_title = font("Montserrat-Bold.ttf", snap(px * 4 / 3));
    const lv_font_t *f_small = font("Montserrat-Medium.ttf", snap(px * 5 / 6));
    const lv_font_t *f_mono = font("DejaVuSansMono.ttf", snap(px * 5 / 6));
    const int line_space = std::max(2, px / 4);
    lv_obj_set_style_pad_row(reader_, std::max(g.s, px * 2 / 3), 0);

    auto label = [&](lv_obj_t *parent, const std::string &text, const lv_font_t *f, std::uint32_t color, int width) {
        lv_obj_t *l = lv_label_create(parent);
        lv_obj_remove_flag(l, LV_OBJ_FLAG_CLICKABLE);
        lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_WRAP);
        lv_obj_set_width(l, width);
        lv_obj_set_style_text_font(l, f, 0);
        lv_obj_set_style_text_line_space(l, line_space, 0);
        set_color(l, color);
        lv_label_set_text(l, text.c_str());
        return l;
    };

    label(reader_, reader_art_.title.empty() ? std::string("(no title)") : reader_art_.title, f_title, kText, tw);
    const std::vector<Block> blocks = html_to_blocks(reader_art_.body_html(), reader_art_.link);
    if (blocks.empty()) label(reader_, "This feed gives no text for this article.", f_text, kMuted, tw);
    std::size_t shown = 0;
    for (const Block &b : blocks) {
        if (shown++ >= kReaderMaxBlocks) {
            label(reader_, "The rest of this article is not shown.", f_small, kMuted, tw);
            break;
        }
        switch (b.type) {
        case BlockType::Heading: label(reader_, b.text, b.level <= 2 ? f_h1 : f_bold, kText, tw); break;
        case BlockType::Paragraph: label(reader_, b.text, f_text, kText, tw); break;
        case BlockType::ListItem: {
            const int indent = std::max(0, b.level - 1) * g.m + g.s;
            lv_obj_t *l = label(reader_, block_text(b), f_text, kText, std::max(10, tw - indent));
            lv_obj_set_style_margin_left(l, indent, 0);
            break;
        }
        case BlockType::Quote: {
            lv_obj_t *q = lv_obj_create(reader_);
            lv_obj_remove_style_all(q);
            lv_obj_remove_flag(q, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_remove_flag(q, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_set_size(q, tw, LV_SIZE_CONTENT);
            lv_obj_set_style_border_side(q, LV_BORDER_SIDE_LEFT, 0);
            lv_obj_set_style_border_width(q, std::max(3, g.s / 2), 0);
            lv_obj_set_style_border_color(q, lv_color_hex(kMuted), 0);
            lv_obj_set_style_border_opa(q, LV_OPA_COVER, 0);
            lv_obj_set_style_pad_left(q, g.m, 0);
            label(q, b.text, f_text, kReadTitle, std::max(10, tw - g.m - std::max(3, g.s / 2)));
            break;
        }
        case BlockType::Preformatted: label(reader_, b.text, f_mono, kText, tw); break;
        case BlockType::Image: label(reader_, block_text(b), f_small, kMuted, tw); break;
        case BlockType::Rule: {
            lv_obj_t *r = make_box(reader_, 0, 0, tw, 1);
            lv_obj_remove_flag(r, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_set_style_bg_color(r, lv_color_hex(kSelected), 0);
            lv_obj_set_style_bg_opa(r, LV_OPA_COVER, 0);
            break;
        }
        }
    }
    if (!reader_art_.link.empty()) label(reader_, reader_art_.link, f_small, kMuted, tw);
    lv_obj_update_layout(reader_);
}

void App::reader_scroll(int dy)
{
    lv_obj_scroll_by_bounded(reader_, 0, -dy, LV_ANIM_OFF);
    invalidate();
}

void App::key_reader(const KeyEvent &e)
{
    const int view = std::max(1, lv_obj_get_height(reader_));
    const int step = std::max(8, reader_built_px_ * 2);
    switch (e.key) {
    case Key::Up: reader_scroll(-step); break;
    case Key::Down: reader_scroll(step); break;
    case Key::PageUp: reader_scroll(-(view - step)); break;
    case Key::PageDown: reader_scroll(view - step); break;
    case Key::Home: lv_obj_scroll_to_y(reader_, 0, LV_ANIM_OFF); break;
    case Key::End: reader_scroll(1 << 20); break;
    case Key::Char:
        if (e.text == " " && !e.ctrl && !e.alt) reader_scroll(e.shift ? -(view - step) : view - step);
        break;
    case Key::Left:
        if (e.repeat) break;
        if (reader_idx_ > 0) show_article(reader_idx_ - 1);
        else notice("This is the first article");
        break;
    case Key::Right:
        if (e.repeat) break;
        if (reader_idx_ + 1 < reader_src_.count()) show_article(reader_idx_ + 1);
        else notice("This is the last article");
        break;
    default: break;
    }
}

/* The options of the article shown: Text size (the choice with arrows), Mark as unread (a feed's article), Article info. */
void App::reader_options()
{
    if (reader_idx_ < 0) return;
    const TextSize ts = store_.settings().text_size;
    std::vector<std::string> items = {"Text size"};
    std::vector<std::string> values = {text_size_label(ts)};
    std::vector<Mark> marks = {Mark::Chevron};
    const bool feed = !reader_src_.saved;
    if (feed) {
        items.push_back("Mark as unread");
        values.push_back("");
        marks.push_back(Mark::Dot);
    }
    items.push_back("Article info");
    values.push_back("");
    marks.push_back(Mark::Chevron);
    open_menu("Options", items, [this, feed](int pick) {
        if (pick == 0) {
            open_choice(text_size_choice(store_.settings().text_size), [this](int idx) {
                Settings s = store_.settings();
                s.text_size = text_size_from_index(idx);
                store_.set_settings(s);
                const int scroll = lv_obj_get_scroll_y(reader_);
                const int before = std::max(1, static_cast<int>(lv_obj_get_scroll_bottom(reader_)) + scroll);
                render_reader_text();
                const int after = static_cast<int>(lv_obj_get_scroll_bottom(reader_)) + lv_obj_get_scroll_y(reader_);
                lv_obj_scroll_to_y(reader_, static_cast<int>(std::lround(static_cast<double>(scroll) * after / before)), LV_ANIM_OFF);
                notice("Text size: " + text_size_label(s.text_size));
            });
        } else if (feed && pick == 1) {
            const std::string id = reader_art_.id;
            store_.set_read(reader_src_.feed_id, id, false);
            read_marks_.erase(reader_src_.feed_id + "\n" + id);
            if (reader_src_.feed_id == art_feed_.id)
                for (StoredArticle &x : *arts_)
                    if (x.article.id == id) x.read = false;
            notice("Marked as unread");
        } else {
            open_info("Article info", article_info(reader_art_, reader_feed_name_, now_epoch()));
        }
        invalidate();
    }, values, marks);
}

} // namespace rssui
