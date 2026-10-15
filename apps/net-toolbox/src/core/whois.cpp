/*
 * SPDX-License-Identifier: MIT
 *
 * Net Toolbox: whois (see whois.hpp).
 */
#include "whois.hpp"

#include "netaddr.hpp"
#include "sock.hpp"
#include "text.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <set>

namespace nt {
namespace {

bool in(const std::string &key, std::initializer_list<const char *> keys)
{
    for (const char *k : keys)
        if (key == k) return true;
    return false;
}

std::string rtrim(std::string s)
{
    while (!s.empty() && s.back() == ' ') s.pop_back();
    return s;
}

/* one line: tabs as spaces, control bytes dropped, invalid UTF-8 as '?', at most kWhoisMaxLineBytes */
std::string clean_line(const std::string &in_text)
{
    std::string out;
    std::size_t i = 0;
    while (i < in_text.size() && out.size() < kWhoisMaxLineBytes) {
        const unsigned char c = static_cast<unsigned char>(in_text[i]);
        if (c == '\t') {
            out += ' ';
            ++i;
            continue;
        }
        if (c < 0x20 || c == 0x7F) {
            ++i;
            continue;
        }
        if (c < 0x80) {
            out += static_cast<char>(c);
            ++i;
            continue;
        }
        std::size_t len = 0;
        std::uint32_t cp = 0;
        if ((c & 0xE0) == 0xC0) {
            len = 2;
            cp = c & 0x1F;
        } else if ((c & 0xF0) == 0xE0) {
            len = 3;
            cp = c & 0x0F;
        } else if ((c & 0xF8) == 0xF0) {
            len = 4;
            cp = c & 0x07;
        }
        bool ok = len > 0 && i + len <= in_text.size();
        for (std::size_t k = 1; ok && k < len; ++k) {
            const unsigned char cc = static_cast<unsigned char>(in_text[i + k]);
            if ((cc & 0xC0) != 0x80) ok = false;
            else cp = (cp << 6) | (cc & 0x3F);
        }
        if (ok) {
            const bool overlong = (len == 2 && cp < 0x80) || (len == 3 && cp < 0x800) || (len == 4 && cp < 0x10000);
            if (overlong || cp > 0x10FFFF || (cp >= 0xD800 && cp < 0xE000)) ok = false;
        }
        if (!ok) {
            out += '?';
            ++i;
            continue;
        }
        if (cp >= 0x80 && cp < 0xA0) { /* C1 controls */
            i += len;
            continue;
        }
        if (out.size() + len > kWhoisMaxLineBytes) break;
        out.append(in_text, i, len);
        i += len;
    }
    return rtrim(out);
}

/* "key: value" of an answer line; false for comments and lines without a key */
bool key_value(const std::string &line, std::string &key, std::string &value)
{
    const std::string t = trim(line);
    if (t.empty() || t[0] == '%' || t[0] == '#' || t[0] == '>') return false;
    const std::size_t colon = t.find(':');
    if (colon == std::string::npos || colon == 0 || colon > 48) return false;
    key = lower(trim(t.substr(0, colon)));
    value = trim(t.substr(colon + 1));
    return true;
}

bool indented(const std::string &line)
{
    return !line.empty() && (line[0] == ' ' || line[0] == '\t') && !trim(line).empty();
}

std::string first_word(const std::string &s)
{
    const std::vector<std::string> f = split_ws(s);
    return f.empty() ? std::string() : f[0];
}

void add_unique(std::vector<std::string> &list, const std::string &v, std::size_t max)
{
    if (v.empty() || list.size() >= max) return;
    if (std::find(list.begin(), list.end(), v) == list.end()) list.push_back(v);
}

std::string server_name(std::string v)
{
    v = lower(trim(v));
    if (starts_with(v, "rwhois://") || starts_with(v, "http://") || starts_with(v, "https://")) return std::string();
    if (starts_with(v, "whois://")) v = v.substr(8);
    const std::size_t slash = v.find('/');
    if (slash != std::string::npos) v.erase(slash);
    const std::size_t colon = v.find(':');
    if (colon != std::string::npos) {
        if (v.substr(colon + 1) != "43") return std::string(); /* another port: not this protocol's */
        v.erase(colon);
    }
    if (!v.empty() && v.back() == '.') v.pop_back();
    if (v.find('.') == std::string::npos || is_ip(v) || !valid_hostname(v)) return std::string();
    return v;
}

std::string parent_domain(const std::string &name)
{
    const std::size_t dot = name.find('.');
    return dot == std::string::npos ? std::string() : name.substr(dot + 1);
}

std::size_t label_count(const std::string &name)
{
    return static_cast<std::size_t>(std::count(name.begin(), name.end(), '.')) + 1;
}

} // namespace

bool parse_whois_query(const std::string &input, std::string &object, bool &is_ip, std::string *why)
{
    const auto fail = [&](const char *text) {
        if (why) *why = text;
        return false;
    };
    object.clear();
    is_ip = false;
    std::string s = trim(input);
    if (s.empty()) return fail("Type a domain (example.com) or an address (8.8.8.8, 2001:4860::8888)");
    if (s[0] == '-') return fail("A query cannot start with '-'");
    if (s.find('%') != std::string::npos) return fail("An address with an interface (%) cannot be looked up");
    Address a;
    if (parse_ip(s, a)) {
        object = a.text();
        is_ip = true;
        return true;
    }
    if (s.size() > 1 && s.back() == '.') s.pop_back();
    std::string w;
    if (!valid_hostname(s, &w)) {
        if (why) *why = w;
        return false;
    }
    if (s.find('.') == std::string::npos) return fail("Type a full domain with its ending (example.com)");
    object = lower(s);
    return true;
}

std::string whois_query_text(const std::string &server, const std::string &object, bool is_ip)
{
    if (is_ip && lower(server) == "whois.arin.net") return "n + " + object;
    return object;
}

std::string whois_referral(const std::vector<std::string> &lines, const std::string &current_server)
{
    /* by priority: IANA's refer: / whois:, ARIN's ReferralServer:, a registry's Registrar WHOIS Server: */
    static const char *const order[] = {"refer", "whois", "referralserver", "registrar whois server", "whois server"};
    std::string found[5];
    for (const std::string &line : lines) {
        std::string key, value;
        if (!key_value(line, key, value) || value.empty()) continue;
        for (int i = 0; i < 5; ++i) {
            if (key != order[i] || !found[i].empty()) continue;
            const std::string name = server_name(value);
            if (!name.empty() && name != lower(current_server)) found[i] = name;
        }
    }
    for (const std::string &f : found)
        if (!f.empty()) return f;
    return std::string();
}

std::vector<std::string> whois_clean_lines(const std::string &raw, std::size_t max_lines, bool *truncated)
{
    std::vector<std::string> out;
    if (truncated) *truncated = false;
    bool blank = true; /* no blank line at the start, nor two in a row */
    for (const std::string &line : split_lines(raw)) {
        const std::string c = clean_line(line);
        if (c.empty()) {
            if (blank) continue;
            blank = true;
        } else {
            blank = false;
        }
        if (out.size() >= max_lines) {
            if (truncated) *truncated = true;
            break;
        }
        out.push_back(c);
    }
    while (!out.empty() && out.back().empty()) out.pop_back();
    return out;
}

bool whois_not_found(const std::vector<std::string> &lines)
{
    for (const std::string &line : lines) {
        const std::string l = lower(line);
        if (l.find("no match for") != std::string::npos || l.find("not found") != std::string::npos ||
            l.find("no data found") != std::string::npos || l.find("no entries found") != std::string::npos ||
            l.find("no matching record") != std::string::npos || l.find("status: free") != std::string::npos ||
            l.find("status: available") != std::string::npos || l.find("no object found") != std::string::npos)
            return true;
    }
    return false;
}

bool WhoisSummary::empty() const
{
    return registrar.empty() && created.empty() && updated.empty() && expires.empty() && status.empty() &&
           name_servers.empty() && netname.empty() && organisation.empty() && country.empty() && cidr.empty() &&
           as_number.empty() && abuse.empty();
}

WhoisSummary parse_whois_summary(const std::vector<std::string> &lines, bool is_ip)
{
    WhoisSummary s;
    /* an address: every candidate with its line and rank; chosen at the end, see pick() */
    struct Cand {
        std::size_t at;
        int rank;
        std::string value;
    };
    std::vector<Cand> orgs, countries, ases, abuses, ranges;
    std::size_t anchor = 0; /* the line of the last network name: the narrowest network's object starts near it */
    bool have_anchor = false;
    for (std::size_t i = 0; i < lines.size(); ++i) {
        const std::string &line = lines[i];
        if (is_ip) {
            /* RIPE: "% Abuse contact for '193.0.0.0 - 193.0.7.255' is 'abuse@ripe.net'" */
            const std::string l = lower(line);
            if (starts_with(l, "% abuse contact for") || starts_with(l, "% abuse-mailbox")) {
                const std::size_t q2 = line.rfind('\'');
                const std::size_t q1 = q2 == std::string::npos || q2 == 0 ? std::string::npos : line.rfind('\'', q2 - 1);
                if (q1 != std::string::npos && q2 > q1 + 1) {
                    const std::string mail = line.substr(q1 + 1, q2 - q1 - 1);
                    if (mail.find('@') != std::string::npos) abuses.push_back({i, 1, mail});
                }
                continue;
            }
        }
        std::string key, value;
        if (!key_value(line, key, value)) continue;
        /* a value on the indented lines under its key (".uk" style) */
        std::vector<std::string> block;
        if (value.empty())
            for (std::size_t j = i + 1; j < lines.size() && indented(lines[j]) && block.size() < 16; ++j)
                block.push_back(trim(lines[j]));
        if (value.empty() && !block.empty()) value = block[0];
        if (value.empty()) continue;
        if (!is_ip) {
            if (in(key, {"registrar", "sponsoring registrar", "registrar name"})) {
                if (s.registrar.empty()) s.registrar = value;
            } else if (in(key, {"creation date", "created", "created on", "registered on", "registration time",
                                "domain registration date", "registered", "registration date"})) {
                if (s.created.empty()) s.created = value;
            } else if (in(key, {"updated date", "last updated", "last updated on", "last-update", "last modified",
                                "changed", "modified"})) {
                if (s.updated.empty()) s.updated = value;
            } else if (in(key, {"registry expiry date", "registrar registration expiration date", "expiration date",
                                "expiry date", "expires", "expires on", "paid-till", "expire", "renewal date",
                                "expiration time"})) {
                if (s.expires.empty()) s.expires = value;
            } else if (in(key, {"domain status", "status", "state"})) {
                add_unique(s.status, first_word(value), 8);
            } else if (in(key, {"name server", "nserver", "nameserver", "name servers", "nameservers"})) {
                const std::vector<std::string> values = block.empty() ? std::vector<std::string>{value} : block;
                for (const std::string &v : values) {
                    std::string ns = lower(first_word(v));
                    if (!ns.empty() && ns.back() == '.') ns.pop_back();
                    if (valid_hostname(ns)) add_unique(s.name_servers, ns, 8);
                }
            }
            continue;
        }
        if (key == "netname") {
            s.netname = value;
            anchor = i;
            have_anchor = true;
        } else if (key == "orgname" || key == "org-name") {
            orgs.push_back({i, 0, value});
        } else if (key == "owner") {
            orgs.push_back({i, 1, value});
        } else if (key == "organization" || (key == "organisation" && !starts_with(value, "ORG-"))) {
            orgs.push_back({i, 2, value}); /* RIPE's "organisation: ORG-..." is a handle, not a name */
        } else if (key == "descr") {
            orgs.push_back({i, 3, value});
        } else if (key == "country") {
            countries.push_back({i, 0, value});
        } else if (key == "cidr") {
            ranges.push_back({i, 0, value});
        } else if (key == "inetnum" || key == "inet6num") {
            ranges.push_back({i, 1, value});
        } else if (key == "netrange") {
            ranges.push_back({i, 2, value});
        } else if (in(key, {"originas", "origin", "aut-num"})) {
            std::string as = first_word(value);
            if (!as.empty() && as.find_first_not_of("0123456789") == std::string::npos) as = "AS" + as;
            if (starts_with(lower(as), "as") && as.size() > 2 &&
                as.find_first_not_of("0123456789", 2) == std::string::npos)
                ases.push_back({i, 0, "AS" + as.substr(2)});
        } else if (in(key, {"orgabuseemail", "abuse-mailbox"})) {
            if (value.find('@') != std::string::npos) abuses.push_back({i, 0, value});
        }
    }
    if (!is_ip) return s;
    /* the best rank among the candidates at or after the anchor (the first of them), else the best rank anywhere (the
     * last of them): the narrowest network's own country, holder and contacts, not those of a parent block or of a
     * contact object further down */
    const auto pick = [&](const std::vector<Cand> &list) -> std::string {
        const Cand *best = nullptr;
        if (have_anchor)
            for (const Cand &c : list)
                if (c.at >= anchor && (!best || c.rank < best->rank)) best = &c;
        if (!best)
            for (const Cand &c : list)
                if (!best || c.rank <= best->rank) best = &c;
        return best ? best->value : std::string();
    };
    /* the range: the last one of the best kind (ARIN lists the wider network first) */
    const Cand *range = nullptr;
    for (const Cand &c : ranges)
        if (!range || c.rank <= range->rank) range = &c;
    if (range) s.cidr = range->value;
    s.organisation = pick(orgs);
    s.country = pick(countries);
    s.as_number = pick(ases);
    s.abuse = pick(abuses);
    return s;
}

std::size_t WhoisResult::line_count() const
{
    std::size_t n = 0;
    for (const WhoisAnswer &a : answers) n += a.lines.size();
    return n;
}

WhoisSummary whois_summary_of(const WhoisResult &r)
{
    std::vector<const WhoisAnswer *> pick;
    for (const WhoisAnswer &a : r.answers)
        if (a.ok && lower(a.server) != kWhoisIana) pick.push_back(&a);
    if (pick.empty())
        for (const WhoisAnswer &a : r.answers)
            if (a.ok) pick.push_back(&a);
    WhoisSummary s;
    for (auto it = pick.rbegin(); it != pick.rend(); ++it) {
        const WhoisSummary &x = (*it)->summary;
        const auto fill = [](std::string &dst, const std::string &src) {
            if (dst.empty()) dst = src;
        };
        fill(s.registrar, x.registrar);
        fill(s.created, x.created);
        fill(s.updated, x.updated);
        fill(s.expires, x.expires);
        if (s.status.empty()) s.status = x.status;
        if (s.name_servers.empty()) s.name_servers = x.name_servers;
        fill(s.netname, x.netname);
        fill(s.organisation, x.organisation);
        fill(s.country, x.country);
        fill(s.cidr, x.cidr);
        fill(s.as_number, x.as_number);
        fill(s.abuse, x.abuse);
    }
    return s;
}

WhoisAnswer whois_ask(const std::string &server, const std::string &query, const WhoisRequest &req,
                      std::int64_t deadline_ms, const CancelToken &cancel)
{
    WhoisAnswer a;
    a.server = server;
    a.query = query;
    const std::int64_t left = deadline_ms - now_ms();
    if (left <= 0) {
        a.error = "time limit reached";
        return a;
    }
    ResolveOptions ro = req.resolve;
    ro.timeout_ms = static_cast<int>(std::max<std::int64_t>(300, std::min<std::int64_t>(ro.timeout_ms, left / 3)));
    ro.tries = 1;
    const Resolved res = resolve_host(server, ro, cancel);
    if (cancel.cancelled()) {
        a.error = "cancelled";
        return a;
    }
    if (!res.ok) {
        a.error = "cannot find " + server + ": " + res.error;
        return a;
    }
    int e = 0;
    Fd fd = tcp_connect(res.address, req.port, std::min(deadline_ms, now_ms() + 5000), cancel, &e);
    if (!fd.valid()) {
        a.error = errno_words(e);
        return a;
    }
    const std::string line = query + "\r\n";
    if ((e = send_all(fd.get(), line.data(), line.size(), deadline_ms, cancel))) {
        a.error = errno_words(e);
        return a;
    }
    std::string raw;
    char buf[4096];
    for (;;) {
        std::size_t got = 0;
        e = recv_some(fd.get(), buf, sizeof(buf), &got, deadline_ms, cancel);
        if (e) {
            if (e == ECANCELED) {
                a.error = "cancelled";
                return a;
            }
            if (raw.empty()) {
                a.error = errno_words(e);
                return a;
            }
            break; /* what came before the deadline or the error is kept */
        }
        if (!got) break;
        const std::size_t room = kWhoisMaxAnswerBytes - raw.size();
        raw.append(buf, std::min(got, room));
        if (got >= room) {
            a.truncated = true;
            break;
        }
    }
    a.bytes = raw.size();
    if (raw.empty()) {
        a.error = "the server closed the connection without answering";
        return a;
    }
    a.ok = true;
    a.lines = whois_clean_lines(raw, SIZE_MAX);
    return a;
}

WhoisResult run_whois(const WhoisRequest &req, const CancelToken &cancel,
                      const std::function<void(const WhoisResult &, const std::string &)> &publish)
{
    WhoisResult r;
    std::string why;
    if (!parse_whois_query(req.query, r.object, r.is_ip, &why)) {
        r.note = why;
        return r;
    }
    const std::int64_t deadline = now_ms() + std::max(500, req.total_ms);
    std::set<std::string> asked; /* server|object */
    std::string server = lower(req.first_server);
    std::string object = r.object;
    int referrals = 0, parent_tries = 0;
    while (!cancel.cancelled()) {
        if (now_ms() >= deadline) {
            r.note = "time limit reached";
            break;
        }
        if (!asked.insert(server + "|" + object).second) {
            r.note = "referral loop stopped (" + server + " was already asked)";
            break;
        }
        if (publish) publish(r, server);
        WhoisAnswer a = whois_ask(server, whois_query_text(server, object, r.is_ip), req, deadline, cancel);
        if (cancel.cancelled()) break;
        a.summary = a.ok ? parse_whois_summary(a.lines, r.is_ip) : WhoisSummary();
        const std::string next = a.ok ? whois_referral(a.lines, server) : std::string();
        const bool retry_parent = a.ok && !r.is_ip && server != kWhoisIana && a.summary.empty() &&
                                  whois_not_found(a.lines) && label_count(object) > 2 && parent_tries < 2;
        /* the line budget of the whole result */
        const std::size_t used = r.line_count();
        const std::size_t room = used < kWhoisMaxLines ? kWhoisMaxLines - used : 0;
        if (a.lines.size() > room) {
            a.lines.resize(room);
            while (!a.lines.empty() && a.lines.back().empty()) a.lines.pop_back();
            r.lines_truncated = true;
        }
        r.answers.push_back(std::move(a));
        if (!r.answers.back().ok) break;
        if (retry_parent) {
            object = parent_domain(object);
            ++parent_tries;
            continue;
        }
        if (next.empty()) break;
        if (referrals >= kWhoisMaxReferrals) {
            r.note = "referral limit reached (" + next + " not asked)";
            break;
        }
        ++referrals;
        server = next;
    }
    if (cancel.cancelled()) r.note = "cancelled";
    r.summary = whois_summary_of(r);
    if (publish) publish(r, std::string());
    return r;
}

bool WhoisSession::start(const WhoisRequest &req)
{
    worker_.cancel();
    const std::uint64_t gen = worker_.generation() + 1;
    WhoisSnapshot first;
    first.input = trim(req.query);
    std::string why;
    if (!parse_whois_query(req.query, first.result.object, first.result.is_ip, &why)) {
        first.state = WhoisSnapshot::State::Failed;
        first.error = why;
        channel_.begin(gen, first);
        return false;
    }
    first.state = WhoisSnapshot::State::Running;
    first.asking = req.first_server;
    channel_.begin(gen, first);
    const std::uint64_t started = worker_.start([this, gen, first, req](const CancelToken &cancel) {
        WhoisSnapshot snap = first;
        const WhoisResult r = run_whois(req, cancel, [&](const WhoisResult &partial, const std::string &asking) {
            if (asking.empty()) return; /* the final one is published below */
            snap.result = partial;
            snap.asking = asking;
            channel_.publish(gen, snap);
        });
        snap.result = r;
        snap.asking.clear();
        snap.state = cancel.cancelled() ? WhoisSnapshot::State::Cancelled : WhoisSnapshot::State::Done;
        channel_.publish(gen, snap);
    });
    if (!started) {
        first.state = WhoisSnapshot::State::Failed;
        first.error = "Could not start the lookup (no thread)";
        channel_.begin(gen, first);
        return false;
    }
    return true;
}

void WhoisSession::stop()
{
    worker_.cancel();
}

} // namespace nt
