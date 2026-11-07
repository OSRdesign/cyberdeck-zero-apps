/*
 * SPDX-License-Identifier: MIT
 *
 * Deck Atlas: online sources as the Sources page shows them (see source_model.hpp).
 */
#include "source_model.hpp"

#include "format.hpp"

#include "cp0_map_common.h"

namespace atlas {

namespace {
std::string counts(std::uint32_t today, std::uint32_t cap)
{
    return count_label(today) + "/" + count_label(cap);
}
} // namespace

StateWord source_state_word(const SourceFacts &f)
{
    if (!f.enabled) return {"Off", Tone::Muted};
    switch (f.state) {
    case CP0_MAP_OK:
        if (f.fetches_today > 0 && f.daily_cap > 0) return {"On " + counts(f.fetches_today, f.daily_cap), Tone::Ok};
        return {"On", Tone::Ok};
    case CP0_MAP_E_NO_KEY: return {"Needs key", Tone::Warn};
    case CP0_MAP_E_DISABLED: return {"Off", Tone::Muted};
    case CP0_MAP_E_OFFLINE: return {"Offline", Tone::Warn};
    case CP0_MAP_E_BLOCKED: return {"Paused by server", Tone::Bad};
    case CP0_MAP_E_CAP_REACHED: return {"Daily cap reached " + counts(f.fetches_today, f.daily_cap), Tone::Warn};
    case CP0_MAP_E_BACKOFF: return {"Server busy", Tone::Warn};
    case CP0_MAP_E_NETWORK: return {"No connection", Tone::Warn};
    case CP0_MAP_E_CURL_MISSING: return {"No libcurl", Tone::Bad};
    case CP0_MAP_E_READ_ONLY: return {"Another app", Tone::Muted};
    case CP0_MAP_E_CACHE_FULL: return {"Cache full", Tone::Warn};
    case CP0_MAP_E_REFUSED: return {"Refused", Tone::Bad};
    case CP0_MAP_E_NO_USER_AGENT: return {"No user agent", Tone::Bad};
    default: return {"Error", Tone::Bad};
    }
}

std::string source_state_sentence(const SourceFacts &f)
{
    if (!f.enabled) return "Switched off: no tile is requested";
    switch (f.state) {
    case CP0_MAP_OK:
        return "On: tiles you look at are fetched and kept (" + count_label(f.fetches_today) + " of " +
               count_label(f.daily_cap) + " requests today)";
    case CP0_MAP_E_NO_KEY: return "Needs your own API key";
    case CP0_MAP_E_OFFLINE: return "Offline mode is on: saved tiles only";
    case CP0_MAP_E_BLOCKED: return "The server sent its 'blocked' picture: paused for an hour, saved tiles only";
    case CP0_MAP_E_CAP_REACHED:
        return "Daily cap reached (" + count_label(f.fetches_today) + " of " + count_label(f.daily_cap) +
               " requests today): saved tiles only until midnight";
    case CP0_MAP_E_BACKOFF: return "The server asked to slow down: retrying later";
    case CP0_MAP_E_NETWORK: return "No connection: saved tiles only, retrying later";
    case CP0_MAP_E_CURL_MISSING: return "Network library missing: install libcurl4t64 (saved tiles still show)";
    case CP0_MAP_E_READ_ONLY: return "Another app updates this source: saved tiles only";
    case CP0_MAP_E_CACHE_FULL: return "The saved-tile cache cannot grow (little free space): saved tiles only";
    case CP0_MAP_E_REFUSED: return "Refused by the tile rules (only HTTPS, never the OpenStreetMap servers)";
    case CP0_MAP_E_NO_USER_AGENT: return "No user agent: nothing is fetched";
    default: return "Error";
    }
}

ToggleStep toggle_step(bool enabled, bool needs_key, bool has_key, bool terms_accepted)
{
    if (enabled) return ToggleStep::Disable;
    if (needs_key && !has_key) return ToggleStep::AskKey;
    if (!terms_accepted) return ToggleStep::AskTerms;
    return ToggleStep::Enable;
}

ToggleStep next_after_key(bool terms_accepted) { return terms_accepted ? ToggleStep::Enable : ToggleStep::AskTerms; }

const char *key_status(bool has_key) { return has_key ? "Key saved" : "No key"; }

std::string trim_key(const std::string &key)
{
    std::size_t a = 0, b = key.size();
    while (a < b && (key[a] == ' ' || key[a] == '\t' || key[a] == '\n' || key[a] == '\r')) ++a;
    while (b > a && (key[b - 1] == ' ' || key[b - 1] == '\t' || key[b - 1] == '\n' || key[b - 1] == '\r')) --b;
    return key.substr(a, b - a);
}

std::string key_error(const std::string &key)
{
    if (key.empty()) return "Type or paste your key";
    if (key.size() < 8 || key.size() > 127) return "A key has 8 to 127 characters";
    for (char c : key) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' ||
                        c == '_' || c == '~' || c == '-';
        if (!ok) return "Only letters, digits and . _ ~ - in a key";
    }
    return std::string();
}

ProviderTerms provider_terms(const std::string &id)
{
    /* the addresses of report 054's sources ([OTM], [TF-terms], [TF-price], [Geoapify-terms], [Geoapify-price]) */
    if (id == "opentopomap") return {"https://opentopomap.org/about", ""};
    if (id == "thunderforest")
        return {"https://www.thunderforest.com/terms/", "A free Hobby key: www.thunderforest.com/pricing/"};
    if (id == "geoapify")
        return {"https://www.geoapify.com/terms-and-conditions/", "A free key: www.geoapify.com/pricing/"};
    return {};
}

std::string cap_line(std::uint32_t daily_cap)
{
    if (daily_cap == 0) return std::string();
    return "At most " + count_label(daily_cap) + " tiles a day from this provider; after that, saved tiles only until midnight.";
}

std::string terms_text(const std::string &note, const std::string &licence, const ProviderTerms &terms,
                       std::uint32_t daily_cap)
{
    std::string s;
    auto para = [&s](const std::string &p) {
        if (p.empty()) return;
        if (!s.empty()) s += "\n\n";
        s += p;
    };
    para(note);
    para(licence);
    if (!terms.terms_url.empty()) para("Terms: " + terms.terms_url);
    para(cap_line(daily_cap));
    para("Only the tiles you look at are requested. A request carries the tile's z/x/y and the app's name and version "
         "(and your key for a source that needs one), nothing else; the provider sees your IP address.");
    return s;
}

} // namespace atlas
