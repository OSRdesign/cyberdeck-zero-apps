/*
 * SPDX-License-Identifier: MIT
 *
 * Copied from Cassette (apps/cassette at origin/main d1cf53f) for Vinyl, report 059 decision D6; Vinyl changes are marked VINYL.
 */
#include "pulse_probe.hpp"

#include <algorithm>
#include <cctype>
#include <map>

namespace vinyl {

const char *pulse_kind_name(PulseKind k) {
    switch (k) {
    case PulseKind::Other: return "other";
    case PulseKind::Headset: return "headset";
    case PulseKind::Headphones: return "headphones";
    case PulseKind::Speaker: return "speaker";
    }
    return "?";
}

namespace {

bool starts_with(const std::string &s, const char *p) {
    const std::string q(p);
    return s.compare(0, q.size(), q) == 0;
}

std::string upper(std::string s) {
    for (char &c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

const PulseCard *card_by_index(const std::vector<PulseCard> &cards, uint32_t index) {
    if (index == kPulseNoIndex) return nullptr;
    for (const PulseCard &c : cards)
        if (c.index == index) return &c;
    return nullptr;
}

bool card_is_bt(const PulseCard &c) {
    return starts_with(c.name, "bluez_card") || c.bus == "bluetooth" || c.device_api == "bluez5";
}

bool device_is_bt(const PulseDevice &d, const PulseCard *card) {
    return starts_with(d.name, "bluez_") || (card && card_is_bt(*card)) || d.bus == "bluetooth" || d.device_api == "bluez5";
}

bool headphone_form(const std::string &f) {
    return f == "headphone" || f == "headset" || f == "hands-free" || f == "handsfree" || f == "earbuds" ||
           f == "headphones";
}

}  // namespace

PulseKind PulseProbe::kind_of(const std::string &mac) const {
    const std::string m = upper(mac);
    for (const PulseProbeDevice &d : devices)
        if (d.bluetooth && upper(d.mac) == m && !m.empty()) return d.kind;
    return PulseKind::Other;
}

const PulseProbeCard *PulseProbe::card_of(const std::string &mac) const {
    const std::string m = upper(mac);
    for (const PulseProbeCard &c : cards)
        if (!m.empty() && upper(c.mac) == m) return &c;
    return nullptr;
}

PulseProbe pulse_probe(const std::vector<PulseDevice> &sources, const std::vector<PulseDevice> &sinks,
                       const std::vector<PulseCard> &cards, const PulseServerInfo *server) {
    PulseProbe p;
    if (server) {
        p.default_source = server->default_source;
        p.default_sink = server->default_sink;
    }
    /* the Bluetooth cards and their profiles */
    for (const PulseCard &c : cards) {
        if (!card_is_bt(c)) continue;
        PulseProbeCard pc;
        pc.name = c.name;
        pc.description = c.description;
        pc.mac = !c.bt_address.empty() ? c.bt_address : pulse_mac_from_name(c.name);
        pc.bluetooth = true;
        for (const PulseProfile &pr : c.profiles) {
            if (pulse_profile_is_music(pr.name)) {
                pc.music_offered = true;
                if (pr.available) {
                    pc.music_available = true;
                    pc.music_profiles.push_back(pr.name);
                }
            } else if (pulse_profile_is_call(pr.name)) {
                pc.call_offered = true;
                if (pr.available) pc.call_profiles.push_back(pr.name);
            }
        }
        pc.active_profile = c.active_profile;
        pc.active_is_music = pulse_profile_is_music(c.active_profile);
        pc.active_is_call = pulse_profile_is_call(c.active_profile);
        p.cards.push_back(pc);
    }
    auto add = [&](const PulseDevice &d, bool source) {
        PulseProbeDevice v;
        const PulseCard *card = card_by_index(cards, d.card);
        v.name = d.name;
        v.description = !d.description.empty() ? d.description : d.device_description;
        v.source = source;
        v.bluetooth = device_is_bt(d, card);
        if (v.bluetooth) {
            v.mac = d.bt_address;
            if (v.mac.empty()) v.mac = pulse_mac_from_name(d.name);
            if (v.mac.empty() && card) v.mac = !card->bt_address.empty() ? card->bt_address : pulse_mac_from_name(card->name);
        }
        v.rate = d.rate;
        v.channels = d.channels;
        v.monitor = source && d.is_monitor;
        v.card = card ? card->name : std::string();
        v.profile = d.bt_profile;
        v.codec = d.bt_codec;
        v.is_default = !d.name.empty() && d.name == (source ? p.default_source : p.default_sink);
        p.devices.push_back(v);
    };
    for (const PulseDevice &d : sources) add(d, true);
    for (const PulseDevice &d : sinks) add(d, false);

    /* the kind, per Bluetooth device (by MAC): a microphone source or a call profile makes it a headset */
    std::map<std::string, PulseKind> kinds;
    std::map<std::string, std::string> forms;
    for (const PulseDevice &d : sinks)
        if (!d.form_factor.empty()) forms[upper(!d.bt_address.empty() ? d.bt_address : pulse_mac_from_name(d.name))] = d.form_factor;
    for (const PulseCard &c : cards)
        if (!c.form_factor.empty() && card_is_bt(c))
            forms.emplace(upper(!c.bt_address.empty() ? c.bt_address : pulse_mac_from_name(c.name)), c.form_factor);
    for (const PulseProbeDevice &v : p.devices) {
        if (!v.bluetooth || v.mac.empty()) continue;
        const std::string m = upper(v.mac);
        if (v.source && !v.monitor) kinds[m] = PulseKind::Headset;
    }
    for (const PulseProbeCard &c : p.cards)
        if (c.call_offered && !c.mac.empty()) kinds[upper(c.mac)] = PulseKind::Headset;
    for (PulseProbeDevice &v : p.devices) {
        if (!v.bluetooth) continue;
        if (v.mac.empty()) { /* no address anywhere: judge the node alone */
            v.kind = v.source && !v.monitor ? PulseKind::Headset : PulseKind::Speaker;
            continue;
        }
        const std::string m = upper(v.mac);
        auto k = kinds.find(m);
        if (k != kinds.end()) {
            v.kind = k->second;
            continue;
        }
        auto f = forms.find(m);
        v.kind = f != forms.end() && headphone_form(f->second) ? PulseKind::Headphones : PulseKind::Speaker;
    }
    return p;
}

}  // namespace vinyl
