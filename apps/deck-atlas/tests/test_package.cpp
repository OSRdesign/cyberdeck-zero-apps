/*
 * SPDX-License-Identifier: MIT
 *
 * Deck Atlas tests: the package files. X-MinSize and X-Portrait of the .desktop file must equal AppSpec's numbers
 * (kMinW etc. in src/ui/atlas_app.hpp, read here as text because the UI needs LVGL); no X-Background (the app does not
 * answer the lifecycle protocol); the manifest's depends carry SQLite, libcurl and the CA certificates; the map engine
 * is switched on for this app only; the icon, the licence file; the OSM Foundation servers appear nowhere as a source.
 */
#include "check.hpp"

#include <string>

namespace {
bool has(const std::string &hay, const std::string &needle) { return hay.find(needle) != std::string::npos; }
} // namespace

void test_package()
{
    const std::string app = test_app_dir();
    const std::string desktop = test_read_file(app + "/root/usr/share/APPLaunch/applications/deck-atlas.desktop");
    CHECK(!desktop.empty());
    CHECK(has(desktop, "\nName=Deck Atlas\n"));
    CHECK(has(desktop, "\nTerminal=false\n"));
    CHECK(has(desktop, "\nX-Fullscreen=true\n"));
    CHECK(has(desktop, "\nX-MinSize=480x320\n"));
    CHECK(has(desktop, "\nX-Portrait=320x480\n"));
    CHECK(!has(desktop, "X-Background"));
    CHECK(has(desktop, "\nExec=/usr/share/APPLaunch/bin/deck-atlas\n"));
    CHECK(has(desktop, "\nIcon=share/images/deck-atlas.png\n"));
    CHECK(!has(desktop, "\r"));
    const std::string hpp = test_read_file(app + "/src/ui/atlas_app.hpp");
    CHECK(has(hpp, "kMinW = 480, kMinH = 320"));
    CHECK(has(hpp, "kPortraitW = 320, kPortraitH = 480"));
    CHECK(has(hpp, "kVersion = \"0.1.0\""));
    const std::string icon = test_read_file(app + "/root/usr/share/APPLaunch/share/images/deck-atlas.png");
    CHECK(!icon.empty());
    CHECK(icon == test_read_file(app + "/icon.png"));
    const std::string copyright = test_read_file(app + "/root/usr/share/doc/deck-atlas/copyright");
    CHECK(has(copyright, "License: MIT"));
    CHECK(has(copyright, "stb_image"));
    const std::string manifest = test_read_file(app + "/app.json");
    CHECK(has(manifest, "\"share_code\": \"deck-atlas\""));
    CHECK(has(manifest, "\"package\": \"deck-atlas\""));
    CHECK(has(manifest, "\"version\": \"0.1.0\""));
    CHECK(has(manifest, "\"title\": \"Deck Atlas\""));
    CHECK(has(manifest, "\"license\": \"MIT\""));
    CHECK(has(manifest, "\"arch\": [\"arm64\", \"amd64\"]"));
    CHECK(has(manifest, "libsqlite3-0"));
    CHECK(has(manifest, "libcurl4t64"));
    CHECK(has(manifest, "ca-certificates"));
    CHECK(!has(manifest, "libc6"));
    /* the engine's switch, for this app only */
    CHECK(has(test_read_file(app + "/cp0_config.mk"), "\nCONFIG_CP0_LVGL_MAP=y\n"));
    /* the user agent is the engine's rule with this app's id */
    CHECK(has(test_read_file(app + "/src/ui/atlas_app.cpp"), "cp0_map_user_agent(ua, sizeof ua, \"deck-atlas\", kVersion)"));
    /* no tile server of the OSM Foundation anywhere in the app's sources */
    for (const char *f : {"/src/ui/atlas_app.cpp", "/src/ui/atlas_sources.cpp", "/src/ui/atlas_import.cpp",
                          "/src/ui/atlas_settings.cpp", "/src/core/source_model.cpp"})
        CHECK(!has(test_read_file(app + f), "tile.openstreetmap.org"));
}
