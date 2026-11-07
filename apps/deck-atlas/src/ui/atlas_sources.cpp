/*
 * SPDX-License-Identifier: MIT
 *
 * Deck Atlas: the Sources tab, a map file's page, the "Map shows" choice, the online sources' switch-on flow (key
 * first, terms the first time) and the key page (see atlas_app.hpp and core/source_model.hpp).
 */
#include "atlas_app.hpp"
#include "atlas_util.hpp"

#include "format.hpp"

#include "cp0_map_widget.h"
#include "cp0_ui_metrics_lvgl.h"

#include <algorithm>
#include <cstring>

namespace atlasui {

namespace {

enum : std::intptr_t {
    kShown = 1,
    kImport = 2,
    kInfo = 3,
    kArchiveBase = 1000,
    kOnlineBase = 2000,
    kKeyBase = 3000,
    kDetailShow = 100,
    kDetailRemove = 101,
};

cp0::ui::ValueTone tone_of(atlas::Tone t)
{
    switch (t) {
    case atlas::Tone::Ok: return cp0::ui::ValueTone::Ok;
    case atlas::Tone::Warn: return cp0::ui::ValueTone::Warn;
    case atlas::Tone::Bad: return cp0::ui::ValueTone::Bad;
    case atlas::Tone::Muted: return cp0::ui::ValueTone::Muted;
    default: return cp0::ui::ValueTone::Normal;
    }
}

std::string first_word(const std::string &s)
{
    const std::size_t sp = s.find(' ');
    return sp == std::string::npos ? s : s.substr(0, sp);
}

int text_w(const char *s, int px)
{
    lv_point_t size{};
    lv_text_get_size(&size, s, cp0_ui_font_px(px), 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    return size.x;
}

} // namespace

/* ------------------------------------------------------------------------------------------------ store data */

const ArchiveItem *AtlasApp::find_archive(const std::string &id) const
{
    for (const ArchiveItem &a : archives_)
        if (a.id == id) return &a;
    return nullptr;
}

const OnlineItem *AtlasApp::find_online(const std::string &id) const
{
    for (const OnlineItem &o : online_)
        if (o.facts.id == id) return &o;
    return nullptr;
}

std::string AtlasApp::shown_title() const
{
    if (settings_.source.empty()) return "All map files";
    if (const ArchiveItem *a = find_archive(settings_.source)) return a->title;
    if (const OnlineItem *o = find_online(settings_.source)) return o->facts.title;
    return "All map files";
}

std::string AtlasApp::active_attribution() const
{
    if (const OnlineItem *o = settings_.source.empty() ? nullptr : find_online(settings_.source)) return o->attribution;
    if (const ArchiveItem *a = settings_.source.empty() ? nullptr : find_archive(settings_.source)) return a->attribution;
    std::string all;
    for (const ArchiveItem &a : archives_) {
        if (a.attribution.empty() || all.find(a.attribution) != std::string::npos) continue;
        all += (all.empty() ? "" : "; ") + a.attribution;
    }
    return all;
}

void AtlasApp::refresh_store(std::function<void()> then)
{
    if (!store_ || !worker_) {
        if (then) then();
        return;
    }
    /* a closure waits for the data of a listing that starts after it was asked (a listing already running may
     * have read the store before the change it follows) */
    if (then) refresh_thens_.push_back(std::move(then));
    if (refreshing_) {
        refresh_again_ = true;
        return;
    }
    refreshing_ = true;
    last_refresh_ = mono_ms();
    auto thens = std::make_shared<std::vector<std::function<void()>>>();
    thens->swap(refresh_thens_);
    cp0_map_store_t *store = store_;
    const std::string dir = store_dir_;
    worker_->submit([this, store, dir, thens]() -> Worker::Done {
        auto arch = std::make_shared<std::vector<ArchiveItem>>();
        auto onl = std::make_shared<std::vector<OnlineItem>>();
        cp0_map_store_refresh(store); /* another app may have imported or removed something */
        const int rc = list_store(store, dir, *arch, *onl);
        return [this, arch, onl, rc, thens] {
            refreshing_ = false;
            last_refresh_ = mono_ms();
            if (rc == CP0_MAP_OK) {
                const bool changed_files = arch->size() != archives_.size() ||
                                           !std::equal(arch->begin(), arch->end(), archives_.begin(),
                                                       [](const ArchiveItem &a, const ArchiveItem &b) {
                                                           return a.id == b.id && a.state == b.state && a.bytes == b.bytes;
                                                       });
                archives_.swap(*arch);
                online_.swap(*onl);
                for (OnlineItem &o : online_) o.facts.network_on = !settings_.offline;
                if (changed_files) store_changed();
                refresh_sources_rows();
                update_chip();
            } else {
                log_line("listing the store: %s", cp0_map_strerror(rc));
            }
            for (auto &fn : *thens)
                if (fn) fn();
            if (refresh_again_) {
                refresh_again_ = false;
                refresh_store();
            }
        };
    });
}

void AtlasApp::refresh_sources_rows()
{
    if (!list_) return;
    if (sub_ == Sub::None && tab_ == Tab::Sources) {
        const std::vector<cp0::ui::ListRow> rows = sources_rows();
        if (static_cast<int>(rows.size()) == list_->count()) list_->set_rows(rows);
        else list_->reset_rows(rows, std::min(list_->selected(), static_cast<int>(rows.size()) - 1));
    } else if (sub_ == Sub::FileDetail) {
        if (!find_archive(detail_id_)) {
            set_tab(Tab::Sources);
            return;
        }
        list_->set_rows(detail_rows());
    }
}

/* ------------------------------------------------------------------------------------------------ the tab */

void AtlasApp::build_sources()
{
    cp0::ui::CompactListEvents ev;
    ev.activate = [this](int, std::intptr_t id) { post([this, id] { sources_activate(id); }); };
    ev.back = [this] { post([this] { back(); }); };
    make_list(true, std::move(ev));
    const std::vector<cp0::ui::ListRow> rows = sources_rows();
    list_->reset_rows(rows, sources_sel_ >= 0 ? std::min(sources_sel_, static_cast<int>(rows.size()) - 1) : 0);
}

std::vector<cp0::ui::ListRow> AtlasApp::sources_rows() const
{
    using cp0::ui::ListRow;
    using cp0::ui::RowAffordance;
    using cp0::ui::ValueTone;
    std::vector<ListRow> rows;
    if (!store_) {
        rows.push_back(ListRow::item(store_error_, "", ValueTone::Bad, kInfo, RowAffordance::Info));
        return rows;
    }
    rows.push_back(ListRow::item("Map shows", shown_title(), ValueTone::Normal, kShown, RowAffordance::Edit));
    rows.push_back(ListRow::section("Map files"));
    const bool wide = w_ >= 480;
    for (std::size_t i = 0; i < archives_.size(); ++i) {
        const ArchiveItem &a = archives_[i];
        std::string value = a.state == CP0_MAP_E_ARCHIVE_MISSING ? "Missing" : atlas::bytes_label(a.bytes);
        const std::string z = atlas::zoom_label(a.minzoom, a.maxzoom);
        if (wide && !z.empty() && a.state != CP0_MAP_E_ARCHIVE_MISSING) value += "  " + z;
        rows.push_back(ListRow::item(a.title, value,
                                     a.state == CP0_MAP_E_ARCHIVE_MISSING ? ValueTone::Bad : ValueTone::Normal,
                                     kArchiveBase + static_cast<std::intptr_t>(i), RowAffordance::Open));
    }
    if (archives_.empty())
        rows.push_back(ListRow::item("No map files yet", "", ValueTone::Muted, kInfo, RowAffordance::Info));
    rows.push_back(ListRow::item("Import a map", "", ValueTone::Normal, kImport, RowAffordance::Open));
    rows.push_back(ListRow::section("Online sources"));
    for (std::size_t i = 0; i < online_.size(); ++i) {
        const OnlineItem &o = online_[i];
        const atlas::StateWord w = atlas::source_state_word(o.facts);
        rows.push_back(ListRow::item(o.facts.title, w.text, tone_of(w.tone), kOnlineBase + static_cast<std::intptr_t>(i),
                                     RowAffordance::Edit));
        if (o.facts.needs_key)
            rows.push_back(ListRow::item(first_word(o.facts.title) + " key", atlas::key_status(o.facts.has_key),
                                         o.facts.has_key ? ValueTone::Normal : ValueTone::Muted,
                                         kKeyBase + static_cast<std::intptr_t>(i), RowAffordance::Edit));
    }
    return rows;
}

void AtlasApp::sources_activate(std::intptr_t id)
{
    if (dialog_) return;
    if (id == kShown) {
        open_shown();
    } else if (id == kImport) {
        if (importing_) {
            sub_ = Sub::Import;
            clear_page_objects();
            show_tabs(false);
            build_import_page();
            set_title();
            return;
        }
        open_browser("");
    } else if (id >= kArchiveBase && id < kArchiveBase + static_cast<std::intptr_t>(archives_.size())) {
        open_file_detail(archives_[static_cast<std::size_t>(id - kArchiveBase)].id);
    } else if (id >= kOnlineBase && id < kOnlineBase + static_cast<std::intptr_t>(online_.size())) {
        toggle_source(online_[static_cast<std::size_t>(id - kOnlineBase)].facts.id);
    } else if (id >= kKeyBase && id < kKeyBase + static_cast<std::intptr_t>(online_.size())) {
        open_key(online_[static_cast<std::size_t>(id - kKeyBase)].facts.id, false);
    }
}

/* ------------------------------------------------------------------------------------------------ a map file */

void AtlasApp::open_file_detail(const std::string &id)
{
    if (!find_archive(id)) return;
    if (list_) sources_sel_ = list_->selected();
    clear_page_objects();
    sub_ = Sub::FileDetail;
    detail_id_ = id;
    show_tabs(false);
    cp0::ui::CompactListEvents ev;
    ev.activate = [this](int, std::intptr_t rid) { post([this, rid] { detail_activate(rid); }); };
    ev.back = [this] { post([this] { back(); }); };
    make_list(false, std::move(ev));
    const std::vector<cp0::ui::ListRow> rows = detail_rows();
    int first_action = 0;
    for (std::size_t i = 0; i < rows.size(); ++i)
        if (rows[i].id == kDetailShow) first_action = static_cast<int>(i);
    list_->reset_rows(rows, first_action);
    set_title();
}

std::vector<cp0::ui::ListRow> AtlasApp::detail_rows() const
{
    using cp0::ui::ListRow;
    using cp0::ui::RowAffordance;
    using cp0::ui::ValueTone;
    std::vector<ListRow> rows;
    const ArchiveItem *a = find_archive(detail_id_);
    if (!a || !list_) return rows;
    /* a value is never cut by the list: long ones are shortened in the middle to what fits beside their label */
    const cp0_ui_list_layout_t &ll = list_->layout();
    auto fit = [&](const char *label, const std::string &value) {
        const int avail = ll.w - 2 * ll.inset - ll.gap - text_w(label, ll.text_px) - ll.gap;
        int n = atlas::utf8_chars(value);
        std::string v = value;
        while (n > 4 && text_w(v.c_str(), ll.value_px) > avail) v = atlas::ellipsize_middle(value, --n);
        return v;
    };
    auto info = [&](const char *label, const std::string &value, ValueTone tone = ValueTone::Normal) {
        if (value.empty()) return;
        rows.push_back(ListRow::item(label, fit(label, value), tone, kInfo, RowAffordance::Info));
    };
    rows.push_back(ListRow::item("Show on map", "", ValueTone::Normal, kDetailShow, RowAffordance::Action));
    rows.push_back(ListRow::item("Remove", "", ValueTone::Normal, kDetailRemove, RowAffordance::Action));
    rows.push_back(ListRow::section("This map file"));
    info("Name", a->title);
    if (a->state == CP0_MAP_E_ARCHIVE_MISSING) info("State", "Missing: the USB stick or card is out", ValueTone::Bad);
    info("File", atlas::path_display(a->path, home_));
    info("Size", a->bytes ? atlas::bytes_label(a->bytes) : std::string());
    info("Zooms", atlas::zoom_label(a->minzoom, a->maxzoom));
    info("Tiles", a->tiles ? atlas::count_label(a->tiles) : std::string());
    if (a->has_bounds) info("Area", atlas::bounds_label(a->bounds.west, a->bounds.south, a->bounds.east, a->bounds.north));
    const char *how = a->mode == CP0_MAP_MODE_COPY ? "Copied into the store"
                      : a->mode == CP0_MAP_MODE_INPLACE ? "Read where it is"
                      : a->mode == CP0_MAP_MODE_CONVERTED ? "Converted from a tile folder" : "";
    info("Kept as", how);
    info("From", atlas::path_display(a->source_path, home_));
    info("Imported", atlas::date_label(a->imported));
    info("Map data", plain_text(a->attribution));
    return rows;
}

void AtlasApp::detail_activate(std::intptr_t id)
{
    if (dialog_) return;
    if (id == kDetailShow) show_on_map(detail_id_);
    else if (id == kDetailRemove) confirm_remove(detail_id_);
}

void AtlasApp::confirm_remove(const std::string &id)
{
    const ArchiveItem *a = find_archive(id);
    if (!a) return;
    const std::string what = a->mode == CP0_MAP_MODE_INPLACE
                                 ? "The file on the stick or card stays where it is; Deck Atlas forgets it."
                                 : "Its file in the map folder is deleted (" + atlas::bytes_label(a->bytes) + ").";
    confirm("Remove " + a->title + "?", what, "Remove", [this, id] {
        if (!store_ || !worker_) return;
        cp0_map_store_t *store = store_;
        worker_->submit([this, store, id]() -> Worker::Done {
            const int rc = cp0_map_store_remove(store, id.c_str());
            return [this, rc, id] {
                if (rc != CP0_MAP_OK) {
                    log_line("remove %s: %s", id.c_str(), cp0_map_strerror(rc));
                    message("Not removed", import_error_text(rc));
                    return;
                }
                log_line("removed %s", id.c_str());
                if (settings_.source == id) {
                    settings_.source.clear();
                    save_settings();
                }
                refresh_store([this] {
                    if (sub_ == Sub::FileDetail) set_tab(Tab::Sources);
                });
            };
        });
    });
}

/* ------------------------------------------------------------------------------------------------ Map shows */

void AtlasApp::open_shown()
{
    if (list_) sources_sel_ = list_->selected();
    clear_page_objects();
    sub_ = Sub::Shown;
    show_tabs(false);
    shown_ids_.clear();
    std::vector<cp0::ui::ListRow> rows;
    int sel = 0;
    auto add = [&](const std::string &id, const std::string &title) {
        const bool cur = settings_.source == id;
        if (cur) sel = static_cast<int>(rows.size());
        rows.push_back(cp0::ui::ListRow::item(title, cur ? "Shown" : "", cp0::ui::ValueTone::Ok,
                                              static_cast<std::intptr_t>(shown_ids_.size()),
                                              cp0::ui::RowAffordance::Action));
        shown_ids_.push_back(id);
    };
    add("", "All map files");
    for (const ArchiveItem &a : archives_) add(a.id, a.title);
    for (const OnlineItem &o : online_)
        if (o.facts.enabled) add(o.facts.id, o.facts.title);
    cp0::ui::CompactListEvents ev;
    ev.activate = [this](int, std::intptr_t id) { post([this, id] { shown_activate(id); }); };
    ev.back = [this] { post([this] { back(); }); };
    make_list(false, std::move(ev));
    list_->reset_rows(rows, sel);
    set_title();
}

void AtlasApp::shown_activate(std::intptr_t id)
{
    if (id < 0 || id >= static_cast<std::intptr_t>(shown_ids_.size())) return;
    const std::string sid = shown_ids_[static_cast<std::size_t>(id)];
    show_on_map(sid);
}

void AtlasApp::set_shown(const std::string &id, bool fit)
{
    settings_.source = id;
    apply_map_source();
    if (fit) {
        if (const ArchiveItem *a = find_archive(id)) fit_archive(*a);
    }
    save_settings();
    update_chip();
}

/* ------------------------------------------------------------------------------------------------ online sources */

void AtlasApp::toggle_source(const std::string &id)
{
    const OnlineItem *o = find_online(id);
    if (!o) return;
    switch (atlas::toggle_step(o->facts.enabled, o->facts.needs_key, o->facts.has_key, atlas::terms_accepted(settings_, id))) {
    case atlas::ToggleStep::Disable: enable_source(id, false); break;
    case atlas::ToggleStep::AskKey: open_key(id, true); break;
    case atlas::ToggleStep::AskTerms: ask_terms(id); break;
    case atlas::ToggleStep::Enable: enable_source(id, true); break;
    }
}

void AtlasApp::ask_terms(const std::string &id)
{
    const OnlineItem *o = find_online(id);
    if (!o) return;
    close_dialog();
    cp0::ui::DialogSpec spec;
    spec.title = o->facts.title;
    spec.page = page_.get();
    spec.buttons.push_back({"Cancel", cp0::ui::ButtonRole::Cancel, [this] { post([this] { close_dialog(); }); }});
    spec.buttons.push_back({"I agree, enable", cp0::ui::ButtonRole::Primary, [this, id] {
                                post([this, id] {
                                    close_dialog();
                                    atlas::accept_terms(settings_, id);
                                    save_settings();
                                    log_line("terms accepted for %s", id.c_str());
                                    enable_source(id, true);
                                });
                            }});
    dialog_ = std::make_unique<cp0::ui::Dialog>(page_->root(), layout_, std::move(spec));
    dialog_->add_text(atlas::terms_text(o->note, o->licence, atlas::provider_terms(id), o->facts.daily_cap));
    dialog_->relayout();
    focus(dialog_->focus_object());
}

void AtlasApp::enable_source(const std::string &id, bool on)
{
    if (!store_ || !worker_) return;
    cp0_map_store_t *store = store_;
    worker_->submit([this, store, id, on]() -> Worker::Done {
        const int rc = cp0_map_source_set_enabled(store, id.c_str(), on ? 1 : 0);
        return [this, rc, id, on] {
            if (rc != CP0_MAP_OK) {
                log_line("switch %s %s: %s", id.c_str(), on ? "on" : "off", cp0_map_strerror(rc));
                message("Not switched", cp0_map_strerror(rc));
                return;
            }
            log_line("%s switched %s", id.c_str(), on ? "on" : "off");
            refresh_store([this, id, on] {
                if (on) {
                    set_shown(id, false); /* switching a source on shows it on the map */
                } else if (settings_.source == id) {
                    set_shown("", false);
                }
                refresh_sources_rows();
            });
        };
    });
}

/* ------------------------------------------------------------------------------------------------ the key page */

void AtlasApp::open_key(const std::string &id, bool then_enable)
{
    const OnlineItem *o = find_online(id);
    if (!o) return;
    if (list_) sources_sel_ = list_->selected();
    clear_page_objects();
    sub_ = Sub::Key;
    key_id_ = id;
    key_then_enable_ = then_enable;
    show_tabs(false);
    set_title();

    lv_obj_t *col = page_->body();
    lv_obj_t *head = make_label(col, body_px_, pal::kText, true);
    lv_obj_set_width(head, layout_.roller_w);
    lv_label_set_text(head, (o->facts.title + " needs your own API key." +
                             (then_enable ? " Save it, then the source is switched on." : ""))
                                .c_str());

    cp0::ui::TextFieldSpec spec;
    spec.label = "API key";
    spec.type = cp0::ui::TextType::Password;
    spec.placeholder = o->facts.has_key ? "a new key replaces the saved one" : "paste or type the key";
    spec.max_bytes = 127;
    cp0::ui::TextFieldEvents events;
    events.submit = [this](const std::string &text) { post([this, text] { submit_key(text); }); };
    events.back = [this] { post([this] { back(); }); };
    field_ = std::make_unique<cp0::ui::TextField>(col, layout_, std::move(spec), std::move(events));

    key_status_ = make_label(col, caption_px_, pal::kMuted, true);
    lv_obj_set_width(key_status_, layout_.roller_w);
    const atlas::ProviderTerms t = atlas::provider_terms(id);
    set_key_status(std::string(atlas::key_status(o->facts.has_key)) + (t.key_hint.empty() ? "" : ". " + t.key_hint), false);

    lv_obj_t *buttons = lv_obj_create(col);
    plain(buttons);
    lv_obj_set_size(buttons, layout_.roller_w, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(buttons, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_column(buttons, space_m_, 0);
    lv_obj_set_style_pad_row(buttons, space_s_, 0);
    lv_obj_set_style_pad_top(buttons, space_s_, 0);
    make_button(buttons, "Save key", [](lv_event_t *e) {
        auto *self = static_cast<AtlasApp *>(lv_event_get_user_data(e));
        if (self->field_) {
            const std::string text = self->field_->text();
            self->post([self, text] { self->submit_key(text); });
        }
    });
    if (o->facts.has_key)
        make_button(buttons, "Remove key", [](lv_event_t *e) {
            auto *self = static_cast<AtlasApp *>(lv_event_get_user_data(e));
            self->post([self] { self->remove_key(); });
        });
    focus(field_->focus_object()); /* editing starts: the on-screen keyboard shows when the shared rule allows it */
}

void AtlasApp::set_key_status(const std::string &text, bool error)
{
    if (!key_status_) return;
    lv_label_set_text(key_status_, text.c_str());
    lv_obj_set_style_text_color(key_status_, lv_color_hex(error ? pal::kBad : pal::kMuted), 0);
}

void AtlasApp::submit_key(const std::string &text)
{
    if (!field_ || sub_ != Sub::Key || !store_ || !worker_) return;
    const std::string key = atlas::trim_key(text);
    const std::string err = atlas::key_error(key);
    if (!err.empty()) {
        set_key_status(err, true);
        if (!field_->editing()) field_->begin_edit();
        return;
    }
    cp0_map_store_t *store = store_;
    const std::string id = key_id_;
    auto secret = std::make_shared<std::string>(key);
    set_key_status("Saving...", false);
    worker_->submit([this, store, id, secret]() -> Worker::Done {
        const int rc = cp0_map_source_set_key(store, id.c_str(), secret->c_str());
        std::fill(secret->begin(), secret->end(), '\0');
        return [this, rc, id] {
            if (rc != CP0_MAP_OK) {
                set_key_status(std::string("Not saved: ") + cp0_map_strerror(rc), true);
                return;
            }
            log_line("key saved for %s", id.c_str()); /* never the key */
            if (field_) field_->wipe();
            const bool then_enable = key_then_enable_ && sub_ == Sub::Key && key_id_ == id;
            refresh_store([this, id, then_enable] {
                if (sub_ == Sub::Key) set_tab(Tab::Sources);
                if (!then_enable) return;
                if (atlas::next_after_key(atlas::terms_accepted(settings_, id)) == atlas::ToggleStep::AskTerms) ask_terms(id);
                else enable_source(id, true);
            });
        };
    });
}

void AtlasApp::remove_key()
{
    const std::string id = key_id_;
    const OnlineItem *o = find_online(id);
    if (!o) return;
    confirm("Remove the key?", "The " + o->facts.title + " key is deleted from the map folder.", "Remove", [this, id] {
        if (!store_ || !worker_) return;
        cp0_map_store_t *store = store_;
        worker_->submit([this, store, id]() -> Worker::Done {
            const int rc = cp0_map_source_clear_key(store, id.c_str());
            return [this, rc, id] {
                if (rc != CP0_MAP_OK) {
                    set_key_status(std::string("Not removed: ") + cp0_map_strerror(rc), true);
                    return;
                }
                log_line("key removed for %s", id.c_str());
                refresh_store([this] {
                    if (sub_ == Sub::Key) set_tab(Tab::Sources);
                });
            };
        });
    });
}

} // namespace atlasui
