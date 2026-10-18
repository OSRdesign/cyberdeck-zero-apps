/*
 * SPDX-License-Identifier: MIT
 *
 * RSS Reader: an image of the article full screen. Black over the whole screen (the header too: the toolkit has no
 * full-screen view, so it is the app's own layer above its shell), the picture fitted to the screen and centred, and a
 * faint line at the bottom, "Esc or tap to close  ·  1 / 2", that fades after 2 s and comes back with any key. Esc or a
 * tap closes; Left / Right, or a swipe, show the previous / next image of the article; no zoom.
 *
 * The thumbnail shows at once (scaled to fit); the picture is decoded again for the screen on a worker thread (bounded by
 * the screen, not by the picture: the decoders stream or scale while decoding, docs/IMAGES.md) and replaces it. Only one
 * full-screen picture is alive: the next one, or closing, frees it. Its bytes come from the saved copy or the image cache
 * (the network only when neither has them and Download images allows it): a saved article shows offline.
 */
#include "rss_app.hpp"

#include "app_style.hpp"
#include "hooks.hpp"

#include "cp0_ui_metrics_lvgl.h"

#include <algorithm>
#include <cstdlib>

using namespace rssreader;

namespace rssui {

namespace {

constexpr std::uint64_t kViewerCaptionMs = 2000, kViewerFadeMs = 400;
constexpr std::uint32_t kViewerBg = 0xFFFFFF;    // a transparent part: the same as the thumbnails

void viewer_event_cb(lv_event_t *e)
{
    auto *self = static_cast<App *>(lv_event_get_user_data(e));
    if (!self) return;
    lv_point_t p = {0, 0};
    if (lv_indev_t *indev = lv_event_get_indev(e)) lv_indev_get_point(indev, &p);
    switch (lv_event_get_code(e)) {
    case LV_EVENT_PRESSED: self->viewer_pressed(0, p.x, p.y); break;
    case LV_EVENT_RELEASED: self->viewer_pressed(2, p.x, p.y); break;
    case LV_EVENT_PRESS_LOST: self->viewer_pressed(3, p.x, p.y); break;
    default: break;
    }
}

} // namespace

void App::build_viewer()
{
    const Geo g = geo();
    viewer_ = make_box(plat_.screen(), 0, 0, layout_.w, layout_.h);
    lv_obj_set_style_bg_color(viewer_, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(viewer_, LV_OPA_COVER, 0);
    lv_obj_add_flag(viewer_, LV_OBJ_FLAG_CLICKABLE);
    for (lv_event_code_t code : {LV_EVENT_PRESSED, LV_EVENT_RELEASED, LV_EVENT_PRESS_LOST})
        lv_obj_add_event_cb(viewer_, viewer_event_cb, code, this);
    viewer_img_ = lv_image_create(viewer_);
    lv_obj_remove_flag(viewer_img_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(viewer_img_, layout_.w, layout_.h);
    lv_obj_set_pos(viewer_img_, 0, 0);
    lv_image_set_inner_align(viewer_img_, LV_IMAGE_ALIGN_CONTAIN);
    const int lh = line_h(f_caption_);
    viewer_cap_ = make_label(viewer_, f_caption_, kMuted, g.inset, layout_.h - lh - g.s, layout_.w - 2 * g.inset, lh);
    set_hidden(viewer_, true);
}

std::vector<int> App::viewer_list() const
{
    std::vector<int> v;
    for (std::size_t i = 0; i < rimgs_.size(); ++i)
        if (rimgs_[i].state == ImageState::Ready && rimgs_[i].pix) v.push_back(static_cast<int>(i));
    return v;
}

void App::open_viewer(int index)
{
    if (index < 0 || index >= static_cast<int>(rimgs_.size()) || !rimgs_[static_cast<std::size_t>(index)].pix) return;
    if (!viewer_) build_viewer();
    hold_.cancel();
    set_hidden(hold_box_, true);
    set_hidden(viewer_, false);
    lv_obj_move_foreground(viewer_);
    viewer_down_ = false;
    viewer_show(index);
    invalidate();
}

void App::close_viewer()
{
    if (viewer_job_) viewer_job_->cancel.store(true);
    viewer_job_.reset();
    ++viewer_seq_;
    if (viewer_img_) lv_image_set_src(viewer_img_, nullptr);
    viewer_pix_.reset();                   // the full-screen picture goes with the view
    viewer_idx_ = -1;
    viewer_down_ = false;
    set_hidden(viewer_, true);
    update_image_focus();
    invalidate();
}

/* The image `index` of the article: its thumbnail now, the screen-sized picture when decoded. */
void App::viewer_show(int index)
{
    if (viewer_job_) viewer_job_->cancel.store(true);
    viewer_job_.reset();
    const unsigned seq = ++viewer_seq_;
    ReaderImage &r = rimgs_[static_cast<std::size_t>(index)];
    viewer_idx_ = index;
    img_focus_ = index;
    lv_image_set_src(viewer_img_, &r.dsc);             // the thumbnail first (the old full picture is let go below)
    viewer_pix_.reset();
    const std::vector<int> list = viewer_list();
    const int pos = static_cast<int>(std::find(list.begin(), list.end(), index) - list.begin());
    lv_label_set_text(viewer_cap_, viewer_caption(pos, static_cast<int>(list.size())).c_str());
    viewer_cap_ms_ = mono_ms();
    lv_obj_set_style_opa(viewer_cap_, LV_OPA_COVER, 0);
    // the thumbnail is the whole picture already (a small one): nothing more to decode
    if (r.pix->src_width <= r.pix->width && r.pix->src_height <= r.pix->height) return;
    const std::string url = r.src.url;
    auto f = reader_saved_files_.find(url);
    const std::string file = f == reader_saved_files_.end() ? std::string() : f->second;
    const std::string saved_id = reader_saved_id_;
    const bool net = store_.settings().image_download == ImageDownload::Always;
    const int w = layout_.w, h = layout_.h;
    Store *store = &store_;
    ImageCache *cache = &image_cache_;
    HttpClient *http = http_.get();
    viewer_job_ = jobs_.start([this, seq, index, url, file, saved_id, net, w, h, store, cache, http](Jobs::Token &tok) {
        std::string bytes, type;
        bool got = local_image(store, cache, saved_id, file, url, bytes, type);
        if (!got && net) {
            FetchError err = FetchError::None;
            got = fetch_to_cache(http, cache, url, &tok.cancel, bytes, type, err);
        }
        if (!got || tok.cancel.load()) return;
        auto pix = std::make_shared<DecodedImage>();
        if (decode_image(bytes, w, h, kViewerBg, *pix, &tok.cancel) != DecodeStatus::Ok) return;
        jobs_.post([this, seq, index, pix] {
            if (seq != viewer_seq_ || viewer_idx_ != index || !viewer_open()) return;
            viewer_pix_ = pix;
            viewer_dsc_ = lv_image_dsc_t{};
            viewer_dsc_.header.magic = LV_IMAGE_HEADER_MAGIC;
            viewer_dsc_.header.cf = LV_COLOR_FORMAT_RGB565;
            viewer_dsc_.header.w = static_cast<uint32_t>(pix->width);
            viewer_dsc_.header.h = static_cast<uint32_t>(pix->height);
            viewer_dsc_.header.stride = static_cast<uint32_t>(pix->width * 2);
            viewer_dsc_.data_size = static_cast<uint32_t>(pix->rgb565.size());
            viewer_dsc_.data = pix->rgb565.data();
            lv_image_set_src(viewer_img_, &viewer_dsc_);
            viewer_job_.reset();
            invalidate();
        });
    });
}

void App::viewer_step(int dir)
{
    const std::vector<int> list = viewer_list();
    auto it = std::find(list.begin(), list.end(), viewer_idx_);
    const int pos = it == list.end() ? 0 : static_cast<int>(it - list.begin());
    const int next = pos + dir;
    viewer_cap_ms_ = mono_ms();
    lv_obj_set_style_opa(viewer_cap_, LV_OPA_COVER, 0);
    if (next < 0 || next >= static_cast<int>(list.size())) return;     // the end: the line ("2 / 2") says where it is
    viewer_show(list[static_cast<std::size_t>(next)]);
    invalidate();
}

void App::viewer_key(const KeyEvent &e)
{
    switch (e.key) {
    case Key::Esc:
        if (!e.repeat) close_viewer();
        return;
    case Key::Left:
        viewer_step(-1);
        return;
    case Key::Right:
        viewer_step(1);
        return;
    default:
        viewer_cap_ms_ = mono_ms();                // any other key: the line comes back
        lv_obj_set_style_opa(viewer_cap_, LV_OPA_COVER, 0);
        return;
    }
}

/* A tap closes, a swipe goes to the previous / next image (a finger moving left shows the next one). */
void App::viewer_pressed(int what, int x, int y)
{
    if (!viewer_open()) return;
    if (what == 0) {
        viewer_down_ = true;
        viewer_px_ = x;
        viewer_py_ = y;
        return;
    }
    if (what == 3) {
        viewer_down_ = false;
        return;
    }
    if (!viewer_down_) return;
    viewer_down_ = false;
    const int dx = x - viewer_px_, dy = y - viewer_py_;
    const int swipe = std::max(40, layout_.w / 8), slop = 16;
    if (std::abs(dx) >= swipe && std::abs(dx) > std::abs(dy)) {
        post([this, dx] {
            if (viewer_open()) viewer_step(dx < 0 ? 1 : -1);
        });
    } else if (std::abs(dx) <= slop && std::abs(dy) <= slop) {
        post([this] {
            if (viewer_open()) close_viewer();
        });
    }
}

void App::viewer_tick(std::uint64_t now)
{
    if (!viewer_open() || !viewer_cap_) return;
    const std::uint64_t t = now - viewer_cap_ms_;
    lv_opa_t opa = LV_OPA_COVER;
    if (t >= kViewerCaptionMs + kViewerFadeMs) opa = LV_OPA_TRANSP;
    else if (t > kViewerCaptionMs) opa = static_cast<lv_opa_t>(255 - (t - kViewerCaptionMs) * 255 / kViewerFadeMs);
    if (lv_obj_get_style_opa(viewer_cap_, LV_PART_MAIN) != opa) lv_obj_set_style_opa(viewer_cap_, opa, 0);
}

} // namespace rssui
