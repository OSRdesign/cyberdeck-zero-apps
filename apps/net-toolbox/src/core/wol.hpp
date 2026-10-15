/*
 * SPDX-License-Identifier: MIT
 *
 * Net Toolbox 0.3: Wake-on-LAN (no LVGL; tests/test_wol.cpp).
 *
 * The magic packet: 6 bytes 0xFF, then the machine's MAC address 16 times (102 bytes), in one UDP datagram with
 * SO_BROADCAST, no root. Where it goes (plan_wol):
 *   - a machine without a saved broadcast address: on each interface that is up with an IPv4 address (or the one
 *     chosen), to 255.255.255.255 and to the directed broadcast of the interface's subnet (192.168.68.255 for
 *     192.168.68.121/24; none for /31 and /32), on the saved port (9 by default) and, when that is 9, on port 7 too.
 *     The socket is bound to the interface's address, which makes Linux send the limited broadcast out of that
 *     interface (no SO_BINDTODEVICE, which needs a capability);
 *   - a machine with a saved broadcast address (another subnet's, or a router that forwards it): that address, on the
 *     port (and 7 when 9), routed by the system.
 * The result counts the packets sent and the errors, each in words.
 *
 * The wake watch (optional, when the machine has a host to watch): after sending, the ping module's echo socket pings
 * it every watch_interval_ms (1 s) for up to watch_max_ms (120 s): "answers after 23 s", "no answer yet (41 s)", "no
 * answer after 120 s". An answer to the very first ping says the machine was probably awake already. Cancellable
 * within a poll slice.
 *
 * Saved machines (at most kMaxWolMachines) live in the settings file as `wol=name|mac|host|broadcast|port` lines.
 */
#pragma once

#include "dns_client.hpp"
#include "interfaces.hpp"
#include "ping.hpp"
#include "worker.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace nt {

struct MacAddr {
    std::uint8_t b[6] = {};
    std::string text() const; /* "aa:bb:cc:dd:ee:ff" */
    bool operator==(const MacAddr &o) const;
};

/* "aa:bb:cc:dd:ee:ff", "AA-BB-CC-DD-EE-FF", "aabbccddeeff" (blanks around ignored). Refused, with words in *why: any
 * other form or length, mixed separators, a character that is not hex, the all-zero and the broadcast address, a
 * multicast address (the first byte odd: not a network card's). */
bool parse_mac(const std::string &text, MacAddr &out, std::string *why = nullptr);

constexpr std::size_t kMagicPacketSize = 102;
std::vector<std::uint8_t> magic_packet(const MacAddr &mac);

constexpr std::size_t kMaxWolMachines = 20;
constexpr std::size_t kWolNameMax = 32;

struct WolMachine {
    std::string name;         /* 1..32 characters, no control character, no '|' */
    std::string mac;          /* as parse_mac() writes it */
    std::string host;         /* optional: the address or name to watch */
    std::string broadcast;    /* optional: an IPv4 address to send to instead of the interfaces' broadcasts */
    int port = 9;             /* 1 .. 65535 */
};

/* checks a machine and puts it in its stored form (blanks trimmed, the MAC as aa:bb:cc:dd:ee:ff); words in *why */
bool normalize_machine(WolMachine &m, std::string *why = nullptr);
/* the settings value "name|mac|host|broadcast|port", and back (false for anything not valid) */
std::string machine_to_setting(const WolMachine &m);
bool machine_from_setting(const std::string &value, WolMachine &out);

struct WolDestination {
    std::string iface;        /* "" = routed by the system */
    std::string source;       /* the interface's IPv4 address the socket is bound to ("" = any) */
    std::string address;      /* where the packet goes */
    std::uint16_t port = 9;
    std::string label() const; /* "255.255.255.255 port 9 on wlan0" */
};

/* "192.168.68.121/22" -> "192.168.71.255"; false for /31, /32, a bad text or an IPv6 address */
bool directed_broadcast(const std::string &cidr, std::string &out);
/* the packets to send (see above); iface "" = every interface that is up, not loopback, with an IPv4 address */
std::vector<WolDestination> plan_wol(const WolMachine &m, const NetState &net, const std::string &iface = "");

/* sends one datagram; 0 or the errno */
class DatagramSender {
public:
    virtual ~DatagramSender() = default;
    virtual int send(const WolDestination &d, const std::vector<std::uint8_t> &payload) = 0;
};

/* the real one: an IPv4 UDP socket per packet, SO_BROADCAST, bound to d.source when given */
class UdpSender : public DatagramSender {
public:
    int send(const WolDestination &d, const std::vector<std::uint8_t> &payload) override;
};

struct WolPacket {
    WolDestination dest;
    int error = 0;            /* 0 = sent */
    std::string words;        /* "sent", or the error in words */
};

struct WolSendResult {
    int sent = 0;
    int errors = 0;
    std::vector<WolPacket> packets;
    std::string summary;      /* "6 packets sent on wlan0." / "Nothing could be sent: network unreachable." */
};

WolSendResult send_magic(const MacAddr &mac, const std::vector<WolDestination> &plan, DatagramSender &sender);

struct WolRequest {
    WolMachine machine;
    NetState net;             /* the interfaces (read_net_state() of the system) */
    std::string iface;        /* "" = all */
    bool watch = true;        /* ping machine.host after sending, when it has one */
    int watch_interval_ms = 1000; /* 20 .. 10000 */
    int watch_max_ms = 120000;    /* 100 .. 600000 */
    ResolveOptions resolve;   /* for a host that is a name */
};

struct WolSnapshot {
    enum class State { Idle, Sending, Watching, Done, Failed, Cancelled };
    State state = State::Idle;
    std::string machine;      /* its name */
    std::string mac;
    WolSendResult send;
    bool watch = false;       /* a watch runs or ran */
    std::string watch_target; /* the host as saved */
    std::string watch_address;
    int attempts = 0;         /* pings sent */
    int waited_s = 0;         /* since the magic packets */
    bool answered = false;
    double answered_after_s = 0;
    bool answered_at_once = false; /* the first ping answered: probably awake already */
    std::string watch_words;  /* "answers after 23 s", "no answer yet (41 s)", "no answer after 120 s", "cannot watch: ..." */
    std::string error;        /* Failed: why nothing was sent */
    std::string hint;         /* no answer: what Wake-on-LAN needs */
    bool running() const { return state == State::Sending || state == State::Watching; }
};

constexpr const char *kWolHint =
    "Wake-on-LAN works only when the machine is set up for it (in its BIOS or UEFI and its network card's settings), "
    "usually over wired Ethernet; machines on Wi-Fi rarely wake up.";

/* The whole tool on the calling thread (the session's worker; tests call it directly). */
WolSnapshot run_wol(const WolRequest &req, DatagramSender &sender, const EchoFactory &echo, const CancelToken &cancel,
                    const std::function<void(const WolSnapshot &)> &publish);

class WolSession {
public:
    WolSession(); /* UdpSender and the ICMP echo socket */
    WolSession(std::shared_ptr<DatagramSender> sender, EchoFactory echo);
    bool start(const WolRequest &req); /* LVGL thread; a running one is stopped first */
    void stop();
    bool running() const { return worker_.running(); }
    std::uint64_t seq() const { return channel_.seq(); }
    std::shared_ptr<const WolSnapshot> latest() const { return channel_.latest(); }
    bool wait(int timeout_ms) const { return worker_.wait(timeout_ms); } /* tests */

private:
    std::shared_ptr<DatagramSender> sender_;
    EchoFactory echo_;
    Channel<WolSnapshot> channel_;
    Worker worker_;
};

} // namespace nt
