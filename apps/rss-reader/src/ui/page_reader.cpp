/*
 * SPDX-License-Identifier: MIT
 *
 * RSS Reader: the reader. The article's title, then its text as html_to_blocks gives it: headings, paragraphs, list items,
 * quotes (a bar at the left), preformatted text, rules, and the images where they stand in the text (an article whose text
 * has none shows the first image of the feed's media under the title). The text column takes the whole width of the
 * screen less a SPACE_L margin on each side; the text size is the setting's. It scrolls by a drag, the wheel, Up / Down,
 * Page Up / Page Down, Space, Home / End; Left / Right go to the previous / next article of the list. Opening an article
 * marks it read.
 *
 * Images (docs/IMAGES.md): the first six different ones of an article are read from its saved copy, the image cache or,
 * with Download images = Always, the network (two worker threads, fetch_image: 2 MB, 15 s), then decoded off the UI
 * thread into a thumbnail no wider than the column and no taller than a third of the screen. Until then, and when that
 * fails, a placeholder tile shows the image's description and why ("Downloading...", "Image unavailable", "Too large to
 * show", "Format not supported", "Images are off", "Not loaded"). Leaving the article cancels the downloads and frees the
 * pictures. A tap on an image, or Enter, opens it full screen (page_viewer.cpp). With the keys, Up / Down scroll the text
 * as always and a ring marks the image nearest to the middle of the screen: Enter opens that one; Tab / Shift+Tab jump
 * to the next / previous image (it comes to the middle).
 */
#include "rss_app.hpp"

#include "app_style.hpp"
#include "hooks.hpp"
#include "html_text.hpp"
#include "util.hpp"

#include "cp0_ui_metrics_lvgl.h"

#include <algorithm>
#include <cmath>
#include <map>

using namespace rssreader;

namespace rssui {

namespace {

constexpr std::uint32_t kImageTile = 0x202838;   // the placeholder of an image (the design's tile)
constexpr std::uint32_t kImageBg = 0xFFFFFF;     // what a transparent part of a picture is drawn over (the page it was made for)

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

void tile_event_cb(lv_event_t *e)
{
    auto *self = static_cast<App *>(lv_event_get_user_data(e));
    auto *tile = static_cast<lv_obj_t *>(lv_event_get_current_target(e));
    if (!self || !tile || lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    self->image_tapped(static_cast<int>(reinterpret_cast<intptr_t>(lv_obj_get_user_data(tile))));
}

ImageState state_of(DecodeStatus s)
{
    switch (s) {
    case DecodeStatus::Ok: return ImageState::Ready;
    case DecodeStatus::TooLarge: return ImageState::TooLarge;
    case DecodeStatus::Unsupported: return ImageState::Unsupported;
    default: return ImageState::Unavailable;
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

int App::thumb_max_w() const
{
    const Geo g = geo();
    return std::max(16, g.w - 2 * g.inset);
}

int App::thumb_max_h() const { return std::max(16, layout_.h / 3); }

void App::open_reader(ReaderSource src, int index)
{
    reader_src_ = std::move(src);
    reader_idx_ = -1;
    open(PageId::Reader);
    show_article(index);
}

/* The article at `index` of the source: shown from the top, marked read, its images on their way. */
void App::show_article(int index)
{
    if (index < 0 || index >= reader_src_.count()) return;
    if (viewer_open()) close_viewer();
    reader_images_stop();
    reader_idx_ = index;
    reader_saved_id_.clear();
    reader_saved_files_.clear();
    SavedArticle sa;
    bool have_saved = false;
    if (reader_src_.saved) {
        const SavedSummary &s = reader_src_.saved_list[static_cast<std::size_t>(index)];
        if (store_.load_saved(s.id, sa)) {
            have_saved = true;
            reader_art_ = sa.article;
            reader_feed_name_ = sa.feed_name;
            reader_feed_id_ = sa.feed_id;
            Subscription sub;
            if (store_.find_subscription(sa.feed_id, sub)) store_.set_read(sa.feed_id, sa.article.id, true);
        } else {
            reader_art_ = Article();
            reader_art_.title = s.title;
            reader_feed_name_ = s.feed_name;
            reader_feed_id_ = s.feed_id;
        }
    } else {
        const StoredArticle &a = (*reader_src_.arts)[static_cast<std::size_t>(index)];
        reader_art_ = a.article;
        reader_feed_name_ = reader_src_.feed_name;
        reader_feed_id_ = reader_src_.feed_id;
        store_.set_read(reader_src_.feed_id, a.article.id, true);
        // the list's copy (the same snapshot unless a refresh replaced it)
        if (reader_src_.feed_id == art_feed_.id)
            for (StoredArticle &x : *arts_)
                if (x.article.id == a.article.id) x.read = true;
        have_saved = store_.is_saved(a.article.id) && store_.load_saved(a.article.id, sa);
    }
    if (have_saved) {                      // a saved copy: its images are read from there, never downloaded again
        reader_saved_id_ = sa.article.id;
        for (const SavedImageInfo &i : sa.images) reader_saved_files_[i.url] = i.file;
    }
    rimgs_.clear();
    for (const ArticleImage &im : article_images(html_to_blocks(reader_art_.body_html(), reader_art_.link), reader_art_)) {
        ReaderImage r;
        r.src = im;
        r.state = im.fetch ? ImageState::Loading : ImageState::NotLoaded;
        rimgs_.push_back(std::move(r));
    }
    img_focus_ = -1;
    render_reader_text();
    lv_obj_scroll_to_y(reader_, 0, LV_ANIM_OFF);
    update_image_focus();
    start_reader_images();
    invalidate();
}

/* The article's image jobs stop, the tiles go, the pictures are freed (leaving the article, the page, the app). */
void App::reader_images_stop()
{
    for (Jobs::Ptr &j : img_jobs_)
        if (j) j->cancel.store(true);
    img_jobs_.clear();
    ++img_seq_;
    if (reader_) lv_obj_clean(reader_);     // no lv_image may point at a picture freed below
    for (ReaderImage &r : rimgs_) {
        r.tile = nullptr;
        r.pix.reset();
    }
    rimgs_.clear();
    img_focus_ = -1;
}

bool App::local_image(Store *store, ImageCache *cache, const std::string &saved_id, const std::string &file, const std::string &url,
                      std::string &bytes, std::string &type)
{
    if (!saved_id.empty() && !file.empty() && store->read_saved_image(saved_id, file, bytes) && !bytes.empty()) {
        const std::string sniffed = sniff_image_type(bytes);
        type = sniffed.empty() ? "application/octet-stream" : sniffed;
        return true;
    }
    return cache->get(url, bytes, &type);
}

bool App::fetch_to_cache(HttpClient *http, ImageCache *cache, const std::string &url, const std::atomic<bool> *cancel, std::string &bytes,
                         std::string &type, FetchError &error)
{
    HttpResponse r = fetch_image(*http, url, cancel);
    error = r.error;
    if (!r.ok()) return false;
    bytes = std::move(r.body);
    type = r.content_type;
    cache->put(url, bytes, type);
    return true;
}

/* Two worker threads share the article's different image addresses (the first six); each reads, downloads when it has
 * to and may, decodes (one decode at a time in the whole app) and posts the thumbnail to every tile of that address. */
void App::start_reader_images()
{
    struct Want {
        std::string url, file;
        std::vector<int> tiles;
    };
    std::vector<Want> wants;
    for (std::size_t i = 0; i < rimgs_.size(); ++i) {
        const ArticleImage &im = rimgs_[i].src;
        if (!im.fetch) continue;
        auto it = std::find_if(wants.begin(), wants.end(), [&](const Want &w) { return w.url == im.url; });
        if (it == wants.end()) {
            Want w;
            w.url = im.url;
            auto f = reader_saved_files_.find(im.url);
            if (f != reader_saved_files_.end()) w.file = f->second;
            wants.push_back(std::move(w));
            it = wants.end() - 1;
        }
        it->tiles.push_back(static_cast<int>(i));
    }
    if (wants.empty()) return;
    const unsigned seq = img_seq_;
    const bool net = store_.settings().image_download == ImageDownload::Always;
    const int tw = thumb_max_w(), th = thumb_max_h();
    const std::string saved_id = reader_saved_id_;
    Store *store = &store_;
    ImageCache *cache = &image_cache_;
    HttpClient *http = http_.get();
    auto shared = std::make_shared<std::vector<Want>>(std::move(wants));
    const int lanes = std::min<int>(2, static_cast<int>(shared->size()));
    for (int lane = 0; lane < lanes; ++lane) {
        img_jobs_.push_back(jobs_.start([this, shared, lane, lanes, seq, net, tw, th, saved_id, store, cache, http](Jobs::Token &tok) {
            for (std::size_t k = static_cast<std::size_t>(lane); k < shared->size(); k += static_cast<std::size_t>(lanes)) {
                if (tok.cancel.load()) return;
                const Want &w = (*shared)[k];
                auto tell = [this, seq, &w](ImageState st, std::shared_ptr<DecodedImage> pix) {
                    for (int t : w.tiles) jobs_.post([this, seq, t, st, pix] { on_image(seq, t, st, pix); });
                };
                std::string bytes, type;
                bool got = local_image(store, cache, saved_id, w.file, w.url, bytes, type);
                if (!got && !net) {
                    tell(ImageState::Off, nullptr);
                    continue;
                }
                if (!got) {
                    tell(ImageState::Downloading, nullptr);
                    FetchError err = FetchError::None;
                    got = fetch_to_cache(http, cache, w.url, &tok.cancel, bytes, type, err);
                    if (err == FetchError::Cancelled || tok.cancel.load()) return;
                    if (!got) {
                        tell(ImageState::Unavailable, nullptr);
                        continue;
                    }
                }
                auto pix = std::make_shared<DecodedImage>();
                const DecodeStatus ds = decode_image(bytes, tw, th, kImageBg, *pix, &tok.cancel);
                if (ds == DecodeStatus::Cancelled) return;
                tell(state_of(ds), ds == DecodeStatus::Ok ? pix : nullptr);
            }
        }));
    }
}

/* A result of the image jobs (UI thread): the tile becomes the picture or says why not. The text under a tile that changes
 * height above the view does not move on the screen. */
void App::on_image(unsigned seq, int index, ImageState st, std::shared_ptr<DecodedImage> pix)
{
    if (seq != img_seq_ || index < 0 || index >= static_cast<int>(rimgs_.size())) return;
    ReaderImage &r = rimgs_[static_cast<std::size_t>(index)];
    if (r.state == ImageState::Ready) return;
    r.state = st;
    if (st == ImageState::Ready && pix && !pix->rgb565.empty()) {
        if (pix->src_width <= 16 && pix->src_height <= 16) {      // a spacer or an icon: not content
            r.state = ImageState::Hidden;
        } else {
            r.pix = std::move(pix);
            r.dsc = lv_image_dsc_t{};
            r.dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
            r.dsc.header.cf = LV_COLOR_FORMAT_RGB565;
            r.dsc.header.w = static_cast<uint32_t>(r.pix->width);
            r.dsc.header.h = static_cast<uint32_t>(r.pix->height);
            r.dsc.header.stride = static_cast<uint32_t>(r.pix->width * 2);
            r.dsc.data_size = static_cast<uint32_t>(r.pix->rgb565.size());
            r.dsc.data = r.pix->rgb565.data();
        }
    } else if (st == ImageState::Ready) {
        r.state = ImageState::Unavailable;
    }
    if (!r.tile) return;
    lv_obj_update_layout(reader_);
    lv_area_t before, view;
    lv_obj_get_coords(r.tile, &before);
    lv_obj_get_coords(reader_, &view);
    const int c0 = lv_obj_get_scroll_y(reader_) + static_cast<int>(lv_obj_get_scroll_bottom(reader_));   // the content's height
    style_image_tile(index);
    lv_obj_update_layout(reader_);
    const int dh = lv_obj_get_scroll_y(reader_) + static_cast<int>(lv_obj_get_scroll_bottom(reader_)) - c0;
    if (dh != 0 && before.y2 < view.y1) lv_obj_scroll_by(reader_, 0, -dh, LV_ANIM_OFF);
    update_image_focus();
    invalidate();
}

lv_obj_t *App::make_image_tile(lv_obj_t *parent, int index)
{
    lv_obj_t *t = lv_obj_create(parent);
    lv_obj_remove_style_all(t);
    lv_obj_remove_flag(t, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(t, LV_OBJ_FLAG_CLICK_FOCUSABLE);
    lv_obj_add_flag(t, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(t, LV_OBJ_FLAG_EVENT_BUBBLE);           // the 3 s hold of the text works on an image too
    lv_obj_set_user_data(t, reinterpret_cast<void *>(static_cast<intptr_t>(index)));
    lv_obj_add_event_cb(t, tile_event_cb, LV_EVENT_CLICKED, this);
    rimgs_[static_cast<std::size_t>(index)].tile = t;
    style_image_tile(index);
    return t;
}

/* A tile: the picture (at least ROW high: 7 mm to tap), or the placeholder (the description, the state). The ring of the
 * keys' image is an outline (it changes nothing in the layout). */
void App::style_image_tile(int index)
{
    ReaderImage &r = rimgs_[static_cast<std::size_t>(index)];
    lv_obj_t *t = r.tile;
    if (!t) return;
    lv_obj_clean(t);
    const cp0_ui_metrics_t *m = cp0_ui_metrics_get();
    const Geo g = geo();
    const int row = m->tok[CP0_TOK_ROW_H];
    set_hidden(t, r.state == ImageState::Hidden);
    if (r.state == ImageState::Ready && r.pix) {
        lv_obj_set_size(t, std::max(r.pix->width, row), std::max(r.pix->height, row));
        lv_obj_set_style_bg_opa(t, LV_OPA_TRANSP, 0);
        lv_obj_set_style_radius(t, m->tok[CP0_TOK_RADIUS_S], 0);      // the ring's corners (the picture is not clipped)
        lv_obj_t *img = lv_image_create(t);
        lv_obj_remove_flag(img, LV_OBJ_FLAG_CLICKABLE);
        lv_image_set_src(img, &r.dsc);
        lv_obj_align(img, LV_ALIGN_LEFT_MID, 0, 0);
    } else if (r.state != ImageState::Hidden) {
        const int px = reader_built_px_ > 0 ? reader_built_px_ : reader_px();
        const lv_font_t *f_alt = font("Montserrat-Medium.ttf", cp0_ui_roller_font_px(px * 5 / 6));
        const int lh = line_h(f_alt), ch = line_h(f_caption_), pad = g.s;
        const int w = std::min(thumb_max_w(), std::max(2 * row, thumb_max_w() * 2 / 5));
        const int h = std::max(row, 2 * pad + 2 * lh + ch);
        lv_obj_set_size(t, w, h);
        lv_obj_set_style_bg_color(t, lv_color_hex(kImageTile), 0);
        lv_obj_set_style_bg_opa(t, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(t, m->tok[CP0_TOK_RADIUS_S], 0);
        lv_obj_t *alt = make_label(t, f_alt, kMuted, pad, pad, w - 2 * pad, 2 * lh);
        lv_label_set_long_mode(alt, LV_LABEL_LONG_MODE_DOTS);
        lv_label_set_text(alt, r.src.alt.empty() ? "Image" : r.src.alt.c_str());
        lv_obj_t *cap = make_label(t, f_caption_, kReadTitle, pad, h - pad - ch, w - 2 * pad, ch);
        lv_label_set_text(cap, image_caption(r.state).c_str());
    }
    const bool ring = reader_keys_ && index == img_focus_;
    lv_obj_set_style_outline_width(t, ring ? 3 : 0, 0);
    lv_obj_set_style_outline_color(t, lv_color_hex(kAccent), 0);
    lv_obj_set_style_outline_pad(t, 2, 0);
    lv_obj_set_style_outline_opa(t, LV_OPA_COVER, 0);
}

/* The keys' image: the one nearest to the middle of the view (at least half in view); the ring shows while the keys lead. */
void App::update_image_focus()
{
    std::vector<std::pair<int, int>> tiles;
    for (const ReaderImage &r : rimgs_) {
        if (!r.tile || is_hidden(r.tile)) {
            tiles.emplace_back(0, 0);
            continue;
        }
        lv_area_t a;
        lv_obj_get_coords(r.tile, &a);
        tiles.emplace_back(a.y1, a.y2 + 1);
    }
    lv_area_t v;
    lv_obj_get_coords(reader_, &v);
    const int focus = nearest_image(tiles, v.y1, v.y2 - v.y1 + 1);
    for (std::size_t i = 0; i < rimgs_.size(); ++i)
        if (lv_obj_t *t = rimgs_[i].tile) lv_obj_set_style_outline_width(t, reader_keys_ && static_cast<int>(i) == focus ? 3 : 0, 0);
    img_focus_ = focus;
}

/* Tab / Shift+Tab: the next / previous image comes to the middle of the view and takes the ring. */
void App::focus_image_step(int dir)
{
    std::vector<int> shown;
    for (std::size_t i = 0; i < rimgs_.size(); ++i)
        if (rimgs_[i].tile && !is_hidden(rimgs_[i].tile)) shown.push_back(static_cast<int>(i));
    if (shown.empty()) {
        notice("No image in this article");
        return;
    }
    lv_area_t v;
    lv_obj_get_coords(reader_, &v);
    const int mid = (v.y1 + v.y2) / 2;
    int next = -1;
    if (img_focus_ >= 0) {
        auto it = std::find(shown.begin(), shown.end(), img_focus_);
        const int pos = it == shown.end() ? -1 : static_cast<int>(it - shown.begin());
        const int np = pos + dir;
        if (np >= 0 && np < static_cast<int>(shown.size())) next = shown[static_cast<std::size_t>(np)];
    } else {                               // none marked: the first below (or above) the middle
        for (int k = 0; k < static_cast<int>(shown.size()); ++k) {
            const int i = shown[static_cast<std::size_t>(dir > 0 ? k : static_cast<int>(shown.size()) - 1 - k)];
            lv_area_t a;
            lv_obj_get_coords(rimgs_[static_cast<std::size_t>(i)].tile, &a);
            if (dir > 0 ? a.y2 >= mid : a.y1 <= mid) {
                next = i;
                break;
            }
        }
    }
    if (next < 0) {
        notice(dir > 0 ? "This is the last image" : "This is the first image");
        return;
    }
    lv_area_t a;
    lv_obj_get_coords(rimgs_[static_cast<std::size_t>(next)].tile, &a);
    lv_obj_scroll_by_bounded(reader_, 0, mid - (a.y1 + a.y2) / 2, LV_ANIM_OFF);
    lv_obj_update_layout(reader_);
    img_focus_ = next;
    for (std::size_t i = 0; i < rimgs_.size(); ++i)
        if (lv_obj_t *t = rimgs_[i].tile) lv_obj_set_style_outline_width(t, static_cast<int>(i) == next ? 3 : 0, 0);
    invalidate();
}

/* A tap (or a pointer click) on an image: full screen when it is there, else what is happening to it. */
void App::image_tapped(int index)
{
    if (hold_.take_swallow()) return;              // the release of a completed 3 s hold is not a tap
    if (hold_release_ms_ && mono_ms() - hold_release_ms_ < 300) {
        hold_release_ms_ = 0;
        return;
    }
    if (mono_ms() - start_ms_ < 600 || popup_open() || page() != PageId::Reader) return;
    if (index < 0 || index >= static_cast<int>(rimgs_.size())) return;
    const ReaderImage &r = rimgs_[static_cast<std::size_t>(index)];
    if (r.state == ImageState::Ready) open_viewer(index);
    else notice(r.state == ImageState::Loading || r.state == ImageState::Downloading ? std::string("The image is still loading") : image_caption(r.state), false);
}

/* The text of the article as labels in the reader's column (rebuilt for a new article or a new text size): the title,
 * the first-run hint, the lead image, the blocks with their image tiles, the link. */
void App::render_reader_text()
{
    for (ReaderImage &r : rimgs_) r.tile = nullptr;
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
    if (hint_shown()) label(reader_, options_hint(), f_caption_, kMuted, tw);
    std::map<int, int> tile_of_block;
    for (std::size_t i = 0; i < rimgs_.size(); ++i) {
        if (rimgs_[i].src.block < 0) make_image_tile(reader_, static_cast<int>(i));    // the lead image, under the title
        else tile_of_block[rimgs_[i].src.block] = static_cast<int>(i);
    }
    const std::vector<Block> blocks = html_to_blocks(reader_art_.body_html(), reader_art_.link);
    if (blocks.empty()) label(reader_, "This feed gives no text for this article.", f_text, kMuted, tw);
    std::size_t shown = 0;
    for (std::size_t bi = 0; bi < blocks.size(); ++bi) {
        const Block &b = blocks[bi];
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
        case BlockType::Image: {
            auto it = tile_of_block.find(static_cast<int>(bi));
            if (it != tile_of_block.end()) make_image_tile(reader_, it->second);
            break;                                   // a junk picture (an emoji, a counter): nothing
        }
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
    lv_obj_update_layout(reader_);
    update_image_focus();
    invalidate();
}

void App::key_reader(const KeyEvent &e)
{
    const int view = std::max(1, lv_obj_get_height(reader_));
    const int step = std::max(8, reader_built_px_ * 2);
    if (!reader_keys_) {
        reader_keys_ = true;
        update_image_focus();
    }
    switch (e.key) {
    case Key::Up: reader_scroll(-step); break;
    case Key::Down: reader_scroll(step); break;
    case Key::PageUp: reader_scroll(-(view - step)); break;
    case Key::PageDown: reader_scroll(view - step); break;
    case Key::Home:
        lv_obj_scroll_to_y(reader_, 0, LV_ANIM_OFF);
        reader_scroll(0);
        break;
    case Key::End: reader_scroll(1 << 20); break;
    case Key::Tab: focus_image_step(1); break;
    case Key::BackTab: focus_image_step(-1); break;
    case Key::Enter:
        if (e.repeat) break;
        if (img_focus_ >= 0) {
            const ReaderImage &r = rimgs_[static_cast<std::size_t>(img_focus_)];
            if (r.state == ImageState::Ready) open_viewer(img_focus_);
            else notice(r.state == ImageState::Loading || r.state == ImageState::Downloading ? std::string("The image is still loading") : image_caption(r.state), false);
        }
        break;
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

/* The options of the article shown: Text size (the choice with arrows), Save for offline or Remove from saved, Mark as
 * unread (a feed's article), Article info. */
void App::reader_options()
{
    if (reader_idx_ < 0) return;
    const TextSize ts = store_.settings().text_size;
    const bool saved = store_.is_saved(reader_art_.id);
    const bool feed = !reader_src_.saved;
    enum { kText, kSave, kUnread, kInfo };
    std::vector<int> what = {kText, kSave};
    std::vector<std::string> items = {"Text size", saved ? "Remove from saved" : "Save for offline"};
    std::vector<std::string> values = {text_size_label(ts), ""};
    std::vector<Mark> marks = {Mark::Chevron, Mark::Dot};
    if (feed) {
        what.push_back(kUnread);
        items.push_back("Mark as unread");
        values.push_back("");
        marks.push_back(Mark::Dot);
    }
    what.push_back(kInfo);
    items.push_back("Article info");
    values.push_back("");
    marks.push_back(Mark::Chevron);
    open_menu("Options", items, [this, what, saved](int pick) {
        if (pick < 0 || pick >= static_cast<int>(what.size())) return;
        switch (what[static_cast<std::size_t>(pick)]) {
        case kText:
            open_choice(text_size_choice(store_.settings().text_size), [this](int idx) {
                Settings s = store_.settings();
                s.text_size = text_size_from_index(idx);
                store_.set_settings(s);
                const int scroll = lv_obj_get_scroll_y(reader_);
                const int before = std::max(1, static_cast<int>(lv_obj_get_scroll_bottom(reader_)) + scroll);
                render_reader_text();
                const int after = static_cast<int>(lv_obj_get_scroll_bottom(reader_)) + lv_obj_get_scroll_y(reader_);
                lv_obj_scroll_to_y(reader_, static_cast<int>(std::lround(static_cast<double>(scroll) * after / before)), LV_ANIM_OFF);
                update_image_focus();
                notice("Text size: " + text_size_label(s.text_size));
            });
            break;
        case kSave:
            if (!saved) {
                save_offline(reader_art_, reader_feed_id_, reader_feed_name_);
            } else {
                const bool from_saved = reader_src_.saved;
                ask_remove_saved(reader_art_.id, reader_art_.title, [this, from_saved] {
                    reader_saved_id_.clear();
                    reader_saved_files_.clear();
                    if (from_saved) go_up();       // its copy is gone: back to the saved list
                });
            }
            break;
        case kUnread: {
            const std::string id = reader_art_.id;
            store_.set_read(reader_src_.feed_id, id, false);
            if (reader_src_.feed_id == art_feed_.id)
                for (StoredArticle &x : *arts_)
                    if (x.article.id == id) x.read = false;
            notice("Marked as unread");
            break;
        }
        default: open_info("Article info", article_info(reader_art_, reader_feed_name_, now_epoch())); break;
        }
        invalidate();
    }, values, marks);
}

} // namespace rssui
