/*
 * SPDX-License-Identifier: MIT
 *
 * Vinyl volume: the level of the output Vinyl plays on (see output_volume.hpp).
 */
#include "output_volume.hpp"

#include <algorithm>
#include <cmath>

namespace vinyl {

namespace {

bool ends_with(const std::string &s, const std::string &tail) {
    return s.size() >= tail.size() && s.compare(s.size() - tail.size(), tail.size(), tail) == 0;
}

const CtlInfo *find(const std::vector<CtlInfo> &ctls, const std::string &name) {
    for (const CtlInfo &c : ctls)
        if (c.name == name) return &c;
    return nullptr;
}

long round_div(long long num, long long den) { return static_cast<long>(std::llround(static_cast<double>(num) / static_cast<double>(den))); }

}  // namespace

bool choose_playback_mixer(const std::vector<CtlInfo> &ctls, MixerChoice &out) {
    out = MixerChoice();
    auto usable = [](const CtlInfo *c) {
        return c && c->type == CtlInfo::Type::Integer && !c->capture && c->readable && c->writable && c->max > c->min;
    };
    const CtlInfo *pick = nullptr;
    std::string prefix;
    for (const char *p : {"Master", "PCM", "Speaker", "Headphone", "Headset"}) {
        const CtlInfo *c = find(ctls, std::string(p) + " Playback Volume");
        if (usable(c)) {
            pick = c;
            prefix = p;
            break;
        }
    }
    if (!pick)
        for (const CtlInfo &c : ctls)
            if (usable(&c) && ends_with(c.name, " Playback Volume")) {
                pick = &c;
                prefix = c.name.substr(0, c.name.size() - std::string(" Playback Volume").size());
                break;
            }
    if (!pick) return false;
    out.volume = pick->name;
    out.min = pick->min;
    out.max = pick->max;
    out.top = pick->max;
    if (const CtlInfo *sw = find(ctls, prefix + " Playback Switch"); sw && sw->type == CtlInfo::Type::Boolean && sw->writable)
        out.sw = sw->name;
    if (pick->has_db && pick->db_max_cdb > pick->db_min_cdb) {
        out.has_db = true;
        out.db_min = pick->db_min_cdb;
        out.db_max = pick->db_max_cdb;
        out.db_top = std::min(out.db_max, 0L);
        if (out.db_top < out.db_max) /* never above 0 dB: the step at 0 dB (steps linear in dB, the usual dB scale) */
            out.top = out.min + round_div(static_cast<long long>(out.db_top - out.db_min) * (out.max - out.min), out.db_max - out.db_min);
        if (out.db_top <= out.db_min) out.top = out.min; /* a control entirely above 0 dB: nothing to give */
    }
    return out.top > out.min || out.ok();
}

long volume_cdb_for(int pct) {
    if (pct <= 0) return -1000000; /* minus infinity: the control's minimum */
    return static_cast<long>(std::lround(6000.0 * std::log10(std::min(pct, 100) / 100.0)));
}

int volume_pct_for(long cdb) { return static_cast<int>(std::lround(100.0 * std::pow(10.0, cdb / 6000.0))); }

long mixer_raw_for(const MixerChoice &m, int pct) {
    pct = std::max(0, std::min(100, pct));
    if (m.top <= m.min) return m.min;
    if (m.has_db) {
        /* the server's curve, clamped to the control's range (never above db_top: 0 dB or the control's max) */
        if (pct == 0) return m.min;
        const long cdb = std::max(m.db_min, std::min(m.db_top, volume_cdb_for(pct)));
        const long raw = m.min + round_div(static_cast<long long>(cdb - m.db_min) * (m.max - m.min), m.db_max - m.db_min);
        return std::max(m.min, std::min(m.top, raw));
    }
    return m.min + round_div(static_cast<long long>(m.top - m.min) * pct, 100);
}

int mixer_pct_for(const MixerChoice &m, long raw) {
    if (m.top <= m.min) return raw > m.min ? 100 : 0;
    if (m.has_db) return std::max(0, volume_pct_for(mixer_cdb_for(m, raw)));
    return static_cast<int>(std::max(0L, round_div(static_cast<long long>(raw - m.min) * 100, m.top - m.min)));
}

int mixer_step_target(const MixerChoice &m, int current_pct, int delta) {
    const int to = volume_after_step(current_pct, delta);
    if (delta < 0 && to > 0 && mixer_raw_for(m, to) >= mixer_raw_for(m, std::max(0, current_pct))) return 0;
    return to;
}

long mixer_cdb_for(const MixerChoice &m, long raw) {
    if (!m.has_db || m.max <= m.min) return 0;
    return m.db_min + round_div(static_cast<long long>(raw - m.min) * (m.db_max - m.db_min), m.max - m.min);
}

std::string mixer_card_for(const std::string &dev) {
    const std::string key = "CARD=";
    const size_t at = dev.find(key);
    if (dev.compare(0, 2, "hw") != 0 && dev.compare(0, 6, "plughw") != 0) return "default";
    if (at == std::string::npos) return "default";
    const size_t start = at + key.size();
    const size_t end = dev.find(',', start);
    const std::string id = dev.substr(start, end == std::string::npos ? std::string::npos : end - start);
    return id.empty() ? "default" : "hw:CARD=" + id;
}

int read_mixer(AlsaApi &alsa, const std::string &card, std::string &choice_card, MixerChoice &choice, MixerLevel &out) {
    out = MixerLevel();
    if (choice_card != card || !choice.ok()) {
        std::vector<CtlInfo> ctls;
        const int r = alsa.mixer_list(card, ctls);
        choice_card = card;
        choice = MixerChoice();
        if (r < 0) {
            choice_card.clear(); /* look again next time (a card that comes back) */
            return r;
        }
        if (!choose_playback_mixer(ctls, choice)) return 0; /* no playback mixer: software volume */
    }
    long raw = 0;
    const int r = alsa.mixer_get(card, choice.volume, raw);
    if (r < 0) {
        choice_card.clear();
        return r;
    }
    out.has_mixer = true;
    out.pct = mixer_pct_for(choice, raw);
    out.cdb = mixer_cdb_for(choice, raw);
    if (!choice.sw.empty()) {
        long on = 1;
        if (alsa.mixer_get(card, choice.sw, on) >= 0) out.muted = on == 0;
    }
    return 0;
}

int write_mixer(AlsaApi &alsa, const std::string &card, const MixerChoice &choice, int pct) {
    if (!choice.ok()) return -ENOENT;
    const int r = alsa.mixer_set(card, choice.volume, mixer_raw_for(choice, pct));
    if (r < 0) return r;
    if (!choice.sw.empty()) alsa.mixer_set(card, choice.sw, pct > 0 ? 1 : 0);
    return 0;
}

std::string volume_chip_text(int level, bool muted) {
    if (level < 0) return "Vol --";
    if (muted) return "Muted";
    return "Vol " + std::to_string(level);
}

std::string volume_log_text(int level) { return level < 0 ? std::string("-- (not known)") : std::to_string(level) + " %"; }

int volume_after_step(int current, int delta) {
    if (current < 0) current = 0;
    const int to = current + delta;
    if (delta > 0) return std::min(100, std::max(to, 0)) < current ? current : std::min(100, to);
    return std::max(0, to);
}

}  // namespace vinyl
