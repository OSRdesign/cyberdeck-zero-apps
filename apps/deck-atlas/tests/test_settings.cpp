/*
 * SPDX-License-Identifier: MIT
 *
 * Deck Atlas tests: settings.conf (defaults, a round trip, a tolerant parser), the cycles of each setting, the terms
 * memory, and the atomic write.
 */
#include "check.hpp"
#include "settings.hpp"

#include <cstdlib>
#include <sys/stat.h>

using namespace atlas;

void test_settings()
{
    /* defaults */
    const Settings d = parse_settings("");
    CHECK(d.units == Units::Km);
    CHECK_EQ(d.scale, 0.0f);
    CHECK_EQ(d.cache_mb, 0);
    CHECK(!d.offline);
    CHECK(d.source.empty());
    CHECK(!d.view.set);
    CHECK(d.terms.empty());
    CHECK_EQ(serialize_settings(d), std::string("v=1\nunits=km\nscale=auto\ncache=auto\noffline=off\n"));

    /* a round trip of every key */
    Settings s;
    s.units = Units::Mi;
    s.scale = 1.5f;
    s.cache_mb = 256;
    s.offline = true;
    s.source = "paris-tiles";
    s.view = {true, 48.915912, 2.274123, 15};
    accept_terms(s, "thunderforest");
    accept_terms(s, "opentopomap");
    const std::string text = serialize_settings(s);
    CHECK_EQ(text, std::string("v=1\nunits=mi\nscale=1.5x\ncache=256\noffline=on\nsource=paris-tiles\n"
                               "view=48.915912,2.274123,15\nterms=opentopomap,thunderforest\n"));
    const Settings r = parse_settings(text);
    CHECK(r.units == Units::Mi);
    CHECK_EQ(r.scale, 1.5f);
    CHECK_EQ(r.cache_mb, 256);
    CHECK(r.offline);
    CHECK_EQ(r.source, std::string("paris-tiles"));
    CHECK(r.view.set);
    CHECK_NEAR(r.view.lat, 48.915912, 1e-6);
    CHECK_NEAR(r.view.lon, 2.274123, 1e-6);
    CHECK_EQ(r.view.zoom, 15);
    CHECK_EQ(serialize_settings(r), text);

    /* tolerant: unknown keys, comments, CRLF, spaces and bad values keep the defaults */
    const Settings t = parse_settings("# comment\r\nfuture_key=1\r\nunits = mi \r\nscale=3x\ncache=300\noffline=yes\n"
                                      "source=Bad Id!\nview=91,0,3\nterms=ok-id,BAD,,also.ok\nnot a line\n");
    CHECK(t.units == Units::Mi);
    CHECK_EQ(t.scale, 0.0f);
    CHECK_EQ(t.cache_mb, 0);
    CHECK(!t.offline);
    CHECK(t.source.empty());
    CHECK(!t.view.set);
    CHECK_EQ(t.terms.size(), static_cast<std::size_t>(2));
    CHECK(terms_accepted(t, "ok-id"));
    CHECK(terms_accepted(t, "also.ok"));
    CHECK(!terms_accepted(t, "BAD"));
    CHECK(!parse_settings("view=48.9,2.2,25\n").view.set);
    CHECK(!parse_settings("view=48.9,2.2\n").view.set);
    CHECK(!parse_settings("view=48.9x,2.2,4\n").view.set);
    CHECK(parse_settings("view=-33.86,151.2,10\n").view.set);
    CHECK_EQ(parse_settings("scale=2\n").scale, 2.0f);
    CHECK_EQ(parse_settings("scale=1x\n").scale, 1.0f);
    CHECK_EQ(parse_settings("cache=1024\n").cache_mb, 1024);
    CHECK_EQ(parse_settings("cache=-128\n").cache_mb, 0);

    /* cycles */
    CHECK(next_units(Units::Km) == Units::Mi);
    CHECK(next_units(Units::Mi) == Units::Km);
    CHECK_EQ(next_scale(0.0f), 1.0f);
    CHECK_EQ(next_scale(1.0f), 1.5f);
    CHECK_EQ(next_scale(1.5f), 2.0f);
    CHECK_EQ(next_scale(2.0f), 0.0f);
    CHECK_EQ(next_cache_mb(0), 128);
    CHECK_EQ(next_cache_mb(128), 256);
    CHECK_EQ(next_cache_mb(256), 512);
    CHECK_EQ(next_cache_mb(512), 1024);
    CHECK_EQ(next_cache_mb(1024), 0);
    CHECK_EQ(next_cache_mb(77), 0);
    CHECK_EQ(snap_scale(1.2f), 1.0f);
    CHECK_EQ(snap_scale(1.3f), 1.5f);
    CHECK_EQ(snap_scale(1.8f), 2.0f);
    CHECK_EQ(snap_scale(-1.0f), 0.0f);

    /* terms: asked once per source; accepting twice keeps one entry; the list stays sorted */
    Settings u;
    CHECK(!terms_accepted(u, "geoapify"));
    accept_terms(u, "geoapify");
    accept_terms(u, "geoapify");
    accept_terms(u, "a-first");
    CHECK_EQ(u.terms.size(), static_cast<std::size_t>(2));
    CHECK_EQ(u.terms[0], std::string("a-first"));
    CHECK(terms_accepted(u, "geoapify"));
    accept_terms(u, "Not Valid");
    CHECK_EQ(u.terms.size(), static_cast<std::size_t>(2));

    /* ids */
    CHECK(valid_id("opentopomap"));
    CHECK(valid_id("paris_2.v-3"));
    CHECK(!valid_id(""));
    CHECK(!valid_id("Upper"));
    CHECK(!valid_id("has space"));
    CHECK(!valid_id(std::string(64, 'a')));

    /* the folder and the atomic write */
    const std::string dir = test_temp_dir();
    CHECK(!dir.empty());
    ::setenv("DECKATLAS_CONFIG_DIR", (dir + "/cfg/deck-atlas").c_str(), 1);
    const std::string cfg = config_dir();
    CHECK_EQ(cfg, dir + "/cfg/deck-atlas");
    CHECK(make_dirs(cfg));
    CHECK(write_file_atomic(cfg + "/settings.conf", text));
    std::string back;
    CHECK(read_small_file(cfg + "/settings.conf", back));
    CHECK_EQ(back, text);
    struct stat st;
    CHECK(::stat((cfg + "/settings.conf").c_str(), &st) == 0 && (st.st_mode & 0777) == 0600);
    CHECK(::stat((cfg + "/settings.conf.tmp").c_str(), &st) != 0);
    CHECK(!read_small_file(cfg + "/missing.conf", back));
    CHECK(!read_small_file(cfg + "/settings.conf", back, 10)); /* larger than the cap */
    ::unsetenv("DECKATLAS_CONFIG_DIR");
    ::setenv("XDG_CONFIG_HOME", "/x/cfg", 1);
    CHECK_EQ(config_dir(), std::string("/x/cfg/deck-atlas"));
    ::unsetenv("XDG_CONFIG_HOME");
    test_remove_tree(dir);
}
