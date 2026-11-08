/*
 * SPDX-License-Identifier: MIT
 *
 * Cassette playback: which output plays the takes (plan question 2): the capture card's own playback side (the headset's
 * earphones) when it has one, else the default output. A choice list is offered (a settings row) only when there is more
 * than one output. LVGL-free.
 */
#pragma once

#include "../alsa/alsa_api.hpp"

#include <string>
#include <vector>

namespace cassette {

struct OutputOption {
    std::string device; /* "hw:CARD=<id>,DEV=0" or "default" */
    std::string label;  /* card name, or "Default output" */
    std::string card_id; /* "" for the default output */
};

struct OutputPlan {
    OutputOption chosen;
    std::vector<OutputOption> choices; /* empty unless there is more than one output to pick from */
};

constexpr const char *kDefaultOutput = "default";

/* `cards` from AlsaApi::cards(); `capture_card_id` the id of the card recording ("" when none); `saved_device` a device
 * the user picked earlier in the settings row ("" for automatic). The options are: the capture card's playback side
 * first, then the other cards with a playback PCM (HDMI cards are left to the default output, as the launcher does),
 * then the default output. A saved device that is still present wins; else the capture card's own playback side; else
 * the default output. */
OutputPlan choose_output(const std::vector<CardInfo> &cards, const std::string &capture_card_id,
                         const std::string &saved_device = "");

} // namespace cassette
