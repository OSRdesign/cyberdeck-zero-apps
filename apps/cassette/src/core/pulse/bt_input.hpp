/*
 * SPDX-License-Identifier: MIT
 *
 * Cassette: the Bluetooth INPUT device model (task T2 of the Bluetooth release). Pure data, no server call: it turns the
 * lists the sound server returned (pulse_probe.hpp) into the entries the input chooser shows next to the ALSA cards, and
 * decides which source node to open and at which rate.
 *
 *   identity  "bt:AA:BB:CC:DD:EE:FF", upper-case MAC (the node name changes with the profile, the MAC does not).
 *   source    in music mode (profile a2dp-sink) the only capture node is a VIRTUAL source whose name carries the MAC with
 *             colons ("bluez_input.70:BF:92:53:6B:8B"); opening it makes WirePlumber switch the card to the call profile
 *             after about 0.65 s. In call mode the real source is "bluez_input.70_BF_92_53_6B_8B.0". The real one is used
 *             when the card is on a call profile, else the virtual one.
 *   rate      the CALL rate, asked for explicitly (the virtual source reports 48 kHz float: opening it "at its own rate"
 *             would give a resampled 48 kHz): the real source's own rate when it is there (8000 or 16000), else 16000 when
 *             the card offers the mSBC profile "headset-head-unit", 8000 when only "headset-head-unit-cvsd" is available.
 *   hint      the card's active profile: "music", "call" or "unknown".
 */
#pragma once

#include "pulse_probe.hpp"

#include <string>
#include <vector>

namespace cassette {

constexpr uint32_t kBtRateWide = 16000;   /* mSBC */
constexpr uint32_t kBtRateNarrow = 8000;  /* CVSD */

struct BtInput {
    std::string mac;          /* "70:BF:92:53:6B:8B" */
    std::string id;           /* "bt:70:BF:92:53:6B:8B" */
    std::string label;        /* the server's description: the BlueZ alias */
    std::string kind;         /* "headset", "headphones", "speaker", "other" */
    uint32_t rate = 0;        /* the call rate that will be requested: 16000 or 8000 */
    bool connected = false;   /* the server knows the device (a card or a source) */
    bool usable = false;      /* a source can be opened now */
    std::string hint_profile = "unknown"; /* "music" | "call" | "unknown" */
    std::string reason;       /* not usable: why */
    std::string source;       /* the node to open ("" when none) */
    std::string card;         /* the owner card's name ("" when none): its profile tells when the call link is up */
    bool virtual_source = false; /* `source` is the music-mode virtual node */
};

/* "bt:AA:BB:..." for a MAC in any case; "" when the text is not a MAC. */
std::string bt_id_of(const std::string &mac);
/* The upper-case MAC inside a "bt:..." identity; "" when the text is not one. */
std::string bt_mac_of_id(const std::string &id);
/* A virtual music-mode node: its name carries the MAC with colons. */
bool bt_source_is_virtual(const std::string &name);

/* One entry per Bluetooth device that has a microphone: a non-monitor source, or a card that offers a call profile. */
std::vector<BtInput> bt_inputs(const PulseProbe &probe);

}  // namespace cassette
