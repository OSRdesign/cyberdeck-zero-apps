/*
 * SPDX-License-Identifier: MIT
 *
 * Net Toolbox tests: the port check (open, closed, filtered on loopback; the classification; the presets; the session
 * with a name from a hosts file, a cancel) and the minimal HTTP client (204, a redirect, not HTTP, nothing).
 */
#include "check.hpp"
#include "fakes.hpp"

#include "http.hpp"
#include "portcheck.hpp"
#include "sock.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <thread>
#include <vector>

using namespace nt;

namespace {

Address loopback()
{
    Address a;
    parse_ip("127.0.0.1", a);
    return a;
}

/* A port whose accept queue is full: new SYNs are dropped, so a connect waits until its timeout ("filtered"). The
 * listener and the connections that fill its queue live as long as the object. */
class FullBacklog {
public:
    FullBacklog()
    {
        listener_ = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
        sockaddr_in a{};
        a.sin_family = AF_INET;
        inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
        if (listener_ < 0 || bind(listener_, reinterpret_cast<sockaddr *>(&a), sizeof(a)) != 0 || listen(listener_, 0) != 0)
            return;
        socklen_t len = sizeof(a);
        getsockname(listener_, reinterpret_cast<sockaddr *>(&a), &len);
        port_ = ntohs(a.sin_port);
        /* backlog 0: the first connection fills the queue, the SYNs after it are dropped */
        for (int i = 0; i < 2; ++i) {
            Fd fd(socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0));
            if (connect(fd.get(), reinterpret_cast<sockaddr *>(&a), sizeof(a)) == 0 || errno == EINPROGRESS)
                fillers_.push_back(std::move(fd));
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    ~FullBacklog()
    {
        fillers_.clear();
        if (listener_ >= 0) close(listener_);
    }
    std::uint16_t port() const { return port_; }

private:
    int listener_ = -1;
    std::uint16_t port_ = 0;
    std::vector<Fd> fillers_;
};

} // namespace

void test_portcheck()
{
    CancelToken none;
    // the classification
    CHECK(classify_connect(0) == PortState::Open);
    CHECK(classify_connect(ECONNREFUSED) == PortState::Closed);
    CHECK(classify_connect(ETIMEDOUT) == PortState::Filtered);
    CHECK(classify_connect(EHOSTUNREACH) == PortState::Unreachable);
    CHECK(classify_connect(ENETUNREACH) == PortState::Unreachable);
    CHECK(classify_connect(EMFILE) == PortState::Error);
    CHECK_EQ(std::string(port_state_name(PortState::Filtered)), std::string("filtered"));
    // the presets
    bool web = false, mail = false;
    for (const PortPreset &p : port_presets()) {
        if (std::string(p.label) == "Web") web = p.ports == std::vector<std::uint16_t>{80, 443};
        if (std::string(p.label) == "Mail") mail = p.ports == std::vector<std::uint16_t>{25, 465, 587, 993};
        CHECK(!p.ports.empty() && p.ports.size() <= 16);
    }
    CHECK(web && mail);
    CHECK_EQ(std::string(port_service(443)), std::string("https"));
    CHECK_EQ(std::string(port_service(3389)), std::string("rdp"));
    CHECK_EQ(std::string(port_service(4242)), std::string(""));

    // open and closed on loopback
    fake::Tcp server;
    if (!server.ok()) {
        SKIP("cannot listen on 127.0.0.1");
        return;
    }
    PortResult r = check_port(loopback(), server.port(), 1000, none);
    CHECK(r.state == PortState::Open && r.err == 0 && r.connect_ms < 200 && r.detail == "connected");
    const std::uint16_t closed = fake::closed_port();
    r = check_port(loopback(), closed, 1000, none);
    CHECK(r.state == PortState::Closed && r.err == ECONNREFUSED && r.detail == "connection refused");
    // filtered: a listener whose queue is full drops the SYN
    {
        FullBacklog full;
        if (!full.port()) {
            SKIP("cannot make a listener with a full queue");
        } else {
            const std::int64_t t0 = now_ms();
            r = check_port(loopback(), full.port(), 300, none);
            const std::int64_t took = now_ms() - t0;
            if (r.state == PortState::Filtered) {
                CHECK(took >= 290 && took < 700 && r.detail == "timed out" && r.connect_ms == 0);
            } else {
                SKIP("this kernel accepted a connection past a full queue: no filtered port on loopback");
            }
            // a cancel ends the wait within a slice
            Worker w;
            PortResult got;
            w.start([&](const CancelToken &c) { got = check_port(loopback(), full.port(), 5000, c); });
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            const std::int64_t t1 = now_ms();
            w.cancel();
            CHECK(w.wait(1000) && now_ms() - t1 < 300);
            CHECK(got.state == PortState::Pending || got.state == PortState::Open); /* Open: the kernel let it in */
        }
    }

    // the session: a name from the hosts file, an open and a closed port, in order
    {
        const std::string dir = test_tmpdir("ports");
        std::ofstream(dir + "/hosts") << "127.0.0.1 localhost\n";
        PortSession s;
        PortRequest req;
        req.host = "localhost";
        req.ports = {server.port(), closed, server.port()}; /* the repeat is dropped */
        req.timeout_ms = 1000;
        req.resolve.hosts_path = dir + "/hosts";
        CHECK(s.start(req));
        CHECK(s.wait(3000));
        auto snap = s.latest();
        CHECK(snap->state == PortSnapshot::State::Done && snap->address == "127.0.0.1" && snap->resolved_by == "hosts");
        CHECK(snap->results.size() == 2);
        CHECK(snap->results[0].state == PortState::Open && snap->results[1].state == PortState::Closed);
        req.ports.clear();
        CHECK(!s.start(req) && s.latest()->error == "Choose a port (1 to 65535)");
        req.ports = {80};
        req.host = "bad host";
        CHECK(!s.start(req) && s.latest()->state == PortSnapshot::State::Failed);
        req.host = "nope.invalid";
        req.resolve.server = "";
        CHECK(s.start(req) && s.wait(2000));
        CHECK(s.latest()->state == PortSnapshot::State::Failed && s.latest()->error.find("Cannot find nope.invalid") == 0);
        remove_dir(dir);
    }
}

void test_http()
{
    CancelToken none;
    int status = 0;
    std::string location;
    CHECK(parse_http_head("HTTP/1.1 204 No Content\r\nContent-Length: 0\r\n\r\n", status, location) && status == 204);
    CHECK(parse_http_head("HTTP/1.0 302 Found\r\nlocation:  http://portal.lan/login \r\n\r\nbody", status, location));
    CHECK(status == 302 && location == "http://portal.lan/login");
    CHECK(!parse_http_head("SSH-2.0-OpenSSH_9.2\r\n", status, location));
    CHECK(!parse_http_head("HTTP/1.1 2000 Nope\r\n", status, location));
    CHECK(!parse_http_head("HTTP/1.1 099 Low\r\n", status, location));
    CHECK(!parse_http_head("", status, location));

    {
        fake::Tcp server("HTTP/1.1 204 No Content\r\nContent-Length: 0\r\n\r\n");
        if (!server.ok()) {
            SKIP("cannot listen on 127.0.0.1");
            return;
        }
        const HttpResult h = http_get(loopback(), server.port(), "connectivitycheck.gstatic.com", "/generate_204", 2000, none);
        CHECK(h.answered && h.status == 204 && h.location.empty() && h.elapsed_ms < 1000);
        const std::string req = server.last_request();
        CHECK(req.find("GET /generate_204 HTTP/1.1\r\nHost: connectivitycheck.gstatic.com\r\n") == 0);
        CHECK(req.find("Connection: close\r\n") != std::string::npos);
    }
    {
        fake::Tcp portal("HTTP/1.1 302 Found\r\nLocation: http://192.168.4.1/login\r\nContent-Length: 0\r\n\r\n");
        const HttpResult h = http_get(loopback(), portal.port(), "connectivitycheck.gstatic.com", "/generate_204", 2000, none);
        CHECK(h.answered && h.status == 302 && h.location == "http://192.168.4.1/login");
    }
    {
        fake::Tcp junk("SSH-2.0-OpenSSH_9.2p1\r\n\r\n");
        const HttpResult h = http_get(loopback(), junk.port(), "x", "/", 2000, none);
        CHECK(!h.answered && h.error == "an answer that is not HTTP");
    }
    {
        fake::Tcp mute(""); /* accepts and closes */
        const HttpResult h = http_get(loopback(), mute.port(), "x", "/", 2000, none);
        CHECK(!h.answered && !h.error.empty());
    }
    {
        const HttpResult h = http_get(loopback(), fake::closed_port(), "x", "/", 2000, none);
        CHECK(!h.answered && h.err == ECONNREFUSED && h.error == "connection refused");
    }
}
