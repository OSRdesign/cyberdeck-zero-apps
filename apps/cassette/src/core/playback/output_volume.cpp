/*
 * SPDX-License-Identifier: MIT
 *
 * Cassette playback VOLUME: the level of the output a take plays on, in steps fitted to it (see output_volume.hpp; copied
 * from Vinyl's src/core/volume/output_volume.cpp without the fine steps).
 */
#include "output_volume.hpp"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdio>

namespace cassette {

namespace {

bool ends_with(const std::string &s, const std::string &tail) {
    return s.size() >= tail.size() && s.compare(s.size() - tail.size(), tail.size(), tail) == 0;
}

bool starts_with(const std::string &s, const std::string &head) { return s.compare(0, head.size(), head) == 0; }

const CtlInfo *find(const std::vector<CtlInfo> &ctls, const std::string &name) {
    for (const CtlInfo &c : ctls)
        if (c.name == name) return &c;
    return nullptr;
}

long round_div(long long num, long long den) {
    return static_cast<long>(std::llround(static_cast<double>(num) / static_cast<double>(den)));
}

/* levels of a control without dB data: its raw steps, at most 30 */
int raw_levels(long span) { return static_cast<int>(std::max(1L, std::min(span, 30L))); }

/* an input's loop-back into the output (the PCM2902's "Mic Playback Volume"): not the output's level */
bool loopback_control(const std::string &name) {
    for (const char *p : {"Mic", "Line", "Sidetone", "Loopback", "Monitor", "Aux", "CD ", "Capture"})
        if (starts_with(name, p)) return true;
    return false;
}

}  // namespace

/* ------------------------------------------------------------------------------------------------ the model */

double OutputVolumeModel::db_of(int level) const {
    if (n <= 1) return hi_db;
    const int i = std::max(1, std::min(n, level));
    return lo_db + (i - 1) * (hi_db - lo_db) / (n - 1);
}

int OutputVolumeModel::pct_of(int level) const {
    if (level <= 0 || n <= 0) return 0;
    return static_cast<int>(std::lround(100.0 * std::min(level, n) / n));
}

int OutputVolumeModel::nearest(double db) const {
    if (n <= 1 || hi_db <= lo_db) return std::max(1, n);
    const long i = 1 + std::lround((db - lo_db) * (n - 1) / (hi_db - lo_db));
    return static_cast<int>(std::max(1L, std::min(static_cast<long>(n), i)));
}

int OutputVolumeModel::from_pct(int pct) const {
    if (pct <= 0) return 0;
    return std::max(1, std::min(n, static_cast<int>(std::lround(std::min(pct, 100) * n / 100.0))));
}

OutputVolumeModel volume_model(double lo_db, double hi_db, double native_step_db) {
    OutputVolumeModel m;
    m.hi_db = hi_db;
    m.lo_db = std::max(lo_db, hi_db - kVolumeWindowDb);
    const double w = m.hi_db - m.lo_db;
    if (w <= 0) { /* a range of one value */
        m.lo_db = m.hi_db;
        m.n = 1;
        return m;
    }
    int n = std::max(2, static_cast<int>(std::ceil(w / kVolumeStepDb - 1e-9)));
    const double min_step = std::max(kVolumeMinStepDb, native_step_db);
    if (w / (n - 1) < min_step - 1e-9) n = std::max(2, static_cast<int>(std::floor(w / min_step + 1e-9)) + 1);
    m.n = n;
    return m;
}

OutputVolumeModel default_volume_model() { return volume_model(-kVolumeWindowDb, 0, 0); }

/* ------------------------------------------------------------------------------------------------ ALSA */

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
            if (usable(&c) && ends_with(c.name, " Playback Volume") && !loopback_control(c.name)) {
                pick = &c;
                prefix = c.name.substr(0, c.name.size() - std::string(" Playback Volume").size());
                break;
            }
    if (!pick) return false;
    CtlInfo c = *pick;
    alsa_fix_mute_db(c);
    out.volume = c.name;
    out.min = c.min;
    out.max = c.max;
    if (const CtlInfo *sw = find(ctls, prefix + " Playback Switch"); sw && sw->type == CtlInfo::Type::Boolean && sw->writable)
        out.sw = sw->name;
    out.min_is_mute = c.min_is_mute;
    if (c.has_db && c.db_max_cdb > c.db_min_cdb && c.db_min_cdb > kAlsaDbGainMute) {
        out.has_db = true;
        out.db_min = c.db_min_cdb;
        out.db_max = c.db_max_cdb;
        const double native = (out.db_max - out.db_min) / 100.0 / static_cast<double>(out.max - out.min);
        const double lo = out.db_min / 100.0 + (out.min_is_mute ? native : 0.0); /* the lowest value that sounds */
        out.model = volume_model(lo, out.db_max / 100.0, native);
    } else {
        out.model.db = false;
        out.model.n = raw_levels(out.max - out.min - (out.min_is_mute ? 1 : 0));
        out.model.lo_db = out.model.hi_db = 0;
    }
    return true;
}

long mixer_cdb_for(const MixerChoice &m, long raw) {
    if (!m.has_db || m.max <= m.min) return 0;
    return m.db_min + round_div(static_cast<long long>(raw - m.min) * (m.db_max - m.db_min), m.max - m.min);
}

long mixer_raw_for_level(const MixerChoice &m, int level) {
    if (!m.ok() || level <= 0) return m.min;
    const long lowest = m.min_is_mute ? m.min + 1 : m.min;
    long raw;
    if (m.has_db) {
        const double cdb = m.model.db_of(level) * 100.0;
        raw = m.min + std::lround((cdb - m.db_min) * (m.max - m.min) / static_cast<double>(m.db_max - m.db_min));
    } else {
        raw = lowest + round_div(static_cast<long long>(m.max - lowest) * std::min(level, m.model.n), m.model.n);
    }
    return std::max(lowest, std::min(m.max, raw));
}

int mixer_level_for_raw(const MixerChoice &m, long raw) {
    if (!m.ok()) return -1;
    if (m.min_is_mute && raw <= m.min) return 0;
    if (m.has_db) return m.model.nearest(mixer_cdb_for(m, raw) / 100.0);
    const long lowest = m.min_is_mute ? m.min + 1 : m.min;
    const long i = round_div(static_cast<long long>(std::max(0L, raw - lowest)) * m.model.n, std::max(1L, m.max - lowest));
    return static_cast<int>(std::max(1L, std::min(static_cast<long>(m.model.n), i)));
}

std::string mixer_card_for(const std::string &dev) {
    const std::string key = "CARD=";
    if (dev.compare(0, 2, "hw") != 0 && dev.compare(0, 6, "plughw") != 0) return "default";
    const size_t at = dev.find(key);
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
    out.raw = raw;
    out.cdb = mixer_cdb_for(choice, raw);
    if (!choice.sw.empty()) {
        long on = 1;
        if (alsa.mixer_get(card, choice.sw, on) >= 0) out.muted = on == 0;
    }
    out.level = mixer_level_for_raw(choice, raw);
    if (out.level == 0) out.muted = true; /* the value that is mute (the Pi jack's PCM) */
    out.pct = choice.model.pct_of(out.level);
    return 0;
}

int write_mixer_level(AlsaApi &alsa, const std::string &card, const MixerChoice &choice, int level) {
    if (!choice.ok()) return -ENOENT;
    const int r = alsa.mixer_set(card, choice.volume, mixer_raw_for_level(choice, level));
    if (r < 0) return r;
    if (!choice.sw.empty()) alsa.mixer_set(card, choice.sw, level > 0 ? 1 : 0);
    return 0;
}

int write_mixer_switch(AlsaApi &alsa, const std::string &card, const MixerChoice &choice, bool on) {
    if (choice.sw.empty()) return -ENOENT;
    return alsa.mixer_set(card, choice.sw, on ? 1 : 0);
}

/* ------------------------------------------------------------------------------------------------ sinks */

uint32_t sink_volume_for_db(double db) {
    if (db <= -200) return 0;
    const double v = 65536.0 * std::pow(10.0, db / 60.0);
    return static_cast<uint32_t>(std::lround(std::min(v, 4294967295.0)));
}

double sink_db_for_volume(uint32_t volume) {
    if (volume == 0) return -INFINITY;
    return 60.0 * std::log10(volume / 65536.0);
}

OutputVolumeModel sink_volume_model(const MixerChoice *card_mixer) {
    if (!card_mixer || !card_mixer->ok() || !card_mixer->has_db) return default_volume_model();
    /* the hardware's top is the sink's 0 dB: the same levels, moved */
    const OutputVolumeModel &c = card_mixer->model;
    OutputVolumeModel m = c;
    m.lo_db = c.lo_db - c.hi_db;
    m.hi_db = 0;
    return m;
}

int sink_level_for_volume(const OutputVolumeModel &m, uint32_t volume) {
    if (volume == 0) return 0;
    return m.nearest(sink_db_for_volume(volume));
}

bool placeholder_sink(const std::string &name) { return name.compare(0, 9, "auto_null") == 0; }

/* ------------------------------------------------------------------------------------------------ presses */

MuteBy mute_by(const MixerChoice &m) {
    if (!m.ok()) return MuteBy::Cassette;
    if (!m.sw.empty()) return MuteBy::Switch;
    return m.min_is_mute ? MuteBy::MuteValue : MuteBy::Cassette;
}

VolumePress mixer_press(const MixerChoice &m, const MixerLevel &lv, int delta) {
    VolumePress p;
    if (!lv.has_mixer || !m.ok() || delta == 0 || lv.level < 0) return p;
    if (lv.muted) {
        if (delta < 0) return p; /* already silent */
        if (lv.level >= 1) {
            p.kind = VolumePress::Kind::Unmute; /* the switch on, or Cassette's own mute off: the level it had */
            p.level = lv.level;
            return p;
        }
        p.kind = VolumePress::Kind::Set; /* at the mute value: the first level that sounds */
        p.level = 1;
        return p;
    }
    if (delta < 0 && lv.level <= 1) {
        switch (mute_by(m)) {
        case MuteBy::Switch: p.kind = VolumePress::Kind::Mute; break;
        case MuteBy::MuteValue: /* the control's own mute value */
            p.kind = VolumePress::Kind::Set;
            p.level = 0;
            break;
        case MuteBy::Cassette: p.kind = VolumePress::Kind::Mute; break; /* the Player silent, the card's level kept */
        }
        return p;
    }
    const int to = m.model.step(lv.level, delta);
    if (to == lv.level) return p;
    p.kind = VolumePress::Kind::Set;
    p.level = to;
    return p;
}

VolumePress sink_press(const OutputVolumeModel &m, int level, bool muted, int delta) {
    VolumePress p;
    if (level < 0 || delta == 0) return p;
    if (muted || level == 0) {
        if (delta < 0) return p;
        if (muted && level >= 1) {
            p.kind = VolumePress::Kind::Unmute; /* the level it had */
            p.level = level;
        } else {
            p.kind = VolumePress::Kind::Set;
            p.level = 1;
        }
        return p;
    }
    if (delta < 0 && level <= 1) {
        p.kind = VolumePress::Kind::Mute;
        return p;
    }
    const int to = m.step(level, delta);
    if (to == level) return p;
    p.kind = VolumePress::Kind::Set;
    p.level = to;
    return p;
}

int software_level(const OwnVolume &v, bool *muted) {
    const int lv = default_volume_model().from_pct(v.pct);
    if (muted) *muted = v.muted || lv == 0;
    return lv;
}

VolumePress software_press(OwnVolume &v, int delta) {
    const OutputVolumeModel m = default_volume_model();
    bool muted = false;
    const int lv = software_level(v, &muted);
    const VolumePress p = sink_press(m, lv, muted, delta);
    switch (p.kind) {
    case VolumePress::Kind::None: break;
    case VolumePress::Kind::Mute: v.muted = true; break; /* the level kept */
    case VolumePress::Kind::Unmute: v.muted = false; break;
    case VolumePress::Kind::Set:
        v.pct = m.pct_of(p.level);
        v.muted = false;
        break;
    }
    return p;
}

MixerLevel mixer_level_shown(const MixerChoice &m, const MixerLevel &lv, bool own_muted) {
    MixerLevel out = lv;
    if (lv.has_mixer && lv.level >= 1 && mute_by(m) == MuteBy::Cassette && own_muted) out.muted = true;
    return out;
}

float volume_gain_for_db(double db) { return static_cast<float>(std::pow(10.0, db / 20.0)); }

/* ------------------------------------------------------------------------------------------------ the target */

VolumeTarget volume_target_for(const std::string &device, bool bluetooth, const std::string &bt_sink, bool server,
                               bool server_expected, const std::string &default_sink) {
    VolumeTarget t;
    if (bluetooth) {
        std::string mac = device.compare(0, 3, "bt:") == 0 ? device.substr(3) : device;
        for (char &c : mac)
            if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
        t.key = "bt:" + mac;
        if (server && !bt_sink.empty()) {
            t.kind = VolumeTarget::Kind::Sink;
            t.sink = bt_sink;
        }
        return t;
    }
    const std::string card = mixer_card_for(device);
    if (card == "default" && (server || server_expected)) { /* ALSA's default plays through the server's default sink */
        if (server && !default_sink.empty() && !placeholder_sink(default_sink)) {
            t.kind = VolumeTarget::Kind::Sink;
            t.sink = default_sink;
            t.key = "pulse:" + default_sink;
        }
        return t;
    }
    t.kind = VolumeTarget::Kind::Mixer;
    t.card = card;
    t.key = "alsa:" + card;
    return t;
}

bool first_use_silent(int level, bool muted) { return level >= 0 && (muted || level <= 1); }

/* ------------------------------------------------------------------------------------------------ texts */

std::string volume_log_text(int pct) { return pct < 0 ? std::string("-- (not known)") : std::to_string(pct) + " %"; }

std::string volume_set_log_text(const std::string &out, const OutputVolumeModel &m, int level, long raw, int read_result,
                                long read_raw) {
    char target[48];
    if (level <= 0) std::snprintf(target, sizeof target, "mute");
    else if (m.db) std::snprintf(target, sizeof target, "%.2f dB", m.db_of(level));
    else std::snprintf(target, sizeof target, "raw step %d", level);
    char back[48];
    if (read_result >= 0) std::snprintf(back, sizeof back, "raw=%ld", read_raw);
    else std::snprintf(back, sizeof back, "failed (%d)", read_result);
    return "set " + out + " step " + std::to_string(level) + "/" + std::to_string(m.n) + " " + std::to_string(m.pct_of(level)) +
           " % target " + target + " raw=" + std::to_string(raw) + " read back " + back;
}

std::string volume_model_text(const OutputVolumeModel &m) {
    char b[96];
    if (!m.db) std::snprintf(b, sizeof b, "%d raw steps (no dB data)", m.n);
    else std::snprintf(b, sizeof b, "%.2f..%.2f dB, %d levels of %.2f dB", m.lo_db, m.hi_db, m.n, m.step_db());
    return b;
}

}  // namespace cassette
