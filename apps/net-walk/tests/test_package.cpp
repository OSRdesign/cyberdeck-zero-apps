/*
 * SPDX-License-Identifier: MIT
 *
 * The package files of the app: the .desktop file (X-MinSize and X-Portrait must equal AppSpec's numbers, kMinW etc. in
 * src/ui/netwalk_app.hpp, read here as text because the UI needs LVGL), the manifest, the icon, the licence file.
 */
#include "test_util.hpp"

#include "fsutil.hpp"

using namespace netwalk;

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
    const std::string desktop = read_text(app + "/root/usr/share/APPLaunch/applications/net-walk.desktop");
    CHECK(!desktop.empty());
    CHECK(has(desktop, "\nX-Fullscreen=true"));
    CHECK(has(desktop, "\nX-MinSize=480x320"));
    CHECK(has(desktop, "\nX-Portrait=480x640"));
    CHECK(has(desktop, "\nExec=/usr/share/APPLaunch/bin/net-walk"));
    CHECK(has(desktop, "\nIcon=share/images/net-walk.png"));
    CHECK(has(desktop, "\nX-Background=freeze\n"));
    /* the UI's constants are the same numbers */
    const std::string hpp = read_text(app + "/src/ui/netwalk_app.hpp");
    CHECK(has(hpp, "kMinW = 480, kMinH = 320"));
    CHECK(has(hpp, "kPortraitW = 480, kPortraitH = 640"));
    CHECK(has(hpp, "kVersion = \"0.1.0\""));
    CHECK(!read_text(app + "/root/usr/share/APPLaunch/share/images/net-walk.png").empty());
    CHECK(!read_text(app + "/root/usr/share/doc/net-walk/copyright").empty());
    CHECK(!read_text(app + "/icon.png").empty());
    const std::string manifest = read_text(app + "/app.json");
    CHECK(has(manifest, "\"share_code\": \"net-walk\""));
    CHECK(has(manifest, "\"package\": \"net-walk\""));
    CHECK(has(manifest, "\"version\": \"0.1.0\""));
    CHECK(has(manifest, "\"title\": \"Net Walk\""));
    CHECK(has(manifest, "\"categories\": [\"Games\"]"));
    CHECK(has(manifest, "\"license\": \"MIT\""));
    CHECK(has(manifest, "\"arch\": [\"arm64\", \"amd64\"]"));
    CHECK(!has(manifest, "SKELETON"));
}
