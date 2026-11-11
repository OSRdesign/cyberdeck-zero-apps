/*
 * SPDX-License-Identifier: MIT
 *
 * Cassette sound server: a pure-data summary of what the server lists, for the input/output choosers and the probe CLI.
 * No server call: it works on the lists PulseApi returned (or the fake's). LVGL-free.
 *
 *   - Bluetooth: the node or card name starts with "bluez_", the owner card is a bluez_card, device.bus is "bluetooth" or
 *     device.api is "bluez5". The MAC comes from the properties, else from the name.
 *   - Kind hint per Bluetooth device: Headset when it has a Bluetooth microphone source (a monitor does not count) or its
 *     card offers a call profile (a headset in music mode shows a sink only); else Headphones when the form factor says
 *     headphone / headset / hands-free / earbuds, else Speaker. Non-Bluetooth devices are Other.
 *   - Per Bluetooth card: whether a music profile (a2dp-sink*) is offered AND available, which profile is active, whether
 *     that one is music or call quality (study C17 / L4: a headset can be limited to the call profile while another A2DP
 *     device holds the link, P6).
 */
#pragma once

#include "pulse_api.hpp"

#include <string>
#include <vector>

namespace cassette {

enum class PulseKind { Other, Headset, Headphones, Speaker };
const char *pulse_kind_name(PulseKind k);

struct PulseProbeDevice {
    std::string name;
    std::string description;
    bool source = true;     /* false: a sink */
    bool bluetooth = false;
    std::string mac;        /* "70:BF:92:53:6B:8B", "" when not Bluetooth or unknown */
    PulseKind kind = PulseKind::Other;
    uint32_t rate = 0;      /* the device's own rate */
    unsigned channels = 0;
    bool monitor = false;   /* a sink's monitor (never a microphone) */
    std::string card;       /* the owner card's name, "" when none */
    std::string profile;    /* Bluetooth: the profile behind this node ("headset-head-unit"), from its properties */
    std::string codec;      /* Bluetooth: "msbc", "cvsd", "sbc", ... when the server says */
    bool is_default = false; /* the server's default source / sink */
};

struct PulseProbeCard {
    std::string name;
    std::string description;
    std::string mac;
    bool bluetooth = false;
    bool music_offered = false;   /* an a2dp-sink* profile is listed */
    bool music_available = false; /* ... and available (P6: not while another A2DP device holds the link) */
    bool call_offered = false;    /* a headset-head-unit* profile is listed (the device has a microphone) */
    std::string active_profile;
    bool active_is_music = false;
    bool active_is_call = false;
    std::vector<std::string> music_profiles, call_profiles;
};

struct PulseProbe {
    std::vector<PulseProbeDevice> devices; /* sources first (server order), then sinks */
    std::vector<PulseProbeCard> cards;     /* Bluetooth cards only */
    std::string default_source, default_sink;
    /* The kind of the Bluetooth device with this MAC (Other when unknown). */
    PulseKind kind_of(const std::string &mac) const;
    const PulseProbeCard *card_of(const std::string &mac) const;
};

PulseProbe pulse_probe(const std::vector<PulseDevice> &sources, const std::vector<PulseDevice> &sinks,
                       const std::vector<PulseCard> &cards, const PulseServerInfo *server = nullptr);

}  // namespace cassette
