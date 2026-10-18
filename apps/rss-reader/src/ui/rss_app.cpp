/*
 * SPDX-License-Identifier: MIT
 *
 * RSS Reader: the shell (header, the grey line, the page bodies), the navigation, the keys, the 3 s hold, the on-screen
 * keyboard and the dialogs. The pages are in pages_lists.cpp, page_reader.cpp (with page_viewer.cpp, the full-screen
 * image), page_browse.cpp, page_add.cpp and page_settings.cpp.
 */
#include "rss_app.hpp"

#include "app.hpp"
#include "app_style.hpp"
#include "build_info.hpp"
#include "fsutil.hpp"
#include "hooks.hpp"
#include "osk_policy.hpp"
#include "util.hpp"

#include "cp0_lvgl_app.h"
#include "cp0_ui_metrics_lvgl.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>

namespace rssui {

namespace {

constexpr std::uint32_t kStartGuardMs = 600;   /* the tap or Enter that started the app must not act */
constexpr std::uint32_t kNoticeMs = 4000;
constexpr int kColumnCapW = 960;               /* the list pages' column (Mesh Hop's and Net Toolbox's cap) */

App *g_app = nullptr;

void trampoline_button(lv_event_t *e)
{
    if (!g_app || lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    g_app->button_clicked(static_cast<int>(reinterpret_cast<intptr_t>(lv_event_get_user_data(e))));
}

void trampoline_popitem(lv_event_t *e)
{
    if (!g_app || lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    g_app->popup_pick(static_cast<int>(reinterpret_cast<intptr_t>(lv_event_get_user_data(e))));
}

void log_line(const std::string &s)
{
    std::fprintf(stderr, "[rss-reader] %s\n", s.c_str());
}

std::string font_file(const char *name)
{
    if (const char *d = std::getenv("RSSREADER_FONT_DIR"); d && *d) {
        const std::string p = std::string(d) + "/" + name;
        if (::access(p.c_str(), R_OK) == 0) return p;
    }
    const std::string rel = std::string("share/font/") + name;
    if (const char *own = cp0_file_path_c(rel.c_str()); own && own[0] && ::access(own, R_OK) == 0) return own;
    const std::string installed = std::string("/usr/share/APPLaunch/") + rel;
    if (::access(installed.c_str(), R_OK) == 0) return installed;
    return "";
}

std::string catalog_path()
{
    if (const char *c = std::getenv("RSSREADER_CATALOG"); c && *c) return c;
    const std::string installed = "/usr/share/APPLaunch/share/rss-reader/catalog.json";
    if (::access(installed.c_str(), R_OK) == 0) return installed;
#ifdef CDK_APP_DIR
    return std::string(CDK_APP_DIR) + "/data/catalog.json";
#else
    return installed;
#endif
}

} // namespace

/* ================================================================== construction */

App::App(Platform &plat) : plat_(plat), store_(rssreader::Store::default_root())
{
    g_app = this;
    start_ms_ = mono_ms();
    if (!store_.open()) log_line("cannot open the data folder " + store_.root());
    const std::string cat = catalog_path();
    catalog_ok_ = catalog_.load_file(cat);
    log_line(std::string("0.1 (") + CDK_APP_VERSION + "), data " + store_.root() + ", catalog " + cat + ": " +
             (catalog_ok_ ? std::to_string(catalog_.size()) + " feeds" : std::string("NOT LOADED")));
    http_ = hooks().make_http ? hooks().make_http() : std::make_unique<rssreader::CurlClient>();

    const cp0_ui_metrics_t *m = cp0_ui_metrics_get();
    cp0_ui_roller_page(m, cp0::ui::status_strip_left(), &layout_);
    load_fonts();
    build_shell();
    build_home();
    build_favorites();
    build_articles();
    build_saved();
    build_reader();
    build_browse();
    build_add();
    build_settings();
    // the cue of a held row: a box with a bar that fills over 3 s (never clickable: it must not take the touch)
    {
        const Geo g = geo();
        hold_box_ = make_box(body_, g.x, 0, g.cw, 10);
        lv_obj_remove_flag(hold_box_, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_bg_color(hold_box_, lv_color_hex(kPanel), 0);
        lv_obj_set_style_bg_opa(hold_box_, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(hold_box_, m->tok[CP0_TOK_RADIUS_M], 0);
        lv_obj_set_style_border_width(hold_box_, 2, 0);
        lv_obj_set_style_border_color(hold_box_, lv_color_hex(kAccent), 0);
        const int lh = line_h(f_caption_), pad = g.s;
        hold_label_ = make_label(hold_box_, f_caption_, kText, pad, pad - 2, g.cw - 2 * pad - 4, lh);
        hold_track_w_ = std::max(1, g.cw - 2 * pad - 4);
        hold_track_ = make_box(hold_box_, pad, pad + lh + pad / 2 - 2, hold_track_w_, pad);
        lv_obj_remove_flag(hold_track_, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_bg_color(hold_track_, lv_color_hex(kSelected), 0);
        lv_obj_set_style_bg_opa(hold_track_, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(hold_track_, LV_RADIUS_CIRCLE, 0);
        hold_bar_ = make_box(hold_track_, 0, 0, 1, pad);
        lv_obj_remove_flag(hold_bar_, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_bg_color(hold_bar_, lv_color_hex(kAccent), 0);
        lv_obj_set_style_bg_opa(hold_bar_, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(hold_bar_, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_height(hold_box_, pad + lh + pad / 2 + pad + pad);
        set_hidden(hold_box_, true);
    }
    plat_.on_key = [this](const KeyEvent &e) { on_key(e); };
    inset_listener_ = cp0::ui::osk::add_inset_listener([this](int inset) { on_osk_inset(inset); });
    render();
}

App::~App()
{
    plat_.on_key = nullptr;
    jobs_.cancel_all();
    jobs_.join_all();
    jobs_.clear_posted();
    if (viewer_) lv_obj_delete(viewer_);       // the full-screen layer is the screen's child, not the shell's
    viewer_ = viewer_img_ = viewer_cap_ = nullptr;
    if (inset_listener_) cp0::ui::osk::remove_inset_listener(inset_listener_);
    inset_listener_ = 0;
    release_osk();
    dialog_.reset();
    shell_.reset();
    for (lv_font_t *f : owned_fonts_) lv_freetype_font_delete(f);
    owned_fonts_.clear();
    g_app = nullptr;
}

/* A FreeType face at px with DejaVu Sans and Noto Sans CJK behind it (the text of feeds: accents, typographic marks,
 * Japanese), from the launcher's font folder; the builtin Montserrat when the face is missing (no fallback then). */
const lv_font_t *App::font(const char *face, int px)
{
    const std::string key = std::string(face) + ":" + std::to_string(px);
    if (auto it = fonts_.find(key); it != fonts_.end()) return it->second;
    auto create = [this](const char *name, int size) -> lv_font_t * {
        const std::string path = font_file(name);
        if (path.empty()) return nullptr;
        lv_font_t *f = lv_freetype_font_create(path.c_str(), LV_FREETYPE_FONT_RENDER_MODE_BITMAP, static_cast<uint32_t>(size),
                                               LV_FREETYPE_FONT_STYLE_NORMAL);
        if (f) owned_fonts_.push_back(f);
        return f;
    };
    // the fallback chain of this size, shared by every face of it
    const std::string chain_key = "chain:" + std::to_string(px);
    lv_font_t *chain = nullptr;
    if (auto it = fonts_.find(chain_key); it != fonts_.end()) {
        chain = const_cast<lv_font_t *>(it->second);
    } else {
        lv_font_t *dejavu = create("DejaVuSans.ttf", px);
        lv_font_t *cjk = create("NotoSansCJK-Regular.ttc", px);
        if (dejavu) dejavu->fallback = cjk;
        chain = dejavu ? dejavu : cjk;
        fonts_[chain_key] = chain;
    }
    const lv_font_t *result = nullptr;
    if (lv_font_t *f = create(face, px)) {
        f->fallback = chain;
        result = f;
    } else {
        result = cp0_ui_font_px(px);
    }
    fonts_[key] = result;
    return result;
}

void App::load_fonts()
{
    const cp0_ui_metrics_t *m = cp0_ui_metrics_get();
    const int body = m->text_px[CP0_TEXT_BODY], heading = m->text_px[CP0_TEXT_HEADING], caption = m->text_px[CP0_TEXT_CAPTION];
    const int label = cp0_ui_roller_font_px((body + heading) / 2);          // the lists' labels (Mesh Hop's Settings size)
    f_row_ = font("Montserrat-Medium.ttf", label);
    f_row_bold_ = font("Montserrat-Bold.ttf", label);
    f_body_ = font("Montserrat-Medium.ttf", body);
    f_caption_ = font("Montserrat-Medium.ttf", caption);
    f_value_ = font("Montserrat-Medium.ttf", cp0_ui_roller_font_px((body * 9 + 5) / 10));
    f_field_ = font("DejaVuSans.ttf", body);
    f_title_ = font("Montserrat-Medium.ttf", layout_.title_px);
}

lv_obj_t *App::make_label(lv_obj_t *parent, const lv_font_t *f, std::uint32_t color, int x, int y, int w, int h)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, f, 0);
    set_color(l, color);
    lv_label_set_text(l, "");
    lv_obj_set_pos(l, x, y);
    lv_obj_set_size(l, std::max(1, w), std::max(1, h));
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_remove_flag(l, LV_OBJ_FLAG_CLICKABLE);
    return l;
}

/* A button of a page: the ROW token high (7 mm), the toolkit's radius; primary: the keyboard's Enter blue. */
lv_obj_t *App::make_button(lv_obj_t *parent, const char *text, int tag, bool primary)
{
    lv_obj_t *b = lv_button_create(parent);
    if (lv_obj_get_group(b)) lv_group_remove_obj(b);
    lv_obj_remove_flag(b, LV_OBJ_FLAG_CLICK_FOCUSABLE);
    lv_obj_set_style_radius(b, layout_.esc_radius, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(primary ? kPrimary : kSelected), 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(primary ? CP0_UI_OSK_ENTER_PRESSED_RGB : kPressed), LV_STATE_PRESSED);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_set_style_border_width(b, 0, 0);
    lv_obj_set_style_border_color(b, lv_color_hex(kText), 0);
    lv_obj_add_event_cb(b, trampoline_button, LV_EVENT_CLICKED, reinterpret_cast<void *>(static_cast<intptr_t>(tag)));
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_font(l, f_body_, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(0xFFFFFF), 0);
    lv_obj_center(l);
    return b;
}

namespace {
constexpr std::uint32_t kFieldBg = 0x1C1C1C;
constexpr std::uint32_t kFieldBorder = 0x3A3A3A;
constexpr std::uint32_t kFieldEditing = 0x2D9CDB;
constexpr std::uint32_t kFieldText = 0xEEEEEE;
constexpr std::uint32_t kFieldPlaceholder = 0x777777;
} // namespace

/* A one-line text field drawn as the toolkit's cp0::ui::TextField (its colours, the border that turns blue while it is
 * typed into, the steady cursor); the app types into it itself (the key bridge: US or FR layout), as Mesh Hop does. */
lv_obj_t *App::make_text_field(lv_obj_t *parent, int tag, std::size_t max_len, int h, const char *placeholder)
{
    const cp0_ui_metrics_t *m = cp0_ui_metrics_get();
    lv_obj_t *ta = lv_textarea_create(parent);
    if (lv_obj_get_group(ta)) lv_group_remove_obj(ta);
    lv_obj_remove_flag(ta, LV_OBJ_FLAG_CLICK_FOCUSABLE);
    lv_obj_remove_flag(ta, LV_OBJ_FLAG_SCROLL_ON_FOCUS);
    lv_textarea_set_one_line(ta, true);
    lv_obj_set_height(ta, h);
    lv_obj_set_scrollbar_mode(ta, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_text_font(ta, f_field_, 0);
    lv_obj_set_style_text_color(ta, lv_color_hex(kFieldText), 0);
    lv_obj_set_style_bg_color(ta, lv_color_hex(kFieldBg), 0);
    lv_obj_set_style_bg_opa(ta, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(ta, m->tok[CP0_TOK_RADIUS_S], 0);
    lv_obj_set_style_border_width(ta, 2, 0);
    lv_obj_set_style_border_color(ta, lv_color_hex(kFieldBorder), 0);
    lv_obj_set_style_border_opa(ta, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_hor(ta, std::max(layout_.gap, h / 5), 0);
    lv_obj_set_style_pad_ver(ta, std::max(0, (h - 4 - line_h(f_field_)) / 2), 0);
    lv_obj_set_style_text_color(ta, lv_color_hex(kFieldPlaceholder), LV_PART_TEXTAREA_PLACEHOLDER);
    lv_obj_set_style_bg_opa(ta, LV_OPA_TRANSP, LV_PART_CURSOR);
    lv_obj_set_style_border_side(ta, LV_BORDER_SIDE_LEFT, LV_PART_CURSOR | LV_STATE_FOCUSED);
    lv_obj_set_style_border_width(ta, 2, LV_PART_CURSOR | LV_STATE_FOCUSED);
    lv_obj_set_style_border_color(ta, lv_color_hex(kFieldEditing), LV_PART_CURSOR | LV_STATE_FOCUSED);
    lv_obj_set_style_anim_duration(ta, 0, LV_PART_CURSOR);
    lv_textarea_set_max_length(ta, static_cast<uint32_t>(max_len));
    if (placeholder) lv_textarea_set_placeholder_text(ta, placeholder);
    if (tag) lv_obj_add_event_cb(ta, trampoline_button, LV_EVENT_CLICKED, reinterpret_cast<void *>(static_cast<intptr_t>(tag)));
    return ta;
}

void App::style_field(lv_obj_t *ta, bool editing)
{
    if (!ta) return;
    lv_obj_set_style_border_color(ta, lv_color_hex(editing ? kFieldEditing : kFieldBorder), 0);
    if (editing) lv_obj_add_state(ta, LV_STATE_FOCUSED);
    else lv_obj_remove_state(ta, LV_STATE_FOCUSED);
}

App::Geo App::geo() const
{
    const cp0_ui_metrics_t *m = cp0_ui_metrics_get();
    Geo g;
    g.w = layout_.w;
    g.inset = m->tok[CP0_TOK_SPACE_L];
    g.s = m->tok[CP0_TOK_SPACE_S];
    g.m = m->tok[CP0_TOK_SPACE_M];
    const PageColumn c = page_column(layout_.w, g.inset, kScreenW, kColumnCapW);
    g.x = c.x;
    g.cw = c.w;
    g.lx = g.x - g.inset;
    g.lw = g.cw + 2 * g.inset;
    g.h = panel_h_;
    g.row = std::max(m->tok[CP0_TOK_ROW_H], line_h(f_row_) + g.s);
    g.bar = std::max(m->tok[CP0_TOK_ROW_H], line_h(f_body_) + line_h(f_caption_) + g.s);
    g.two_line = std::max(m->tok[CP0_TOK_ROW_H], line_h(f_row_) + line_h(f_caption_) + g.s);
    return g;
}

RowList::Config App::list_config(int max_cells) const
{
    const Geo g = geo();
    RowList::Config cfg;
    cfg.max_cells = max_cells;
    cfg.min_row_h = g.row;
    cp0_ui_list_layout(cp0_ui_metrics_get(), CP0_UI_LIST_COMFORTABLE, g.lx, 0, g.lw, std::max(1, full_h_), &cfg.marks);
    cfg.marks.text_px = cp0_ui_roller_font_px((cp0_ui_metrics_get()->text_px[CP0_TEXT_BODY] + cp0_ui_metrics_get()->text_px[CP0_TEXT_HEADING]) / 2);
    cfg.lead_x = cfg.marks.inset;
    return cfg;
}

void App::make_list(RowList &list, lv_obj_t *parent, int y, int h, int max_cells)
{
    const Geo g = geo();
    list.create(parent, g.lx, y, g.lw, std::max(1, h), list_config(max_cells));
    list.on_click = [this, &list](int index) { row_clicked(&list, index); };
    list.on_press = [this, &list](int what, int index, int x, int y2) { row_pressed(&list, what, index, x, y2); };
}

/* The shell: cp0::ui::Page (the header: the title, the Esc button, the shared status strip) with the title in the
 * launcher's Montserrat as FreeType (feed names have accents and Japanese), the grey line under it, the page bodies. */
void App::build_shell()
{
    const cp0_ui_metrics_t *m = cp0_ui_metrics_get();
    const int cap_lh = line_h(f_caption_);
    layout_.caption_y = layout_.body_y + m->tok[CP0_TOK_SPACE_S] / 4;
    body_y_ = layout_.caption_y + cap_lh + m->tok[CP0_TOK_SPACE_S] / 2;
    full_h_ = layout_.h - body_y_;
    panel_h_ = full_h_;
    const lv_font_t *title_font = f_title_;
    shell_ = std::make_unique<cp0::ui::Page>(
        plat_.screen(), layout_,
        [title_font](lv_obj_t *parent, const char *text) {
            lv_obj_t *t = cp0::ui::add_title(parent, text);
            if (title_font) lv_obj_set_style_text_font(t, title_font, 0);
            return t;
        },
        [this] { header_back(); });
    cp0::ui::StatusStrip::add(shell_->header());
    shell_->set_title({"RSS Reader"});
    cap_ = make_label(shell_->root(), f_caption_, kMuted, layout_.caption_x, layout_.caption_y, std::max(1, layout_.caption_w), cap_lh);
    body_ = make_box(shell_->root(), 0, body_y_, layout_.w, panel_h_);
    lv_obj_remove_flag(body_, LV_OBJ_FLAG_CLICKABLE);
}

lv_obj_t *App::make_page()
{
    lv_obj_t *p = make_box(body_, 0, 0, layout_.w, panel_h_);
    lv_obj_set_style_bg_color(p, lv_color_hex(kBackground), 0);
    lv_obj_set_style_bg_opa(p, LV_OPA_COVER, 0);
    lv_obj_remove_flag(p, LV_OBJ_FLAG_CLICKABLE);
    set_hidden(p, true);
    return p;
}

/* The room under the shell changed (the on-screen keyboard came or went): the pages end above the keyboard. */
void App::layout_pages()
{
    panel_h_ = std::max(1, full_h_ - osk_inset_);
    lv_obj_set_height(body_, panel_h_);
    for (auto &kv : pages_) lv_obj_set_height(kv.second, panel_h_);
    home_list_.resize(panel_h_);
    fav_list_.resize(std::max(1, panel_h_ - lv_obj_get_y(fav_list_.obj())));
    art_list_.resize(std::max(1, panel_h_ - lv_obj_get_y(art_list_.obj())));
    saved_list_.resize(std::max(1, panel_h_ - saved_footer_h_ - lv_obj_get_y(saved_list_.obj())));
    if (saved_footer_) lv_obj_set_y(saved_footer_, panel_h_ - saved_footer_h_);
    settings_list_.resize(panel_h_);
    browse_list_.resize(std::max(1, panel_h_ - lv_obj_get_y(browse_list_.obj())));
    lv_obj_set_height(reader_, panel_h_);
    layout_add();
    invalidate();
}

/* ================================================================== navigation */

void App::open(PageId p)
{
    if (popup_open()) return;
    hold_.cancel();
    stack_.push_back(p);
    switch (p) {
    case PageId::Favorites: {
        subs_ = store_.subscriptions();
        fav_sel_ = 0;
        fav_sel_id_.clear();
        fav_list_.scroll_to_top();
        // Refresh when opening (Settings): the whole list, unless it was updated in the last 10 minutes
        const rssreader::Settings st = store_.settings();
        std::int64_t last = 0;
        for (const rssreader::Subscription &s : subs_) last = std::max(last, s.last_attempt);
        if (st.refresh_on_open && !subs_.empty() && !refreshing_ && now_epoch() - std::max(last, last_auto_refresh_) >= 600) {
            last_auto_refresh_ = now_epoch();
            start_refresh(subs_, true);
        }
        break;
    }
    case PageId::Browse:
        blevel_ = BrowseLevel::Countries;
        bcountry_.clear();
        btopic_.clear();
        bsel_stack_.clear();
        browse_sel_ = 0;
        search_text_.clear();
        search_typing_ = false;
        lv_textarea_set_text(search_ta_, "");
        browse_list_.scroll_to_top();
        break;
    case PageId::Add: add_reset(); break;
    case PageId::Saved:
        saved_ = store_.list_saved();
        saved_sel_ = 0;
        saved_list_.scroll_to_top();
        break;
    case PageId::Reader: reader_keys_ = false; break;
    case PageId::Settings:
        settings_sel_ = 0;
        settings_list_.scroll_to_top();
        break;
    default: break;
    }
    invalidate();
}

void App::go_up()
{
    if (stack_.size() <= 1) return;
    const PageId leaving = stack_.back();
    hold_.cancel();
    switch (leaving) {
    case PageId::Favorites: stop_refresh(); break;          // leaving the page cancels its refresh
    case PageId::Articles:
        if (load_job_) load_job_->cancel.store(true);
        ++load_seq_;
        arts_loading_ = false;
        break;
    case PageId::Add:
        if (add_job_) add_job_->cancel.store(true);
        add_typing_ = false;
        break;
    case PageId::Browse: search_typing_ = false; break;
    case PageId::Reader:
        if (viewer_open()) close_viewer();
        reader_images_stop();              // the downloads stop, the thumbnails are freed
        break;
    default: break;
    }
    stack_.pop_back();
    if (page() == PageId::Favorites) subs_ = store_.subscriptions();
    if (page() == PageId::Saved) {
        saved_ = store_.list_saved();
        saved_sel_ = std::clamp(saved_sel_, 0, std::max(0, static_cast<int>(saved_.size()) - 1));
    }
    invalidate();
}

/* Esc (a key) and the header's Esc button: back one step. A typing field with the on-screen keyboard up stops first;
 * Browse goes up a level; Home asks "Quit RSS Reader?" (the guidelines: a short Esc at the root ends the app). */
void App::back()
{
    if (popup_open()) {
        popup_cancel();
        return;
    }
    switch (page()) {
    case PageId::Home: ask_quit(); return;
    case PageId::Add:
        if (add_typing_ && osk_inset_ > 0) {
            add_typing_ = false;
            invalidate();
            return;
        }
        if (add_state_ == AddState::Working) {
            if (add_job_) add_job_->cancel.store(true);
            return;
        }
        if (add_state_ == AddState::Found) {
            add_state_ = AddState::Idle;
            add_typing_ = true;
            invalidate();
            return;
        }
        break;
    case PageId::Browse:
        if (search_typing_ && osk_inset_ > 0) {
            search_typing_ = false;
            invalidate();
            return;
        }
        if (browse_back()) return;
        break;
    default: break;
    }
    go_up();
}

void App::header_back()
{
    post([this] {
        if (mono_ms() - start_ms_ < kStartGuardMs) return;
        back();
    });
}

void App::ask_quit()
{
    if (popup_open()) return;
    open_confirm("Quit RSS Reader?", "Your favorites, what you have read and your settings are kept.", [] { cdk::request_quit(0); }, "Quit",
                 "Cancel");
}

void App::post(std::function<void()> fn)
{
    posted_.push_back(std::move(fn));
}

void App::show_page()
{
    const PageId p = page();
    for (auto &kv : pages_) set_hidden(kv.second, kv.first != p);
}

void App::update_title()
{
    std::vector<std::string> path;
    switch (page()) {
    case PageId::Home: path = {"RSS Reader"}; break;
    case PageId::Favorites: path = {"Favorites"}; break;
    case PageId::Articles: path = {art_feed_.name.empty() ? std::string("Articles") : art_feed_.name}; break;
    case PageId::Reader: path = {reader_feed_name_.empty() ? std::string("Reading") : reader_feed_name_}; break;
    case PageId::Browse: path = browse_path(); break;
    case PageId::Saved: path = {"Saved articles"}; break;
    case PageId::Add: path = {"Add a feed"}; break;
    case PageId::Settings: path = {"Settings"}; break;
    }
    const lv_font_t *tf = f_title_;
    const std::string title = fit_title(path, [tf](const std::string &t) { return text_w(t, tf); }, layout_.title_max_w);
    if (title == title_key_) return;
    title_key_ = title;
    shell_->set_title({title});
}

void App::set_caption(const std::string &text)
{
    const bool show_notice = !notice_text_.empty();
    const std::string &t = show_notice ? notice_text_ : text;
    if (std::string(lv_label_get_text(cap_)) != t) lv_label_set_text(cap_, t.c_str());
    set_color(cap_, show_notice ? (notice_ok_ ? kOk : kWarn) : kMuted);
}

void App::notice(const std::string &text, bool ok)
{
    notice_text_ = text;
    notice_ok_ = ok;
    notice_ms_ = mono_ms();
    log_line("notice: " + text);
    invalidate();
}

/* ================================================================== tick and render */

void App::tick()
{
    while (!posted_.empty()) {
        std::vector<std::function<void()>> now;
        now.swap(posted_);
        for (auto &fn : now) fn();
    }
    jobs_.run_posted();
    const std::uint64_t now = mono_ms();
    if (pop_dirty_) {                              // the widgets of a new dialog are built here, never inside the click of another
        pop_dirty_ = false;
        popup_build();
        popup_refresh();
    }
    hold_tick(now);
    viewer_tick(now);
    if (!notice_text_.empty() && now > notice_ms_ && now - notice_ms_ >= kNoticeMs) {
        notice_text_.clear();
        dirty_ = true;
    }
    if (now - last_render_ms_ >= 1000 && !plat_.touching()) dirty_ = true;      // the ages move on; never under a finger
    if (dirty_) render();
}

void App::render()
{
    last_render_ms_ = mono_ms();
    dirty_ = false;
    show_page();
    switch (page()) {
    case PageId::Home: render_home(); break;
    case PageId::Favorites: render_favorites(); break;
    case PageId::Articles: render_articles(); break;
    case PageId::Reader: set_caption(reader_caption(reader_feed_name_, reader_art_, now_epoch())); break;
    case PageId::Browse: render_browse(); break;
    case PageId::Saved: render_saved(); break;
    case PageId::Add: render_add(); break;
    case PageId::Settings: render_settings(); break;
    }
    update_title();
    sync_osk();
}

/* ================================================================== the on-screen keyboard */

void App::release_osk()
{
    if (osk_field_) cp0::ui::osk::release(osk_field_);
    osk_field_ = nullptr;
}

/* The field the state says is being typed into asks for the keyboard (the shared rule decides whether it shows: never
 * with a physical keyboard); the one before gives it back. */
void App::sync_osk()
{
    lv_obj_t *want = nullptr;
    if (!popup_open()) {
        if (page() == PageId::Add && add_typing_ && add_state_ != AddState::Working) want = add_ta_;
        else if (page() == PageId::Browse && search_typing_ && blevel_ == BrowseLevel::Countries) want = search_ta_;
    }
    style_field(add_ta_, page() == PageId::Add && add_typing_);
    style_field(search_ta_, page() == PageId::Browse && search_typing_);
    if (want == osk_field_) return;
    release_osk();
    osk_field_ = want;
    if (want) cp0::ui::osk::request(want, cp0::ui::TextType::Text, false);
}

void App::on_osk_inset(int inset)
{
    if (inset == osk_inset_) return;
    osk_inset_ = inset;
    layout_pages();
}

bool App::field_key(lv_obj_t *ta, const KeyEvent &e, std::size_t max_len)
{
    if (!ta) return false;
    switch (e.key) {
    case Key::Backspace: lv_textarea_delete_char(ta); break;
    case Key::Delete: lv_textarea_delete_char_forward(ta); break;
    case Key::Left: lv_textarea_cursor_left(ta); break;
    case Key::Right: lv_textarea_cursor_right(ta); break;
    case Key::Home: lv_textarea_set_cursor_pos(ta, 0); break;
    case Key::End: lv_textarea_set_cursor_pos(ta, LV_TEXTAREA_CURSOR_LAST); break;
    case Key::Char: {
        if (e.ctrl || e.alt || e.text.empty()) return false;
        const char *cur = lv_textarea_get_text(ta);
        if ((cur ? std::strlen(cur) : 0) + e.text.size() > max_len) return true;
        lv_textarea_add_text(ta, e.text.c_str());
        break;
    }
    default: return false;
    }
    invalidate();
    return true;
}

/* ================================================================== keys, taps, the hold */

void App::on_key(const KeyEvent &e)
{
    if (e.key == Key::None) return;
    if (viewer_open()) {                   // the full-screen image takes every key (Esc closes it)
        viewer_key(e);
        return;
    }
    if (popup_open()) {
        popup_key(e);
        return;
    }
    if (mono_ms() - start_ms_ < kStartGuardMs && (e.key == Key::Enter || e.key == Key::Esc)) return;
    if (e.key == Key::Esc) {
        if (!e.repeat) back();
        return;
    }
    if (is_ctrl_letter(e, 'o')) {
        open_options();
        return;
    }
    switch (page()) {
    case PageId::Home: key_home(e); break;
    case PageId::Favorites: key_favorites(e); break;
    case PageId::Articles: key_articles(e); break;
    case PageId::Reader: key_reader(e); break;
    case PageId::Browse: key_browse(e); break;
    case PageId::Saved: key_saved(e); break;
    case PageId::Add: key_add(e); break;
    case PageId::Settings: key_settings(e); break;
    }
}

/* The options of what the page shows (Ctrl+O, or a 3 s hold on a row or on the text). */
void App::open_options()
{
    if (popup_open() || viewer_open()) return;
    switch (page()) {
    case PageId::Favorites:
        if (!subs_.empty()) favorite_options(fav_sel_);
        break;
    case PageId::Articles: article_options(art_sel_); break;
    case PageId::Reader: reader_options(); break;
    case PageId::Saved:
        if (!saved_.empty()) saved_row_options(saved_sel_);
        break;
    default: break;
    }
    if (popup_open()) options_opened();
}

/* The first-run hint shows until the options were opened once (any page): settings.json keeps it. */
bool App::hint_shown()
{
    if (hint_seen_ < 0) hint_seen_ = store_.settings().options_seen ? 1 : 0;
    return hint_seen_ == 0;
}

void App::options_opened()
{
    if (!hint_shown()) return;
    hint_seen_ = 1;
    rssreader::Settings s = store_.settings();
    s.options_seen = true;
    store_.set_settings(s);
    if (page() == PageId::Reader && reader_idx_ >= 0) {      // the hint line of the article goes, the reading position stays
        const int y = lv_obj_get_scroll_y(reader_);
        render_reader_text();
        lv_obj_scroll_to_y(reader_, y, LV_ANIM_OFF);
        update_image_focus();
    }
    invalidate();
}

void App::row_pressed(RowList *list, int what, int index, int x, int y)
{
    if (what == 1) {
        hold_.move(x, y);
        return;
    }
    if (what == 2) {
        if (hold_.visible(mono_ms())) hold_release_ms_ = mono_ms();     // the cue showed: letting go is not a tap
        hold_.cancel();
        return;
    }
    const bool holdable = (list == &fav_list_ && page() == PageId::Favorites) || (list == &art_list_ && page() == PageId::Articles) ||
                          (list == &saved_list_ && page() == PageId::Saved);
    if (!holdable || popup_open() || list->id_at(index).empty()) {
        hold_.cancel();
        hold_.take_swallow();
        return;
    }
    hold_list_ = list;
    hold_index_ = index;
    hold_scroll_y_ = lv_obj_get_scroll_y(list->obj());
    hold_.begin(list->id_at(index), x, y, mono_ms());
}

void App::reader_pressed(int what, int x, int y)
{
    if (what == 1) {
        hold_.move(x, y);
        return;
    }
    if (what == 2) {
        if (hold_.visible(mono_ms())) hold_release_ms_ = mono_ms();
        hold_.cancel();
        return;
    }
    if (popup_open() || page() != PageId::Reader || viewer_open()) return;
    if (reader_keys_) {                    // the touch leads now: no ring
        reader_keys_ = false;
        update_image_focus();
    }
    hold_list_ = nullptr;
    hold_index_ = -1;
    hold_scroll_y_ = lv_obj_get_scroll_y(reader_);
    hold_.begin("reader", x, y, mono_ms());
}

void App::hold_tick(std::uint64_t now)
{
    if (!hold_.active()) {
        set_hidden(hold_box_, true);
        return;
    }
    lv_obj_t *scroller = hold_list_ ? hold_list_->obj() : reader_;
    if (popup_open() || lv_obj_get_scroll_y(scroller) != hold_scroll_y_) {
        hold_.cancel();
        set_hidden(hold_box_, true);
        return;
    }
    if (hold_.poll(now)) {
        set_hidden(hold_box_, true);
        if (hold_list_ == &fav_list_) {
            fav_sel_ = hold_index_;
            fav_sel_id_ = fav_list_.id_at(hold_index_);
            fav_list_.set_selected(fav_sel_, false);
        } else if (hold_list_ == &art_list_) {
            art_sel_ = hold_index_;
            art_sel_id_ = art_list_.id_at(hold_index_);
            art_list_.set_selected(art_sel_, false);
        } else if (hold_list_ == &saved_list_) {
            saved_sel_ = hold_index_;
            saved_list_.set_selected(saved_sel_, false);
        }
        open_options();
        return;
    }
    if (hold_.visible(now)) {
        std::string what = "options";
        if (hold_list_ && hold_list_->row(hold_index_) && !hold_list_->row(hold_index_)->cells.empty())
            what = "options for " + truncate_text(hold_list_->row(hold_index_)->cells[0].text, 28);
        else if (!hold_list_) what = "options of this article";
        lv_label_set_text(hold_label_, ("Keep holding: " + what).c_str());
        lv_obj_set_width(hold_bar_, std::max(1, static_cast<int>(hold_.progress(now) * hold_track_w_)));
        const int h = lv_obj_get_height(hold_box_);
        lv_obj_set_y(hold_box_, std::max(0, panel_h_ - h - geo().s));
        lv_obj_move_foreground(hold_box_);
        set_hidden(hold_box_, false);
    }
}

void App::row_clicked(RowList *list, int index)
{
    if (hold_.take_swallow()) return;              // the release of a completed 3 s hold is not a tap
    if (hold_release_ms_ && mono_ms() - hold_release_ms_ < 300) {   // nor the release of a hold given up after its cue
        hold_release_ms_ = 0;
        return;
    }
    if (mono_ms() - start_ms_ < kStartGuardMs || popup_open()) return;
    const Row *r = list->row(index);
    if (!r || !r->selectable || r->section) return;
    if (list == &home_list_) {
        if (index == home_sel_) home_activate(index);
        else home_sel_ = index;
    } else if (list == &fav_list_) {
        if (index == fav_sel_ && fav_sel_id_ == r->id) favorite_activate(index);
        fav_sel_ = index;
        fav_sel_id_ = r->id;
    } else if (list == &art_list_) {
        if (index == art_sel_ && art_sel_id_ == r->id) article_activate(index);
        else {
            art_sel_ = index;
            art_sel_id_ = r->id;
        }
    } else if (list == &saved_list_) {
        if (index == saved_sel_) saved_activate(index);
        else saved_sel_ = index;
    } else if (list == &browse_list_) {
        if (search_typing_) search_typing_ = false;   // a tap on the list puts the keyboard away
        if (index == browse_sel_) browse_activate(index);
        else browse_sel_ = index;
    } else if (list == &settings_list_) {
        if (index == settings_sel_) settings_activate(index);
        else settings_sel_ = index;
    }
    list->set_selected(index, false);
    invalidate();
}

void App::button_clicked(int tag)
{
    if (mono_ms() - start_ms_ < kStartGuardMs) return;
    if (tag >= kTagPopLeft && tag <= kTagPopNo) {
        post([this, tag] {
            if (popup_open()) popup_click(tag);
        });
        return;
    }
    if (popup_open()) return;
    switch (tag) {
    case kTagFavBar:
        if (refreshing_) stop_refresh();
        else start_refresh(store_.subscriptions(), true);
        break;
    case kTagSearchField:
        search_typing_ = true;
        break;
    case kTagAddField:
        if (add_state_ != AddState::Working) {
            if (add_state_ == AddState::Found) add_state_ = AddState::Idle;
            add_typing_ = true;
        }
        break;
    case kTagAddFind: post([this] { add_find(); }); break;
    case kTagAddPin: post([this] { add_pin(); }); break;
    case kTagAddCancel:
        post([this] {
            add_state_ = AddState::Idle;
            add_typing_ = true;
            invalidate();
        });
        break;
    default: break;
    }
    invalidate();
}

/* ================================================================== dialogs */

void App::popup_begin(Popup p)
{
    hold_.cancel();
    p.opened_ms = mono_ms();
    pop_ = std::move(p);
    pop_dirty_ = true;                             // built in the next tick
    release_osk();
    invalidate();
}

void App::close_popup()
{
    pop_.kind = Popup::None;
    pop_.on_accept = nullptr;
    pop_.on_yes = nullptr;
    pop_dirty_ = false;
    pop_items_.clear();
    pop_value_ = pop_detail_ = pop_pos_ = pop_left_ = pop_right_ = nullptr;
    dialog_.reset();
    invalidate();
}

void App::open_choice(rssui::Choice c, std::function<void(int)> on_accept)
{
    Popup p;
    p.kind = Popup::Choice;
    p.title = c.title;
    p.choice = std::move(c);
    p.on_accept = std::move(on_accept);
    popup_begin(std::move(p));
}

void App::open_confirm(const std::string &title, const std::string &body, std::function<void()> on_yes, const std::string &yes,
                       const std::string &no, bool danger)
{
    Popup p;
    p.kind = Popup::Confirm;
    p.title = title;
    p.body = body;
    p.yes_text = yes;
    p.no_text = no;
    p.danger = danger;
    p.focus = 0;
    p.on_yes = std::move(on_yes);
    popup_begin(std::move(p));
}

void App::open_menu(const std::string &title, const std::vector<std::string> &items, std::function<void(int)> on_pick,
                    const std::vector<std::string> &values, const std::vector<Mark> &marks)
{
    Popup p;
    p.kind = Popup::Menu;
    p.title = title;
    p.items = items;
    p.values = values;
    p.marks = marks;
    p.on_accept = std::move(on_pick);
    popup_begin(std::move(p));
}

void App::open_list(const std::string &title, const std::vector<std::string> &items, int focus, std::function<void(int)> on_pick)
{
    Popup p;
    p.kind = Popup::List;
    p.title = title;
    p.items = items;
    p.marks.assign(items.size(), Mark::Dot);
    p.focus = std::clamp(focus, 0, std::max(0, static_cast<int>(items.size()) - 1));
    p.on_accept = std::move(on_pick);
    popup_begin(std::move(p));
}

void App::open_info(const std::string &title, const std::string &body, const std::string &small)
{
    Popup p;
    p.kind = Popup::Info;
    p.title = title;
    p.body = body;
    p.small = small;
    popup_begin(std::move(p));
}

lv_obj_t *App::dialog_button(std::size_t index) const
{
    if (!dialog_) return nullptr;
    lv_obj_t *backdrop = dialog_->root();
    if (!backdrop || lv_obj_get_child_count(backdrop) < 2) return nullptr;
    lv_obj_t *box = lv_obj_get_child(backdrop, 1);
    std::size_t n = 0;
    for (uint32_t i = 0; i < lv_obj_get_child_count(box); ++i) {
        lv_obj_t *c = lv_obj_get_child(box, static_cast<int32_t>(i));
        if (!lv_obj_check_type(c, &lv_button_class)) continue;
        if (n++ == index) return c;
    }
    return nullptr;
}

/* A text of a dialog in the app's FreeType face (the toolkit's builtin one has no accents): BODY, or CAPTION grey. */
lv_obj_t *App::dialog_label(const std::string &text, bool small, std::uint32_t color)
{
    lv_obj_t *l = lv_label_create(dialog_->body());
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_width(l, dialog_->geometry().content_w);
    lv_obj_set_style_text_font(l, small ? f_caption_ : f_body_, 0);
    set_color(l, color);
    lv_label_set_text(l, text.c_str());
    return l;
}

/* A row of a menu or a pick list: the ROW token high, the label, a grey value at the right, the marker (a dot: it acts;
 * a chevron: it opens a dialog or a choice). A tap picks it. */
lv_obj_t *App::dialog_row(const std::string &text, const std::string &value, Mark mark, int index)
{
    const cp0_ui_metrics_t *m = cp0_ui_metrics_get();
    const int w = dialog_->geometry().content_w, h = std::max(m->tok[CP0_TOK_ROW_H], line_h(f_body_) + m->tok[CP0_TOK_SPACE_S]);
    cp0_ui_list_layout_t l{};
    cp0_ui_list_layout(m, CP0_UI_LIST_COMFORTABLE, 0, 0, w, h, &l);
    l.text_px = m->text_px[CP0_TEXT_BODY];
    lv_obj_t *row = make_box(dialog_->body(), 0, 0, w, h);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_radius(row, m->tok[CP0_TOK_RADIUS_S], 0);
    lv_obj_set_style_bg_color(row, lv_color_hex(kPressed), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, LV_STATE_PRESSED);
    const bool chev = mark == Mark::Chevron, dot = mark == Mark::Dot || mark == Mark::DotRunning;
    const int trail = cp0_ui_list_trail_w(&l, chev ? CP0_UI_LIST_AFFORDANCE_OPEN : dot ? CP0_UI_LIST_AFFORDANCE_ACTION : CP0_UI_LIST_AFFORDANCE_UNSPECIFIED);
    const int lh = line_h(f_body_), vh = line_h(f_value_);
    const int vw = value.empty() ? 0 : std::min(text_w(value, f_value_) + 2, (w - 2 * l.inset - trail) / 2);
    lv_obj_t *label = make_label(row, f_body_, kText, l.inset, (h - lh) / 2, std::max(10, w - 2 * l.inset - trail - (vw ? vw + l.gap : 0)), lh);
    lv_label_set_text(label, text.c_str());
    if (vw) {
        lv_obj_t *v = make_label(row, f_value_, kValue, w - l.inset - trail - vw, (h - vh) / 2, vw, vh);
        lv_obj_set_style_text_align(v, LV_TEXT_ALIGN_RIGHT, 0);
        lv_label_set_text(v, value.c_str());
    }
    if (dot) {
        const int d = cp0_ui_list_dot_px(&l);
        lv_obj_t *o = make_box(row, cp0_ui_list_dot_x(&l, w), (h - d) / 2, d, d);
        lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_radius(o, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(o, lv_color_hex(cp0_ui_list_dot_rgb(mark == Mark::DotRunning ? CP0_UI_LIST_AFFORDANCE_ACTION_RUNNING
                                                                                               : CP0_UI_LIST_AFFORDANCE_ACTION)), 0);
        lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    } else if (chev) {
        const lv_font_t *cf = mark_font(cp0_ui_list_chevron_px(&l));
        lv_obj_t *c = lv_label_create(row);
        lv_obj_remove_flag(c, LV_OBJ_FLAG_CLICKABLE);
        lv_font_glyph_dsc_t glyph{};
        const bool has = lv_font_get_glyph_dsc(cf, &glyph, CP0_UI_LIST_CHEVRON_CODEPOINT, 0);
        if (!has) lv_font_get_glyph_dsc(cf, &glyph, static_cast<uint32_t>(CP0_UI_LIST_CHEVRON_FALLBACK[0]), 0);
        lv_label_set_text(c, has ? CP0_UI_LIST_CHEVRON_TEXT : CP0_UI_LIST_CHEVRON_FALLBACK);
        lv_obj_set_style_text_font(c, cf, 0);
        lv_obj_set_style_text_color(c, lv_color_hex(CP0_UI_LIST_CHEVRON_RGB), 0);
        lv_obj_set_style_text_align(c, LV_TEXT_ALIGN_RIGHT, 0);
        const int clh = line_h(cf);
        const int ink_top = (clh - cf->base_line) - glyph.box_h - glyph.ofs_y;
        lv_obj_set_size(c, cp0_ui_list_chevron_w(&l), clh);
        lv_obj_set_pos(c, w - l.inset - cp0_ui_list_chevron_w(&l), cp0_ui_list_chevron_y(h, ink_top, glyph.box_h));
    }
    lv_obj_add_event_cb(row, trampoline_popitem, LV_EVENT_CLICKED, reinterpret_cast<void *>(static_cast<intptr_t>(index)));
    return row;
}

/* The widgets of the pending dialog (from the tick, never inside an event of a dialog's button): a cp0::ui::Dialog, which
 * keeps the header free and follows the on-screen keyboard. Titles stay ASCII (the toolkit draws them in its builtin
 * face); feed names and article titles go in the body, in the app's face. */
void App::popup_build()
{
    dialog_.reset();
    pop_items_.clear();
    pop_value_ = pop_detail_ = pop_pos_ = pop_left_ = pop_right_ = nullptr;
    if (pop_.kind == Popup::None) return;
    using cp0::ui::ButtonRole;
    const cp0_ui_metrics_t *m = cp0_ui_metrics_get();
    auto tapped = [this](int tag) {
        return [this, tag] {
            post([this, tag] {
                if (popup_open()) popup_click(tag);
            });
        };
    };
    cp0::ui::DialogSpec spec;
    spec.title = pop_.title;
    spec.page = shell_.get();
    switch (pop_.kind) {
    case Popup::Choice:
        spec.buttons.push_back({"Cancel", ButtonRole::Cancel, tapped(kTagPopCancel)});
        spec.buttons.push_back({"OK", ButtonRole::Primary, tapped(kTagPopOk)});
        break;
    case Popup::Confirm:
        spec.buttons.push_back({pop_.no_text, ButtonRole::Cancel, tapped(kTagPopNo)});
        spec.buttons.push_back({pop_.yes_text, ButtonRole::Primary, tapped(kTagPopYes)});
        break;
    case Popup::Menu:
    case Popup::List: spec.buttons.push_back({"Cancel", ButtonRole::Cancel, tapped(kTagPopCancel)}); break;
    case Popup::Info: spec.buttons.push_back({"OK", ButtonRole::Primary, tapped(kTagPopOk)}); break;
    case Popup::None: break;
    }
    dialog_ = std::make_unique<cp0::ui::Dialog>(shell_->root(), layout_, std::move(spec));
    const int cw = dialog_->geometry().content_w;
    switch (pop_.kind) {
    case Popup::Choice: {
        // the value in the middle, an arrow on each side (ROW square: 7 mm); Left / Right do the same
        const int side = m->tok[CP0_TOK_ROW_H], gap = m->tok[CP0_TOK_SPACE_M];
        lv_obj_t *row = lv_obj_create(dialog_->body());
        lv_obj_remove_style_all(row);
        lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_remove_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_size(row, cw, side);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        auto arrow = [&](const char *symbol, int tag) {
            lv_obj_t *b = lv_button_create(row);
            if (lv_obj_get_group(b)) lv_group_remove_obj(b);
            lv_obj_remove_flag(b, LV_OBJ_FLAG_CLICK_FOCUSABLE);
            lv_obj_set_size(b, side, side);
            lv_obj_set_style_radius(b, layout_.esc_radius, 0);
            lv_obj_set_style_bg_color(b, lv_color_hex(kSelected), 0);
            lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
            lv_obj_set_style_bg_color(b, lv_color_hex(kPressed), LV_STATE_PRESSED);
            lv_obj_set_style_shadow_width(b, 0, 0);
            lv_obj_set_style_border_width(b, 0, 0);
            lv_obj_add_event_cb(b, trampoline_button, LV_EVENT_CLICKED, reinterpret_cast<void *>(static_cast<intptr_t>(tag)));
            lv_obj_t *l = lv_label_create(b);
            lv_label_set_text(l, symbol);
            lv_obj_set_style_text_font(l, cp0_ui_font(CP0_TEXT_SYMBOL), 0);
            lv_obj_center(l);
            return b;
        };
        pop_left_ = arrow(LV_SYMBOL_LEFT, kTagPopLeft);
        pop_value_ = lv_label_create(row);
        lv_label_set_long_mode(pop_value_, LV_LABEL_LONG_MODE_WRAP);
        lv_obj_set_width(pop_value_, std::max(10, cw - 2 * side - 2 * gap));
        lv_obj_set_style_text_align(pop_value_, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_font(pop_value_, f_row_bold_, 0);
        set_color(pop_value_, kText);
        pop_right_ = arrow(LV_SYMBOL_RIGHT, kTagPopRight);
        pop_detail_ = dialog_label("", false, kText);
        pop_pos_ = dialog_label("", true, kMuted);
        lv_obj_set_style_text_align(pop_detail_, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_align(pop_pos_, LV_TEXT_ALIGN_CENTER, 0);
        if (!pop_.choice.note.empty()) {
            lv_obj_t *note = dialog_label(pop_.choice.note, true, kMuted);
            lv_obj_set_style_text_align(note, LV_TEXT_ALIGN_CENTER, 0);
        }
        break;
    }
    case Popup::Confirm:
        dialog_label(pop_.body, false, kText);
        break;
    case Popup::Menu:
    case Popup::List:
        lv_obj_set_style_pad_row(dialog_->body(), 0, 0);       // the rows of a list touch, as in a list
        if (!pop_.body.empty()) dialog_label(pop_.body, true, kMuted);
        for (std::size_t i = 0; i < pop_.items.size(); ++i) {
            const std::string value = i < pop_.values.size() ? pop_.values[i] : std::string();
            const Mark mk = i < pop_.marks.size() ? pop_.marks[i] : Mark::Dot;
            pop_items_.push_back(dialog_row(pop_.items[i], value, mk, static_cast<int>(i)));
        }
        break;
    case Popup::Info:
        dialog_label(pop_.body, false, kText);
        if (!pop_.small.empty()) dialog_label(pop_.small, true, kMuted);
        break;
    case Popup::None: break;
    }
    dialog_->relayout();
    if (lv_obj_t *vp = lv_obj_get_parent(dialog_->body())) style_list_scrollbar(vp);
    if (pop_.kind == Popup::Confirm && pop_.danger)
        if (lv_obj_t *yes = dialog_button(1)) lv_obj_set_style_bg_color(yes, lv_color_hex(kDangerRed), 0);
}

void App::popup_refresh()
{
    switch (pop_.kind) {
    case Popup::Choice: {
        if (!pop_value_) return;
        lv_label_set_text(pop_value_, pop_.choice.label().c_str());
        lv_label_set_text(pop_detail_, pop_.choice.detail().c_str());
        lv_label_set_text(pop_pos_, pop_.choice.position().c_str());
        const bool prev = pop_.choice.can_prev(), next = pop_.choice.can_next();
        set_color(lv_obj_get_child(pop_left_, 0), prev ? kText : kPressed);
        set_color(lv_obj_get_child(pop_right_, 0), next ? kText : kPressed);
        if (dialog_) dialog_->relayout();
        break;
    }
    case Popup::Confirm: {
        lv_obj_t *no = dialog_button(0), *yes = dialog_button(1);
        if (no) lv_obj_set_style_border_width(no, pop_.focus == 0 ? 3 : 0, 0);
        if (yes) lv_obj_set_style_border_width(yes, pop_.focus == 1 ? 3 : 0, 0);
        if (no) lv_obj_set_style_border_color(no, lv_color_hex(kText), 0);
        if (yes) lv_obj_set_style_border_color(yes, lv_color_hex(kText), 0);
        break;
    }
    case Popup::Menu:
    case Popup::List:
        for (std::size_t i = 0; i < pop_items_.size(); ++i) {
            const bool sel = static_cast<int>(i) == pop_.focus;
            lv_obj_set_style_bg_color(pop_items_[i], lv_color_hex(kSelected), 0);
            lv_obj_set_style_bg_opa(pop_items_[i], sel ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
        }
        if (pop_.focus >= 0 && pop_.focus < static_cast<int>(pop_items_.size()))
            lv_obj_scroll_to_view_recursive(pop_items_[static_cast<std::size_t>(pop_.focus)], LV_ANIM_OFF);
        break;
    default: break;
    }
}

void App::popup_accept()
{
    auto cb = pop_.on_accept;
    const int idx = pop_.kind == Popup::Choice ? pop_.choice.index : pop_.focus;
    close_popup();
    if (cb) cb(idx);
}

void App::popup_cancel()
{
    close_popup();
}

void App::popup_key(const KeyEvent &e)
{
    if (mono_ms() - pop_.opened_ms < 300 && e.key == Key::Enter) return;      // the Enter that opened it must not answer it
    if (pop_dirty_) return;
    switch (pop_.kind) {
    case Popup::Choice: {
        const ChoiceResult r = choice_key(e, pop_.choice);
        if (r == ChoiceResult::Moved) popup_refresh();
        else if (r == ChoiceResult::Accept) popup_accept();
        else if (r == ChoiceResult::Cancel) popup_cancel();
        break;
    }
    case Popup::Confirm: {
        const ConfirmResult r = confirm_key(e, pop_.focus);
        if (r == ConfirmResult::Moved) popup_refresh();
        else if (r == ConfirmResult::Yes) {
            auto cb = pop_.on_yes;
            close_popup();
            if (cb) cb();
        } else if (r == ConfirmResult::No) popup_cancel();
        break;
    }
    case Popup::Menu:
    case Popup::List: {
        const MenuResult r = menu_key(e, pop_.focus, static_cast<int>(pop_.items.size()));
        if (r == MenuResult::Moved) popup_refresh();
        else if (r == MenuResult::Accept) popup_accept();
        else if (r == MenuResult::Cancel) popup_cancel();
        break;
    }
    case Popup::Info:
        if (!e.repeat && (e.key == Key::Enter || e.key == Key::Esc)) popup_cancel();
        break;
    case Popup::None: break;
    }
}

void App::popup_click(int tag)
{
    if (mono_ms() - pop_.opened_ms < 150 || pop_dirty_) return;
    switch (tag) {
    case kTagPopLeft:
        if (pop_.kind == Popup::Choice && pop_.choice.step(-1)) popup_refresh();
        break;
    case kTagPopRight:
        if (pop_.kind == Popup::Choice && pop_.choice.step(1)) popup_refresh();
        break;
    case kTagPopOk:
        if (pop_.kind == Popup::Choice) popup_accept();
        else popup_cancel();
        break;
    case kTagPopCancel:
    case kTagPopNo: popup_cancel(); break;
    case kTagPopYes: {
        auto cb = pop_.on_yes;
        close_popup();
        if (cb) cb();
        break;
    }
    default: break;
    }
}

void App::popup_pick(int index)
{
    if (pop_dirty_ || (pop_.kind != Popup::Menu && pop_.kind != Popup::List)) return;
    if (mono_ms() - pop_.opened_ms < 150 || index < 0 || index >= static_cast<int>(pop_.items.size())) return;
    pop_.focus = index;
    post([this] {
        if (pop_.kind == Popup::Menu || pop_.kind == Popup::List) popup_accept();
    });
}

/* ================================================================== the state line of the scripted tests */

namespace {
const char *page_key(PageId p)
{
    switch (p) {
    case PageId::Home: return "home";
    case PageId::Favorites: return "favorites";
    case PageId::Articles: return "articles";
    case PageId::Reader: return "reader";
    case PageId::Browse: return "browse";
    case PageId::Saved: return "saved";
    case PageId::Add: return "add";
    case PageId::Settings: return "settings";
    }
    return "?";
}
} // namespace

std::string App::debug_state() const
{
    const char *pk = pop_.kind == Popup::Choice ? "choice" : pop_.kind == Popup::Confirm ? "confirm" : pop_.kind == Popup::Menu ? "menu"
                     : pop_.kind == Popup::Info ? "info" : pop_.kind == Popup::List ? "list" : "none";
    int unread = 0;
    for (const rssreader::Subscription &s : subs_) unread += s.unread;
    std::string images;
    for (const ReaderImage &r : rimgs_) images += image_state_letter(r.state);
    if (images.empty()) images = "-";
    std::string s = std::string("state page=") + page_key(page()) + " depth=" + std::to_string(stack_.size()) + " home_sel=" + std::to_string(home_sel_) +
                    " subs=" + std::to_string(subs_.size()) + " unread=" + std::to_string(unread) + " fav_sel=" + std::to_string(fav_sel_) +
                    " refreshing=" + (refreshing_ ? "1" : "0") + " refresh=" + std::to_string(refresh_done_) + "/" + std::to_string(refresh_total_) +
                    " arts=" + std::to_string(arts_->size()) + " art_sel=" + std::to_string(art_sel_) + " loading=" + (arts_loading_ ? "1" : "0") +
                    " reader_idx=" + std::to_string(reader_idx_) + " reader_px=" + std::to_string(reader_built_px_) + " text_size=" + rssreader::text_size_name(const_cast<rssreader::Store &>(store_).settings().text_size) +
                    " images=" + images + " img_focus=" + std::to_string(img_focus_) + " ring=" + (reader_keys_ ? "1" : "0") +
                    " viewer=" + std::to_string(viewer_idx_) + " viewer_full=" + (viewer_pix_ ? std::to_string(viewer_pix_->width) + "x" + std::to_string(viewer_pix_->height) : std::string("-")) +
                    " viewer_hd=" + (viewer_pix_ ? "1" : "0") +
                    " saving=" + (save_job_ ? "1" : "0") + " is_saved=" + (reader_idx_ >= 0 && const_cast<rssreader::Store &>(store_).is_saved(reader_art_.id) ? "1" : "0") +
                    " hint=" + (hint_seen_ == 0 ? "1" : hint_seen_ == 1 ? "0" : "?") + " decodes=" + std::to_string(decodes_done()) + " decode_peak=" + std::to_string(decodes_peak_parallel()) +
                    " img_cache=" + std::to_string(const_cast<rssreader::ImageCache &>(image_cache_).count()) +
                    " reader_scroll=" + std::to_string(reader_ ? lv_obj_get_scroll_y(reader_) : 0) +
                    " blevel=" + std::to_string(static_cast<int>(blevel_)) + " country=" + bcountry_ + " topic=" + btopic_ + " search='" + search_text_ +
                    "' browse_rows=" + std::to_string(browse_list_.count()) + " browse_sel=" + std::to_string(browse_sel_) +
                    " pinned=" + std::to_string(pinned_.size()) + " add_state=" + std::to_string(static_cast<int>(add_state_)) +
                    " add_text='" + (add_ta_ ? lv_textarea_get_text(add_ta_) : "") + "' saved=" + std::to_string(saved_.size()) +
                    " settings_sel=" + std::to_string(settings_sel_) + " popup=" + pk + " popup_title='" + (pop_.kind == Popup::None ? std::string() : pop_.title) +
                    "' popup_value='" + (pop_.kind == Popup::Choice ? pop_.choice.label() : std::string()) + "' osk=" + (cp0::ui::osk::visible() ? "up" : "down") +
                    " typing=" + (osk_field_ == add_ta_ && osk_field_ ? "add" : osk_field_ == search_ta_ && osk_field_ ? "search" : "-") +
                    " notice='" + notice_text_ + "' title='" + title_key_ + "' caption='" + (cap_ ? lv_label_get_text(cap_) : "") + "'";
    return s;
}

} // namespace rssui
