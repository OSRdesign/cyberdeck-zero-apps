/*
 * SPDX-License-Identifier: MIT
 *
 * Copied from Cassette (apps/cassette at origin/main d1cf53f) for Vinyl, report 059 decision D6; Vinyl changes are marked VINYL.
 */
#include "bt_input.hpp"

#include <algorithm>
#include <cctype>
#include <map>

namespace vinyl {

namespace {

std::string upper(std::string s) {
    for (char &c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

bool has(const std::string &s, const char *sub) { return s.find(sub) != std::string::npos; }

}  // namespace

std::string bt_id_of(const std::string &mac) {
    const std::string m = pulse_parse_mac(mac);
    return m.empty() ? std::string() : "bt:" + upper(m);
}

std::string bt_mac_of_id(const std::string &id) {
    if (id.compare(0, 3, "bt:") != 0) return "";
    const std::string m = pulse_parse_mac(id.substr(3));
    return upper(m);
}

bool bt_source_is_virtual(const std::string &name) { return has(name, ":"); }

std::vector<BtInput> bt_inputs(const PulseProbe &probe) {
    std::vector<std::string> order; /* MACs in the order first seen */
    std::map<std::string, std::vector<const PulseProbeDevice *>> srcs;
    for (const PulseProbeDevice &d : probe.devices) {
        if (!d.bluetooth || !d.source || d.monitor || d.mac.empty()) continue;
        const std::string m = upper(d.mac);
        if (!srcs.count(m)) order.push_back(m);
        srcs[m].push_back(&d);
    }
    for (const PulseProbeCard &c : probe.cards) { /* a card with a microphone profile but no source now (profile off) */
        if (!c.call_offered || c.mac.empty()) continue;
        const std::string m = upper(c.mac);
        if (!srcs.count(m)) {
            order.push_back(m);
            srcs[m];
        }
    }
    std::vector<BtInput> out;
    for (const std::string &m : order) {
        BtInput b;
        b.mac = m;
        b.id = "bt:" + m;
        b.connected = true;
        const PulseProbeCard *pc = probe.card_of(m);
        const PulseProbeDevice *real = nullptr, *virt = nullptr;
        for (const PulseProbeDevice *d : srcs[m]) {
            if (bt_source_is_virtual(d->name)) {
                if (!virt) virt = d;
            } else if (!real) {
                real = d;
            }
        }
        if (pc) b.card = pc->name;
        if (pc && pc->active_is_music) b.hint_profile = "music";
        else if (pc && pc->active_is_call) b.hint_profile = "call";
        const PulseProbeDevice *use = nullptr;
        if (pc && pc->active_is_call && real) use = real;
        else if (virt) use = virt;
        else if (real) use = real;
        if (use) {
            b.source = use->name;
            b.virtual_source = use == virt;
            b.usable = true;
            if (b.card.empty()) b.card = use->card;
        }
        /* the label: the alias the server shows */
        if (use && !use->description.empty()) b.label = use->description;
        else if (pc && !pc->description.empty()) b.label = pc->description;
        else if (!srcs[m].empty() && !srcs[m].front()->description.empty()) b.label = srcs[m].front()->description;
        else b.label = "Bluetooth device";
        b.kind = pulse_kind_name(probe.kind_of(m));
        if (b.kind == "other" && pc && pc->call_offered) b.kind = "headset";
        /* the call rate */
        if (real && (real->rate == kBtRateNarrow || real->rate == kBtRateWide)) {
            b.rate = real->rate;
        } else {
            bool wide = false, narrow = false;
            if (pc) {
                for (const std::string &n : pc->call_profiles) {
                    if (has(n, "cvsd")) narrow = true;
                    else wide = true;
                }
                if (pc->active_is_call) {
                    if (has(pc->active_profile, "cvsd")) narrow = true, wide = false;
                    else wide = true;
                }
            }
            b.rate = (wide || !narrow) ? kBtRateWide : kBtRateNarrow;
        }
        if (!b.usable) b.reason = "The headset microphone is off";
        out.push_back(b);
    }
    return out;
}

}  // namespace vinyl
