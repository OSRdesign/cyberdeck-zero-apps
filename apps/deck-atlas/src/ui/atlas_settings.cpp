/*
 * SPDX-License-Identifier: MIT
 *
 * Deck Atlas: the Settings tab and the About page (see atlas_app.hpp). The cache statistics and clearing run on the
 * worker (the engine counts the saved tiles on disk).
 */
#include "atlas_app.hpp"
#include "atlas_util.hpp"

#include "build_info.hpp"
#include "format.hpp"

#include "cp0_map_widget.h"

#include <algorithm>

namespace atlasui {

namespace {

enum : std::intptr_t {
    kUnits = 1,
    kScale,
    kOffline,
    kCache,
    kClear,
    kUsage,
    kVersionRow,
    kAbout,
};

} // namespace

void AtlasApp::build_settings()
{
    cp0::ui::CompactListEvents ev;
    ev.activate = [this](int, std::intptr_t id) { post([this, id] { settings_activate(id); }); };
    ev.back = [this] { post([this] { back(); }); };
    make_list(true, std::move(ev));
    const std::vector<cp0::ui::ListRow> rows = settings_rows();
    list_->reset_rows(rows, settings_sel_ >= 0 ? std::min(settings_sel_, static_cast<int>(rows.size()) - 1) : 0);
}

std::vector<cp0::ui::ListRow> AtlasApp::settings_rows() const
{
    using cp0::ui::ListRow;
    using cp0::ui::RowAffordance;
    using cp0::ui::ValueTone;
    std::vector<ListRow> rows;
    rows.push_back(ListRow::section("Map"));
    rows.push_back(ListRow::item("Units", atlas::units_label(settings_.units), ValueTone::Normal, kUnits, RowAffordance::Edit));
    rows.push_back(ListRow::item("Display scale", atlas::scale_label(settings_.scale, map_ ? cp0_map_get_scale(map_) : 0),
                                 ValueTone::Normal, kScale, RowAffordance::Edit));
    rows.push_back(ListRow::section("Online tiles"));
    rows.push_back(ListRow::item("Offline mode", atlas::on_off(settings_.offline),
                                 settings_.offline ? ValueTone::Warn : ValueTone::Normal, kOffline, RowAffordance::Edit));
    rows.push_back(ListRow::item("Cache size", atlas::cache_label(settings_.cache_mb, stats_known_ ? stats_cap_ : 0),
                                 ValueTone::Normal, kCache, RowAffordance::Edit));
    rows.push_back(ListRow::item("Clear saved tiles", "", ValueTone::Normal, kClear,
                                 clearing_ ? RowAffordance::ActionRunning : RowAffordance::Action));
    std::string usage = "...";
    if (!store_) usage = "-";
    else if (stats_known_)
        usage = atlas::bytes_label(stats_disk_) + ", " + atlas::count_label(stats_tiles_) + (stats_tiles_ == 1 ? " tile" : " tiles");
    rows.push_back(ListRow::item("Saved now", usage, ValueTone::Normal, kUsage, RowAffordance::Info));
    rows.push_back(ListRow::section("About"));
    rows.push_back(ListRow::item("Version", kVersion, ValueTone::Normal, kVersionRow, RowAffordance::Info));
    rows.push_back(ListRow::item("Map data and credits", "", ValueTone::Normal, kAbout, RowAffordance::Open));
    return rows;
}

void AtlasApp::settings_activate(std::intptr_t id)
{
    if (dialog_) return;
    switch (id) {
    case kUnits:
        settings_.units = atlas::next_units(settings_.units);
        if (map_) cp0_map_set_units(map_, settings_.units == atlas::Units::Mi ? CP0_MAP_UNITS_IMPERIAL : CP0_MAP_UNITS_METRIC);
        break;
    case kScale:
        settings_.scale = atlas::next_scale(settings_.scale);
        if (map_) cp0_map_set_scale(map_, settings_.scale);
        update_chip(); /* the ground the view covers changed with the scale */
        break;
    case kOffline: {
        settings_.offline = !settings_.offline;
        for (OnlineItem &o : online_) o.facts.network_on = !settings_.offline;
        if (store_ && worker_) {
            cp0_map_store_t *store = store_;
            const int on = (!settings_.offline && !demo_) ? 1 : 0;
            worker_->submit([this, store, on]() -> Worker::Done {
                const int rc = cp0_map_store_set_network(store, on);
                return [this, rc, on] {
                    log_line("network %s%s", on ? "on" : "off", rc == CP0_MAP_OK ? "" : cp0_map_strerror(rc));
                    refresh_store();
                };
            });
        }
        break;
    }
    case kCache: {
        settings_.cache_mb = atlas::next_cache_mb(settings_.cache_mb);
        if (store_ && worker_) {
            cp0_map_store_t *store = store_;
            const std::uint64_t cap = static_cast<std::uint64_t>(settings_.cache_mb) << 20;
            worker_->submit([this, store, cap]() -> Worker::Done {
                const int rc = cp0_map_store_set_cap(store, cap);
                return [this, rc] {
                    if (rc != CP0_MAP_OK) log_line("cache cap: %s", cp0_map_strerror(rc));
                    last_stats_ = 0;
                    refresh_stats();
                };
            });
        }
        break;
    }
    case kClear: confirm_clear(); return;
    case kAbout: open_about(); return;
    default: return;
    }
    save_settings();
    if (list_ && sub_ == Sub::None && tab_ == Tab::Settings) list_->set_rows(settings_rows());
}

void AtlasApp::refresh_stats()
{
    last_stats_ = mono_ms();
    if (!store_ || !worker_ || stats_busy_) return;
    stats_busy_ = true;
    cp0_map_store_t *store = store_;
    worker_->submit([this, store]() -> Worker::Done {
        auto st = std::make_shared<cp0_map_store_stats_t>();
        const int rc = cp0_map_store_cache_stats(store, st.get());
        return [this, st, rc] {
            stats_busy_ = false;
            last_stats_ = mono_ms();
            if (rc != CP0_MAP_OK) return;
            stats_known_ = true;
            stats_disk_ = st->disk_bytes;
            stats_cap_ = st->cap;
            stats_tiles_ = st->tiles;
            if (list_ && sub_ == Sub::None && tab_ == Tab::Settings) list_->set_rows(settings_rows());
        };
    });
}

void AtlasApp::confirm_clear()
{
    std::string text = "The tiles the online sources saved while you looked at the map are deleted";
    if (stats_known_) text += " (" + atlas::bytes_label(stats_disk_) + ")";
    text += ". Imported map files are not touched.";
    confirm("Clear saved tiles?", text, "Clear", [this] {
        if (!store_ || !worker_) return;
        clearing_ = true;
        if (list_ && sub_ == Sub::None && tab_ == Tab::Settings) list_->set_rows(settings_rows());
        cp0_map_store_t *store = store_;
        worker_->submit([this, store]() -> Worker::Done {
            const int rc = cp0_map_store_clear_cache(store, nullptr);
            return [this, rc] {
                clearing_ = false;
                log_line("saved tiles cleared%s%s", rc == CP0_MAP_OK ? "" : ": ", rc == CP0_MAP_OK ? "" : cp0_map_strerror(rc));
                if (rc != CP0_MAP_OK) message("Not cleared", cp0_map_strerror(rc));
                refresh_stats();
                if (list_ && sub_ == Sub::None && tab_ == Tab::Settings) list_->set_rows(settings_rows());
            };
        });
    });
}

std::string AtlasApp::about_text() const
{
    std::string s = std::string("Deck Atlas ") + kVersion +
                    "\nOffline maps for the deck: your own map files, and online sources you may switch on.";
    const std::string attr = active_attribution();
    s += "\n\nMAP DATA SHOWN NOW\n" + (attr.empty() ? std::string("No map file yet.") : plain_text(attr));
    s += "\n\nMAP FILES\nEach file's own attribution is drawn at the map's bottom edge. You are responsible for the terms "
         "of the maps you import (a tile folder converted without one is credited to OpenStreetMap contributors).";
    s += "\n\nONLINE SOURCES (off until you switch them on)";
    for (const OnlineItem &o : online_) s += "\n" + o.facts.title + ": " + plain_text(o.attribution);
    s += "\nNo request ever goes to the OpenStreetMap Foundation's tile servers. A request carries the tile's z/x/y "
         "and the app's name and version (and your key for a keyed source), nothing else.";
    s += "\n\nLICENCE AND CREDITS\nDeck Atlas: MIT. The map engine of the CyberdeckZero Launcher's toolkit, cp0_lvgl "
         "(MIT); stb_image 2.30 by Sean Barrett (public domain or MIT), inside the engine; SQLite (public domain) and "
         "libcurl (curl licence), system libraries loaded when present; LVGL (MIT); the Montserrat font (OFL) from "
         "the launcher.";
    s += "\n\nFILES\nSettings: ~/.config/deck-atlas/settings.conf\nMaps, keys and saved tiles: " +
         atlas::path_display(store_dir_, home_);
    s += std::string("\n\nBuilt against launcher ") + std::string(CDK_LAUNCHER_REF).substr(0, 7) + " (" + CDK_LAUNCHER_PIN + ")";
    return s;
}

void AtlasApp::open_about()
{
    if (list_) settings_sel_ = list_->selected();
    clear_page_objects();
    sub_ = Sub::About;
    show_tabs(false);
    set_title();
    text_ = std::make_unique<cp0::ui::TextBlock>(page_->root(), layout_, about_text(), nullptr,
                                                 [this] { post([this] { back(); }); });
    focus(text_->focus_object());
}

} // namespace atlasui
