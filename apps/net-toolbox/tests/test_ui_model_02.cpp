/*
 * SPDX-License-Identifier: MIT
 *
 * Net Toolbox 0.2 tests: what the new pages say (src/ui/view_model_02.cpp, compiled into test_ui_model.cpp): the days-left
 * wording, the hop looks and texts, the bar scale, the trace status line and verdict, the waterfall, the HTTP badges, the
 * public address rows and the whois summary.
 */
#include "check.hpp"

#include "../src/ui/view_model.hpp"

#include <ctime>

using namespace nt;
using namespace nt::ui;

namespace {

TraceHop t_hop(int ttl, int sent, int lost, double avg, const char *addr = "10.0.0.1", const char *name = "")
{
    TraceHop h;
    h.ttl = ttl;
    h.stats.transmitted = sent;
    h.stats.lost = lost;
    h.stats.received = sent - lost;
    if (h.stats.received > 0) {
        h.stats.sum_ms = avg * h.stats.received;
        h.stats.min_ms = avg * 0.8;
        h.stats.max_ms = avg * 1.5;
        h.stats.last_ms = avg;
    }
    if (addr[0] && h.stats.received > 0) {
        TraceHopAddress a;
        a.address = addr;
        a.name = name;
        a.count = h.stats.received;
        h.addresses.push_back(a);
    }
    return h;
}

int widths(const std::vector<WaterSeg> &w)
{
    int sum = 0;
    for (const WaterSeg &g : w) sum += g.w;
    return sum;
}

} // namespace

void test_ui_model_02()
{
    // days left in words and tones: green from 30, amber under 30, red under 7 and once expired
    CHECK_EQ(days_words(83), std::string("expires in 83 days"));
    CHECK_EQ(days_words(5), std::string("expires in 5 days"));
    CHECK_EQ(days_words(1), std::string("expires in 1 day"));
    CHECK_EQ(days_words(0), std::string("expires today"));
    CHECK_EQ(days_words(-1), std::string("expired 1 day ago"));
    CHECK_EQ(days_words(-12), std::string("expired 12 days ago"));
    CHECK(days_tone(83) == Tone::Ok && days_tone(30) == Tone::Ok && days_tone(29) == Tone::Warn);
    CHECK(days_tone(7) == Tone::Warn && days_tone(6) == Tone::Bad && days_tone(0) == Tone::Bad && days_tone(-3) == Tone::Bad);
    CHECK_EQ(size_text(0), std::string("0 bytes"));
    CHECK_EQ(size_text(1), std::string("1 byte"));
    CHECK_EQ(size_text(812), std::string("812 bytes"));
    CHECK_EQ(size_text(1256), std::string("1.3 kB"));
    CHECK_EQ(size_text(345000), std::string("345 kB"));
    CHECK_EQ(size_text(3400000), std::string("3.4 MB"));

    // the node of a hop
    CHECK(hop_look(t_hop(1, 10, 0, 12)) == HopLook::Ok);
    CHECK(hop_look(t_hop(1, 10, 0, 151)) == HopLook::Slow); // over 150 ms
    CHECK(hop_look(t_hop(1, 10, 0, 150)) == HopLook::Ok);
    CHECK(hop_look(t_hop(1, 10, 1, 12)) == HopLook::Slow);  // 10 % loss
    CHECK(hop_look(t_hop(1, 10, 2, 12)) == HopLook::Slow);  // exactly 20 %
    CHECK(hop_look(t_hop(1, 10, 3, 12)) == HopLook::Lossy); // 30 %
    TraceHop limited = t_hop(1, 10, 3, 12);
    limited.loss_only_here = true;
    CHECK(hop_look(limited) == HopLook::RateLimited); // loss here, clean later: never red
    TraceHop silent = t_hop(1, 10, 10, 0, "");
    silent.silent = true;
    CHECK(hop_look(silent) == HopLook::Silent);
    TraceHop waiting;
    waiting.ttl = 3;
    waiting.stats.transmitted = 1;
    CHECK(hop_look(waiting) == HopLook::Pending); // nothing resolved yet
    TraceHop unreach = t_hop(5, 10, 0, 12);
    unreach.unreachable = "host unreachable";
    CHECK(hop_look(unreach) == HopLook::Lossy);
    CHECK_EQ(std::string(hop_look_word(HopLook::Silent)), "no reply");

    // loss tags, names and notes
    CHECK_EQ(loss_tag(t_hop(1, 10, 3, 12)), std::string("30 %"));
    CHECK_EQ(loss_tag(t_hop(1, 200, 1, 12)), std::string("1 %")); // any loss shows
    CHECK_EQ(loss_tag(t_hop(1, 10, 0, 12)), std::string());
    CHECK_EQ(loss_tag(silent), std::string()); // a silent hop is not "100 % lost"
    HopTexts t = hop_texts(t_hop(2, 10, 0, 43.2, "62.1.2.3", "core-b.example"));
    CHECK_EQ(t.number, std::string("2"));
    CHECK_EQ(t.name, std::string("core-b.example"));
    CHECK_EQ(t.ip, std::string("62.1.2.3"));
    CHECK_EQ(t.avg, std::string("43 ms"));
    CHECK(t.has_bar && t.note.empty());
    t = hop_texts(t_hop(2, 10, 0, 43.2, "62.1.2.3"));
    CHECK_EQ(t.name, std::string("62.1.2.3"));
    CHECK_EQ(t.ip, std::string()); // the address is the name: not twice
    TraceHop dest = t_hop(9, 10, 0, 43.2, "93.184.215.14");
    dest.destination = true;
    t = hop_texts(dest);
    CHECK(t.destination);
    CHECK_EQ(t.note, std::string("reached"));
    t = hop_texts(limited);
    CHECK_EQ(t.note, std::string("probably rate limiting"));
    t = hop_texts(silent);
    CHECK_EQ(t.bar_word, std::string("no reply"));
    CHECK(!t.has_bar);
    CHECK_EQ(hop_texts(waiting).bar_word, std::string("waiting"));
    TraceHop balanced = t_hop(4, 10, 0, 12, "10.0.0.1");
    TraceHopAddress second;
    second.address = "10.0.0.2";
    balanced.addresses.push_back(second);
    CHECK_EQ(hop_texts(balanced).ip, std::string("+1 more"));

    // the bar: one linear scale, a stub for a tiny value, never past the end
    std::vector<TraceHop> hops = {t_hop(1, 10, 0, 1.3), t_hop(2, 10, 0, 40), t_hop(3, 10, 10, 0, "")};
    CHECK_NEAR(chain_scale_ms(hops, 3), 40, 1e-9);
    CHECK_NEAR(chain_scale_ms(hops, 1), 1.3, 1e-9);
    CHECK_NEAR(chain_scale_ms({}, 0), 1.0, 1e-9);
    CHECK_EQ(bar_px(40, 40, 100, 3), 100);
    CHECK_EQ(bar_px(20, 40, 100, 3), 50);
    CHECK_EQ(bar_px(0.4, 40, 100, 3), 3); // a stub, still visible
    CHECK_EQ(bar_px(0, 40, 100, 3), 0);
    CHECK_EQ(bar_px(80, 40, 100, 3), 100); // clipped
    CHECK_EQ(bar_px(5, 40, 2, 3), 2);      // the stub never exceeds the column

    // the status line and the verdict
    TraceSnapshot s;
    s.state = TraceSnapshot::State::Running;
    s.target = "example.com";
    s.address = "93.184.215.14";
    s.rounds = 3;
    s.hops = {t_hop(1, 3, 0, 1.3), t_hop(2, 3, 0, 8)};
    s.shown = 2;
    CHECK_EQ(trace_progress(s), std::string("to example.com 93.184.215.14, round 3, 2 hops"));
    CHECK_EQ(trace_progress(s, false), std::string("to example.com, round 3, 2 hops"));
    s.rounds = 0;
    s.shown = 1;
    CHECK_EQ(trace_progress(s), std::string("to example.com 93.184.215.14, round 1, 1 hop"));
    s.target = "1.1.1.1";
    s.address = "1.1.1.1";
    CHECK_EQ(trace_progress(s), std::string("to 1.1.1.1, round 1, 1 hop"));
    CHECK(trace_band(s, "").tone == Tone::Ok);
    TraceSnapshot idle;
    CHECK_EQ(trace_band(idle, "Gateway 192.168.68.1").text, std::string("Press Start to trace Gateway 192.168.68.1"));
    TraceSnapshot done;
    done.state = TraceSnapshot::State::Cancelled;
    done.reached = true;
    done.dest_hop = 9;
    for (int i = 1; i <= 9; ++i) done.hops.push_back(t_hop(i, 14, 0, i == 9 ? 43.2 : 10.0));
    done.hops[8].destination = true;
    done.shown = 9;
    CHECK_EQ(trace_verdict(done), std::string("Reached in 9 hops, 43 ms"));
    CHECK(trace_band(done, "").tone == Tone::Ok);
    TraceSnapshot one = done;
    one.dest_hop = 1;
    one.hops.resize(1);
    one.hops[0].stats = t_hop(1, 14, 0, 0.4).stats;
    CHECK_EQ(trace_verdict(one), std::string("Reached in 1 hop, 0.4 ms"));
    TraceSnapshot lost = done;
    lost.reached = false;
    lost.dest_hop = 0;
    lost.shown = 6;
    CHECK_EQ(trace_verdict(lost), std::string("Did not reach the destination: no answer after hop 6"));
    CHECK(trace_band(lost, "").tone == Tone::Warn);
    lost.shown = 0;
    CHECK_EQ(trace_verdict(lost), std::string("Did not reach the destination: no hop answered"));
    TraceSnapshot router = lost;
    router.unreachable = true;
    router.dest_hop = 5;
    router.hops[4].unreachable = "host unreachable";
    CHECK_EQ(trace_verdict(router), std::string("Did not reach the destination: unreachable at hop 5"));
    CHECK(trace_band(router, "").tone == Tone::Bad);
    TraceSnapshot failed;
    failed.state = TraceSnapshot::State::Failed;
    failed.error = "Cannot find x: no such name (NXDOMAIN)";
    CHECK(trace_band(failed, "").tone == Tone::Bad);
    CHECK_EQ(trace_band(failed, "").text, failed.error);

    // the details of a hop
    HopDetail d = hop_detail(limited);
    CHECK(d.note.find("rate limiting") != std::string::npos);
    CHECK(d.counts.find("sent 10  received 7  lost 3 (") == 0);
    d = hop_detail(dest);
    CHECK(d.note.find("This is the destination.") == 0);
    CHECK(d.timing.find("avg 43 ms") != std::string::npos && d.timing.find("worst 65 ms") != std::string::npos);
    d = hop_detail(silent);
    CHECK_EQ(d.addresses, std::string("no address answered"));
    CHECK_EQ(d.timing, std::string("no answer yet"));

    // the waterfall: one scale, tiny segments stay visible, zero ones take no room, the widths add up
    HttpTimings tm;
    tm.valid = tm.connected = tm.tls = tm.first_byte = true;
    tm.dns_ms = 12;
    tm.connect_ms = 18;
    tm.tls_ms = 31;
    tm.wait_ms = 94;
    tm.transfer_ms = 28;
    tm.total_ms = 183;
    std::vector<WaterSeg> w = waterfall(tm, 400);
    CHECK_EQ(w.size(), std::size_t(5));
    CHECK_EQ(widths(w), 400);
    CHECK_EQ(w[0].x, 0);
    CHECK_EQ(w[1].x, w[0].w);
    CHECK_EQ(w[4].x + w[4].w, 400);
    CHECK(w[3].w > w[2].w && w[2].w > w[1].w); // Waiting is the longest here
    CHECK_EQ(std::string(w[0].name), "DNS");
    CHECK_EQ(water_label(w[3]), std::string("Waiting 94 ms"));
    CHECK_EQ(waterfall_total(tm), std::string("Total 183 ms"));
    tm.dns_ms = 0.2;     // a tiny one: 2 px at least
    tm.transfer_ms = 0;  // a zero one: no room
    tm.total_ms = 143;
    w = waterfall(tm, 300);
    CHECK(w[0].w >= 2);
    CHECK_EQ(w[4].w, 0);
    CHECK_EQ(widths(w), 300);
    tm.tls = false;
    tm.first_byte = false; // a refused connection: DNS and connect only
    w = waterfall(tm, 300);
    CHECK_EQ(w.size(), std::size_t(2));
    CHECK_EQ(widths(w), 300);
    HttpTimings none;
    CHECK(waterfall(none, 300).empty());
    HttpTimings zero;
    zero.valid = true;
    w = waterfall(zero, 300);
    CHECK_EQ(w.size(), std::size_t(1));
    CHECK_EQ(w[0].w, 0);
    HttpTimings narrow;
    narrow.valid = narrow.connected = true;
    narrow.dns_ms = narrow.connect_ms = 0.01;
    w = waterfall(narrow, 10);
    CHECK_EQ(widths(w), 10);
    CHECK(w[0].w >= 2 && w[1].w >= 2);

    // the status badge
    const auto snap_with = [](int status, const char *reason) {
        HttpCheckSnapshot sn;
        sn.state = HttpCheckSnapshot::State::Done;
        sn.result.answered = true;
        sn.result.status = status;
        sn.result.reason = reason;
        sn.result.url = "https://example.com";
        return sn;
    };
    PortSummary b = http_badge(snap_with(200, "OK"));
    CHECK_EQ(b.word, std::string("200 OK"));
    CHECK(b.tone == Tone::Ok && b.mark == Mark::Check);
    b = http_badge(snap_with(301, "Moved Permanently"));
    CHECK_EQ(b.word, std::string("301 moved"));
    CHECK(b.tone == Tone::Warn && b.mark == Mark::Arrow);
    b = http_badge(snap_with(404, "Not Found"));
    CHECK_EQ(b.word, std::string("404 Not Found"));
    CHECK(b.tone == Tone::Bad && b.mark == Mark::Cross);
    CHECK(http_badge(snap_with(500, "Internal Server Error")).tone == Tone::Bad);
    HttpCheckSnapshot err;
    err.state = HttpCheckSnapshot::State::Done;
    err.result.problem = HttpProblem::Resolve;
    err.result.problem_words = "The name nx.example cannot be found (DNS). Check the spelling or the DNS server.";
    b = http_badge(err);
    CHECK_EQ(b.word, std::string("Name not found"));
    CHECK(b.tone == Tone::Bad && b.mark == Mark::Cross);
    CHECK_EQ(b.detail, err.result.problem_words);
    HttpCheckSnapshot running;
    running.state = HttpCheckSnapshot::State::Running;
    CHECK_EQ(http_badge(running).word, std::string("Checking..."));
    CHECK(http_band(snap_with(200, "OK")).tone == Tone::Ok);

    // the certificate badge
    HttpCheckResult cr;
    cr.url = "https://example.com";
    cr.cert.present = true;
    cr.cert.has_days = true;
    cr.cert.days_left = 83;
    cr.cert.subject = "CN = example.com";
    CertBadge cb = cert_badge(cr);
    CHECK_EQ(cb.word, std::string("expires in 83 days"));
    CHECK(cb.tone == Tone::Ok && cb.mark == Mark::Check);
    cr.cert.days_left = 20;
    CHECK(cert_badge(cr).tone == Tone::Warn && cert_badge(cr).mark == Mark::Bang);
    cr.cert.days_left = 5;
    CHECK_EQ(cert_badge(cr).word, std::string("expires in 5 days"));
    CHECK(cert_badge(cr).tone == Tone::Bad);
    cr.cert.days_left = -12;
    cr.cert.expired = true;
    CHECK_EQ(cert_badge(cr).word, std::string("expired 12 days ago"));
    CHECK(cert_badge(cr).mark == Mark::Cross);
    HttpCheckResult plain;
    plain.url = "http://example.com";
    CHECK_EQ(cert_badge(plain).word, std::string("no certificate"));
    plain.url = "https://example.com";
    CHECK_EQ(cert_badge(plain).word, std::string("not read"));

    // the rows around the blocks
    HttpCheckResult rr;
    rr.url = "http://example.com";
    rr.final_url = "https://www.example.com/";
    rr.answered = true;
    rr.http_version = "HTTP/2";
    rr.remote_ip = "93.184.215.14";
    rr.remote_port = 443;
    rr.size_bytes = 1256;
    rr.redirects = 1;
    CHECK_EQ(http_final_rows(rr).size(), std::size_t(1));
    CHECK_EQ(http_final_rows(rr)[0].value, std::string("https://www.example.com/"));
    rr.final_url = "http://example.com/";
    CHECK(http_final_rows(rr).empty()); // a trailing slash is not a change
    std::vector<RowDef> info = http_info_rows(rr);
    CHECK_EQ(info.size(), std::size_t(4));
    CHECK_EQ(info[0].value, std::string("93.184.215.14:443"));
    CHECK_EQ(info[2].value, std::string("1.3 kB"));
    CHECK_EQ(info[3].value, std::string("1"));
    rr.head = true;
    CHECK(http_info_rows(rr)[2].value.find("none") == 0);

    // the public address
    CHECK_EQ(country_text("CH"), std::string("Switzerland (CH)"));
    CHECK_EQ(country_text("ch"), std::string("Switzerland (CH)"));
    CHECK_EQ(country_text("XX"), std::string("XX"));
    PublicIpSnapshot pi;
    pi.state = PublicIpSnapshot::State::Done;
    pi.result.v4.tried = pi.result.v4.ok = true;
    pi.result.v4.trace.ip = "198.51.100.23";
    pi.result.v4.trace.loc = "DE";
    pi.result.v4.trace.colo = "FRA";
    pi.result.v6.tried = true;
    pi.result.v6.error = "no IPv6";
    std::vector<RowDef> rows = publicip_rows(pi, "100.64.8.3/24");
    CHECK_EQ(rows.size(), std::size_t(5));
    CHECK_EQ(rows[0].value, std::string("198.51.100.23"));
    CHECK_EQ(rows[1].value, std::string("none")); // no IPv6 is normal: neutral
    CHECK(rows[1].tone == Tone::Normal);
    CHECK_EQ(rows[4].value, std::string("100.64.8.3 (carrier-grade NAT)"));
    CHECK(nat_note("100.64.8.3", pi.result).find("carrier-grade NAT") != std::string::npos);
    CHECK(nat_note("192.168.1.5", pi.result).empty());
    pi.result.v4.ok = false;
    pi.result.v4.error = "no connection to Cloudflare";
    rows = publicip_rows(pi, "");
    CHECK_EQ(rows[0].value, std::string("not found: no connection to Cloudflare"));
    CHECK(rows[0].tone == Tone::Bad);
    CHECK_EQ(rows.back().value, std::string("none"));
    CHECK(publicip_address(pi).empty());

    // whois
    std::int64_t epoch = 0;
    std::string iso;
    CHECK(whois_day("2027-01-06T04:00:00Z", epoch, &iso));
    CHECK_EQ(iso, std::string("2027-01-06"));
    CHECK(whois_day("13-aug-2026", epoch, &iso));
    CHECK_EQ(iso, std::string("2026-08-13"));
    CHECK(whois_day("2026.08.13", epoch, &iso));
    CHECK(whois_day("2026/08/13", epoch, &iso));
    CHECK(!whois_day("soon", epoch, &iso));
    CHECK(!whois_day("2026-13-40", epoch, &iso));
    WhoisResult wr;
    wr.object = "example.com";
    wr.summary.registrar = "RESERVED-Internet Assigned Numbers Authority";
    wr.summary.created = "1995-08-14T04:00:00Z";
    wr.summary.expires = "2027-01-06T04:00:00Z";
    wr.summary.name_servers = {"a.iana-servers.net", "b.iana-servers.net"};
    wr.summary.status = {"clientDeleteProhibited", "clientTransferProhibited"};
    std::tm now_tm{};
    now_tm.tm_year = 126;
    now_tm.tm_mon = 9;
    now_tm.tm_mday = 15;
    now_tm.tm_hour = 12;
    const std::time_t now = timegm(&now_tm);
    rows = whois_rows(wr, now);
    CHECK_EQ(rows[0].label, std::string("Registrar"));
    CHECK_EQ(rows[0].value, wr.summary.registrar); // never cut
    CHECK_EQ(rows[1].value, std::string("1995-08-14"));
    CHECK_EQ(rows[2].label, std::string("Expires"));
    CHECK_EQ(rows[2].value, std::string("2027-01-06 (expires in 83 days)"));
    CHECK(rows[2].tone == Tone::Ok);
    CHECK_EQ(rows.size(), std::size_t(6)); // empty fields (Updated) are not rows
    CHECK_EQ(rows[3].value, std::string("clientDeleteProhibited, clientTransferProhibited"));
    CHECK_EQ(rows[4].label, std::string("Name server"));
    wr.summary.expires = "2026-10-03T00:00:00Z";
    CHECK_EQ(whois_rows(wr, now)[2].value, std::string("2026-10-03 (expired 12 days ago)"));
    CHECK(whois_rows(wr, now)[2].tone == Tone::Bad);
    WhoisResult ip;
    ip.is_ip = true;
    ip.summary.netname = "GOGL";
    ip.summary.country = "US";
    ip.summary.cidr = "8.8.8.0/24";
    rows = whois_rows(ip, now);
    CHECK_EQ(rows.size(), std::size_t(3));
    CHECK_EQ(rows[0].label, std::string("Net name"));
    CHECK_EQ(rows[1].value, std::string("United States (US)"));
    CHECK_EQ(rows[2].label, std::string("Range"));
    WhoisAnswer a1, a2;
    a1.server = "whois.iana.org";
    a1.ok = true;
    a1.lines = {"refer: whois.arin.net", "", "x"};
    a2.server = "whois.arin.net";
    a2.ok = true;
    a2.lines = {"NetName: GOGL", "y"};
    ip.answers = {a1, a2};
    std::size_t more = 0;
    std::vector<std::string> raw = whois_raw(ip, 100, &more);
    CHECK_EQ(raw.size(), std::size_t(7));
    CHECK_EQ(raw[0], std::string("-- whois.iana.org --"));
    CHECK_EQ(more, std::size_t(0));
    raw = whois_raw(ip, 4, &more);
    CHECK_EQ(raw.size(), std::size_t(4));
    CHECK_EQ(more, std::size_t(3));
    WhoisSnapshot ws;
    ws.state = WhoisSnapshot::State::Done;
    ws.result = ip;
    ws.result.object = "8.8.8.8";
    CHECK_EQ(whois_band(ws).text, std::string("8.8.8.8: answered by whois.arin.net"));
    ws.result.answers.back().lines = {"No match for \"X\"."};
    CHECK(whois_band(ws).tone == Tone::Warn);
    ws.result.answers[0].ok = ws.result.answers[1].ok = false;
    CHECK_EQ(whois_band(ws).text, std::string("No whois server answered"));
    CHECK(whois_band(ws).tone == Tone::Bad);
}
