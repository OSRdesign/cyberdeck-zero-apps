/*
 * SPDX-License-Identifier: MIT
 *
 * Cassette: the card's capture level through the sound server (0.4.1 draft.2). On a board whose sound server is PulseAudio (the
 * Panasonic FZ-M1 tablet, Armbian x86) the server owns the ALSA "Capture" mixer element: it keeps its own saved volume of the
 * card's source and writes it back, so a direct ALSA write is reset (the CARD gain stuck at -17.3 dB). When a server answers, the
 * level is therefore set on the server's SOURCE of the card, through the pactl command line (a few calls at a card open and one per
 * change; libpulse would need the server connection the Bluetooth worker owns), and READ BACK from the ALSA control, which is
 * what the server really wrote to the hardware.
 *
 * The mapping (the only semantics this module needs; checked against the tablet, see docs/decisions.md): a source's volume is a
 * pa_volume_t V (0x10000 = 100 % = 0 dB, dB = 60 * log10(V / 0x10000), the "cubic" scale pactl prints as "3277 / 5% / -78.00 dB").
 * The source's BASE volume B ("Base Volume: 6554 / 10% / -60.00 dB") is the V at which the hardware gain is 0 dB, so the
 * hardware gain is  G = dB(V) - dB(B)  and for a wanted card gain G:  V = B * 10^(G / 60).  On the tablet B = -60 dB, so the card's
 * -17.25 .. +30 dB is V = 5.2 % .. 31.6 % (and 100 % would ask for +60 dB, clamped by the server to the card's +30). A source
 * without a base volume uses 0x10000. V is passed to `pactl set-source-volume` as a plain integer (a raw pa_volume_t: no sign,
 * no locale, no percent rounding).
 *
 * Nothing here talks to a server that is not there: the real runner looks for the server's socket first (pactl would start a
 * PulseAudio daemon next to PipeWire, or on a console board, through autospawn) and runs pactl with LC_ALL=C and a time limit.
 * Everything that parses pactl text, and the mapping, is pure and tested with canned output.
 */
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace cassette {

/* Runs `pactl <args>`. true when it ran, exited 0 and printed its output into `out` (stdout only). false: no server socket, pactl
 * missing, a non-zero exit or the time limit. */
class PactlRunner {
public:
    virtual ~PactlRunner() = default;
    virtual bool run(const std::vector<std::string> &args, std::string &out, int timeout_ms) = 0;
};
/* The production runner (posix_spawnp, LC_ALL=C, stderr to /dev/null). Never null. */
std::unique_ptr<PactlRunner> make_real_pactl();

/* One source of `pactl list sources`. */
struct ServerSource {
    std::string name;
    bool monitor = false;      /* the monitor of a sink: never a microphone */
    int alsa_card = -1;        /* alsa.card, else api.alsa.card, else the N of an "hw:N" api.alsa.path; -1 none (Bluetooth, software) */
    uint32_t base_raw = 0;     /* Base Volume (0: none listed) */
    uint32_t volume_raw = 0;   /* the loudest channel (0: none listed) */
};

std::vector<ServerSource> parse_pactl_sources(const std::string &text);
/* "Default Source: x" of `pactl info`; "" when none. server_name: "Server Name: x" ("pulseaudio", "PulseAudio (on PipeWire 1.4.2)"). */
std::string parse_pactl_default_source(const std::string &info);
std::string parse_pactl_server_name(const std::string &info);
/* The source of ALSA card `alsa_card`: the default source when it is that card's, else the first non-monitor source of the card.
 * nullptr when none. */
const ServerSource *pick_server_source(const std::vector<ServerSource> &v, const std::string &default_source, int alsa_card);

/* V for a wanted card gain, and the card gain of a V (see the mapping above). base_raw 0 = 0x10000. */
uint32_t server_volume_for_gain(double gain_db, uint32_t base_raw);
double server_gain_of_volume(uint32_t volume_raw, uint32_t base_raw);

/* One card's source on the server. */
class ServerGain {
public:
    explicit ServerGain(PactlRunner &r) : run_(r) {}
    /* Asks the server (pactl info, pactl list sources) for the source of ALSA card `alsa_card`. true: a server answers and has one. */
    bool resolve(int alsa_card);
    bool active() const { return !source_.empty(); }
    const std::string &source() const { return source_; }
    const std::string &server_name() const { return server_name_; }
    uint32_t base_raw() const { return base_raw_; }
    /* pactl set-source-volume <source> <V for gain_db>; true when pactl accepted it. */
    bool set_gain_db(double gain_db);
    /* The volume V the server has on the source now (pactl list sources again); false when it cannot be read. */
    bool read_volume(uint32_t &volume_raw);
    uint32_t last_asked() const { return asked_; }

private:
    PactlRunner &run_;
    std::string source_, server_name_;
    uint32_t base_raw_ = 0, asked_ = 0;
};

} // namespace cassette
