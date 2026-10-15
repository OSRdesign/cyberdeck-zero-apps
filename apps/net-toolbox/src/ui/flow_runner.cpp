/*
 * SPDX-License-Identifier: MIT
 *
 * Net Toolbox 0.2: the command runners on async_run (see flow_runner.hpp).
 */
#include "flow_runner.hpp"

#include "text.hpp"

#include <utility>

namespace nt {
namespace ui {

/* ---------------------------------------------------------------- the driver */

FlowDriver::~FlowDriver()
{
    drop();
}

/* ends what runs without telling anyone */
void FlowDriver::drop()
{
    if (!st_) return;
    st_->active = false;
    if (st_->job) cdk::async_run::cancel(st_->job);
    st_.reset();
}

bool FlowDriver::running() const
{
    return st_ && st_->active;
}

void FlowDriver::start(std::shared_ptr<CommandFlow> flow, std::function<void()> after_step, std::function<void(bool)> finished)
{
    drop();
    st_ = std::make_shared<State>();
    st_->flow = std::move(flow);
    st_->after_step = std::move(after_step);
    st_->finished = std::move(finished);
    st_->active = true;
    step(st_);
}

void FlowDriver::cancel()
{
    if (!st_ || !st_->active) return;
    const std::shared_ptr<State> st = st_;
    st->active = false;
    if (st->job) cdk::async_run::cancel(st->job); /* its on_done finds the state inactive and says nothing */
    st->job = 0;
    CommandResult stopped;
    stopped.cancelled = true;
    st->flow->feed(stopped);
    if (st->finished) st->finished(false);
}

void FlowDriver::step(const std::shared_ptr<State> &st)
{
    CommandSpec cmd;
    if (!st->flow->next(cmd)) {
        st->active = false;
        if (st->finished) st->finished(true);
        return;
    }
    auto output = std::make_shared<std::string>();
    auto truncated = std::make_shared<bool>(false);
    cdk::async_run::Spec spec;
    spec.argv = cmd.argv;
    spec.timeout_ms = cmd.timeout_ms;
    spec.grace_ms = cmd.grace_ms;
    spec.merge_stderr = true;
    spec.max_line = cmd.max_line;
    const std::size_t cap = cmd.max_output;
    spec.on_line = [output, truncated, cap](const std::string &line) {
        if (output->size() + line.size() + 1 > cap) {
            *truncated = true;
            return;
        }
        if (!output->empty()) *output += '\n';
        *output += line;
    };
    spec.on_done = [st, output, truncated](const cdk::async_run::Result &r) {
        if (!st->active) return; /* cancelled: the page was told already */
        CommandResult result;
        result.started = r.started;
        result.exit_code = r.exit_code;
        result.term_signal = r.term_signal;
        result.timed_out = r.timed_out;
        result.cancelled = r.cancelled;
        result.truncated = *truncated;
        result.output = *output;
        result.elapsed_ms = static_cast<int>(r.elapsed_ms);
        st->job = 0;
        st->flow->feed(result);
        if (st->after_step) st->after_step();
        if (result.cancelled) {
            st->active = false;
            if (st->finished) st->finished(false);
            return;
        }
        step(st);
    };
    st->job = cdk::async_run::run(std::move(spec));
}

/* ---------------------------------------------------------------- the HTTP check */

bool AsyncHttpRunner::start(const HttpCheckRequest &req)
{
    const std::uint64_t gen = ++gen_;
    HttpCheckSnapshot first;
    first.input = trim(req.url);
    auto flow = std::make_shared<HttpCheckFlow>(req, clock_);
    first.result = flow->result();
    if (!flow->valid()) {
        driver_.cancel();
        first.state = HttpCheckSnapshot::State::Failed;
        first.error = flow->result().problem_words;
        channel_.begin(gen, first);
        return false;
    }
    first.state = HttpCheckSnapshot::State::Running;
    first.step = "request";
    channel_.begin(gen, first);
    auto snap = std::make_shared<HttpCheckSnapshot>(first);
    driver_.start(
        flow,
        [this, gen, flow, snap] {
            snap->result = flow->result();
            if (flow->step() == HttpCheckFlow::Step::Cert) {
                snap->step = "certificate";
                channel_.publish(gen, *snap);
            }
        },
        [this, gen, flow, snap](bool finished) {
            snap->result = flow->result();
            snap->step.clear();
            snap->state = finished ? HttpCheckSnapshot::State::Done : HttpCheckSnapshot::State::Cancelled;
            channel_.publish(gen, *snap);
        });
    return true;
}

/* ---------------------------------------------------------------- the public address */

bool AsyncPublicIpRunner::start(const NoRequest &)
{
    const std::uint64_t gen = ++gen_;
    PublicIpSnapshot first;
    first.state = PublicIpSnapshot::State::Running;
    first.family = 4;
    channel_.begin(gen, first);
    auto flow = std::make_shared<PublicIpFlow>();
    auto snap = std::make_shared<PublicIpSnapshot>(first);
    driver_.start(
        flow,
        [this, gen, flow, snap] {
            snap->result = flow->result();
            snap->family = flow->family();
            channel_.publish(gen, *snap);
        },
        [this, gen, flow, snap](bool finished) {
            snap->result = flow->result();
            snap->family = 0;
            snap->state = finished ? PublicIpSnapshot::State::Done : PublicIpSnapshot::State::Cancelled;
            channel_.publish(gen, *snap);
        });
    return true;
}

} // namespace ui
} // namespace nt
