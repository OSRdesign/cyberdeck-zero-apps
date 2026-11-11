/* SPDX-License-Identifier: MIT */
#include "mixer.hpp"

#include <algorithm>
#include <cerrno>
#include <cmath>

namespace cassette {

double LevelRange::raw_to_db(long raw) const {
    if (raw_max <= raw_min) return db_min;
    raw = std::max(raw_min, std::min(raw_max, raw));
    return db_min + static_cast<double>(raw - raw_min) * (db_max - db_min) / static_cast<double>(raw_max - raw_min);
}

long LevelRange::db_to_raw(double db) const {
    if (raw_max <= raw_min || db_max <= db_min) return raw_min;
    db = std::max(db_min, std::min(db_max, db));
    double pos = (db - db_min) / (db_max - db_min) * static_cast<double>(raw_max - raw_min);
    long raw = raw_min + std::lround(pos);
    long st = std::max<long>(step, 1);
    if (st > 1) raw = raw_min + std::lround(static_cast<double>(raw - raw_min) / st) * st; /* nearest step */
    return std::max(raw_min, std::min(raw_max, raw));
}

namespace {

bool ends_with(const std::string &s, const char *tail) {
    std::string t = tail;
    return s.size() >= t.size() && s.compare(s.size() - t.size(), t.size(), t) == 0;
}

/* The best match: "Mic" first, then plain "Capture ...", then any other prefix. */
const CtlInfo *pick(const std::vector<CtlInfo> &v, CtlInfo::Type type, const char *suffix) {
    const CtlInfo *best = nullptr;
    int best_rank = 99;
    for (const CtlInfo &c : v) {
        if (c.type != type || !c.readable || !ends_with(c.name, suffix)) continue;
        int rank = c.name.rfind("Mic", 0) == 0 ? 0 : (c.name.rfind("Capture", 0) == 0 ? 1 : 2);
        if (rank < best_rank) {
            best = &c;
            best_rank = rank;
        }
    }
    return best;
}

} // namespace

int Mixer::discover() {
    caps_ = MixerCaps();
    level_name_.clear();
    mute_name_.clear();
    agc_name_.clear();
    ctls_.clear();
    int r = api_.mixer_list(card_, ctls_);
    if (r < 0) {
        ctls_.clear();
        return r;
    }
    if (const CtlInfo *c = pick(ctls_, CtlInfo::Type::Integer, "Capture Volume"); c && c->max > c->min) {
        caps_.has_level = true;
        level_name_ = c->name;
        range_.raw_min = c->min;
        range_.raw_max = c->max;
        range_.step = c->step > 0 ? c->step : 1;
        if (c->has_db) {
            range_.db_min = c->db_min_cdb / 100.0;
            range_.db_max = c->db_max_cdb / 100.0;
            range_.db_inferred = false;
        } else {
            range_.db_min = kInferredDbMin;
            range_.db_max = kInferredDbMax;
            range_.db_inferred = true;
        }
    }
    if (const CtlInfo *c = pick(ctls_, CtlInfo::Type::Boolean, "Capture Switch")) {
        caps_.has_mute = true;
        mute_name_ = c->name;
    }
    for (const CtlInfo &c : ctls_)
        if (c.type == CtlInfo::Type::Boolean && c.readable && ends_with(c.name, "Auto Gain Control")) {
            caps_.has_agc = true;
            agc_name_ = c.name;
            break;
        }
    return 0;
}

MixerState Mixer::read() {
    MixerState s;
    long v = 0;
    if (caps_.has_level && api_.mixer_get(card_, level_name_, v) >= 0) {
        s.level_valid = true;
        s.level_raw = v;
        s.level_db = range_.raw_to_db(v);
    }
    if (caps_.has_agc && api_.mixer_get(card_, agc_name_, v) >= 0) {
        s.agc_valid = true;
        s.agc = v != 0;
    }
    if (caps_.has_mute && api_.mixer_get(card_, mute_name_, v) >= 0) {
        s.mute_valid = true;
        s.mute = v == 0;
    }
    return s;
}

namespace {
bool writable(const std::vector<CtlInfo> &v, const std::string &name) {
    for (const CtlInfo &c : v)
        if (c.name == name) return c.writable;
    return false;
}
} // namespace

bool Mixer::set_level_db(double db, double *applied_db, int *err) {
    if (!caps_.has_level || !writable(ctls_, level_name_)) {
        if (err) *err = -ENOENT;
        return false;
    }
    long raw = range_.db_to_raw(db);
    int r = api_.mixer_set(card_, level_name_, raw);
    if (err) *err = r;
    if (r < 0) return false;
    if (applied_db) *applied_db = range_.raw_to_db(raw);
    return true;
}

bool Mixer::set_agc(bool on, int *err) {
    if (!caps_.has_agc || !writable(ctls_, agc_name_)) {
        if (err) *err = -ENOENT;
        return false;
    }
    int r = api_.mixer_set(card_, agc_name_, on ? 1 : 0);
    if (err) *err = r;
    return r >= 0;
}

bool Mixer::set_mute(bool muted, int *err) {
    if (!caps_.has_mute || !writable(ctls_, mute_name_)) {
        if (err) *err = -ENOENT;
        return false;
    }
    int r = api_.mixer_set(card_, mute_name_, muted ? 0 : 1); /* the switch is "capture enabled" */
    if (err) *err = r;
    return r >= 0;
}

int Mixer::reapply(const MixerSetting &s) {
    int n = 0;
    if (s.set_level && set_level_db(s.level_db)) ++n;
    if (s.set_agc && set_agc(s.agc)) ++n;
    if (s.set_mute && set_mute(s.mute)) ++n;
    return n;
}

} // namespace cassette
