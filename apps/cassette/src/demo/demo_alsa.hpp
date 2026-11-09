/*
 * SPDX-License-Identifier: MIT
 *
 * Cassette demo mode (CASSETTE_DEMO=1, task T6c): a scripted sound system, so the whole bound app (controller and UI)
 * runs on a PC or a board with no sound card, and the headless scenes can drive it. Nothing here touches ALSA, a sound
 * device or a speaker: every card is made up, the capture is SYNTHETIC (a generated 440 Hz tone whose level sweeps
 * slowly through the meter's target zone, -20 to -4 dBFS over 6 s), and playback writes go nowhere (paced in real time
 * so the position moves as it would).
 *
 * The scripted cards (CASSETTE_DEMO_CARD):
 *   level  (default) one card with a capture level (16 steps over 0 to +23.81 dB, 8 at start), a capture switch and
 *          auto gain; its level scales the tone, so CARD moves the meter
 *   plain  one card without any mixer control (the "no card control" variant)
 *   speech one speech headset: mono, 16000 Hz only, a level and a mute switch, no auto gain
 *   both   both cards, the one with a level first (Settings > Input switches)
 *   none   no card at all (the "no sound input" page)
 * CASSETTE_DEMO_UNPLUG_MS=<ms> unplugs every card that long after the app started (reads fail as a pulled USB card's
 * do, the card leaves the list); CASSETTE_DEMO_REPLUG_MS=<ms> brings them back (the controller finds them by itself).
 *
 * The recordings go to a demo home, never the user's: CASSETTE_DEMO_HOME (a relative path is taken inside
 * $APPLAUNCH_STATE_DIR, the scene harness's own folder per screen size, else the current folder), or a fresh
 * ${TMPDIR:-/tmp}/cassette-demo-XXXXXX. The settings file lives there too (<home>/.config/cassette).
 */
#pragma once

#include "core/alsa/alsa_api.hpp"

#include <cstdint>
#include <memory>
#include <string>

namespace cassette {
namespace demo {

struct Script {
    std::string cards = "level"; /* level | plain | both | speech | none */
    int64_t unplug_ms = -1;      /* -1: never */
    int64_t replug_ms = -1;
};

/* CASSETTE_DEMO is "1" (the bound demo; other values are the UI's own synthetic states). */
bool wanted();
Script script_from_env();
std::unique_ptr<AlsaApi> make_demo_alsa(const Script &script);
/* The demo home (created). Empty with *error set when it cannot be made. */
std::string demo_home(std::string *error);

/* The synthetic signal (exposed for the tests): the tone's level in dBFS at t seconds, before the card's gain. */
double tone_level_db(double t);

} // namespace demo
} // namespace cassette
