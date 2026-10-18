/*
 * SPDX-License-Identifier: MIT
 *
 * RSS Reader: Settings. The row kinds of the guidelines: a value that changes (in place or through a choice) shows the
 * value then a chevron; a row that acts right there has the dot; a read-out is grey, without a marker, and the
 * selection skips it.
 *
 *   READING         Text size (choice), Refresh when opening (On / Off), Download images (Always / Off), Clear image cache
 *                   (its size; at once: it is only a cache)
 *   SAVED ARTICLES  Storage used (read-out), Storage cap (choice: 50 / 100 / 200 / 500 MB)
 *   FAVORITES       Clear read history (asked first), Export favorites (~/rss-reader-favorites.opml), Import favorites
 *                   (the .opml files of the home folder)
 *   ABOUT           version, catalog, launcher pin, data folder (read-outs)
 */
#include "rss_app.hpp"

#include "app_style.hpp"
#include "build_info.hpp"
#include "fsutil.hpp"
#include "hooks.hpp"
#include "opml.hpp"
#include "util.hpp"

#include <algorithm>
#include <cstdlib>

using namespace rssreader;

namespace rssui {

namespace {

enum SetKind {
    kSecReading, kTextSize, kRefreshOpen, kImages, kImageCache, kSecSaved, kStorageUsed, kStorageCap, kSecFavorites, kClearRead, kExport, kImport,
    kSecAbout, kVersion, kCatalog, kLauncher, kDataDir,
};

std::string home_dir()
{
    const char *h = std::getenv("HOME");
    return h && *h ? h : "/tmp";
}

std::string tilde(const std::string &path)
{
    const std::string home = home_dir();
    if (path.size() >= home.size() && path.compare(0, home.size(), home) == 0) return "~" + path.substr(home.size());
    return path;
}

const char *const kExportName = "rss-reader-favorites.opml";

} // namespace

void App::build_settings()
{
    lv_obj_t *p = make_page();
    pages_[PageId::Settings] = p;
    const Geo g = geo();
    make_list(settings_list_, p, 0, panel_h_, 3);
}

void App::render_settings()
{
    const Settings st = store_.settings();
    set_caption("Reading, saved articles, favorites");
    std::vector<Row> rows;
    settings_kinds_.clear();
    auto section = [&](SetKind k, const char *title) {
        RowText t;
        t.section = true;
        t.line1 = title;
        rows.push_back(build_row(t));
        settings_kinds_.push_back(k);
    };
    auto item = [&](SetKind k, const std::string &label, const std::string &value, std::uint32_t vcolor, Mark mark, bool selectable = true) {
        RowText t;
        t.id = std::to_string(static_cast<int>(k));
        t.line1 = label;
        t.right = value;
        t.c_right = vcolor;
        t.trail = mark;
        t.selectable = selectable;
        rows.push_back(build_row(t));
        settings_kinds_.push_back(k);
    };
    section(kSecReading, "READING");
    item(kTextSize, "Text size", text_size_label(st.text_size), kValue, Mark::Chevron);
    item(kRefreshOpen, "Refresh when opening", st.refresh_on_open ? "On" : "Off", kValue, Mark::Chevron);
    item(kImages, "Download images", st.image_download == ImageDownload::Off ? "Off" : "Always", kValue, Mark::Chevron);
    item(kImageCache, "Clear image cache", image_cache_text(image_cache_.usage()), kMuted, Mark::Dot);
    section(kSecSaved, "SAVED ARTICLES");
    item(kStorageUsed, "Storage used", storage_text(store_.saved_usage_bytes(), st.storage_cap_mb), kMuted, Mark::None, false);
    item(kStorageCap, "Storage cap", std::to_string(st.storage_cap_mb) + " MB", kValue, Mark::Chevron);
    section(kSecFavorites, "FAVORITES");
    item(kClearRead, "Clear read history", "", kMuted, Mark::Dot);
    item(kExport, "Export favorites", "", kMuted, Mark::Dot);
    item(kImport, "Import favorites", "", kMuted, Mark::Chevron);
    section(kSecAbout, "ABOUT");
    item(kVersion, "Version", CDK_APP_VERSION, kMuted, Mark::None, false);
    item(kCatalog, "Catalog", catalog_ok_ ? plural(static_cast<long>(catalog_.size()), "feed", "feeds") + "  \xC2\xB7  " + catalog_.generated()
                                          : std::string("not loaded"), kMuted, Mark::None, false);
    item(kLauncher, "Launcher", launcher_text(CDK_LAUNCHER_REF, CDK_LAUNCHER_PIN), kMuted, Mark::None, false);
    item(kDataDir, "Data", tilde(store_.root()), kMuted, Mark::None, false);
    // the selection only on a row that does something
    auto selectable = [&](int i) { return i >= 0 && i < static_cast<int>(rows.size()) && rows[static_cast<std::size_t>(i)].selectable; };
    if (!selectable(settings_sel_)) {
        int i = std::max(0, settings_sel_);
        while (i < static_cast<int>(rows.size()) && !selectable(i)) ++i;
        if (i >= static_cast<int>(rows.size())) {
            i = std::min(settings_sel_, static_cast<int>(rows.size()) - 1);
            while (i > 0 && !selectable(i)) --i;
        }
        settings_sel_ = i;
    }
    settings_list_.set(std::move(rows), settings_sel_);
}

void App::key_settings(const KeyEvent &e)
{
    const int n = settings_list_.count();
    auto selectable = [&](int i) { const Row *r = settings_list_.row(i); return r && r->selectable; };
    int sel = settings_sel_;
    auto step = [&](int d) {
        int i = sel + d;
        while (i >= 0 && i < n && !selectable(i)) i += d;
        if (i >= 0 && i < n) sel = i;
    };
    switch (e.key) {
    case Key::Up: step(-1); break;
    case Key::Down: step(1); break;
    case Key::Home: sel = -1; step(1); break;
    case Key::End: sel = n; step(-1); break;
    case Key::PageUp: for (int k = 0; k < 4; ++k) step(-1); break;
    case Key::PageDown: for (int k = 0; k < 4; ++k) step(1); break;
    case Key::Enter:
    case Key::Right:
        if (!e.repeat) settings_activate(settings_sel_);
        return;
    case Key::Left:
        if (!e.repeat) go_up();
        return;
    default: return;
    }
    if (sel == settings_sel_) {
        // no row that acts further on: the read-outs beyond it (About) or the heading before it come into view
        if (e.key == Key::Down || e.key == Key::End || e.key == Key::PageDown) settings_list_.scroll_by(1 << 20);
        else if (e.key == Key::Up || e.key == Key::Home || e.key == Key::PageUp) settings_list_.scroll_to_top();
        return;
    }
    settings_sel_ = sel;
    settings_list_.set_selected(sel, true);
}

void App::settings_activate(int index)
{
    if (index < 0 || index >= static_cast<int>(settings_kinds_.size())) return;
    Settings st = store_.settings();
    switch (settings_kinds_[static_cast<std::size_t>(index)]) {
    case kTextSize:
        open_choice(text_size_choice(st.text_size), [this](int idx) {
            Settings s = store_.settings();
            s.text_size = text_size_from_index(idx);
            store_.set_settings(s);
            notice("Text size: " + text_size_label(s.text_size));
        });
        break;
    case kRefreshOpen:
        st.refresh_on_open = !st.refresh_on_open;
        store_.set_settings(st);
        notice(st.refresh_on_open ? "Favorites are updated when you open them" : "Favorites are updated only by Refresh all");
        break;
    case kImages:
        st.image_download = st.image_download == ImageDownload::Off ? ImageDownload::Always : ImageDownload::Off;
        store_.set_settings(st);
        notice(st.image_download == ImageDownload::Off ? "Images are not downloaded (saved ones still show)" : "Images are downloaded when you open an article");
        break;
    case kImageCache: {
        const std::uint64_t had = image_cache_.usage();
        if (image_cache_.clear()) notice(had ? "Image cache cleared: " + fmt_bytes(had) + " freed" : std::string("The image cache is empty"));
        else notice("Could not clear the image cache", false);
        break;
    }
    case kStorageCap:
        open_choice(storage_cap_choice(st.storage_cap_mb), [this](int idx) {
            Settings s = store_.settings();
            s.storage_cap_mb = storage_cap_from_index(idx);
            store_.set_settings(s);
            notice("Storage cap: " + std::to_string(s.storage_cap_mb) + " MB");
        });
        break;
    case kClearRead:
        open_confirm("Clear read history?", "Every article of your favorites shows as unread again.", [this] {
            if (store_.clear_read_history()) {
                for (StoredArticle &a : *arts_) a.read = false;
                notice("Read history cleared");
            } else {
                notice("Could not clear the read history", false);
            }
        }, "Clear", "Cancel", true);
        break;
    case kExport: export_opml(); break;
    case kImport: choose_import(); break;
    default: break;
    }
    invalidate();
}

void App::export_opml()
{
    const std::vector<Subscription> subs = store_.subscriptions();
    const std::string path = home_dir() + "/" + kExportName;
    if (subs.empty()) {
        open_info("Export favorites", "You have no favorites to export yet.");
        return;
    }
    if (write_file_atomic(path, write_opml(opml_from_subscriptions(subs), "RSS Reader favorites"))) {
        open_info("Export favorites", "Exported " + plural(static_cast<long>(subs.size()), "feed", "feeds") + " to\n" + tilde(path),
                  "An OPML file: other feed readers import it, and so does Import favorites here.");
    } else {
        open_info("Export favorites", "Could not write " + tilde(path) + ".", "Is the home folder full or read-only?");
    }
}

void App::choose_import()
{
    const std::string home = home_dir();
    const std::vector<std::string> files = opml_candidates(list_dir(home));
    if (files.empty()) {
        open_info("Import favorites", "No OPML file in your home folder.", "Copy a .opml file (an export of another reader) to " + tilde(home) + " and try again.");
        return;
    }
    open_list("Import favorites", files, 0, [this, home, files](int pick) {
        if (pick >= 0 && pick < static_cast<int>(files.size())) import_opml(home + "/" + files[static_cast<std::size_t>(pick)]);
    });
}

void App::import_opml(const std::string &path)
{
    std::string text;
    if (!read_file(path, text, 4u * 1024 * 1024)) {
        open_info("Import favorites", "Could not read " + tilde(path) + ".");
        return;
    }
    const std::vector<OpmlFeed> feeds = parse_opml(text);
    const OpmlImportResult r = rssreader::import_opml(store_, catalog_ok_ ? &catalog_ : nullptr, feeds, now_epoch());
    open_info("Import favorites", import_summary(r, feeds.size()),
              r.added > 0 ? "The new favorites are fetched with the next Refresh all, or when you open them." : "");
    invalidate();
}

} // namespace rssui
