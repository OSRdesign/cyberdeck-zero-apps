/*
 * SPDX-License-Identifier: MIT
 *
 * Vinyl output: the chooser's rows and the production sink provider (see outputs.hpp).
 */
#include "outputs.hpp"

#include "../alsa/card.hpp"
#include "../playback/pulse_sink.hpp"

#include <algorithm>
#include <cctype>
#include <mutex>

namespace vinyl {

std::string OutputTarget::key() const {
    switch (kind) {
    case Kind::Bt: return "bt:" + id;
    case Kind::Pulse: return "pulse:" + id;
    case Kind::Alsa: return "alsa:" + id;
    default: return "auto";
    }
}

OutputTarget OutputTarget::from_key(const std::string &k) {
    OutputTarget t;
    auto take = [&](const char *p, Kind kind) {
        const size_t n = std::char_traits<char>::length(p);
        if (k.compare(0, n, p) == 0 && k.size() > n) {
            t.kind = kind;
            t.id = k.substr(n);
            return true;
        }
        return false;
    };
    if (take("bt:", Kind::Bt) || take("pulse:", Kind::Pulse) || take("alsa:", Kind::Alsa)) return t;
    return OutputTarget();
}

namespace {

std::string upper(std::string s) {
    for (char &c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

std::string sink_mac(const PulseDevice &d) {
    if (!d.bt_address.empty()) return upper(d.bt_address);
    return upper(pulse_mac_from_name(d.name));
}

bool is_bt(const PulseDevice &d) {
    return d.bus == "bluetooth" || d.device_api == "bluez5" || d.name.compare(0, 12, "bluez_output") == 0 ||
           !d.bt_address.empty();
}

const PulseCard *card_of(const PulseDevice &d, const std::vector<PulseCard> &cards) {
    for (const PulseCard &c : cards)
        if (c.index == d.card && d.card != kPulseNoIndex) return &c;
    const std::string mac = sink_mac(d);
    if (!mac.empty())
        for (const PulseCard &c : cards)
            if (upper(c.bt_address) == mac || upper(pulse_mac_from_name(c.name)) == mac) return &c;
    return nullptr;
}

}  // namespace

std::string bt_quality(const PulseDevice &sink, const std::vector<PulseCard> &cards) {
    std::string prof = sink.bt_profile;
    if (prof.empty())
        if (const PulseCard *c = card_of(sink, cards)) prof = c->active_profile;
    if (pulse_profile_is_music(prof)) return "music quality";
    if (pulse_profile_is_call(prof)) return "call quality";
    if (sink.rate && sink.rate <= 16000) return "call quality";
    return "";
}

std::string sink_label(const PulseDevice &sink) {
    std::string n = sink.description.empty() ? sink.name : sink.description;
    std::string l = n;
    for (char &c : l) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (l.find("hdmi") != std::string::npos) return "HDMI";
    /* VINYL: the server's profile words are not the device's name ("USB Audio Device Analog Stereo" -> "USB Audio
     * Device"): the chip and the output list keep the card's own name (board screenshot 2026-11-13) */
    for (const char *tail : {" Analog Stereo", " Analog Mono", " Analog Surround", " Digital Stereo", " Digital Surround",
                             " Pro Audio"}) {
        const size_t at = n.rfind(tail);
        if (at != std::string::npos && at > 0) {
            n.erase(at);
            break;
        }
    }
    return n;
}

bool bluetooth_not_set_up(const OutputWorld &w) { return !w.server || w.facts.stack_none(); }

std::vector<OutputRow> build_output_rows(const OutputWorld &w, const OutputTarget &chosen) {
    std::vector<OutputRow> rows;
    OutputRow autorow;
    autorow.target = OutputTarget();
    autorow.label = "Automatic";
    if (w.server) {
        std::string def;
        for (const PulseDevice &s : w.sinks)
            if (s.name == w.default_sink) def = sink_label(s);
        autorow.value = def.empty() ? "follows system" : "follows system: " + def;
    } else {
        autorow.value = "system default";
    }
    rows.push_back(autorow);
    std::vector<std::string> listed_macs;
    if (w.server) {
        /* Bluetooth sinks first, then the wired ones, each in the server's order */
        for (int pass = 0; pass < 2; ++pass) {
            for (const PulseDevice &s : w.sinks) {
                if (s.is_monitor) continue;
                const bool bt = is_bt(s);
                if ((pass == 0) != bt) continue;
                OutputRow r;
                r.bluetooth = bt;
                if (bt) {
                    const std::string mac = sink_mac(s);
                    if (mac.empty()) continue;
                    if (std::find(listed_macs.begin(), listed_macs.end(), mac) != listed_macs.end()) continue;
                    listed_macs.push_back(mac);
                    r.target.kind = OutputTarget::Kind::Bt;
                    r.target.id = mac;
                    r.label = s.description.empty() ? bt_generic_name(mac) : s.description;
                    r.value = bt_quality(s, w.cards);
                    if (r.value.empty()) {
                        const LauncherBtDev *f = w.facts.find(mac);
                        if (f && f->profile == "a2dp") r.value = "music quality";
                        else if (f && f->profile == "hfp") r.value = "call quality";
                    }
                } else {
                    r.target.kind = OutputTarget::Kind::Pulse;
                    r.target.id = s.name;
                    r.label = sink_label(s);
                    r.value = "wired";
                }
                rows.push_back(r);
            }
        }
        /* paired devices the launcher says are not connected: dimmed */
        for (const LauncherBtDev &d : w.facts.devices) {
            if (d.connected) continue;
            if (d.kind != "headset" && d.kind != "headphones" && d.kind != "speaker") continue;
            const std::string mac = upper(d.mac);
            if (std::find(listed_macs.begin(), listed_macs.end(), mac) != listed_macs.end()) continue;
            OutputRow r;
            r.target.kind = OutputTarget::Kind::Bt;
            r.target.id = mac;
            auto it = w.bt_names.find(mac);
            r.label = it != w.bt_names.end() ? it->second : bt_generic_name(mac);
            r.value = "paired, not connected";
            r.dimmed = true;
            r.bluetooth = true;
            rows.push_back(r);
        }
    } else {
        for (const CardInfo &c : w.alsa_cards) {
            if (!c.playback) continue;
            OutputRow r;
            r.target.kind = OutputTarget::Kind::Alsa;
            r.target.id = "plughw:CARD=" + c.id + ",DEV=0";
            std::string l = c.name.empty() ? c.id : c.name;
            std::string low = l;
            for (char &ch : low) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
            r.label = low.find("hdmi") != std::string::npos ? "HDMI" : l;
            r.value = "wired";
            rows.push_back(r);
        }
    }
    bool any = false;
    for (OutputRow &r : rows) {
        r.chosen = r.target == chosen;
        any = any || r.chosen;
    }
    if (!any && chosen.kind != OutputTarget::Kind::Auto) {
        /* a choice made earlier that is not there now: keep it visible, dimmed */
        OutputRow r;
        r.target = chosen;
        r.chosen = true;
        r.dimmed = true;
        r.bluetooth = chosen.kind == OutputTarget::Kind::Bt;
        if (r.bluetooth) {
            auto it = w.bt_names.find(upper(chosen.id));
            r.label = it != w.bt_names.end() ? it->second : bt_generic_name(chosen.id);
        } else {
            r.label = chosen.id;
        }
        r.value = "not connected";
        rows.push_back(r);
    } else if (!any) {
        rows.front().chosen = true;
    }
    return rows;
}

/* -------------------------------------------------------------------------------------------- the real provider */

namespace {

class RealSinkProvider final : public SinkProvider {
public:
    RealSinkProvider(PulseApi *pulse, AlsaApi &alsa) : pulse_(pulse), alsa_(alsa) {}

    int open(const OutputTarget &t, OpenedSink &out, std::string &why) override {
        out = OpenedSink();
        if (t.kind == OutputTarget::Kind::Alsa) return open_alsa(t.id, t.id, out);
        const bool server = server_up();
        if (t.kind == OutputTarget::Kind::Auto && !server) return open_alsa("default", "Default output", out);
        if (!server) {
            why = "no sound server";
            return -ENODEV;
        }
        std::vector<PulseDevice> sinks;
        if (pulse_->list_sinks(sinks) != 0) {
            why = "the sound server did not list its outputs";
            return -EIO;
        }
        const PulseDevice *found = nullptr;
        if (t.kind == OutputTarget::Kind::Auto) {
            PulseServerInfo si;
            if (pulse_->server_info(si) != 0 || si.default_sink.empty()) {
                why = "the sound server has no default output";
                return -ENODEV;
            }
            for (const PulseDevice &s : sinks)
                if (s.name == si.default_sink) found = &s;
        } else if (t.kind == OutputTarget::Kind::Bt) {
            for (const PulseDevice &s : sinks)
                if (!s.is_monitor && sink_mac(s) == upper(t.id)) found = &s;
        } else {
            for (const PulseDevice &s : sinks)
                if (s.name == t.id) found = &s;
        }
        if (!found) {
            why = "not connected";
            return -ENODEV;
        }
        PulseSinkOptions opt;
        opt.buffer_ms = 200; /* music: a larger server target than Cassette's 60 ms (report 059, 6.5) */
        opt.ring_ms = 250;
        opt.period_ms = 40;
        opt.ready_timeout_ms = 1500;
        opt.stream_name = "Vinyl";
        opt.role = "music";
        auto ps = std::make_unique<PulsePlaybackSink>(*pulse_, opt);
        ps->set_sink(found->name);
        out.sink = std::move(ps);
        out.name = found->name;
        out.label = is_bt(*found) ? (found->description.empty() ? bt_generic_name(sink_mac(*found)) : found->description)
                                  : sink_label(*found);
        out.device = is_bt(*found) ? sink_mac(*found) : found->name;
        out.pulse = true;
        return 0;
    }

    bool still_there(const OpenedSink &o) override {
        if (!o.pulse) {
            if (o.name == "default") return true;
            /* a card: present when ALSA lists it again */
            for (const CardInfo &c : alsa_.cards())
                if (o.name == "plughw:CARD=" + c.id + ",DEV=0") return true;
            return false;
        }
        if (!server_up()) return false;
        std::vector<PulseDevice> sinks;
        if (pulse_->list_sinks(sinks) != 0) return false;
        for (const PulseDevice &s : sinks) {
            if (s.is_monitor) continue;
            if (s.name == o.device || (!o.device.empty() && sink_mac(s) == o.device)) return true;
        }
        return false;
    }

    bool default_moved(const OpenedSink &o) override {
        if (!o.pulse || !server_up()) return false;
        PulseServerInfo si;
        if (pulse_->server_info(si) != 0 || si.default_sink.empty()) return false;
        return si.default_sink != o.name;
    }

private:
    bool server_up() {
        if (!pulse_) return false;
        std::lock_guard<std::mutex> g(mu_);
        if (pulse_->connected()) return true;
        return pulse_->connect("Vinyl", 1500) == 0;
    }

    int open_alsa(const std::string &device, const std::string &label, OpenedSink &out) {
        std::string why;
        if (!alsa_.available(&why)) return kNoAlsa;
        out.sink = std::make_unique<AlsaPlaybackSink>(alsa_, device);
        out.name = device;
        out.device = device;
        out.label = label;
        if (device != "default") {
            for (const CardInfo &c : alsa_.cards())
                if (device == "plughw:CARD=" + c.id + ",DEV=0") out.label = c.name.empty() ? c.id : c.name;
        }
        out.pulse = false;
        return 0;
    }

    PulseApi *pulse_;
    AlsaApi &alsa_;
    std::mutex mu_;
};

}  // namespace

std::unique_ptr<SinkProvider> make_real_sink_provider(PulseApi *pulse, AlsaApi &alsa) {
    return std::make_unique<RealSinkProvider>(pulse, alsa);
}

std::string output_volume_key(const std::string &sink_name, bool pulse, const OutputTarget &target) {
    if (pulse && !sink_name.empty()) {
        std::string mac = pulse_mac_from_name(sink_name);
        if (!mac.empty() && sink_name.compare(0, 6, "bluez_") == 0) {
            for (char &c : mac) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
            return "bt:" + mac;
        }
        return "pulse:" + sink_name;
    }
    return target.key();
}

bool placeholder_sink(const std::string &name) { return name.compare(0, 9, "auto_null") == 0; }

const PulseDevice *sink_device_of(const OutputWorld &w, const std::string &sink) {
    if (!w.server) return nullptr;
    const std::string &name = sink.empty() ? w.default_sink : sink;
    if (name.empty() || placeholder_sink(name)) return nullptr;
    for (const PulseDevice &d : w.sinks)
        if (d.name == name && (d.volume_raw >= 0 || d.volume_pct >= 0)) return &d;
    return nullptr;
}

bool sink_volume_of(const OutputWorld &w, const std::string &sink, int &pct, bool &muted) {
    if (!w.server) return false;
    const std::string &name = sink.empty() ? w.default_sink : sink;
    if (name.empty() || placeholder_sink(name)) return false;
    for (const PulseDevice &d : w.sinks)
        if (d.name == name && d.volume_pct >= 0) {
            pct = d.volume_pct;
            muted = d.muted;
            return true;
        }
    return false;
}

}  // namespace vinyl
