// Host test of the scan backend against a stub nmcli (stub/ first in PATH).
//   g++ -std=c++17 -I../main/ui test_scan.cpp ../main/ui/wifiscan.cpp -lpthread -o test_scan
//   PATH=$PWD/stub:$PATH ./test_scan            (STUB_BUSY=1 for the "scan busy" path)
#include "wifiscan.hpp"
#include <cassert>
#include <chrono>
#include <cstdio>
#include <thread>
int main()
{
    using namespace wifisurvey;
    auto f = split_terse("*:AA\\:BB:Home\\:Net:x");
    assert(f.size() == 4 && f[1] == "AA:BB" && f[2] == "Home:Net");
    Scanner s;
    s.request();
    Snapshot snap;
    for (int i = 0; i < 100; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        snap = s.snapshot();
        if (snap.generation) break;
    }
    std::printf("state=%d iface=%s cached=%d note=%s n=%zu\n", (int)snap.state, snap.iface.c_str(), snap.cached,
                snap.note.c_str(), snap.aps.size());
    for (auto &ap : snap.aps)
        std::printf("  %s|%s|%s|ch%d|%dMHz|%dMb|%d%%|%ddBm|band%d|%s|%s|%s|conn=%d\n", ap.ssid.c_str(), ap.bssid.c_str(),
                    ap.vendor.c_str(), ap.channel, ap.freq_mhz, ap.rate_mbps, ap.signal_pct, ap.dbm(), ap.band(),
                    ap.security.c_str(), ap.security_short().c_str(), ap.hidden() ? "hidden" : "", ap.connected);
    return snap.generation ? 0 : 1;
}
