/*
 * SPDX-License-Identifier: MIT
 *
 * Copied from Cassette (apps/cassette at origin/main d1cf53f) for Vinyl, report 059 decision D6; Vinyl changes are marked VINYL.
 */
#include "bt_output.hpp"

#include "bt_input.hpp"

#include <cctype>
#include <map>

namespace vinyl {

namespace {
std::string upper(std::string s) {
    for (char &c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}
}  // namespace

std::vector<BtOutput> bt_outputs(const PulseProbe &probe) {
    std::vector<std::string> order;
    std::map<std::string, const PulseProbeDevice *> sinks;
    for (const PulseProbeDevice &d : probe.devices) {
        if (!d.bluetooth || d.source || d.monitor || d.mac.empty()) continue;
        const std::string m = upper(d.mac);
        if (sinks.count(m)) continue;
        sinks[m] = &d;
        order.push_back(m);
    }
    for (const PulseProbeCard &c : probe.cards) { /* a card with an audio profile but no sink now (profile off) */
        if (c.mac.empty() || (!c.music_offered && !c.call_offered)) continue;
        const std::string m = upper(c.mac);
        if (!sinks.count(m)) {
            sinks[m] = nullptr;
            order.push_back(m);
        }
    }
    std::vector<BtOutput> out;
    for (const std::string &m : order) {
        BtOutput b;
        b.mac = m;
        b.id = "bt:" + m;
        b.connected = true;
        const PulseProbeDevice *d = sinks[m];
        const PulseProbeCard *pc = probe.card_of(m);
        if (pc) {
            b.card = pc->name;
            b.music_offered = pc->music_offered;
            b.music_available = pc->music_available;
            b.call_offered = pc->call_offered;
            if (pc->active_is_music) b.hint_profile = "music";
            else if (pc->active_is_call) b.hint_profile = "call";
        }
        if (d) {
            b.sink = d->name;
            b.usable = true;
            b.rate = d->rate;
            b.channels = d->channels;
            if (b.card.empty()) b.card = d->card;
            if (pulse_profile_is_music(d->profile)) b.sink_profile = "music";
            else if (pulse_profile_is_call(d->profile)) b.sink_profile = "call";
            if (b.hint_profile == "unknown" && !d->profile.empty()) {
                if (pulse_profile_is_music(d->profile)) b.hint_profile = "music";
                else if (pulse_profile_is_call(d->profile)) b.hint_profile = "call";
            }
        }
        if (d && !d->description.empty()) b.label = d->description;
        else if (pc && !pc->description.empty()) b.label = pc->description;
        else b.label = "Bluetooth device";
        b.kind = pulse_kind_name(probe.kind_of(m));
        if (b.kind == "other" && pc && pc->call_offered) b.kind = "headset";
        if (!b.usable) b.reason = "The device has no audio output now";
        out.push_back(b);
    }
    return out;
}

const BtOutput *bt_output_find(const std::vector<BtOutput> &list, const std::string &mac) {
    const std::string m = upper(mac);
    for (const BtOutput &b : list)
        if (b.mac == m) return &b;
    return nullptr;
}

}  // namespace vinyl
