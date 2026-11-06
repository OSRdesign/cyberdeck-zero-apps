/*
 * SPDX-License-Identifier: MIT
 *
 * mmWave Radar tests: the package files. X-MinSize and X-Portrait of the .desktop file must equal AppSpec's numbers
 * (kMinW etc. in src/ui/radar_app.hpp, read here as text because the UI needs LVGL); the manifest, the icon and the
 * licence file are there; nothing in the sources can write to a port.
 */
#include "check.hpp"

#include <string>

namespace {

std::string app_dir()
{
#ifdef CDK_APP_DIR
    return CDK_APP_DIR;
#else
    return test_data_dir() + "../..";
#endif
}

bool has(const std::string &hay, const std::string &needle) { return hay.find(needle) != std::string::npos; }

} // namespace

void test_package()
{
    const std::string app = app_dir();
    const std::string desktop = test_read_file(app + "/root/usr/share/APPLaunch/applications/mmwave-radar.desktop");
    CHECK(!desktop.empty());
    CHECK(has(desktop, "\nName=mmWave Radar\n"));
    CHECK(has(desktop, "\nTerminal=false\n"));
    CHECK(has(desktop, "\nX-Fullscreen=true\n"));
    CHECK(has(desktop, "\nX-MinSize=480x320\n"));
    CHECK(has(desktop, "\nX-Portrait=320x480\n"));
    CHECK(!has(desktop, "X-Background"));  // the app does not answer the lifecycle protocol (contract section 13)
    CHECK(has(desktop, "\nExec=/usr/share/APPLaunch/bin/mmwave-radar\n"));
    CHECK(has(desktop, "\nIcon=share/images/mmwave-radar.png\n"));
    CHECK(!has(desktop, "\r"));
    const std::string hpp = test_read_file(app + "/src/ui/radar_app.hpp");
    CHECK(has(hpp, "kMinW = 480, kMinH = 320"));
    CHECK(has(hpp, "kPortraitW = 320, kPortraitH = 480"));
    CHECK(has(hpp, "kVersion = \"0.1.0\""));
    CHECK(!test_read_file(app + "/root/usr/share/APPLaunch/share/images/mmwave-radar.png").empty());
    CHECK(test_read_file(app + "/root/usr/share/APPLaunch/share/images/mmwave-radar.png") ==
          test_read_file(app + "/icon.png"));
    const std::string copyright = test_read_file(app + "/root/usr/share/doc/mmwave-radar/copyright");
    CHECK(has(copyright, "License: MIT"));
    const std::string manifest = test_read_file(app + "/app.json");
    CHECK(has(manifest, "\"share_code\": \"mmwave-radar\""));
    CHECK(has(manifest, "\"package\": \"mmwave-radar\""));
    CHECK(has(manifest, "\"version\": \"0.1.0\""));
    CHECK(has(manifest, "\"title\": \"mmWave Radar\""));
    CHECK(has(manifest, "\"license\": \"MIT\""));
    CHECK(has(manifest, "\"arch\": [\"arm64\", \"amd64\"]"));
    /* listen-only by construction: the port interface has no write call (port_sys.hpp) */
    const std::string port = test_read_file(app + "/src/core/port_sys.hpp");
    CHECK(!port.empty());
    CHECK(!has(port, "write("));
    CHECK(!has(test_read_file(app + "/src/core/posix_sys.cpp"), "::write("));
}
