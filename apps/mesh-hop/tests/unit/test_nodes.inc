// 0.5: the TCP transport against a fake companion on the loopback (framing, connect timeout, refused, remote close,
// reconnect, a host name), the Client over TCP, and the node book of the start page. Included by test_core.cpp.

#include "nodes.hpp"
#include "tcp_transport.hpp"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <chrono>
#include <thread>

/* A TCP server on 127.0.0.1 (a free port), driven by hand: accept, read, write, close. */
struct LoopServer {
    int lfd = -1, cfd = -1;
    int port = 0;
    LoopServer()
    {
        lfd = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
        const int on = 1;
        ::setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        ::bind(lfd, reinterpret_cast<sockaddr *>(&a), sizeof(a));
        ::listen(lfd, 4);
        socklen_t l = sizeof(a);
        ::getsockname(lfd, reinterpret_cast<sockaddr *>(&a), &l);
        port = ntohs(a.sin_port);
    }
    ~LoopServer()
    {
        drop();
        if (lfd >= 0) ::close(lfd);
    }
    bool accept_one()
    {
        const int fd = ::accept4(lfd, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (fd < 0) return false;
        drop();                                   // like the firmware: a new client replaces the old one
        cfd = fd;
        return true;
    }
    void drop()
    {
        if (cfd >= 0) ::close(cfd);
        cfd = -1;
    }
    void send_bytes(const Bytes &b) { if (cfd >= 0 && ::send(cfd, b.data(), b.size(), MSG_NOSIGNAL) < 0) {} }
    Bytes recv_all()
    {
        Bytes out;
        uint8_t buf[512];
        while (cfd >= 0) {
            const ssize_t n = ::recv(cfd, buf, sizeof(buf), MSG_DONTWAIT);
            if (n <= 0) break;
            out.insert(out.end(), buf, buf + n);
        }
        return out;
    }
};

static void sleep_a_little(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

/* open() until it is Ok or fails, with the server accepting meanwhile; never one long call */
static OpenStatus open_tcp(TcpTransport &t, LoopServer *srv, int max_ms, int *slowest_ms = nullptr)
{
    OpenStatus s = OpenStatus::NotFound;
    for (int waited = 0; waited < max_ms; waited += 5) {
        const auto t0 = std::chrono::steady_clock::now();
        s = t.open();
        const int took = static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count());
        if (slowest_ms) *slowest_ms = std::max(*slowest_ms, took);
        if (srv) srv->accept_one();
        if (s != OpenStatus::NotFound || t.is_open()) break;
        sleep_a_little(5);
    }
    return s;
}

static void test_tcp_transport()
{
    // the host checks of the IP dialog
    CHECK(valid_host("192.168.1.20") && valid_host("fe80::1") && valid_host("node.lan") && valid_host("mesh-node-1"));
    CHECK(!valid_host("") && !valid_host("192.168.1.") && !valid_host("-node") && !valid_host("node-") && !valid_host("a b") &&
          !valid_host("999.1.1.1") && !valid_host("node..lan") && !valid_host(std::string(254, 'a')));

    // connect, then the companion framing both ways in any chunking ('>' / '<' + u16 length, as over USB)
    {
        LoopServer srv;
        TcpTransport t("127.0.0.1", srv.port, "Kitchen node");
        CHECK(t.kind() == "Network (TCP)" && t.info().path == "tcp:127.0.0.1:" + std::to_string(srv.port) && !t.is_open());
        int slowest = 0;
        CHECK(open_tcp(t, &srv, 2000, &slowest) == OpenStatus::Ok && t.is_open() && slowest < 100);
        sleep_a_little(20);
        srv.accept_one();
        const Bytes q = build_device_query();
        const Bytes wire = encode_frame(q);
        CHECK(t.write(wire.data(), wire.size()));
        sleep_a_little(20);
        CHECK(srv.recv_all() == wire);                                // exactly the serial frame: '<' + length + payload
        const Bytes info = framed(hex(kRealDeviceInfo + 6));
        Bytes stream = Bytes{0x0D, 0x0A} + info + framed(Bytes{resp::kOk});    // junk, then two frames
        for (size_t i = 0; i < stream.size(); i += 7) srv.send_bytes(Bytes(stream.begin() + static_cast<long>(i), stream.begin() + static_cast<long>(std::min(stream.size(), i + 7))));
        sleep_a_little(30);
        FrameParser p;
        std::vector<Bytes> frames;
        uint8_t buf[64];                                              // a small buffer: the frames come over several reads
        for (int i = 0; i < 50; ++i) {
            const long n = t.read(buf, sizeof(buf));
            if (n > 0) p.feed(buf, static_cast<size_t>(n), frames);
            if (n == 0) break;
        }
        CHECK(frames.size() == 2 && frames[0] == hex(kRealDeviceInfo + 6) && frames[1] == Bytes{resp::kOk} && p.junk_bytes() == 2);
        // the node closes the connection: -1 once, the reason in words
        srv.drop();
        sleep_a_little(30);
        long n = 0;
        for (int i = 0; i < 20 && n >= 0; ++i) n = t.read(buf, sizeof(buf));
        CHECK(n == -1 && t.last_error().find("closed the connection") != std::string::npos);
        CHECK(!t.write(wire.data(), wire.size()));
        // the Client closes and opens again: a new connection (the board serves the newest client)
        t.close();
        CHECK(!t.is_open());
        CHECK(open_tcp(t, &srv, 2000) == OpenStatus::Ok && t.is_open() && t.last_failure().empty());
    }
    // nothing listens on the port: refused, as an Error with the reason; the next open() tries again
    {
        int port = 0;
        {
            LoopServer gone;
            port = gone.port;
        }
        TcpTransport t("127.0.0.1", port);
        const OpenStatus s = open_tcp(t, nullptr, 2000);
        CHECK(s == OpenStatus::Error && t.last_error().find("refused") != std::string::npos && t.last_failure().find("refused") != std::string::npos);
        CHECK(!t.is_open());
    }
    // a host that never answers (TEST-NET-1): the connect gives up after its time-out, and no call ever blocks
    {
        TcpTransport t("192.0.2.1", 5000, "", 300);
        int slowest = 0;
        const auto t0 = std::chrono::steady_clock::now();
        const OpenStatus s = open_tcp(t, nullptr, 3000, &slowest);
        const int took = static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count());
        CHECK(s == OpenStatus::Error && took < 2000 && slowest < 100);
        CHECK(t.last_error().find("no answer") != std::string::npos || t.last_error().find("unreachable") != std::string::npos);
    }
    // a host name: looked up on a thread, then connected
    {
        LoopServer srv;
        TcpTransport t("localhost", srv.port);
        int slowest = 0;
        CHECK(open_tcp(t, &srv, 3000, &slowest) == OpenStatus::Ok && slowest < 100);
        TcpTransport bad("no-such-host.invalid", 5000, "", 3000);
        CHECK(open_tcp(bad, nullptr, 5000) == OpenStatus::Error &&
              (bad.last_error().find("unknown host") != std::string::npos || bad.last_error().find("took too long") != std::string::npos));
        TcpTransport junk("not a host", 5000);
        CHECK(junk.open() == OpenStatus::Error);
    }
    // the transport of "no node chosen"
    {
        NullTransport n;
        CHECK(n.open() == OpenStatus::NotFound && !n.is_open() && n.read(nullptr, 0) == -1 && n.last_error() == "No node chosen");
    }
}

/* The Client over TCP against the scripted companion (FakePeer serves the socket): the whole handshake, a message synced. */
static void test_client_over_tcp()
{
    set_time_source(fake_time);
    const std::string dir = tmpdir("tcp-client");
    Model m;
    Store st(dir);
    attach_b(st, m);
    LoopServer srv;
    FakePeer peer;
    peer.messages.push_back(make_chan_msg_v3(0, "Zed: over Wi-Fi"));
    TcpTransport t("127.0.0.1", srv.port, "Kitchen node");
    Client c(t, m);
    c.set_deck_clock([] { return DeckClock::Synced; }, [] {});
    set_time_offset(0);
    bool served = false;
    for (int i = 0; i < 1500 && !(c.link() == Link::Ready && m.message_count() == 1); ++i) {
        g_now += 20;
        t.poll();
        c.poll(g_now);
        if (srv.accept_one()) {
            served = true;
            peer.open();
        }
        const Bytes in = srv.recv_all();
        if (!in.empty()) peer.write(in.data(), in.size());
        uint8_t buf[512];
        long n;
        while ((n = peer.read(buf, sizeof(buf))) > 0) srv.send_bytes(Bytes(buf, buf + n));
        sleep_a_little(1);
    }
    CHECK(served && c.link() == Link::Ready);
    CHECK(m.device() && m.device()->model == "Seeed Xiao-nrf52" && m.self() && m.self()->name == "Deck Zero");
    CHECK(m.contact_count() == 2 && m.message_count() == 1);
    // the node goes away (Wi-Fi lost): the client sees the link drop
    srv.drop();
    for (int i = 0; i < 40 && c.link() == Link::Ready; ++i) {
        g_now += 20;
        t.poll();
        c.poll(g_now);
        sleep_a_little(1);
    }
    CHECK(c.link() != Link::Ready);
}

static void test_node_book()
{
    // keys
    NodeKind k = NodeKind::Usb;
    std::string id;
    CHECK(node_key(NodeKind::Usb, "/dev/ttyACM0") == "usb:/dev/ttyACM0" && parse_node_key("usb:/dev/ttyACM0", k, id) && k == NodeKind::Usb && id == "/dev/ttyACM0");
    CHECK(parse_node_key("ble:EC:EB:45:39:28:2D", k, id) && k == NodeKind::Ble && id == "EC:EB:45:39:28:2D");
    CHECK(parse_node_key("ip:[fe80::1]:5000", k, id) && k == NodeKind::Ip && id == "[fe80::1]:5000");
    CHECK(!parse_node_key("tcp:1.2.3.4", k, id) && !parse_node_key("usb:", k, id));

    // what the user types in the IP dialog
    std::string host;
    int port = 0;
    CHECK(parse_ip_input(" 192.168.1.20 ", 5000, host, port).empty() && host == "192.168.1.20" && port == 5000);
    CHECK(parse_ip_input("192.168.1.20:5001", 5000, host, port).empty() && host == "192.168.1.20" && port == 5001);
    CHECK(parse_ip_input("node.lan:7000", 5000, host, port).empty() && host == "node.lan" && port == 7000);
    CHECK(parse_ip_input("[fe80::1]:5000", 5000, host, port).empty() && host == "fe80::1" && port == 5000);
    CHECK(parse_ip_input("fe80::1", 5000, host, port).empty() && host == "fe80::1" && port == 5000);
    CHECK(parse_ip_input("", 5000, host, port) == "Type the node's IP address or name");
    CHECK(parse_ip_input("192.168.1.20:", 5000, host, port) == "The port is a number from 1 to 65535");
    CHECK(parse_ip_input("192.168.1.20:70000", 5000, host, port) == "The port is a number from 1 to 65535");
    CHECK(parse_ip_input("192.168.1.20:5x", 5000, host, port) == "The port is a number from 1 to 65535");
    CHECK(parse_ip_input("192.168.1", 5000, host, port) == "Not an IP address or host name: 192.168.1");
    CHECK(parse_ip_input("my node", 5000, host, port) == "Not an IP address or host name: my node");
    CHECK(parse_ip_input("[fe80::1", 5000, host, port).rfind("Not an IP address", 0) == 0);
    CHECK(validate_node_name("").empty() && validate_node_name("Kitchen node").empty() && validate_node_name("Küche").empty());
    CHECK(validate_node_name(std::string(33, 'x')) == "At most 32 characters" && validate_node_name("a\tb") == "No control characters");

    // the signal bars: the launcher's rule (percent = 2 x (dBm + 100); bars from 1, 30, 60, 80 %)
    CHECK(signal_bars(-45) == 4 && signal_bars(-60) == 4 && signal_bars(-61) == 3 && signal_bars(-70) == 3 && signal_bars(-71) == 2);
    CHECK(signal_bars(-85) == 2 && signal_bars(-86) == 1 && signal_bars(-99) == 1 && signal_bars(-100) == 0 && signal_bars(-120) == 0);
    CHECK(signal_bars(0) == 0);

    // the book: IP nodes, the node chosen last, the scan answer, the learned names; written and read back
    const std::string dir = tmpdir("nodes");
    CHECK(std::system(("mkdir -p " + dir).c_str()) == 0);
    {
        NodeBook b(dir);
        CHECK(!b.load() && b.ip_nodes().empty() && b.last().empty() && b.ble_scan() == NodeBook::Scan::Ask);
        CHECK(b.add_ip("192.168.1.20", 5000, "Kitchen node").empty());
        CHECK(b.add_ip("192.168.1.20", 5000, "again") == "This node is in the list already");
        CHECK(b.add_ip("fe80::1", 5000, "").empty());
        CHECK(b.add_ip("bad host", 5000, "") == "Not an IP address or host name: bad host" && b.add_ip("1.2.3.4", 0, "") == "The port is a number from 1 to 65535");
        CHECK(b.ip_nodes().size() == 2 && b.ip_nodes()[1].key() == "ip:[fe80::1]:5000" && b.ip_nodes()[0].address() == "192.168.1.20:5000");
        b.set_last("ble:EC:EB:45:39:28:2D");
        b.set_ble_scan(NodeBook::Scan::No);
        b.set_name("usb:/dev/ttyACM0", "OSR wio");
        b.set_name("ble:EC:EB:45:39:28:2D", "OSR\tT1000");          // a tab never reaches the file
        CHECK(b.save());
    }
    {
        NodeBook b(dir);
        CHECK(b.load());
        CHECK(b.ip_nodes().size() == 2 && b.ip_nodes()[0].name == "Kitchen node" && b.ip_nodes()[1].host == "fe80::1");
        CHECK(b.last() == "ble:EC:EB:45:39:28:2D" && b.ble_scan() == NodeBook::Scan::No);
        CHECK(b.name_of("usb:/dev/ttyACM0") == "OSR wio" && b.name_of("ble:EC:EB:45:39:28:2D") == "OSR T1000" && b.name_of("ip:x:1").empty());
        CHECK(b.find_ip("ip:192.168.1.20:5000") && !b.find_ip("ip:192.168.1.21:5000"));
        b.set_last("ip:192.168.1.20:5000");
        CHECK(b.remove_ip("ip:192.168.1.20:5000") && !b.remove_ip("ip:192.168.1.20:5000"));
        CHECK(b.last().empty() && b.ip_nodes().size() == 1);                  // the node chosen last went with it
        b.forget_name("usb:/dev/ttyACM0");
        CHECK(b.name_of("usb:/dev/ttyACM0").empty());
        b.set_ble_scan(NodeBook::Scan::Yes);
        CHECK(b.save());
        NodeBook c(dir);
        CHECK(c.load() && c.ble_scan() == NodeBook::Scan::Yes && c.ip_nodes().size() == 1);
    }
    // a damaged line is skipped, the rest is read
    {
        FILE *f = std::fopen((dir + "/nodes.txt").c_str(), "w");
        std::fputs("# x\nip\tnot a host\t5000\t\nip\t10.0.0.2\t99999\t\nip\t10.0.0.3\t5000\tok\nlast\tnope\nble_scan\tyes\n", f);
        std::fclose(f);
        NodeBook b(dir);
        CHECK(b.load() && b.ip_nodes().size() == 1 && b.ip_nodes()[0].host == "10.0.0.3" && b.last().empty() && b.ble_scan() == NodeBook::Scan::Yes);
    }
}

static void test_nodes()
{
    test_tcp_transport();
    test_client_over_tcp();
    test_node_book();
}
