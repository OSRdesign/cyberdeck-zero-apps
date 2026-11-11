/*
 * SPDX-License-Identifier: MIT
 *
 * Cassette UI: the demonstration model for the headless scenes (and for running the UI before the controller is
 * bound). Every value is SYNTHETIC: made-up take names, sizes and times, peaks from a fixed formula (the boards'
 * own); nothing comes from a recording.
 *
 * CASSETTE_DEMO=<scene> picks the starting state:
 *   recording      recording a mono take, the level in the TARGET zone (the board Cassette)
 *   recording-low  the same with a low level (under the zone)
 *   nocard         recording, the card has no capture level control (CassetteNoCard)
 *   countdown      the record countdown at 3 (CassetteCount)
 *   playing        playing Take 006 (the played level on both bars: a mono take drives L and R equally)
 *   playing-meter  the same with the played level at -12 dBFS
 *   play-paused    Take 006 paused (the played level falling away)
 *   wind           rewinding at 5x while playing, REW held (CassetteWind)
 *   fwd            scanning forward at 5x while playing, FWD held
 *   paused         a recording paused
 *   noinput        no sound input found
 *   idle           nothing running, Take 006 loaded (the default)
 *   deckempty      nothing running and no take loaded: the blank cassette (no name on the label), PLAY is dimmed
 *   notakes        the same with an empty recordings list
 *   onetake        idle, Take 006 loaded and the only take in the list (deleting it brings the blank cassette)
 *   stereo         recording two channels
 *   long           recording for more than an hour (H:MM:SS)
 * plus, for every state, the settings rows, five takes and the trim state of Take 006 (two splits, the second
 * selected); CASSETTE_DEMO_TRIM=none starts the trim page without splits, =unselected with none selected;
 * CASSETTE_DEMO_BOX=save|confirm opens a box; CASSETTE_DEMO_NOTICE=<text> shows a notice (until a tap dismisses it);
 * CASSETTE_DEMO_SETTINGS=full adds the card-dependent rows (Mic mute) and the Output row; CASSETTE_DEMO_PAGE=settings|list|trim starts on that page;
 * CASSETTE_DEMO_MERGE=0|1|3|box|confirm|progress|done shows merge takes (merge mode with 0, 1 or 3 takes chosen and the
 * incompatible takes dimmed, the merge box, the delete-originals confirm, the progress box, the list after a merge).
 *
 * DemoDriver's UiActions change the model the way the controller would in simple cases (the steppers step, toggles
 * flip, a choice is taken, a tap on the countdown cancels it, the trim marker, zoom, flags, IN / OUT and the boxes
 * follow; a small deck: REC records, PLAY/PAUSE plays Take 006 or pauses, STOP stops, REW / FWD held scan at 5x and
 * with CASSETTE_DEMO_REWFWD=jump each click jumps 5 s; DELETE on the list asks in the confirm box) and log each call
 * ("[cassette-demo] ..."), so the scenes can prove that a touch or a key reached its action.
 */
#pragma once

#include "view_model.hpp"

#include <algorithm>
#include <functional>
#include <string>

namespace cassette {
namespace ui {

ViewModel demo_model(const std::string &scene);

/* The synthetic peaks: the window around marker_ms (span_ms, n values) and the whole take (n values). */
std::vector<float> demo_window_peaks(std::int64_t length_ms, std::int64_t marker_ms, std::int64_t span_ms, int n);
std::vector<float> demo_overview_peaks(std::int64_t length_ms, int n);

class DemoDriver {
public:
    explicit DemoDriver(ViewModel vm) : vm_(std::move(vm))
    {
        /* merge mode from the model (CASSETTE_DEMO_MERGE): the chosen rows in their order */
        for (int n = 1;; ++n) {
            int found = -1;
            for (std::size_t i = 0; i < vm_.list.takes.size(); ++i)
                if (vm_.list.takes[i].merge_order == n) found = static_cast<int>(i);
            if (found < 0) break;
            merge_sel_.push_back(found);
        }
        loaded_ = -1;
        if (vm_.rec.tape.loaded && !vm_.rec.tape.label.empty()) {
            loaded_ = std::min(1, static_cast<int>(vm_.list.takes.size()) - 1); /* Take 006: the second row, the only one in "onetake" */
        }
    }
    /* The actions; after each change `changed` runs with the new model (bind it to CassetteUi::update). */
    UiActions actions();
    std::function<void(const ViewModel &)> changed;
    const ViewModel &model() const { return vm_; }
    /* A notice (empty: none), as the binding would set it. */
    void set_notice(const std::string &text)
    {
        vm_.notice = text;
        emit();
    }
    /* CASSETTE_DEMO_LIVE=1 (the timing scene): a moving synthetic level and a running counter, called at the meter's
     * rate (every 33 ms) with the elapsed time, as the controller would update the model. */
    void tick_live(std::uint32_t elapsed_ms);

private:
    void emit();
    void refresh_trim();
    void deck(KeyId k, KeyEdge e);
    void set_transport(Transport t);
    void show_take(int index); /* the cassette and the strip name list row `index`, counter at its start */
    void stop_preview();
    void merge_toggle(int i);
    ViewModel vm_;
    std::vector<int> merge_sel_; /* merge mode: the chosen rows, in the order chosen */
    int loaded_ = 1;      /* the list row in the deck (Take 006: what PLAY plays) */
    int previewing_ = -1; /* the list row previewing (list_activate), -1 none */
    Transport before_scan_ = Transport::Playing;
    int pending_delete_ = -1; /* the take the confirm box asks about (-1: the replace of the save box) */
};

} // namespace ui
} // namespace cassette
