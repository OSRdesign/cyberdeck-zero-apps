/*
 * SPDX-License-Identifier: MIT
 *
 * Net Toolbox 0.3: the words and numbers of the speed test, Wake-on-LAN and Bluetooth radar pages (see view_model_03.hpp).
 */
#include "view_model_03.hpp"

#include "netaddr.hpp"
#include "text.hpp"

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>

namespace nt {
namespace ui {

namespace {

std::string fmt(const char *format, ...)
{
    char buf[256];
    va_list ap;
    va_start(ap, format);
    std::vsnprintf(buf, sizeof(buf), format, ap);
    va_end(ap);
    return buf;
}

double clamp01(double v) { return std::min(1.0, std::max(0.0, v)); }

std::string capitalized(std::string s)
{
    if (!s.empty() && s[0] >= 'a' && s[0] <= 'z') s[0] = static_cast<char>(s[0] - 'a' + 'A');
    return s;
}

bool contains(const std::string &s, const char *part) { return s.find(part) != std::string::npos; }

std::string plural(int n, const char *one, const char *many) { return std::to_string(n) + " " + (n == 1 ? one : many); }

/* a host as the status line shows it: a long one gives way to "the machine" */
std::string short_host(const std::string &host)
{
    return host.empty() || host.size() > 28 ? std::string("the machine") : host;
}

} // namespace

/* ================================================================ the dial */

const std::vector<double> &gauge_steps()
{
    static const std::vector<double> steps = {10, 25, 50, 100, 250, 500, 1000};
    return steps;
}

double gauge_scale_for(double mbps)
{
    for (const double s : gauge_steps())
        if (mbps <= 0.92 * s) return s;
    return gauge_steps().back();
}

void GaugeScaler::reset()
{
    scale = gauge_steps().front();
    max_seen = 0;
}

bool GaugeScaler::feed(double mbps)
{
    if (!std::isfinite(mbps)) return false;
    max_seen = std::max(max_seen, mbps);
    const double wanted = gauge_scale_for(max_seen);
    if (wanted > scale) {
        scale = wanted;
        return true;
    }
    return false;
}

double gauge_fraction(double value, double scale)
{
    if (!(scale > 0) || !std::isfinite(value)) return 0;
    return clamp01(value / scale);
}

double gauge_needle_deg(double value, double scale)
{
    return -kGaugeHalfSweepDeg + kGaugeSweepDeg * gauge_fraction(value, scale);
}

std::string gauge_number(double value)
{
    if (std::fabs(value - std::round(value)) < 0.001) return fmt("%.0f", value);
    return fmt("%.1f", value);
}

std::vector<GaugeTick> gauge_ticks(double scale)
{
    std::vector<GaugeTick> out;
    const double step = scale / 5.0;
    for (int i = 0; i <= 10; ++i) {
        GaugeTick t;
        t.value = step * i / 2.0;
        t.major = i % 2 == 0;
        if (t.major) t.label = gauge_number(t.value);
        out.push_back(t);
    }
    return out;
}

double ease_out(double from, double to, double elapsed_ms, double dur_ms)
{
    if (!(dur_ms > 0) || elapsed_ms >= dur_ms) return to;
    if (elapsed_ms <= 0) return from;
    const double t = elapsed_ms / dur_ms;
    const double e = 1.0 - (1.0 - t) * (1.0 - t) * (1.0 - t);
    return from + (to - from) * e;
}

void polar_point(int cx, int cy, double radius, double deg, double &x, double &y)
{
    const double rad = deg * 3.14159265358979323846 / 180.0;
    x = cx + radius * std::sin(rad);
    y = cy - radius * std::cos(rad);
}

GaugeRadii gauge_radii(int outer)
{
    GaugeRadii r;
    r.outer = outer;
    const int track = std::max(6, outer * 11 / 100);
    r.track = track;
    r.inner = outer - track;
    r.tick_major = std::max(5, outer * 8 / 100);
    r.tick_minor = std::max(3, outer * 45 / 1000);
    r.label = std::max(8, r.inner - r.tick_major - std::max(8, outer * 10 / 100));
    r.needle = std::max(6, r.inner - 3);
    r.hub = std::max(5, outer * 65 / 1000);
    return r;
}

/* ================================================================ speed test texts */

std::string speed_value_text(double mbps)
{
    if (!std::isfinite(mbps) || mbps < 0) return "--";
    if (mbps < 1.0) return fmt("%.2f", mbps);
    if (mbps < 99.95) return fmt("%.1f", mbps);
    return fmt("%.0f", mbps);
}

std::string ms_text(double ms)
{
    if (!std::isfinite(ms) || ms < 0) return "--";
    return ms < 10.0 ? fmt("%.1f ms", ms) : fmt("%.0f ms", ms);
}

std::string ms_delta_text(double ms)
{
    const double r = std::round(ms);
    return r >= 0 ? fmt("+%.0f ms", r) : fmt("%.0f ms", r);
}

std::string speed_note(std::uint64_t max_bytes)
{
    const double mb = static_cast<double>(max_bytes) / 1e6;
    return "Uses up to about " + fmt("%.0f", mb) + " MB each way. Start it yourself; on a phone hotspot that is mobile data.";
}

std::string speed_phase_label(const SpeedSnapshot &s)
{
    switch (s.state) {
    case SpeedSnapshot::State::Idle: return "Ready";
    case SpeedSnapshot::State::Done: return "Finished";
    case SpeedSnapshot::State::Cancelled: return "Stopped";
    case SpeedSnapshot::State::Failed: return "Failed";
    case SpeedSnapshot::State::Running: break;
    }
    switch (s.phase) {
    case SpeedSnapshot::Phase::Latency: return "Latency";
    case SpeedSnapshot::Phase::Download: return "Download";
    case SpeedSnapshot::Phase::Upload: return "Upload";
    case SpeedSnapshot::Phase::None: break;
    }
    return "Ready";
}

double speed_progress(const SpeedSnapshot &s, double target_s)
{
    if (!s.running() || !(target_s > 0)) return 0;
    if (s.phase != SpeedSnapshot::Phase::Download && s.phase != SpeedSnapshot::Phase::Upload) return 0;
    return clamp01(s.phase_s / target_s);
}

std::string speed_progress_text(const SpeedSnapshot &s, double target_s)
{
    if (!s.running()) return "";
    if (s.phase == SpeedSnapshot::Phase::Latency) return "Latency";
    const int shown = static_cast<int>(std::min(s.phase_s, target_s));
    return fmt("%d s of %.0f s", shown, target_s);
}

/* ---------------------------------------------------------------- the grade */

std::string grade_line(BloatGrade g)
{
    switch (g) {
    case BloatGrade::A: return "A: latency stays flat under load";
    case BloatGrade::B: return "B: latency grows a little under load";
    case BloatGrade::C: return "C: latency grows noticeably under load: calls and games may stutter";
    case BloatGrade::D: return "D: latency grows a lot under load: everything gets slow while the line is busy";
    case BloatGrade::F: return "F: latency explodes under load: heavy buffering in the router";
    case BloatGrade::Unknown: break;
    }
    return "Not measured";
}

Tone grade_tone(BloatGrade g)
{
    switch (g) {
    case BloatGrade::A:
    case BloatGrade::B: return Tone::Ok;
    case BloatGrade::C: return Tone::Warn;
    case BloatGrade::D:
    case BloatGrade::F: return Tone::Bad;
    case BloatGrade::Unknown: break;
    }
    return Tone::Normal;
}

Mark grade_mark(BloatGrade g)
{
    switch (g) {
    case BloatGrade::A:
    case BloatGrade::B: return Mark::Check;
    case BloatGrade::C: return Mark::Bang;
    case BloatGrade::D:
    case BloatGrade::F: return Mark::Cross;
    case BloatGrade::Unknown: break;
    }
    return Mark::Dash;
}

SpeedGrade speed_grade(const SpeedResult &r)
{
    SpeedGrade g;
    g.grade = r.grade;
    g.letter = grade_letter(r.grade);
    g.tone = grade_tone(r.grade);
    g.mark = grade_mark(r.grade);
    if (r.grade == BloatGrade::Unknown) {
        g.line = r.ping_note.empty() ? "Not measured: no latency under load" : "Not measured: " + r.ping_note;
        const std::size_t dot = g.line.find_last_not_of('.');
        if (dot != std::string::npos && dot + 1 < g.line.size()) g.line.erase(dot + 1);
    } else {
        g.line = grade_line(r.grade);
    }
    return g;
}

/* ---------------------------------------------------------------- the result card */

std::string speed_server_text(const SpeedResult &r)
{
    return r.colo.empty() ? std::string() : "Cloudflare, " + r.colo;
}

namespace {

void add_loaded_row(std::vector<RowDef> &rows, const char *label, const SpeedDirection &d)
{
    if (!d.tried || !d.measured) return;
    RowDef row;
    row.label = label;
    if (d.loaded.ok) {
        row.value = ms_text(d.loaded.median_ms);
        if (d.has_increase) row.value += " (" + ms_delta_text(d.increase_ms) + ")";
        row.tone = grade_tone(d.grade);
    } else {
        row.value = "not measured";
    }
    rows.push_back(row);
}

} // namespace

std::vector<RowDef> speed_rows(const SpeedSnapshot &s)
{
    const SpeedResult &r = s.result;
    std::vector<RowDef> rows;
    if (r.latency_ok || r.idle.ok) {
        RowDef lat;
        lat.label = "Latency (idle)";
        lat.value = ms_text(r.idle.ok ? r.idle.median_ms : r.latency_ms);
        rows.push_back(lat);
        RowDef jit;
        jit.label = "Jitter";
        jit.value = ms_text(r.idle.ok ? r.idle.jitter_ms : r.jitter_ms);
        rows.push_back(jit);
    }
    add_loaded_row(rows, "Latency while downloading", r.down);
    add_loaded_row(rows, "Latency while uploading", r.up);
    const std::string server = speed_server_text(r);
    if (!server.empty()) {
        RowDef row;
        row.label = "Server";
        row.value = server;
        rows.push_back(row);
    }
    return rows;
}

std::vector<std::string> speed_notes(const SpeedSnapshot &s)
{
    std::vector<std::string> out;
    const SpeedResult &r = s.result;
    if (!r.ping_note.empty() && r.grade == BloatGrade::Unknown) out.push_back(r.ping_note);
    if (!r.down.note.empty()) out.push_back("Download: " + r.down.note);
    if (!r.up.note.empty()) out.push_back("Upload: " + r.up.note);
    if (r.down.capped) out.push_back("The download reached its limit before the time was up.");
    if (r.up.capped) out.push_back("The upload reached its limit before the time was up.");
    return out;
}

std::string speed_error_title(const std::string &error)
{
    if (contains(error, "certificate")) return "Certificate not accepted";
    if (contains(error, "TLS")) return "Secure connection failed";
    if (contains(error, "refused") || contains(error, "HTTP 403") || contains(error, "HTTP 429")) return "Cloudflare refused the test";
    if (contains(error, "No connection") || contains(error, "cannot be found") || contains(error, "No answer in time"))
        return "No connection";
    return "Speed test failed";
}

Band speed_band(const SpeedSnapshot &s, double target_s)
{
    Band b;
    switch (s.state) {
    case SpeedSnapshot::State::Idle: b.text = "Press Start to test your connection"; break;
    case SpeedSnapshot::State::Running:
        if (s.phase == SpeedSnapshot::Phase::Latency) b.text = "Measuring latency";
        else if (s.phase == SpeedSnapshot::Phase::Download) b.text = "Downloading, " + speed_progress_text(s, target_s);
        else b.text = "Uploading, " + speed_progress_text(s, target_s);
        break;
    case SpeedSnapshot::State::Done: {
        const SpeedResult &r = s.result;
        b.text = "Done: " + speed_value_text(r.down.mbps) + " Mbit/s down, " + speed_value_text(r.up.mbps) + " up";
        if (r.grade != BloatGrade::Unknown) {
            b.text += std::string(", grade ") + grade_letter(r.grade);
            b.tone = grade_tone(r.grade);
        }
        break;
    }
    case SpeedSnapshot::State::Cancelled:
        b.text = "Stopped";
        b.tone = Tone::Warn;
        break;
    case SpeedSnapshot::State::Failed:
        b.text = speed_error_title(s.error);
        b.tone = Tone::Bad;
        break;
    }
    return b;
}

/* ---------------------------------------------------------------- the live block's layout */

SpeedLayout speed_layout(int w, int h, int lh, int big_h, int pad)
{
    SpeedLayout L;
    w = std::max(1, w);
    h = std::max(1, h);
    const int top = pad / 2 + 1;
    const int text_h = 2 * lh + big_h; /* phase label, big value, unit */
    const int prog_h = lh + pad;
    const auto radius_for = [&](int box_w, int box_h) {
        const int by_w = box_w / 2 - 2;
        const int by_h = static_cast<int>((box_h - top - text_h) / 1.08);
        return std::max(24, std::min(by_w, by_h));
    };
    /* side by side: the dial left, the chart and the progress right */
    const int chart_w = std::max(w * 36 / 100, 160);
    const int side_gw = std::max(1, w - chart_w - pad);
    const int r_side = radius_for(side_gw, h);
    /* stacked: the dial over the chart over the progress */
    const int chart_min_h = 3 * lh;
    const int r_stack = radius_for(w, h - chart_min_h - prog_h - 2 * pad);
    L.side = r_side >= r_stack * 95 / 100;
    if (L.side) {
        L.radius = r_side;
        L.gauge = {0, 0, side_gw, h};
        L.cx = side_gw / 2;
        const int block = top + L.radius + static_cast<int>(0.08 * L.radius) + text_h;
        L.cy = std::max(0, (h - block) / 2) + top + L.radius;
        const int cx0 = side_gw + pad;
        L.chart = {cx0, 0, std::max(1, w - cx0), std::max(1, h - prog_h - pad)};
        L.progress = {cx0, h - prog_h, std::max(1, w - cx0), prog_h};
    } else {
        L.radius = r_stack;
        const int gh = top + L.radius + static_cast<int>(0.08 * L.radius) + text_h;
        L.gauge = {0, 0, w, gh};
        L.cx = w / 2;
        L.cy = top + L.radius;
        const int cy0 = gh + pad;
        L.chart = {0, cy0, w, std::max(1, h - cy0 - prog_h - pad)};
        L.progress = {0, h - prog_h, w, prog_h};
    }
    return L;
}

/* ================================================================ Wake-on-LAN */

std::string wol_sent_text(const WolSendResult &r)
{
    if (r.sent == 0) return r.summary.empty() ? std::string("Nothing could be sent") : r.summary;
    std::vector<std::string> ok, failed;
    std::string first_failure;
    for (const WolPacket &p : r.packets) {
        std::vector<std::string> &into = p.error == 0 ? ok : failed;
        if (!p.dest.iface.empty() && std::find(into.begin(), into.end(), p.dest.iface) == into.end()) into.push_back(p.dest.iface);
        if (p.error != 0 && first_failure.empty()) first_failure = p.words;
    }
    std::string out = "Packet sent";
    if (!ok.empty()) {
        out += " on ";
        for (std::size_t i = 0; i < ok.size(); ++i) out += (i ? ", " : "") + ok[i];
    }
    if (r.errors > 0 && !failed.empty()) {
        out += ". Failed on ";
        for (std::size_t i = 0; i < failed.size(); ++i) out += (i ? ", " : "") + failed[i];
        if (!first_failure.empty()) out += ": " + first_failure;
    }
    return out;
}

WakeStatus wake_status(const WolSnapshot &s)
{
    WakeStatus w;
    w.seconds = s.waited_s;
    switch (s.state) {
    case WolSnapshot::State::Idle:
        w.kind = WakeKind::None;
        w.word = "Not sent yet";
        break;
    case WolSnapshot::State::Sending:
        w.kind = WakeKind::Sending;
        w.word = "Sending";
        w.spinning = true;
        break;
    case WolSnapshot::State::Watching:
        w.kind = WakeKind::Waking;
        w.word = "Waking... " + std::to_string(s.waited_s) + " s";
        w.detail = wol_sent_text(s.send);
        w.spinning = true;
        break;
    case WolSnapshot::State::Done:
        if (s.answered) {
            w.kind = WakeKind::Awake;
            w.tone = Tone::Ok;
            w.mark = Mark::Check;
            if (s.answered_at_once) {
                w.word = "Already awake";
                w.detail = "It answered the first ping.";
                w.seconds = 0;
            } else {
                w.seconds = static_cast<int>(std::max(1L, std::lround(s.answered_after_s)));
                w.word = "Awake after " + std::to_string(w.seconds) + " s";
                w.detail = wol_sent_text(s.send);
            }
        } else if (s.watch && s.watch_words.rfind("cannot watch", 0) != 0) {
            w.kind = WakeKind::NoAnswer;
            w.tone = Tone::Warn;
            w.mark = Mark::Dash;
            w.word = "No answer yet";
            w.detail = capitalized(s.watch_words.empty() ? std::string("no answer") : s.watch_words);
        } else {
            w.kind = WakeKind::Sent;
            w.mark = Mark::Arrow;
            w.word = "Sent";
            w.detail = wol_sent_text(s.send);
            if (s.watch && !s.watch_words.empty()) w.detail += ". " + capitalized(s.watch_words);
        }
        break;
    case WolSnapshot::State::Failed:
        w.kind = WakeKind::Failed;
        w.tone = Tone::Bad;
        w.mark = Mark::Cross;
        w.word = "Not sent";
        w.detail = s.error.empty() ? s.send.summary : s.error;
        break;
    case WolSnapshot::State::Cancelled:
        w.kind = WakeKind::Stopped;
        w.mark = Mark::Dash;
        if (s.send.sent > 0) {
            w.word = "Watch stopped";
            w.detail = wol_sent_text(s.send) + ". Stopped after " + std::to_string(s.waited_s) + " s.";
        } else {
            w.word = "Stopped";
        }
        break;
    }
    return w;
}

std::vector<StripCell> wake_strip(const WolSnapshot &s, int cells)
{
    std::vector<StripCell> out(static_cast<std::size_t>(std::max(0, cells)), StripCell::Future);
    const int n = static_cast<int>(out.size());
    const auto fill = [&](int from, int to, StripCell c) {
        for (int i = std::max(0, from); i < std::min(n, to); ++i) out[static_cast<std::size_t>(i)] = c;
    };
    switch (s.state) {
    case WolSnapshot::State::Watching:
    case WolSnapshot::State::Cancelled: fill(0, s.waited_s, StripCell::Waiting); break;
    case WolSnapshot::State::Done:
        if (s.answered) {
            const int a = std::min(n - 1, std::max(0, static_cast<int>(std::floor(s.answered_after_s))));
            fill(0, a, StripCell::Waiting);
            fill(a, n, StripCell::Awake);
        } else if (s.watch) {
            fill(0, n, StripCell::Waiting);
        }
        break;
    default: break;
    }
    return out;
}

WakeLast wake_last_of(const WolSnapshot &s, std::time_t sent_at)
{
    WakeLast l;
    l.sent_at = sent_at;
    switch (s.state) {
    case WolSnapshot::State::Idle: break;
    case WolSnapshot::State::Sending:
    case WolSnapshot::State::Watching: l.kind = WakeKind::Sent; break;
    case WolSnapshot::State::Done:
        if (s.answered) {
            l.kind = WakeKind::Awake;
            l.seconds = s.answered_at_once ? 0 : static_cast<int>(std::max(1L, std::lround(s.answered_after_s)));
        } else if (s.watch && s.watch_words.rfind("cannot watch", 0) != 0) {
            l.kind = WakeKind::NoAnswer;
        } else {
            l.kind = WakeKind::Sent;
        }
        break;
    case WolSnapshot::State::Failed: l.kind = WakeKind::Failed; break;
    case WolSnapshot::State::Cancelled: l.kind = s.send.sent > 0 ? WakeKind::Sent : WakeKind::None; break;
    }
    return l;
}

std::string wake_last_text(const WakeLast &last)
{
    switch (last.kind) {
    case WakeKind::Sent:
    case WakeKind::Sending:
    case WakeKind::Waking:
    case WakeKind::Stopped: {
        char buf[16] = "";
        struct tm tm{};
        localtime_r(&last.sent_at, &tm);
        std::strftime(buf, sizeof(buf), "%H:%M", &tm);
        return std::string("Sent ") + buf;
    }
    case WakeKind::Awake: return last.seconds > 0 ? "Awake after " + std::to_string(last.seconds) + " s" : std::string("Already awake");
    case WakeKind::NoAnswer: return "No answer yet";
    case WakeKind::Failed: return "Not sent";
    case WakeKind::None: break;
    }
    return "Not woken yet";
}

Tone wake_last_tone(const WakeLast &last)
{
    switch (last.kind) {
    case WakeKind::Awake: return Tone::Ok;
    case WakeKind::NoAnswer: return Tone::Warn;
    case WakeKind::Failed: return Tone::Bad;
    default: return Tone::Normal;
    }
}

Band wol_band(const WolSnapshot &s, const std::string &machine)
{
    (void)machine;
    Band b;
    const std::string host = short_host(s.watch_target);
    switch (s.state) {
    case WolSnapshot::State::Idle: b.text = "Press Wake now to send the magic packet"; break;
    case WolSnapshot::State::Sending: b.text = "Sending the magic packet"; break;
    case WolSnapshot::State::Watching:
        b.text = "Packet sent, waiting for " + host + ": " + std::to_string(s.waited_s) + " s";
        break;
    case WolSnapshot::State::Done:
        if (s.answered && s.answered_at_once) {
            b.text = host + " answered at once: probably awake already";
            b.tone = Tone::Ok;
        } else if (s.answered) {
            b.text = host + " answers after " + std::to_string(static_cast<int>(std::max(1L, std::lround(s.answered_after_s)))) + " s";
            b.tone = Tone::Ok;
        } else if (s.watch && s.watch_words.rfind("cannot watch", 0) != 0) {
            b.text = "No answer from " + host + " after " + std::to_string(s.waited_s) + " s";
            b.tone = Tone::Warn;
        } else {
            b.text = "Magic packet sent";
            if (s.send.errors > 0) b.tone = Tone::Warn;
        }
        break;
    case WolSnapshot::State::Failed:
        b.text = "Nothing was sent";
        b.tone = Tone::Bad;
        break;
    case WolSnapshot::State::Cancelled: b.text = "Stopped"; b.tone = Tone::Warn; break;
    }
    return b;
}

std::string wol_name_problem(const std::string &name_in, const Settings &settings, const std::string &own)
{
    const std::string name = trim(name_in);
    if (name.empty()) return "Type a name for the machine";
    WolMachine m;
    m.name = name;
    m.mac = "02:00:00:00:00:01";
    std::string why;
    if (!normalize_machine(m, &why)) return why.empty() ? std::string("This name cannot be used") : why;
    for (const WolMachine &other : settings.wol_machines)
        if (lower(other.name) == lower(name) && lower(other.name) != lower(own)) return "A machine called " + other.name + " is already saved";
    return "";
}

std::string wol_mac_problem(const std::string &text)
{
    MacAddr mac;
    std::string why;
    if (parse_mac(text, mac, &why)) return "";
    /* the core's long explanation of a wrong form does not fit the line under the field: the short one says it */
    if (why.empty() || why.size() > 60) return "Not a MAC address: type 12 hex digits, like aa:bb:cc:dd:ee:ff";
    return why;
}

std::string wol_host_problem(const std::string &text)
{
    const std::string host = trim(text);
    if (host.empty()) return "";
    std::string why;
    return valid_host(host, &why) ? std::string() : why;
}

/* ================================================================ Bluetooth radar */

BlePlain ble_problem(const std::string &error)
{
    BlePlain p;
    p.words = error;
    if (contains(error, "no Bluetooth adapter")) {
        p.headline = "No Bluetooth adapter";
        p.fix = "Use a board with Bluetooth, or plug in a USB Bluetooth adapter";
    } else if (contains(error, "hardware switch")) {
        p.headline = "Bluetooth is blocked by a switch";
        p.fix = "Turn the board's wireless switch on";
    } else if (contains(error, "rfkill")) {
        p.headline = "Bluetooth is blocked by rfkill";
        p.fix = "Turn Bluetooth on in Settings > Bluetooth";
    } else if (contains(error, "powered off")) {
        p.headline = "Bluetooth is switched off";
        p.fix = "Turn Bluetooth on in Settings > Bluetooth";
    } else if (contains(error, "bluez") || contains(error, "bluetoothctl is missing")) {
        p.headline = "Bluetooth is not installed";
        p.fix = "Install the bluez package: sudo apt install bluez";
    } else if (contains(error, "bluetoothd")) {
        p.headline = "Bluetooth service is not running";
        p.fix = "Start it with: sudo systemctl start bluetooth";
    } else {
        p.headline = "The scan could not run";
    }
    return p;
}

Band ble_band(const BleSnapshot &s)
{
    Band b;
    switch (s.state) {
    case BleSnapshot::State::Idle: b.text = "Press Start to scan for Bluetooth devices"; break;
    case BleSnapshot::State::Checking: b.text = "Checking Bluetooth"; break;
    case BleSnapshot::State::Scanning: {
        const int left = std::max(0, static_cast<int>(std::ceil(s.seconds - s.elapsed_s)));
        b.text = "Scanning, " + plural(s.live, "device", "devices") + ", " + std::to_string(left) + " s left";
        break;
    }
    case BleSnapshot::State::Done: b.text = "Finished, " + std::to_string(s.seen) + (s.seen == 1 ? " device seen" : " devices seen"); break;
    case BleSnapshot::State::Cancelled: b.text = "Stopped, " + std::to_string(s.seen) + (s.seen == 1 ? " device seen" : " devices seen"); break;
    case BleSnapshot::State::Failed: {
        const BlePlain p = ble_problem(s.error);
        b.text = p.headline;
        b.tone = p.fix.empty() ? Tone::Bad : Tone::Warn;
        break;
    }
    }
    return b;
}

double blip_angle_deg(const std::string &address)
{
    std::uint32_t h = 2166136261u;
    for (const char ch : address) {
        char c = ch;
        if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
        h ^= static_cast<std::uint8_t>(c);
        h *= 16777619u;
    }
    return static_cast<double>(h % 3600u) / 10.0;
}

double blip_radius_frac(double dbm)
{
    if (!std::isfinite(dbm)) return 0.95;
    double lo, hi, t;
    if (dbm >= -60.0) { /* near: inside the inner ring */
        lo = 0.10;
        hi = 0.31;
        t = clamp01((-30.0 - dbm) / 30.0);
    } else if (dbm >= -80.0) { /* medium: between the rings */
        lo = 0.37;
        hi = 0.64;
        t = clamp01((-60.0 - dbm) / 20.0);
    } else { /* far: outside the middle ring */
        lo = 0.70;
        hi = 0.95;
        t = clamp01((-80.0 - dbm) / 20.0);
    }
    return lo + (hi - lo) * t;
}

int blip_alpha(std::int64_t age_ms, int expiry_ms, double sweep_deg, double angle_deg)
{
    if (expiry_ms <= 0 || age_ms >= expiry_ms) return 0;
    const double base = clamp01(1.0 - static_cast<double>(std::max<std::int64_t>(0, age_ms)) / expiry_ms);
    const double fade = 0.25 + 0.75 * base;
    double pulse = 0.35;
    if (sweep_deg >= 0) {
        const double d = std::fmod(sweep_deg - angle_deg + 720.0, 360.0); /* degrees since the sweep passed the blip */
        pulse = d < 120.0 ? 1.0 - d / 120.0 : 0.0;
    }
    const double a = fade * (0.40 + 0.60 * pulse) * 255.0;
    return static_cast<int>(std::lround(std::min(255.0, std::max(0.0, a))));
}

double sweep_deg_at(std::int64_t ms)
{
    const double m = std::fmod(static_cast<double>(ms), kRadarTurnMs);
    return (m < 0 ? m + kRadarTurnMs : m) / kRadarTurnMs * 360.0;
}

std::string ble_row_label(const BleDevice &d)
{
    return d.name.empty() ? d.address : d.name;
}

std::string ble_row_value(const BleDevice &d)
{
    if (!d.has_rssi) return "no signal";
    std::string v = fmt("%.0f dBm", std::round(d.smooth_dbm));
    const char *cls = distance_words(distance_class(d.smooth_dbm));
    if (cls && cls[0]) v += std::string(", ") + cls;
    return v;
}

std::string ago_text(std::int64_t ms)
{
    if (ms < 1000) return "just now";
    const std::int64_t s = ms / 1000;
    if (s < 60) return std::to_string(s) + " s ago";
    const std::int64_t m = s / 60, rest = s % 60;
    return rest ? std::to_string(m) + " min " + std::to_string(rest) + " s ago" : std::to_string(m) + " min ago";
}

std::vector<RowDef> ble_detail_rows(const BleDevice &d, std::int64_t now_ms)
{
    std::vector<RowDef> rows;
    const auto add = [&](const char *label, const std::string &value) {
        if (value.empty()) return;
        RowDef r;
        r.label = label;
        r.value = value;
        rows.push_back(r);
    };
    if (!d.name.empty()) add("Name", d.name);
    add("Address", d.address);
    add("Address type", d.address_type);
    add("Vendor", d.vendor);
    if (d.has_rssi) {
        add("Signal", fmt("%d dBm", d.rssi_dbm));
        const char *cls = distance_words(distance_class(d.smooth_dbm));
        add("Smoothed signal", fmt("%.0f dBm", std::round(d.smooth_dbm)) + (cls && cls[0] ? std::string(", ") + cls : std::string()));
    }
    if (d.has_tx) add("TX power", fmt("%d dBm", d.tx_dbm));
    add("Last seen", ago_text(now_ms - d.last_seen_ms));
    add("Heard", d.readings == 1 ? std::string("1 time") : std::to_string(d.readings) + " times");
    return rows;
}

BleLayout ble_layout(int w, int h, int row_h, int gap)
{
    BleLayout L;
    w = std::max(1, w);
    h = std::max(1, h);
    L.side = h * 100 < w * 75; /* a wide area: side by side; a tall one: stacked */
    if (L.side) {
        int d = std::min({h, w / 2, w - gap - 240});
        d = std::max(std::min(120, std::min(h, w)), d);
        L.radar = {0, (h - d) / 2, d, d};
        L.list = {d + gap, 0, std::max(1, w - d - gap), h};
    } else {
        int d = static_cast<int>(std::max(250.0, std::min<double>(w, h * 0.62)));
        d = std::min({d, w, h - gap - 3 * row_h});
        d = std::max(60, d);
        L.radar = {(w - d) / 2, 0, d, d};
        L.list = {0, d + gap, w, std::max(1, h - d - gap)};
    }
    return L;
}

} // namespace ui
} // namespace nt
