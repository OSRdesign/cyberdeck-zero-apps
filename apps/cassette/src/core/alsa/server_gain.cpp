/* SPDX-License-Identifier: MIT */
#include "server_gain.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

namespace cassette {

namespace {

constexpr double kNorm = 65536.0; /* PA_VOLUME_NORM */

std::string trim(const std::string &s) {
    size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r')) ++a;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r')) --b;
    return s.substr(a, b - a);
}

bool starts(const std::string &s, const char *p) { return s.rfind(p, 0) == 0; }

/* "Name: x" -> "x" when the line starts with the prefix */
bool field(const std::string &line, const char *prefix, std::string &value) {
    if (!starts(line, prefix)) return false;
    value = trim(line.substr(std::strlen(prefix)));
    return true;
}

uint32_t first_uint(const std::string &s) {
    const char *p = s.c_str();
    while (*p && (*p < '0' || *p > '9')) ++p;
    return static_cast<uint32_t>(std::strtoul(p, nullptr, 10));
}

/* "front-left: 3277 /   5% / -78.00 dB,   front-right: 3277 / ..." -> the loudest channel */
uint32_t loudest(const std::string &s) {
    uint32_t top = 0;
    size_t pos = 0;
    while (pos < s.size()) {
        size_t comma = s.find(',', pos);
        if (comma == std::string::npos) comma = s.size();
        const std::string part = s.substr(pos, comma - pos);
        const size_t colon = part.find(':');
        if (colon != std::string::npos) top = std::max(top, first_uint(part.substr(colon + 1)));
        pos = comma + 1;
    }
    return top;
}

std::string unquoted(std::string v) {
    v = trim(v);
    if (v.size() >= 2 && v.front() == '"' && v.back() == '"') v = v.substr(1, v.size() - 2);
    return v;
}

int digits_or(const std::string &s, int none) {
    if (s.empty() || s[0] < '0' || s[0] > '9') return none;
    return std::atoi(s.c_str());
}

/* "hw:1", "front:1", "hw:1,0" -> 1 */
int card_of_path(const std::string &path) {
    const size_t c = path.find(':');
    if (c == std::string::npos) return -1;
    return digits_or(path.substr(c + 1), -1);
}

void finish(ServerSource &s, int alsa_card, int api_card, const std::string &path, std::vector<ServerSource> &out) {
    if (s.name.empty()) return;
    s.alsa_card = alsa_card >= 0 ? alsa_card : (api_card >= 0 ? api_card : card_of_path(path));
    if (s.name.size() > 8 && s.name.compare(s.name.size() - 8, 8, ".monitor") == 0) s.monitor = true;
    out.push_back(s);
}

bool socket_present() {
    const char *srv = std::getenv("PULSE_SERVER");
    if (srv && *srv) return true;
    std::string dir;
    if (const char *x = std::getenv("XDG_RUNTIME_DIR"); x && *x) dir = x;
    else dir = "/run/user/" + std::to_string(static_cast<long>(getuid()));
    struct stat st;
    return ::stat((dir + "/pulse/native").c_str(), &st) == 0 && S_ISSOCK(st.st_mode);
}

class RealPactl : public PactlRunner {
public:
    bool run(const std::vector<std::string> &args, std::string &out, int timeout_ms) override {
        out.clear();
        if (!socket_present()) return false; /* pactl would autospawn a daemon: ask only when a server is there */
        std::vector<std::string> envs;
        for (char **e = environ; e && *e; ++e)
            if (!starts(*e, "LC_ALL=") && !starts(*e, "LANGUAGE=") && !starts(*e, "LANG=")) envs.push_back(*e);
        envs.push_back("LC_ALL=C");
        std::vector<char *> envp;
        for (std::string &s : envs) envp.push_back(&s[0]);
        envp.push_back(nullptr);
        std::vector<std::string> av = {"pactl"};
        av.insert(av.end(), args.begin(), args.end());
        std::vector<char *> argv;
        for (std::string &s : av) argv.push_back(&s[0]);
        argv.push_back(nullptr);

        int fd[2];
        if (::pipe2(fd, O_CLOEXEC) != 0) return false;
        posix_spawn_file_actions_t fa;
        posix_spawn_file_actions_init(&fa);
        posix_spawn_file_actions_adddup2(&fa, fd[1], 1);
        posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
        posix_spawn_file_actions_addopen(&fa, 2, "/dev/null", O_WRONLY, 0);
        pid_t pid = 0;
        const int sr = ::posix_spawnp(&pid, "pactl", &fa, nullptr, argv.data(), envp.data());
        posix_spawn_file_actions_destroy(&fa);
        ::close(fd[1]);
        if (sr != 0) {
            ::close(fd[0]);
            return false;
        }
        const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
        bool timed_out = false;
        for (;;) {
            const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(end - std::chrono::steady_clock::now()).count();
            if (left <= 0) {
                timed_out = true;
                break;
            }
            pollfd p{fd[0], POLLIN, 0};
            const int pr = ::poll(&p, 1, static_cast<int>(left));
            if (pr < 0 && errno == EINTR) continue;
            if (pr <= 0) {
                timed_out = pr == 0;
                break;
            }
            char buf[4096];
            const ssize_t n = ::read(fd[0], buf, sizeof buf);
            if (n > 0) {
                out.append(buf, static_cast<size_t>(n));
                if (out.size() > (4u << 20)) break; /* not a pactl we know */
            } else if (n == 0) {
                break; /* end of output: it exited (or closed its output) */
            } else if (errno != EINTR) {
                break;
            }
        }
        ::close(fd[0]);
        if (timed_out) ::kill(pid, SIGKILL);
        int status = 0;
        while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
        if (timed_out) return false;
        return WIFEXITED(status) && WEXITSTATUS(status) == 0;
    }
};

std::string info_field(const std::string &info, const char *key) {
    size_t pos = 0;
    while (pos < info.size()) {
        size_t nl = info.find('\n', pos);
        if (nl == std::string::npos) nl = info.size();
        std::string v;
        if (field(trim(info.substr(pos, nl - pos)), key, v)) return v;
        pos = nl + 1;
    }
    return "";
}

} // namespace

std::unique_ptr<PactlRunner> make_real_pactl() { return std::unique_ptr<PactlRunner>(new RealPactl()); }

std::vector<ServerSource> parse_pactl_sources(const std::string &text) {
    std::vector<ServerSource> out;
    ServerSource cur;
    bool in = false, props = false;
    int alsa_card = -1, api_card = -1;
    std::string path;
    size_t pos = 0;
    while (pos < text.size()) {
        size_t nl = text.find('\n', pos);
        if (nl == std::string::npos) nl = text.size();
        const std::string line = trim(text.substr(pos, nl - pos));
        pos = nl + 1;
        if (starts(line, "Source #")) {
            if (in) finish(cur, alsa_card, api_card, path, out);
            cur = ServerSource();
            alsa_card = api_card = -1;
            path.clear();
            in = true;
            props = false;
            continue;
        }
        if (!in) continue;
        std::string v;
        if (props) {
            const size_t eq = line.find(" = ");
            if (eq != std::string::npos) {
                const std::string k = line.substr(0, eq);
                v = unquoted(line.substr(eq + 3));
                if (k == "alsa.card") alsa_card = digits_or(v, -1);
                else if (k == "api.alsa.card") api_card = digits_or(v, -1);
                else if (k == "api.alsa.path") path = v;
                continue;
            }
        }
        if (line == "Properties:") props = true;
        else if (field(line, "Name:", v)) cur.name = v, props = false;
        else if (field(line, "Monitor of Sink:", v)) cur.monitor = v != "n/a" && !v.empty();
        else if (field(line, "Volume:", v)) cur.volume_raw = loudest(v);
        else if (field(line, "Base Volume:", v)) cur.base_raw = first_uint(v);
    }
    if (in) finish(cur, alsa_card, api_card, path, out);
    return out;
}

std::string parse_pactl_default_source(const std::string &info) { return info_field(info, "Default Source:"); }
std::string parse_pactl_server_name(const std::string &info) { return info_field(info, "Server Name:"); }

const ServerSource *pick_server_source(const std::vector<ServerSource> &v, const std::string &default_source, int alsa_card) {
    if (alsa_card < 0) return nullptr;
    const ServerSource *first = nullptr;
    for (const ServerSource &s : v) {
        if (s.monitor || s.alsa_card != alsa_card) continue;
        if (!default_source.empty() && s.name == default_source) return &s;
        if (!first) first = &s;
    }
    return first;
}

uint32_t server_volume_for_gain(double gain_db, uint32_t base_raw) {
    const double base = base_raw ? static_cast<double>(base_raw) : kNorm;
    const double v = base * std::pow(10.0, gain_db / 60.0);
    return static_cast<uint32_t>(std::max(1.0, std::min(4.0e9, std::round(v))));
}

double server_gain_of_volume(uint32_t volume_raw, uint32_t base_raw) {
    const double base = base_raw ? static_cast<double>(base_raw) : kNorm;
    if (volume_raw == 0) return -200.0;
    return 60.0 * std::log10(static_cast<double>(volume_raw) / base);
}

bool ServerGain::resolve(int alsa_card) {
    source_.clear();
    server_name_.clear();
    base_raw_ = 0;
    std::string info, list;
    if (!run_.run({"info"}, info, 1500)) return false;
    if (!run_.run({"list", "sources"}, list, 2500)) return false;
    const std::vector<ServerSource> sources = parse_pactl_sources(list);
    const ServerSource *s = pick_server_source(sources, parse_pactl_default_source(info), alsa_card);
    if (!s) return false;
    source_ = s->name;
    base_raw_ = s->base_raw;
    server_name_ = parse_pactl_server_name(info);
    return true;
}

bool ServerGain::set_gain_db(double gain_db) {
    if (!active()) return false;
    std::string out;
    return run_.run({"set-source-volume", source_, std::to_string(server_volume_for_gain(gain_db, base_raw_))}, out, 2000);
}

} // namespace cassette
