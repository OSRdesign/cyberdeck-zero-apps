/*
 * SPDX-License-Identifier: MIT
 *
 * Net Toolbox 0.2: the real runners of the tools that run a command (curl, openssl): the core's CommandFlow driven by the
 * shared async_run on the LVGL thread, as wifi_link.cpp does with `iw` (no worker thread of our own, nothing blocks; a
 * command runs in its own process group, Stop ends it with SIGTERM and SIGKILL, leaving the app ends it too).
 *
 * The core's session classes (HttpCheckSession, PublicIpSession) drive the same flows with a blocking runner on a worker
 * thread; the app uses these instead, on async_run's callbacks:
 *   Spec::merge_stderr = true and Spec::max_line = CommandSpec::max_line (a JSON line is never cut in pieces); the lines
 *   joined with '\n' are CommandResult::output; Result maps one to one.
 */
#pragma once

#include "backend.hpp"

#include "async_run.hpp"

#include <functional>
#include <memory>

namespace nt {
namespace ui {

/* runs a flow's commands one after the other */
class FlowDriver {
public:
    FlowDriver() = default;
    ~FlowDriver();
    FlowDriver(const FlowDriver &) = delete;
    FlowDriver &operator=(const FlowDriver &) = delete;

    /* after_step runs after each command's result was fed to the flow; finished(true) when the flow ran out of commands,
     * finished(false) when it was cancelled (by cancel() or a start() that replaced it: then nothing is called) */
    void start(std::shared_ptr<CommandFlow> flow, std::function<void()> after_step, std::function<void(bool)> finished);
    /* ends the command in progress; finished(false) runs now */
    void cancel();
    bool running() const;

private:
    struct State {
        std::shared_ptr<CommandFlow> flow;
        std::function<void()> after_step;
        std::function<void(bool)> finished;
        cdk::async_run::JobId job = 0;
        bool active = false;
    };
    static void step(const std::shared_ptr<State> &st);
    void drop();
    std::shared_ptr<State> st_;
};

/* the HTTP check on curl (and openssl for a refused certificate) */
class AsyncHttpRunner : public HttpRunner {
public:
    explicit AsyncHttpRunner(WallClock clock = nullptr) : clock_(std::move(clock)) {}
    bool start(const HttpCheckRequest &req) override;
    void stop() override { driver_.cancel(); }
    bool running() const override { return driver_.running(); }
    std::uint64_t seq() const override { return channel_.seq(); }
    std::shared_ptr<const HttpCheckSnapshot> latest() const override { return channel_.latest(); }

private:
    WallClock clock_;
    Channel<HttpCheckSnapshot> channel_;
    FlowDriver driver_;
    std::uint64_t gen_ = 0;
};

/* the public address: Cloudflare's trace, IPv4 then IPv6, on curl */
class AsyncPublicIpRunner : public PublicIpRunner {
public:
    bool start(const NoRequest &) override;
    void stop() override { driver_.cancel(); }
    bool running() const override { return driver_.running(); }
    std::uint64_t seq() const override { return channel_.seq(); }
    std::shared_ptr<const PublicIpSnapshot> latest() const override { return channel_.latest(); }

private:
    Channel<PublicIpSnapshot> channel_;
    FlowDriver driver_;
    std::uint64_t gen_ = 0;
};

} // namespace ui
} // namespace nt
