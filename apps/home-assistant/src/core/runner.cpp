/*
 * SPDX-License-Identifier: MIT
 *
 * Home Assistant app core: the connection thread (see runner.hpp).
 */
#include "runner.hpp"

#include "curl_api.hpp"
#include "store.hpp"

#include <algorithm>
#include <cstdio>
#include <ctime>
#include <poll.h>
#include <sys/eventfd.h>
#include <time.h>
#include <unistd.h>

namespace ha {

namespace {

constexpr int kHttpTimeout = 10000;
constexpr int kConnectTimeout = 8000;

void log(const std::string &line)
{
    std::fprintf(stderr, "[home-assistant] %s\n", line.c_str());
}

/* the URL without its query: never log a query string (codes, states) */
std::string loggable(const std::string &url)
{
    return url.substr(0, url.find('?'));
}

const char *tag_name(HttpTag t)
{
    switch (t) {
    case HttpTag::CheckServer: return "server check";
    case HttpTag::Exchange: return "sign-in (code)";
    case HttpTag::Refresh: return "token refresh";
    case HttpTag::Revoke: return "sign out (revoke)";
    case HttpTag::RevokeQuiet: return "revoke";
    }
    return "request";
}

} // namespace

ClientBackend::ClientBackend(Settings settings, Credentials creds, std::vector<std::string> favourites,
                             std::string config_dir, std::map<std::string, std::string> names)
    : engine_(std::move(settings), std::move(creds), std::move(favourites)), dir_(std::move(config_dir))
{
    engine_.set_cached_names(std::move(names));
    {
        std::vector<Area> areas;
        std::vector<RegEntry> entries;
        IconRules icons;
        if (engine_.signed_in() && store::load_layout(dir_, areas, entries, icons)) {
            log("layout of the last session: " + std::to_string(areas.size()) + " areas, " +
                std::to_string(entries.size()) + " entities");
            engine_.set_cached_layout(std::move(areas), std::move(entries), std::move(icons));
        }
    }
    wake_fd_ = ::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    std::string why;
    if (!curl::load(why)) {
        fatal_ = why;
        log(why);
    } else {
        log("libcurl " + curl::api().version + " with WebSocket support");
    }
    {
        std::lock_guard<std::mutex> lock(m_);
        view_ = engine_.view(now_ms(), wall());
    }
    version_ = 1;
    thread_ = std::thread([this] { loop(); });
}

ClientBackend::~ClientBackend()
{
    stop_ = true;
    wake();
    if (thread_.joinable()) thread_.join();
    ws_.close();
    listener_.stop();
    if (wake_fd_ >= 0) ::close(wake_fd_);
}

std::int64_t ClientBackend::now_ms()
{
    /* CLOCK_BOOTTIME runs through a suspend: the token's 30 minutes are real minutes */
    struct timespec ts;
    clock_gettime(CLOCK_BOOTTIME, &ts);
    return static_cast<std::int64_t>(ts.tv_sec) * 1000 + ts.tv_nsec / 1000000;
}

std::int64_t ClientBackend::wall()
{
    return static_cast<std::int64_t>(std::time(nullptr));
}

void ClientBackend::wake()
{
    if (wake_fd_ < 0) return;
    const std::uint64_t one = 1;
    (void)!::write(wake_fd_, &one, sizeof one);
}

View ClientBackend::snapshot()
{
    std::lock_guard<std::mutex> lock(m_);
    return view_;
}

void ClientBackend::send(const Command &c)
{
    {
        std::lock_guard<std::mutex> lock(m_);
        commands_.push_back(c);
    }
    wake();
}

void ClientBackend::publish(bool force)
{
    const std::uint64_t v = engine_.version();
    if (!force && v == published_) return;
    published_ = v;
    View view = engine_.view(now_ms(), wall());
    if (!fatal_.empty()) {
        view.pair_status = "This deck cannot talk to Home Assistant: " + fatal_;
        view.pair_tone = Tone::Error;
        view.qr_url.clear();
    }
    {
        std::lock_guard<std::mutex> lock(m_);
        view_ = std::move(view);
    }
    version_.fetch_add(1);
}

void ClientBackend::run_actions()
{
    for (int guard = 0; guard < 1000 && !stop_; ++guard) {
        std::vector<Action> acts = engine_.take_actions();
        if (acts.empty()) return;
        for (const Action &a : acts) {
            if (stop_) return;
            exec(a);
        }
    }
}

void ClientBackend::exec(const Action &a)
{
    switch (a.kind) {
    case Action::Kind::HttpGet:
    case Action::Kind::HttpPost: {
        if (!fatal_.empty()) {
            engine_.on_http(a.tag, 0, "", fatal_, now_ms());
            return;
        }
        const HttpResult r =
            http_request(a.url, a.kind == Action::Kind::HttpPost ? &a.body : nullptr, kHttpTimeout, &stop_);
        if (r.status) log(std::string(tag_name(a.tag)) + ": HTTP " + std::to_string(r.status) + " from " + loggable(a.url));
        else log(std::string(tag_name(a.tag)) + " failed: " + r.error + " (" + loggable(a.url) + ")");
        engine_.on_http(a.tag, r.status, r.body, r.error, now_ms());
        return;
    }
    case Action::Kind::WsOpen: {
        std::string why = fatal_;
        const bool ok = fatal_.empty() && ws_.open(a.url, kConnectTimeout, &stop_, why);
        log(ok ? "connected to " + a.url : "cannot connect to " + a.url + ": " + why);
        ws_kick_ = ok;
        engine_.on_ws_open(ok, why, now_ms());
        return;
    }
    case Action::Kind::WsSend: {
        if (!ws_.is_open()) return;
        std::string why;
        if (!ws_.send(a.body, why)) {
            log("send failed: " + why);
            ws_.close();
            engine_.on_ws_closed(why, now_ms());
        }
        return;
    }
    case Action::Kind::WsClose: ws_.close(); return;
    case Action::Kind::ListenStart: {
        std::string lan, ha_ip, why;
        bool ok = false;
        Server s;
        parse_server(engine_.settings().server, s);
        if (lan_address_towards(a.host, s.effective_port(), lan, ha_ip, why) && listener_.start(why)) ok = true;
        const std::string state = ok ? random_state() : std::string();
        if (ok) log("pairing: listening on " + lan + ":" + std::to_string(listener_.port()));
        else log("pairing: " + why);
        engine_.on_listen(ok, lan, listener_.port(), ha_ip, state, why, now_ms());
        return;
    }
    case Action::Kind::ListenStop:
        if (listener_.running()) log("pairing: listener closed");
        listener_.stop();
        return;
    case Action::Kind::SaveCredentials:
        log(store::save_credentials(dir_, engine_.credentials()) ? "signed in: credentials saved (mode 0600)"
                                                                  : "cannot save the credentials in " + dir_);
        return;
    case Action::Kind::DeleteCredentials:
        store::delete_credentials(dir_);
        log("credentials removed");
        return;
    case Action::Kind::SaveSettings: store::save_settings(dir_, engine_.settings()); return;
    case Action::Kind::SaveFavourites: store::save_favourites(dir_, engine_.favourites()); return;
    case Action::Kind::SaveNames: store::save_names(dir_, engine_.cached_names()); return;
    case Action::Kind::SaveLayout:
        if (!store::save_layout(dir_, engine_.layout_text())) log("cannot save the layout in " + dir_);
        return;
    }
}

void ClientBackend::loop()
{
    engine_.start(now_ms());
    run_actions();
    publish(true);
    std::vector<std::string> messages;
    while (!stop_) {
        const std::int64_t now = now_ms();
        const int timeout =
            ws_kick_ ? 0 : static_cast<int>(std::clamp<std::int64_t>(engine_.next_deadline(now) - now, 0, 1000));
        struct pollfd fds[3];
        int n = 0;
        fds[n++] = {wake_fd_, POLLIN, 0};
        const int ws_i = ws_.is_open() ? n : -1;
        if (ws_i >= 0) fds[n++] = {ws_.fd(), POLLIN, 0};
        const int li = listener_.running() ? n : -1;
        if (li >= 0) fds[n++] = {listener_.fd(), POLLIN, 0};
        ::poll(fds, static_cast<nfds_t>(n), timeout);
        if (stop_) break;

        if (fds[0].revents & POLLIN) {
            std::uint64_t v;
            (void)!::read(wake_fd_, &v, sizeof v);
        }
        std::deque<Command> cmds;
        {
            std::lock_guard<std::mutex> lock(m_);
            cmds.swap(commands_);
        }
        for (const Command &c : cmds) {
            engine_.on_command(c, now_ms());
            run_actions();
        }
        const bool kicked = ws_kick_;
        ws_kick_ = false;
        if (ws_.is_open() && (kicked || (ws_i >= 0 && (fds[ws_i].revents & (POLLIN | POLLHUP | POLLERR))))) {
            messages.clear();
            std::string why;
            const bool alive = ws_.read(messages, why);
            if (ws_.largest_message() > largest_) largest_ = ws_.largest_message();
            const std::int64_t t = now_ms();
            for (const std::string &m : messages) {
                engine_.on_ws_message(m, t, wall());
                run_actions();
                if (!ws_.is_open()) break;
            }
            if (!alive && ws_.is_open()) {
                log("connection lost: " + why);
                ws_.close();
                engine_.on_ws_closed(why, now_ms());
                run_actions();
            }
        }
        if (li >= 0 && listener_.running() && (fds[li].revents & POLLIN)) {
            std::string path, peer;
            std::map<std::string, std::string> query;
            if (listener_.accept_request(path, query, peer)) {
                log("pairing: request from " + peer + " for " + path);
                const bool had_code = query.count("code") > 0;
                const bool had_error = query.count("error") > 0;
                engine_.on_callback(path, query, now_ms());
                const bool taken = !engine_.listening() || engine_.view(now_ms(), wall()).pair_status == "Signing in...";
                if (path == "/callback" && had_code && taken)
                    listener_.respond(200, "Home Assistant answered the deck. You can close this page and look at "
                                           "the deck.");
                else if (path == "/callback" && had_error)
                    listener_.respond(200, "Home Assistant refused the sign-in. See the deck.");
                else if (path == "/callback")
                    listener_.respond(200, "This link is not the deck's current code. Scan the code on the deck again.");
                else
                    listener_.respond(path == "/" ? 200 : 404, "The deck is waiting for Home Assistant's sign-in.");
                run_actions();
            }
        }
        engine_.on_tick(now_ms(), wall());
        run_actions();
        publish(false);
    }
}

} // namespace ha
