/*
 * SPDX-License-Identifier: MIT
 *
 * Cassette playback: which output plays the takes (plan question 2): the capture card's own playback side (the headset's
 * earphones) when it has one, else the default output. A choice list is offered (a settings row) only when there is more
 * than one output. LVGL-free.
 *
 * Bluetooth (docs/PULSE.md, task T3): a Bluetooth sink the sound server lists is an output "bt:<MAC>" between the cards and
 * "Default output". The choice stays a placeholder (connected false) when the device is not there. Nothing Bluetooth is listed
 * while the server is unreachable (the caller passes no list), except the placeholder of a choice made earlier.
 */
#pragma once

#include "../alsa/alsa_api.hpp"
#include "../pulse/bt_output.hpp"

#include <string>
#include <vector>

namespace cassette {

struct OutputOption {
    std::string device; /* "hw:CARD=<id>,DEV=0", "default" or "bt:AA:BB:CC:DD:EE:FF" */
    std::string label;  /* card name, "Default output", or the server's description of a Bluetooth device */
    std::string card_id; /* "" for the default output and Bluetooth devices */
    /* Bluetooth entries only (ALSA entries keep the defaults) */
    bool bluetooth = false;
    std::string kind;          /* "headset", "headphones", "speaker", "other" */
    uint32_t rate = 0;         /* the sink's own rate now */
    bool connected = true;     /* false: chosen earlier, not there now */
    bool usable = true;        /* false: it cannot play now (see reason) */
    std::string hint_profile;  /* "music", "call" or "unknown" */
    std::string reason;
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
                         const std::string &saved_device = "", const std::vector<BtOutput> *bt = nullptr,
                         const std::string &saved_bt_label = "");

/* The OutputOption of a Bluetooth output. */
OutputOption bt_output_option(const BtOutput &b);

} // namespace cassette
