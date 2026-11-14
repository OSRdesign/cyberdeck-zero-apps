/*
 * SPDX-License-Identifier: MIT
 *
 * Vinyl library: the Library controller (see library_ctl.hpp).
 */
#include "library_ctl.hpp"

#include "schema.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <thread>

namespace vinyl {
namespace ui {

namespace {

constexpr uint32_t kEvKeyF = 33, kEvKeySlash = 53, kEvKeyO = 24;
constexpr uint32_t kCtrl = 1u << 1;

Page page_of(libm::Place p) {
    switch (p) {
    case libm::Place::Hub: return Page::LibHub;
    case libm::Place::Artists: return Page::LibArtists;
    case libm::Place::Artist: return Page::LibArtist;
    case libm::Place::Albums:
    case libm::Place::GenreAlbums: return Page::LibAlbums;
    case libm::Place::Album: return Page::LibAlbum;
    case libm::Place::Songs: return Page::LibSongs;
    case libm::Place::Genres: return Page::LibGenres;
    case libm::Place::Search: return Page::LibSearch;
    case libm::Place::Info: return Page::LibInfo;
    case libm::Place::Settings: return Page::LibSettings;
    case libm::Place::Folders: return Page::LibFolders;
    }
    return Page::LibHub;
}

bool is_paged(libm::Place p) {
    return p == libm::Place::Artists || p == libm::Place::Albums || p == libm::Place::Songs || p == libm::Place::Genres;
}

std::string trimmed(const std::string &s) {
    size_t a = 0, b = s.size();
    while (a < b && static_cast<unsigned char>(s[a]) <= ' ') ++a;
    while (b > a && static_cast<unsigned char>(s[b - 1]) <= ' ') --b;
    return s.substr(a, b - a);
}

bool selectable(libm::Row::Type t) { return t == libm::Row::Type::Track || t == libm::Row::Type::Album || t == libm::Row::Type::Artist; }

}  // namespace

struct LibraryCtl::Req {
    libm::Place place = libm::Place::Hub;
    int64_t id = 0;
    int offset = 0, limit = kLibraryPage;
    bool append = false;
    std::string query;
    uint64_t serial = 0;
};

struct LibraryCtl::Fetched {
    bool ok = false;
    int total = 0;
    LibraryCounts counts;
    std::vector<RootInfo> roots;
    LibraryStats stats;
    std::vector<ArtistRow> artists;
    std::vector<AlbumRow> albums;
    std::vector<TrackRow> tracks;
    std::vector<GenreRow> genres;
    ArtistRow artist;
    AlbumRow album;
    int artist_tracks = 0;
    TrackDetails info;
    bool info_ok = false;
    SearchResults search;
};

LibraryCtl::LibraryCtl(LibHost host, LibConfig cfg) : host_(std::move(host)), cfg_(std::move(cfg)) {
    worker_.start(cfg_.db_path, cfg_.ram_mb);
    /* the thread opens the database; one empty job tells whether it did (a few ms, once at start) */
    auto ok = std::make_shared<bool>(false);
    worker_.post([ok](Library *lib) { *ok = lib != nullptr; });
    worker_.wait_idle();
    available_ = *ok;
    if (available_) scan_ = std::make_unique<ScanThread>(cfg_.db_path, cfg_.ram_mb, cfg_.scan_hooks);
}

LibraryCtl::~LibraryCtl() {
    if (scan_) {
        scan_->cancel();
        scan_->wait();
    }
    worker_.stop();
}

std::string LibraryCtl::error() const { return worker_.open_error(); }

void LibraryCtl::wait_idle() { worker_.wait_idle(); }

bool LibraryCtl::scan_running() const { return scan_ && scan_->running(); }

void LibraryCtl::demo_progress(const ScanProgress &p, int prior_tracks) {
    demo_progress_on_ = true;
    demo_progress_ = p;
    prior_tracks_ = prior_tracks;
}

/* ------------------------------------------------------------------------------------------------ scans (D27) */

void LibraryCtl::start_launch_scan() {
    if (!scan_ || !host_.wanted) return;
    auto f = std::make_shared<Fetched>();
    worker_.post([f](Library *lib) {
        if (lib) f->counts = lib->counts();
    }, [this, f] {
        prior_tracks_ = f->counts.tracks;
        if (scan_ && !scan_->running()) scan_->start_all(host_.wanted());
    });
}

void LibraryCtl::start_full_scan(bool visible_notice) {
    if (!scan_ || !host_.wanted) return;
    prior_tracks_ = d_.counts.tracks;
    if (!scan_->start_all(host_.wanted())) {
        if (visible_notice && host_.notice) host_.notice(Notice::Kind::Info, "A scan is running already.");
        return;
    }
    visited_ = in_library_;
}

void LibraryCtl::browser_opened(const std::string &folder) {
    /* a folder opened from the Library is scanned (not recursively: its own files; its sub-folders when they are opened) */
    if (!scan_ || folder.empty() || !in_library_) return;
    visited_ = true;
    if (!scan_->running()) scan_->start_folder(folder, false);
}

void LibraryCtl::left() {
    if (!in_library_) return;
    in_library_ = false;
    sheet_on_ = false;
    if (host_.hide_sheet) host_.hide_sheet();
    if (visited_ && scan_ && scan_->running()) {
        scan_->cancel();
        std::fprintf(stderr, "[vinyl] library: left the Library, the scan is stopped (it resumes at the next start)\n");
    }
    visited_ = false;
}

/* ------------------------------------------------------------------------------------------------ places */

void LibraryCtl::enter() {
    in_library_ = true;
    visited_ = true;
    nav_.clear();
    libm::Loc hub;
    hub.place = libm::Place::Hub;
    sync_mounts(true);
    open_place(hub, true);
}

void LibraryCtl::enter_settings() {
    in_library_ = true;
    visited_ = true;
    nav_.clear();
    libm::Loc l;
    l.place = libm::Place::Settings;
    l.title = "Library";
    open_place(l, true);
}

void LibraryCtl::reenter() {
    in_library_ = true;
    visited_ = true;
    reload();
}

void LibraryCtl::resume() {
    if (nav_.empty()) {
        enter();
        return;
    }
    in_library_ = true;
    visited_ = true;
    open_place(nav_.top(), false);
}

void LibraryCtl::note_played(const std::string &path) {
    if (!available_ || path.empty()) return;
    worker_.post([path](Library *lib) {
        if (!lib) return;
        const int64_t id = lib->find_track(path);
        if (id > 0) lib->record_play(id, static_cast<int64_t>(std::time(nullptr)));
    });
}

Page LibraryCtl::page() const { return nav_.empty() ? Page::LibHub : page_of(nav_.top().place); }

void LibraryCtl::close_sheet() {
    if (!sheet_on_) return;
    sheet_on_ = false;
    if (host_.hide_sheet) host_.hide_sheet();
}

bool LibraryCtl::back() {
    if (sheet_on_) {
        close_sheet();
        return true;
    }
    if (!nav_.pop()) return false;
    open_place(nav_.top(), false);
    return true;
}

void LibraryCtl::open_place(libm::Loc loc, bool push) {
    close_sheet();
    if (push) nav_.push(loc);
    ++serial_;
    ++rows_gen_;
    const libm::Place pl = loc.place;
    /* the data of the page: counts and roots stay (the hub does not blink back to empty) */
    d_.artists.clear();
    d_.albums.clear();
    d_.tracks.clear();
    d_.genres.clear();
    d_.artist = ArtistRow();
    d_.album = AlbumRow();
    d_.artist_tracks = 0;
    d_.info_ok = false;
    d_.loaded = pl == libm::Place::Hub && d_.counts_known;
    d_.failed = false;
    d_.search = SearchResults();
    d_.search_done = false;
    rows_.clear();
    rows_cache_.reset();
    ++rows_ver_;
    pager_.reset(0);
    sel_ = loc.sel;
    scroll_to_ = std::max(0, loc.sel);
    results_mode_ = pl == libm::Place::Search && !push;
    if (pl == libm::Place::Search && push) {
        query_.clear();
        searched_.clear();
        query_pending_ = false;
    }
    if (host_.show) host_.show(page_of(pl));
    if (pl == libm::Place::Search) {
        if (host_.search_open) host_.search_open(query_, push);
        if (!push && !trimmed(query_).empty()) request(libm::Place::Search, 0, 0, 0, false, trimmed(query_));
    }
    switch (pl) {
    case libm::Place::Hub: request(pl, 0, 0, 0, false, ""); break;
    case libm::Place::Artists:
    case libm::Place::Albums:
    case libm::Place::Songs:
    case libm::Place::Genres: request(pl, 0, 0, std::max(kLibraryPage, (std::max(0, loc.sel) / kLibraryPage + 1) * kLibraryPage), false, ""); break;
    case libm::Place::Search: break;
    default: request(pl, loc.id, 0, 0, false, ""); break;
    }
    rebuild_rows();
    ++view_gen_;
}

void LibraryCtl::request(libm::Place place, int64_t id, int offset, int limit, bool append, const std::string &query) {
    if (!available_) return;
    auto rq = std::make_shared<Req>();
    rq->place = place;
    rq->id = id;
    rq->offset = offset;
    rq->limit = limit;
    rq->append = append;
    rq->query = query;
    rq->serial = serial_;
    auto f = std::make_shared<Fetched>();
    const std::string key = place == libm::Place::Search ? "search" : "";
    worker_.post(
        [rq, f](Library *lib) {
            if (!lib) return;
            Fetched &o = *f;
            const Req &r = *rq;
            switch (r.place) {
            case libm::Place::Hub:
                o.counts = lib->counts();
                o.ok = lib->roots_info(o.roots);
                break;
            case libm::Place::Artists:
                o.total = lib->counts().artists;
                o.ok = lib->artists(r.offset, r.limit, o.artists);
                break;
            case libm::Place::Albums:
                o.total = lib->counts().albums;
                o.ok = lib->albums(r.offset, r.limit, o.albums);
                break;
            case libm::Place::Songs:
                o.total = lib->counts().tracks;
                o.ok = lib->songs(r.offset, r.limit, o.tracks);
                break;
            case libm::Place::Genres:
                o.total = lib->counts().genres;
                o.ok = lib->genres(r.offset, r.limit, o.genres);
                break;
            case libm::Place::Artist: {
                std::vector<TrackRow> t;
                o.ok = lib->artist(r.id, o.artist) && lib->artist_albums(r.id, o.albums);
                if (o.ok && lib->artist_tracks(r.id, t)) o.artist_tracks = static_cast<int>(t.size());
                break;
            }
            case libm::Place::GenreAlbums: o.ok = lib->genre_albums(r.id, 0, 2000, o.albums); break;
            case libm::Place::Album: o.ok = lib->album(r.id, o.album) && lib->album_tracks(r.id, o.tracks); break;
            case libm::Place::Info: o.info_ok = o.ok = lib->track(r.id, o.info); break;
            case libm::Place::Settings:
                o.stats = lib->stats();
                o.ok = lib->roots_info(o.roots);
                break;
            case libm::Place::Folders: o.ok = lib->roots_info(o.roots); break;
            case libm::Place::Search: o.ok = lib->search(r.query, o.search); break;
            }
        },
        [this, rq, f] { apply(*f, *rq, rq->serial); }, key);
}

void LibraryCtl::reload() {
    if (nav_.empty() || !in_library_) return;
    const libm::Loc &l = nav_.top();
    int limit = kLibraryPage;
    if (is_paged(l.place)) limit = std::max(kLibraryPage, pager_.loaded());
    if (l.place == libm::Place::Search) {
        if (!trimmed(searched_).empty()) request(l.place, 0, 0, 0, false, trimmed(searched_));
        return;
    }
    request(l.place, l.id, 0, limit, false, "");
}

void LibraryCtl::apply(const Fetched &f, const Req &rq, uint64_t serial) {
    if (serial != serial_ || nav_.empty() || nav_.top().place != rq.place) return;
    if (rq.append && rq.offset != pager_.loaded()) { /* a reload came first: this page is stale */
        pager_.failed();
        return;
    }
    if (!f.ok && rq.place != libm::Place::Search) {
        d_.failed = true;
        d_.loaded = true;
        rebuild_rows();
        return;
    }
    switch (rq.place) {
    case libm::Place::Hub:
        d_.counts = f.counts;
        d_.counts_known = true;
        d_.roots = f.roots;
        break;
    case libm::Place::Artists:
        if (!rq.append) d_.artists.clear(), pager_.reset(f.total);
        d_.artists.insert(d_.artists.end(), f.artists.begin(), f.artists.end());
        pager_.appended(static_cast<int>(f.artists.size()));
        break;
    case libm::Place::Albums:
        if (!rq.append) d_.albums.clear(), pager_.reset(f.total);
        d_.albums.insert(d_.albums.end(), f.albums.begin(), f.albums.end());
        pager_.appended(static_cast<int>(f.albums.size()));
        break;
    case libm::Place::Songs:
        if (!rq.append) d_.tracks.clear(), pager_.reset(f.total);
        d_.tracks.insert(d_.tracks.end(), f.tracks.begin(), f.tracks.end());
        pager_.appended(static_cast<int>(f.tracks.size()));
        break;
    case libm::Place::Genres:
        if (!rq.append) d_.genres.clear(), pager_.reset(f.total);
        d_.genres.insert(d_.genres.end(), f.genres.begin(), f.genres.end());
        pager_.appended(static_cast<int>(f.genres.size()));
        break;
    case libm::Place::Artist:
        d_.artist = f.artist;
        d_.albums = f.albums;
        d_.artist_tracks = f.artist_tracks;
        break;
    case libm::Place::GenreAlbums: d_.albums = f.albums; break;
    case libm::Place::Album:
        d_.album = f.album;
        d_.tracks = f.tracks;
        break;
    case libm::Place::Info:
        d_.info = f.info;
        d_.info_ok = f.info_ok;
        break;
    case libm::Place::Settings:
        d_.stats = f.stats;
        d_.roots = f.roots;
        break;
    case libm::Place::Folders: d_.roots = f.roots; break;
    case libm::Place::Search:
        if (trimmed(rq.query) != trimmed(query_) && !results_mode_) return; /* the field moved on */
        d_.search = f.search;
        d_.search_done = true;
        searched_ = rq.query;
        break;
    }
    d_.loaded = true;
    rebuild_rows();
}

void LibraryCtl::rebuild_rows() {
    const std::string playing = host_.playing_path ? host_.playing_path() : std::string();
    playing_ = playing;
    const libm::Place pl = nav_.empty() ? libm::Place::Hub : nav_.top().place;
    std::vector<libm::Row> rows;
    switch (pl) {
    case libm::Place::Hub: rows = libm::hub_rows(d_.counts, d_.counts_known); break;
    case libm::Place::Artists: rows = libm::artist_rows(d_.artists); break;
    case libm::Place::Artist: rows = libm::album_rows(d_.albums, false); break;
    case libm::Place::Albums: rows = libm::album_rows(d_.albums, true); break;
    case libm::Place::GenreAlbums: rows = libm::album_rows(d_.albums, true); break;
    case libm::Place::Album: rows = libm::album_track_rows(d_.tracks, playing); break;
    case libm::Place::Songs: rows = libm::song_rows(d_.tracks, playing); break;
    case libm::Place::Genres: rows = libm::genre_rows(d_.genres); break;
    case libm::Place::Search: rows = libm::search_rows(d_.search, playing); break;
    case libm::Place::Info: break;
    case libm::Place::Settings: {
        const bool scanning = scan_running() || (demo_progress_on_ && libm::scan_note(demo_progress_, prior_tracks_).shown);
        rows = libm::settings_rows(static_cast<int>(d_.roots.size()), libm::last_scan_text(d_.stats), scanning);
        break;
    }
    case libm::Place::Folders:
        rows = libm::folder_rows(d_.roots, cfg_.home);
        for (libm::Row &r : rows) r.mark = libm::Row::Mark::None;
        break;
    }
    rows_ = std::move(rows);
    rows_cache_.reset();
    ++rows_ver_;
    /* keep the selection on a row that exists and is not a heading */
    if (rows_.empty()) sel_ = -1;
    else {
        if (sel_ < 0 || sel_ >= static_cast<int>(rows_.size()) || rows_[static_cast<size_t>(sel_)].kind != libm::Row::Kind::Item) {
            int s = std::min(std::max(0, sel_), static_cast<int>(rows_.size()) - 1);
            if (rows_[static_cast<size_t>(s)].kind != libm::Row::Kind::Item) {
                const int f = libm::step_row(rows_, s, 1);
                s = f >= 0 ? f : std::max(0, libm::step_row(rows_, s, -1));
            }
            sel_ = rows_[static_cast<size_t>(s)].kind == libm::Row::Kind::Item ? s : -1;
        }
    }
    if (!nav_.empty()) nav_.top().sel = std::max(0, sel_);
    ++view_gen_;
}

void LibraryCtl::set_selection(int i, bool) {
    sel_ = i;
    if (!nav_.empty()) nav_.top().sel = std::max(0, i);
    ++view_gen_;
    /* the next page of a long list when the selection nears the end of what is loaded */
    const libm::Place pl = nav_.empty() ? libm::Place::Hub : nav_.top().place;
    if (is_paged(pl) && i >= 0 && pager_.want_more(i)) request(pl, 0, pager_.next_offset(), pager_.page(), true, "");
}

/* ------------------------------------------------------------------------------------------------ input */

libm::Row::Type LibraryCtl::selected_type() const {
    if (sel_ < 0 || sel_ >= static_cast<int>(rows_.size())) return libm::Row::Type::None;
    return rows_[static_cast<size_t>(sel_)].type;
}

void LibraryCtl::row(Page, int index) {
    if (sheet_on_ || index < 0 || index >= static_cast<int>(rows_.size())) return;
    if (rows_[static_cast<size_t>(index)].kind != libm::Row::Kind::Item) return;
    set_selection(index, false);
    activate(index); /* a tap acts on the first tap, as the Browser's rows */
}

void LibraryCtl::hold(Page, int index) {
    if (sheet_on_ || index < 0 || index >= static_cast<int>(rows_.size())) return;
    if (!selectable(rows_[static_cast<size_t>(index)].type)) return;
    set_selection(index, false);
    open_sheet(index);
}

void LibraryCtl::activate(int index) {
    const libm::Row r = rows_[static_cast<size_t>(index)];
    const libm::Place pl = nav_.top().place;
    auto go = [&](libm::Place p, int64_t id, const std::string &title) {
        libm::Loc l;
        l.place = p;
        l.id = id;
        l.title = title;
        open_place(l, true);
    };
    switch (r.type) {
    case libm::Row::Type::Hub:
        if (r.key == "folders") {
            if (host_.open_folders) host_.open_folders();
        } else if (r.key == "playlists") {
            if (host_.notice) host_.notice(Notice::Kind::Info, "Playlists arrive in a later version.");
        } else if (r.key == "artists") go(libm::Place::Artists, 0, "Artists");
        else if (r.key == "albums") go(libm::Place::Albums, 0, "Albums");
        else if (r.key == "songs") go(libm::Place::Songs, 0, "Songs");
        else if (r.key == "genres") go(libm::Place::Genres, 0, "Genres");
        break;
    case libm::Row::Type::Artist: go(libm::Place::Artist, r.id, r.text); break;
    case libm::Row::Type::Album: go(libm::Place::Album, r.id, r.text); break;
    case libm::Row::Type::Genre: go(libm::Place::GenreAlbums, r.id, r.text); break;
    case libm::Row::Type::Track: {
        const std::vector<TrackRow> &list = pl == libm::Place::Search ? d_.search.songs : d_.tracks;
        std::vector<std::string> paths;
        for (const TrackRow &t : list) paths.push_back(t.path);
        if (r.index >= 0 && r.index < static_cast<int>(paths.size()) && host_.play) host_.play(paths, r.index);
        break;
    }
    case libm::Row::Type::None:
        if (pl == libm::Place::Settings) {
            if (r.key == "folders") go(libm::Place::Folders, 0, "Music folders");
            else if (r.key == "rescan" && !r.dimmed) {
                start_full_scan(true);
                rebuild_rows();
            }
        }
        break;
    }
}

void LibraryCtl::chip(Page p) {
    if (sheet_on_) return;
    if (p == Page::LibAlbum) { /* Play album */
        std::vector<std::string> paths;
        for (const TrackRow &t : d_.tracks) paths.push_back(t.path);
        if (!paths.empty() && host_.play) host_.play(paths, 0);
        return;
    }
    if (nav_.empty() || nav_.top().place != libm::Place::Search) {
        libm::Loc l;
        l.place = libm::Place::Search;
        open_place(l, true);
    }
}

void LibraryCtl::button(Page p, int index) {
    if (sheet_on_) return;
    if (p == Page::LibHub) { /* the empty library's "Add a music folder": the Music folders page, choosing at once */
        libm::Loc l;
        l.place = libm::Place::Folders;
        l.title = "Music folders";
        open_place(l, true);
        if (host_.choose_folder) host_.choose_folder();
        return;
    }
    if (p != Page::LibFolders) return;
    if (index == 0) {
        if (host_.choose_folder) host_.choose_folder();
        return;
    }
    /* Remove selected folder: only the folders the user added; Music and the drives are always indexed */
    if (sel_ < 0 || sel_ >= static_cast<int>(rows_.size())) return;
    const int ri = rows_[static_cast<size_t>(sel_)].index;
    if (ri < 0 || ri >= static_cast<int>(d_.roots.size())) return;
    const RootInfo root = d_.roots[static_cast<size_t>(ri)];
    std::vector<std::string> mine = host_.folders ? host_.folders() : std::vector<std::string>();
    auto it = std::find(mine.begin(), mine.end(), root.path);
    if (it == mine.end()) {
        if (host_.notice) host_.notice(Notice::Kind::Info, "Music and USB drives are always in the library. Only folders you added can be removed.");
        return;
    }
    mine.erase(it);
    if (host_.set_folders) host_.set_folders(mine);
    ScanThread *st = scan_.get();
    if (st) st->cancel(); /* returns at once; the worker waits for the scan to stop (never on the UI thread, D-63) */
    const int64_t id = root.id;
    worker_.post([id, st](Library *lib) {
        for (int i = 0; st && i < 250 && st->running(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(20));
        if (lib) remove_root(lib->db(), id);
    }, [this] {
        reload();
        if (host_.notice) host_.notice(Notice::Kind::Info, "Folder removed from the library. No file was touched.");
    });
    sel_ = std::max(0, sel_ - 1);
}

void LibraryCtl::add_folder(const std::string &path) {
    std::vector<std::string> mine = host_.folders ? host_.folders() : std::vector<std::string>();
    bool inside = false;
    if (host_.wanted)
        for (const RootSpec &r : host_.wanted())
            if (path == r.path || (path.size() > r.path.size() && path.compare(0, r.path.size(), r.path) == 0 && path[r.path.size()] == '/')) inside = true;
    const bool known = inside || std::find(mine.begin(), mine.end(), path) != mine.end();
    if (!known) {
        mine.push_back(path);
        if (host_.set_folders) host_.set_folders(mine);
        start_full_scan(false);
    }
    choose_cancelled(); /* back on the Music folders page first: showing a page takes a notice away */
    if (host_.notice) host_.notice(Notice::Kind::Info, known ? "That folder is in the library already." : "Folder added. Scanning.");
}

void LibraryCtl::choose_cancelled() {
    /* back on the Music folders page */
    if (nav_.empty()) return;
    if (host_.show) host_.show(Page::LibFolders);
    reload();
    ++view_gen_;
}

bool LibraryCtl::key(uint32_t k) {
    constexpr uint32_t kUp = 17, kDown = 18, kRight = 19, kLeft = 20, kEnter = 10;
    if (sheet_on_) {
        const int n = static_cast<int>(sheet_.rows.size());
        auto step = [&](int d) {
            int i = sheet_sel_;
            for (int tries = 0; tries < n; ++tries) {
                i = (i + d + n) % n;
                if (sheet_.rows[static_cast<size_t>(i)].enabled) break;
            }
            sheet_sel_ = i;
            if (host_.sheet_select) host_.sheet_select(i);
        };
        if (k == kUp) step(-1);
        else if (k == kDown) step(1);
        else if (k == kEnter || k == kRight) sheet_pick(sheet_sel_);
        else if (k == kLeft) close_sheet();
        return true;
    }
    const libm::Place pl = nav_.empty() ? libm::Place::Hub : nav_.top().place;
    if (pl == libm::Place::Info) {
        if (k == kUp && host_.scroll) host_.scroll(Page::LibInfo, -1);
        else if (k == kDown && host_.scroll) host_.scroll(Page::LibInfo, 1);
        return k == kUp || k == kDown;
    }
    if (k == kUp || k == kDown) {
        if (rows_.empty()) return true;
        const int base = sel_ < 0 ? (k == kDown ? -1 : static_cast<int>(rows_.size())) : sel_;
        const int n = libm::step_row(rows_, base, k == kDown ? 1 : -1);
        if (n >= 0) set_selection(n, true);
        else if (k == kUp && pl == libm::Place::Search && results_mode_) { /* above the first result: the field again */
            results_mode_ = false;
            if (host_.search_edit) host_.search_edit();
        }
        return true;
    }
    if (k == kEnter || k == kRight) {
        if (sel_ >= 0 && sel_ < static_cast<int>(rows_.size())) activate(sel_);
        return true;
    }
    return false;
}

bool LibraryCtl::letter(uint32_t evdev, uint32_t mods) {
    if (sheet_on_) {
        const std::string id = libm::sheet_action_for_key(evdev);
        if (id.empty() || mods) return false;
        const int i = libm::sheet_index(sheet_, id);
        if (i >= 0 && sheet_.rows[static_cast<size_t>(i)].enabled) sheet_pick(i);
        return true;
    }
    if ((mods & kCtrl) && evdev == kEvKeyO) { /* the options key: the sheet of the selected row, else the Menu (the host) */
        if (sel_ >= 0 && selectable(selected_type())) {
            open_sheet(sel_);
            return true;
        }
        return false;
    }
    if ((!(mods & kCtrl) && evdev == kEvKeySlash) || ((mods & kCtrl) && evdev == kEvKeyF)) {
        chip(Page::LibHub);
        return true;
    }
    return false;
}

void LibraryCtl::query_changed(const std::string &text) {
    query_ = text;
    if (trimmed(text).empty()) { /* nothing to search: the results go at once */
        query_pending_ = false;
        d_.search = SearchResults();
        d_.search_done = false;
        searched_.clear();
        rebuild_rows();
        return;
    }
    pending_query_ = text;
    query_pending_ = true;
    query_at_ = now_ms_;
}

void LibraryCtl::submit() {
    /* Enter in the field: the keys go to the results; a query not yet sent is sent now */
    if (host_.search_end_edit) host_.search_end_edit();
    results_mode_ = true;
    if (query_pending_) {
        query_pending_ = false;
        request(libm::Place::Search, 0, 0, 0, false, trimmed(query_));
    }
    if (sel_ < 0 && !rows_.empty()) sel_ = libm::first_row(rows_);
    ++view_gen_;
}

/* ------------------------------------------------------------------------------------------------ sheets */

void LibraryCtl::open_sheet(int index) {
    const libm::Row r = rows_[static_cast<size_t>(index)];
    const libm::Place pl = nav_.top().place;
    libm::Sheet s;
    if (r.type == libm::Row::Type::Track) {
        const std::vector<TrackRow> &list = pl == libm::Place::Search ? d_.search.songs : d_.tracks;
        if (r.index < 0 || r.index >= static_cast<int>(list.size())) return;
        s = libm::track_sheet(list[static_cast<size_t>(r.index)].title, list[static_cast<size_t>(r.index)].artist);
    } else if (r.type == libm::Row::Type::Album) {
        const std::vector<AlbumRow> &list = pl == libm::Place::Search ? d_.search.albums : d_.albums;
        if (r.index < 0 || r.index >= static_cast<int>(list.size())) return;
        s = libm::album_sheet(list[static_cast<size_t>(r.index)].title, list[static_cast<size_t>(r.index)].tracks);
    } else if (r.type == libm::Row::Type::Artist) {
        const std::vector<ArtistRow> &list = pl == libm::Place::Search ? d_.search.artists : d_.artists;
        if (r.index < 0 || r.index >= static_cast<int>(list.size())) return;
        s = libm::artist_sheet(list[static_cast<size_t>(r.index)].name, list[static_cast<size_t>(r.index)].albums);
    } else {
        return;
    }
    sheet_ = s;
    sheet_row_ = r;
    sheet_sel_ = 0;
    sheet_on_ = true;
    if (host_.show_sheet) host_.show_sheet(sheet_view());
}

SheetView LibraryCtl::sheet_view() const {
    SheetView v;
    v.title = sheet_.title;
    v.hint = sheet_.hint;
    v.selected = sheet_sel_;
    for (const libm::SheetRow &r : sheet_.rows) v.rows.push_back({r.label, r.key, r.enabled});
    return v;
}

void LibraryCtl::sheet_pick(int i) {
    if (!sheet_on_ || i < 0 || i >= static_cast<int>(sheet_.rows.size())) return;
    if (!sheet_.rows[static_cast<size_t>(i)].enabled) return;
    sheet_act(sheet_.rows[static_cast<size_t>(i)].id);
}

void LibraryCtl::sheet_act(const std::string &id) {
    const libm::Row r = sheet_row_;
    const libm::Place pl = nav_.top().place;
    const int row_index = sel_;
    close_sheet();
    if (r.type == libm::Row::Type::Track) {
        const std::vector<TrackRow> &list = pl == libm::Place::Search ? d_.search.songs : d_.tracks;
        if (r.index < 0 || r.index >= static_cast<int>(list.size())) return;
        const TrackRow &t = list[static_cast<size_t>(r.index)];
        if (id == "play") {
            if (row_index >= 0 && row_index < static_cast<int>(rows_.size())) activate(row_index);
        } else if (id == "next") {
            if (host_.play_next) host_.play_next({t.path});
            if (host_.notice) host_.notice(Notice::Kind::Info, "Will play next.");
        } else if (id == "add") {
            if (host_.add_to_queue) host_.add_to_queue({t.path});
            if (host_.notice) host_.notice(Notice::Kind::Info, "Added to the queue.");
        } else if (id == "info") {
            libm::Loc l;
            l.place = libm::Place::Info;
            l.id = t.id;
            l.title = t.title;
            open_place(l, true);
        }
        return;
    }
    const int mode = id == "play" ? 0 : id == "next" ? 1 : id == "add" ? 2 : -1;
    if (mode >= 0) tracks_job(r.type, r.id, mode);
}

/* an album's or an artist's tracks, then play them, play them next or add them */
void LibraryCtl::tracks_job(libm::Row::Type type, int64_t id, int mode) {
    if (!available_) return;
    auto f = std::make_shared<std::vector<TrackRow>>();
    worker_.post([type, id, f](Library *lib) {
        if (!lib) return;
        if (type == libm::Row::Type::Album) lib->album_tracks(id, *f);
        else lib->artist_tracks(id, *f);
    }, [this, f, mode] {
        std::vector<std::string> paths;
        for (const TrackRow &t : *f) paths.push_back(t.path);
        if (paths.empty()) {
            if (host_.notice) host_.notice(Notice::Kind::Info, "No track to play: its drive is not connected.");
            return;
        }
        const std::string n = libm::plural(static_cast<int64_t>(paths.size()), "track", "tracks");
        if (mode == 0 && host_.play) host_.play(paths, 0);
        else if (mode == 1) {
            if (host_.play_next) host_.play_next(paths);
            if (host_.notice) host_.notice(Notice::Kind::Info, (paths.size() == 1 ? "1 track" : n) + " will play next.");
        } else if (mode == 2) {
            if (host_.add_to_queue) host_.add_to_queue(paths);
            if (host_.notice) host_.notice(Notice::Kind::Info, "Added " + n + " to the queue.");
        }
    });
}

/* ------------------------------------------------------------------------------------------------ drives, ticks */

void LibraryCtl::sync_mounts(bool force) {
    if (cfg_.demo || !available_ || !host_.wanted) return;
    const std::string text = read_mounts();
    const bool first = mounts_sig_.empty();
    if (!force && text == mounts_sig_) return;
    mounts_sig_ = text;
    if (first && !force) return;
    const std::vector<RootSpec> wanted = host_.wanted();
    worker_.post([wanted, text](Library *lib) {
        if (lib) sync_roots(lib->db(), wanted, parse_mounts(text), "/dev/disk/by-uuid");
    }, [this] { reload(); });
}

void LibraryCtl::tick(uint64_t now) {
    now_ms_ = now;
    if (!available_) return;
    if (worker_.drain() > 0) ++view_gen_;
    if (scan_) { /* the scan's end goes in the journal (B37-B45), the pages ask again at each commit */
        const uint64_t g = scan_->gen();
        const ScanProgress prog = scan_->progress();
        const ScanProgress::State st = prog.state;
        if (g != scan_gen_seen_ || st != last_state_) {
            const ScanProgress::State was = last_state_;
            scan_gen_seen_ = g;
            last_state_ = st;
            if (st != was && (st == ScanProgress::State::Done || st == ScanProgress::State::Cancelled || st == ScanProgress::State::Failed))
                std::fprintf(stderr, "[vinyl] library scan %s: %llu files in %llu folders, %llu tags read, %llu added, %llu changed, %llu removed%s%s\n",
                             scan_state_name(st), static_cast<unsigned long long>(prog.files), static_cast<unsigned long long>(prog.folders),
                             static_cast<unsigned long long>(prog.read), static_cast<unsigned long long>(prog.added),
                             static_cast<unsigned long long>(prog.updated), static_cast<unsigned long long>(prog.removed),
                             prog.error.empty() ? "" : ": ", prog.error.c_str());
            reload();
            if (was == ScanProgress::State::Scanning && st == ScanProgress::State::Failed && host_.notice) {
                const std::string why = prog.error;
                host_.notice(Notice::Kind::Info, why.find("full") != std::string::npos ? std::string("Not enough space for the library.")
                                                                                       : "The library scan stopped: " + why);
            }
        }
    }
    if (!in_library_) return;
    if (query_pending_ && now - query_at_ >= cfg_.search_debounce_ms) {
        query_pending_ = false;
        const std::string q = trimmed(pending_query_);
        if (!q.empty()) request(libm::Place::Search, 0, 0, 0, false, q);
    }
    if (!cfg_.demo && now - mounts_at_ >= 2000) {
        mounts_at_ = now;
        sync_mounts(false);
    }
    const std::string playing = host_.playing_path ? host_.playing_path() : std::string();
    if (playing != playing_) {
        const libm::Place pl = nav_.empty() ? libm::Place::Hub : nav_.top().place;
        if (pl == libm::Place::Songs || pl == libm::Place::Album || pl == libm::Place::Search) rebuild_rows();
        else playing_ = playing;
    }
}

/* ------------------------------------------------------------------------------------------------ views */

LibView LibraryCtl::view() {
    LibView v;
    if (nav_.empty()) return v;
    const libm::Loc &loc = nav_.top();
    const libm::Place pl = loc.place;
    if (!rows_cache_) {
        auto out = std::make_shared<std::vector<RowView>>();
        for (const libm::Row &r : rows_) {
            RowView rv;
            rv.text = r.text;
            rv.value = r.value;
            rv.heading = r.kind == libm::Row::Kind::Heading;
            rv.mark = r.mark == libm::Row::Mark::Chevron ? RowView::Mark::Chevron
                      : r.mark == libm::Row::Mark::Dot   ? RowView::Mark::Dot
                                                         : RowView::Mark::None;
            rv.playing = r.playing;
            rv.dimmed = r.dimmed;
            rv.mono_value = r.mono_value;
            rv.flush = pl == libm::Place::Folders;
            out->push_back(rv);
        }
        rows_cache_ = out;
    }
    v.rows = rows_cache_;
    v.rows_ver = rows_ver_;
    v.rows_gen = rows_gen_;
    v.selected = sel_;
    v.scroll_to = scroll_to_;
    scroll_to_ = -1;
    ScanProgress prog = demo_progress_on_ ? demo_progress_ : (scan_ ? scan_->progress() : ScanProgress());
    const libm::ScanNote note = libm::scan_note(prog, prior_tracks_);
    switch (pl) {
    case libm::Place::Hub: {
        int64_t hidden = 0;
        for (const RootInfo &r : d_.roots) hidden += r.tracks;
        v.heading = libm::hub_heading(d_.counts.tracks, note.shown);
        v.chip = "Search";
        v.mini = host_.mini ? host_.mini() : MiniView();
        if (note.shown) {
            v.note.kind = LibNote::Kind::Scan;
            v.note.text = note.text;
            v.note.pct = note.pct;
            v.note.hot = note.hot;
        } else {
            const std::string off = libm::offline_note(d_.roots);
            if (!off.empty()) {
                v.note.kind = LibNote::Kind::Offline;
                v.note.text = off;
            }
        }
        if (d_.counts_known && d_.counts.tracks == 0 && hidden == 0 && !note.shown) {
            v.empty = true;
            v.empty_title = "No music yet";
            v.empty_sub = "Put music in the Music folder or plug in a USB drive. Vinyl finds it by itself.";
            v.empty_button = "Add a music folder";
        }
        break;
    }
    case libm::Place::Artists: v.heading = libm::artists_heading(pager_.total()); break;
    case libm::Place::Artist:
        v.title = libm::artist_name(d_.artist.name.empty() ? loc.title : d_.artist.name);
        v.heading = libm::artist_heading(d_.artist.name.empty() ? loc.title : d_.artist.name, static_cast<int>(d_.albums.size()),
                                         d_.artist_tracks);
        break;
    case libm::Place::Albums: v.heading = libm::albums_heading(pager_.total()); break;
    case libm::Place::GenreAlbums:
        v.title = libm::genre_name(loc.title);
        v.heading = libm::genre_heading(loc.title, static_cast<int>(d_.albums.size()));
        break;
    case libm::Place::Album:
        v.heading = libm::album_heading(d_.album.title.empty() ? loc.title : d_.album.title, d_.album.year, static_cast<int>(d_.tracks.size()));
        v.chip = d_.tracks.empty() ? std::string() : "Play album";
        break;
    case libm::Place::Songs: v.heading = libm::songs_heading(pager_.total()); break;
    case libm::Place::Genres: v.heading = libm::genres_heading(pager_.total()); break;
    case libm::Place::Search: {
        v.query = query_;
        const std::string q = trimmed(searched_.empty() ? query_ : searched_);
        if (trimmed(query_).empty()) {
            v.empty = true;
            v.empty_title = "Search your music";
            v.empty_sub = "Results update as you type. Two letters match the start of a title.";
        } else if (d_.search_done && d_.search.empty()) {
            v.empty = true;
            v.empty_title = "Nothing found for \"" + q + "\"";
            v.empty_sub = "Songs, albums, artists and genres were searched. Two letters match the start of a title.";
        }
        break;
    }
    case libm::Place::Info:
        v.heading = "TRACK INFO";
        if (d_.info_ok) v.info = libm::track_info(d_.info, cfg_.home);
        break;
    case libm::Place::Settings:
        if (note.shown) {
            v.block.kind = LibNote::Kind::Scan;
            v.block.pct = note.pct;
            v.block.hot = note.hot;
            v.block_title = libm::scan_heading(prog, prior_tracks_);
            v.block_text = "You can keep listening. The scan runs at low priority and stops when you leave the page or the board gets hot.";
        }
        break;
    case libm::Place::Folders:
        v.heading = "MUSIC FOLDERS, " + std::to_string(d_.roots.size());
        v.buttons = {"Add a folder", "Remove selected folder"};
        v.footnote = "Add opens the folder browser in choose mode. Removing a folder only removes its tracks from the library; no file is "
                     "touched. A drive that is not connected keeps its entry.";
        break;
    }
    return v;
}

}  // namespace ui
}  // namespace vinyl
