// SPDX-License-Identifier: MIT
#include "pulse_report.hpp"

#include <cctype>
#include <cstdarg>
#include <cstdio>

namespace cassette {

namespace {

void add(std::string &out, const char *fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    out += buf;
}

std::string upper(std::string s) {
    for (char &c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

bool wanted(const std::string &mac, const std::string &filter) {
    return filter.empty() || upper(mac) == upper(filter);
}

}  // namespace

std::string pulse_probe_summary(const PulseProbe &probe, const std::string &mac_filter) {
    std::string out = "summary:\n";
    for (const PulseProbeDevice &d : probe.devices) {
        if (!wanted(d.mac, mac_filter)) continue;
        add(out, "  %-6s %-50s bt=%s mac=%-17s kind=%-10s %u Hz%s%s\n", d.source ? "source" : "sink", d.name.c_str(),
            d.bluetooth ? "yes" : "no", d.mac.c_str(), pulse_kind_name(d.kind), d.rate, d.monitor ? " monitor" : "",
            d.is_default ? " (default)" : "");
    }
    for (const PulseProbeCard &k : probe.cards) {
        if (!wanted(k.mac, mac_filter)) continue;
        add(out, "  card %s: music %s, call %s, active %s (%s)\n", k.name.c_str(),
            k.music_available ? "available" : (k.music_offered ? "OFFERED BUT UNAVAILABLE" : "not offered"),
            k.call_offered ? "offered" : "not offered", k.active_profile.c_str(),
            k.active_is_music ? "music" : (k.active_is_call ? "call" : "other"));
    }
    return out;
}

int pulse_server_report(PulseApi &api, const std::string &mac_filter, std::string &out) {
    const int c = api.connect("Cassette probe", 3000);
    if (c) {
        add(out, "connect: %s (%s)\n", pulse_error_text(c).c_str(), api.last_error().c_str());
        out += "Bluetooth audio is not set up on this board (USB cards work as before)\n";
        return 1;
    }
    PulseServerInfo si;
    std::vector<PulseDevice> src, snk;
    std::vector<PulseCard> cards;
    int r = api.server_info(si);
    if (!r) r = api.list_sources(src);
    if (!r) r = api.list_sinks(snk);
    if (!r) r = api.list_cards(cards);
    if (r) {
        add(out, "listing failed: %s\n", pulse_error_text(r).c_str());
        api.disconnect();
        return 1;
    }
    api.disconnect();
    add(out, "server: %s, version %s, libpulse %s, default rate %u\n", si.server_name.c_str(), si.server_version.c_str(),
        si.library_version.c_str(), si.rate);
    add(out, "default source: %s\ndefault sink:   %s\n", si.default_source.c_str(), si.default_sink.c_str());
    const PulseProbe probe = pulse_probe(src, snk, cards, &si);
    out += pulse_probe_summary(probe, mac_filter);
    if (mac_filter.empty()) return 0;
    for (const PulseProbeDevice &d : probe.devices)
        if (upper(d.mac) == upper(mac_filter)) return 0;
    for (const PulseProbeCard &k : probe.cards)
        if (upper(k.mac) == upper(mac_filter)) return 0;
    add(out, "no device bt:%s on the sound server (paired but not connected? connect it in Settings > Bluetooth)\n",
        upper(mac_filter).c_str());
    return 1;
}

}  // namespace cassette
