/*
 * SPDX-License-Identifier: MIT
 *
 * Copied from Cassette (apps/cassette at origin/main d1cf53f) for Vinyl, report 059 decision D6; Vinyl changes are marked VINYL.
 */
#include "launcher_facts.hpp"

#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <signal.h>
#include <sys/stat.h>
#include <sys/types.h>

namespace vinyl {

const char *const kBtPairedReason = "Not connected: connect it in Settings > Bluetooth";

namespace {

std::string upper(std::string s) {
    for (char &c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

std::vector<std::string> split(const std::string &s, char sep) {
    std::vector<std::string> v;
    size_t from = 0;
    for (;;) {
        const size_t at = s.find(sep, from);
        if (at == std::string::npos) {
            v.push_back(s.substr(from));
            return v;
        }
        v.push_back(s.substr(from, at - from));
        from = at + 1;
    }
}

int word_01(const std::string &v) { return v == "0" ? 0 : v == "1" ? 1 : -1; }

}  // namespace

std::string launcher_mac(const std::string &text) {
    if (text.size() != 17) return "";
    std::string m = text;
    for (size_t i = 0; i < 17; ++i) {
        char &c = m[i];
        if (i % 3 == 2) {
            if (c != ':' && c != '_') return "";
            c = ':';
        } else if (!std::isxdigit(static_cast<unsigned char>(c))) {
            return "";
        }
    }
    return upper(m);
}

const LauncherBtDev *LauncherFacts::find(const std::string &mac) const {
    const std::string m = launcher_mac(mac);
    if (m.empty()) return nullptr;
    for (const LauncherBtDev &d : devices)
        if (d.mac == m) return &d;
    return nullptr;
}

bool LauncherFacts::operator==(const LauncherFacts &o) const {
    if (have_status != o.have_status || have_board != o.have_board || bt_audio != o.bt_audio || bt_hfp != o.bt_hfp ||
        bt_le_audio != o.bt_le_audio || devices.size() != o.devices.size())
        return false;
    for (size_t i = 0; i < devices.size(); ++i)
        if (devices[i].mac != o.devices[i].mac || devices[i].kind != o.devices[i].kind ||
            devices[i].connected != o.devices[i].connected || devices[i].profile != o.devices[i].profile)
            return false;
    return true;
}

std::vector<LauncherBtDev> parse_bt_audio_dev(const std::string &value) {
    std::vector<LauncherBtDev> out;
    for (const std::string &entry : split(value, ';')) {
        if (entry.empty()) continue;
        if (out.size() >= kLauncherBtDevMax) break;
        const std::vector<std::string> f = split(entry, ':');
        /* MAC with underscores "70_BF_92_53_6B_8B" has no colon, so the fields are MAC, kind, connected[, profile[, ...]]; a MAC
         * with colons would split into six more fields: accepted too (a tolerant reader) */
        std::string mac_text = f.empty() ? std::string() : f[0];
        size_t next = 1;
        if (launcher_mac(mac_text).empty() && f.size() >= 8) { /* "70:BF:92:53:6B:8B:kind:1" */
            mac_text = f[0] + ":" + f[1] + ":" + f[2] + ":" + f[3] + ":" + f[4] + ":" + f[5];
            next = 6;
        }
        const std::string mac = launcher_mac(mac_text);
        if (mac.empty() || f.size() < next + 2) continue;
        const std::string &kind = f[next];
        const int conn = word_01(f[next + 1]);
        if (kind.empty() || conn < 0) continue;
        LauncherBtDev d;
        d.mac = mac;
        d.kind = kind;
        d.connected = conn == 1;
        if (f.size() > next + 2 && (f[next + 2] == "a2dp" || f[next + 2] == "hfp")) d.profile = f[next + 2];
        out.push_back(d);
    }
    return out;
}

StateText parse_state_text(const std::string &text) {
    StateText st;
    size_t from = 0;
    while (from <= text.size()) {
        size_t nl = text.find('\n', from);
        if (nl == std::string::npos) nl = text.size();
        std::string line = text.substr(from, nl - from);
        from = nl + 1;
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        const size_t eq = line.find('=');
        if (eq == std::string::npos || eq == 0) continue;
        st.kv[line.substr(0, eq)] = line.substr(eq + 1);
    }
    auto v = st.kv.find("v");
    st.ok = v != st.kv.end() && v->second == "1";
    auto p = st.kv.find("pid");
    if (p != st.kv.end() && !p->second.empty() && p->second.size() < 10 &&
        p->second.find_first_not_of("0123456789") == std::string::npos)
        st.pid = std::strtol(p->second.c_str(), nullptr, 10);
    return st;
}

void apply_status_text(const StateText &st, LauncherFacts &out) {
    out.devices.clear();
    auto it = st.kv.find("bt_audio_dev");
    if (it != st.kv.end()) out.devices = parse_bt_audio_dev(it->second);
}

void apply_board_text(const StateText &st, LauncherFacts &out) {
    out.bt_audio.clear();
    out.bt_hfp = out.bt_le_audio = -1;
    auto a = st.kv.find("bt_audio");
    if (a != st.kv.end() && (a->second == "pipewire" || a->second == "pulseaudio" || a->second == "bluealsa" || a->second == "none"))
        out.bt_audio = a->second;
    auto h = st.kv.find("bt_hfp");
    if (h != st.kv.end()) out.bt_hfp = word_01(h->second);
    auto l = st.kv.find("bt_le_audio");
    if (l != st.kv.end()) out.bt_le_audio = word_01(l->second);
}

std::string launcher_state_path(const std::string &file, const std::string &state_dir, const std::string &runtime_dir) {
    if (!state_dir.empty()) return state_dir + "/" + file;
    if (!runtime_dir.empty()) return runtime_dir + "/applaunch/" + file;
    return "/tmp/applaunch-" + file;
}

namespace {

class PosixFactsIo : public FactsIo {
public:
    bool stat(const std::string &path, std::string &stamp) override {
        struct stat sb;
        if (::stat(path.c_str(), &sb) != 0 || !S_ISREG(sb.st_mode)) return false;
        stamp = std::to_string(static_cast<long long>(sb.st_mtim.tv_sec)) + "." + std::to_string(static_cast<long long>(sb.st_mtim.tv_nsec)) +
                "/" + std::to_string(static_cast<long long>(sb.st_size)) + "/" + std::to_string(static_cast<unsigned long long>(sb.st_ino));
        return true;
    }
    bool read(const std::string &path, std::string &text, size_t max) override {
        FILE *f = std::fopen(path.c_str(), "rb");
        if (!f) return false;
        text.clear();
        char buf[1024];
        size_t n;
        while (text.size() < max && (n = std::fread(buf, 1, sizeof buf, f)) > 0) text.append(buf, n);
        std::fclose(f);
        if (text.size() > max) text.resize(max);
        return true;
    }
    bool pid_alive(long pid) override { return pid > 0 && (::kill(static_cast<pid_t>(pid), 0) == 0 || errno == EPERM); }
};

std::string env_text(const char *name) {
    const char *v = std::getenv(name);
    return v ? std::string(v) : std::string();
}

constexpr size_t kStateMax = 16 * 1024;

}  // namespace

std::unique_ptr<FactsIo> make_posix_facts_io() { return std::unique_ptr<FactsIo>(new PosixFactsIo()); }

LauncherFactsProvider::LauncherFactsProvider(std::unique_ptr<FactsIo> io, std::string status_path, std::string board_path)
    : io_(io ? std::move(io) : make_posix_facts_io()), status_path_(std::move(status_path)), board_path_(std::move(board_path)) {
    const std::string dir = env_text("APPLAUNCH_STATE_DIR"), run = env_text("XDG_RUNTIME_DIR");
    if (status_path_.empty()) status_path_ = launcher_state_path("status.state", dir, run);
    if (board_path_.empty()) board_path_ = launcher_state_path("board.state", dir, run);
    status_.path = status_path_;
    board_.path = board_path_;
}

void LauncherFactsProvider::look(Watched &w, bool &changed) {
    std::string stamp;
    if (!io_->stat(w.path, stamp)) {
        if (w.present) changed = true;
        w = Watched{w.path, "", "", false};
        return;
    }
    if (w.present && stamp == w.stamp) return;
    std::string text;
    if (!io_->read(w.path, text, kStateMax)) {
        if (w.present) changed = true;
        w = Watched{w.path, "", "", false};
        return;
    }
    w.present = true;
    w.stamp = stamp;
    w.text = std::move(text);
    changed = true;
}

bool LauncherFactsProvider::poll(uint64_t now_ms) {
    if (looked_ && now_ms - last_ms_ < kPollMs) return false;
    looked_ = true;
    last_ms_ = now_ms;
    bool file_changed = false;
    look(status_, file_changed);
    look(board_, file_changed);
    LauncherFacts f;
    if (status_.present) {
        const StateText st = parse_state_text(status_.text);
        if (st.ok && st.pid > 0 && io_->pid_alive(st.pid)) {
            f.have_status = true;
            apply_status_text(st, f);
        }
    }
    if (board_.present) {
        const StateText st = parse_state_text(board_.text);
        if (st.ok && st.pid > 0 && io_->pid_alive(st.pid)) {
            f.have_board = true;
            apply_board_text(st, f);
        }
    }
    (void)file_changed;
    const bool changed = f != facts_;
    facts_ = std::move(f);
    return changed;
}

/* ---- merging ---- */

namespace {

bool kind_known(const std::string &k) { return k == "headset" || k == "headphones" || k == "speaker"; }

const char *hint_of(const std::string &profile) { return profile == "a2dp" ? "music" : profile == "hfp" ? "call" : ""; }

}  // namespace

std::string bt_generic_name(const std::string &mac) {
    return "Bluetooth device " + (mac.size() >= 5 ? mac.substr(mac.size() - 5) : mac);
}

void bt_facts_apply_inputs(std::vector<BtInput> &list, const LauncherFacts &facts) {
    if (!facts.have_status) return;
    std::vector<BtInput> out;
    for (BtInput &b : list) {
        const LauncherBtDev *d = facts.find(b.mac);
        if (d && kind_known(d->kind)) {
            if (d->kind != "headset") continue; /* a speaker or headphones: no microphone, never an input */
            b.kind = d->kind;
        }
        if (d && b.hint_profile == "unknown" && hint_of(d->profile)[0]) b.hint_profile = hint_of(d->profile);
        out.push_back(std::move(b));
    }
    list = std::move(out);
}

void bt_facts_apply_outputs(std::vector<BtOutput> &list, const LauncherFacts &facts) {
    if (!facts.have_status) return;
    for (BtOutput &b : list) {
        const LauncherBtDev *d = facts.find(b.mac);
        if (!d) continue;
        if (kind_known(d->kind)) b.kind = d->kind;
        if (b.hint_profile == "unknown" && hint_of(d->profile)[0]) b.hint_profile = hint_of(d->profile);
    }
}

namespace {
std::string name_for(const std::map<std::string, std::string> &names, const std::string &mac) {
    auto it = names.find(mac);
    return it != names.end() && !it->second.empty() ? it->second : bt_generic_name(mac);
}
}  // namespace

std::vector<BtInput> bt_paired_inputs(const std::vector<BtInput> &listed, const LauncherFacts &facts,
                                      const std::map<std::string, std::string> &names) {
    std::vector<BtInput> out;
    if (!facts.have_status) return out;
    for (const LauncherBtDev &d : facts.devices) {
        if (d.connected || d.kind != "headset") continue;
        bool seen = false;
        for (const BtInput &b : listed) seen = seen || b.mac == d.mac;
        if (seen) continue;
        BtInput b;
        b.mac = d.mac;
        b.id = "bt:" + d.mac;
        b.label = name_for(names, d.mac);
        b.kind = d.kind;
        b.connected = false;
        b.usable = false;
        b.paired = true;
        b.reason = kBtPairedReason;
        out.push_back(b);
    }
    return out;
}

std::vector<BtOutput> bt_paired_outputs(const std::vector<BtOutput> &listed, const LauncherFacts &facts,
                                        const std::map<std::string, std::string> &names) {
    std::vector<BtOutput> out;
    if (!facts.have_status) return out;
    for (const LauncherBtDev &d : facts.devices) {
        if (d.connected || !kind_known(d.kind)) continue;
        bool seen = false;
        for (const BtOutput &b : listed) seen = seen || b.mac == d.mac;
        if (seen) continue;
        BtOutput b;
        b.mac = d.mac;
        b.id = "bt:" + d.mac;
        b.label = name_for(names, d.mac);
        b.kind = d.kind;
        b.connected = false;
        b.usable = false;
        b.paired = true;
        b.reason = kBtPairedReason;
        out.push_back(b);
    }
    return out;
}

}  // namespace vinyl
