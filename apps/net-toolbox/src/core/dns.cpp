/*
 * SPDX-License-Identifier: MIT
 *
 * Net Toolbox: the DNS wire format (see dns.hpp).
 */
#include "dns.hpp"

#include "netaddr.hpp"
#include "text.hpp"

#include <arpa/inet.h>
#include <sys/socket.h>

#include <cstdio>
#include <cstring>

namespace nt {
namespace dns {
namespace {

constexpr int kMaxPointerHops = 32;
constexpr std::size_t kMaxNameBytes = 255; /* wire length, labels and their length bytes */
constexpr std::size_t kMaxRecords = 512;

bool fail(std::string *why, const char *text)
{
    if (why) *why = text;
    return false;
}

std::uint16_t get16(const std::uint8_t *p)
{
    return static_cast<std::uint16_t>((p[0] << 8) | p[1]);
}

std::uint32_t get32(const std::uint8_t *p)
{
    return (static_cast<std::uint32_t>(p[0]) << 24) | (static_cast<std::uint32_t>(p[1]) << 16) |
           (static_cast<std::uint32_t>(p[2]) << 8) | p[3];
}

void put16(std::vector<std::uint8_t> &out, std::uint16_t v)
{
    out.push_back(static_cast<std::uint8_t>(v >> 8));
    out.push_back(static_cast<std::uint8_t>(v & 0xFF));
}

/* a label byte in presentation form: letters, digits, '-', '_' as they are; anything else \DDD */
void append_label_byte(std::string &out, std::uint8_t c)
{
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '*' ||
        c == '/') {
        out += static_cast<char>(c);
        return;
    }
    char buf[8];
    std::snprintf(buf, sizeof(buf), "\\%03u", c);
    out += buf;
}

/* a TXT character-string: printable ASCII as is, '"' and '\' escaped, the rest \DDD */
std::string quote_string(const std::uint8_t *p, std::size_t n)
{
    std::string out = "\"";
    for (std::size_t i = 0; i < n; ++i) {
        const std::uint8_t c = p[i];
        if (c == '"' || c == '\\') {
            out += '\\';
            out += static_cast<char>(c);
        } else if (c >= 0x20 && c < 0x7F) {
            out += static_cast<char>(c);
        } else {
            char buf[8];
            std::snprintf(buf, sizeof(buf), "\\%03u", c);
            out += buf;
        }
    }
    return out + "\"";
}

std::string malformed(std::size_t rdlen)
{
    return "(malformed, " + std::to_string(rdlen) + " bytes)";
}

/* a name inside the record data: it must start and (where it started) end within [pos, end) */
bool rdata_name(const std::uint8_t *data, std::size_t len, std::size_t pos, std::size_t end, std::string &out,
                std::size_t *next)
{
    std::size_t after = 0;
    if (pos >= end || !read_name(data, len, pos, out, &after)) return false;
    if (after > end) return false;
    if (next) *next = after;
    return true;
}

std::string format_rdata(const std::uint8_t *data, std::size_t len, std::uint16_t type, std::size_t pos,
                         std::size_t rdlen)
{
    const std::size_t end = pos + rdlen;
    const std::uint8_t *p = data + pos;
    std::string name;
    std::size_t next = 0;
    switch (type) {
    case kA: {
        if (rdlen != 4) return malformed(rdlen);
        char buf[INET_ADDRSTRLEN] = {};
        inet_ntop(AF_INET, p, buf, sizeof(buf));
        return buf;
    }
    case kAAAA: {
        if (rdlen != 16) return malformed(rdlen);
        char buf[INET6_ADDRSTRLEN] = {};
        inet_ntop(AF_INET6, p, buf, sizeof(buf));
        return buf;
    }
    case kNS:
    case kCNAME:
    case kPTR:
        if (!rdata_name(data, len, pos, end, name, &next) || next != end) return malformed(rdlen);
        return name;
    case kMX: {
        if (rdlen < 3) return malformed(rdlen);
        const std::uint16_t pref = get16(p);
        if (!rdata_name(data, len, pos + 2, end, name, &next) || next != end) return malformed(rdlen);
        return std::to_string(pref) + " " + name;
    }
    case kSRV: {
        if (rdlen < 7) return malformed(rdlen);
        const std::string head = std::to_string(get16(p)) + " " + std::to_string(get16(p + 2)) + " " +
                                 std::to_string(get16(p + 4)) + " ";
        if (!rdata_name(data, len, pos + 6, end, name, &next) || next != end) return malformed(rdlen);
        return head + name;
    }
    case kTXT: {
        std::string out;
        std::size_t i = 0;
        if (rdlen == 0) return malformed(rdlen);
        while (i < rdlen) {
            const std::size_t n = p[i];
            if (i + 1 + n > rdlen) return malformed(rdlen);
            if (!out.empty()) out += ' ';
            out += quote_string(p + i + 1, n);
            i += 1 + n;
        }
        return out;
    }
    case kSOA: {
        std::string mname, rname;
        if (!rdata_name(data, len, pos, end, mname, &next)) return malformed(rdlen);
        if (!rdata_name(data, len, next, end, rname, &next)) return malformed(rdlen);
        if (end - next != 20) return malformed(rdlen);
        const std::uint8_t *q = data + next;
        return mname + " " + rname + " " + std::to_string(get32(q)) + " " + std::to_string(get32(q + 4)) + " " +
               std::to_string(get32(q + 8)) + " " + std::to_string(get32(q + 12)) + " " + std::to_string(get32(q + 16));
    }
    default: {
        /* RFC 3597 generic form, the first 32 bytes */
        std::string out = "\\# " + std::to_string(rdlen);
        if (rdlen) out += ' ';
        static const char hex[] = "0123456789abcdef";
        for (std::size_t i = 0; i < rdlen && i < 32; ++i) {
            out += hex[p[i] >> 4];
            out += hex[p[i] & 0xF];
        }
        if (rdlen > 32) out += "...";
        return out;
    }
    }
}

} // namespace

std::string type_name(std::uint16_t type)
{
    switch (type) {
    case kA: return "A";
    case kNS: return "NS";
    case kCNAME: return "CNAME";
    case kSOA: return "SOA";
    case kPTR: return "PTR";
    case kMX: return "MX";
    case kTXT: return "TXT";
    case kAAAA: return "AAAA";
    case kSRV: return "SRV";
    case kOPT: return "OPT";
    case kANY: return "ANY";
    default: return "TYPE" + std::to_string(type);
    }
}

bool type_from_name(const std::string &name, std::uint16_t &type)
{
    const std::string n = lower(trim(name));
    static const std::uint16_t all[] = {kA, kNS, kCNAME, kSOA, kPTR, kMX, kTXT, kAAAA, kSRV};
    for (const std::uint16_t t : all)
        if (lower(type_name(t)) == n) {
            type = t;
            return true;
        }
    return false;
}

const std::vector<std::uint16_t> &lookup_types()
{
    static const std::vector<std::uint16_t> types = {kA, kAAAA, kMX, kTXT, kNS, kPTR, kCNAME};
    return types;
}

std::string rcode_name(int rcode)
{
    switch (rcode) {
    case kNoError: return "NOERROR";
    case kFormErr: return "FORMERR";
    case kServFail: return "SERVFAIL";
    case kNxDomain: return "NXDOMAIN";
    case kNotImp: return "NOTIMP";
    case kRefused: return "REFUSED";
    case 6: return "YXDOMAIN";
    case 9: return "NOTAUTH";
    case 16: return "BADVERS";
    default: return "RCODE " + std::to_string(rcode);
    }
}

std::string rcode_words(int rcode)
{
    switch (rcode) {
    case kNoError: return "OK";
    case kFormErr: return "format error";
    case kServFail: return "server failure";
    case kNxDomain: return "no such name";
    case kNotImp: return "not implemented";
    case kRefused: return "refused";
    default: return "error " + std::to_string(rcode);
    }
}

bool parse_header(const std::uint8_t *data, std::size_t len, Header &h)
{
    if (!data || len < 12) return false;
    h.id = get16(data);
    const std::uint16_t flags = get16(data + 2);
    h.qr = flags & 0x8000;
    h.opcode = (flags >> 11) & 0xF;
    h.aa = flags & 0x0400;
    h.tc = flags & 0x0200;
    h.rd = flags & 0x0100;
    h.ra = flags & 0x0080;
    h.ad = flags & 0x0020;
    h.cd = flags & 0x0010;
    h.rcode = flags & 0xF;
    h.qdcount = get16(data + 4);
    h.ancount = get16(data + 6);
    h.nscount = get16(data + 8);
    h.arcount = get16(data + 10);
    return true;
}

bool read_name(const std::uint8_t *data, std::size_t len, std::size_t pos, std::string &out, std::size_t *next,
               std::string *why)
{
    out.clear();
    std::size_t p = pos;
    std::size_t after = 0;
    bool jumped = false;
    int hops = 0;
    std::size_t wire = 0;
    for (;;) {
        if (!data || p >= len) return fail(why, "a name runs past the end");
        const std::uint8_t n = data[p];
        if (n == 0) {
            if (!jumped) after = p + 1;
            ++wire;
            break;
        }
        if ((n & 0xC0) == 0xC0) {
            if (p + 1 >= len) return fail(why, "a cut compression pointer");
            const std::size_t target = (static_cast<std::size_t>(n & 0x3F) << 8) | data[p + 1];
            if (target >= p) return fail(why, "a compression pointer that does not point back");
            if (++hops > kMaxPointerHops) return fail(why, "too many compression pointers");
            if (!jumped) after = p + 2;
            jumped = true;
            p = target;
            continue;
        }
        if (n & 0xC0) return fail(why, "an unknown label type");
        if (p + 1 + n > len) return fail(why, "a label runs past the end");
        wire += 1 + static_cast<std::size_t>(n);
        if (wire + 1 > kMaxNameBytes) return fail(why, "a name longer than 255 bytes");
        if (!out.empty()) out += '.';
        for (std::size_t i = 0; i < n; ++i) append_label_byte(out, data[p + 1 + i]); /* '.' and '\' come out as \DDD */
        p += 1 + n;
    }
    if (out.empty()) out = ".";
    if (next) *next = after;
    return true;
}

bool build_query(const std::string &input, std::uint16_t type, std::uint16_t id, const QueryOptions &opt,
                 std::vector<std::uint8_t> &out, std::string *why)
{
    out.clear();
    std::string reason;
    if (!valid_hostname(input, &reason)) {
        if (why) *why = reason;
        return false;
    }
    std::string name = input;
    if (!name.empty() && name.back() == '.') name.pop_back();
    put16(out, id);
    put16(out, opt.rd ? 0x0100 : 0x0000);
    put16(out, 1);
    put16(out, 0);
    put16(out, 0);
    put16(out, opt.edns ? 1 : 0);
    std::size_t start = 0;
    while (start <= name.size()) {
        std::size_t dot = name.find('.', start);
        if (dot == std::string::npos) dot = name.size();
        const std::size_t n = dot - start;
        out.push_back(static_cast<std::uint8_t>(n));
        out.insert(out.end(), name.begin() + static_cast<std::ptrdiff_t>(start),
                   name.begin() + static_cast<std::ptrdiff_t>(dot));
        start = dot + 1;
    }
    out.push_back(0);
    put16(out, type);
    put16(out, kClassIN);
    if (opt.edns) {
        out.push_back(0);          /* root name */
        put16(out, kOPT);
        put16(out, opt.udp_size);  /* class = the UDP payload we accept */
        put16(out, 0);             /* extended rcode, version */
        put16(out, 0);             /* flags (no DO) */
        put16(out, 0);             /* no options */
    }
    return true;
}

bool parse_message(const std::uint8_t *data, std::size_t len, Message &m, std::string *why)
{
    m = Message();
    if (!parse_header(data, len, m.header)) return fail(why, "shorter than a DNS header");
    std::size_t pos = 12;
    const Header &h = m.header;
    if (h.qdcount > 8) return fail(why, "too many questions");
    for (int i = 0; i < h.qdcount; ++i) {
        Question q;
        std::size_t next = 0;
        if (!read_name(data, len, pos, q.name, &next, why)) return false;
        if (next + 4 > len) return fail(why, "a question runs past the end");
        q.type = get16(data + next);
        q.cls = get16(data + next + 2);
        pos = next + 4;
        m.questions.push_back(q);
    }
    const std::size_t total = static_cast<std::size_t>(h.ancount) + h.nscount + h.arcount;
    if (total > kMaxRecords) return fail(why, "too many records");
    if (total * 11 > len - pos) /* a record takes at least 11 bytes: counts that cannot fit */
        return fail(why, "the record counts do not fit the packet");
    for (std::size_t i = 0; i < total; ++i) {
        Record r;
        r.section = i < h.ancount ? Section::Answer
                    : i < static_cast<std::size_t>(h.ancount) + h.nscount ? Section::Authority
                                                                          : Section::Additional;
        std::size_t next = 0;
        if (!read_name(data, len, pos, r.name, &next, why)) return false;
        if (next + 10 > len) return fail(why, "a record runs past the end");
        r.type = get16(data + next);
        r.cls = get16(data + next + 2);
        r.ttl = get32(data + next + 4);
        const std::size_t rdlen = get16(data + next + 8);
        const std::size_t rd = next + 10;
        if (rd + rdlen > len) return fail(why, "a record's data runs past the end");
        pos = rd + rdlen;
        if (r.type == kOPT) {
            if (r.section == Section::Additional && r.name == "." && !m.edns) {
                m.edns = true;
                m.udp_size = r.cls;
                m.header.rcode |= static_cast<int>((r.ttl >> 24) & 0xFF) << 4;
            }
            continue;
        }
        if (r.ttl & 0x80000000u) r.ttl = 0; /* RFC 2181: a TTL with the top bit set counts as 0 */
        r.data = format_rdata(data, len, r.type, rd, rdlen);
        switch (r.section) {
        case Section::Answer: m.answers.push_back(std::move(r)); break;
        case Section::Authority: m.authority.push_back(std::move(r)); break;
        case Section::Additional: m.additional.push_back(std::move(r)); break;
        }
    }
    return true;
}

bool names_equal(const std::string &a, const std::string &b)
{
    std::string x = lower(a), y = lower(b);
    if (x.size() > 1 && x.back() == '.') x.pop_back();
    if (y.size() > 1 && y.back() == '.') y.pop_back();
    return x == y;
}

bool response_matches(const Message &m, std::uint16_t id, const std::string &qname, std::uint16_t type)
{
    if (m.header.id != id || !m.header.qr) return false;
    if (m.questions.empty()) return m.header.rcode != kNoError; /* an error answer may leave the question out */
    return names_equal(m.questions[0].name, qname) && m.questions[0].type == type;
}

std::vector<std::string> cname_chain(const Message &m, const std::string &qname)
{
    std::vector<std::string> chain;
    std::string cur = qname;
    if (cur.size() > 1 && cur.back() == '.') cur.pop_back();
    chain.push_back(cur);
    for (int step = 0; step < 16; ++step) {
        const Record *alias = nullptr;
        for (const Record &r : m.answers)
            if (r.type == kCNAME && names_equal(r.name, cur)) {
                alias = &r;
                break;
            }
        if (!alias) break;
        bool seen = false;
        for (const std::string &n : chain)
            if (names_equal(n, alias->data)) seen = true;
        if (seen) break;
        cur = alias->data;
        chain.push_back(cur);
    }
    return chain;
}

} // namespace dns
} // namespace nt
