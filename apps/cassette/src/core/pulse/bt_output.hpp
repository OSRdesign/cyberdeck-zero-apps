/*
 * SPDX-License-Identifier: MIT
 *
 * Cassette: the Bluetooth OUTPUT device model (task T3 of the Bluetooth release). Pure data, no server call, like bt_input.hpp:
 * it turns the lists the sound server returned (pulse_probe.hpp) into the entries the output chooser shows next to the ALSA
 * cards and "Default output", and says which sink to open.
 *
 *   identity  "bt:AA:BB:CC:DD:EE:FF", upper-case MAC (the same text as the input identity: one headset, one identity).
 *   sink      the node to open ("bluez_output.<MAC with underscores>.1": the name is the same in music mode (48 kHz stereo,
 *             SBC) and in call mode (16 kHz or 8 kHz mono)); never "default".
 *   hint      the owner card's active profile: "music" (a2dp), "call" (headset-head-unit*) or "unknown".
 *   music     whether the card offers a music profile and whether it is AVAILABLE now (another A2DP device holding the link
 *             makes it unavailable, P6): a headset without an available music profile plays at call quality only.
 */
#pragma once

#include "pulse_probe.hpp"

#include <string>
#include <vector>

namespace cassette {

struct BtOutput {
    std::string mac;          /* "70:BF:92:53:6B:8B" */
    std::string id;           /* "bt:70:BF:92:53:6B:8B" */
    std::string label;        /* the server's description: the BlueZ alias */
    std::string kind;         /* "headset", "headphones", "speaker", "other" */
    uint32_t rate = 0;        /* the sink's own rate now (48000, 16000, 8000), 0 when there is no sink */
    unsigned channels = 0;
    bool connected = false;   /* the server knows the device (a sink or a card) */
    bool usable = false;      /* a sink exists now */
    std::string hint_profile = "unknown"; /* "music" | "call" | "unknown" */
    bool music_offered = false, music_available = false, call_offered = false;
    std::string reason;       /* not usable: why */
    std::string sink;         /* the node to open ("" when none) */
    std::string card;         /* the owner card's name ("" when none) */
};

/* One entry per Bluetooth device that has a sink now, or a card with an audio profile (no sink while its profile is off). */
std::vector<BtOutput> bt_outputs(const PulseProbe &probe);

/* The entry of a MAC (any case) in a list, or null. */
const BtOutput *bt_output_find(const std::vector<BtOutput> &list, const std::string &mac);

}  // namespace cassette
