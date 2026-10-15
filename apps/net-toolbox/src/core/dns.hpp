/*
 * SPDX-License-Identifier: MIT
 *
 * Net Toolbox: the DNS wire format (RFC 1035, EDNS0 of RFC 6891), pure functions on bytes (no LVGL, no network;
 * tests/test_dns.cpp builds, parses and fuzzes them).
 *
 * The parser never trusts the packet: every read is bounds-checked, a name follows at most 32 compression pointers and
 * each must point backwards (no loop can form), a name is at most 255 bytes, the record counts are bounded by what
 * fits in the packet, and a record whose data does not match its type is kept with "(malformed ...)" as its data
 * rather than failing the whole answer. A packet that cannot be framed gives false and a reason.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace nt {
namespace dns {

enum : std::uint16_t {
    kA = 1, kNS = 2, kCNAME = 5, kSOA = 6, kPTR = 12, kMX = 15, kTXT = 16, kAAAA = 28, kSRV = 33, kOPT = 41, kANY = 255
};
enum : std::uint16_t { kClassIN = 1 };
enum : int { kNoError = 0, kFormErr = 1, kServFail = 2, kNxDomain = 3, kNotImp = 4, kRefused = 5 };

/* "A", "AAAA", "MX", ... "TYPE99" */
std::string type_name(std::uint16_t type);
/* "a", "AAAA", "mx" -> the type; false for an unknown name */
bool type_from_name(const std::string &name, std::uint16_t &type);
/* The types the lookup offers, in order: A, AAAA, MX, TXT, NS, PTR, CNAME */
const std::vector<std::uint16_t> &lookup_types();
/* "NOERROR", "NXDOMAIN", ... "RCODE 11" */
std::string rcode_name(int rcode);
/* the same in plain words: "OK", "no such name", "server failure", "refused", "format error", "not implemented" */
std::string rcode_words(int rcode);

struct Header {
    std::uint16_t id = 0;
    bool qr = false, aa = false, tc = false, rd = false, ra = false, ad = false, cd = false;
    int opcode = 0;
    int rcode = 0;  /* with the EDNS extended bits once parse_message() saw an OPT record */
    std::uint16_t qdcount = 0, ancount = 0, nscount = 0, arcount = 0;
};

/* the 12-byte header only (enough to see TC on a truncated answer) */
bool parse_header(const std::uint8_t *data, std::size_t len, Header &out);

struct Question {
    std::string name;
    std::uint16_t type = 0;
    std::uint16_t cls = 0;
};

enum class Section { Answer, Authority, Additional };

struct Record {
    std::string name;       /* presentation form, no trailing dot ("." for the root) */
    std::uint16_t type = 0;
    std::uint16_t cls = 0;
    std::uint32_t ttl = 0;
    std::string data;       /* "93.184.215.14", "10 mail.example.com", "\"v=spf1 -all\"", ... */
    Section section = Section::Answer;
};

struct Message {
    Header header;
    std::vector<Question> questions;
    std::vector<Record> answers, authority, additional; /* OPT is not listed */
    bool edns = false;
    std::uint16_t udp_size = 0;
};

struct QueryOptions {
    bool rd = true;              /* recursion desired */
    bool edns = true;            /* an OPT record: answers up to udp_size bytes over UDP */
    std::uint16_t udp_size = 1232;
};

/* A query for one name and type. The name must pass valid_hostname() (in-addr.arpa / ip6.arpa names do). */
bool build_query(const std::string &name, std::uint16_t type, std::uint16_t id, const QueryOptions &opt,
                 std::vector<std::uint8_t> &out, std::string *why = nullptr);

/* A whole message (a response, or our own query). */
bool parse_message(const std::uint8_t *data, std::size_t len, Message &out, std::string *why = nullptr);

/* A name at `pos` (compression allowed). *next = the byte after it where it started (not where a pointer led). */
bool read_name(const std::uint8_t *data, std::size_t len, std::size_t pos, std::string &out, std::size_t *next,
               std::string *why = nullptr);

/* case-insensitive, a trailing dot ignored */
bool names_equal(const std::string &a, const std::string &b);

/* the response belongs to our query: same id, QR set, the same question (or no question echoed, as some servers do
 * on an error) */
bool response_matches(const Message &m, std::uint16_t id, const std::string &qname, std::uint16_t type);

/* From qname through the CNAME records of the answer: {qname, alias1, alias2, ...}; at most 16 names, loop-safe. */
std::vector<std::string> cname_chain(const Message &m, const std::string &qname);

} // namespace dns
} // namespace nt
