/*
 * SPDX-License-Identifier: MIT
 *
 * Vinyl UI: what the pages show, filled by the controller (vinyl_app.cpp) and drawn by the view (vinyl_view.cpp). Plain
 * data, no LVGL.
 */
#pragma once

#include "art/cover.hpp"
#include "menu/menu_model.hpp"
#include "queue/queue.hpp"

#include <algorithm>
#include <cstdint>
#include <functional>
#include <memory>
#include <utility>
#include <algorithm>
#include <string>
#include <vector>

namespace vinyl {
namespace ui {

/* Notices: a demonstration page for the scenes only. Menu .. About: the Menu pages (P2a, ui/menu_pages.hpp). */
enum class Page {
    Now, Browser, Queue, Output, Notices, Menu, Sleep, Playback, ReplayGain, Boost, VolumeSteps, Keys, About,
    /* P2b, the Library: the hub (Folders opens the Browser), the lists, an album's tracks, Search, Track info, the Library
     * settings and the Music folders (ui/library_pages.hpp, ui/library_ctl.hpp) */
    LibHub, LibArtists, LibArtist, LibAlbums, LibAlbum, LibSongs, LibGenres, LibSearch, LibInfo, LibSettings, LibFolders,
    /* P2c, the playlists: the list (with "+ New playlist") and one playlist's edit page */
    LibPlaylists, LibPlaylist,
    /* the Transition pick list (from Playback; P2d's Crossfade list until the owner merged it with Gapless, 2026-11-20), the
     * Equalizer page (ui/eq_page.hpp) and its presets' pick list */
    Transition, Eq, EqPreset,
};
inline bool is_lib_page(Page p) { return p >= Page::LibHub && p <= Page::LibPlaylist; }

struct RowView {
    enum class Mark { None, Chevron, Dot };
    std::string text;
    std::string value;          /* duration, or the output's word */
    Mark mark = Mark::Dot;
    bool playing = false;       /* blue dot: the playing track / the chosen output */
    bool dimmed = false;        /* paired, not connected */
    bool mono_value = true;     /* the value in JetBrains Mono (durations) */
    bool heading = false;       /* P2b: a section heading ("DISC 2", "SONGS"): grey capitals, never tapped */
    bool flush = false;         /* P2b: no mark and the text at the left edge (the Music folders rows) */
    bool amber = false;         /* the action dot is amber (a drive that is not mounted: a tap mounts it) */
    int check = 0;              /* P2c, Add tracks: the track is already in the playlist this many times (a green check; "x2" from 2) */
    /* 0.8.0, network shares: the small server glyph (teal; amber and slashed while the share does not answer; the "+" of "Add a
     * network folder"): at the row's left on a flush row (Music folders), before the value on a track row (the queue); the
     * second line under the text (Music folders: "192.168.68.142 / home-share", amber for "Not answering"); the teal text */
    enum class Glyph { None, Share, ShareDown, Add };
    Glyph glyph = Glyph::None;
    std::string sub;
    bool sub_amber = false;
    bool accent = false;
    bool operator==(const RowView &o) const {
        return text == o.text && value == o.value && mark == o.mark && playing == o.playing && dimmed == o.dimmed &&
               mono_value == o.mono_value && heading == o.heading && flush == o.flush && amber == o.amber && check == o.check &&
               glyph == o.glyph && sub == o.sub && sub_amber == o.sub_amber && accent == o.accent;
    }
};

/* Where a row's title starts: after the dot when the row has one (the same indent as the song rows), never under it. Only a row
 * without a dot may be flush at the left edge (Music folders). Pure, so the tests check it at every size (owner board test
 * 2026-11-14: the amber dot of an unmounted drive was drawn over the first letter). */
inline double row_title_x(const RowView &v, double dot_x, double text_x) {
    if (v.mark == RowView::Mark::Dot) return text_x;
    return v.flush ? dot_x : text_x;
}
inline int row_dot_diameter(double s) { return std::max(4, static_cast<int>(10 * s + 0.5)); }

struct NowView {
    bool has_track = false;
    std::string count;      /* "03/20" */
    std::string vol;        /* "Vol 60" */
    bool vol_bt = false;
    int vol_level = -1;     /* the level the volume group shows, 0..100 (-1: not known) */
    std::string vol_value;  /* its text as the controller formats it ("60"; "-22.5 dB" in the planned dB mode) */
    bool vol_muted = false; /* muted: the group shows Muted (vol_level is the level it comes back at) */
    bool queue_empty = false; /* nothing to play: the transport keys dimmed */
    bool vol_adjustable = false; /* the group's "-" / "+" change it; false: a tap says the volume is not available */
    std::string title, artist, album;
    std::string format;     /* "FLAC 44.1 kHz / 16 bit" */
    std::string output;     /* "Evolve 65, music quality" */
    bool output_bt = false;
    std::string extra_chip; /* the transition chip: "Gapless", "Crossfade 10 s", none for Normal (medium, square, large) */
    int share = 0;          /* 0.8.0: the track is on a network share: 1 the server glyph (teal), 2 not answering (amber, slashed) */
    std::string share_name; /* its name in the chip beside the format ("home-share"; the narrow portrait shows the glyph alone) */
    uint64_t pos_ms = 0, total_ms = 0;
    bool playing = false;
    bool shuffle = false;
    Repeat repeat = Repeat::Off;
    std::string upnext;     /* "Amerie, 1 Thing (Siik Remix)" */
    std::shared_ptr<const PixelImage> cover; /* null: the drawn sleeve */
    uint64_t track_gen = 0; /* a new track (the record slides out again) */
    std::string queue_heading;            /* large: "QUEUE, 20 TRACKS" */
    std::shared_ptr<const std::vector<RowView>> queue_rows; /* large: the queue pane (shared: built when it changes) */
    uint64_t queue_ver = 0;               /* changes when the rows' contents change */
    int queue_current = -1, queue_selected = -1;
    bool queue_selected_by_user = false; /* queue_selected is the pane's cursor (a key or a tap), not the track after the playing one */
};

struct MiniView {
    bool shown = false;
    std::string title, sub; /* sub: the artist (dots when long) */
    int share = 0;          /* 0.8.0: the track is on a network share: 1 the glyph (teal), 2 not answering (amber, slashed) */
    std::string share_name; /* ... its name, before the times ("home-share, 01:24 / 04:04") */
    std::string time;       /* "01:14 / 4:04", "0:01:14 / 1:02:30": always whole */
    double progress = 0;
    bool playing = false;
};

struct ListView {
    std::string left;       /* "Browse", "Queue", "Output" */
    std::string vol;
    bool vol_bt = false;
    bool show_vol = true;
    std::string heading;    /* "MUSIC / PHOENIX", "3 OF 20, 1 H 24 MIN LEFT" */
    std::string sort;       /* "A-Z" (browser) */
    std::shared_ptr<const std::vector<RowView>> rows; /* built only when its contents change (a queue may hold thousands) */
    uint64_t rows_ver = 0;  /* changes with the rows' contents */
    int selected = -1;
    int scroll_to = -1;     /* make this row the first visible one (the playing track), -1 = keep */
    uint64_t rows_gen = 0;  /* new contents (reset the scroll) */
    MiniView mini;          /* browser */
    unsigned queue_buttons = 0xF; /* queue: the buttons that can act (core/nav queue_buttons_enabled), the others dimmed */
    std::string hint;       /* output: the strip's text */
    bool hint_notice = false; /* output: the strip is the "not set up" notice */
    /* browser, choose mode (P2b: Music folders > Add a folder): the strip asks "Use folder", the mini player is away */
    bool choose = false;
    std::string choose_text; /* the strip's second line */
    bool save_chip = false;  /* queue (P2c): the "Save as playlist" chip at the heading's right */
};

/* The Library pages (P2b): one view per page, filled by the library controller. */
struct LibNote {
    enum class Kind { None, Scan, Offline };
    Kind kind = Kind::None;
    std::string text;
    int pct = -1;     /* Scan: the bar, 0..100, -1 none */
    bool hot = false; /* Scan: paused for heat, the bar red */
    bool operator==(const LibNote &o) const { return kind == o.kind && text == o.text && pct == o.pct && hot == o.hot; }
};

struct LibView {
    std::string title;      /* the header's title where it names the page (an artist's name); "" keeps the page's own */
    std::string heading;
    std::shared_ptr<const std::vector<RowView>> rows;
    uint64_t rows_ver = 0;  /* changes with the rows' contents */
    uint64_t rows_gen = 0;  /* a new list (the scroll resets) */
    int selected = -1;
    int scroll_to = -1;
    std::string chip;       /* the top-right chip: the hub's "Search", an album's "Play album" ("" none) */
    LibNote note;           /* hub: the scan strip or the drive note */
    MiniView mini;          /* hub */
    std::string vol;
    bool vol_bt = false;
    /* an empty page: the message replaces the rows */
    bool empty = false;
    std::string empty_title, empty_sub, empty_button;
    /* Track info */
    std::vector<std::pair<std::string, std::string>> info;
    /* Library settings: the cream block over the rows while a scan runs (its heading, the bar, the sentence) */
    LibNote block;
    std::string block_title, block_text;
    /* Music folders: the two buttons and the note under them */
    std::vector<std::string> buttons;
    /* 0.8.0: the network share selected in Music folders: its bar at the panel's bottom (its name, Rescan, Remove: buttons 30
     * and 31) instead of the buttons and the note; on the large class a panel at the right with these lines and the two buttons */
    bool share_bar = false;
    bool share_down = false;
    std::string share_name;
    std::vector<std::string> share_lines;
    bool button_enabled[5] = {true, true, true, true, true};
    bool button_primary[5] = {true, false, false, false, false}; /* the brown look: Add a folder, or Mount <drive> when that is what the drive button does */
    std::string footnote;
    /* P2c, a playlist's edit page: the chips at the heading's right ("Rename", "Export M3U"; a tap is a button of index 10 + i) and
     * the five buttons under the rows (Move up, Move down, Remove, Add tracks, Play: the last one in the primary look) */
    std::vector<std::string> chips;
    /* the Playlists page with no playlist: the message sits under this many rows (the "+ New playlist" row stays) */
    int empty_after_rows = 0;
    /* Add tracks (pick mode): the "Done" chip at the heading's right on the list pages (a tap is the button of index 20) */
    bool done_chip = false;
    /* Add tracks / Add to the queue: the full-width "Done" button at the bottom, where the mini player strip is (the hub hides the strip) */
    bool done_button = false;
    /* Search: the field's text and the line shown while it is empty */
    std::string query;
};

/* The actions sheet (Track, Album, Artist). */
struct SheetView {
    struct Row {
        std::string label, key;
        bool enabled = true;
    };
    std::string title, hint;
    std::vector<Row> rows;
    int selected = 0;
};

/* a Menu list page (Menu, Sleep timer, Playback, ReplayGain): core/menu's rows */
struct MenuView {
    std::vector<menu::Row> rows;
    int selected = -1;
    std::string heading;     /* the line over the rows ("STOP PLAYING AFTER"), "" for none */
    std::string hint;        /* the strip under the rows ("" for none) */
    bool hint_clock = false; /* the strip's icon: the clock (the sleep timer's status) instead of the info mark */
    bool value_bright = false; /* Playback: the values white and bold (Playback-480x320), else grey (Menu-480x320) */
    uint64_t gen = 0;        /* the page opened again: the selection scrolled into view */
};

/* P2d: the Equalizer page (EQ-480x320, EQ-320x480) */
struct EqView {
    EqGains gains{};     /* the curve chosen (shown even while the EQ is Off) */
    int band = 0;        /* the selected band (its outline, the band line) */
    bool on = true;      /* the last button: "On" on teal, or "Off" on the button colour */
    std::string preset;  /* "PRESET: BASS BOOST" */
    bool operator==(const EqView &o) const { return gains == o.gains && band == o.band && on == o.on && preset == o.preset; }
};

struct Notice {
    enum class Kind { Info, Bluetooth, Call, Danger };
    Kind kind = Kind::Info;
    std::string text;
    uint64_t gen = 0;
};

/* A name asked for in a dialog with a text field (Save the queue, New playlist, Rename, Import as): the title, the line over the field
 * ("20 tracks. Name:"), its first text and the primary button's word. The answer is Actions::name_answer. */
struct NameAsk {
    std::string title, sub, text, ok;
};

/* What the pages ask the controller to do. */
struct Actions {
    std::function<void()> back, toggle, prev, next, shuffle, repeat;
    std::function<void(const std::string &)> button; /* "queue", "output", "browse", "menu", "sleep" */
    std::function<void(double)> seek_to;             /* 0..1 of the track */
    std::function<void(Page, int)> row;              /* a tap on a row (the list pages, the Menu pages) */
    std::function<void(int)> queue_button;           /* 0 move up, 1 move down, 2 remove, 3 clear all */
    std::function<void()> mini_open;                 /* the mini player (opens now playing) */
    std::function<void(bool, int)> transport;        /* previous (false) / next (true): 0 press, 1 release, 2 cancel */
    std::function<void(int)> volume_step;            /* the volume group's "-" / "+": one level down / up */
    std::function<void(int, int)> volume_hold;       /* the same as a held button: (-1 / +1, 0 press, 1 release, 2 cancel); a hold repeats */
    std::function<void()> volume_unavailable;        /* a tap on the volume chip where nothing can change the volume */
    /* P2b */
    std::function<void(Page, int)> lib_row;          /* a tap on a Library row */
    std::function<void(Page, int)> lib_hold;         /* a 1.5 s hold on a Library row */
    std::function<bool(Page, int)> lib_hold_ok;      /* P2c, the Browser: does this row have a sheet (a .m3u / .m3u8 row) */
    std::function<void(Page)> lib_chip;              /* the hub's Search chip, an album's Play album */
    std::function<void(Page, int)> lib_button;       /* Music folders' buttons, an empty page's button; Browser: choose mode's "Use this folder" */
    std::function<void(const std::string &)> lib_query; /* the search field's text changed */
    std::function<void()> lib_submit;                /* Enter in the search field */
    std::function<void()> lib_field_escape;          /* Esc in the search field */
    std::function<void(int)> sheet_pick;             /* a tap on a sheet row */
    std::function<void()> sheet_dismiss;             /* a tap outside the sheet */
    /* P2c */
    std::function<void(bool, const std::string &)> name_answer; /* the name dialog: true = the primary button, with the text */
    std::function<void(bool)> confirm_answer;                   /* the question dialog (Delete "Evening"?): true = the destructive answer */
    std::function<void()> queue_save;                           /* the Queue page's "Save as playlist" */
    /* Knock-knock (2026-11-16): every touch of the screen, whatever it lands on (phase 0 press, 1 move, 2 release, 3 lost;
     * the point in screen px; on_cover: the press landed on the cover square's zone of Now playing) */
    std::function<void(int, int, int, bool)> touch;
    /* P2d, the Equalizer page: a band's column (band, phase 0 press: select it, 1 dragged to `db`, 2 released, 3 lost) and
     * its buttons (0 "-", 1 "+", 2 Preset, 3 Reset, 4 On / Off) */
    std::function<void(int, int, int)> eq_band;
    std::function<void(int)> eq_button;
};

} // namespace ui
} // namespace vinyl
