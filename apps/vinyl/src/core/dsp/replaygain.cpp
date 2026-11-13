/*
 * SPDX-License-Identifier: MIT
 *
 * Vinyl ReplayGain (see replaygain.hpp).
 */
#include "replaygain.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>

namespace vinyl {

const char *replaygain_mode_name(ReplayGainMode m) {
    switch (m) {
    case ReplayGainMode::Off: return "off";
    case ReplayGainMode::Album: return "album";
    default: return "track";
    }
}

ReplayGainMode replaygain_mode_from(const std::string &s, ReplayGainMode fallback) {
    if (s == "off") return ReplayGainMode::Off;
    if (s == "track") return ReplayGainMode::Track;
    if (s == "album") return ReplayGainMode::Album;
    return fallback;
}

const char *replaygain_mode_word(ReplayGainMode m) {
    switch (m) {
    case ReplayGainMode::Off: return "Off";
    case ReplayGainMode::Album: return "Album";
    default: return "Track";
    }
}

namespace {

/* a leading decimal number (locale-free: strtod would follow LC_NUMERIC) */
bool leading_number(const std::string &v, double &out) {
    size_t i = 0;
    while (i < v.size() && (v[i] == ' ' || v[i] == '\t')) ++i;
    bool neg = false;
    if (i < v.size() && (v[i] == '+' || v[i] == '-')) neg = v[i++] == '-';
    double x = 0;
    bool digits = false;
    while (i < v.size() && std::isdigit(static_cast<unsigned char>(v[i]))) {
        x = x * 10 + (v[i++] - '0');
        digits = true;
    }
    if (i < v.size() && (v[i] == '.' || v[i] == ',')) {
        ++i;
        double scale = 0.1;
        while (i < v.size() && std::isdigit(static_cast<unsigned char>(v[i]))) {
            x += (v[i++] - '0') * scale;
            scale /= 10;
            digits = true;
        }
    }
    if (!digits) return false;
    out = neg ? -x : x;
    return std::isfinite(out);
}

std::string upper(std::string s) {
    for (char &c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

}  // namespace

bool parse_gain_db(const std::string &v, float &out) {
    double x = 0;
    if (!leading_number(v, x) || std::fabs(x) > 60) return false; /* a gain past 60 dB is a damaged tag */
    out = static_cast<float>(x);
    return true;
}

bool parse_peak(const std::string &v, float &out) {
    double x = 0;
    if (!leading_number(v, x) || x < 0 || x > 100) return false;
    out = static_cast<float>(x);
    return true;
}

bool parse_r128_db(const std::string &v, float &out) {
    double x = 0;
    if (!leading_number(v, x) || std::fabs(x) > 32768) return false;
    out = static_cast<float>(x / 256.0 + 5.0);
    return true;
}

bool apply_replaygain_tag(ReplayGainTags &rg, const std::string &name_in, const std::string &value) {
    const std::string name = upper(name_in);
    float f = 0;
    if (name == "R128_TRACK_GAIN") {
        if (parse_r128_db(value, f)) {
            rg.track_db = f;
            rg.has_track = true;
            rg.r128_track = true;
        }
        return true;
    }
    if (name == "R128_ALBUM_GAIN") {
        if (parse_r128_db(value, f)) {
            rg.album_db = f;
            rg.has_album = true;
            rg.r128_album = true;
        }
        return true;
    }
    if (name == "REPLAYGAIN_TRACK_GAIN") {
        if (!rg.r128_track && parse_gain_db(value, f)) {
            rg.track_db = f;
            rg.has_track = true;
        }
        return true;
    }
    if (name == "REPLAYGAIN_ALBUM_GAIN") {
        if (!rg.r128_album && parse_gain_db(value, f)) {
            rg.album_db = f;
            rg.has_album = true;
        }
        return true;
    }
    if (name == "REPLAYGAIN_TRACK_PEAK") {
        if (parse_peak(value, f)) {
            rg.track_peak = f;
            rg.has_track_peak = true;
        }
        return true;
    }
    if (name == "REPLAYGAIN_ALBUM_PEAK") {
        if (parse_peak(value, f)) {
            rg.album_peak = f;
            rg.has_album_peak = true;
        }
        return true;
    }
    return false;
}

double replaygain_db(ReplayGainMode mode, const ReplayGainTags &rg, double preamp_db) {
    if (mode == ReplayGainMode::Off || !rg.any()) return 0.0;
    const bool album = mode == ReplayGainMode::Album ? rg.has_album : !rg.has_track;
    double g = (album ? rg.album_db : rg.track_db) + preamp_db;
    /* the matching peak first, else the other one (a file with one peak tag) */
    float peak = 0;
    if (album) peak = rg.has_album_peak ? rg.album_peak : (rg.has_track_peak ? rg.track_peak : 0.0f);
    else peak = rg.has_track_peak ? rg.track_peak : (rg.has_album_peak ? rg.album_peak : 0.0f);
    if (peak > 0) g = std::min(g, -20.0 * std::log10(static_cast<double>(peak)));
    return g;
}

float replaygain_factor(ReplayGainMode mode, const ReplayGainTags &rg, double preamp_db) {
    const double db = replaygain_db(mode, rg, preamp_db);
    if (db == 0.0) return 1.0f;
    return static_cast<float>(std::pow(10.0, db / 20.0));
}

std::string replaygain_chip_text(ReplayGainMode mode, const ReplayGainTags &rg) {
    if (mode == ReplayGainMode::Off || !rg.any()) return "";
    double db = replaygain_db(mode, rg);
    if (std::fabs(db) < 0.05) db = 0.0; /* never "-0.0" */
    char b[32];
    std::snprintf(b, sizeof b, "RG %+.1f dB", db);
    if (db == 0.0) std::snprintf(b, sizeof b, "RG 0.0 dB");
    return b;
}

}  // namespace vinyl
