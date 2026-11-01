/*
 * SPDX-License-Identifier: MIT
 *
 * Integration test: the app's real client (src/core: the connection thread, libcurl's WebSocket, the HTTP calls, the
 * pairing listener, the files) against the fake Home Assistant of tests/fake_ha (run.sh starts it, with 2,000 entities,
 * 20 areas, messages cut into 1 KB frames and 40 s access tokens). The test plays the phone: it opens the QR code's
 * address on the fake server, which answers like Home Assistant after a login, and follows the redirect to the deck.
 *
 *   ha-integration <fake port> <work dir>
 *
 * Steps: pairing and sign-in (credentials file 0600) -> registry (2,000 entries) -> 100 favourites subscribed ->
 * service calls (light, switch, input_boolean, scene, script, lock, cover, brightness) and their states -> a change
 * made at the wall -> a WebSocket ping from the server (libcurl's automatic pong) -> a 5 MB message (dropped, the link
 * stays) -> a TCP reset (reconnects, subscribes again) -> an access token refused by the server (one refresh, connected)
 * -> the token refreshed before its end while connected -> Sign out (revoked on the server, file gone, Connect page)
 * -> a second pairing -> the sign-in deleted on the server (refused, file gone). Memory (VmRSS, VmHWM) at each stage.
 * Never prints a token (run.sh checks the logs against every token the fake issued).
 */
#include "net.hpp"
#include "runner.hpp"
#include "store.hpp"
#include "text.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>

using namespace ha;

namespace {

int g_fail = 0, g_pass = 0;
std::string g_base;

#define EXPECT(c, what)                                                                                                \
    do {                                                                                                               \
        if (c) {                                                                                                       \
            ++g_pass;                                                                                                  \
            std::printf("  ok    %s\n", what);                                                                         \
        } else {                                                                                                       \
            ++g_fail;                                                                                                  \
            std::printf("  FAIL  %s (line %d)\n", what, __LINE__);                                                     \
        }                                                                                                              \
    } while (0)

long status_kb(const char *key)
{
    FILE *f = std::fopen("/proc/self/status", "r");
    char line[256];
    long v = -1;
    const std::size_t n = std::strlen(key);
    while (f && std::fgets(line, sizeof line, f))
        if (std::strncmp(line, key, n) == 0 && line[n] == ':') {
            v = std::atol(line + n + 1);
            break;
        }
    if (f) std::fclose(f);
    return v;
}

void memory(const char *stage)
{
    std::printf("  mem   %-46s VmRSS %6ld kB   VmHWM %6ld kB\n", stage, status_kb("VmRSS"), status_kb("VmHWM"));
}

bool wait_for(ClientBackend &b, const std::function<bool(const View &)> &pred, int ms)
{
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (std::chrono::steady_clock::now() < end) {
        if (pred(b.snapshot())) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return pred(b.snapshot());
}

std::string get(const std::string &path)
{
    return http_request(g_base + path, nullptr, 5000, nullptr).body;
}

long stat_of(const std::string &key)
{
    const std::string s = get("/fake/stats");
    const std::size_t at = s.find("\"" + key + "\":");
    return at == std::string::npos ? -1 : std::atol(s.c_str() + at + key.size() + 3);
}

/* the phone: open the QR code's address on the fake server, follow its redirect to the deck */
bool scan(const View &v)
{
    const std::size_t q = v.qr_url.find("/auth/authorize?");
    if (q == std::string::npos) return false;
    const HttpResult r = http_request(g_base + v.qr_url.substr(q), nullptr, 5000, nullptr);
    if (r.status != 302) return false;
    const HttpResult back = http_request(r.body, nullptr, 5000, nullptr); /* the deck's listener */
    return back.status == 200 && back.body.find("You can close this page") != std::string::npos;
}

unsigned file_mode(const std::string &path)
{
    struct stat st {};
    return ::stat(path.c_str(), &st) == 0 ? (st.st_mode & 0777) : 0;
}

const EntityRow *fav(const View &v, const std::string &id)
{
    for (const EntityRow &r : v.favourites)
        if (r.id == id) return &r;
    return nullptr;
}

} // namespace

int main(int argc, char **argv)
{
    if (argc < 3) {
        std::fprintf(stderr, "usage: ha-integration <fake port> <work dir>\n");
        return 2;
    }
    const std::string port = argv[1];
    const std::string dir = std::string(argv[2]) + "/config";
    g_base = "http://127.0.0.1:" + port;
    setenv("HA_CONFIG_DIR", dir.c_str(), 1);
    std::setvbuf(stdout, nullptr, _IOLBF, 0);
    memory("start (no libcurl yet)");
    std::string why;
    if (!http_request(g_base + "/fake/stats", nullptr, 3000, nullptr).status) {
        std::printf("the fake server does not answer on %s\n", g_base.c_str());
        return 2;
    }
    memory("libcurl loaded");

    Settings settings;
    settings.server = "127.0.0.1:" + port;
    std::vector<std::string> favourites;
    {
        std::puts("== 1. pairing by the QR code");
        ClientBackend b(settings, Credentials(), favourites, dir);
        EXPECT(wait_for(b, [](const View &v) { return !v.qr_url.empty(); }, 5000), "QR code shown (server checked, listener open)");
        View v = b.snapshot();
        EXPECT(v.qr_url.find("client_id=http%3A%2F%2F127.0.0.1%3A") != std::string::npos, "client id on the deck's address");
        EXPECT(v.pair_status == "Waiting for the phone...", "status: Waiting for the phone...");
        EXPECT(stat_of("providers") == 1, "one unauthenticated /auth/providers check");
        EXPECT(scan(v), "the phone came back to the deck with a code");
        EXPECT(wait_for(b, [](const View &v) { return v.paired_now && v.conn == Conn::Connected; }, 8000),
               "signed in and connected: \"Connected as deck\"");
        v = b.snapshot();
        EXPECT(v.pair_status == "Connected as deck", "status line names the user");
        EXPECT(file_mode(dir + "/credentials.json") == 0600, "credentials file mode 0600");
        EXPECT(file_mode(dir) == 0700, "config folder mode 0700");
        EXPECT(store::load_credentials(dir).valid(), "credentials file complete (server, client id, refresh token)");
        EXPECT(stat_of("exchanges") == 1 && stat_of("refreshes") == 0, "one code exchange, no refresh needed");

        std::puts("== 2. registry and 100 favourites");
        EXPECT(wait_for(b, [](const View &v) { return v.registry_loaded && v.rooms.size() >= 20; }, 8000), "areas, devices, 2000 entities loaded");
        memory("registry of 2000 entities");
        v = b.snapshot();
        std::printf("  info  %zu rooms, largest message %zu bytes, %ld frames sent by the fake so far\n", v.rooms.size(),
                    b.largest_message(), stat_of("frames_out"));
        int fav_n = 0;
        for (int i = 0; i < 2000 && fav_n < 100; ++i) {
            static const char *doms[] = {"light", "switch", "sensor", "binary_sensor", "input_boolean", "scene", "script",
                                         "cover", "lock", "climate", "media_player", "automation", "update", "person"};
            const char *d = doms[i % 14];
            if (i % 14 >= 10) continue; /* only the phase 1 domains */
            char id[64];
            std::snprintf(id, sizeof id, "%s.fake_%04d", d, i);
            b.send(Command::of(Command::Kind::ToggleFavourite, id));
            ++fav_n;
        }
        EXPECT(wait_for(b, [](const View &v) {
                   if (v.favourites.size() != 100) return false;
                   for (const EntityRow &r : v.favourites)
                       if (r.value.empty() && r.domain != Domain::Scene && r.domain != Domain::Script) return false;
                   return true;
               }, 8000),
               "100 favourites, each with its live state");
        memory("2000 registry + 100 subscribed");
        EXPECT(store::load_favourites(dir).size() == 100, "favourites saved");
        EXPECT(wait_for(b, [&dir](const View &) {
                   const auto names = store::load_names(dir);
                   auto it = names.find("light.fake_0000");
                   return names.size() == 100 && it != names.end() && it->second == "Fake Light 0";
               }, 4000),
               "the favourites' names cached (names.json, the states' friendly names)");
        EXPECT(stat_of("subscribes") >= 1, "subscribe_entities sent");

        std::puts("== 3. service calls");
        b.send(Command::of(Command::Kind::Toggle, "light.fake_0000")); /* off -> on */
        EXPECT(wait_for(b, [](const View &v) { const EntityRow *r = fav(v, "light.fake_0000"); return r && r->value == "On"; }, 4000),
               "light.turn_on -> On");
        b.send(Command::of(Command::Kind::SetBrightness, "light.fake_0000", "", 40));
        b.send(Command::of(Command::Kind::Toggle, "switch.fake_0001")); /* on -> off */
        EXPECT(wait_for(b, [](const View &v) { const EntityRow *r = fav(v, "switch.fake_0001"); return r && r->value == "Off"; }, 4000),
               "switch.toggle -> Off");
        b.send(Command::of(Command::Kind::Toggle, "input_boolean.fake_0004"));
        b.send(Command::of(Command::Kind::Run, "scene.fake_0005"));
        EXPECT(wait_for(b, [](const View &v) { const EntityRow *r = fav(v, "scene.fake_0005"); return r && r->done; }, 4000),
               "scene.turn_on -> Done");
        b.send(Command::of(Command::Kind::Run, "script.fake_0006"));
        b.send(Command::of(Command::Kind::Service, "lock.fake_0008", "unlock"));
        EXPECT(wait_for(b, [](const View &v) { const EntityRow *r = fav(v, "lock.fake_0008"); return r && r->value == "Unlocked"; }, 4000),
               "lock.unlock -> Unlocked");
        b.send(Command::of(Command::Kind::Service, "cover.fake_0007", "open_cover"));
        EXPECT(wait_for(b, [](const View &v) { const EntityRow *r = fav(v, "cover.fake_0007"); return r && r->value == "Open"; }, 4000),
               "cover.open_cover -> Open");
        const std::string calls = get("/fake/calls");
        EXPECT(calls.find("\"domain\": \"light\", \"service\": \"turn_on\", \"entity_id\": \"light.fake_0000\", \"data\": {\"brightness_pct\": 40}") != std::string::npos,
               "light.turn_on with brightness_pct 40 reached the server");
        EXPECT(calls.find("\"script\", \"service\": \"turn_on\"") != std::string::npos, "script.turn_on reached the server");
        EXPECT(calls.find("\"input_boolean\", \"service\": \"toggle\"") != std::string::npos, "input_boolean.toggle reached the server");
        get("/fake/state?entity=switch.fake_0001&s=on");
        EXPECT(wait_for(b, [](const View &v) { const EntityRow *r = fav(v, "switch.fake_0001"); return r && r->value == "On"; }, 4000),
               "a change made at the wall switch shows");

        std::puts("== 4. pings, a 5 MB message, a reset link");
        get("/fake/wsping");
        EXPECT(wait_for(b, [](const View &) { return stat_of("pongs") >= 1; }, 3000), "the server's WebSocket ping answered (libcurl)");
        get("/fake/huge");
        std::this_thread::sleep_for(std::chrono::milliseconds(1500));
        EXPECT(b.snapshot().conn == Conn::Connected, "a 5 MB message is dropped, the link stays");
        memory("after the 5 MB message");
        const long connects = stat_of("ws_connects");
        get("/fake/rst");
        EXPECT(wait_for(b, [](const View &v) { return v.conn != Conn::Connected; }, 3000), "TCP reset seen (Reconnecting)");
        EXPECT(wait_for(b, [](const View &v) { return v.conn == Conn::Connected; }, 6000), "reconnected after the back-off");
        EXPECT(stat_of("ws_connects") == connects + 1, "one new connection");
        EXPECT(wait_for(b, [](const View &v) { const EntityRow *r = fav(v, "lock.fake_0008"); return r && r->value == "Unlocked" && !r->muted; }, 4000),
               "subscribed again, live values back");

        std::puts("== 5. the token refreshed before its end, while connected");
        const long r0 = stat_of("refreshes");
        EXPECT(wait_for(b, [r0](const View &) { return stat_of("refreshes") > r0; }, 40000), "a refresh 10 s before the 40 s token ends");
        EXPECT(b.snapshot().conn == Conn::Connected, "the link stayed up during the refresh");

        std::puts("== 6. an access token the server no longer takes");
        std::this_thread::sleep_for(std::chrono::milliseconds(11000)); /* older than a quarter of its 40 s */
        const long refreshes = stat_of("refreshes");
        get("/fake/expire");
        get("/fake/rst");
        EXPECT(wait_for(b, [refreshes](const View &v) { return v.conn == Conn::Connected && stat_of("refreshes") == refreshes + 1; }, 8000),
               "auth_invalid -> one refresh (refresh_token grant) -> connected");
        EXPECT(stat_of("auth_invalid") == 1, "exactly one refused sign-in");
        EXPECT(b.snapshot().signed_in, "still signed in");

        std::puts("== 7. sign out");
        b.send(Command::of(Command::Kind::SignOut));
        EXPECT(wait_for(b, [](const View &v) { return !v.signed_in; }, 5000), "signed out");
        EXPECT(stat_of("revokes") == 1, "/auth/revoke called once");
        EXPECT(stat_of("refresh_tokens") == 0, "the server holds no refresh token of the deck any more");
        EXPECT(::access((dir + "/credentials.json").c_str(), F_OK) != 0, "credentials file deleted");
        EXPECT(wait_for(b, [](const View &v) { return !v.qr_url.empty(); }, 5000), "the Connect page shows a new QR code");

        std::puts("== 8. a second pairing, then the sign-in deleted on the server");
        EXPECT(scan(b.snapshot()), "the phone scanned again");
        EXPECT(wait_for(b, [](const View &v) { return v.signed_in && v.conn == Conn::Connected; }, 8000), "signed in again");
        get("/fake/revoke_all");
        EXPECT(wait_for(b, [](const View &v) { return !v.signed_in; }, 15000), "refused after the server deleted the sign-in");
        const View after = b.snapshot();
        EXPECT(after.pair_tone == Tone::Error, "the Connect page says why, in red");
        std::printf("  info  status line: \"%s\"\n", after.pair_status.c_str());
        EXPECT(::access((dir + "/credentials.json").c_str(), F_OK) != 0, "credentials file deleted");
        memory("end");
        std::printf("  info  largest message %zu bytes; fake: %ld frames out, max message %ld bytes\n", b.largest_message(),
                    stat_of("frames_out"), stat_of("max_message_out"));
    }
    std::printf("%d checks passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
