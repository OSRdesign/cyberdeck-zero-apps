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
    /* Bluetooth: Bluetooth needs BlueZ, recommended (as Mesh Hop does), never required */
    CHECK(has(manifest, "\"recommends\": \"bluez\""));
    CHECK(!has(manifest, "\"depends\": \"libfreetype6, libinput10, libxkbcommon0, bluez"));
    /* ... and listen-only by construction too: no D-Bus call that sends or fetches a characteristic value, bonds, marks
     * a device, registers an agent, forgets a device or sets any property (Properties "Set": Powered, Alias, ...);
     * the only thing sent to the module is the notification subscription (AcquireNotify / StartNotify) */
    const char *const ble_files[] = {"ble_sys.hpp", "ble_sys.cpp", "bluez_backend.hpp", "bluez_backend.cpp",
                                     "mux_sys.hpp", "mux_sys.cpp", "demo_ble.hpp", "demo_ble.cpp"};
    const char *const banned[] = {"WriteValue", "ReadValue", "AcquireWrite", "\"Pair\"", "Pair(", "CancelPairing",
                                  "\"Trusted\"", "\"Blocked\"", "\"Set\"", "RegisterAgent", "RequestDefaultAgent",
                                  "RemoveDevice", "::write(", "::send(", "sendmsg", "writev("};
    for (const char *f : ble_files) {
        const std::string text = test_read_file(app + "/src/core/" + f);
        CHECK(!text.empty());
        for (const char *b : banned) {
            const bool found = has(text, b);
            if (found) std::fprintf(stderr, "listen-only: %s contains %s\n", f, b);
            CHECK(!found);
        }
    }
    const std::string bluez = test_read_file(app + "/src/core/bluez_backend.cpp");
    CHECK(has(bluez, "\"AcquireNotify\"")); /* the subscription is there (the grep above is not looking at an empty file) */
    CHECK(has(bluez, "\"StartNotify\""));
    CHECK(has(bluez, "\"StopNotify\""));
    CHECK(has(bluez, "\"Disconnect\""));
    CHECK(has(bluez, "kHlkDataNotify"));
    /* the backend interface offers no write or read of a value at all */
    const std::string iface = test_read_file(app + "/src/core/ble_sys.hpp");
    CHECK(!has(iface, "write("));
    CHECK(!has(iface, "read_value"));
    CHECK(has(iface, "virtual void subscribe(const std::string &address) = 0;"));
}
