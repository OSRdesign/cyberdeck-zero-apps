/*
 * SPDX-License-Identifier: MIT
 *
 * Deck Atlas: the file browser and the import page (see atlas_app.hpp, core/browser.hpp, core/import_model.hpp).
 * Folder listings and the import itself run on the worker; the engine's progress callback writes a small record under
 * a mutex that the LVGL timer reads (no LVGL call off the LVGL thread).
 */
#include "atlas_app.hpp"
#include "atlas_util.hpp"

#include "format.hpp"

#include "cp0_ui_metrics_lvgl.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <thread>

namespace atlasui {

namespace {

cp0::ui::ListRow browser_list_row(const atlas::BrowserRow &r, std::intptr_t id)
{
    using cp0::ui::ListRow;
    using cp0::ui::RowAffordance;
    using cp0::ui::ValueTone;
    switch (r.kind) {
    case atlas::RowKind::Root: return ListRow::item(r.label, r.value, ValueTone::Muted, id, RowAffordance::Open);
    case atlas::RowKind::Up: return ListRow::item(r.label, "", ValueTone::Normal, id, RowAffordance::Open);
    case atlas::RowKind::Folder: return ListRow::item(r.label, "", ValueTone::Normal, id, RowAffordance::Open);
    case atlas::RowKind::Archive: return ListRow::item(r.label, r.value, ValueTone::Muted, id, RowAffordance::Action);
    case atlas::RowKind::ImportFolder: return ListRow::item(r.label, r.value, ValueTone::Muted, id, RowAffordance::Action);
    default: return ListRow::item(r.label, r.value, ValueTone::Muted, id, RowAffordance::Info);
    }
}

constexpr const char *kTermsNote = "You are responsible for the terms of the maps you import.";

} // namespace

/* ------------------------------------------------------------------------------------------------ the browser */

void AtlasApp::open_browser(const std::string &path)
{
    if (sub_ == Sub::None && list_) sources_sel_ = list_->selected();
    clear_page_objects();
    sub_ = Sub::Browser;
    browser_path_ = path;
    show_tabs(false);
    set_title();
    cp0::ui::CompactListEvents ev;
    ev.activate = [this](int, std::intptr_t id) { post([this, id] { browser_activate(id); }); };
    ev.back = [this] { post([this] { back(); }); };
    make_list(false, std::move(ev));
    list_->reset_rows({cp0::ui::ListRow::item("Reading the folder...", "", cp0::ui::ValueTone::Muted, -1,
                                              cp0::ui::RowAffordance::Info)});
    if (!worker_) return;
    const int req = ++browser_req_;
    const std::string home = home_, user = user_;
    std::vector<std::string> roots = root_paths_;
    std::string demo_root;
    if (demo_) {
        if (const char *b = std::getenv("DECKATLAS_BROWSE"); b && *b) demo_root = b;
#ifdef CDK_APP_DIR
        if (demo_root.empty()) demo_root = std::string(CDK_APP_DIR) + "/tests/fixtures";
#endif
    }
    worker_->submit([this, req, path, home, user, roots, demo_root]() -> Worker::Done {
        auto rows = std::make_shared<std::vector<atlas::BrowserRow>>();
        auto root_list = std::make_shared<std::vector<std::string>>(roots);
        std::string error;
        atlas::PosixLister lister;
        if (path.empty()) {
            if (!demo_root.empty()) *rows = {{atlas::RowKind::Root, "Test files", "fixtures", demo_root}};
            else *rows = atlas::browser_roots(lister, home, user);
            root_list->clear();
            for (const atlas::BrowserRow &r : *rows) root_list->push_back(r.path);
        } else {
            const bool at_top = std::find(roots.begin(), roots.end(), path) != roots.end();
            *rows = atlas::browser_rows(lister, path, at_top, &error);
        }
        return [this, req, path, rows, root_list, error] {
            if (req != browser_req_ || sub_ != Sub::Browser) return;
            root_paths_ = *root_list;
            browser_loaded(path, std::move(*rows), error);
        };
    });
}

void AtlasApp::browser_loaded(const std::string &path, std::vector<atlas::BrowserRow> rows, const std::string &error)
{
    if (!list_) return;
    using cp0::ui::ListRow;
    browser_rows_ = std::move(rows);
    std::vector<ListRow> lrows;
    if (path.empty()) {
        lrows.push_back(ListRow::section("Where are your maps?"));
    } else {
        /* where we are: the folder's path, shortened in the middle to the list's width */
        const cp0_ui_list_layout_t &ll = list_->layout();
        const int chars = std::max(12, (ll.w - 2 * ll.inset) * 100 / std::max(1, ll.text_px * 58));
        lrows.push_back(ListRow::item(atlas::ellipsize_middle(atlas::path_display(path, home_), chars), "",
                                      cp0::ui::ValueTone::Muted, -1, cp0::ui::RowAffordance::Info));
    }
    if (!error.empty()) lrows.push_back(ListRow::item(error, "", cp0::ui::ValueTone::Bad, -1, cp0::ui::RowAffordance::Info));
    const int first = static_cast<int>(lrows.size());
    for (std::size_t i = 0; i < browser_rows_.size(); ++i)
        lrows.push_back(browser_list_row(browser_rows_[i], static_cast<std::intptr_t>(i)));
    if (path.empty())
        lrows.push_back(ListRow::item("Maps: .mbtiles, .pmtiles, tile folders", "", cp0::ui::ValueTone::Muted, -1,
                                      cp0::ui::RowAffordance::Info));
    /* the highlight: a tile folder's import row, else the first folder or file, else "Up one level" */
    int sel = first;
    for (std::size_t i = 0; i < browser_rows_.size(); ++i) {
        const atlas::RowKind k = browser_rows_[i].kind;
        if (k == atlas::RowKind::ImportFolder || k == atlas::RowKind::Folder || k == atlas::RowKind::Archive ||
            k == atlas::RowKind::Root) {
            sel = first + static_cast<int>(i);
            break;
        }
    }
    list_->reset_rows(lrows, sel);
}

void AtlasApp::browser_activate(std::intptr_t id)
{
    if (dialog_ || id < 0 || id >= static_cast<std::intptr_t>(browser_rows_.size())) return;
    const atlas::BrowserRow r = browser_rows_[static_cast<std::size_t>(id)];
    switch (r.kind) {
    case atlas::RowKind::Root:
    case atlas::RowKind::Folder: open_browser(r.path); break;
    case atlas::RowKind::Up: browser_back(); break;
    case atlas::RowKind::Archive:
    case atlas::RowKind::ImportFolder: confirm_import(r); break;
    default: break;
    }
}

void AtlasApp::browser_back()
{
    if (browser_path_.empty()) {
        close_sub();
        return;
    }
    if (std::find(root_paths_.begin(), root_paths_.end(), browser_path_) != root_paths_.end()) {
        open_browser("");
        return;
    }
    open_browser(atlas::path_parent(browser_path_));
}

void AtlasApp::confirm_import(const atlas::BrowserRow &row)
{
    close_dialog();
    const bool folder = row.kind == atlas::RowKind::ImportFolder;
    const std::string where = atlas::path_display(store_dir_, home_);
    cp0::ui::DialogSpec spec;
    spec.page = page_.get();
    const std::string path = row.path;
    spec.buttons.push_back({"Cancel", cp0::ui::ButtonRole::Cancel, [this] { post([this] { close_dialog(); }); }});
    std::string text;
    if (folder) {
        spec.title = "Import this tile folder?";
        text = atlas::path_name(path) + " is converted into one MBTiles file in " + where +
               ". The folder itself is not changed.\n\n" + kTermsNote;
        spec.buttons.push_back({"Import", cp0::ui::ButtonRole::Primary, [this, path] {
                                    post([this, path] {
                                        close_dialog();
                                        start_import(path, atlas::ImportKind::Folder);
                                    });
                                }});
    } else {
        spec.title = "Import " + row.label + "?";
        text = "Copy it into " + where +
               " (recommended: it stays when the stick or card is out), or read it where it is.\n\n" + kTermsNote;
        spec.buttons.push_back({"In place", cp0::ui::ButtonRole::Normal, [this, path] {
                                    post([this, path] {
                                        close_dialog();
                                        start_import(path, atlas::ImportKind::FileInPlace);
                                    });
                                }});
        spec.buttons.push_back({"Copy", cp0::ui::ButtonRole::Primary, [this, path] {
                                    post([this, path] {
                                        close_dialog();
                                        start_import(path, atlas::ImportKind::FileCopy);
                                    });
                                }});
    }
    dialog_ = std::make_unique<cp0::ui::Dialog>(page_->root(), layout_, std::move(spec));
    dialog_->add_text(text);
    dialog_->relayout();
    focus(dialog_->focus_object());
}

/* ------------------------------------------------------------------------------------------------ the import */

void AtlasApp::import_progress_cb(void *user, const cp0_map_progress_t *p)
{
    auto *self = static_cast<AtlasApp *>(user);
    if (!self || !p) return;
    std::lock_guard<std::mutex> lock(self->prog_mu_);
    self->prog_.phase = static_cast<int>(p->phase);
    self->prog_.done = p->done;
    self->prog_.total = p->total;
    self->prog_.skipped = p->skipped;
    self->prog_.resumed = p->resumed;
    self->prog_.fresh = true;
}

void AtlasApp::start_import(const std::string &path, atlas::ImportKind kind)
{
    if (importing_ || !store_ || !worker_) return;
    importing_ = true;
    import_cancel_ = 0;
    import_id_.clear();
    {
        std::lock_guard<std::mutex> lock(prog_mu_);
        prog_ = Prog();
    }
    import_.start(kind, atlas::path_name(path), mono_ms());
    clear_page_objects();
    sub_ = Sub::Import;
    show_tabs(false);
    build_import_page();
    set_title();
    log_line("import %s (%s)", path.c_str(),
             kind == atlas::ImportKind::Folder ? "tile folder" : kind == atlas::ImportKind::FileCopy ? "copy" : "in place");

    cp0_map_store_t *store = store_;
    const std::string store_dir = store_dir_;
    const bool simulate = demo_ && demo_mode_.rfind("import-hold", 0) == 0;
    worker_->submit([this, store, store_dir, path, kind, simulate]() -> Worker::Done {
        auto cancelled = [this] { return import_cancel_ != 0; };
        if (simulate) {
            /* the scenes' import: a 10,326-tile folder converted at 800 tiles/s, held at 40 % until Cancel */
            auto step = [this](int phase, std::uint64_t done, std::uint64_t total) {
                std::lock_guard<std::mutex> lock(prog_mu_);
                prog_.phase = phase;
                prog_.done = done;
                prog_.total = total;
                prog_.fresh = true;
            };
            const std::uint64_t total = 10326;
            for (int i = 0; i <= 10 && !cancelled(); ++i) {
                step(1, total * static_cast<std::uint64_t>(i) / 10, 0);
                std::this_thread::sleep_for(std::chrono::milliseconds(60));
            }
            std::uint64_t done = 0;
            while (!cancelled()) {
                if (done < total * 4 / 10) done = std::min<std::uint64_t>(total * 4 / 10, done + 80);
                step(3, done, total);
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            return [this] { finish_import(CP0_MAP_E_CANCELLED, "", ""); };
        }
        /* free space first (a folder: its bytes counted, a copy: the file's size) */
        std::uint64_t source_bytes = 0;
        if (kind == atlas::ImportKind::Folder) source_bytes = atlas::folder_bytes(path, nullptr, cancelled);
        else if (kind == atlas::ImportKind::FileCopy) source_bytes = atlas::file_size(path);
        if (cancelled()) return [this] { finish_import(CP0_MAP_E_CANCELLED, "", ""); };
        const std::uint64_t need = atlas::space_needed(kind, source_bytes);
        const std::uint64_t free_now = need ? atlas::free_bytes(store_dir) : 0;
        const std::string space = need ? atlas::space_error(need, free_now) : std::string();
        if (!space.empty()) return [this, space] { finish_import(CP0_MAP_E_FULL, "", space); };

        char out_id[CP0_MAP_ID_MAX] = "";
        cp0_map_import_opts_t o{};
        o.copy = kind == atlas::ImportKind::FileCopy ? 1 : 0;
        o.progress = import_progress_cb;
        o.user = this;
        o.cancel = &import_cancel_;
        o.out_id = out_id;
        o.out_id_size = sizeof out_id;
        const int rc = cp0_map_store_import_ex(store, path.c_str(), &o);
        const std::string id = out_id;
        return [this, rc, id] { finish_import(rc, id, ""); };
    });
}

void AtlasApp::finish_import(int rc, const std::string &id, const std::string &reason)
{
    importing_ = false;
    /* the last progress the engine reported, before the end state */
    update_import_page(false);
    const std::int64_t now = mono_ms();
    if (rc == CP0_MAP_OK) {
        import_id_ = id;
        refresh_store([this, id] {
            const ArchiveItem *a = find_archive(id);
            import_.finish_ok(a ? a->tiles : 0, a ? a->bytes : 0, mono_ms());
            log_line("imported %s: %s", id.c_str(), import_.result_line().c_str());
            update_import_page(true);
        });
        return;
    }
    if (rc == CP0_MAP_E_CANCELLED) import_.finish_cancelled(now);
    else import_.finish_failed(reason.empty() ? import_error_text(rc) : reason, now);
    log_line("import ended: %s", import_.result_line().c_str());
    refresh_store();
    update_import_page(true);
}

void AtlasApp::build_import_page()
{
    page_box_ = lv_obj_create(page_->root());
    plain(page_box_);
    lv_obj_set_pos(page_box_, 0, body_y_);
    lv_obj_set_size(page_box_, w_, h_ - body_y_);
    lv_obj_add_flag(page_box_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(page_box_, LV_DIR_VER);
    lv_obj_set_flex_flow(page_box_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_hor(page_box_, std::max(space_m_, layout_.roller_x), 0);
    lv_obj_set_style_pad_ver(page_box_, space_m_, 0);
    lv_obj_set_style_pad_row(page_box_, space_s_, 0);
    lv_obj_add_event_cb(
        page_box_,
        [](lv_event_t *e) {
            auto *self = static_cast<AtlasApp *>(lv_event_get_user_data(e));
            const uint32_t k = lv_event_get_key(e);
            if (k == LV_KEY_ESC) self->post([self] { self->back(); });
            else if (k == LV_KEY_ENTER && !self->importing_) {
                const bool ok = self->import_.phase() == atlas::ImportPhase::Done;
                self->post([self, ok] {
                    if (ok && !self->import_id_.empty()) self->show_on_map(self->import_id_);
                    else self->close_sub();
                });
            }
        },
        LV_EVENT_KEY, this);

    imp_title_ = make_label(page_box_, body_px_, pal::kText, false);
    lv_obj_set_width(imp_title_, lv_pct(100));
    lv_obj_t *name = make_label(page_box_, caption_px_, pal::kMuted, false);
    lv_obj_set_width(name, lv_pct(100));
    lv_label_set_text(name, import_.name().c_str());
    imp_bar_ = lv_bar_create(page_box_);
    lv_obj_set_size(imp_bar_, lv_pct(100), std::max(8, space_s_));
    lv_bar_set_range(imp_bar_, 0, 1000);
    lv_obj_set_style_bg_color(imp_bar_, lv_color_hex(pal::kBar), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(imp_bar_, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(imp_bar_, lv_color_hex(pal::kAccent), LV_PART_INDICATOR);
    lv_obj_set_style_radius(imp_bar_, 2, LV_PART_MAIN);
    lv_obj_set_style_radius(imp_bar_, 2, LV_PART_INDICATOR);
    imp_count_ = make_label(page_box_, body_px_, pal::kText, false);
    lv_obj_set_width(imp_count_, lv_pct(100));
    imp_rate_ = make_label(page_box_, caption_px_, pal::kMuted, false);
    lv_obj_set_width(imp_rate_, lv_pct(100));
    imp_result_ = make_label(page_box_, body_px_, pal::kText, true);
    lv_obj_set_width(imp_result_, lv_pct(100));

    lv_obj_t *buttons = lv_obj_create(page_box_);
    plain(buttons);
    lv_obj_set_size(buttons, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(buttons, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_column(buttons, space_m_, 0);
    lv_obj_set_style_pad_row(buttons, space_s_, 0);
    imp_cancel_ = make_button(buttons, "Cancel", import_cancel_cb);
    imp_show_ = make_button(buttons, "Show on map", import_show_cb);
    imp_done_ = make_button(buttons, "Done", import_done_cb);

    imp_note_ = make_label(page_box_, caption_px_, pal::kMuted, true);
    lv_obj_set_width(imp_note_, lv_pct(100));
    lv_label_set_text(imp_note_, (std::string(kTermsNote) + " Imported maps are kept in " +
                                  atlas::path_display(store_dir_, home_) + ".")
                                     .c_str());
    focus(page_box_);
    update_import_page(true);
}

void AtlasApp::update_import_page(bool force)
{
    const std::int64_t now = mono_ms();
    Prog p;
    {
        std::lock_guard<std::mutex> lock(prog_mu_);
        p = prog_;
        prog_.fresh = false;
    }
    if (p.fresh && import_.running()) import_.update(p.phase, p.done, p.total, p.skipped, p.resumed, now);
    if (!imp_title_) return;
    if (!force && now - imp_updated_ < 250) return;
    imp_updated_ = now;
    const std::string title = import_.title();
    const std::string count = import_.count_line();
    const std::string rate = import_.rate_line(now);
    const std::string result = import_.result_line();
    const int frac = static_cast<int>(import_.fraction() * 1000.0 + 0.5);
    const std::string key = title + "|" + count + "|" + rate + "|" + result + "|" + std::to_string(frac) +
                            (importing_ ? "1" : "0");
    if (!force && key == imp_shown_) return;
    imp_shown_ = key;
    lv_label_set_text(imp_title_, title.c_str());
    lv_label_set_text(imp_count_, count.c_str());
    lv_label_set_text(imp_rate_, rate.c_str());
    lv_label_set_text(imp_result_, result.c_str());
    const atlas::ImportPhase ph = import_.phase();
    lv_obj_set_style_text_color(imp_result_, lv_color_hex(ph == atlas::ImportPhase::Failed ? pal::kBad : pal::kText), 0);
    lv_bar_set_value(imp_bar_, frac, LV_ANIM_OFF);
    auto show = [](lv_obj_t *o, bool on) {
        if (!o) return;
        if (on) lv_obj_remove_flag(o, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
    };
    show(imp_count_, !count.empty());
    show(imp_rate_, !rate.empty());
    show(imp_result_, !result.empty());
    show(imp_cancel_, importing_);
    show(imp_show_, !importing_ && ph == atlas::ImportPhase::Done && !import_id_.empty());
    show(imp_done_, !importing_);
    if (imp_done_) lv_label_set_text(lv_obj_get_child(imp_done_, 0), ph == atlas::ImportPhase::Done ? "Done" : "Back");
}

void AtlasApp::import_cancel_cb(lv_event_t *e)
{
    auto *self = static_cast<AtlasApp *>(lv_event_get_user_data(e));
    self->import_cancel_ = 1;
    log_line("import: cancel asked");
}

void AtlasApp::import_show_cb(lv_event_t *e)
{
    auto *self = static_cast<AtlasApp *>(lv_event_get_user_data(e));
    const std::string id = self->import_id_;
    self->post([self, id] { self->show_on_map(id); });
}

void AtlasApp::import_done_cb(lv_event_t *e)
{
    auto *self = static_cast<AtlasApp *>(lv_event_get_user_data(e));
    self->post([self] { self->close_sub(); });
}

} // namespace atlasui
