/*
 * SPDX-License-Identifier: MIT
 *
 * Unit tests: the files (atomic writes, modes, round trips) and the package files.
 */
#include "test_util.hpp"

#include "store.hpp"

#include <cstdlib>
#include <sys/stat.h>
#include <unistd.h>

using namespace ha;

namespace {

unsigned mode_of(const std::string &path)
{
    struct stat st {};
    if (::stat(path.c_str(), &st) != 0) return 0;
    return st.st_mode & 0777;
}

std::string read_text(const std::string &path)
{
    std::string s;
    store::read_file(path, s);
    return s;
}

bool has(const std::string &hay, const std::string &needle)
{
    return hay.find(needle) != std::string::npos;
}

} // namespace

void test_store_group()
{
    char tmpl[] = "/tmp/ha-store-test-XXXXXX";
    const char *base = ::mkdtemp(tmpl);
    CHECK(base != nullptr);
    if (!base) return;
    const std::string dir = std::string(base) + "/config/home-assistant";
    ::umask(022);

    /* nothing yet: defaults */
    CHECK_EQ(store::load_settings(dir).server, std::string(kDefaultServer));
    CHECK(!store::load_credentials(dir).valid());
    CHECK(store::load_favourites(dir).empty());

    Credentials c;
    c.server = "http://homeassistant.local:8123";
    c.client_id = "http://192.168.1.42:41234/";
    c.refresh_token = "refresh \"with\" quotes";
    CHECK(store::save_credentials(dir, c));
    CHECK(mode_of(dir) == 0700);
    CHECK(mode_of(dir + "/credentials.json") == 0600);
    CHECK(::access((dir + "/credentials.json.tmp").c_str(), F_OK) != 0); /* renamed into place */
    const Credentials back = store::load_credentials(dir);
    CHECK(back.valid());
    CHECK_EQ(back.refresh_token, c.refresh_token);
    CHECK_EQ(back.client_id, c.client_id);
    /* a second save keeps 0600 even with a wide umask */
    ::umask(0);
    CHECK(store::save_credentials(dir, c));
    CHECK(mode_of(dir + "/credentials.json") == 0600);
    ::umask(022);
    CHECK(store::delete_credentials(dir));
    CHECK(::access((dir + "/credentials.json").c_str(), F_OK) != 0);
    CHECK(store::delete_credentials(dir)); /* already gone: fine */

    Settings s;
    s.server = "192.168.68.178:8123";
    s.qr_use_ip = true;
    CHECK(store::save_settings(dir, s));
    CHECK_EQ(store::load_settings(dir).server, std::string("192.168.68.178:8123"));
    CHECK(store::load_settings(dir).qr_use_ip);
    CHECK(store::save_favourites(dir, {"light.a", "scene.b"}));
    const auto f = store::load_favourites(dir);
    CHECK(f.size() == 2 && f[1] == "scene.b");
    CHECK(store::load_names(dir).empty());
    CHECK(store::save_names(dir, {{"light.a", "Kitchen \"main\" light"}, {"scene.b", "Evening"}}));
    const auto names = store::load_names(dir);
    CHECK(names.size() == 2);
    CHECK(mode_of(dir + "/names.json") == 0600); /* 0.2.0: the names of the user's home, private like the sign-in */
    CHECK_EQ(names.at("light.a"), std::string("Kitchen \"main\" light"));
    CHECK(store::save_names(dir, {}));
    CHECK(store::load_names(dir).empty());
    /* a damaged file reads as nothing, never crashes */
    CHECK(store::write_atomic(dir + "/favourites.json", "{not json", 0644));
    CHECK(store::load_favourites(dir).empty());
    std::string cmd = std::string("rm -rf ") + base;
    (void)!std::system(cmd.c_str());
}

void test_package_group()
{
    const std::string app = CDK_APP_DIR;
    const std::string desktop = read_text(app + "/root/usr/share/APPLaunch/applications/home-assistant.desktop");
    CHECK(!desktop.empty());
    CHECK(has(desktop, "\nName=Home Assistant\n"));
    CHECK(has(desktop, "\nX-Fullscreen=true"));
    CHECK(has(desktop, "\nX-MinSize=480x320"));
    CHECK(has(desktop, "\nX-Portrait=480x640"));
    CHECK(has(desktop, "\nExec=/usr/share/APPLaunch/bin/home-assistant"));
    CHECK(has(desktop, "\nIcon=share/images/home-assistant.png"));
    CHECK(has(desktop, "\nX-Background=freeze\n"));
    const std::string hpp = read_text(app + "/src/ui/ha_app.hpp");
    CHECK(has(hpp, "kMinW = 480, kMinH = 320"));
    CHECK(has(hpp, "kPortraitW = 480, kPortraitH = 640"));
    CHECK(has(hpp, "kVersion = \"0.3.0\""));
    CHECK(!read_text(app + "/root/usr/share/APPLaunch/share/images/home-assistant.png").empty());
    const std::string copyright = read_text(app + "/root/usr/share/doc/home-assistant/copyright");
    CHECK(has(copyright, "Project Nayuki"));
    CHECK(has(copyright, "License: MIT"));
    /* 0.2.0: Material Design Icons (Apache-2.0), the font and its name table, with the licence text */
    CHECK(has(copyright, "Material Design Icons"));
    CHECK(has(copyright, "License: Apache-2.0"));
    const std::string mdi_licence = read_text(app + "/root/usr/share/doc/home-assistant/LICENSE.mdi");
    CHECK(has(mdi_licence, "Pictogrammers"));
    CHECK(has(mdi_licence, "Apache License"));
    CHECK(has(mdi_licence, "Version 2.0, January 2004"));
    std::string ttf;
    CHECK(store::read_file(app + "/root/usr/share/APPLaunch/share/home-assistant/materialdesignicons-webfont.ttf", ttf,
                           2u << 20));
    CHECK(ttf.size() == 1307660);
    CHECK(read_text(app + "/root/usr/share/APPLaunch/share/home-assistant/mdi-names.bin").rfind("MDN1", 0) == 0);
    CHECK(!read_text(app + "/icon.png").empty());
    const std::string manifest = read_text(app + "/app.json");
    CHECK(has(manifest, "\"share_code\": \"home-assistant\""));
    CHECK(has(manifest, "\"package\": \"home-assistant\""));
    CHECK(has(manifest, "\"version\": \"0.3.0\""));
    CHECK(has(manifest, "\"title\": \"Home Assistant\""));
    CHECK(has(manifest, "\"draft\": true"));
    CHECK(has(manifest, "libcurl4t64"));
    CHECK(has(manifest, "\"arch\": [\"arm64\", \"amd64\"]"));
    CHECK(has(manifest, "\"license\": \"MIT AND Apache-2.0\"")); /* the app MIT, the bundled icon font Apache-2.0 */
}
