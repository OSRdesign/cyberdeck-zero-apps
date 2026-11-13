/*
 * SPDX-License-Identifier: MIT
 *
 * Vinyl UI: what the pages show, filled by the controller (vinyl_app.cpp) and drawn by the view (vinyl_view.cpp). Plain
 * data, no LVGL.
 */
#pragma once

#include "art/cover.hpp"
#include "queue/queue.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace vinyl {
namespace ui {

enum class Page { Now, Browser, Queue, Output, Notices };

struct RowView {
    enum class Mark { None, Chevron, Dot };
    std::string text;
    std::string value;          /* duration, or the output's word */
    Mark mark = Mark::Dot;
    bool playing = false;       /* blue dot: the playing track / the chosen output */
    bool dimmed = false;        /* paired, not connected */
    bool mono_value = true;     /* the value in JetBrains Mono (durations) */
    bool operator==(const RowView &o) const {
        return text == o.text && value == o.value && mark == o.mark && playing == o.playing && dimmed == o.dimmed &&
               mono_value == o.mono_value;
    }
};

struct NowView {
    bool has_track = false;
    std::string count;      /* "03/20" */
    std::string vol;        /* "Vol 60" */
    bool vol_bt = false;
    int vol_level = -1;     /* the level the volume group shows, 0..100 (-1: not known) */
    bool vol_muted = false; /* muted: the group shows Muted (vol_level is the level it comes back at) */
    bool queue_empty = false; /* nothing to play: the transport keys dimmed */
    bool vol_adjustable = false; /* the group's "-" / "+" change it; false: a tap says the volume is not available */
    std::string title, artist, album;
    std::string format;     /* "FLAC 44.1 kHz / 16 bit" */
    std::string output;     /* "Evolve 65, music quality" */
    bool output_bt = false;
    std::string extra_chip; /* "Gapless" (square, large) */
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
};

struct MiniView {
    bool shown = false;
    std::string title, sub; /* sub: the artist (dots when long) */
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
    std::string hint;       /* output: the strip's text */
    bool hint_notice = false; /* output: the strip is the "not set up" notice */
};

struct Notice {
    enum class Kind { Info, Bluetooth, Call, Danger };
    Kind kind = Kind::Info;
    std::string text;
    uint64_t gen = 0;
};

/* What the pages ask the controller to do. */
struct Actions {
    std::function<void()> back, toggle, prev, next, shuffle, repeat;
    std::function<void(const std::string &)> button; /* "queue", "output", "browse" */
    std::function<void(double)> seek_to;             /* 0..1 of the track */
    std::function<void(Page, int)> row;              /* a tap on a row */
    std::function<void(int)> queue_button;           /* 0 move up, 1 move down, 2 remove, 3 clear all */
    std::function<void()> mini_open;                 /* the mini player (opens now playing) */
    std::function<void(bool, int)> transport;        /* previous (false) / next (true): 0 press, 1 release, 2 cancel */
    std::function<void(int)> volume_step;            /* the volume group's "-" / "+": -5 / +5 */
    std::function<void()> volume_unavailable;        /* a tap on the volume chip where nothing can change the volume */
    std::function<void()> queue_clear;               /* "Clear the queue?" answered Clear */
};

} // namespace ui
} // namespace vinyl
