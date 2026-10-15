/*
 * SPDX-License-Identifier: MIT
 *
 * Net Toolbox tests: DNS packets built by hand, and small servers on loopback (a DNS server over UDP and TCP, a TCP
 * server that answers a fixed text) that the client tests talk to. Each server runs one thread with poll() and stops
 * in its destructor.
 */
#pragma once

#include <atomic>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace fake {

using Bytes = std::vector<std::uint8_t>;

Bytes wire_name(const std::string &name); /* uncompressed */
Bytes rd_a(const char *ip);
Bytes rd_aaaa(const char *ip);
Bytes rd_name(const std::string &name);
Bytes rd_mx(std::uint16_t pref, const std::string &name);
Bytes rd_txt(const std::vector<std::string> &strings);

struct Record {
    std::string name;
    std::uint16_t type = 1;
    std::uint32_t ttl = 300;
    Bytes rdata;
};

/* A response to `query`: its id (+ id_delta), its question, the records (a record named like the question gets a
 * compression pointer to it), the code and the TC flag. */
Bytes make_response(const Bytes &query, int rcode, const std::vector<Record> &answers, bool tc = false,
                    int id_delta = 0, const std::vector<Record> &authority = {});

class Dns {
public:
    enum class Mode {
        Answer,         /* the zone's records, NXDOMAIN for a name it does not have */
        Silent,         /* never answers */
        Truncate,       /* UDP: TC and no records; TCP: the full answer */
        FormErrOnEdns,  /* FORMERR to a query with an OPT record, the answer without */
        WrongIdFirst,   /* first an answer with another id (to be ignored), then the right one */
        ServFail
    };
    /* binds UDP and TCP on ip:port (port 0: any free one, the same for both) */
    explicit Dns(const char *ip = "127.0.0.1", std::uint16_t port = 0);
    ~Dns();
    void add(const std::string &name, std::uint16_t type, const Record &r);
    std::uint16_t port() const { return port_; }
    bool ok() const { return udp_ >= 0 && tcp_ >= 0; }
    std::atomic<int> mode{static_cast<int>(Mode::Answer)};
    std::atomic<int> udp_queries{0}, tcp_queries{0};
    void set_mode(Mode m) { mode = static_cast<int>(m); }

private:
    void loop();
    Bytes answer(const Bytes &query, bool over_tcp);
    int udp_ = -1, tcp_ = -1;
    std::uint16_t port_ = 0;
    std::mutex mu_;
    std::map<std::pair<std::string, std::uint16_t>, std::vector<Record>> zone_;
    std::atomic<bool> stop_{false};
    std::thread thread_;
};

/* Accepts TCP connections on 127.0.0.1; to each, after reading what comes (up to a blank line), sends `reply` and
 * closes. */
class Tcp {
public:
    explicit Tcp(std::string reply = "");
    ~Tcp();
    std::uint16_t port() const { return port_; }
    bool ok() const { return fd_ >= 0; }
    std::atomic<int> accepted{0};
    std::string last_request() const;

private:
    void loop();
    int fd_ = -1;
    std::uint16_t port_ = 0;
    std::string reply_;
    mutable std::mutex mu_;
    std::string last_;
    std::atomic<bool> stop_{false};
    std::thread thread_;
};

/* a free TCP port on 127.0.0.1 with nothing listening (bound, then closed) */
std::uint16_t closed_port();

} // namespace fake
