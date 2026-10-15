/*
 * SPDX-License-Identifier: MIT
 *
 * Net Toolbox 0.2: what the new pages say (see view_model.hpp): the traceroute chain, the HTTP check, the public address
 * and whois. No LVGL, no network (tests/test_ui_model.cpp compiles this file).
 */
#include "view_model.hpp"

#include "text.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <ctime>

namespace nt {
namespace ui {

namespace {

RowDef item2(std::string label, std::string value, Tone tone = Tone::Normal)
{
    RowDef r;
    r.label = std::move(label);
    r.value = std::move(value);
    r.tone = tone;
    return r;
}

std::string capital2(std::string s)
{
    if (!s.empty() && s[0] >= 'a' && s[0] <= 'z') s[0] = static_cast<char>(s[0] - 'a' + 'A');
    return s;
}

std::string plural(long n, const char *one, const char *many)
{
    return std::to_string(n) + " " + (n == 1 ? one : many);
}

std::string pct_tag(double pct)
{
    long v = std::lround(pct);
    if (v < 1) v = 1;
    return std::to_string(v) + " %";
}

} // namespace

/* ---------------------------------------------------------------- shared */

std::string days_words(int days)
{
    if (days >= 2) return "expires in " + std::to_string(days) + " days";
    if (days == 1) return "expires in 1 day";
    if (days == 0) return "expires today";
    if (days == -1) return "expired 1 day ago";
    return "expired " + std::to_string(-days) + " days ago";
}

Tone days_tone(int days)
{
    if (days < 7) return Tone::Bad;
    if (days < 30) return Tone::Warn;
    return Tone::Ok;
}

std::string size_text(std::uint64_t bytes)
{
    if (bytes == 1) return "1 byte";
    if (bytes < 1000) return std::to_string(bytes) + " bytes";
    const auto scaled = [](double v, const char *unit) {
        char buf[32];
        if (v < 10) std::snprintf(buf, sizeof(buf), "%.1f %s", v, unit);
        else std::snprintf(buf, sizeof(buf), "%.0f %s", v, unit);
        return std::string(buf);
    };
    if (bytes < 1000000) return scaled(static_cast<double>(bytes) / 1000.0, "kB");
    if (bytes < 1000000000) return scaled(static_cast<double>(bytes) / 1e6, "MB");
    return scaled(static_cast<double>(bytes) / 1e9, "GB");
}

/* ---------------------------------------------------------------- Traceroute */

HopLook hop_look(const TraceHop &h)
{
    const PingStats &st = h.stats;
    if (!h.unreachable.empty()) return HopLook::Lossy;
    if (st.received == 0) return st.lost > 0 || h.silent ? HopLook::Silent : HopLook::Pending;
    const double loss = st.loss_pct();
    if (loss > 0) {
        if (h.loss_only_here) return HopLook::RateLimited;
        return loss > kHopLossBadPct ? HopLook::Lossy : HopLook::Slow;
    }
    return st.avg_ms() > kHopSlowAvgMs ? HopLook::Slow : HopLook::Ok;
}

const char *hop_look_word(HopLook look)
{
    switch (look) {
    case HopLook::Pending: return "waiting";
    case HopLook::Ok: return "ok";
    case HopLook::Slow: return "slow or some loss";
    case HopLook::Lossy: return "real loss";
    case HopLook::Silent: return "no reply";
    case HopLook::RateLimited: return "probably rate limiting";
    }
    return "";
}

std::string loss_tag(const TraceHop &h)
{
    const PingStats &st = h.stats;
    if (st.received == 0 || st.lost == 0) return std::string();
    return pct_tag(st.loss_pct());
}

HopTexts hop_texts(const TraceHop &h)
{
    HopTexts t;
    t.look = hop_look(h);
    t.destination = h.destination;
    t.number = std::to_string(h.ttl);
    if (!h.addresses.empty()) {
        const TraceHopAddress &a = h.addresses[0];
        t.name = a.name.empty() ? a.address : printable(a.name);
        if (!a.name.empty()) t.ip = a.address;
        const int extra = static_cast<int>(h.addresses.size()) - 1 + (h.other_answers > 0 ? 1 : 0);
        if (extra > 0) {
            if (t.ip.empty()) t.ip = "+" + std::to_string(extra) + " more";
            else t.ip += " +" + std::to_string(extra);
        }
    }
    const PingStats &st = h.stats;
    if (st.received > 0) {
        t.avg_ms = st.avg_ms();
        t.best_ms = st.min_ms;
        t.worst_ms = st.max_ms;
        t.avg = fmt_ms(st.avg_ms());
        t.has_bar = true;
    }
    t.loss = loss_tag(h);
    t.loss_tone = t.look == HopLook::Lossy ? Tone::Bad : t.look == HopLook::Slow ? Tone::Warn : Tone::Normal;
    if (t.look == HopLook::Silent) t.bar_word = "no reply";
    else if (t.look == HopLook::Pending) t.bar_word = "waiting";
    if (h.destination) {
        t.note = "reached";
        t.note_tone = Tone::Ok;
    } else if (!h.unreachable.empty()) {
        t.note = capital2(h.unreachable);
        t.note_tone = Tone::Bad;
    } else if (t.look == HopLook::RateLimited) {
        t.note = "probably rate limiting";
    }
    return t;
}

double chain_scale_ms(const std::vector<TraceHop> &hops, std::size_t shown)
{
    double m = 1.0;
    for (std::size_t i = 0; i < shown && i < hops.size(); ++i)
        if (hops[i].stats.received > 0) m = std::max(m, hops[i].stats.avg_ms());
    return m;
}

int bar_px(double ms, double scale_ms, int full_px, int stub_px)
{
    if (ms <= 0 || full_px <= 0) return 0;
    const double scale = scale_ms > 0 ? scale_ms : 1.0;
    int px = static_cast<int>(std::lround(ms / scale * full_px));
    px = std::max(px, std::min(stub_px, full_px));
    return std::min(px, full_px);
}

std::string trace_progress(const TraceSnapshot &s, bool with_address)
{
    std::string t = "to " + s.target;
    if (with_address && !s.address.empty() && s.address != s.target) t += " " + s.address;
    t += ", round " + std::to_string(std::max(1, s.rounds)) + ", " + plural(static_cast<long>(s.shown), "hop", "hops");
    return t;
}

std::string trace_verdict(const TraceSnapshot &s)
{
    const std::size_t dest = s.dest_hop > 0 ? static_cast<std::size_t>(s.dest_hop) : 0;
    if (s.reached && dest > 0 && dest <= s.hops.size()) {
        const TraceHop &h = s.hops[dest - 1];
        std::string t = "Reached in " + plural(s.dest_hop, "hop", "hops");
        if (h.stats.received > 0) t += ", " + fmt_ms(h.stats.avg_ms());
        return t;
    }
    if (s.unreachable && dest > 0 && dest <= s.hops.size())
        return "Did not reach the destination: unreachable at hop " + std::to_string(s.dest_hop);
    if (s.shown == 0) return "Did not reach the destination: no hop answered";
    return "Did not reach the destination: no answer after hop " + std::to_string(s.shown);
}

Band trace_band(const TraceSnapshot &s, const std::string &idle_target, bool with_address)
{
    Band b;
    switch (s.state) {
    case TraceSnapshot::State::Idle:
        b.text = idle_target.empty() ? std::string("Press Start to trace") : "Press Start to trace " + idle_target;
        break;
    case TraceSnapshot::State::Resolving:
        b.text = "Finding " + s.target + "...";
        break;
    case TraceSnapshot::State::Running:
        if (!s.error.empty()) {
            b.text = s.error;
            b.tone = Tone::Warn;
        } else {
            b.text = trace_progress(s, with_address);
            b.tone = Tone::Ok;
        }
        break;
    case TraceSnapshot::State::Failed:
        b.text = s.error.empty() ? std::string("The trace failed") : s.error;
        b.tone = Tone::Bad;
        break;
    case TraceSnapshot::State::Done:
    case TraceSnapshot::State::Cancelled:
        b.text = trace_verdict(s);
        b.tone = s.reached ? Tone::Ok : s.unreachable ? Tone::Bad : Tone::Warn;
        break;
    }
    return b;
}

HopDetail hop_detail(const TraceHop &h)
{
    HopDetail d;
    const PingStats &st = h.stats;
    const HopLook look = hop_look(h);
    if (h.addresses.empty()) {
        d.addresses = look == HopLook::Pending ? "waiting for the first answer" : "no address answered";
    } else {
        for (const TraceHopAddress &a : h.addresses) {
            d.addresses += d.addresses.empty() ? "" : "; ";
            d.addresses += a.address;
            if (!a.name.empty()) d.addresses += " " + printable(a.name);
            d.addresses += ", " + plural(a.count, "answer", "answers");
        }
        if (h.other_answers > 0) d.addresses += "; " + plural(h.other_answers, "answer", "answers") + " from other addresses";
    }
    if (st.received > 0)
        d.timing = "last " + fmt_ms(st.last_ms) + "  avg " + fmt_ms(st.avg_ms()) + "  best " + fmt_ms(st.min_ms) + "  worst " +
                   fmt_ms(st.max_ms) + "  jitter " + fmt_ms(st.jitter_ms());
    else
        d.timing = "no answer yet";
    d.counts = "sent " + std::to_string(st.transmitted) + "  received " + std::to_string(st.received) + "  lost " +
               std::to_string(st.lost) + " (" + fmt_pct(st.loss_pct()) + ")";
    switch (look) {
    case HopLook::Pending: d.note = "Waiting for the first answers."; break;
    case HopLook::Ok: d.note = "Fine: it answers every probe."; break;
    case HopLook::Slow: d.note = "Some packets are lost or slow here."; break;
    case HopLook::Lossy: d.note = "Real loss: later hops lose packets too, so the problem starts here or before."; break;
    case HopLook::Silent:
        d.note = "No reply: many routers never answer these probes. Later hops can still be fine.";
        break;
    case HopLook::RateLimited:
        d.note = "Probably rate limiting: this router drops some of its own answers, later hops are clean, the path is fine.";
        break;
    }
    if (!h.unreachable.empty()) d.note = "A router says: " + h.unreachable + ".";
    if (h.destination) d.note = "This is the destination. " + d.note;
    return d;
}

/* ---------------------------------------------------------------- HTTP check */

PortSummary http_badge(const HttpCheckSnapshot &s)
{
    PortSummary p;
    p.host = s.result.url.empty() ? s.input : s.result.url;
    switch (s.state) {
    case HttpCheckSnapshot::State::Idle:
        p.detail = "Choose an address, then press Run";
        return p;
    case HttpCheckSnapshot::State::Running:
        p.word = "Checking...";
        p.detail = s.step == "certificate" ? "Reading the certificate"
                                           : "Waiting for the server, up to 15 seconds. Stop cancels it";
        return p;
    case HttpCheckSnapshot::State::Failed:
        p.word = "Not checked";
        p.detail = s.error;
        p.tone = Tone::Bad;
        p.mark = Mark::Cross;
        return p;
    case HttpCheckSnapshot::State::Cancelled:
        p.word = "Stopped";
        p.detail = "The check was stopped";
        p.mark = Mark::Dash;
        return p;
    case HttpCheckSnapshot::State::Done: break;
    }
    const HttpCheckResult &r = s.result;
    if (r.problem == HttpProblem::Cancelled) {
        p.word = "Stopped";
        p.detail = "The check was stopped";
        p.mark = Mark::Dash;
        return p;
    }
    if (r.answered && r.status > 0) {
        const int code = r.status;
        const std::string reason = r.reason.empty() ? http_reason(code) : r.reason;
        if (code >= 200 && code < 300) {
            p.word = std::to_string(code) + " " + reason;
            p.tone = Tone::Ok;
            p.mark = Mark::Check;
        } else if (code >= 300 && code < 400) {
            p.word = std::to_string(code) + (code == 304 ? " not modified" : " moved");
            p.tone = Tone::Warn;
            p.mark = Mark::Arrow;
        } else if (code >= 400) {
            p.word = std::to_string(code) + " " + reason;
            p.tone = Tone::Bad;
            p.mark = Mark::Cross;
        } else {
            p.word = std::to_string(code) + " " + reason;
            p.mark = Mark::Dash;
        }
        if (code >= 300 && code < 400 && !r.redirect_url.empty()) p.detail = "Moves to " + r.redirect_url;
        else if (r.timings.valid)
            p.detail = "Answered in " + fmt_ms(r.timings.total_ms) + (r.http_version.empty() ? "" : " over " + r.http_version);
        return p;
    }
    p.word = capital2(http_problem_name(r.problem));
    p.detail = r.problem_words;
    p.tone = Tone::Bad;
    p.mark = Mark::Cross;
    return p;
}

Band http_band(const HttpCheckSnapshot &s)
{
    Band b;
    switch (s.state) {
    case HttpCheckSnapshot::State::Idle:
        b.text = "Press Run to check an address";
        return b;
    case HttpCheckSnapshot::State::Running:
        b.text = (s.step == "certificate" ? "Reading the certificate of " : "Asking ") + s.input + "...";
        return b;
    case HttpCheckSnapshot::State::Failed:
        b.text = s.error.empty() ? std::string("The address cannot be used") : s.error;
        b.tone = Tone::Bad;
        return b;
    case HttpCheckSnapshot::State::Cancelled:
        b.text = "Stopped";
        return b;
    case HttpCheckSnapshot::State::Done: break;
    }
    const HttpCheckResult &r = s.result;
    if (r.answered && r.status > 0) {
        b.text = std::to_string(r.status) + (r.reason.empty() ? "" : " " + r.reason);
        if (r.timings.valid) b.text += " in " + fmt_ms(r.timings.total_ms);
        b.tone = r.status < 300 ? Tone::Ok : r.status < 400 ? Tone::Warn : Tone::Bad;
        if (r.insecure) b.text += ", certificate not checked";
        return b;
    }
    if (r.problem == HttpProblem::Cancelled) {
        b.text = "Stopped";
        return b;
    }
    b.text = "No answer: " + std::string(http_problem_name(r.problem));
    b.tone = Tone::Bad;
    return b;
}

std::vector<WaterSeg> waterfall(const HttpTimings &t, int width_px, int min_px)
{
    std::vector<WaterSeg> segs;
    if (!t.valid) return segs;
    const auto add = [&](WaterPart part, const char *name, double ms) {
        WaterSeg s;
        s.part = part;
        s.name = name;
        s.ms = std::max(0.0, ms);
        segs.push_back(s);
    };
    add(WaterPart::Dns, "DNS", t.dns_ms);
    if (t.connected) add(WaterPart::Connect, "Connect", t.connect_ms);
    if (t.tls) add(WaterPart::Tls, "TLS", t.tls_ms);
    if (t.first_byte) {
        add(WaterPart::Wait, "Waiting", t.wait_ms);
        add(WaterPart::Transfer, "Transfer", t.transfer_ms);
    }
    double total = 0;
    for (const WaterSeg &s : segs) total += s.ms;
    const int width = std::max(0, width_px);
    if (total <= 0 || width == 0) return segs;
    int sum = 0;
    for (WaterSeg &s : segs) {
        s.w = s.ms > 0 ? std::max(min_px, static_cast<int>(std::floor(s.ms / total * width))) : 0;
        sum += s.w;
    }
    int diff = width - sum;
    while (diff != 0) {
        std::size_t pick = segs.size();
        for (std::size_t i = 0; i < segs.size(); ++i)
            if (pick == segs.size() || segs[i].w > segs[pick].w) pick = i;
        if (pick == segs.size()) break;
        if (diff > 0) {
            segs[pick].w += diff;
            diff = 0;
        } else {
            if (segs[pick].w <= min_px) break; /* nothing left to take from */
            const int take = std::min(-diff, segs[pick].w - min_px);
            segs[pick].w -= take;
            diff += take;
        }
    }
    int x = 0;
    for (WaterSeg &s : segs) {
        s.x = x;
        x += s.w;
    }
    return segs;
}

std::string waterfall_total(const HttpTimings &t)
{
    return "Total " + fmt_ms(t.total_ms);
}

std::string water_label(const WaterSeg &seg)
{
    return std::string(seg.name) + " " + fmt_ms(seg.ms);
}

CertBadge cert_badge(const HttpCheckResult &r)
{
    CertBadge b;
    const CertInfo &c = r.cert;
    if (c.present) {
        b.detail = printable(c.subject);
        if (c.not_yet_valid) {
            b.word = "not valid yet";
            b.tone = Tone::Bad;
            b.mark = Mark::Cross;
        } else if (c.has_days) {
            b.has_days = true;
            b.days = c.days_left;
            b.word = days_words(c.days_left);
            b.tone = days_tone(c.days_left);
            b.mark = b.tone == Tone::Ok ? Mark::Check : b.tone == Tone::Warn ? Mark::Bang : Mark::Cross;
        } else {
            b.word = "date not readable";
            b.tone = Tone::Warn;
            b.mark = Mark::Bang;
        }
        return b;
    }
    const std::string where = r.final_url.empty() ? r.url : r.final_url;
    if (starts_with(lower(where), "http://")) {
        b.word = "no certificate";
        b.detail = "Plain HTTP: nothing is encrypted";
        b.mark = Mark::Dash;
        return b;
    }
    b.word = "not read";
    b.detail = starts_with(lower(where), "https://") ? "The certificate could not be read" : "";
    b.mark = Mark::Dash;
    return b;
}

std::vector<RowDef> http_final_rows(const HttpCheckResult &r)
{
    std::vector<RowDef> rows;
    const auto same = [](std::string a, std::string b) {
        if (!a.empty() && a.back() == '/') a.pop_back();
        if (!b.empty() && b.back() == '/') b.pop_back();
        return a == b;
    };
    if (!r.final_url.empty() && !same(r.final_url, r.url)) rows.push_back(item2("Final URL", r.final_url));
    if (!r.redirect_url.empty()) rows.push_back(item2("Moves to", r.redirect_url, Tone::Warn));
    return rows;
}

std::vector<RowDef> http_cert_rows(const HttpCheckResult &r)
{
    std::vector<RowDef> rows;
    const CertInfo &c = r.cert;
    if (c.present) {
        if (!c.subject.empty()) rows.push_back(item2("Subject", printable(c.subject)));
        if (!c.issuer.empty()) rows.push_back(item2("Issuer", printable(c.issuer)));
        if (!c.not_after.empty()) rows.push_back(item2("Valid until", c.not_after));
    }
    if (!r.cert_note.empty()) rows.push_back(item2("Not checked", r.cert_note, Tone::Warn));
    return rows;
}

std::vector<RowDef> http_info_rows(const HttpCheckResult &r)
{
    std::vector<RowDef> rows;
    if (!r.remote_ip.empty()) {
        std::string ip = r.remote_ip;
        if (r.remote_port > 0) {
            if (r.remote_ip.find(':') != std::string::npos) ip += " port " + std::to_string(r.remote_port);
            else ip += ":" + std::to_string(r.remote_port);
        }
        rows.push_back(item2("Remote IP", ip));
    }
    if (!r.answered) return rows;
    if (!r.http_version.empty()) rows.push_back(item2("HTTP version", r.http_version));
    rows.push_back(item2("Size", r.head ? std::string("none (HEAD asks for the headers only)") : size_text(r.size_bytes)));
    rows.push_back(item2("Redirects", r.redirects > 0 ? std::to_string(r.redirects) : std::string("none")));
    return rows;
}

/* ---------------------------------------------------------------- Public IP */

std::string country_text(const std::string &code)
{
    static const struct {
        const char *code, *name;
    } table[] = {{"US", "United States"}, {"CA", "Canada"}, {"MX", "Mexico"}, {"BR", "Brazil"}, {"AR", "Argentina"},
                 {"CL", "Chile"}, {"CO", "Colombia"}, {"GB", "United Kingdom"}, {"IE", "Ireland"}, {"FR", "France"},
                 {"DE", "Germany"}, {"NL", "Netherlands"}, {"BE", "Belgium"}, {"LU", "Luxembourg"}, {"CH", "Switzerland"},
                 {"AT", "Austria"}, {"IT", "Italy"}, {"ES", "Spain"}, {"PT", "Portugal"}, {"SE", "Sweden"},
                 {"NO", "Norway"}, {"DK", "Denmark"}, {"FI", "Finland"}, {"IS", "Iceland"}, {"PL", "Poland"},
                 {"CZ", "Czechia"}, {"SK", "Slovakia"}, {"HU", "Hungary"}, {"RO", "Romania"}, {"BG", "Bulgaria"},
                 {"GR", "Greece"}, {"TR", "Turkey"}, {"UA", "Ukraine"}, {"RU", "Russia"}, {"IL", "Israel"},
                 {"AE", "United Arab Emirates"}, {"SA", "Saudi Arabia"}, {"EG", "Egypt"}, {"ZA", "South Africa"},
                 {"NG", "Nigeria"}, {"KE", "Kenya"}, {"IN", "India"}, {"PK", "Pakistan"}, {"CN", "China"},
                 {"HK", "Hong Kong"}, {"TW", "Taiwan"}, {"JP", "Japan"}, {"KR", "South Korea"}, {"SG", "Singapore"},
                 {"MY", "Malaysia"}, {"TH", "Thailand"}, {"VN", "Vietnam"}, {"ID", "Indonesia"}, {"PH", "Philippines"},
                 {"AU", "Australia"}, {"NZ", "New Zealand"}};
    std::string up = trim(code);
    for (char &ch : up)
        if (ch >= 'a' && ch <= 'z') ch = static_cast<char>(ch - 'a' + 'A');
    for (const auto &e : table)
        if (up == e.code) return std::string(e.name) + " (" + e.code + ")";
    return trim(code);
}

std::string nat_note(const std::string &interface_ip, const PublicIpResult &r)
{
    if (!r.v4.ok) return std::string();
    if (nat_kind(interface_ip, r.v4.trace.ip) != NatKind::Cgnat) return std::string();
    return "Your provider shares one public address between customers (carrier-grade NAT): you cannot receive inbound "
           "connections";
}

std::vector<RowDef> publicip_rows(const PublicIpSnapshot &s, const std::string &interface_ip)
{
    std::vector<RowDef> rows;
    const bool running = s.state == PublicIpSnapshot::State::Running;
    const PublicIpResult &r = s.result;
    const auto family = [&](const char *label, const PublicIpFamily &f, int fam) {
        if (!f.tried) {
            rows.push_back(item2(label, running ? (s.family == fam ? "asking..." : "waiting") : "not checked"));
            return;
        }
        if (f.ok) {
            rows.push_back(item2(label, f.trace.ip, Tone::Ok));
            return;
        }
        if (fam == 6) rows.push_back(item2(label, "none")); /* normal: most networks have no IPv6 */
        else rows.push_back(item2(label, "not found: " + f.error, Tone::Bad));
    };
    family("Public IPv4", r.v4, 4);
    family("Public IPv6", r.v6, 6);
    const CfTrace *tr = r.v4.ok ? &r.v4.trace : r.v6.ok ? &r.v6.trace : nullptr;
    if (tr && !tr->loc.empty()) rows.push_back(item2("Country", country_text(tr->loc)));
    if (tr && !tr->colo.empty()) rows.push_back(item2("Cloudflare data centre", tr->colo));
    std::string local = trim(interface_ip);
    const std::size_t slash = local.find('/');
    if (slash != std::string::npos) local.erase(slash);
    if (local.empty()) rows.push_back(item2("Local address", "none"));
    else rows.push_back(item2("Local address", local + " (" + ip_class_name(ip_class(local)) + ")"));
    return rows;
}

Band publicip_band(const PublicIpSnapshot &s)
{
    Band b;
    switch (s.state) {
    case PublicIpSnapshot::State::Idle: return b;
    case PublicIpSnapshot::State::Running:
        b.text = s.family == 6 ? "Asking Cloudflare for the IPv6 address..." : "Asking Cloudflare for the IPv4 address...";
        return b;
    case PublicIpSnapshot::State::Cancelled:
        b.text = "Stopped";
        return b;
    case PublicIpSnapshot::State::Done: break;
    }
    const PublicIpResult &r = s.result;
    if (r.v4.ok) {
        b.text = "The internet sees you as " + r.v4.trace.ip;
        b.tone = Tone::Ok;
    } else if (r.v6.ok) {
        b.text = "The internet sees you as " + r.v6.trace.ip + " (no IPv4)";
        b.tone = Tone::Ok;
    } else {
        b.text = "Not reachable: " + r.v4.error;
        b.tone = Tone::Bad;
    }
    return b;
}

std::string publicip_address(const PublicIpSnapshot &s)
{
    if (s.result.v4.ok) return s.result.v4.trace.ip;
    if (s.result.v6.ok) return s.result.v6.trace.ip;
    return std::string();
}

/* ---------------------------------------------------------------- Whois */

bool whois_day(const std::string &text, std::int64_t &epoch, std::string *iso)
{
    const std::string t = trim(text);
    int y = 0, m = 0, d = 0;
    const auto digits = [&](std::size_t at, std::size_t n, int &out) {
        if (at + n > t.size()) return false;
        int v = 0;
        for (std::size_t i = 0; i < n; ++i) {
            const char c = t[at + i];
            if (c < '0' || c > '9') return false;
            v = v * 10 + (c - '0');
        }
        out = v;
        return true;
    };
    bool ok = false;
    if (digits(0, 4, y) && t.size() >= 10 && (t[4] == '-' || t[4] == '.' || t[4] == '/') && t[7] == t[4] && digits(5, 2, m) &&
        digits(8, 2, d)) {
        ok = true;
    } else if (digits(0, 2, d) && t.size() >= 11 && t[2] == '-' && t[6] == '-' && digits(7, 4, y)) {
        static const char *months[] = {"jan", "feb", "mar", "apr", "may", "jun", "jul", "aug", "sep", "oct", "nov", "dec"};
        const std::string mon = lower(t.substr(3, 3));
        for (int i = 0; i < 12; ++i)
            if (mon == months[i]) m = i + 1;
        ok = m > 0;
    }
    if (!ok || y < 1970 || y > 9999 || m < 1 || m > 12 || d < 1 || d > 31) return false;
    std::tm tm{};
    tm.tm_year = y - 1900;
    tm.tm_mon = m - 1;
    tm.tm_mday = d;
    tm.tm_hour = 12;
    const std::time_t e = timegm(&tm);
    if (e == static_cast<std::time_t>(-1)) return false;
    epoch = static_cast<std::int64_t>(e);
    if (iso) {
        char buf[16];
        std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d", y, m, d);
        *iso = buf;
    }
    return true;
}

std::vector<RowDef> whois_rows(const WhoisResult &r, std::time_t now)
{
    std::vector<RowDef> rows;
    const WhoisSummary &s = r.summary;
    const auto field = [&](const char *label, const std::string &value, Tone tone = Tone::Normal) {
        if (!trim(value).empty()) rows.push_back(item2(label, trim(value), tone));
    };
    const auto day = [&](const char *label, const std::string &value, bool expiry) {
        if (trim(value).empty()) return;
        std::int64_t epoch = 0;
        std::string iso;
        if (!whois_day(value, epoch, &iso)) {
            rows.push_back(item2(label, trim(value)));
            return;
        }
        if (!expiry) {
            rows.push_back(item2(label, iso));
            return;
        }
        const int days = static_cast<int>(epoch / 86400 - static_cast<std::int64_t>(now) / 86400);
        rows.push_back(item2(label, iso + " (" + days_words(days) + ")", days_tone(days)));
    };
    if (r.is_ip) {
        field("Net name", s.netname);
        field("Organisation", s.organisation);
        if (!s.country.empty()) field("Country", country_text(s.country));
        field("Range", s.cidr);
        field("AS number", s.as_number);
        field("Abuse contact", s.abuse);
        return rows;
    }
    field("Registrar", s.registrar);
    day("Created", s.created, false);
    day("Updated", s.updated, false);
    day("Expires", s.expires, true);
    if (!s.status.empty()) {
        std::string joined;
        for (const std::string &st : s.status) joined += (joined.empty() ? "" : ", ") + st;
        field("Status", joined);
    }
    for (const std::string &ns : s.name_servers) field("Name server", ns);
    return rows;
}

std::vector<std::string> whois_raw(const WhoisResult &r, std::size_t max_lines, std::size_t *more)
{
    std::vector<std::string> out;
    std::size_t skipped = 0;
    const auto push = [&](const std::string &line) {
        if (out.size() < max_lines) out.push_back(line);
        else ++skipped;
    };
    for (const WhoisAnswer &a : r.answers) {
        push("-- " + a.server + (a.ok ? "" : ": " + a.error) + " --");
        for (const std::string &line : a.lines) push(line);
    }
    if (more) *more = skipped;
    return out;
}

Band whois_band(const WhoisSnapshot &s)
{
    Band b;
    switch (s.state) {
    case WhoisSnapshot::State::Idle:
        b.text = "Type a domain or an address and press Run";
        return b;
    case WhoisSnapshot::State::Running:
        b.text = "Asking " + (s.asking.empty() ? std::string(kWhoisIana) : s.asking) + "...";
        return b;
    case WhoisSnapshot::State::Failed:
        b.text = s.error.empty() ? std::string("The query cannot be used") : s.error;
        b.tone = Tone::Bad;
        return b;
    case WhoisSnapshot::State::Cancelled:
        b.text = "Stopped";
        return b;
    case WhoisSnapshot::State::Done: break;
    }
    const WhoisResult &r = s.result;
    const WhoisAnswer *last_ok = nullptr;
    for (const WhoisAnswer &a : r.answers)
        if (a.ok) last_ok = &a;
    if (!last_ok) {
        b.text = "No whois server answered"; /* the reason is a row of the page */
        b.tone = Tone::Bad;
        return b;
    }
    if (whois_not_found(last_ok->lines)) {
        b.text = "No match for " + r.object + " at " + last_ok->server;
        b.tone = Tone::Warn;
        return b;
    }
    b.text = r.object + ": answered by " + last_ok->server;
    b.tone = Tone::Ok;
    return b;
}

} // namespace ui
} // namespace nt
