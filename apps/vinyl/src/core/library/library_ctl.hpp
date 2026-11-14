/*
 * SPDX-License-Identifier: MIT
 *
 * Vinyl library: the Library controller (P2b, 0.3.0; it fills the UI's plain views, ui/view_model.hpp). It owns the LibraryWorker (every query runs on its thread, never on
 * the UI's: D-63) and the ScanThread, the stack of places the Library has been through (the hub, Artists, an artist, an
 * album, Search, Track info, the settings, the Music folders), the rows each page shows, the 40-row paging, the debounced
 * search and the actions sheet, and turns taps, holds and keys into commands for the host (the VinylApp controller): play
 * these tracks, play them next, add them to the queue.
 *
 * Scans (owner decision D27 as the owner put it on 2026-11-13: "scan at launch and when opening a folder from the
 * library but not out of the library"; the core leaves the wiring here):
 *   - launch: start_launch_scan() runs the full scan once at app start;
 *   - opening a folder from the Library (Folders, then a folder): scan_folder(); "Rescan now" and a folder just added in
 *     Music folders start a full scan on purpose;
 *   - leaving the Library after having been in it: cancel() (a cancelled full scan resumes at the next start); a scan
 *     nobody visited is left to finish; never a timer, never a background scan.
 * When the drives mounted change while the Library is open, the roots are brought in line again (a pulled stick's tracks
 * leave the views, hidden not deleted; a stick plugged in again brings them back).
 * Search: 250 ms after the last key, on the worker (a newer search replaces an older one). Esc leaves the field and the
 * page; Enter hands the keys to the results.
 * LVGL-free but for the views' plain data (ui/view_model.hpp).
 */
#pragma once

#include "ui/view_model.hpp"

#include "lib_model.hpp"
#include "library.hpp"
#include "roots.hpp"
#include "scanner.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace vinyl {
namespace ui {

struct LibHost {
    std::function<std::string()> playing_path;
    /* a new queue: these tracks, starting at `start`; the host plays and shows Now playing */
    std::function<void(const std::vector<std::string> &, int start)> play;
    std::function<void(const std::vector<std::string> &)> play_next, add_to_queue;
    std::function<void(Notice::Kind, const std::string &)> notice;
    std::function<void(Page)> show;                    /* make a page the visible one */
    std::function<void()> open_folders;                /* the Browser, from the hub's Folders row */
    std::function<void()> choose_folder;               /* the Browser in choose mode (Music folders > Add a folder) */
    std::function<std::vector<RootSpec>()> wanted;     /* the roots the library indexes now */
    std::function<std::vector<std::string>()> folders; /* the folders the user added (settings) */
    std::function<void(const std::vector<std::string> &)> set_folders;
    std::function<MiniView()> mini;
    std::function<void()> quit_library;                /* Esc at the hub: leave the Library (the host decides where) */
    std::function<void(Page, int)> scroll;             /* Track info: scroll by rows */
    std::function<int()> scroll_y;                     /* where the page shown is scrolled to, in pixels (kept when a place is left) */
    /* drives (owner decision 2026-11-14, "Mount on request in Vinyl"): the removable drives now (null: no drive support, the
     * Music folders page has no Eject), whether udisks is there, and Eject (unmounts a drive Vinyl mounted) */
    std::function<std::vector<DriveInfo>()> drives;
    std::function<bool()> drives_available;
    std::function<void(const std::string &dev, const std::string &name)> eject;
    std::function<void(const std::string &dev, const std::string &name)> mount; /* the same as a tap on its row in Folders */
    std::function<void(const std::string &, bool)> search_open; /* the field's text, editing */
    std::function<void()> search_edit;
    std::function<void()> search_end_edit;
    std::function<bool()> search_editing;
    std::function<void(const SheetView &)> show_sheet;
    std::function<void()> hide_sheet;
    std::function<void(int)> sheet_select;
};

struct LibConfig {
    std::string db_path;
    int ram_mb = 0;
    std::string home;          /* "~" in paths shown */
    bool demo = false;         /* the scenes: no mounts, no launch scan by the thread */
    uint64_t search_debounce_ms = 250;
    ScanHooks scan_hooks = default_scan_hooks(); /* the tests replace the tag reader */
};

class LibraryCtl {
public:
    LibraryCtl(LibHost host, LibConfig cfg);
    ~LibraryCtl();
    LibraryCtl(const LibraryCtl &) = delete;
    LibraryCtl &operator=(const LibraryCtl &) = delete;

    /* the database opened (false: the Library is unavailable, `error` says why; Browse and playback do not care) */
    bool available() const { return available_; }
    std::string error() const;

    /* the full scan at app start (D27) */
    void start_launch_scan();
    /* scenes: the progress shown instead of the thread's (an empty state: none) */
    void demo_progress(const ScanProgress &p, int prior_tracks);

    /* the Library places: enter at the hub (from Browse, the Now playing flow, the key B); true when a page was shown */
    void enter();
    /* the Library settings from the Menu: the settings are the first place (Esc leaves the Library) */
    void enter_settings();
    /* the host showed a Library page again after a visit elsewhere (the Menu): back in the Library */
    void reenter();
    /* show the place on top of the stack again (the Browser opened from Folders was left) */
    void resume();
    bool in_library() const { return in_library_; }
    /* the mounts changed (a drive was mounted or ejected): the roots are brought in line and the page asks again */
    void mounts_changed();
    /* a drive Vinyl mounted is open in the Browser: its root joins the library and the folder is scanned (D27: a folder
     * opened from the Library) */
    void drive_mounted(const std::string &dir);
    /* a track played to its end: its play count (one small write on the worker) */
    void note_played(const std::string &path);
    /* the host left the Library for a page that is not one of its own (Now playing, the Queue ...): the scan stops */
    void left();
    /* the Browser came up from Folders: it is part of the Library for the scan rule */
    void browser_opened(const std::string &folder); /* a folder opened from the Library: scanned (D27) */

    /* Esc and Left on a Library page: back one place; false at the hub (the host leaves the Library) */
    bool back();
    Page page() const;
    bool sheet_open() const { return sheet_on_; }
    void close_sheet();

    /* input */
    void row(Page p, int index);
    void hold(Page p, int index);
    void chip(Page p);
    void button(Page p, int index);
    bool key(uint32_t lv_key);                            /* Up, Down, Enter, Right, Left on a Library page */
    bool letter(uint32_t evdev, uint32_t mods);           /* sheet letters, "/" and Ctrl+F (Search), Ctrl+O (the sheet) */
    void query_changed(const std::string &text);
    void submit();
    void sheet_pick(int i);
    /* Music folders > Add a folder > "Use this folder": the folder joins the library and a scan starts */
    void add_folder(const std::string &path);
    /* Esc in the choose mode: back to the Music folders page */
    void choose_cancelled();

    /* the 100 ms tick: the finished jobs, the scan's changes, the debounced search, the mounts */
    void tick(uint64_t now_ms);
    /* the current Library page's view, and the sheet's */
    LibView view();
    SheetView sheet_view() const;
    uint64_t gen() const { return view_gen_; }

    /* notices the scan's end or failure produced, and whether a drive the playing track lives on went away is the host's */
    bool scan_running() const;

    /* tests */
    libm::Nav &nav() { return nav_; }
    LibraryWorker &worker() { return worker_; }
    ScanThread *scan_thread() { return scan_.get(); }
    int selected() const { return sel_; }
    const std::vector<libm::Row> &rows() const { return rows_; }
    void wait_idle();

    struct Fetched;

private:
    struct Req;
    void open_place(libm::Loc loc, bool push);
    void request(libm::Place place, int64_t id, int offset, int limit, bool append, const std::string &query);
    void reload();
    void apply(const Fetched &f, const Req &rq, uint64_t serial);
    void rebuild_rows();
    void set_selection(int i, bool scroll);
    void activate(int index);
    void open_sheet(int index);
    void sheet_act(const std::string &id);
    void tracks_job(libm::Row::Type type, int64_t id, int mode); /* 0 play, 1 next, 2 add */
    std::vector<std::string> track_paths(int from, int to) const;
    void start_full_scan(bool visible_notice);
    void sync_mounts(bool force);
    libm::Row::Type selected_type() const;

    LibHost host_;
    LibConfig cfg_;
    LibraryWorker worker_;
    std::unique_ptr<ScanThread> scan_;
    bool available_ = false;
    bool in_library_ = false, visited_ = false;
    libm::Nav nav_, saved_nav_;
    uint64_t serial_ = 0;
    uint64_t view_gen_ = 1, rows_ver_ = 1, rows_gen_ = 1;
    int sel_ = -1, want_sel_ = -1, restore_top_ = -1; /* a place given back: its row and scroll position, once its rows are in */

    /* the data of the current place */
    struct Data {
        LibraryCounts counts;
        bool counts_known = false;
        std::vector<RootInfo> roots;
        std::vector<DriveInfo> drives;
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
        bool search_done = false; /* results (maybe none) for `search.query` arrived */
        bool loaded = false;
        bool failed = false;
    } d_;
    libm::Pager pager_;
    std::vector<libm::Row> rows_;
    std::shared_ptr<std::vector<RowView>> rows_cache_;
    std::string playing_;
    uint64_t scan_gen_seen_ = 0;
    uint64_t last_scan_state_ = 0;
    ScanProgress::State last_state_ = ScanProgress::State::Idle;
    int prior_tracks_ = 0;
    bool demo_progress_on_ = false;
    ScanProgress demo_progress_;
    /* search */
    std::string query_;           /* the field's text */
    std::string pending_query_;
    bool query_pending_ = false;
    uint64_t query_at_ = 0;
    std::string searched_;        /* the text the results are for */
    bool results_mode_ = false;   /* the keys are the results': the field is not editing */
    /* sheet */
    bool sheet_on_ = false;
    libm::Sheet sheet_;
    int sheet_sel_ = 0;
    libm::Row sheet_row_;
    std::string sheet_title_;
    /* mounts */
    std::string mounts_sig_;
    uint64_t mounts_at_ = 0, drives_at_ = 0;
    uint64_t now_ms_ = 0;
    std::vector<std::string> folders_before_;
    bool choose_pending_ = false;
};

} // namespace ui
} // namespace vinyl
