/*
 * SPDX-License-Identifier: MIT
 *
 * RSS Reader: Browse the bundled catalog. Level 1 the countries (International first; the languages of each on the second
 * line; the feed count; a search field over the whole catalog above them), level 2 the topics of a country, level 3 the
 * feeds of a topic with the favorite toggle at the end of the row (a blue dot: pinned; an empty ring: not; a tap on the
 * selected row toggles it: no chevron, the row acts). The search shows flat results with the same toggle. Podcasts are
 * not shown (text feeds only; they stay in catalog.json). The header title follows the path ("Browse > France >
 * Technology", shortened in the middle when it would reach the Esc button); Esc goes up one level.
 */
#include "rss_app.hpp"

#include "app_style.hpp"
#include "hooks.hpp"
#include "util.hpp"

#include <algorithm>

using namespace rssreader;

namespace rssui {

void App::build_browse()
{
    lv_obj_t *p = make_page();
    pages_[PageId::Browse] = p;
    const Geo g = geo();
    search_ta_ = make_text_field(p, kTagSearchField, 64, g.row, "Search all feeds");
    lv_obj_set_pos(search_ta_, g.x, g.s / 2);
    lv_obj_set_width(search_ta_, g.cw);
    make_list(browse_list_, p, 0, panel_h_, 4);
}

std::vector<std::string> App::browse_path() const
{
    std::vector<std::string> path = {"Browse"};
    if (blevel_ == BrowseLevel::Countries) {
        if (!search_text_.empty()) path.push_back("Search");
        return path;
    }
    path.push_back(country_name(bcountry_));
    if (blevel_ == BrowseLevel::Feeds) path.push_back(topic_label(btopic_));
    return path;
}

void App::render_browse()
{
    const Geo g = geo();
    pinned_.clear();
    for (const Subscription &s : store_.subscriptions()) pinned_.insert(s.id);
    const bool countries = blevel_ == BrowseLevel::Countries;
    set_hidden(search_ta_, !countries);
    const int list_y = countries ? g.s / 2 + g.row + g.s / 2 : 0;
    if (lv_obj_get_y(browse_list_.obj()) != list_y || browse_list_.height() != std::max(1, panel_h_ - list_y)) {
        browse_list_.set_y(list_y);
        browse_list_.resize(std::max(1, panel_h_ - list_y));
    }
    browse_list_.set_hidden(panel_h_ - list_y < g.row);       // the on-screen keyboard leaves room for the field only
    std::vector<Row> rows;
    auto feed_row = [&](const CatalogFeed *f, const std::string &line2) {
        RowText t;
        t.id = f->id;
        t.line1 = f->name;
        t.line2 = line2;
        t.trail = pinned_.count(f->id) ? Mark::Filled : Mark::Ring;
        rows.push_back(build_row(t));
    };
    bfeeds_.clear();
    std::string caption;
    if (!catalog_ok_) {
        caption = "The feed catalog could not be read";
    } else if (countries && !search_text_.empty()) {
        bfeeds_ = browse_search(catalog_, search_text_);
        for (const CatalogFeed *f : bfeeds_) feed_row(f, search_line2(*f));
        caption = bfeeds_.empty() ? std::string("No feed matches") : plural(static_cast<long>(bfeeds_.size()), "feed matches", "feeds match") +
                                                                       "  \xC2\xB7  tap a selected one to pin or unpin it";
        // portrait: the caption is one line of a narrow screen; the shorter hint when the long one is cut
        if (portrait() && !bfeeds_.empty() && text_w(caption, f_caption_) > layout_.caption_w)
            caption = plural(static_cast<long>(bfeeds_.size()), "feed matches", "feeds match") + "  \xC2\xB7  tap a selected one to pin";
        browse_list_.set_empty_text("No feed matches \"" + search_text_ + "\".", f_body_, kMuted);
    } else if (countries) {
        std::size_t total = 0;
        for (const CatalogGroup &c : browse_countries(catalog_)) {
            RowText t;
            t.id = c.key;
            t.line1 = c.label;
            t.line2 = country_languages(catalog_, c.key);
            t.right = std::to_string(c.feeds);
            t.trail = Mark::Chevron;
            rows.push_back(build_row(t));
            total += c.feeds;
        }
        caption = "Choose a country, or search " + plural(static_cast<long>(total), "feed", "feeds");
        browse_list_.set_empty_text("", f_body_, kMuted);
    } else if (blevel_ == BrowseLevel::Topics) {
        std::size_t total = 0;
        for (const CatalogGroup &t0 : browse_topics(catalog_, bcountry_)) {
            RowText t;
            t.id = t0.key;
            t.line1 = t0.label;
            t.right = std::to_string(t0.feeds);
            t.trail = Mark::Chevron;
            rows.push_back(build_row(t));
            total += t0.feeds;
        }
        caption = country_name(bcountry_) + "  \xC2\xB7  " + country_languages(catalog_, bcountry_) + "  \xC2\xB7  " +
                  plural(static_cast<long>(total), "feed", "feeds");
    } else {
        bfeeds_ = browse_feeds(catalog_, bcountry_, btopic_);
        for (const CatalogFeed *f : bfeeds_) feed_row(f, feed_content_text(*f));
        caption = country_name(bcountry_) + "  \xC2\xB7  " + topic_label(btopic_) + "  \xC2\xB7  tap a selected feed to pin or unpin it";
        if (portrait() && text_w(caption, f_caption_) > layout_.caption_w)       // the header names the topic
            caption = country_name(bcountry_) + "  \xC2\xB7  tap a selected feed to pin or unpin it";
        if (portrait() && text_w(caption, f_caption_) > layout_.caption_w)
            caption = country_name(bcountry_) + "  \xC2\xB7  tap a selected feed to pin";
    }
    set_caption(caption);
    browse_sel_ = rows.empty() ? 0 : std::clamp(browse_sel_, 0, static_cast<int>(rows.size()) - 1);
    browse_list_.set(std::move(rows), browse_sel_);
    // the field shows the words (typed by keys or the on-screen keyboard)
    if (search_text_ != lv_textarea_get_text(search_ta_)) lv_textarea_set_text(search_ta_, search_text_.c_str());
}

void App::browse_activate(int index)
{
    const bool search = blevel_ == BrowseLevel::Countries && !search_text_.empty();
    if (search || blevel_ == BrowseLevel::Feeds) {
        if (index >= 0 && index < static_cast<int>(bfeeds_.size())) toggle_favorite(bfeeds_[static_cast<std::size_t>(index)]);
        return;
    }
    const std::string id = browse_list_.id_at(index);
    if (id.empty()) return;
    bsel_stack_.push_back(index);
    if (blevel_ == BrowseLevel::Countries) {
        bcountry_ = id;
        blevel_ = BrowseLevel::Topics;
        search_typing_ = false;
    } else {
        btopic_ = id;
        blevel_ = BrowseLevel::Feeds;
    }
    browse_sel_ = 0;
    browse_list_.set({}, 0);
    browse_list_.scroll_to_top();
    invalidate();
}

/* One level up (or the search words away). False at the countries without a search: the page itself goes. */
bool App::browse_back()
{
    if (blevel_ == BrowseLevel::Countries) {
        if (search_text_.empty()) return false;
        search_text_.clear();
        search_typing_ = false;
        lv_textarea_set_text(search_ta_, "");
        browse_sel_ = 0;
        browse_list_.scroll_to_top();
        invalidate();
        return true;
    }
    blevel_ = blevel_ == BrowseLevel::Feeds ? BrowseLevel::Topics : BrowseLevel::Countries;
    if (blevel_ == BrowseLevel::Countries) bcountry_.clear();
    btopic_.clear();
    browse_sel_ = bsel_stack_.empty() ? 0 : bsel_stack_.back();
    if (!bsel_stack_.empty()) bsel_stack_.pop_back();
    browse_list_.set({}, 0);
    invalidate();
    render();
    browse_list_.scroll_to_selected();
    return true;
}

void App::toggle_favorite(const CatalogFeed *f)
{
    if (!f) return;
    if (pinned_.count(f->id)) {
        if (refreshing_ && std::find(refresh_ids_.begin(), refresh_ids_.end(), f->id) != refresh_ids_.end()) stop_refresh();
        if (store_.remove_subscription(f->id)) notice("Removed from favorites: " + f->name);
        else notice("Could not remove " + f->name, false);
    } else {
        const Subscription s = make_subscription(*f, now_epoch());
        const StoreError e = store_.add_subscription(s);
        if (e == StoreError::Ok) {
            notice("Added to favorites: " + f->name);
            if (!refreshing_) start_refresh({s}, false, true);       // its articles, in the background
        } else if (e == StoreError::Duplicate) {
            notice("Already in your favorites (added by its address)", false);
        } else {
            notice("Could not add " + f->name, false);
        }
    }
    invalidate();
}

void App::key_browse(const KeyEvent &e)
{
    const bool countries = blevel_ == BrowseLevel::Countries;
    if (countries && (is_typing(e) || (e.key == Key::Backspace && (search_typing_ || !search_text_.empty())) ||
                      (search_typing_ && (e.key == Key::Delete || e.key == Key::Home || e.key == Key::End || e.key == Key::Left || e.key == Key::Right)))) {
        search_typing_ = true;
        field_key(search_ta_, e, 64);
        const std::string now = lv_textarea_get_text(search_ta_);
        if (now != search_text_) {
            search_text_ = now;
            browse_sel_ = 0;
            browse_list_.scroll_to_top();
        }
        invalidate();
        return;
    }
    if (e.key == Key::Enter && search_typing_ && osk_inset_ > 0 && !e.repeat) {   // Enter puts the keyboard away: the results show
        search_typing_ = false;
        invalidate();
        return;
    }
    if (search_typing_ && (e.key == Key::Up || e.key == Key::Down || e.key == Key::Enter)) search_typing_ = false;   // to the list
    int sel = browse_sel_;
    const int n = browse_list_.count();
    switch (e.key) {
    case Key::Up: sel = std::max(0, sel - 1); break;
    case Key::Down: sel = std::min(std::max(0, n - 1), sel + 1); break;
    case Key::Home: sel = 0; break;
    case Key::End: sel = std::max(0, n - 1); break;
    case Key::PageUp: sel = std::max(0, sel - 4); break;
    case Key::PageDown: sel = std::min(std::max(0, n - 1), sel + 4); break;
    case Key::Enter:
    case Key::Right:
        if (!e.repeat && n > 0) browse_activate(browse_sel_);
        return;
    case Key::Left:
        if (!e.repeat && !browse_back()) go_up();
        return;
    default: return;
    }
    browse_sel_ = sel;
    browse_list_.set_selected(sel, true);
}

} // namespace rssui
