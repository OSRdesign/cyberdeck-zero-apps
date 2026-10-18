/*
 * SPDX-License-Identifier: MIT
 *
 * RSS Reader: Home, Favorites (and the refresh), the article list of a feed, the saved articles.
 */
#include "rss_app.hpp"

#include "app_style.hpp"
#include "hooks.hpp"
#include "util.hpp"

#include "cp0_ui_metrics_lvgl.h"

#include <algorithm>

using namespace rssreader;

namespace rssui {

namespace {

enum HomeRow { kHomeFavorites = 0, kHomeBrowse, kHomeSaved, kHomeAdd, kHomeSettings };

/* Up / Down / Home / End / Page Up / Page Down over `count` rows; true when the selection moved. */
bool list_move(const KeyEvent &e, int &sel, int count, int page)
{
    if (count <= 0) return false;
    int n = sel;
    switch (e.key) {
    case Key::Up: n = sel - 1; break;
    case Key::Down: n = sel + 1; break;
    case Key::Home: n = 0; break;
    case Key::End: n = count - 1; break;
    case Key::PageUp: n = sel - std::max(1, page); break;
    case Key::PageDown: n = sel + std::max(1, page); break;
    default: return false;
    }
    n = std::clamp(n, 0, count - 1);
    if (n == sel) return true;
    sel = n;
    return true;
}

} // namespace

/* ================================================================== rows */

Row App::build_row(const RowText &t) const
{
    const Geo g = geo();
    const RowList::Config cfg = list_config(4);
    const cp0_ui_list_layout_t &ml = cfg.marks;
    Row r;
    r.id = t.id;
    r.selectable = t.selectable && !t.section;
    r.section = t.section;
    r.lead_dot = t.lead_dot;
    r.trail = t.trail;
    if (t.section) {                                   // a grey heading in capitals, never selected
        r.h = line_h(f_caption_) + g.m;
        Cell c;
        c.text = t.line1;
        c.color = CP0_UI_LIST_SECTION_RGB;
        c.font = f_caption_;
        c.x = ml.inset;
        c.y = r.h - line_h(f_caption_) - g.s / 2;
        c.w = g.lw - 2 * ml.inset;
        r.cells.push_back(c);
        return r;
    }
    const lv_font_t *f1 = t.f1 ? t.f1 : f_row_;
    const bool two = !t.line2.empty() || !t.line2_extra.empty();
    r.h = two ? g.two_line : g.row;
    const cp0_ui_list_affordance_t aff = t.trail == Mark::Chevron ? CP0_UI_LIST_AFFORDANCE_OPEN
                                         : t.trail == Mark::None ? CP0_UI_LIST_AFFORDANCE_UNSPECIFIED : CP0_UI_LIST_AFFORDANCE_ACTION;
    const int trail = cp0_ui_list_trail_w(&ml, aff);
    const int x0 = ml.inset + (t.indent ? cp0_ui_list_dot_px(&ml) + g.s : 0);
    const int right_edge = g.lw - ml.inset - trail;
    const int rw = t.right.empty() ? 0 : std::min(text_w(t.right, f_value_) + 2, std::max(10, (right_edge - x0) * 3 / 5));
    const int lh1 = line_h(f1), lh2 = line_h(f_caption_), lhv = line_h(f_value_);
    const int top = (r.h - (two ? lh1 + lh2 : lh1)) / 2;
    Cell c1;
    c1.text = t.line1;
    c1.color = t.c1;
    c1.font = f1;
    c1.x = x0;
    c1.y = top;
    c1.w = std::max(10, right_edge - x0 - (rw ? rw + ml.gap : 0));
    r.cells.push_back(c1);
    if (two) {
        Cell c2;
        c2.text = t.line2;
        c2.color = t.c2;
        c2.font = f_caption_;
        c2.x = x0;
        c2.y = top + lh1;
        c2.w = std::max(10, right_edge - x0);
        if (!t.line2_extra.empty()) c2.w = std::min(c2.w, text_w(t.line2, f_caption_) + 1);
        r.cells.push_back(c2);
        if (!t.line2_extra.empty()) {
            Cell c3;
            c3.text = t.line2_extra;
            c3.color = t.c2_extra;
            c3.font = f_caption_;
            c3.x = x0 + c2.w;
            c3.y = c2.y;
            c3.w = std::max(10, right_edge - c3.x);
            r.cells.push_back(c3);
        }
    }
    if (rw) {
        Cell cr;
        cr.text = t.right;
        cr.color = t.c_right;
        cr.font = f_value_;
        cr.x = right_edge - rw;
        cr.y = two ? top + (lh1 - lhv) / 2 : (r.h - lhv) / 2;
        cr.w = rw;
        cr.align = LV_TEXT_ALIGN_RIGHT;
        r.cells.push_back(cr);
    }
    return r;
}

/* ================================================================== Home */

void App::build_home()
{
    lv_obj_t *p = make_page();
    pages_[PageId::Home] = p;
    const Geo g = geo();
    make_list(home_list_, p, 0, panel_h_, 3);
}

/* The rows of Home: Favorites (the unread count in the accent blue), Browse feeds, Saved articles (their count), Add a
 * feed, Settings. Every row opens a page: the chevron. */
void App::render_home()
{
    subs_ = store_.subscriptions();
    int unread = 0;
    for (const Subscription &s : subs_) unread += s.unread;
    set_caption(home_caption(unread, subs_.size()));
    const std::size_t saved = store_.list_saved().size();
    std::vector<Row> rows;
    auto add = [&](const char *id, const char *label, const std::string &right, std::uint32_t color) {
        RowText t;
        t.id = id;
        t.line1 = label;
        t.right = right;
        t.c_right = color;
        t.trail = Mark::Chevron;
        rows.push_back(build_row(t));
    };
    add("favorites", "Favorites", new_text(unread), kAccent);
    add("browse", "Browse feeds", "", kMuted);
    add("saved", "Saved articles", saved ? std::to_string(saved) : std::string(), kMuted);
    add("add", "Add a feed", "", kMuted);
    add("settings", "Settings", "", kMuted);
    home_sel_ = std::clamp(home_sel_, 0, static_cast<int>(rows.size()) - 1);
    home_list_.set(std::move(rows), home_sel_);
}

void App::home_activate(int index)
{
    switch (index) {
    case kHomeFavorites: open(PageId::Favorites); break;
    case kHomeBrowse: open(PageId::Browse); break;
    case kHomeSaved: open(PageId::Saved); break;
    case kHomeAdd: open(PageId::Add); break;
    case kHomeSettings: open(PageId::Settings); break;
    default: break;
    }
}

void App::key_home(const KeyEvent &e)
{
    if (list_move(e, home_sel_, home_list_.count(), 4)) {
        home_list_.set_selected(home_sel_, true);
        return;
    }
    if ((e.key == Key::Enter || e.key == Key::Right) && !e.repeat) home_activate(home_sel_);
}

/* ================================================================== Favorites */

void App::build_favorites()
{
    lv_obj_t *p = make_page();
    pages_[PageId::Favorites] = p;
    const Geo g = geo();
    // the Refresh bar: one cell the size of an action bar (ROW high, 7 mm), "Refresh all" (or "Stop") and the status under it
    fav_bar_ = make_box(p, g.x, g.s / 2, g.cw, g.bar);
    lv_obj_add_flag(fav_bar_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_radius(fav_bar_, cp0_ui_metrics_get()->tok[CP0_TOK_RADIUS_S], 0);
    lv_obj_set_style_bg_opa(fav_bar_, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(fav_bar_, lv_color_hex(kSelected), 0);
    lv_obj_set_style_bg_color(fav_bar_, lv_color_hex(kPressed), LV_STATE_PRESSED);
    lv_obj_add_event_cb(
        fav_bar_, [](lv_event_t *e) {
            auto *self = static_cast<App *>(lv_event_get_user_data(e));
            if (self && lv_event_get_code(e) == LV_EVENT_CLICKED) self->button_clicked(kTagFavBar);
        },
        LV_EVENT_CLICKED, this);
    const int lh = line_h(f_body_), vh = line_h(f_caption_);
    const int top = std::max(0, (g.bar - lh - vh) / 2);
    fav_bar_label_ = make_label(fav_bar_, f_body_, kText, g.s, top, g.cw - 2 * g.s, lh);
    lv_obj_set_style_text_align(fav_bar_label_, LV_TEXT_ALIGN_CENTER, 0);
    fav_bar_value_ = make_label(fav_bar_, f_caption_, kMuted, g.s, top + lh, g.cw - 2 * g.s, vh);
    lv_obj_set_style_text_align(fav_bar_value_, LV_TEXT_ALIGN_CENTER, 0);
    const int list_y = g.s / 2 + g.bar + g.s / 2;
    make_list(fav_list_, p, list_y, panel_h_ - list_y, 4);
    fav_list_.set_empty_text("No favorites yet. Browse feeds or add one by its address.", f_body_, kMuted);
}

void App::render_favorites()
{
    subs_ = store_.subscriptions();
    const std::int64_t now = now_epoch();
    set_caption(favorites_caption(subs_));
    const Geo g = geo();
    const bool any = !subs_.empty();
    set_hidden(fav_bar_, !any);
    const int list_y = any ? g.s / 2 + g.bar + g.s / 2 : 0;
    if (lv_obj_get_y(fav_list_.obj()) != list_y) {
        fav_list_.set_y(list_y);
        fav_list_.resize(std::max(1, panel_h_ - list_y));
    }
    lv_label_set_text(fav_bar_label_, refreshing_ ? "Stop" : "Refresh all");
    lv_label_set_text(fav_bar_value_, refresh_status(refreshing_, refresh_done_, std::max<std::size_t>(1, refresh_total_), subs_, now).c_str());
    const std::string in_flight = refreshing_ && refresh_done_ < refresh_ids_.size() ? refresh_ids_[refresh_done_] : std::string();
    std::vector<Row> rows;
    for (const Subscription &s : subs_) {
        const FavRow f = favorite_row(s, &catalog_, now);
        RowText t;
        t.id = f.id;
        t.line1 = f.name;
        t.f1 = f.unread ? f_row_bold_ : f_row_;
        t.c1 = f.unread ? kText : kReadTitle;
        t.line2 = s.id == in_flight ? "Updating\xE2\x80\xA6" : f.line2;
        t.c2 = s.id == in_flight ? kAccent : f.warn ? kWarn : kMuted;
        t.right = f.right;
        t.c_right = kAccent;
        t.lead_dot = f.unread;
        t.indent = true;
        t.trail = Mark::Chevron;
        rows.push_back(build_row(t));
    }
    // the selection follows its feed (a refresh or a move changes the order or the rows)
    if (!fav_sel_id_.empty()) {
        for (std::size_t i = 0; i < subs_.size(); ++i)
            if (subs_[i].id == fav_sel_id_) fav_sel_ = static_cast<int>(i);
    }
    fav_sel_ = rows.empty() ? 0 : std::clamp(fav_sel_, 0, static_cast<int>(rows.size()) - 1);
    if (!rows.empty()) fav_sel_id_ = subs_[static_cast<std::size_t>(fav_sel_)].id;
    fav_list_.set(std::move(rows), fav_sel_);
}

void App::key_favorites(const KeyEvent &e)
{
    if ((is_ctrl_letter(e, 'r') || (e.key == Key::Function && e.fn == 5)) && !e.repeat) {      // Ctrl+R / F5: the Refresh bar
        button_clicked(kTagFavBar);
        return;
    }
    if (list_move(e, fav_sel_, fav_list_.count(), 3)) {
        fav_sel_id_ = fav_list_.id_at(fav_sel_);
        fav_list_.set_selected(fav_sel_, true);
        return;
    }
    if ((e.key == Key::Enter || e.key == Key::Right) && !e.repeat) favorite_activate(fav_sel_);
}

void App::favorite_activate(int index)
{
    if (index < 0 || index >= static_cast<int>(subs_.size())) return;
    open_articles(subs_[static_cast<std::size_t>(index)]);
}

/* The options of a favorite: Move up / down, Unpin (asked first), Mark all read, Refresh this feed, Details. */
void App::favorite_options(int index)
{
    if (index < 0 || index >= static_cast<int>(subs_.size())) return;
    const Subscription s = subs_[static_cast<std::size_t>(index)];
    const std::vector<FavOption> opts = rssui::favorite_options(static_cast<std::size_t>(index), subs_.size());
    std::vector<std::string> items;
    std::vector<Mark> marks;
    for (FavOption o : opts) {
        items.push_back(fav_option_label(o));
        marks.push_back(o == FavOption::Details ? Mark::Chevron : Mark::Dot);
    }
    open_menu("Options", items, [this, opts, s, index](int pick) {
        if (pick < 0 || pick >= static_cast<int>(opts.size())) return;
        switch (opts[static_cast<std::size_t>(pick)]) {
        case FavOption::MoveUp:
            store_.move_subscription(s.id, static_cast<std::size_t>(index - 1));
            fav_sel_id_ = s.id;
            break;
        case FavOption::MoveDown:
            store_.move_subscription(s.id, static_cast<std::size_t>(index + 1));
            fav_sel_id_ = s.id;
            break;
        case FavOption::Unpin:
            open_confirm("Unpin this feed?", s.name + "\n\nIts downloaded articles go. Saved articles stay.", [this, s] {
                if (refreshing_ && std::find(refresh_ids_.begin(), refresh_ids_.end(), s.id) != refresh_ids_.end()) stop_refresh();
                if (store_.remove_subscription(s.id)) notice("Unpinned " + s.name);
                else notice("Could not unpin " + s.name, false);
                fav_sel_id_.clear();
                invalidate();
            }, "Unpin", "Cancel", true);
            break;
        case FavOption::MarkAllRead:
            store_.mark_all_read(s.id);
            notice("All read: " + s.name);
            break;
        case FavOption::Refresh:
            if (refreshing_) notice("Already updating", false);
            else start_refresh({s}, false);
            break;
        case FavOption::Details: open_info("Details", favorite_details(s, &catalog_, now_epoch())); break;
        }
        invalidate();
    }, {}, marks);
    // the feed's name over the rows (dialog titles stay plain ASCII: the toolkit's face)
    pop_.body = s.name;
}

/* ---- the refresh: on a worker thread (Refresher with the app's HTTP client), one feed after the other; the rows and the
 * bar follow; leaving Favorites (or Stop) cancels it. */
void App::start_refresh(const std::vector<Subscription> &subs, bool all, bool quiet)
{
    if (refreshing_ || subs.empty()) return;
    refreshing_ = true;
    refresh_quiet_ = quiet;
    refresh_all_ = all;
    refresh_done_ = 0;
    refresh_total_ = subs.size();
    refresh_failed_ = 0;
    refresh_new_ = 0;
    refresh_ids_.clear();
    for (const Subscription &s : subs) refresh_ids_.push_back(s.id);
    refreshing_id_ = all ? std::string() : subs[0].id;
    HttpClient *http = http_.get();
    Store *store = &store_;
    refresh_job_ = jobs_.start([this, subs, all, http, store](Jobs::Token &tok) {
        Refresher r(*store, *http, [] { return now_epoch(); });
        r.refresh_all(subs, &tok.cancel, [this, all](std::size_t done, std::size_t total, const RefreshResult &res) {
            jobs_.post([this, res, done, total, all] { on_refresh_result(res, done, total, all); });
        });
        const bool cancelled = tok.cancel.load();
        jobs_.post([this, cancelled, all] {
            refreshing_ = false;
            refresh_job_.reset();
            if (cancelled) {
                if (!refresh_quiet_) notice("Refresh stopped", false);
            } else if (all || refresh_total_ > 1) {
                if (refresh_failed_ == static_cast<int>(refresh_total_)) {
                    Subscription s;
                    std::string why = "Could not update";
                    if (!refresh_ids_.empty() && store_.find_subscription(refresh_ids_[0], s)) why = fetch_problem(s.last_error);
                    notice("Not updated: " + why, false);
                } else if (refresh_failed_ > 0) {
                    notice(plural(refresh_failed_, "feed", "feeds") + " could not be updated", false);
                } else {
                    notice(refresh_new_ > 0 ? "Updated: " + new_text(refresh_new_) : std::string("Everything is up to date"));
                }
            }
            refresh_ids_.clear();
            invalidate();
        });
    });
    invalidate();
}

void App::stop_refresh()
{
    if (refresh_job_) refresh_job_->cancel.store(true);
}

void App::on_refresh_result(const RefreshResult &r, std::size_t done, std::size_t total, bool all)
{
    refresh_done_ = done;
    refresh_total_ = total;
    if (r.status == RefreshResult::Status::Failed && r.error != FetchError::Cancelled) ++refresh_failed_;
    refresh_new_ += r.new_articles;
    // the read marks of this session that the refresh may have written over (it merged a copy read before them)
    for (const std::string &k : read_marks_) {
        const std::size_t nl = k.find('\n');
        if (nl != std::string::npos && k.compare(0, nl, r.feed_id) == 0) store_.set_read(r.feed_id, k.substr(nl + 1), true);
    }
    if (!all && total == 1 && !refresh_quiet_) notice(refresh_result_text(r), r.status != RefreshResult::Status::Failed);
    if (page() == PageId::Articles && art_feed_.id == r.feed_id) {
        store_.find_subscription(r.feed_id, art_feed_);
        if (r.status == RefreshResult::Status::Updated) load_articles();
    }
    invalidate();
}

/* ================================================================== the article list */

void App::build_articles()
{
    lv_obj_t *p = make_page();
    pages_[PageId::Articles] = p;
    const Geo g = geo();
    make_list(art_list_, p, 0, panel_h_, 4);
}

void App::open_articles(const Subscription &s)
{
    art_feed_ = s;
    arts_ = std::make_shared<std::vector<StoredArticle>>();
    art_sel_ = 0;
    art_sel_id_.clear();
    art_list_.set({}, 0);
    art_list_.scroll_to_top();
    open(PageId::Articles);
    load_articles();
    // a feed never fetched yet (just pinned, imported): fetch it now
    if (s.last_fetch == 0 && s.last_attempt == 0 && !refreshing_) start_refresh({s}, false);
}

/* The articles of the feed are read on a worker thread (a cache can be a few MB of JSON); the list shows when they are in. */
void App::load_articles()
{
    if (load_job_) load_job_->cancel.store(true);
    arts_loading_ = arts_->empty();
    const unsigned seq = ++load_seq_;
    const std::string id = art_feed_.id;
    Store *store = &store_;
    load_job_ = jobs_.start([this, seq, id, store](Jobs::Token &tok) {
        auto loaded = std::make_shared<std::vector<StoredArticle>>(store->load_articles(id));
        if (tok.cancel.load()) return;
        jobs_.post([this, seq, loaded] {
            if (seq != load_seq_) return;
            arts_ = loaded;
            arts_loading_ = false;
            load_job_.reset();
            invalidate();
        });
    });
    invalidate();
}

void App::render_articles()
{
    const std::int64_t now = now_epoch();
    int unread = 0;
    for (const StoredArticle &a : *arts_) unread += a.read ? 0 : 1;
    const bool updating = refreshing_ && std::find(refresh_ids_.begin(), refresh_ids_.end(), art_feed_.id) != refresh_ids_.end();
    set_caption(articles_caption(unread, arts_->size(), arts_loading_, updating));
    std::vector<Row> rows;
    rows.reserve(arts_->size());
    int sel_by_id = -1;
    for (const StoredArticle &a : *arts_) {
        const ArticleRow ar = article_row(a, store_.is_saved(a.article.id), now);
        RowText t;
        t.id = ar.id;
        t.line1 = ar.title;
        t.c1 = ar.unread ? kText : kReadTitle;
        t.line2 = ar.age.empty() ? std::string("no date") : ar.age;
        if (ar.saved) t.line2_extra = "  \xC2\xB7  saved";
        t.lead_dot = ar.unread;
        t.indent = true;
        t.trail = Mark::Chevron;
        if (ar.id == art_sel_id_) sel_by_id = static_cast<int>(rows.size());
        rows.push_back(build_row(t));
    }
    if (sel_by_id >= 0) art_sel_ = sel_by_id;
    art_sel_ = rows.empty() ? 0 : std::clamp(art_sel_, 0, static_cast<int>(rows.size()) - 1);
    if (!rows.empty()) art_sel_id_ = (*arts_)[static_cast<std::size_t>(art_sel_)].article.id;
    art_list_.set_empty_text(arts_loading_ ? std::string() : updating ? std::string("Updating this feed\xE2\x80\xA6")
                                                                      : std::string("No articles in this feed yet."),
                             f_body_, kMuted);
    art_list_.set(std::move(rows), art_sel_);
}

void App::key_articles(const KeyEvent &e)
{
    if (list_move(e, art_sel_, art_list_.count(), 4)) {
        art_sel_id_ = art_list_.id_at(art_sel_);
        art_list_.set_selected(art_sel_, true);
        return;
    }
    if ((e.key == Key::Enter || e.key == Key::Right) && !e.repeat) article_activate(art_sel_);
}

void App::article_activate(int index)
{
    if (index < 0 || index >= static_cast<int>(arts_->size())) return;
    ReaderSource src;
    src.arts = arts_;
    src.feed_id = art_feed_.id;
    src.feed_name = art_feed_.name;
    open_reader(std::move(src), index);
}

void App::set_article_read(int index, bool read)
{
    if (index < 0 || index >= static_cast<int>(arts_->size())) return;
    StoredArticle &a = (*arts_)[static_cast<std::size_t>(index)];
    a.read = read;
    store_.set_read(art_feed_.id, a.article.id, read);
    const std::string k = art_feed_.id + "\n" + a.article.id;
    if (read) read_marks_.insert(k);
    else read_marks_.erase(k);
    invalidate();
}

/* The options of an article of the list: Mark as read / unread, Mark all read, Refresh this feed, Article info. */
void App::article_options(int index)
{
    const bool have = index >= 0 && index < static_cast<int>(arts_->size());
    std::vector<ArticleOption> opts = rssui::article_options(have && (*arts_)[static_cast<std::size_t>(index)].read);
    if (!have) opts = {ArticleOption::MarkAllRead, ArticleOption::Refresh};
    std::vector<std::string> items;
    std::vector<Mark> marks;
    for (ArticleOption o : opts) {
        items.push_back(article_option_label(o));
        marks.push_back(o == ArticleOption::Info ? Mark::Chevron : Mark::Dot);
    }
    open_menu("Options", items, [this, opts, index, have](int pick) {
        if (pick < 0 || pick >= static_cast<int>(opts.size())) return;
        switch (opts[static_cast<std::size_t>(pick)]) {
        case ArticleOption::MarkRead: set_article_read(index, true); break;
        case ArticleOption::MarkUnread: set_article_read(index, false); break;
        case ArticleOption::MarkAllRead:
            store_.mark_all_read(art_feed_.id);
            for (StoredArticle &a : *arts_) a.read = true;
            notice("All read: " + art_feed_.name);
            break;
        case ArticleOption::Refresh:
            if (refreshing_) notice("Already updating", false);
            else start_refresh({art_feed_}, false);
            break;
        case ArticleOption::Info:
            if (have) open_info("Article info", article_info((*arts_)[static_cast<std::size_t>(index)].article, art_feed_.name, now_epoch()));
            break;
        }
        invalidate();
    }, {}, marks);
    if (have) pop_.body = (*arts_)[static_cast<std::size_t>(index)].article.title;
}

/* ================================================================== the saved articles */

void App::build_saved()
{
    lv_obj_t *p = make_page();
    pages_[PageId::Saved] = p;
    const Geo g = geo();
    make_list(saved_list_, p, 0, panel_h_, 3);
    saved_list_.set_empty_text("No saved articles yet. Open an article and choose Save for offline in its options.", f_body_, kMuted);
}

void App::render_saved()
{
    const std::int64_t now = now_epoch();
    std::uint64_t bytes = 0;
    for (const SavedSummary &s : saved_) bytes += s.bytes;
    set_caption(saved_.empty() ? std::string("Kept on the deck for offline reading")
                               : plural(static_cast<long>(saved_.size()), "article", "articles") + "  \xC2\xB7  " + fmt_bytes(bytes));
    std::vector<Row> rows;
    for (const SavedSummary &s : saved_) {
        RowText t;
        t.id = s.id;
        t.line1 = s.title.empty() ? std::string("(no title)") : s.title;
        t.line2 = s.feed_name + "  \xC2\xB7  saved " + fmt_age(s.saved_at, now);
        t.trail = Mark::Chevron;
        rows.push_back(build_row(t));
    }
    saved_sel_ = rows.empty() ? 0 : std::clamp(saved_sel_, 0, static_cast<int>(rows.size()) - 1);
    saved_list_.set(std::move(rows), saved_sel_);
}

void App::key_saved(const KeyEvent &e)
{
    if (list_move(e, saved_sel_, saved_list_.count(), 4)) {
        saved_list_.set_selected(saved_sel_, true);
        return;
    }
    if ((e.key == Key::Enter || e.key == Key::Right) && !e.repeat) saved_activate(saved_sel_);
}

void App::saved_activate(int index)
{
    if (index < 0 || index >= static_cast<int>(saved_.size())) return;
    ReaderSource src;
    src.saved = true;
    src.saved_list = saved_;
    open_reader(std::move(src), index);
}

} // namespace rssui
