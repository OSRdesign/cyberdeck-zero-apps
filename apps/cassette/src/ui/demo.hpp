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
 *   playing        playing Take 006
 *   wind           rewinding at 5x while playing (CassetteWind)
 *   paused         a recording paused
 *   noinput        no sound input found
 *   idle           nothing running (the default)
 *   stereo         recording two channels
 *   long           recording for more than an hour (H:MM:SS)
 * plus, for every state, the settings rows, five takes and the trim state of Take 006 (two splits, the second
 * selected); CASSETTE_DEMO_TRIM=none starts the trim page without splits, =unselected with none selected;
 * CASSETTE_DEMO_BOX=save|confirm opens a box; CASSETTE_DEMO_NOTICE=<text> shows a notice (until a tap dismisses it);
 * CASSETTE_DEMO_SETTINGS=full adds the card-dependent rows (Mic mute) and the Output row; CASSETTE_DEMO_PAGE=settings|list|trim starts on that page.
 *
 * DemoDriver's UiActions change the model the way the controller would in simple cases (the steppers step, toggles
 * flip, a choice is taken, a tap on the countdown cancels it, the trim marker, zoom, flags, IN / OUT and the boxes
 * follow; a small deck: REC records, PLAY/PAUSE plays Take 006 or pauses, STOP stops, REW / FWD held scan at 5x and
 * with CASSETTE_DEMO_REWFWD=jump each click jumps 5 s; DELETE on the list asks in the confirm box) and log each call
 * ("[cassette-demo] ..."), so the scenes can prove that a touch or a key reached its action.
 */
#pragma once

#include "view_model.hpp"

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
    explicit DemoDriver(ViewModel vm) : vm_(std::move(vm)) {}
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
    ViewModel vm_;
    Transport before_scan_ = Transport::Playing;
    int pending_delete_ = -1; /* the take the confirm box asks about (-1: the replace of the save box) */
};

} // namespace ui
} // namespace cassette
