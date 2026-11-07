/*
 * SPDX-License-Identifier: MIT
 *
 * Deck Atlas: the online sources as the Sources page shows them, and the switch-on flow (owner decisions 2026-11-07):
 *   - a source's row says its state in a word (Off, On, Needs key, Offline, Paused by server, Daily cap reached with
 *     the counts, ...) from the engine's listing (cp0_map_source_info_t: enabled, state, has_key, counts);
 *   - switching a source ON: a source that needs a key and has none asks for the key first; then, the first time only
 *     (remembered per source id in settings.conf), the terms dialog (name, the app's own plain text per source, the daily
 *     limit, the provider's address as plain text) with Cancel / "I agree, enable"; switching it off and on again does
 *     not ask again. Switching OFF never asks.
 *   - a key is never shown again once saved: the page says "Key saved" or "No key".
 * Pure: plain values in, texts and steps out; the engine's state codes come from cp0_map_common.h (constants only).
 */
#pragma once

#include <cstdint>
#include <string>

namespace atlas {

enum class Tone { Normal, Ok, Warn, Bad, Muted };

struct SourceFacts {
    std::string id, title;
    bool enabled = false;
    bool needs_key = false;
    bool has_key = false;
    bool network_on = true;       /* the app's offline switch is off */
    int state = 0;                /* cp0_map_source_info_t.state: CP0_MAP_OK or a CP0_MAP_E_* code */
    std::int64_t until = 0;
    std::uint32_t fetches_today = 0, daily_cap = 0;
    std::uint64_t cache_tiles = 0;
};

struct StateWord {
    std::string text;
    Tone tone = Tone::Normal;
};

/* The word in the source's row: "Off", "On", "On 120/2,000", "Needs key", "Offline", "Paused by server",
 * "Daily cap reached 2,000/2,000", "Server busy", "No connection", "No libcurl", "Another app", "Cache full",
 * "Refused", "No user agent". */
StateWord source_state_word(const SourceFacts &f);
/* A longer sentence for the source's page or a log line ("Daily cap reached (2,000 of 2,000 tiles today): saved tiles
 * only until midnight"). */
std::string source_state_sentence(const SourceFacts &f);

/* What a tap on the source's switch does next. */
enum class ToggleStep {
    Disable,    /* it was on: switch it off */
    AskKey,     /* off, needs a key, none saved: the key page first (then next_after_key) */
    AskTerms,   /* off, terms not accepted yet: the terms dialog (I agree -> Enable) */
    Enable,     /* off, everything settled: switch it on */
};
ToggleStep toggle_step(bool enabled, bool needs_key, bool has_key, bool terms_accepted);
/* After the key was saved on the way to switching on. */
ToggleStep next_after_key(bool terms_accepted);

/* "Key saved" / "No key". */
const char *key_status(bool has_key);
/* "" for a usable key (8..127 characters of A-Z a-z 0-9 . _ ~ -, the engine's rule), else why not. Spaces around a
 * pasted key are not part of it: trim_key() first. */
std::string key_error(const std::string &key);
std::string trim_key(const std::string &key);

/* The provider's terms as plain text for the terms dialog (no network): known for the shipped records. */
struct ProviderTerms {
    std::string terms_url;        /* "https://www.thunderforest.com/terms/" */
    std::string key_hint;         /* where a key comes from ("" for a source without key) */
    std::string what;             /* the app's own words: what the source is */
    std::string rules;            /* the plain rules in one or two sentences */
};
/* The app's table for the shipped sources; an unknown id gets a short generic text (no URL, no internal wording). */
ProviderTerms provider_terms(const std::string &id);

/* "Daily limit in this app: 2,000 requests" ("" for no cap). */
std::string daily_limit_line(std::uint32_t daily_cap);
/* The terms dialog's text, all from the app's own table (never the engine record's note): what the source is, the plain
 * rules, the daily limit, "Check the provider's current terms before relying on this." with the provider's address
 * (when known), then the map data attribution line (the record's licence text). `title` is for the generic fallback. */
std::string terms_text(const std::string &title, const std::string &licence, const ProviderTerms &terms,
                       std::uint32_t daily_cap);

} // namespace atlas
