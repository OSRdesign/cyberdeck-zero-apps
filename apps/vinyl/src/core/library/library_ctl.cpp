/*
 * SPDX-License-Identifier: MIT
 *
 * Vinyl library: the Library controller (see library_ctl.hpp).
 */
#include "library_ctl.hpp"

#include "net_share.hpp"
#include "schema.hpp"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <sstream>
#include <sys/stat.h>
#include <set>
#include <cstdio>
#include <ctime>
#include <thread>

namespace vinyl {
namespace ui {

namespace {

constexpr uint32_t kEvKeyF = 33, kEvKeySlash = 53, kEvKeyO = 24, kEvKeyU = 22, kEvKeyD = 32, kEvKeyX = 45;
constexpr uint32_t kKeyDel = 127; /* LV_KEY_DEL */
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
    case libm::Place::Playlists: return Page::LibPlaylists;
    case libm::Place::Playlist: return Page::LibPlaylist;
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

bool selectable(libm::Row::Type t) {
    return t == libm::Row::Type::Track || t == libm::Row::Type::Album || t == libm::Row::Type::Artist || t == libm::Row::Type::Playlist ||
           t == libm::Row::Type::Entry;
}

std::string base_of(const std::string &p) {
    const size_t s = p.rfind('/');
    return s == std::string::npos ? p : p.substr(s + 1);
}

std::string read_bytes(const std::string &path) {
    std::ifstream f(path, std::ios::binary);
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

bool is_regular(const std::string &p) {
    struct stat st;
    return ::stat(p.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

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
    std::vector<PlaylistRow> playlists;
    std::vector<PlaylistEntryRow> entries;
    PlaylistStats pstats;
};

LibraryCtl::LibraryCtl(LibHost host, LibConfig cfg) : host_(std::move(host)), cfg_(std::move(cfg)) {
    playlist_dir_ = !cfg_.playlists_dir.empty() ? cfg_.playlists_dir : (cfg_.home.empty() ? std::string("Music/Playlists") : cfg_.home + "/Music/Playlists");
    store_ = PlaylistStore(playlist_dir_);
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
    pick_ = false;
    pick_queue_ = false;
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
    pick_ = false;
    pick_queue_ = false;
    edit_mode_ = false;
    export_name_.clear();
    nav_.clear();
    saved_nav_.clear();
    libm::Loc hub;
    hub.place = libm::Place::Hub;
    sync_mounts(true);
    open_place(hub, true);
}

void LibraryCtl::enter_settings() {
    in_library_ = true;
    visited_ = true;
    if (!nav_.empty() && nav_.top().place != libm::Place::Settings) saved_nav_ = nav_; /* Esc from the settings restores it */
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
    if (edit_mode_ && !pick_ && !nav_.empty() && nav_.top().place == libm::Place::Playlist) { /* Esc: back to the view; a second Esc leaves */
        edit_mode_ = false;
        rebuild_rows();
        return true;
    }
    if (!nav_.pop()) {
        if (pick_) { /* Add tracks: Esc at the hub is back on the playlist, with what was added */
            finish_pick();
            return true;
        }
        if (!saved_nav_.empty()) { /* the settings were opened from the Menu over a page: that page is the Library's place again */
            nav_ = saved_nav_;
            saved_nav_.clear();
        }
        return false;
    }
    open_place(nav_.top(), false);
    return true;
}

void LibraryCtl::open_place(libm::Loc loc, bool push) {
    close_sheet();
    /* a place that is left for another keeps its scroll position (and its selection, kept as it moves) for Esc */
    if (push && !nav_.empty() && host_.scroll_y) nav_.top().top = host_.scroll_y();
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
    d_.entries.clear();
    d_.pstats = PlaylistStats();
    rows_.clear();
    rows_cache_.reset();
    ++rows_ver_;
    pager_.reset(0);
    sel_ = loc.sel;
    want_sel_ = loc.sel;
    restore_top_ = push ? -1 : loc.top;
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
    case libm::Place::Playlist: request(pl, loc.id, 0, 0, false, loc.title); break;
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
    const std::vector<std::string> roots = root_paths();
    worker_.post(
        [this, rq, f, roots](Library *lib) {
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
            case libm::Place::Playlists:
                store_.set_roots(roots);
                playlist_sync(*lib, store_);
                o.ok = playlist_list(*lib, o.playlists);
                break;
            case libm::Place::Playlist:
                store_.set_roots(roots);
                o.ok = playlist_rows(*lib, store_, r.query, o.entries, &o.pstats);
                break;
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
    request(l.place, l.id, 0, limit, false, l.place == libm::Place::Playlist ? l.title : std::string());
}

void LibraryCtl::apply(const Fetched &f, const Req &rq, uint64_t serial) {
    if (serial != serial_ || nav_.empty() || nav_.top().place != rq.place) return;
    if (rq.append && rq.offset != pager_.loaded()) { /* a reload came first: this page is stale */
        pager_.failed();
        return;
    }
    if (!f.ok && rq.place == libm::Place::Playlist) { /* deleted outside Vinyl while it was open */
        if (host_.notice) host_.notice(Notice::Kind::Info, "That playlist is gone.");
        edit_mode_ = false;
        back();
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
    case libm::Place::Playlists: d_.playlists = f.playlists; break;
    case libm::Place::Playlist:
        d_.entries = f.entries;
        d_.pstats = f.pstats;
        break;
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
    case libm::Place::Hub: rows = pick_ ? libm::hub_pick_rows(d_.counts, d_.counts_known) : libm::hub_rows(d_.counts, d_.counts_known); break;
    case libm::Place::Artists: rows = libm::artist_rows(d_.artists); break;
    case libm::Place::Artist: rows = libm::album_rows(d_.albums, false); break;
    case libm::Place::Albums: rows = libm::album_rows(d_.albums, true); break;
    case libm::Place::GenreAlbums: rows = libm::album_rows(d_.albums, true); break;
    case libm::Place::Album: rows = libm::album_track_rows(d_.tracks, playing); break;
    case libm::Place::Songs: rows = libm::song_rows(d_.tracks, playing); break;
    case libm::Place::Genres: rows = libm::genre_rows(d_.genres); break;
    case libm::Place::Search: rows = libm::search_rows(d_.search, playing); break;
    case libm::Place::Info: break;
    case libm::Place::Playlists: rows = libm::playlist_list_rows(d_.playlists); break;
    case libm::Place::Playlist: rows = libm::playlist_entry_rows(d_.entries, playing); break;
    case libm::Place::Settings: {
        const bool scanning = scan_running() || (demo_progress_on_ && libm::scan_note(demo_progress_, prior_tracks_).shown);
        rows = libm::settings_rows(libm::online_roots(d_.roots), libm::last_scan_text(d_.stats), scanning);
        break;
    }
    case libm::Place::Folders:
        d_.drives = host_.drives ? host_.drives() : std::vector<DriveInfo>();
        {
            std::vector<RootInfo> shown = d_.roots; /* a root on a network share is a folder, not a USB drive */
            if (!cfg_.demo) {
                const std::vector<MountEntry> mounts = parse_mounts(read_mounts());
                for (RootInfo &r : shown) r.network = share_mount_of(mounts, r.path) != nullptr;
            }
            rows = libm::folder_rows(shown, cfg_.home, d_.drives);
        }
        break;
    }
    if (pick_) { /* Add tracks: a check on the songs that are in the playlist already (a hash lookup a row) */
        if (pl == libm::Place::Songs || pl == libm::Place::Album) libm::apply_marks(rows, d_.tracks, pick_marks_);
        else if (pl == libm::Place::Search) libm::apply_marks(rows, d_.search.songs, pick_marks_);
    }
    rows_ = std::move(rows);
    rows_cache_.reset();
    ++rows_ver_;
    /* keep the selection on a row that exists and is not a heading; a place given back keeps its saved row until its rows are
     * in (the empty placeholder must not overwrite it) */
    if (rows_.empty()) sel_ = -1;
    else {
        if (want_sel_ >= 0) sel_ = want_sel_;
        want_sel_ = -1;
        if (sel_ < 0 || sel_ >= static_cast<int>(rows_.size()) || rows_[static_cast<size_t>(sel_)].kind != libm::Row::Kind::Item) {
            int s = std::min(std::max(0, sel_), static_cast<int>(rows_.size()) - 1);
            if (rows_[static_cast<size_t>(s)].kind != libm::Row::Kind::Item) {
                const int f = libm::step_row(rows_, s, 1);
                s = f >= 0 ? f : std::max(0, libm::step_row(rows_, s, -1));
            }
            sel_ = rows_[static_cast<size_t>(s)].kind == libm::Row::Kind::Item ? s : -1;
        }
    }
    if (!nav_.empty() && !rows_.empty()) nav_.top().sel = std::max(0, sel_);
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
    if (rows_[static_cast<size_t>(index)].type == libm::Row::Type::Entry) { /* a playlist's track: a tap only selects it, it never plays (owner, 2026-11-15) */
        ++view_gen_;
        return;
    }
    activate(index); /* a tap acts on the first tap, as the Browser's rows */
}

void LibraryCtl::hold(Page, int index) {
    if (sheet_on_ || (pick_ && !pick_queue_) || index < 0 || index >= static_cast<int>(rows_.size())) return;
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
        } else if (r.key == "playlists") go(libm::Place::Playlists, 0, "Playlists");
        else if (r.key == "artists") go(libm::Place::Artists, 0, "Artists");
        else if (r.key == "albums") go(libm::Place::Albums, 0, "Albums");
        else if (r.key == "songs") go(libm::Place::Songs, 0, "Songs");
        else if (r.key == "genres") go(libm::Place::Genres, 0, "Genres");
        break;
    case libm::Row::Type::Artist: go(libm::Place::Artist, r.id, r.text); break;
    case libm::Row::Type::Album: go(libm::Place::Album, r.id, r.text); break;
    case libm::Row::Type::Genre: go(libm::Place::GenreAlbums, r.id, r.text); break;
    case libm::Row::Type::NewPlaylist: {
        std::vector<std::string> names;
        for (const PlaylistRow &p : d_.playlists) names.push_back(p.name);
        ask_paths_.clear();
        ask(Ask::New, NameAsk{"New playlist", "An empty playlist. Name:", libm::free_name_among(names, "Playlist"), "Create"});
        break;
    }
    case libm::Row::Type::Playlist:
        edit_mode_ = false;
        go(libm::Place::Playlist, r.id, r.text);
        break;
    case libm::Row::Type::Entry:
        if (!r.dimmed && !edit_mode_) play_playlist_rows(r.index); /* Enter (never a tap): Play from here, in the view; a dimmed entry is inert */
        break;
    case libm::Row::Type::Track: {
        const std::vector<TrackRow> &list = pl == libm::Place::Search ? d_.search.songs : d_.tracks;
        if (pick_) { /* Add tracks: the song is appended, nothing plays */
            if (r.index >= 0 && r.index < static_cast<int>(list.size())) pick_add({list[static_cast<size_t>(r.index)].path});
            break;
        }
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
    if (p == Page::LibAlbum) { /* Play album (Add tracks: Add album) */
        std::vector<std::string> paths;
        for (const TrackRow &t : d_.tracks) paths.push_back(t.path);
        if (pick_) {
            if (!paths.empty()) pick_add(paths);
            return;
        }
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
    if (pick_ && index == 20) { /* the Done button (Add tracks, Add to the queue) */
        finish_pick();
        return;
    }
    if (p == Page::LibHub) { /* the empty library's "Add a music folder": the Music folders page, choosing at once */
        libm::Loc l;
        l.place = libm::Place::Folders;
        l.title = "Music folders";
        open_place(l, true);
        if (host_.choose_folder) host_.choose_folder();
        return;
    }
    if (p == Page::LibPlaylist && !pick_) {
        const std::string name = entry_name();
        if (!edit_mode_) { /* the view: Play (from the first track), Edit */
            if (index == 0) play_playlist_first();
            else if (index == 1) {
                edit_mode_ = true;
                rebuild_rows();
            }
        } else if (index == 20) { /* Done */
            edit_mode_ = false;
            rebuild_rows();
        } else if (index >= 0 && index <= 2) { /* Move up, Move down, Remove */
            playlist_edit(index);
        } else if (index == 3) { /* Add tracks: the Library in pick mode; it comes back here, in the edit mode */
            begin_pick(name);
        }
        return;
    }
    if (p != Page::LibFolders) return;
    if (index == 0) {
        if (host_.choose_folder) host_.choose_folder();
        return;
    }
    const bool eject_shown = host_.drives && host_.drives_available && host_.drives_available();
    if (eject_shown && index == 1) { /* Mount on a drive that is plugged in and not mounted; Eject on one Vinyl mounted */
        if (sel_ < 0 || sel_ >= static_cast<int>(rows_.size())) return;
        const std::string &key = rows_[static_cast<size_t>(sel_)].key;
        const bool mount = key.compare(0, 7, "ndrive:") == 0;
        if (!mount && key.compare(0, 7, "vdrive:") != 0) return; /* inert (dimmed) for any other row: a drive the system mounted too */
        const std::string dev = key.substr(7);
        std::string name = "the drive";
        for (const DriveInfo &d : d_.drives)
            if (d.dev == dev) name = d.name();
        if (mount) {
            if (host_.mount) host_.mount(dev, name);
        } else if (host_.eject) {
            host_.eject(dev, name);
        }
        return;
    }
    /* Remove selected folder: only the folders the user added; Music and the drives are always indexed */
    if (sel_ < 0 || sel_ >= static_cast<int>(rows_.size())) return;
    const int ri = rows_[static_cast<size_t>(sel_)].index;
    if (ri < 0 || ri >= static_cast<int>(d_.roots.size())) { /* a drive that is not mounted: nothing of the library's to remove */
        if (host_.notice) host_.notice(Notice::Kind::Info, "Music and USB drives are always in the library. Only folders you added can be removed.");
        return;
    }
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
    export_name_.clear();
    if (host_.show) host_.show(page_of(nav_.top().place)); /* Music folders, or the playlist an export was asked from */
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
        if (k == kKeyDel && sheet_.kind == libm::Sheet::Kind::PlaylistPage) sheet_pick(libm::sheet_index(sheet_, "delete"));
        else if (k == kUp) step(-1);
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
    if (k == kKeyDel && pl == libm::Place::Playlist && !pick_ && edit_mode_) { /* Remove the selected entry (the edit mode) */
        playlist_edit(2);
        return true;
    }
    if (skip_arrow_ && (k == kUp || k == kDown)) { /* the arrow of a Shift+arrow that moved a track: not a move of the selection too */
        skip_arrow_ = false;
        return true;
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
        const std::string id = libm::sheet_action_for_key(evdev, sheet_);
        if (id.empty() || mods) return false;
        const int i = libm::sheet_index(sheet_, id);
        if (i >= 0 && sheet_.rows[static_cast<size_t>(i)].enabled) sheet_pick(i);
        return true;
    }
    if (mods == 1 && !pick_ && edit_mode_ && !nav_.empty() && nav_.top().place == libm::Place::Playlist && (evdev == 103 || evdev == 108)) {
        skip_arrow_ = true; /* Shift+Up / Shift+Down: move the selected track (the keypad's own arrow that follows is dropped) */
        playlist_edit(evdev == 103 ? 0 : 1);
        return true;
    }
    if (!mods && !pick_ && edit_mode_ && !nav_.empty() && nav_.top().place == libm::Place::Playlist &&
        (evdev == kEvKeyU || evdev == kEvKeyD || evdev == kEvKeyX)) { /* the edit page: U up, D down, X remove (the Queue's letters) */
        playlist_edit(evdev == kEvKeyU ? 0 : evdev == kEvKeyD ? 1 : 2);
        return true;
    }
    if (!mods && !pick_ && !edit_mode_ && evdev == 57 && !nav_.empty() && nav_.top().place == libm::Place::Playlist) { /* Space: Play, as the button */
        play_playlist_first();
        return true;
    }
    if ((mods & kCtrl) && evdev == kEvKeyO) { /* the options key: the sheet of the selected row, else the Menu (the host) */
        if ((!pick_ || pick_queue_) && sel_ >= 0 && selectable(selected_type())) {
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
    } else if (r.type == libm::Row::Type::Playlist) {
        if (r.index < 0 || r.index >= static_cast<int>(d_.playlists.size())) return;
        s = libm::playlist_sheet(d_.playlists[static_cast<size_t>(r.index)].name, d_.playlists[static_cast<size_t>(r.index)].tracks, false);
    } else if (r.type == libm::Row::Type::Entry) {
        s = libm::playlist_page_sheet(entry_name(), static_cast<int>(d_.entries.size()), !r.dimmed);
    } else {
        return;
    }
    if (pick_queue_) { /* Add to the queue: Play next and Add to the queue on the song, the album or the artist */
        libm::Sheet q;
        q.kind = s.kind;
        q.title = s.title;
        q.rows = {{"next", "Play next", "N", true}, {"add", "Add to the queue", "A", true}};
        q.hint = s.hint;
        s = q;
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
    const libm::Sheet::Kind kind = sheet_.kind;
    const libm::Place pl = nav_.empty() ? libm::Place::Hub : nav_.top().place;
    const int row_index = sel_;
    close_sheet();
    if (kind == libm::Sheet::Kind::AddTo) { /* the picker: + New playlist, or one of the playlists */
        if (id == "new") {
            std::vector<std::string> names;
            for (const PlaylistRow &p : addto_lists_) names.push_back(p.name);
            ask_paths_ = addto_paths_;
            ask(Ask::AddNew, NameAsk{"New playlist", libm::plural(static_cast<int64_t>(addto_paths_.size()), "track", "tracks") + ". Name:",
                                     libm::free_name_among(names, "Playlist"), "Create"});
        } else if (id.compare(0, 3, "pl:") == 0) {
            const size_t i = static_cast<size_t>(std::atoi(id.c_str() + 3));
            if (i >= addto_lists_.size()) return;
            const std::string name = addto_lists_[i].name;
            const std::vector<std::string> paths = addto_paths_;
            auto res = std::make_shared<PlResult>(PlResult::Io);
            store_job([res, name, paths](Library &lib, PlaylistStore &st) {
                *res = st.append(name, entries_from_paths(lib, paths));
                if (*res == PlResult::Ok) playlist_reindex(lib, st, name);
            }, [this, res, name] {
                if (host_.notice) {
                    if (*res == PlResult::Ok) host_.notice(Notice::Kind::Info, "Added to " + name + ".");
                    else host_.notice(Notice::Kind::Danger, *res == PlResult::NotFound ? "That playlist is gone." : "The playlist could not be saved.");
                }
                playlists_changed();
            });
        }
        return;
    }
    if (kind == libm::Sheet::Kind::Import) {
        import_act(id);
        return;
    }
    if (kind == libm::Sheet::Kind::PlaylistPage) { /* the options of the open playlist, in the view and in the edit mode */
        const std::string name = entry_name();
        const int tracks = static_cast<int>(d_.entries.size());
        if (name.empty()) return;
        if (id == "playhere") {
            if (!r.dimmed) play_playlist_rows(r.index);
        } else if (id == "rename") begin_rename(name, tracks);
        else if (id == "delete") begin_delete(name, tracks);
        else if (id == "export") {
            if (tracks <= 0) {
                if (host_.notice) host_.notice(Notice::Kind::Info, "This playlist has no tracks to export.");
            } else {
                begin_export(name);
            }
        }
        return;
    }
    if (kind == libm::Sheet::Kind::Playlist) { /* a playlist row on the list */
        std::string name;
        int tracks = 0;
        if (r.type == libm::Row::Type::Playlist && r.index >= 0 && r.index < static_cast<int>(d_.playlists.size())) {
            name = d_.playlists[static_cast<size_t>(r.index)].name;
            tracks = d_.playlists[static_cast<size_t>(r.index)].tracks;
        } else {
            name = entry_name();
            tracks = static_cast<int>(d_.entries.size());
        }
        if (name.empty()) return;
        if (id == "play") {
            auto rows = std::make_shared<std::vector<PlaylistEntryRow>>();
            store_job([rows, name](Library &lib, PlaylistStore &st) { playlist_rows(lib, st, name, *rows); }, [this, rows, name] {
                int start = 0;
                const std::vector<std::string> play = playlist_playable(*rows, 0, &start);
                if (play.empty()) {
                    if (host_.notice) host_.notice(Notice::Kind::Info, "Nothing to play: no file of this playlist was found.");
                    return;
                }
                if (host_.play_source) host_.play_source(play, 0, store_.path_of(name));
                else if (host_.play) host_.play(play, 0);
            });
        } else if (id == "rename") begin_rename(name, tracks);
        else if (id == "delete") begin_delete(name, tracks);
        else if (id == "export") {
            if (tracks <= 0) {
                if (host_.notice) host_.notice(Notice::Kind::Info, "This playlist has no tracks to export.");
            } else {
                begin_export(name);
            }
        } else if (id == "addtracks") begin_pick(name);
        return;
    }
    if (id == "playlist") { /* Add to playlist: the picker */
        begin_add_to(r);
        return;
    }
    if (r.type == libm::Row::Type::Track) {
        const std::vector<TrackRow> &list = pl == libm::Place::Search ? d_.search.songs : d_.tracks;
        if (r.index < 0 || r.index >= static_cast<int>(list.size())) return;
        const TrackRow &t = list[static_cast<size_t>(r.index)];
        if (id == "play") {
            if (row_index >= 0 && row_index < static_cast<int>(rows_.size())) activate(row_index);
        } else if (id == "next") {
            if (host_.play_next) host_.play_next({t.path});
            if (pick_queue_) {
                pick_marks_.add({t.path});
                rebuild_rows();
            }
            if (host_.notice) host_.notice(Notice::Kind::Info, "Will play next.");
        } else if (id == "add") {
            if (host_.add_to_queue) host_.add_to_queue({t.path});
            if (pick_queue_) {
                pick_marks_.add({t.path});
                rebuild_rows();
            }
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
        if (pick_queue_ && mode != 0) {
            pick_marks_.add(paths);
            rebuild_rows();
        }
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

/* ------------------------------------------------------------------------------------------------ playlists (P2c) */

std::vector<std::string> LibraryCtl::root_paths() const {
    std::vector<std::string> p;
    if (host_.wanted)
        for (const RootSpec &r : host_.wanted()) p.push_back(r.path);
    return p;
}

std::string LibraryCtl::entry_name() const {
    if (nav_.empty() || nav_.top().place != libm::Place::Playlist) return std::string();
    return nav_.top().title;
}

/* a job on the worker with the playlist folder's store (the store is touched by the worker only); `done` runs on the UI thread */
void LibraryCtl::store_job(std::function<void(Library &, PlaylistStore &)> work, std::function<void()> done) {
    if (!available_) return;
    const std::vector<std::string> roots = root_paths();
    worker_.post([this, roots, work = std::move(work)](Library *lib) {
        if (!lib) return;
        store_.set_roots(roots);
        work(*lib, store_);
    }, std::move(done));
}

void LibraryCtl::sync_playlists() {
    store_job([](Library &lib, PlaylistStore &st) { playlist_sync(lib, st); }, nullptr);
}

void LibraryCtl::playlists_changed() {
    if (!in_library_ || nav_.empty()) return;
    const libm::Place pl = nav_.top().place;
    if (pl == libm::Place::Playlists || pl == libm::Place::Playlist) reload();
}

void LibraryCtl::ask(Ask what, const NameAsk &a) {
    ask_ = what;
    ask_spec_ = a;
    if (host_.ask_name) host_.ask_name(a);
}

void LibraryCtl::save_queue(const std::vector<std::string> &paths) {
    if (!available_ || paths.empty()) {
        if (host_.notice) host_.notice(Notice::Kind::Info, "The queue is empty.");
        return;
    }
    auto names = std::make_shared<std::vector<std::string>>();
    store_job([names](Library &lib, PlaylistStore &st) {
        playlist_sync(lib, st);
        *names = st.names();
    }, [this, names, paths] {
        ask_paths_ = paths;
        ask(Ask::SaveQueue, NameAsk{"Save queue as playlist", libm::plural(static_cast<int64_t>(paths.size()), "track", "tracks") + ". Name:",
                                    libm::free_name_among(*names, "Queue"), "Save"});
    });
}

void LibraryCtl::begin_rename(const std::string &name, int tracks) {
    ask_name_ = name;
    ask(Ask::Rename, NameAsk{"Rename playlist", libm::plural(tracks, "track", "tracks") + ". New name:", name, "Rename"});
}

void LibraryCtl::begin_delete(const std::string &name, int tracks) {
    ask_ = Ask::Delete;
    ask_name_ = name;
    if (host_.ask_confirm)
        host_.ask_confirm("Delete \"" + name + "\"?",
                          libm::plural(tracks, "track", "tracks") + ". The playlist file is deleted; the songs stay in your library.", "Delete");
}

void LibraryCtl::confirm_answer(bool yes) {
    if (ask_ != Ask::Delete) return;
    ask_ = Ask::None;
    if (!yes) return;
    const std::string name = ask_name_;
    auto res = std::make_shared<PlResult>(PlResult::Io);
    store_job([res, name](Library &lib, PlaylistStore &st) {
        *res = st.remove(name);
        playlist_reindex(lib, st, name);
    }, [this, res, name] {
        if (*res == PlResult::Ok || *res == PlResult::NotFound) {
            /* the page first (showing a page takes a notice away), then the notice */
            if (!nav_.empty() && nav_.top().place == libm::Place::Playlist && nav_.top().title == name) {
                edit_mode_ = false;
                back(); /* the open one: its page goes */
            }
            else playlists_changed();
            if (host_.notice) host_.notice(Notice::Kind::Info, "Playlist deleted.");
        } else if (host_.notice) {
            host_.notice(Notice::Kind::Danger, "The playlist could not be deleted.");
        }
    });
}

void LibraryCtl::begin_export(const std::string &name) {
    if (!host_.choose_export) return;
    export_name_ = name;
    host_.choose_export(name);
}

void LibraryCtl::export_to(const std::string &folder) {
    if (export_name_.empty() || folder.empty()) return;
    const std::string name = export_name_;
    auto res = std::make_shared<PlResult>(PlResult::Io);
    auto out = std::make_shared<std::string>();
    store_job([res, out, name, folder](Library &, PlaylistStore &st) { *res = st.export_to(name, folder, out.get()); },
              [this, res, out, folder] {
                  if (*res == PlResult::Ok) {
                      choose_cancelled(); /* back on the playlist first: showing a page takes a notice away */
                      if (host_.notice)
                          host_.notice(Notice::Kind::Info, "Exported to " + libm::where_text(folder) + ": " + base_of(*out) + ".");
                  } else if (host_.notice) {
                      host_.notice(Notice::Kind::Danger, "Could not export here: the folder cannot be written. Choose another.");
                  }
              });
}

void LibraryCtl::begin_pick(const std::string &name) {
    std::vector<std::string> in;
    for (const PlaylistEntryRow &e : d_.entries) in.push_back(e.entry.path); /* missing entries count for their path */
    pick_marks_.set(in);
    pick_saved_ = nav_;
    nav_.clear();
    pick_ = true;
    pick_name_ = name;
    libm::Loc hub;
    hub.place = libm::Place::Hub;
    open_place(hub, true);
}

void LibraryCtl::finish_pick() {
    if (!pick_) return;
    const bool q = pick_queue_;
    pick_ = false;
    pick_queue_ = false;
    nav_ = pick_saved_;
    pick_saved_.clear();
    pick_marks_.clear();
    if (q) { /* back on the Queue page */
        if (host_.open_queue) host_.open_queue();
        return;
    }
    if (!nav_.empty()) open_place(nav_.top(), false);
}

void LibraryCtl::begin_queue_pick(const std::vector<std::string> &queue_paths) {
    if (!available_) {
        if (host_.notice) host_.notice(Notice::Kind::Info, "The Library is not available.");
        return;
    }
    pick_marks_.set(queue_paths);
    pick_saved_ = nav_;
    nav_.clear();
    saved_nav_.clear();
    pick_ = true;
    pick_queue_ = true;
    pick_name_ = "the queue";
    in_library_ = true;
    visited_ = true;
    libm::Loc hub;
    hub.place = libm::Place::Hub;
    open_place(hub, true);
}

/* a tap on a song (or "Add album"): it is appended again even when it is in the playlist (owner, 2026-11-15: "Add it again, the number
 * grows"); the mark and the count change at once, before the file is written, and are taken back if the write fails */
void LibraryCtl::pick_add(const std::vector<std::string> &paths) {
    if (!pick_ || paths.empty()) return;
    const std::string name = pick_name_;
    const bool again = paths.size() == 1 && pick_marks_.count(paths[0]) > 0;
    if (pick_queue_) { /* a tap adds the song at the END of the queue (owner: "Adds it at the END of the queue, marks show") */
        if (host_.add_to_queue) host_.add_to_queue(paths);
        pick_marks_.add(paths);
        rebuild_rows();
        if (host_.notice) host_.notice(Notice::Kind::Info, "Added to the queue.");
        return;
    }
    pick_marks_.add(paths);
    rebuild_rows();
    auto res = std::make_shared<PlResult>(PlResult::Io);
    store_job([res, name, paths](Library &lib, PlaylistStore &st) {
        *res = st.append(name, entries_from_paths(lib, paths));
        if (*res == PlResult::Ok) playlist_reindex(lib, st, name);
    }, [this, res, name, paths, again] {
        if (*res != PlResult::Ok && pick_ && pick_name_ == name) {
            for (const std::string &p : paths) pick_marks_.remove(p);
            rebuild_rows();
        }
        if (!host_.notice) return;
        if (*res == PlResult::Ok) host_.notice(Notice::Kind::Info, again ? std::string("Added again.") : "Added to " + name + ".");
        else host_.notice(Notice::Kind::Danger, *res == PlResult::NotFound ? "That playlist is gone." : "The playlist could not be saved.");
    });
}

void LibraryCtl::show_sheet_obj(const libm::Sheet &s) {
    sheet_ = s;
    sheet_sel_ = 0;
    sheet_on_ = true;
    if (host_.show_sheet) host_.show_sheet(sheet_view());
}

/* Add to playlist (P): the tracks of a song, an album (in album order) or an artist, then the picker */
void LibraryCtl::begin_add_to(const libm::Row &r) {
    if (!available_) return;
    const libm::Place pl = nav_.empty() ? libm::Place::Hub : nav_.top().place;
    std::string what;
    std::string track_path;
    if (r.type == libm::Row::Type::Track) {
        const std::vector<TrackRow> &list = pl == libm::Place::Search ? d_.search.songs : d_.tracks;
        if (r.index < 0 || r.index >= static_cast<int>(list.size())) return;
        what = list[static_cast<size_t>(r.index)].title;
        track_path = list[static_cast<size_t>(r.index)].path;
    } else if (r.type == libm::Row::Type::Album) {
        const std::vector<AlbumRow> &list = pl == libm::Place::Search ? d_.search.albums : d_.albums;
        if (r.index < 0 || r.index >= static_cast<int>(list.size())) return;
        what = libm::album_name(list[static_cast<size_t>(r.index)].title);
    } else if (r.type == libm::Row::Type::Artist) {
        const std::vector<ArtistRow> &list = pl == libm::Place::Search ? d_.search.artists : d_.artists;
        if (r.index < 0 || r.index >= static_cast<int>(list.size())) return;
        what = libm::artist_name(list[static_cast<size_t>(r.index)].name);
    } else {
        return;
    }
    auto lists = std::make_shared<std::vector<PlaylistRow>>();
    auto paths = std::make_shared<std::vector<std::string>>();
    auto has = std::make_shared<std::vector<int>>();
    const libm::Row::Type type = r.type;
    const int64_t id = r.id;
    store_job([lists, paths, has, type, id, track_path](Library &lib, PlaylistStore &st) {
        playlist_sync(lib, st);
        playlist_list(lib, *lists);
        if (type == libm::Row::Type::Track) /* the playlists that have the song already ("in it", "in it x2") */
            for (const PlaylistRow &p : *lists) has->push_back(playlist_count_of(lib, p.id, track_path));
        if (type == libm::Row::Type::Track) {
            paths->push_back(track_path);
        } else {
            std::vector<TrackRow> t;
            if (type == libm::Row::Type::Album) lib.album_tracks(id, t);
            else lib.artist_tracks(id, t);
            for (const TrackRow &x : t) paths->push_back(x.path);
        }
    }, [this, lists, paths, has, what] {
        if (paths->empty()) {
            if (host_.notice) host_.notice(Notice::Kind::Info, "No track to add: its drive is not connected.");
            return;
        }
        addto_lists_ = *lists;
        addto_paths_ = *paths;
        show_sheet_obj(libm::add_to_sheet(what, *lists, *has));
    });
}

void LibraryCtl::name_answer(bool ok, const std::string &text_in) {
    const Ask what = ask_;
    if (what == Ask::None || what == Ask::Delete) return;
    if (!ok) {
        ask_ = Ask::None;
        return;
    }
    const std::string text = PlaylistStore::trim_name(text_in);
    const NameProblem np = PlaylistStore::name_problem(text);
    auto reask = [this, text_in](const std::string &notice_text) {
        NameAsk a = ask_spec_;
        a.text = text_in;
        if (host_.ask_name) host_.ask_name(a); /* the dialog first: the notice then lies over it */
        if (host_.notice) host_.notice(Notice::Kind::Danger, notice_text);
    };
    if (np != NameProblem::None) {
        reask(libm::name_problem_text(np));
        return;
    }
    const std::string old_name = ask_name_;
    const std::string src = ask_src_;
    const std::vector<std::string> paths = ask_paths_;
    auto res = std::make_shared<PlResult>(PlResult::Io);
    store_job([res, what, text, old_name, src, paths](Library &lib, PlaylistStore &st) {
        switch (what) {
        case Ask::New: *res = st.create(text); break;
        case Ask::Rename: *res = st.rename(old_name, text); break;
        case Ask::SaveQueue:
        case Ask::AddNew: *res = st.create(text, entries_from_paths(lib, paths)); break;
        case Ask::ImportAs: *res = st.import_file(src, text); break;
        default: break;
        }
        if (*res == PlResult::Ok) {
            if (what == Ask::Rename) playlist_reindex(lib, st, old_name);
            playlist_reindex(lib, st, text);
        }
    }, [this, res, what, text, reask] {
        switch (*res) {
        case PlResult::Ok: {
            ask_ = Ask::None;
            std::string msg;
            if (what == Ask::SaveQueue) msg = "Playlist saved.";
            else if (what == Ask::AddNew) msg = "Added to " + text + ".";
            else if (what == Ask::ImportAs) msg = "Imported as " + text + ".";
            if (what == Ask::Rename && !nav_.empty() && nav_.top().place == libm::Place::Playlist) nav_.top().title = text;
            if (host_.notice && !msg.empty()) host_.notice(Notice::Kind::Info, msg);
            playlists_changed();
            break;
        }
        case PlResult::Taken: reask("That name is taken. Pick another."); break;
        case PlResult::Invalid: reask("Letters, digits, space, - and _ only."); break;
        case PlResult::NotFound:
            ask_ = Ask::None;
            if (host_.notice) host_.notice(Notice::Kind::Danger, what == Ask::ImportAs ? "That playlist file is not there any more." : "That playlist is gone.");
            playlists_changed();
            break;
        default:
            ask_ = Ask::None;
            if (host_.notice) host_.notice(Notice::Kind::Danger, "The playlist could not be written.");
            break;
        }
    });
}

void LibraryCtl::playlist_edit(int what) {
    if (nav_.empty() || nav_.top().place != libm::Place::Playlist) return;
    const int idx = sel_;
    const std::string name = entry_name();
    if (idx < 0 || idx >= static_cast<int>(d_.entries.size()) || name.empty()) return;
    if ((what == 0 && idx == 0) || (what == 1 && idx == static_cast<int>(d_.entries.size()) - 1)) return;
    auto res = std::make_shared<std::pair<PlResult, int>>(PlResult::Io, idx);
    store_job([res, name, idx, what](Library &lib, PlaylistStore &st) {
        PlResult r;
        if (what == 2) r = st.remove_at(name, idx);
        else r = st.move(name, idx, what == 0 ? -1 : 1, &res->second);
        res->first = r;
        if (r == PlResult::Ok) playlist_reindex(lib, st, name);
    }, [this, res, what, idx] {
        if (res->first == PlResult::Ok) {
            const int n = static_cast<int>(d_.entries.size());
            want_sel_ = what == 2 ? std::min(idx, n - 2) : res->second;
            reload();
        } else if (res->first != PlResult::Range && host_.notice) {
            host_.notice(Notice::Kind::Danger, res->first == PlResult::NotFound ? "That playlist is gone." : "The playlist could not be saved.");
        }
    });
}

/* the Play button, and Space on the page: the playlist from its first track that is there (owner, 2026-11-15: "when from the library I am in a
 * playlist at the bottom I should not only have add track but also play") */
void LibraryCtl::play_playlist_first() {
    const std::string name = entry_name();
    int start = -1;
    const std::vector<std::string> paths = playlist_playable(d_.entries, -1, &start);
    if (paths.empty() || name.empty()) {
        if (host_.notice) host_.notice(Notice::Kind::Info, "Nothing in it can be played.");
        return;
    }
    if (host_.play_source) host_.play_source(paths, 0, store_.path_of(name));
    else if (host_.play) host_.play(paths, 0);
}

/* Enter or a tap on an entry: the queue becomes the playlist's files that are there, starting at this one */
void LibraryCtl::play_playlist_rows(int start_row) {
    const std::string name = entry_name();
    int start = -1;
    const std::vector<std::string> paths = playlist_playable(d_.entries, start_row, &start);
    if (start < 0 || paths.empty() || name.empty()) return;
    if (host_.play_source) host_.play_source(paths, start, store_.path_of(name));
    else if (host_.play) host_.play(paths, start);
}

void LibraryCtl::open_import_sheet(const std::string &file) {
    if (!available_) return;
    import_file_ = file;
    auto count = std::make_shared<int>(0);
    store_job([count, file](Library &, PlaylistStore &st) {
        *count = static_cast<int>(m3u::parse(read_bytes(file), st.parse_options(m3u::dir_of(file))).size());
    }, [this, count, file] { show_sheet_obj(libm::import_sheet(base_of(file), *count)); });
}

void LibraryCtl::import_act(const std::string &id) {
    const std::string file = import_file_;
    if (file.empty()) return;
    if (id == "play" || id == "add") {
        auto paths = std::make_shared<std::vector<std::string>>();
        store_job([paths, file](Library &, PlaylistStore &st) {
            for (const m3u::Entry &e : m3u::parse(read_bytes(file), st.parse_options(m3u::dir_of(file))))
                if (e.resolved && is_regular(e.path)) paths->push_back(e.path);
        }, [this, paths, file, id] {
            if (paths->empty()) {
                if (host_.notice) host_.notice(Notice::Kind::Info, "This playlist has no track Vinyl can find.");
                return;
            }
            if (id == "play") {
                if (host_.play_source) host_.play_source(*paths, 0, file);
                else if (host_.play) host_.play(*paths, 0);
            } else {
                if (host_.add_to_queue) host_.add_to_queue(*paths);
                if (host_.notice)
                    host_.notice(Notice::Kind::Info, "Added " + libm::plural(static_cast<int64_t>(paths->size()), "track", "tracks") + " to the queue.");
            }
        });
        return;
    }
    if (id == "import") { /* the name of the file, cleaned; a taken one asks for another, suggesting a free one */
        const std::string wanted = PlaylistStore::clean_name(playlist_stem(file));
        auto res = std::make_shared<PlResult>(PlResult::Io);
        auto suggestion = std::make_shared<std::string>();
        store_job([res, suggestion, wanted, file](Library &lib, PlaylistStore &st) {
            if (st.exists(wanted)) {
                *res = PlResult::Taken;
                *suggestion = st.free_name(wanted);
                return;
            }
            *res = st.import_file(file, wanted);
            if (*res == PlResult::Ok) playlist_reindex(lib, st, wanted);
        }, [this, res, suggestion, wanted, file] {
            if (*res == PlResult::Ok) {
                if (host_.notice) host_.notice(Notice::Kind::Info, "Imported as " + wanted + ".");
            } else if (*res == PlResult::Taken) {
                ask_src_ = file;
                ask(Ask::ImportAs, NameAsk{"Import as a playlist", "Name:", *suggestion, "Import"});
                if (host_.notice) host_.notice(Notice::Kind::Danger, "That name is taken. Pick another.");
            } else if (host_.notice) {
                host_.notice(Notice::Kind::Danger, *res == PlResult::NotFound ? "That playlist file is not there any more." : "The playlist could not be written.");
            }
        });
    }
}

/* ------------------------------------------------------------------------------------------------ drives, ticks */

void LibraryCtl::sync_mounts(bool force) {
    if (cfg_.demo || !available_ || !host_.wanted) return;
    const std::string text = read_mounts();
    /* the drives that are mounted and unreadable (a stick pulled while its mount is still listed) are gone as well: the
     * signature has each drive root's state (owner ruling 2026-11-14, B45) */
    const std::vector<RootSpec> wanted = host_.wanted();
    const std::vector<MountEntry> mounts = parse_mounts(text);
    /* a network share is never probed here: this is the UI thread, and a hard NFS mount holds a stat for as long as the server is
     * away (core/library/net_share; its mount line is in the signature, which is all that matters of it) */
    auto probed = [&](const std::string &path) { return libm::is_drive_path(path) && !share_mount_of(mounts, path); };
    std::string sig = text + "\n";
    for (const RootSpec &r : wanted)
        if (probed(r.path)) sig += r.path + (probe_root(r.path) ? " ok\n" : " gone\n");
    for (const RootInfo &r : d_.roots)
        if (probed(r.path)) sig += r.path + (probe_root(r.path) ? " ok\n" : " gone\n");
    const bool first = mounts_sig_.empty();
    if (!force && sig == mounts_sig_) return;
    mounts_sig_ = sig;
    if (first && !force) return;
    worker_.post([wanted, text](Library *lib) {
        if (lib) sync_roots(lib->db(), wanted, parse_mounts(text), "/dev/disk/by-uuid");
    }, [this] { reload(); });
}

void LibraryCtl::mounts_changed() {
    if (cfg_.demo && host_.drives && available_) { /* the scenes have no /proc mounts: a drive root is there while its drive is mounted */
        std::set<std::string> mounted;
        for (const DriveInfo &d : host_.drives())
            if (d.mounted) mounted.insert(d.mount_dir);
        worker_.post([mounted](Library *lib) {
            if (!lib) return;
            for (const RootRow &r : load_roots(lib->db()))
                if (!r.mount.empty()) set_root_online(lib->db(), r.id, mounted.count(r.path) != 0);
        }, [this] { reload(); });
    }
    sync_mounts(true);
    if (in_library_ && !nav_.empty() && nav_.top().place == libm::Place::Folders) rebuild_rows();
}

void LibraryCtl::drive_mounted(const std::string &dir) {
    if (!available_ || dir.empty()) return;
    visited_ = in_library_;
    const std::vector<RootSpec> wanted = host_.wanted ? host_.wanted() : std::vector<RootSpec>();
    const std::string text = cfg_.demo ? std::string() : read_mounts();
    worker_.post([wanted, text, dir, demo = cfg_.demo](Library *lib) {
        if (!lib) return;
        if (!demo) {
            sync_roots(lib->db(), wanted, parse_mounts(text), "/dev/disk/by-uuid");
        } else { /* the scenes have no /proc mounts: the drive's folder is made a root here */
            LibraryWriter w(lib->db());
            const size_t slash = dir.rfind('/');
            w.root(dir, slash == std::string::npos ? dir : dir.substr(slash + 1), "", dir);
        }
    }, [this, dir] {
        reload();
        if (scan_ && in_library_ && !scan_->running()) scan_->start_folder(dir, false);
    });
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
            if (st != was && st == ScanProgress::State::Done) sync_playlists(); /* a playlist file added or changed outside Vinyl */
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
    if (host_.drives && !nav_.empty() && nav_.top().place == libm::Place::Folders && now - drives_at_ >= 1000) { /* plugged in, mounted, pulled */
        drives_at_ = now;
        const std::vector<DriveInfo> now_drives = host_.drives();
        if (now_drives.size() != d_.drives.size() ||
            !std::equal(now_drives.begin(), now_drives.end(), d_.drives.begin(), [](const DriveInfo &a, const DriveInfo &b) {
                return a.dev == b.dev && a.mounted == b.mounted && a.by_vinyl == b.by_vinyl && a.mount_dir == b.mount_dir;
            }))
            rebuild_rows();
    }
    const std::string playing = host_.playing_path ? host_.playing_path() : std::string();
    if (playing != playing_) {
        const libm::Place pl = nav_.empty() ? libm::Place::Hub : nav_.top().place;
        if (pl == libm::Place::Songs || pl == libm::Place::Album || pl == libm::Place::Search || pl == libm::Place::Playlist) rebuild_rows();
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
            rv.amber = r.amber;
            rv.check = r.check;
            rv.mono_value = r.mono_value;
            rv.flush = pl == libm::Place::Folders || pl == libm::Place::Playlist;
            if (pl == libm::Place::Playlist && r.type == libm::Row::Type::Entry) rv.text = std::to_string(r.index + 1) + "  " + r.text; /* the boards number them */
            out->push_back(rv);
        }
        rows_cache_ = out;
    }
    v.rows = rows_cache_;
    v.rows_ver = rows_ver_;
    v.rows_gen = rows_gen_;
    v.selected = sel_;
    v.scroll_to = -1;
    if (restore_top_ >= 0 && !rows_.empty() && d_.loaded) { /* a place given back: its top row, once */
        v.scroll_to = restore_top_;
        restore_top_ = -1;
    }
    if (pick_ && (pl == libm::Place::Hub || pl == libm::Place::Album || pl == libm::Place::Search)) v.done_button = true; /* every page of the pick mode */
    if (pick_ && (pl == libm::Place::Artists || pl == libm::Place::Artist || pl == libm::Place::Albums || pl == libm::Place::GenreAlbums ||
                  pl == libm::Place::Songs || pl == libm::Place::Genres)) {
        v.done_button = true; /* the Done button at the bottom, with the pick heading */
        v.heading = libm::hub_pick_heading(pick_name_, pick_marks_.total());
    }
    ScanProgress prog = demo_progress_on_ ? demo_progress_ : (scan_ ? scan_->progress() : ScanProgress());
    const libm::ScanNote note = libm::scan_note(prog, prior_tracks_);
    switch (pl) {
    case libm::Place::Hub: {
        int64_t hidden = 0; /* the rows of the roots that are there (a gone drive's stay hidden and count for nothing) */
        for (const RootInfo &r : d_.roots)
            if (r.online) hidden += r.tracks;
        v.heading = pick_ ? libm::hub_pick_heading(pick_name_, pick_marks_.total()) : libm::hub_heading(d_.counts.tracks, note.shown);
        if (pick_) v.mini = MiniView(); /* the Done button takes the mini player's place */
        v.chip = "Search";
        v.mini = host_.mini ? host_.mini() : MiniView();
        if (note.shown) {
            v.note.kind = LibNote::Kind::Scan;
            v.note.text = note.text;
            v.note.pct = note.pct;
            v.note.hot = note.hot;
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
        v.chip = d_.tracks.empty() ? std::string() : (pick_ ? "Add album" : "Play album");
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
    case libm::Place::Playlists:
        v.heading = libm::playlists_heading(static_cast<int>(d_.playlists.size()));
        if (d_.loaded && d_.playlists.empty()) {
            v.empty = true;
            v.empty_after_rows = 1;
            v.empty_title = "No playlists yet";
            v.empty_sub = "Save the queue as a playlist, or add songs from the Library: hold a song and choose Add to playlist. Vinyl keeps them as .m3u8 files in Music/Playlists.";
        }
        break;
    case libm::Place::Playlist: {
        v.title = loc.title;
        bool any_playable = false;
        for (const PlaylistEntryRow &e : d_.entries) any_playable = any_playable || e.exists;
        const int n = static_cast<int>(rows_.size());
        for (int i = 0; i < 5; ++i) v.button_primary[i] = false;
        if (!edit_mode_ || pick_) { /* the view: Play (primary) and Edit; no chips, a tap never plays */
            v.heading = libm::playlist_heading(d_.pstats);
            v.buttons = {"Play", "Edit"};
            v.button_primary[0] = true;
            v.button_enabled[0] = any_playable; /* dimmed with nothing to play: a tap says so */
            v.button_enabled[1] = true;
        } else { /* the edit mode: EDITING <NAME>, N TRACKS, Done, four buttons of the same secondary look */
            v.heading = libm::playlist_edit_heading(loc.title, static_cast<int>(d_.entries.size()));
            v.done_chip = true;
            v.buttons = {"Move up", "Move down", "Remove", "Add tracks"};
            v.button_enabled[0] = sel_ > 0;
            v.button_enabled[1] = sel_ >= 0 && sel_ < n - 1;
            v.button_enabled[2] = sel_ >= 0 && sel_ < n;
            v.button_enabled[3] = true;
        }
        if (d_.loaded && d_.entries.empty()) {
            v.empty = true;
            v.empty_title = "No tracks yet";
            v.empty_sub = "Press Edit, then Add tracks, or hold a song in the Library and choose Add to playlist.";
        }
        break;
    }
    case libm::Place::Folders: {
        v.heading = "MUSIC FOLDERS, " + std::to_string(rows_.size());
        v.buttons = {"Add a folder"};
        if (host_.drives && host_.drives_available && host_.drives_available()) {
            /* the drive button: Mount on a drive plugged in and not mounted (the brown look), Eject on one Vinyl mounted, Eject
             * dimmed for one the system mounted and with no drive selected (owner, 2026-11-14) */
            std::string label = "Eject";
            bool can = false, mount = false;
            if (sel_ >= 0 && sel_ < static_cast<int>(rows_.size())) {
                const std::string &key = rows_[static_cast<size_t>(sel_)].key;
                const size_t colon = key.find(':');
                if (colon != std::string::npos) {
                    const std::string dev = key.substr(colon + 1);
                    mount = key.compare(0, 7, "ndrive:") == 0;
                    for (const DriveInfo &d : d_.drives)
                        if (d.dev == dev) label = (mount ? "Mount " : "Eject ") + d.name();
                    can = mount || key.compare(0, 7, "vdrive:") == 0;
                }
            }
            v.buttons.push_back(label);
            v.button_enabled[1] = can;
            v.button_primary[1] = mount;
            v.button_primary[0] = !mount;
        }
        v.buttons.push_back("Remove selected folder");
        v.footnote = host_.drives && host_.drives_available && host_.drives_available()
                         ? "Add opens the folder browser in choose mode. Eject unmounts a drive Vinyl mounted so you can unplug it; a drive the "
                           "system mounted is left alone. Removing a folder only removes its tracks from the library; no file is touched."
                         : "Add opens the folder browser in choose mode. Removing a folder only removes its tracks from the library; no file is "
                           "touched.";
        break;
    }
    }
    if (pick_ && v.done_button) v.heading = libm::hub_pick_heading(pick_name_, pick_marks_.total()); /* the pages' own headings give way to it */
    return v;
}

}  // namespace ui
}  // namespace vinyl
