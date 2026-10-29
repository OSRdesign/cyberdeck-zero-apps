/*
 * SPDX-License-Identifier: MIT
 *
 * The package files of the app: the .desktop file (X-MinSize and X-Portrait must equal AppSpec's numbers, kMinW etc. in
 * src/ui/minefield_app.hpp, read here as text because the UI needs LVGL), the manifest, the icon, the licence file.
 */
#include "test_util.hpp"

#include "fsutil.hpp"

using namespace minefield;

namespace {

std::string read_text(const std::string &path)
{
    std::string s;
    read_file(path, s);
    return s;
}

bool has(const std::string &hay, const std::string &needle)
{
    return hay.find(needle) != std::string::npos;
}

} // namespace

void test_package_group()
{
    const std::string app = CDK_APP_DIR;
    const std::string desktop = read_text(app + "/root/usr/share/APPLaunch/applications/mine-field.desktop");
    CHECK(!desktop.empty());
    CHECK(has(desktop, "\nX-Fullscreen=true"));
    CHECK(has(desktop, "\nX-MinSize=640x480"));
    CHECK(has(desktop, "\nX-Portrait=480x640"));
    CHECK(has(desktop, "\nExec=/usr/share/APPLaunch/bin/mine-field"));
    CHECK(has(desktop, "\nIcon=share/images/mine-field.png"));
    CHECK(has(desktop, "\nX-Background=freeze\n"));
    /* the UI's constants are the same numbers */
    const std::string hpp = read_text(app + "/src/ui/minefield_app.hpp");
    CHECK(has(hpp, "kMinW = 640, kMinH = 480"));
    CHECK(has(hpp, "kPortraitW = 480, kPortraitH = 640"));
    CHECK(has(hpp, "kVersion = \"0.1.0\""));
    CHECK(!read_text(app + "/root/usr/share/APPLaunch/share/images/mine-field.png").empty());
    CHECK(!read_text(app + "/root/usr/share/doc/mine-field/copyright").empty());
    const std::string manifest = read_text(app + "/app.json");
    CHECK(has(manifest, "\"share_code\": \"mine-field\""));
    CHECK(has(manifest, "\"package\": \"mine-field\""));
    CHECK(has(manifest, "\"version\": \"0.1.0\""));
    CHECK(has(manifest, "\"categories\": [\"Games\"]"));
    CHECK(has(manifest, "\"license\": \"MIT\""));
    CHECK(has(manifest, "\"arch\": [\"arm64\", \"amd64\"]"));
    CHECK(!has(manifest, "SKELETON"));
    /* the word of the trade is not used anywhere the user can read */
    CHECK(!has(manifest, "weeper"));
}
