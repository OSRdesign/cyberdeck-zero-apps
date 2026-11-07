/*
 * SPDX-License-Identifier: MIT
 *
 * Deck Atlas tests: the online sources' state words, the switch-on flow (key first, the terms once per source), the
 * key rule and the terms dialog's text.
 */
#include "check.hpp"
#include "settings.hpp"
#include "source_model.hpp"

#include "cp0_map_common.h"

using namespace atlas;

void test_sources()
{
    SourceFacts f;
    f.id = "opentopomap";
    f.title = "OpenTopoMap";
    f.daily_cap = 2000;

    /* the row's word, by the engine's state */
    f.enabled = false;
    f.state = CP0_MAP_E_DISABLED;
    CHECK_EQ(source_state_word(f).text, std::string("Off"));
    CHECK(source_state_word(f).tone == Tone::Muted);
    f.enabled = true;
    f.state = CP0_MAP_OK;
    CHECK_EQ(source_state_word(f).text, std::string("On"));
    CHECK(source_state_word(f).tone == Tone::Ok);
    f.fetches_today = 120;
    CHECK_EQ(source_state_word(f).text, std::string("On 120/2,000"));
    f.state = CP0_MAP_E_OFFLINE;
    CHECK_EQ(source_state_word(f).text, std::string("Offline"));
    CHECK(source_state_word(f).tone == Tone::Warn);
    f.state = CP0_MAP_E_NO_KEY;
    CHECK_EQ(source_state_word(f).text, std::string("Needs key"));
    f.state = CP0_MAP_E_BLOCKED;
    CHECK_EQ(source_state_word(f).text, std::string("Paused by server"));
    CHECK(source_state_word(f).tone == Tone::Bad);
    f.state = CP0_MAP_E_CAP_REACHED;
    f.fetches_today = 2000;
    CHECK_EQ(source_state_word(f).text, std::string("Daily cap reached 2,000/2,000"));
    CHECK_EQ(source_state_sentence(f),
             std::string("Daily cap reached (2,000 of 2,000 requests today): saved tiles only until midnight"));
    f.state = CP0_MAP_E_BACKOFF;
    CHECK_EQ(source_state_word(f).text, std::string("Server busy"));
    f.state = CP0_MAP_E_NETWORK;
    CHECK_EQ(source_state_word(f).text, std::string("No connection"));
    f.state = CP0_MAP_E_CURL_MISSING;
    CHECK_EQ(source_state_word(f).text, std::string("No libcurl"));
    CHECK_EQ(source_state_sentence(f),
             std::string("Network library missing: install libcurl4t64 (saved tiles still show)"));
    f.state = CP0_MAP_E_READ_ONLY;
    CHECK_EQ(source_state_word(f).text, std::string("Another app"));
    f.state = CP0_MAP_E_CACHE_FULL;
    CHECK_EQ(source_state_word(f).text, std::string("Cache full"));
    f.state = CP0_MAP_E_REFUSED;
    CHECK_EQ(source_state_word(f).text, std::string("Refused"));
    f.state = CP0_MAP_E_NO_USER_AGENT;
    CHECK_EQ(source_state_word(f).text, std::string("No user agent"));
    f.state = -999;
    CHECK_EQ(source_state_word(f).text, std::string("Error"));
    f.enabled = false;
    f.state = CP0_MAP_E_NO_KEY; /* off wins over the engine's reason */
    CHECK_EQ(source_state_word(f).text, std::string("Off"));
    CHECK_EQ(source_state_sentence(f), std::string("Switched off: no tile is requested"));

    /* the switch-on flow */
    CHECK(toggle_step(true, true, true, true) == ToggleStep::Disable);
    CHECK(toggle_step(true, false, false, false) == ToggleStep::Disable); /* off never asks */
    CHECK(toggle_step(false, true, false, false) == ToggleStep::AskKey);  /* the key first ... */
    CHECK(toggle_step(false, true, false, true) == ToggleStep::AskKey);
    CHECK(next_after_key(false) == ToggleStep::AskTerms);                 /* ... then the terms */
    CHECK(next_after_key(true) == ToggleStep::Enable);
    CHECK(toggle_step(false, true, true, false) == ToggleStep::AskTerms);
    CHECK(toggle_step(false, false, false, false) == ToggleStep::AskTerms);
    CHECK(toggle_step(false, false, false, true) == ToggleStep::Enable);
    CHECK(toggle_step(false, true, true, true) == ToggleStep::Enable);

    /* the terms once per source: accepted in the settings, off and on again does not ask */
    Settings s;
    CHECK(toggle_step(false, false, false, terms_accepted(s, "opentopomap")) == ToggleStep::AskTerms);
    accept_terms(s, "opentopomap");
    CHECK(toggle_step(true, false, false, terms_accepted(s, "opentopomap")) == ToggleStep::Disable);
    CHECK(toggle_step(false, false, false, terms_accepted(s, "opentopomap")) == ToggleStep::Enable);
    CHECK(toggle_step(false, true, true, terms_accepted(s, "geoapify")) == ToggleStep::AskTerms);
    const Settings back = parse_settings(serialize_settings(s));
    CHECK(terms_accepted(back, "opentopomap"));
    CHECK(!terms_accepted(back, "geoapify"));

    /* keys: never shown, only saved or not; the engine's character rule */
    CHECK_EQ(std::string(key_status(true)), std::string("Key saved"));
    CHECK_EQ(std::string(key_status(false)), std::string("No key"));
    CHECK_EQ(key_error("abcd1234"), std::string(""));
    CHECK_EQ(key_error("A1b2C3d4._~-xyz"), std::string(""));
    CHECK_EQ(key_error(""), std::string("Type or paste your key"));
    CHECK_EQ(key_error("short"), std::string("A key has 8 to 127 characters"));
    CHECK_EQ(key_error(std::string(128, 'a')), std::string("A key has 8 to 127 characters"));
    CHECK_EQ(key_error(std::string(127, 'a')), std::string(""));
    CHECK_EQ(key_error("abcd 1234"), std::string("Only letters, digits and . _ ~ - in a key"));
    CHECK_EQ(key_error("abcd/1234"), std::string("Only letters, digits and . _ ~ - in a key"));
    CHECK_EQ(trim_key("  abcd1234\n"), std::string("abcd1234"));
    CHECK_EQ(trim_key("\t"), std::string(""));

    /* the providers' terms (plain text, no network) */
    CHECK_EQ(provider_terms("opentopomap").terms_url, std::string("https://opentopomap.org/about"));
    CHECK(provider_terms("opentopomap").key_hint.empty());
    CHECK_EQ(provider_terms("thunderforest").terms_url, std::string("https://www.thunderforest.com/terms/"));
    CHECK(!provider_terms("thunderforest").key_hint.empty());
    CHECK_EQ(provider_terms("geoapify").terms_url, std::string("https://www.geoapify.com/terms-and-conditions/"));
    CHECK(provider_terms("my-own").terms_url.empty());
    CHECK_EQ(daily_limit_line(2000), std::string("Daily limit in this app: 2,000 requests"));
    CHECK_EQ(daily_limit_line(0), std::string(""));
    /* the dialog text comes from the app's own table: every shipped source has text, none of it internal wording */
    const char *shipped[] = {"opentopomap", "thunderforest", "geoapify"};
    /* the privacy line: in every online source's text, its own paragraph */
    const std::string privacy = "\n\nThe tile provider can see your IP address when this device fetches tiles; tiles "
                                "already saved on the SD card are not fetched again.\n\n";
    for (const char *id : shipped) {
        const ProviderTerms pt = provider_terms(id);
        CHECK(!pt.what.empty());
        CHECK(!pt.rules.empty());
        CHECK(!pt.terms_url.empty());
        const std::string tt = terms_text(id, "Map data (c) somebody", pt, 1500);
        CHECK(!tt.empty());
        CHECK(tt.find("054") == std::string::npos);
        CHECK(tt.find("unverified") == std::string::npos);
        CHECK(tt.find("check before enabling") == std::string::npos);
        CHECK(tt.find("Daily limit in this app: 1,500 requests") != std::string::npos);
        CHECK(tt.find("Check the provider's current terms before relying on this.\n" + pt.terms_url) != std::string::npos);
        CHECK(tt.find("Map data (c) somebody") != std::string::npos);
        CHECK(tt.find(privacy) != std::string::npos);
    }
    const std::string t = terms_text("OpenTopoMap", "Map style CC-BY-SA", provider_terms("opentopomap"), 2000);
    CHECK(t.find("Free public server run by volunteers. Tiles are saved on this device as you view them. No bulk downloads.") !=
          std::string::npos);
    CHECK(t.find("\n\nMap style CC-BY-SA") != std::string::npos);
    CHECK(t.find("Daily limit in this app: 2,000 requests") != std::string::npos);
    CHECK(provider_terms("thunderforest").rules.find("API key from thunderforest.com") != std::string::npos);
    CHECK(provider_terms("geoapify").rules.find("API key from geoapify.com") != std::string::npos);
    /* an id the table does not know: short generic text, no address, no internal wording */
    const std::string g = terms_text("My Server", "", provider_terms("my-own"), 0);
    CHECK(g.find("My Server is an online map server.") == 0);
    CHECK(g.find("Daily limit") == std::string::npos);
    CHECK(g.find(privacy) != std::string::npos);
    CHECK(g.find("Check the provider's current terms before relying on this.") != std::string::npos);
    CHECK(g.find("http") == std::string::npos);
    CHECK(g.find("054") == std::string::npos && g.find("unverified") == std::string::npos);
}
