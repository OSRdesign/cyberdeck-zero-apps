/*
 * SPDX-License-Identifier: MIT
 *
 * Vinyl output: where the music goes (report 059 section 3, decisions D5, D8, D11, D18) and the rows of the output
 * chooser as drawn (Output-480x320):
 *
 *   Automatic   with a sound server: the server's DEFAULT sink, looked up when playback starts and at each track change
 *               (D8: "Automatic" follows the default sink at PLAY / next track, never in the middle of a track); without
 *               one (the Lite boards): ALSA "default";
 *   Bluetooth   "bt:<MAC>": the device's sink (bluez_output.*), opened by name with DONT_MOVE (Cassette's layer); the row
 *               says "music quality" (A2DP) or "call quality" (headset profile); a paired device the launcher says is not
 *               connected is a dimmed row ("paired, not connected", Cassette's T8 facts);
 *   wired       server sinks that are not Bluetooth ("pulse:<sink>": USB cards, HDMI, the jack) or, without a server, the
 *               ALSA cards with a playback device ("alsa:plughw:CARD=<id>,DEV=0": plug converts the rate; never hw:,
 *               decision D5).
 * The engine never switches output by itself: when the output in use goes away, playback pauses (D8). LVGL-free.
 */
#pragma once

#include "../alsa/alsa_api.hpp"
#include "../playback/sink.hpp"
#include "../pulse/launcher_facts.hpp"
#include "../pulse/pulse_api.hpp"

#include <map>
#include <memory>
#include <string>
#include <vector>

namespace vinyl {

struct OutputTarget {
    enum class Kind { Auto, Bt, Pulse, Alsa };
    Kind kind = Kind::Auto;
    std::string id; /* MAC (Bt), sink name (Pulse), ALSA device (Alsa) */
    std::string key() const; /* "auto", "bt:<MAC>", "pulse:<sink>", "alsa:<device>" */
    static OutputTarget from_key(const std::string &k);
    bool operator==(const OutputTarget &o) const { return kind == o.kind && id == o.id; }
    bool operator!=(const OutputTarget &o) const { return !(*this == o); }
};

/* What the chooser knows about the system (the controller fills it from the sound-server worker, the launcher's files
 * and the ALSA card list). */
struct OutputWorld {
    bool server = false;             /* a sound server answered */
    std::string default_sink;        /* its default sink's name */
    std::vector<PulseDevice> sinks;  /* the server's sinks (monitors excluded) */
    std::vector<PulseCard> cards;
    std::vector<CardInfo> alsa_cards; /* without a server */
    LauncherFacts facts;
    std::map<std::string, std::string> bt_names; /* upper-case MAC -> the last name seen */
};

struct OutputRow {
    OutputTarget target;
    std::string label;  /* "Automatic", "Evolve 65", "USB sound card UC02", "HDMI" */
    std::string value;  /* "follows system: Evolve 65", "music quality", "call quality", "wired", "paired, not connected" */
    bool chosen = false;
    bool dimmed = false; /* paired, not connected */
    bool bluetooth = false;
};

std::vector<OutputRow> build_output_rows(const OutputWorld &w, const OutputTarget &chosen);

/* The output a level is remembered for (Vinyl's own volume, per output: board test B7): "bt:<MAC>" for a Bluetooth sink
 * (the same key after a reconnect, whatever the node's suffix), "pulse:<sink>" for another sink of the server, else the
 * chosen target's key ("alsa:<device>", "auto"). */
std::string output_volume_key(const std::string &sink_name, bool pulse, const OutputTarget &target);

/* The system volume of the sink Vinyl plays to as the sound server reports it (board test B9: the chip follows the
 * server's change events, not the launcher's file): the sink `sink`, else (nothing open yet) the server's default sink.
 * false when the server does not report it (no server, the sink is not listed): the caller uses status.state. */
bool sink_volume_of(const OutputWorld &w, const std::string &sink, int &pct, bool &muted);
/* VINYL: the same sink's device (its raw volume, mute, card, base volume), nullptr where sink_volume_of is false. */
const PulseDevice *sink_device_of(const OutputWorld &w, const std::string &sink);
/* The server's placeholder while no real output exists ("auto_null", PipeWire's and PulseAudio's dummy): no level of it
 * is ever shown (the owner's deck log, 2026-11-13: "volume: sink auto_null 100 %" at start). */
bool placeholder_sink(const std::string &name);
/* The quality word of a Bluetooth sink ("music quality", "call quality", "" when unknown). */
std::string bt_quality(const PulseDevice &sink, const std::vector<PulseCard> &cards);
/* The name the screen gives a server sink (its description; "HDMI" for an HDMI one). */
std::string sink_label(const PulseDevice &sink);
/* The Output page's strip: Bluetooth is not available (no server, or the launcher says bt_audio=none). */
bool bluetooth_not_set_up(const OutputWorld &w);

/* -------------------------------------------------------------------------------------------- opening a sink */

struct OpenedSink {
    std::unique_ptr<PlaybackSink> sink;
    std::string name;   /* the server sink or the ALSA device opened */
    std::string label;  /* for the screen and the notice: "Evolve 65" */
    std::string device; /* what "the same device" means after a loss: a MAC, a sink name or an ALSA device */
    bool pulse = false;
};

class SinkProvider {
public:
    virtual ~SinkProvider() = default;
    /* Opens the output for `t` (the sink object only; the engine opens the stream with the track's format). 0 or < 0
     * with `why`. */
    virtual int open(const OutputTarget &t, OpenedSink &out, std::string &why) = 0;
    /* After a loss: the same device can be opened again now (a Bluetooth profile switch re-creates the node). */
    virtual bool still_there(const OpenedSink &o) = 0;
    /* Automatic only: the server's default sink is now another than o.name. */
    virtual bool default_moved(const OpenedSink &o) {
        (void)o;
        return false;
    }
};

/* The production provider: the sound server through `pulse` (may be null), else ALSA through `alsa`. */
std::unique_ptr<SinkProvider> make_real_sink_provider(PulseApi *pulse, AlsaApi &alsa);

}  // namespace vinyl
