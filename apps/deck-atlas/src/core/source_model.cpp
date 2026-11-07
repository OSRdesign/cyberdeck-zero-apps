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
    if (id == "opentopomap")
        return {"https://opentopomap.org/about", "",
                "OpenTopoMap is a topographic map with contour lines.",
                "Free public server run by volunteers. Tiles are saved on this device as you view them. No bulk downloads."};
    if (id == "thunderforest")
        return {"https://www.thunderforest.com/terms/", "A free Hobby key: www.thunderforest.com/pricing/",
                "Thunderforest offers cycling, outdoor and transport map styles.",
                "Needs your own API key from thunderforest.com; free plans have a monthly limit. Tiles are saved on this "
                "device as you view them."};
    if (id == "geoapify")
        return {"https://www.geoapify.com/terms-and-conditions/", "A free key: www.geoapify.com/pricing/",
                "Geoapify serves street map tiles.",
                "Needs your own API key from geoapify.com; the free plan has a daily limit."};
    return {};
}

std::string daily_limit_line(std::uint32_t daily_cap)
{
    if (daily_cap == 0) return std::string();
    return "Daily limit in this app: " + count_label(daily_cap) + " requests";
}

std::string terms_text(const std::string &title, const std::string &licence, const ProviderTerms &terms,
                       std::uint32_t daily_cap)
{
    std::string s;
    auto para = [&s](const std::string &p) {
        if (p.empty()) return;
        if (!s.empty()) s += "\n\n";
        s += p;
    };
    if (terms.what.empty())
        para((title.empty() ? std::string("This source") : title) + " is an online map server.\n"
             "Tiles are saved on this device as you view them.");
    else
        para(terms.what + "\n" + terms.rules);
    para(daily_limit_line(daily_cap));
    para("Check the provider's current terms before relying on this." +
         (terms.terms_url.empty() ? std::string() : "\n" + terms.terms_url));
    para(licence);
    return s;
}

} // namespace atlas
