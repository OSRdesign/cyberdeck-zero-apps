/*
 * SPDX-License-Identifier: MIT
 *
 * Net Toolbox 0.3: the speed test (see speedtest.hpp).
 */
#include "speedtest.hpp"

#include "text.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace nt {
namespace {

template <typename T> T clampv(T v, T lo, T hi)
{
    return std::min(hi, std::max(lo, v));
}

/* fields split by '|', at most `max` (the last keeps the rest) */
std::vector<std::string> split_bars(const std::string &s, std::size_t max)
{
    std::vector<std::string> out;
    std::size_t from = 0;
    while (out.size() + 1 < max) {
        const std::size_t bar = s.find('|', from);
        if (bar == std::string::npos) break;
        out.push_back(s.substr(from, bar - from));
        from = bar + 1;
    }
    out.push_back(s.substr(from));
    return out;
}

double seconds_field(const std::string &s)
{
    double v = 0;
    return parse_double(s, v) && v >= 0 ? v : 0;
}

/* the -w report: the tag, the kind, then curl's variables; Server-Timing last (it holds blanks and commas) */
std::string write_out(char kind)
{
    return std::string("%{stderr}") + kSpeedTag + "|" + kind +
           "|%{http_code}|%{exitcode}|%{num_connects}|%{time_connect}|%{time_appconnect}|%{time_pretransfer}"
           "|%{time_starttransfer}|%{time_total}|%{size_download}|%{size_upload}|%header{colo}|%header{cf-meta-colo}"
           "|%header{cf-meta-upload-bytes}|%header{server-timing}\n";
}

std::vector<std::string> common_argv(char kind)
{
    return {"curl", "-sS", "-g", "--http1.1", "--proto", "=https", "--tls13-ciphers", kSpeedCiphers,
            "--connect-timeout", "8", "-w", write_out(kind)};
}

} // namespace

SpeedConfig clamp_speed_config(const SpeedConfig &in)
{
    SpeedConfig c = in;
    c.latency_requests = clampv(c.latency_requests, 3, 30);
    c.target_ms = clampv(c.target_ms, 2000, 20000);
    c.max_bytes = clampv<std::uint64_t>(c.max_bytes, 1000000, 500000000);
    c.chunk_bytes = clampv<std::uint64_t>(c.chunk_bytes, 1000000, kSpeedMaxRequest);
    c.sample_ms = clampv(c.sample_ms, 200, 2000);
    c.ramp_ms = clampv(c.ramp_ms, 0, 4000);
    c.ping_interval_ms = clampv(c.ping_interval_ms, 200, 1000);
    c.idle_pings = clampv(c.idle_pings, 3, 30);
    c.ping_target = trim(c.ping_target);
    if (c.ping_target == "off") c.ping_target.clear(); /* the settings' word for none */
    return c;
}

/* ---------------------------------------------------------------- curl's report */

double parse_server_timing(const std::string &header)
{
    double sum = 0;
    bool any = false;
    std::size_t at = 0;
    while ((at = header.find("dur=", at)) != std::string::npos) {
        at += 4;
        std::size_t end = at;
        while (end < header.size() && ((header[end] >= '0' && header[end] <= '9') || header[end] == '.')) ++end;
        double v = 0;
        if (end > at && end - at < 16 && parse_double(header.substr(at, end - at), v) && v >= 0 && v < 1e6) {
            sum += v;
            any = true;
        }
        at = end;
    }
    return any ? sum : -1;
}

double SpeedTransfer::rtt_ms() const
{
    if (starttransfer_s <= 0 || starttransfer_s < pretransfer_s) return -1;
    double ms = (starttransfer_s - pretransfer_s) * 1000.0;
    if (server_ms > 0) ms -= server_ms;
    return std::max(0.0, ms);
}

bool parse_speed_transfer(const std::string &raw, SpeedTransfer &out)
{
    out = SpeedTransfer();
    const std::string line = trim(raw);
    if (!starts_with(line, std::string(kSpeedTag) + "|")) return false;
    const std::vector<std::string> f = split_bars(line, 16);
    if (f.size() < 16 || f[1].size() != 1) return false;
    const char kind = f[1][0];
    if (kind != 'L' && kind != 'D' && kind != 'U') return false;
    out.kind = kind;
    int v = 0;
    if (parse_int(f[2], v) && v >= 0 && v < 1000) out.http_code = v;
    if (parse_int(f[3], v)) out.exit_code = v;
    if (parse_int(f[4], v) && v >= 0) out.connects = v;
    out.connect_s = seconds_field(f[5]);
    out.tls_s = seconds_field(f[6]);
    out.pretransfer_s = seconds_field(f[7]);
    out.starttransfer_s = seconds_field(f[8]);
    out.total_s = seconds_field(f[9]);
    parse_u64(f[10], out.size_download);
    parse_u64(f[11], out.size_upload);
    std::string colo = trim(f[12]).empty() ? trim(f[13]) : trim(f[12]);
    if (colo.size() > 8) colo.resize(8);
    for (const char c : colo)
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'))) colo.clear();
    out.colo = colo;
    std::uint64_t got = 0;
    if (parse_u64(f[14], got)) {
        out.has_server_bytes = true;
        out.server_bytes = got;
    }
    out.server_ms = parse_server_timing(f[15]);
    return true;
}

std::vector<std::string> speed_latency_argv(int requests)
{
    std::vector<std::string> a = common_argv('L');
    a.insert(a.end(), {"-m", "20"});
    const int n = clampv(requests, 1, 30) + 1; /* + the warm-up */
    for (int i = 0; i < n; ++i) {
        a.push_back("--url");
        a.push_back(std::string(kSpeedDownUrl) + "0");
    }
    return a;
}

std::vector<std::string> speed_download_argv(std::uint64_t max_bytes, std::uint64_t chunk_bytes)
{
    std::vector<std::string> a = common_argv('D');
    const std::uint64_t chunk = clampv<std::uint64_t>(chunk_bytes, 1, kSpeedMaxRequest);
    std::uint64_t left = max_bytes;
    while (left > 0) {
        const std::uint64_t n = std::min(left, chunk);
        a.push_back("--url");
        a.push_back(std::string(kSpeedDownUrl) + std::to_string(n));
        left -= n;
    }
    return a;
}

std::vector<std::string> speed_upload_argv()
{
    std::vector<std::string> a = common_argv('U');
    a.insert(a.end(), {"-X", "POST", "-T", "-", "-H", "Expect:", "-H", "Content-Type: application/octet-stream", "--url",
                       kSpeedUpUrl});
    return a;
}

std::uint64_t speed_download_total(const std::vector<std::string> &argv)
{
    std::uint64_t sum = 0;
    const std::string prefix = kSpeedDownUrl;
    for (const std::string &a : argv) {
        std::uint64_t n = 0;
        if (starts_with(a, prefix) && parse_u64(a.substr(prefix.size()), n)) sum += n;
    }
    return sum;
}

std::string speed_error_words(const CommandResult &r, int http_code, const std::string &curl_message)
{
    if (r.cancelled) return "Stopped.";
    if (!r.started || r.exit_code == 127) return "curl is not installed (package curl).";
    if (http_code == 429) return "Cloudflare refused the test: too many tests in a short time (HTTP 429). Wait a minute and try again.";
    if (http_code == 403) return "Cloudflare refused the request (HTTP 403).";
    if (http_code >= 500) return "Cloudflare's speed server has a problem (HTTP " + std::to_string(http_code) + "). Try again later.";
    if (http_code != 0 && http_code != 200) return "Cloudflare answered HTTP " + std::to_string(http_code) + " instead of the test data.";
    if (r.timed_out) return "The test took too long and was stopped (the connection is very slow or stalled).";
    std::string words;
    switch (r.exit_code) {
    case 6: words = std::string(kSpeedHost) + " cannot be found: no DNS answer. Is the network up?"; break;
    case 7: words = std::string("No connection to ") + kSpeedHost + ": no internet, or a firewall blocks it."; break;
    case 28: words = "No answer in time: the connection to Cloudflare timed out."; break;
    case 35:
    case 58:
    case 59:
    case 80:
    case 90:
    case 91: words = "The secure (TLS) connection to Cloudflare failed."; break;
    case 51:
    case 60:
    case 77:
    case 83: words = "Cloudflare's certificate was not accepted. Is the board's clock right?"; break;
    case 52: words = "Cloudflare closed the connection without an answer."; break;
    case 18:
    case 55:
    case 56: words = "The connection broke during the test."; break;
    case 0: words = "Cloudflare gave no test data."; break;
    default: words = "curl failed (exit " + std::to_string(r.exit_code) + ").";
    }
    if (r.term_signal) words = "curl ended unexpectedly (signal " + std::to_string(r.term_signal) + ").";
    (void)curl_message; /* kept in the log by the caller, the words stay plain */
    return words;
}

/* ---------------------------------------------------------------- numbers */

ThroughputMeter::ThroughputMeter(int sample_ms, int ramp_ms, std::size_t max_history)
    : sample_us_(static_cast<std::int64_t>(std::max(1, sample_ms)) * 1000),
      ramp_us_(static_cast<std::int64_t>(std::max(0, ramp_ms)) * 1000), max_history_(std::max<std::size_t>(1, max_history))
{
}

double ThroughputMeter::mbps(std::uint64_t bytes, double seconds)
{
    return seconds > 0 ? static_cast<double>(bytes) * 8.0 / seconds / 1e6 : 0;
}

bool ThroughputMeter::add(std::int64_t at_us, std::uint64_t bytes)
{
    if (!started_) {
        if (bytes == 0) return false;
        started_ = true;
        t0_ = last_t_ = sample_t_ = at_us;
        b0_ = last_b_ = sample_b_ = bytes;
        return false;
    }
    if (at_us < last_t_ || bytes < last_b_) return false; /* never backwards */
    last_t_ = at_us;
    last_b_ = bytes;
    if (!ramp_set_ && ramp_us_ > 0 && at_us - t0_ >= ramp_us_) {
        ramp_set_ = true;
        ramp_t_ = at_us;
        ramp_b_ = bytes;
    }
    if (at_us - sample_t_ < sample_us_) return false;
    current_ = mbps(bytes - sample_b_, static_cast<double>(at_us - sample_t_) / 1e6);
    if (history_.size() >= max_history_) history_.erase(history_.begin());
    history_.push_back({static_cast<double>(at_us - t0_) / 1e6, current_});
    sample_t_ = at_us;
    sample_b_ = bytes;
    return true;
}

double ThroughputMeter::result_mbps() const
{
    if (!started_ || last_t_ <= t0_) return 0;
    if (ramp_set_ && last_t_ - ramp_t_ >= ramp_us_ && last_t_ > ramp_t_)
        return mbps(last_b_ - ramp_b_, static_cast<double>(last_t_ - ramp_t_) / 1e6);
    return mbps(last_b_ - b0_, static_cast<double>(last_t_ - t0_) / 1e6);
}

BloatGrade bufferbloat_grade(double increase_ms)
{
    if (!std::isfinite(increase_ms)) return BloatGrade::Unknown;
    if (increase_ms < 30) return BloatGrade::A;
    if (increase_ms < 60) return BloatGrade::B;
    if (increase_ms < 200) return BloatGrade::C;
    if (increase_ms < 400) return BloatGrade::D;
    return BloatGrade::F;
}

const char *grade_letter(BloatGrade g)
{
    switch (g) {
    case BloatGrade::A: return "A";
    case BloatGrade::B: return "B";
    case BloatGrade::C: return "C";
    case BloatGrade::D: return "D";
    case BloatGrade::F: return "F";
    case BloatGrade::Unknown: break;
    }
    return "-";
}

const char *grade_words(BloatGrade g)
{
    switch (g) {
    case BloatGrade::A: return "Excellent: the delay hardly grows while the line is busy.";
    case BloatGrade::B: return "Good: the delay grows a little while the line is busy.";
    case BloatGrade::C: return "Fair: calls and games may stutter while the line is busy.";
    case BloatGrade::D: return "Poor: everything gets slow to answer while the line is busy.";
    case BloatGrade::F: return "Bad: the line is very slow to answer while busy (bufferbloat).";
    case BloatGrade::Unknown: break;
    }
    return "Not measured.";
}

BloatGrade worse_grade(BloatGrade a, BloatGrade b)
{
    if (a == BloatGrade::Unknown) return b;
    if (b == BloatGrade::Unknown) return a;
    return static_cast<int>(a) > static_cast<int>(b) ? a : b;
}

double median_of(std::vector<double> v)
{
    if (v.empty()) return 0;
    std::sort(v.begin(), v.end());
    const std::size_t n = v.size();
    return n % 2 ? v[n / 2] : (v[n / 2 - 1] + v[n / 2]) / 2.0;
}

double jitter_of(const std::vector<double> &v)
{
    if (v.size() < 2) return 0;
    double sum = 0;
    for (std::size_t i = 1; i < v.size(); ++i) sum += std::fabs(v[i] - v[i - 1]);
    return sum / static_cast<double>(v.size() - 1);
}

LatencyStats latency_from_values(const std::vector<double> &rtts, int lost)
{
    LatencyStats s;
    s.received = static_cast<int>(rtts.size());
    s.lost = std::max(0, lost);
    s.sent = s.received + s.lost;
    if (rtts.empty()) return s;
    s.ok = true;
    s.median_ms = median_of(rtts);
    s.min_ms = *std::min_element(rtts.begin(), rtts.end());
    s.max_ms = *std::max_element(rtts.begin(), rtts.end());
    s.jitter_ms = jitter_of(rtts);
    return s;
}

LatencyStats latency_from_ping(const PingSnapshot &snap)
{
    std::vector<double> rtts;
    int lost = 0;
    for (const PacketResult &p : snap.recent) {
        if (p.state == PacketState::Ok || p.state == PacketState::Slow) rtts.push_back(p.rtt_ms);
        else if (p.state == PacketState::Lost) ++lost;
    }
    LatencyStats s = latency_from_values(rtts, lost);
    s.sent = snap.stats.transmitted;
    return s;
}

const char *speed_phase_name(SpeedSnapshot::Phase p)
{
    switch (p) {
    case SpeedSnapshot::Phase::Latency: return "latency";
    case SpeedSnapshot::Phase::Download: return "download";
    case SpeedSnapshot::Phase::Upload: return "upload";
    case SpeedSnapshot::Phase::None: break;
    }
    return "";
}

/* ---------------------------------------------------------------- the ping beside the load */

namespace {

class SessionLoadPing : public LoadPing {
public:
    explicit SessionLoadPing(EchoFactory factory) : session_(std::move(factory)) {}
    bool start(const PingConfig &cfg) override { return session_.start(cfg); }
    void stop() override { session_.stop(); }
    bool wait(int timeout_ms) override { return session_.wait(timeout_ms); }
    std::shared_ptr<const PingSnapshot> latest() const override { return session_.latest(); }

private:
    PingSession session_;
};

} // namespace

LoadPingFactory session_load_ping(EchoFactory factory)
{
    if (!factory)
        factory = [](const Address &target, int payload, int *err) { return open_icmp_echo(target, payload, err); };
    return [factory]() -> std::unique_ptr<LoadPing> { return std::make_unique<SessionLoadPing>(factory); };
}

/* ---------------------------------------------------------------- the engine */

namespace {

/* the lines of one curl: its -w reports, and its last other line (the error message) */
struct Report {
    std::vector<SpeedTransfer> transfers;
    std::string message;
    void line(const std::string &l)
    {
        SpeedTransfer t;
        if (parse_speed_transfer(l, t)) transfers.push_back(t);
        else if (!trim(l).empty()) message = trim(l).substr(0, 300);
    }
    /* the first answer that is not 200, else 200 when one came, else 0 */
    int http_code() const
    {
        int code = 0;
        for (const SpeedTransfer &t : transfers) {
            if (t.http_code != 0 && t.http_code != 200) return t.http_code;
            if (t.http_code == 200) code = 200;
        }
        return code;
    }
    std::string colo() const
    {
        for (const SpeedTransfer &t : transfers)
            if (!t.colo.empty()) return t.colo;
        return std::string();
    }
};

class LatencyClient : public StreamClient {
public:
    Report report;
    void on_line(const std::string &line, std::int64_t, StreamOrders &) override { report.line(line); }
};

/* one direction: the meter, the target time, the ping beside it */
class DirectionClient : public StreamClient {
    bool upload_;
    std::int64_t target_us_;
    std::uint64_t max_bytes_;
    std::function<void()> first_byte_, sample_;

public:
    DirectionClient(bool upload, const SpeedConfig &cfg, std::function<void()> first_byte, std::function<void()> sample)
        : upload_(upload), target_us_(static_cast<std::int64_t>(cfg.target_ms) * 1000), max_bytes_(cfg.max_bytes),
          first_byte_(std::move(first_byte)), sample_(std::move(sample)), meter(cfg.sample_ms, cfg.ramp_ms)
    {
    }
    void on_line(const std::string &line, std::int64_t, StreamOrders &) override { report.line(line); }
    void on_tick(const StreamTick &tick, StreamOrders &orders) override
    {
        const std::uint64_t bytes = upload_ ? tick.in_bytes : tick.out_bytes;
        last_at = tick.at_us;
        if (!ended) {
            const bool was = meter.started();
            const bool sampled = meter.add(tick.at_us, bytes);
            if (!was && meter.started() && first_byte_) first_byte_();
            if (sampled && sample_) sample_();
            if (meter.started() && bytes >= max_bytes_) capped = true;
            /* an upload whose bytes were all handed to curl runs on until the answer: the time it takes curl to send
             * what waits in the buffers belongs to the measure */
            if (meter.started() && tick.at_us - meter.start_us() >= target_us_ && !(upload_ && capped)) {
                ended = true;
                ended_at = tick.at_us;
                if (upload_) orders.end_input = true; /* curl ends the body and reads the answer */
                else orders.stop = true;
            }
        } else if (upload_ && tick.at_us - ended_at > 6000000) {
            orders.stop = true; /* the server's answer does not come: what was sent is measured anyway */
            no_answer = true;
        }
    }

    ThroughputMeter meter;
    Report report;
    bool ended = false, capped = false, no_answer = false;
    std::int64_t ended_at = 0, last_at = 0;
};

void fill_grade(SpeedDirection &d, const LatencyStats &idle)
{
    if (d.loaded.ok && idle.ok) {
        d.has_increase = true;
        d.increase_ms = std::max(0.0, d.loaded.median_ms - idle.median_ms);
        d.grade = bufferbloat_grade(d.increase_ms);
    } else if (idle.ok && !d.loaded.ok && d.loaded.lost >= 3) {
        d.grade = BloatGrade::F;
        d.grade_note = "Every ping was lost while the line was busy.";
    }
}

} // namespace

SpeedSnapshot run_speedtest(const SpeedConfig &input, StreamRunner &runner, const LoadPingFactory &ping_factory,
                            const CancelToken &cancel, const std::function<void(const SpeedSnapshot &)> &publish)
{
    const SpeedConfig cfg = clamp_speed_config(input);
    SpeedSnapshot snap;
    SpeedResult &res = snap.result;
    res.ping_target = cfg.ping_target;
    snap.state = SpeedSnapshot::State::Running;
    snap.phase = SpeedSnapshot::Phase::Latency;
    const auto emit = [&] {
        if (publish) publish(snap);
    };
    const auto finish = [&](SpeedSnapshot::State state, const std::string &error) {
        snap.state = state;
        if (!error.empty()) snap.error = error;
        snap.phase = SpeedSnapshot::Phase::None;
        snap.now_mbps = 0;
        snap.ping_now_ms = -1;
        res.grade = worse_grade(res.down.grade, res.up.grade);
        emit();
        return snap;
    };
    emit();

    bool ping_works = !cfg.ping_target.empty() && ping_factory;
    if (cfg.ping_target.empty()) res.ping_note = "No ping target: the latency under load is not measured.";
    else if (!ping_factory) res.ping_note = "No ping: the latency under load is not measured.";

    const auto make_ping = [&](int count) -> std::unique_ptr<LoadPing> {
        if (!ping_works) return nullptr;
        std::unique_ptr<LoadPing> lp = ping_factory();
        PingConfig pc;
        pc.target = cfg.ping_target;
        pc.interval_ms = cfg.ping_interval_ms;
        pc.timeout_ms = 2000;
        pc.count = count;
        pc.resolve = cfg.resolve;
        if (!lp || !lp->start(pc)) {
            if (lp && lp->latest() && !lp->latest()->error.empty()) res.ping_note = lp->latest()->error;
            else res.ping_note = "The ping could not start.";
            ping_works = false;
            return nullptr;
        }
        return lp;
    };
    /* stops a ping, keeps what it saw; a ping that failed says why and is not started again */
    const auto end_ping = [&](std::unique_ptr<LoadPing> &lp, LatencyStats &into) {
        if (!lp) return;
        lp->stop();
        lp->wait(1500);
        const std::shared_ptr<const PingSnapshot> s = lp->latest();
        if (s) {
            into = latency_from_ping(*s);
            if (!s->address.empty()) res.ping_address = s->address;
            if (s->state == PingSnapshot::State::Failed) {
                res.ping_note = s->error.empty() ? "The ping failed." : s->error;
                ping_works = false;
            }
        }
        lp.reset();
    };

    /* ---- latency */
    {
        std::unique_ptr<LoadPing> idle = make_ping(cfg.idle_pings);
        StreamSpec spec;
        spec.argv = speed_latency_argv(cfg.latency_requests);
        spec.timeout_ms = 25000;
        spec.grace_ms = 300;
        spec.stdout_is_data = true;
        LatencyClient client;
        const StreamResult r = runner.run(spec, cancel, client);
        if (idle && !r.cancelled) { /* let the idle pings end (count reached), 4 s at most */
            const std::int64_t until = now_ms() + 4000;
            while (!idle->wait(50) && !cancel.cancelled() && now_ms() < until) {
            }
        }
        end_ping(idle, res.idle);
        if (r.cancelled || cancel.cancelled()) return finish(SpeedSnapshot::State::Cancelled, "");
        std::vector<double> rtts;
        bool first = true;
        for (const SpeedTransfer &t : client.report.transfers) {
            if (t.kind != 'L' || t.http_code != 200) continue;
            if (first && t.connects > 0) {
                res.setup_ms = t.pretransfer_s * 1000.0; /* DNS, TCP and TLS */
                first = false;
                continue;
            }
            first = false;
            const double rtt = t.rtt_ms();
            if (rtt >= 0) rtts.push_back(rtt);
        }
        res.colo = client.report.colo();
        if (rtts.empty()) {
            snap.detail = printable(client.report.message);
            return finish(SpeedSnapshot::State::Failed,
                          speed_error_words(r, client.report.http_code(), client.report.message));
        }
        const LatencyStats ls = latency_from_values(rtts);
        res.latency_ok = true;
        res.latency_ms = ls.median_ms;
        res.jitter_ms = ls.jitter_ms;
        res.latency_min_ms = ls.min_ms;
        res.latency_max_ms = ls.max_ms;
        res.latency_samples = ls.received;
        if (ping_works && !res.idle.ok && res.ping_note.empty())
            res.ping_note = "The ping target did not answer: the latency under load is not measured.";
        if (!res.idle.ok) ping_works = false;
        emit();
    }

    /* ---- one direction */
    const auto direction = [&](bool upload) -> bool {
        SpeedDirection &d = upload ? res.up : res.down;
        d.tried = true;
        snap.phase = upload ? SpeedSnapshot::Phase::Upload : SpeedSnapshot::Phase::Download;
        snap.now_mbps = 0;
        snap.phase_s = 0;
        snap.phase_bytes = 0;
        snap.ping_now_ms = -1;
        emit();
        std::unique_ptr<LoadPing> loaded;
        DirectionClient *self = nullptr;
        DirectionClient client(
            upload, cfg, [&] { loaded = make_ping(0); },
            [&] {
                snap.now_mbps = self->meter.current_mbps();
                snap.phase_s = self->meter.elapsed_s();
                snap.phase_bytes = self->meter.bytes();
                d.history = self->meter.history();
                if (loaded) {
                    const std::shared_ptr<const PingSnapshot> ps = loaded->latest();
                    if (ps && ps->stats.has_last) snap.ping_now_ms = ps->stats.last_ms;
                }
                emit();
            });
        self = &client;
        StreamSpec spec;
        spec.argv = upload ? speed_upload_argv() : speed_download_argv(cfg.max_bytes, cfg.chunk_bytes);
        spec.timeout_ms = cfg.target_ms + 20000;
        spec.grace_ms = 300;
        spec.stdout_is_data = true;
        if (upload) {
            spec.input = StreamSpec::Input::Zeros;
            spec.zero_bytes = cfg.max_bytes;
        }
        const StreamResult r = runner.run(spec, cancel, client);
        end_ping(loaded, d.loaded);
        if (r.cancelled || cancel.cancelled()) return false;
        const ThroughputMeter &m = client.meter;
        d.history = m.history();
        d.bytes = m.bytes();
        d.seconds = m.elapsed_s();
        d.capped = client.capped && !client.ended;
        for (const SpeedTransfer &t : client.report.transfers)
            if (t.has_server_bytes) {
                d.has_server_bytes = true;
                d.server_bytes = t.server_bytes;
            }
        if (res.colo.empty()) res.colo = client.report.colo();
        const int http = client.report.http_code();
        const bool clean = (r.stopped && client.ended) || r.ok();
        /* a download stopped at its time has no report for the request in progress: 8 s of data are the answer */
        const bool http_ok = http == 200 || (http == 0 && r.stopped && client.ended);
        if (clean && http_ok && m.started()) {
            d.measured = true;
            d.mbps = m.result_mbps();
            if (client.no_answer) d.note = "Cloudflare did not confirm the upload; the speed is what curl sent.";
        } else if (m.started() && m.elapsed_s() >= 1.0 && (http == 0 || http == 200)) {
            d.measured = true; /* it broke on the way: what came is still a measure */
            d.mbps = m.result_mbps();
            char buf[32];
            std::snprintf(buf, sizeof(buf), "%.1f", m.elapsed_s());
            d.note = std::string("Measured over ") + buf + " s only: " + speed_error_words(r, http, client.report.message);
        } else {
            d.error = speed_error_words(r, http, client.report.message);
        }
        if (!d.note.empty() || !d.error.empty()) snap.detail = printable(client.report.message);
        if (ping_works || d.loaded.sent) fill_grade(d, res.idle);
        snap.now_mbps = 0;
        emit();
        return true;
    };

    if (cfg.download && !direction(false)) return finish(SpeedSnapshot::State::Cancelled, "");
    if (cfg.download && !res.down.measured) return finish(SpeedSnapshot::State::Failed, res.down.error);
    if (cfg.upload && !direction(true)) return finish(SpeedSnapshot::State::Cancelled, "");
    if (cfg.upload && !res.up.measured) return finish(SpeedSnapshot::State::Failed, res.up.error);
    return finish(SpeedSnapshot::State::Done, "");
}

/* ---------------------------------------------------------------- the session */

SpeedTestSession::SpeedTestSession()
    : runner_(std::make_shared<DirectStreamRunner>()), ping_(session_load_ping())
{
}

SpeedTestSession::SpeedTestSession(std::shared_ptr<StreamRunner> runner, LoadPingFactory ping)
    : runner_(std::move(runner)), ping_(std::move(ping))
{
}

bool SpeedTestSession::start(const SpeedConfig &cfg)
{
    worker_.cancel();
    const std::uint64_t gen = worker_.generation() + 1;
    SpeedSnapshot first;
    first.state = SpeedSnapshot::State::Running;
    first.phase = SpeedSnapshot::Phase::Latency;
    first.result.ping_target = trim(cfg.ping_target);
    channel_.begin(gen, first);
    const std::shared_ptr<StreamRunner> runner = runner_;
    const LoadPingFactory ping = ping_;
    const std::uint64_t started = worker_.start([this, gen, cfg, runner, ping](const CancelToken &cancel) {
        run_speedtest(cfg, *runner, ping, cancel, [this, gen](const SpeedSnapshot &s) { channel_.publish(gen, s); });
    });
    if (!started) {
        first.state = SpeedSnapshot::State::Failed;
        first.phase = SpeedSnapshot::Phase::None;
        first.error = "Could not start the speed test (no thread).";
        channel_.begin(gen, first);
        return false;
    }
    return true;
}

void SpeedTestSession::stop()
{
    worker_.cancel();
}

} // namespace nt
