/*
 * SPDX-License-Identifier: MIT
 *
 * Net Toolbox tests (0.3): the scripted runners (see stream_fakes.hpp).
 */
#include "stream_fakes.hpp"

#include <signal.h>
#include <unistd.h>

#include <algorithm>

namespace fake {

nt::StreamResult StreamRunner::run(const nt::StreamSpec &spec, const nt::CancelToken &cancel, nt::StreamClient &client)
{
    const std::size_t idx = specs.size();
    specs.push_back(spec);
    stdin_log.push_back(spec.input == nt::StreamSpec::Input::Text ? spec.text : std::string());
    input_ended.push_back(false);
    stopped.push_back(false);
    cancelled.push_back(false);
    farewell.push_back(false);
    const Script script = idx < scripts.size() ? scripts[idx] : Script();
    nt::StreamResult r;
    r.started = script.started;
    if (!script.started) {
        r.exit_code = 127;
        return r;
    }
    bool input_open = spec.input != nt::StreamSpec::Input::Null;
    std::uint64_t out = 0, in = 0;
    std::int64_t at = 0;
    enum class End { None, Stop, Cancel, EndInput };
    const auto obey = [&](nt::StreamOrders &o) -> End {
        End e = End::None;
        if (!o.send.empty() && input_open && spec.input == nt::StreamSpec::Input::Text) stdin_log[idx] += o.send;
        if (o.end_input && input_open) {
            input_open = false;
            input_ended[idx] = true;
            e = End::EndInput;
        }
        if (o.stop) e = End::Stop;
        o = nt::StreamOrders();
        return e;
    };
    const auto play = [&](const Step &s) -> End {
        if (cancel.cancelled()) return End::Cancel;
        if (script.real_sleep_us) usleep(static_cast<useconds_t>(script.real_sleep_us));
        at = s.at_us;
        out = std::max(out, s.out);
        in = std::max(in, s.in);
        End got = End::None;
        nt::StreamOrders o;
        for (const std::string &line : s.lines) {
            if (r.output.size() + line.size() + 1 <= spec.max_text) r.output += (r.output.empty() ? "" : "\n") + line;
            client.on_line(line, s.at_us, o);
            const End e = obey(o);
            if (e == End::Stop) return e;
            if (e == End::EndInput) got = e;
        }
        nt::StreamTick t;
        t.at_us = s.at_us;
        t.out_bytes = out;
        t.in_bytes = in;
        t.input_open = input_open;
        client.on_tick(t, o);
        const End e = obey(o);
        return e != End::None ? e : got;
    };
    const auto finish_stop = [&](bool by_cancel) {
        if (!spec.farewell.empty() && input_open) {
            stdin_log[idx] += spec.farewell;
            farewell[idx] = true;
            input_open = false;
        }
        if (by_cancel) {
            r.cancelled = true;
            cancelled[idx] = true;
        } else {
            r.stopped = true;
            stopped[idx] = true;
            r.term_signal = SIGTERM;
        }
        r.out_bytes = out;
        r.in_bytes = in;
        return r;
    };
    const auto after_end = [&]() -> nt::StreamResult {
        for (const Step &s : script.after_end) {
            const End e = play(s);
            if (e == End::Stop || e == End::Cancel) return finish_stop(e == End::Cancel);
        }
        r.exit_code = 0;
        r.out_bytes = out;
        r.in_bytes = in;
        return r;
    };
    for (const Step &s : script.steps) {
        const End e = play(s);
        if (e == End::Stop || e == End::Cancel) return finish_stop(e == End::Cancel);
        if (e == End::EndInput) return after_end();
    }
    if (script.hang) {
        const std::int64_t limit = at + script.hang_limit_us;
        while (at < limit) {
            Step s;
            s.at_us = at + script.tick_us;
            s.out = out;
            s.in = in;
            const End e = play(s);
            if (e == End::Stop || e == End::Cancel) return finish_stop(e == End::Cancel);
            if (e == End::EndInput) return after_end();
        }
        /* the runner's own timeout: the farewell, then the group ended */
        if (!spec.farewell.empty() && input_open) {
            stdin_log[idx] += spec.farewell;
            farewell[idx] = true;
        }
        r.timed_out = true;
        r.out_bytes = out;
        r.in_bytes = in;
        return r;
    }
    r.exit_code = script.exit_code;
    r.term_signal = script.term_signal;
    r.timed_out = script.timed_out;
    r.out_bytes = out;
    r.in_bytes = in;
    return r;
}

nt::CommandResult CommandRunner::run(const nt::CommandSpec &spec, const nt::CancelToken &cancel)
{
    const std::size_t idx = specs.size();
    specs.push_back(spec);
    if (cancel.cancelled()) {
        nt::CommandResult c;
        c.cancelled = true;
        return c;
    }
    if (idx < results.size()) return results[idx];
    nt::CommandResult r;
    r.started = true;
    r.exit_code = 0;
    return r;
}

namespace {

class ScriptedPing : public nt::LoadPing {
public:
    ScriptedPing(std::shared_ptr<PingPlan> plan, std::vector<double> rtts) : plan_(std::move(plan)), rtts_(std::move(rtts)) {}
    bool start(const nt::PingConfig &cfg) override
    {
        plan_->started.push_back(cfg);
        nt::PingSnapshot s;
        s.target = cfg.target;
        if (plan_->fail_start) {
            s.state = nt::PingSnapshot::State::Failed;
            s.error = plan_->fail_words;
            snap_ = std::make_shared<const nt::PingSnapshot>(s);
            return false;
        }
        s.state = nt::PingSnapshot::State::Done;
        s.address = cfg.target;
        std::uint32_t n = 0;
        for (const double rtt : rtts_) {
            nt::PacketResult p;
            p.number = ++n;
            s.stats.add_sent();
            if (rtt < 0) {
                p.state = nt::PacketState::Lost;
                s.stats.add_lost();
            } else {
                p.state = s.stats.add_reply(rtt);
                p.rtt_ms = rtt;
            }
            s.recent.push_back(p);
        }
        snap_ = std::make_shared<const nt::PingSnapshot>(s);
        return true;
    }
    void stop() override { ++plan_->stopped; }
    bool wait(int) override { return true; }
    std::shared_ptr<const nt::PingSnapshot> latest() const override { return snap_; }

private:
    std::shared_ptr<PingPlan> plan_;
    std::vector<double> rtts_;
    std::shared_ptr<const nt::PingSnapshot> snap_;
};

} // namespace

nt::LoadPingFactory scripted_ping(std::shared_ptr<PingPlan> plan)
{
    return [plan]() -> std::unique_ptr<nt::LoadPing> {
        const std::size_t i = static_cast<std::size_t>(plan->made++);
        std::vector<double> rtts = i < plan->runs.size() ? plan->runs[i] : std::vector<double>();
        return std::make_unique<ScriptedPing>(plan, rtts);
    };
}

std::vector<std::string> raw_lines(const std::string &text)
{
    std::vector<std::string> out;
    std::size_t from = 0;
    while (from < text.size()) {
        std::size_t nl = text.find('\n', from);
        if (nl == std::string::npos) nl = text.size();
        out.push_back(text.substr(from, nl - from));
        from = nl + 1;
    }
    return out;
}

} // namespace fake
