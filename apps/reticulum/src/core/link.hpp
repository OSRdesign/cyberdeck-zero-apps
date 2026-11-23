/*
 * SPDX-License-Identifier: MIT
 *
 * The connection to the bridge (docs/dev/reticulum-bridge.md, section 3): one Unix stream socket, one JSON object per
 * line. Everything slow happens on ONE worker thread (connect, write, wait for the reply, read the event lines that
 * arrive while idle); the UI thread only queues requests and, from its LVGL timer, calls poll(), which runs the
 * callbacks of the finished requests on the calling thread. No LVGL here, no blocking call on the UI thread.
 *
 * Transmit guard: a request that can make the deck send (the verb `announce`, `iface.outgoing` with outgoing true, a
 * message send) is refused here, before it reaches the socket, unless the caller says it comes from an explicit user
 * action (`user_action`). Switching a connection on is not in that list: it only listens until sending is allowed.
 */
#pragma once

#include "json.hpp"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace ret {

struct Response {
    bool transport = true; /* false: no bridge answered (not running, connection lost, no answer in time) */
    bool ok = false;       /* the bridge answered ok */
    json::Value result;    /* ok: the result object */
    json::Value error;     /* not ok: the error object ({code, message, field?, ...}) */
    std::string code;      /* not ok: the error code; transport failures: no_service, lost, no_answer, blocked */
    std::string message;
    std::string field;
};

/* True for a request that can make the deck transmit: `announce`, `lxmf.send`, `iface.outgoing` with outgoing true. */
bool is_transmit_enabling(const std::string &verb, const json::Value &params);

/* A reply line -> Response (pure, tested). `id` is the id the caller expects; false when the line is not the reply
 * to it (an event, another id, not JSON). */
bool parse_reply(const std::string &line, std::int64_t id, Response &out);

class Link {
public:
    using Done = std::function<void(const Response &)>;

    explicit Link(std::string sock_path);
    ~Link();
    Link(const Link &) = delete;
    Link &operator=(const Link &) = delete;

    const std::string &path() const { return path_; }

    /* Queues a request; `done` runs from poll() (the caller's thread), exactly once, also on failure. timeout_s is the
     * bridge's own timeout (0.5-120); the link gives up 3 s after it. Requests run one at a time, in order. */
    void request(const std::string &verb, json::Value params, double timeout_s, Done done, bool user_action = false);
    /* Runs the callbacks of the requests that finished. UI thread. */
    void poll();
    /* Requests queued or running, and callbacks not yet delivered. */
    int pending() const { return pending_.load(); }
    /* After each (re)connect, subscribe to the heard-announce events (read-only). Their `recent` list and the events
     * that follow arrive through take_events(). */
    void set_subscribe(bool on) { subscribe_.store(on); }
    /* The event objects ({"event": ..., "data": ...}) that arrived since the last call. */
    std::vector<json::Value> take_events();
    /* Requests the guard refused (tests, the log). */
    int blocked() const { return blocked_.load(); }
    bool connected() const { return connected_.load(); }

private:
    struct Item {
        std::string verb;
        json::Value params;
        double timeout = 15;
        Done done;
    };
    void run();
    bool open_socket(std::string &why);
    void close_socket();
    bool send_line(const std::string &line, double budget_s);
    /* Reads lines until the reply to `id` (events go to events_), or until the deadline / the end of the stream. */
    bool read_reply(std::int64_t id, double budget_s, Response &out);
    void take_lines(bool *got_eof);
    void handle_line(const std::string &line);
    void finish(Done done, Response r);

    std::string path_;
    int fd_ = -1;
    int wake_[2] = {-1, -1};
    std::string buf_;
    bool eof_ = false; /* the last read_reply() ended because the stream closed */
    std::int64_t next_id_ = 0;
    std::atomic<bool> connected_{false};
    std::atomic<bool> subscribe_{false};
    std::atomic<int> pending_{0};
    std::atomic<int> blocked_{0};

    std::mutex mu_;
    std::deque<Item> queue_;
    std::vector<std::pair<Done, Response>> done_;
    std::vector<json::Value> events_;
    bool stop_ = false;
    std::thread thread_; /* last: started in the constructor's body */
};

} // namespace ret
