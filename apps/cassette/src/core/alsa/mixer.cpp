/* SPDX-License-Identifier: MIT */
#include "mixer.hpp"

#include <algorithm>
#include <cerrno>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>

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

std::string lower(std::string s) {
    for (char &c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string trimmed(std::string s) {
    while (!s.empty() && s.back() == ' ') s.pop_back();
    while (!s.empty() && s.front() == ' ') s.erase(s.begin());
    return s;
}

/* "+10 dB", "+2.5 dB", "-6 dB"; "Off" for a step of no gain */
std::string boost_label(double db) {
    if (std::fabs(db) < 0.05) return "Off";
    char b[32];
    if (std::fabs(db - std::round(db)) < 0.05) std::snprintf(b, sizeof b, "%+d dB", static_cast<int>(std::lround(db)));
    else std::snprintf(b, sizeof b, "%+.1f dB", db);
    return b;
}

bool writable(const std::vector<CtlInfo> &v, const std::string &name) {
    for (const CtlInfo &c : v)
        if (c.name == name) return c.writable;
    return false;
}

} // namespace

void Mixer::pick_active_boost() {
    boost_active_ = -1;
    boost_name_.clear();
    boost_source_.clear();
    boost_choices_.clear();
    if (boosts_.empty()) return;
    /* the input the card is capturing from: the selected item of the source enumeration, else the capture switch that is on */
    std::string src;
    for (const CtlInfo &c : ctls_) {
        if (c.type != CtlInfo::Type::Enumerated || !c.readable || (c.name != "Input Source" && c.name != "Capture Source")) continue;
        long v = 0;
        if (!c.items.empty() && api_.mixer_get(card_, c.name, v) >= 0 && v >= 0 && v < static_cast<long>(c.items.size())) {
            src = c.items[static_cast<size_t>(v)];
            break;
        }
    }
    if (src.empty()) {
        static const char kSw[] = " Capture Switch";
        for (const CtlInfo &c : ctls_) {
            if (c.type != CtlInfo::Type::Boolean || !c.readable || !ends_with(c.name, kSw) || c.name.size() == sizeof kSw - 1) continue;
            long v = 0;
            if (api_.mixer_get(card_, c.name, v) >= 0 && v != 0) {
                src = c.name.substr(0, c.name.size() - (sizeof kSw - 1));
                break;
            }
        }
    }
    int pick = -1;
    if (!src.empty()) {
        const std::string want = lower(trimmed(src));
        for (size_t i = 0; i < boosts_.size() && pick < 0; ++i)
            if (lower(boosts_[i].source) == want) pick = static_cast<int>(i);
        for (size_t i = 0; i < boosts_.size() && pick < 0; ++i) { /* "Mic" and "Internal Mic" are the same input to a boost named for one */
            const std::string b = lower(boosts_[i].source);
            if (!b.empty() && (want.find(b) != std::string::npos || b.find(want) != std::string::npos)) pick = static_cast<int>(i);
        }
    }
    if (pick < 0)
        for (size_t i = 0; i < boosts_.size() && pick < 0; ++i)
            if (lower(boosts_[i].source).rfind("internal", 0) == 0) pick = static_cast<int>(i);
    if (pick < 0) pick = 0;
    boost_active_ = pick;
    const Boost &b = boosts_[static_cast<size_t>(pick)];
    boost_name_ = b.name;
    boost_source_ = b.source;
    const CtlInfo &c = b.ctl;
    long first = c.min, last = std::min(c.max, c.min + 15); /* a boost has a few steps */
    for (long raw = first; raw <= last; ++raw) {
        BoostChoice ch;
        ch.raw = raw;
        if (c.has_db && c.max > c.min) {
            ch.db = (c.db_min_cdb + (c.db_max_cdb - c.db_min_cdb) * static_cast<double>(raw - c.min) / static_cast<double>(c.max - c.min)) / 100.0;
            ch.label = boost_label(ch.db);
        } else {
            ch.label = raw == c.min ? "Off" : "Step " + std::to_string(raw - c.min); /* no dB from the driver */
        }
        boost_choices_.push_back(ch);
    }
}

int Mixer::discover() {
    caps_ = MixerCaps();
    level_name_.clear();
    mute_name_.clear();
    agc_name_.clear();
    ctls_.clear();
    boosts_.clear();
    boost_active_ = -1;
    boost_name_.clear();
    boost_source_.clear();
    boost_choices_.clear();
    sg_.reset();
    server_off_ = false;
    note_.clear();
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
    static const char kBoost[] = "Mic Boost Volume";
    for (const CtlInfo &c : ctls_) {
        if (c.type != CtlInfo::Type::Integer || !c.readable || !c.writable || c.max <= c.min || !ends_with(c.name, kBoost)) continue;
        Boost b;
        b.name = c.name;
        b.ctl = c;
        /* "Internal Mic Boost Volume" -> "Internal Mic"; "Mic Boost Volume" -> "" */
        const std::string head = trimmed(c.name.substr(0, c.name.size() - (sizeof kBoost - 1)));
        b.source = head.empty() ? std::string() : head + " Mic";
        boosts_.push_back(b);
    }
    caps_.has_boost = !boosts_.empty();
    pick_active_boost();
    if (runner_ && alsa_card_ >= 0 && caps_.has_level && writable(ctls_, level_name_)) {
        std::unique_ptr<ServerGain> g(new ServerGain(*runner_));
        if (g->resolve(alsa_card_)) sg_ = std::move(g);
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
    if (caps_.has_boost) {
        pick_active_boost(); /* the jack may have switched the input since the last read */
        if (!boost_name_.empty() && api_.mixer_get(card_, boost_name_, v) >= 0) {
            s.boost_valid = true;
            s.boost_raw = v;
            for (size_t i = 0; i < boost_choices_.size(); ++i)
                if (boost_choices_[i].raw == v) {
                    s.boost_index = static_cast<int>(i);
                    s.boost_db = boost_choices_[i].db;
                }
        }
    }
    return s;
}

bool Mixer::set_level_db(double db, double *applied_db, int *err) {
    if (!caps_.has_level || !writable(ctls_, level_name_)) {
        if (err) *err = -ENOENT;
        return false;
    }
    long raw = range_.db_to_raw(db);
    if (level_via_server() && set_level_via_server(raw, range_.raw_to_db(raw))) {
        long now = raw;
        if (api_.mixer_get(card_, level_name_, now) < 0) now = raw;
        if (err) *err = 0;
        if (applied_db) *applied_db = range_.raw_to_db(now); /* what the card really has */
        return true;
    }
    int r = api_.mixer_set(card_, level_name_, raw);
    if (err) *err = r;
    if (r < 0) return false;
    if (applied_db) *applied_db = range_.raw_to_db(raw);
    return true;
}

/* Sets the level on the server's source and looks at the ALSA control to see what the server wrote. Some servers round a step
 * the other way: one more try, a quarter step towards the wanted one. true when the card is at the wanted step (or within a
 * step and a half); false (and the server is not used again) when it is not: the caller writes the card directly. */
bool Mixer::set_level_via_server(long want_raw, double want_db) {
    const double span = static_cast<double>(range_.raw_max - range_.raw_min);
    const double step_db = span > 0 ? (range_.db_max - range_.db_min) / span * static_cast<double>(std::max<long>(range_.step, 1)) : 1.0;
    /* The server maps our volume to the card's steps its own way (it may round down, or need a little more than a step): the exact
     * value first, then a quarter and a bit more of a step towards the wanted one. Only the wanted step itself counts as set: a card
     * that stays a step short is NOT "set" (the stepper must move), the card is written directly instead. */
    static const double kNudge[] = {0.0, 0.25, 0.6};
    long got = 0;
    bool read_ok = false;
    report_.clear();
    for (double nudge : kNudge) {
        double ask = want_db;
        if (nudge != 0.0) ask += (want_raw > got ? nudge : -nudge) * step_db;
        if (!sg_->set_gain_db(ask)) {
            note_ = "the sound server did not take the level (pactl failed); the card is written directly";
            server_off_ = true;
            return false;
        }
        read_ok = api_.mixer_get(card_, level_name_, got) >= 0;
        if (!read_ok) break;
        if (got == want_raw) break;
    }
    uint32_t sv = 0;
    char r[160];
    if (sg_->read_volume(sv))
        std::snprintf(r, sizeof r, "asked V %u, source now V %u (%.2f dB from its base)", sg_->last_asked(), sv,
                      server_gain_of_volume(sv, sg_->base_raw()));
    else std::snprintf(r, sizeof r, "asked V %u, source volume not readable", sg_->last_asked());
    report_ = r;
    if (read_ok && got == want_raw) return true;
    char b[220];
    if (read_ok)
        std::snprintf(b, sizeof b, "the sound server did not set the card to %.2f dB (the card has %.2f dB); the card is written directly",
                      want_db, range_.raw_to_db(got));
    else std::snprintf(b, sizeof b, "the card's level could not be read back after the sound server call; the card is written directly");
    note_ = b;
    server_off_ = true;
    return false;
}

bool Mixer::set_boost_index(int index, int *err) {
    if (!caps_.has_boost || boost_active_ < 0 || index < 0 || index >= static_cast<int>(boost_choices_.size()) ||
        !writable(ctls_, boost_name_)) {
        if (err) *err = -ENOENT;
        return false;
    }
    int r = api_.mixer_set(card_, boost_name_, boost_choices_[static_cast<size_t>(index)].raw);
    if (err) *err = r;
    return r >= 0;
}

bool Mixer::set_boost_db(double db, double *applied_db, int *err) {
    if (boost_choices_.empty()) {
        if (err) *err = -ENOENT;
        return false;
    }
    int best = 0;
    for (size_t i = 1; i < boost_choices_.size(); ++i)
        if (std::fabs(boost_choices_[i].db - db) < std::fabs(boost_choices_[static_cast<size_t>(best)].db - db)) best = static_cast<int>(i);
    if (!set_boost_index(best, err)) return false;
    if (applied_db) *applied_db = boost_choices_[static_cast<size_t>(best)].db;
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
    if (s.set_boost && set_boost_db(s.boost_db)) ++n;
    if (s.set_agc && set_agc(s.agc)) ++n;
    if (s.set_mute && set_mute(s.mute)) ++n;
    return n;
}

} // namespace cassette
