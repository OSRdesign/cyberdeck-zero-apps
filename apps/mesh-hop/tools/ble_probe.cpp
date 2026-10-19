// SPDX-License-Identifier: MIT
/*
 * ble_probe: the Bluetooth LE link of Mesh Hop (src/core/ble) against a real MeshCore BLE companion, on the deck, from a
 * terminal. The same code as the app will use: BluezBackend (sd-bus loaded at run time), BleLink, BleTransport, the
 * protocol of src/core. Build: tools/build_ble_probe.sh (aarch64 for the deck, or the PC for the mock BlueZ test).
 *
 *   ble_probe info                         the adapter and the devices BlueZ knows (no radio activity)
 *   ble_probe scan [--seconds 8]           an LE discovery (at most 15 s), then the MeshCore boards seen
 *   ble_probe connect AA:BB:CC:DD:EE:FF [--pin 123456] [--listen 0] [--reconnect] [--log]
 *        connects; pairs first when the board is not paired yet (the PIN from --pin, else typed here when BlueZ asks);
 *        sends DEVICE_QUERY and APP_START and prints DEVICE_INFO and SELF_INFO; --listen N prints what the board pushes for
 *        N more seconds (nothing else is sent: no message is taken off the board); with --reconnect a lost link (board
 *        switched off, out of range) is reconnected by the link itself and DEVICE_QUERY is sent again; then disconnects.
 *        The pairing (bond) stays in BlueZ; `bluetoothctl remove AA:BB:CC:DD:EE:FF` forgets it.
 *   --log  prints every step of the link and of BlueZ.
 *
 * Exit status: 0 PASS (SELF_INFO received, or the scan / info ran), 1 FAIL, 2 usage. Run it with Mesh Hop closed.
 */

#include "ble/ble_link.hpp"
#include "ble/ble_transport.hpp"
#include "ble/bluez_backend.hpp"
#include "frame.hpp"
#include "protocol.hpp"

#include <algorithm>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <set>
#include <string>
#include <thread>
#include <variant>

using namespace meshzero;
using namespace meshzero::ble;

namespace {

volatile std::sig_atomic_t g_stop = 0;
uint64_t g_t0 = 0;

uint64_t mono_ms()
{
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}

void sleep_ms(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

std::string stamp()
{
    char b[16];
    std::snprintf(b, sizeof(b), "%7.2f", static_cast<double>(mono_ms() - g_t0) / 1000.0);
    return b;
}

void say(const std::string &s)
{
    std::printf("%s %s\n", stamp().c_str(), s.c_str());
    std::fflush(stdout);
}

const char *code_name(uint8_t c)
{
    switch (c) {
    case 0: return "OK";
    case 1: return "ERROR";
    case 2: return "CONTACT_START";
    case 3: return "CONTACT";
    case 4: return "CONTACT_END";
    case 5: return "SELF_INFO";
    case 6: return "MSG_SENT";
    case 7: return "CONTACT_MSG";
    case 8: return "CHANNEL_MSG";
    case 9: return "CURRENT_TIME";
    case 10: return "NO_MORE_MSGS";
    case 12: return "BATTERY";
    case 13: return "DEVICE_INFO";
    case 16: return "CONTACT_MSG_V3";
    case 17: return "CHANNEL_MSG_V3";
    case 18: return "CHANNEL_INFO";
    case 0x80: return "PUSH_ADVERT";
    case 0x81: return "PUSH_PATH_UPDATE";
    case 0x82: return "PUSH_ACK";
    case 0x83: return "PUSH_MSG_WAITING";
    case 0x88: return "PUSH_LOG_RX_DATA";
    case 0x8A: return "PUSH_NEW_ADVERT";
    default: return "?";
    }
}

const char *cmd_name(uint8_t c)
{
    switch (c) {
    case 1: return "APP_START";
    case 22: return "DEVICE_QUERY";
    default: return "?";
    }
}

void print_frame(const char *dir, const Bytes &f)
{
    const std::string hex = to_hex(f.data(), std::min<size_t>(f.size(), 48));
    const bool out = dir[0] == '-';
    char head[96];
    std::snprintf(head, sizeof(head), "%s %02x %s (%zu bytes) ", dir, f.empty() ? 0 : f[0],
                  f.empty() ? "" : out ? cmd_name(f[0]) : code_name(f[0]), f.size());
    say(head + hex + (f.size() > 48 ? "..." : ""));
}

struct Opts {
    std::string cmd, address;
    int seconds = 8;
    long pin = -1;
    int listen = 0;
    bool reconnect = false;
    bool log = false;
};

int usage()
{
    std::fprintf(stderr,
                 "usage: ble_probe info | scan [--seconds N] | connect AA:BB:CC:DD:EE:FF [--pin 123456] [--listen N] [--reconnect]"
                 "  [--log]\n");
    return 2;
}

void on_signal(int) { g_stop = 1; }

std::function<void(const std::string &)> logger(bool on)
{
    if (!on) return {};
    return [](const std::string &l) { say("  . " + l); };
}

/* ---------------------------------------------------------------- info, scan */

bool wait_ready(BluezBackend &be, BleLink *link, int ms)
{
    const uint64_t until = mono_ms() + static_cast<uint64_t>(ms);
    std::vector<BleEvent> ev;
    while (!be.ready() && mono_ms() < until && !g_stop) {
        if (link) link->tick(mono_ms());
        else {
            ev.clear();
            be.poll(ev);
            for (const BleEvent &e : ev)
                if (e.kind == BleEvent::Kind::Ready && !e.ok) {
                    say("FAIL Bluetooth: " + e.error);
                    return false;
                }
        }
        sleep_ms(20);
    }
    return be.ready();
}

std::string describe(const BleDevice &d)
{
    char b[64];
    std::snprintf(b, sizeof(b), "%s %-6s", d.address.c_str(), d.address_type.c_str());
    std::string s = b;
    s += d.has_rssi ? " rssi " + std::to_string(d.rssi) : " rssi -  ";
    s += std::string(d.paired || d.bonded ? " paired" : "       ") + (d.connected ? " connected" : "          ");
    s += d.nus ? " NUS" : "    ";
    s += "  " + (d.name.empty() ? std::string("(no name)") : d.name);
    return s;
}

int cmd_info(const Opts &o)
{
    BluezBackend be;
    be.set_log(logger(o.log));
    std::string why;
    if (!be.start(why)) {
        say("FAIL Bluetooth: " + why);
        return 1;
    }
    if (!wait_ready(be, nullptr, 5000)) {
        if (!g_stop) say("FAIL BlueZ did not answer");
        return 1;
    }
    say("adapter " + be.adapter_path() + (be.adapter_powered() ? " powered" : " OFF"));
    for (int i = 0; i < 10; ++i) {                       // the agent registration answer
        std::vector<BleEvent> ev;
        be.poll(ev);
        sleep_ms(20);
    }
    say(std::string("agent ") + (be.agent_registered() ? "registered (not the default one)" : "NOT registered"));
    for (const BleDevice &d : be.devices()) say(std::string(is_candidate(d) ? "* " : "  ") + describe(d));
    say("(* = a MeshCore board: the Nordic UART service or the MeshCore- name)");
    be.stop();
    return 0;
}

int cmd_scan(const Opts &o)
{
    BluezBackend be;
    be.set_log(logger(o.log));
    BleTimings t;
    t.list_scan_ms = static_cast<uint64_t>(o.seconds) * 1000;
    BleLink link(be, t);
    link.set_log(logger(o.log));
    std::string why;
    if (!link.start_scan(mono_ms(), &why)) {
        say("FAIL Bluetooth: " + why);
        return 1;
    }
    say("scanning " + std::to_string(o.seconds) + " s (LE, active discovery by BlueZ)");
    std::set<std::string> shown;
    const uint64_t until = mono_ms() + static_cast<uint64_t>(o.seconds) * 1000 + 300;
    while (mono_ms() < until && !g_stop) {
        link.tick(mono_ms());
        for (const BleDevice &d : link.candidates())
            if (d.has_rssi && shown.insert(d.address).second) say("seen  " + describe(d));
        sleep_ms(20);
    }
    link.stop_scan();
    for (int i = 0; i < 10; ++i) {
        link.tick(mono_ms());
        sleep_ms(20);
    }
    const std::vector<BleDevice> c = link.candidates();
    say(std::to_string(c.size()) + " MeshCore board(s) known:");
    for (const BleDevice &d : c) say("  " + describe(d));
    be.stop();
    return 0;
}

/* ---------------------------------------------------------------- connect */

bool wait_frame(BleTransport &t, FrameParser &p, uint8_t want, int ms, Bytes &out)
{
    const uint64_t until = mono_ms() + static_cast<uint64_t>(ms);
    uint8_t buf[512];
    while (mono_ms() < until && !g_stop) {
        const long n = t.read(buf, sizeof(buf));
        if (n < 0) {
            say("link lost: " + t.last_error());
            return false;
        }
        if (n > 0) {
            std::vector<Bytes> frames;
            p.feed(buf, static_cast<size_t>(n), frames);
            for (const Bytes &f : frames) {
                print_frame("<-", f);
                if (!f.empty() && f[0] == want) {
                    out = f;
                    return true;
                }
            }
            continue;
        }
        sleep_ms(10);
    }
    return false;
}

bool send(BleTransport &t, const Bytes &payload)
{
    print_frame("->", payload);
    const Bytes w = encode_frame(payload);
    return t.write(w.data(), w.size());
}

/* Prints what the board pushes for --listen seconds. With --reconnect a lost link is left to BleLink, which reconnects by
 * itself (backoff 2, 4, 8 ... 60 s); each time it is back, DEVICE_QUERY is sent again. Nothing else is sent. */
void listen(BleTransport &t, FrameParser &p, const Opts &o)
{
    say("listening " + std::to_string(o.listen) + " s (nothing is sent" + (o.reconnect ? "; reconnects by itself)" : ")"));
    const uint64_t until = mono_ms() + static_cast<uint64_t>(o.listen) * 1000;
    uint8_t buf[512];
    uint64_t lost_at = 0;
    BleState last = BleState::Connected;
    std::string last_detail;
    while (mono_ms() < until && !g_stop) {
        if (!t.is_open()) {
            t.poll();
            const BleStatus &s = t.link().status();
            if (s.state != last || s.detail != last_detail) {
                say(std::string("state ") + state_name(s.state) + (s.detail.empty() ? "" : ": " + s.detail));
                last = s.state;
                last_detail = s.detail;
            }
            if (s.state == BleState::Failed) {
                say(std::string("gave up: ") + fail_text(s.fail));
                return;
            }
            if (t.open() == OpenStatus::Ok) {
                say("RECONNECTED after " + std::to_string((mono_ms() - lost_at) / 1000) + " s, MTU " + std::to_string(t.link().status().mtu));
                p.reset();
                send(t, build_device_query());
            }
            sleep_ms(20);
            continue;
        }
        const long n = t.read(buf, sizeof(buf));
        if (n < 0) {
            say("link lost: " + t.last_error());
            if (!o.reconnect) return;
            t.close();
            lost_at = mono_ms();
            continue;
        }
        if (n > 0) {
            std::vector<Bytes> frames;
            p.feed(buf, static_cast<size_t>(n), frames);
            for (const Bytes &f : frames) print_frame("<-", f);
            continue;
        }
        sleep_ms(10);
    }
}

int cmd_connect(const Opts &o)
{
    auto owned = std::make_unique<BluezBackend>();
    BluezBackend *be = owned.get();
    be->set_log(logger(o.log));
    BleTransport t(std::move(owned), o.address, "", mono_ms);
    t.link().set_log(logger(o.log));

    say("connecting to " + o.address + " (Mesh Hop must be closed)");
    BleState last = BleState::Idle;
    std::string last_detail;
    uint64_t next_open = 0;
    bool asked = false;
    const uint64_t give_up = mono_ms() + 180000;
    OpenStatus st = OpenStatus::NotFound;
    while (!g_stop && mono_ms() < give_up) {
        t.poll();
        const BleStatus &s = t.link().status();
        if (s.state != last || s.detail != last_detail) {
            say(std::string("state ") + state_name(s.state) + (s.name.empty() ? "" : " [" + s.name + "]") +
                (s.detail.empty() ? "" : ": " + s.detail));
            last = s.state;
            last_detail = s.detail;
        }
        if (s.pin_needed) {
            asked = true;
            long pin = o.pin;
            if (pin < 0) {
                std::printf("The board asks for its PIN (6 digits; 123456 unless the board shows another): ");
                std::fflush(stdout);
                std::string line;
                if (!std::getline(std::cin, line)) line.clear();
                pin = std::strtol(line.c_str(), nullptr, 10);
            }
            say("sending the PIN");
            t.link().submit_pin(static_cast<uint32_t>(pin));
        }
        if (s.state == BleState::Failed) {
            say(std::string("FAIL ") + fail_text(s.fail) + ": " + s.detail);
            return 1;
        }
        if (mono_ms() >= next_open || s.state == BleState::Connected) {
            st = t.open();
            next_open = mono_ms() + 250;
            if (st == OpenStatus::Ok) break;
        }
        sleep_ms(20);
    }
    if (st != OpenStatus::Ok) {
        say(g_stop ? "stopped" : "FAIL not connected after 180 s: " + t.last_error());
        return 1;
    }
    const uint64_t t_conn = mono_ms();
    say("CONNECTED " + t.info().product + ", MTU " + std::to_string(t.link().status().mtu) + ", notify by " +
        (be->notify_acquired() ? "AcquireNotify (socket)" : "StartNotify (PropertiesChanged)") +
        (asked ? ", paired now" : ", already paired"));

    FrameParser p;
    Bytes f;
    bool ok = true;
    if (!send(t, build_device_query()) || !wait_frame(t, p, resp::kDeviceInfo, 5000, f)) {
        say("FAIL no DEVICE_INFO in 5 s");
        ok = false;
    } else if (auto pk = parse_packet(f); pk && std::holds_alternative<DeviceInfo>(*pk)) {
        const DeviceInfo &d = std::get<DeviceInfo>(*pk);
        say("DEVICE_INFO: firmware level " + std::to_string(d.fw_ver) + ", " + d.model + ", " + d.version + " (" + d.fw_build +
            "), contacts max " + std::to_string(d.max_contacts_value()) + ", channels max " + std::to_string(d.max_channels) +
            ", ble_pin setting " + (d.ble_pin ? std::to_string(d.ble_pin) : std::string("0 (default: 123456 or random shown)")));
        say("first answer " + std::to_string(mono_ms() - t_conn) + " ms after the link was up");
    }
    Bytes si;
    if (ok && (!send(t, build_app_start("mhprobe")) || !wait_frame(t, p, resp::kSelfInfo, 5000, si))) {
        say("FAIL no SELF_INFO in 5 s");
        ok = false;
    } else if (ok) {
        if (auto pk = parse_packet(si); pk && std::holds_alternative<SelfInfo>(*pk)) {
            const SelfInfo &s = std::get<SelfInfo>(*pk);
            char radio[96];
            std::snprintf(radio, sizeof(radio), "%.3f MHz, BW %.1f kHz, SF %u, CR %u, TX %d dBm", s.freq_mhz(), s.bw_khz(), s.sf, s.cr,
                          s.tx_power);
            say("SELF_INFO: \"" + s.name + "\", key " + to_hex(s.key.data(), 6) + "..., " + radio);
        }
    }
    if (ok && o.listen > 0) listen(t, p, o);
    const BleLink::Stats &x = t.link().stats();
    say("stats: frames in " + std::to_string(x.frames_in) + ", out " + std::to_string(x.frames_out) + ", joined " +
        std::to_string(x.joined) + ", long writes " + std::to_string(x.long_writes) + ", write errors " + std::to_string(x.write_errors));
    t.shutdown();
    say("disconnected (the pairing stays; `bluetoothctl remove " + o.address + "` forgets it)");
    say(ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}

} // namespace

int main(int argc, char **argv)
{
    g_t0 = mono_ms();
    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);
    Opts o;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&](long &v) {
            if (i + 1 >= argc) return false;
            v = std::strtol(argv[++i], nullptr, 10);
            return true;
        };
        long v = 0;
        if (a == "--seconds" && next(v)) o.seconds = static_cast<int>(v);
        else if (a == "--pin" && next(v)) o.pin = v;
        else if (a == "--listen" && next(v)) o.listen = static_cast<int>(v);
        else if (a == "--log") o.log = true;
        else if (a == "--reconnect") o.reconnect = true;
        else if (a == "-h" || a == "--help") return usage();
        else if (o.cmd.empty()) o.cmd = a;
        else if (o.address.empty()) o.address = a;
        else return usage();
    }
    o.seconds = std::max(1, std::min(o.seconds, 15));          // a short scan: the radio is shared with Wi-Fi and the keyboard
    o.listen = std::max(0, std::min(o.listen, 600));
    if (o.cmd == "info") return cmd_info(o);
    if (o.cmd == "scan") return cmd_scan(o);
    if (o.cmd == "connect" && valid_address(o.address)) return cmd_connect(o);
    return usage();
}
