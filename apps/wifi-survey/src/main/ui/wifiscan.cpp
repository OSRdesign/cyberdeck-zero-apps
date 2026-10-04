/*
 * SPDX-License-Identifier: MIT
 */

#include "wifiscan.hpp"

#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <mutex>
#include <thread>
#include <unordered_map>

namespace wifisurvey {

/* ---------------------------------------------------------------- helpers */

std::string AccessPoint::security_short() const
{
    if (security.empty() || security == "--") return "Open";
    const bool wpa3 = security.find("WPA3") != std::string::npos;
    const bool wpa2 = security.find("WPA2") != std::string::npos;
    const bool wpa1 = security.find("WPA1") != std::string::npos;
    const bool eap = security.find("802.1X") != std::string::npos;
    std::string s;
    if (wpa3 && wpa2) s = "WPA2/3";
    else if (wpa3) s = "WPA3";
    else if (wpa2) s = "WPA2";
    else if (wpa1) s = "WPA";
    else if (security.find("WEP") != std::string::npos) s = "WEP";
    else if (security.find("OWE") != std::string::npos) s = "OWE";
    else s = security;
    if (eap) s += "-EAP";
    return s;
}

std::vector<std::string> split_terse(const std::string &line)
{
    std::vector<std::string> fields;
    std::string cur;
    for (size_t i = 0; i < line.size(); ++i) {
        const char c = line[i];
        if (c == '\\' && i + 1 < line.size()) {
            cur += line[++i];
        } else if (c == ':') {
            fields.push_back(cur);
            cur.clear();
        } else {
            cur += c;
        }
    }
    fields.push_back(cur);
    return fields;
}

namespace {

int to_int(const std::string &s)
{
    return static_cast<int>(std::strtol(s.c_str(), nullptr, 10));   // "2437 MHz" -> 2437
}

std::string lower(std::string s)
{
    for (char &c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// The full IEEE registry (prefix<TAB>vendor per line), installed next to the app; loaded on first use.
const std::unordered_map<uint32_t, std::string> &oui_table()
{
    static std::unordered_map<uint32_t, std::string> table;
    static std::once_flag once;
    std::call_once(once, [] {
        const char *env = std::getenv("WIFISURVEY_OUI");
        const char *paths[] = {env, "/usr/share/APPLaunch/share/wifi-survey/oui.tsv",
                               "/usr/share/APPLaunch/share/oui.tsv"};   // the LAN Scan copy, if that is installed
        for (const char *path : paths) {
            if (!path) continue;
            std::ifstream file(path);
            if (!file) continue;
            std::string line;
            table.reserve(45000);
            while (std::getline(file, line)) {
                if (line.size() < 8 || line[6] != '\t') continue;
                table[static_cast<uint32_t>(std::strtoul(line.substr(0, 6).c_str(), nullptr, 16))] = line.substr(7);
            }
            if (!table.empty()) break;
        }
    });
    return table;
}

/* Runs a shell command, collects stdout. Returns the exit code, 127 when the shell could not find the command,
 * -1 when it could not be started at all. */
int run_cmd(const std::string &cmd, std::string &out)
{
    out.clear();
    FILE *pipe = popen(cmd.c_str(), "r");
    if (!pipe) return -1;
    char buf[4096];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), pipe)) > 0) out.append(buf, n);
    const int status = pclose(pipe);
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    return out.empty() ? -1 : 0;   // pclose() cannot report the status when SIGCHLD is ignored
}

std::string nmcli_prefix()
{
    // LC_ALL=C: stable field values whatever the system language. timeout: nmcli must never hang the worker.
    std::string p = "LC_ALL=C ";
    if (access("/usr/bin/timeout", X_OK) == 0) p += "/usr/bin/timeout 25 ";
    return p + "nmcli ";
}

bool safe_ifname(const std::string &s)
{
    if (s.empty() || s.size() > 15) return false;
    for (char c : s)
        if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '.' || c == '-' || c == '_')) return false;
    return true;
}

} // namespace

std::string vendor_for(const std::string &mac)
{
    if (mac.size() < 8) return "";
    const long first = std::strtol(mac.substr(0, 2).c_str(), nullptr, 16);
    if (first & 2) return "Locally administered";
    const std::string hex = mac.substr(0, 2) + mac.substr(3, 2) + mac.substr(6, 2);
    const auto &table = oui_table();
    const auto found = table.find(static_cast<uint32_t>(std::strtoul(hex.c_str(), nullptr, 16)));
    return found != table.end() ? found->second : "";
}

std::vector<AccessPoint> parse_wifi_list(const std::string &text)
{
    std::vector<AccessPoint> aps;
    size_t pos = 0;
    while (pos < text.size()) {
        size_t end = text.find('\n', pos);
        if (end == std::string::npos) end = text.size();
        const std::string line = text.substr(pos, end - pos);
        pos = end + 1;
        if (line.empty()) continue;
        const std::vector<std::string> f = split_terse(line);
        if (f.size() < 9) continue;   // not a result line
        AccessPoint ap;
        ap.connected = f[0] == "*";
        ap.bssid = lower(f[1]);
        ap.ssid = f[2] == "--" ? "" : f[2];
        ap.infra = f[3] != "Ad-Hoc";
        ap.channel = to_int(f[4]);
        ap.freq_mhz = to_int(f[5]);
        ap.rate_mbps = to_int(f[6]);
        ap.signal_pct = std::clamp(to_int(f[7]), 0, 100);
        ap.security = f[8];
        for (size_t i = 9; i < f.size(); ++i) ap.security += ":" + f[i];
        if (ap.bssid.empty() || ap.channel <= 0) continue;
        aps.push_back(std::move(ap));
    }
    return aps;
}

/* ---------------------------------------------------------------- scanner */

struct Scanner::Impl {
    mutable std::mutex mutex;
    Snapshot snap;
    bool running = false;

    void publish(Snapshot s)
    {
        std::lock_guard<std::mutex> lock(mutex);
        s.generation = snap.generation + 1;
        s.scanning = false;
        snap = std::move(s);
        running = false;
    }

    void scan_once()
    {
        Snapshot result;
        const std::string nm = nmcli_prefix();
        std::string out;

        // 1. is there a Wi-Fi adapter, and is NetworkManager there to ask?
        int rc = run_cmd(nm + "-t -f DEVICE,TYPE,STATE device status 2>/dev/null", out);
        if (rc == 127) {
            result.state = State::NoNmcli;
            result.note = "nmcli not found";
            publish(std::move(result));
            return;
        }
        if (rc != 0) {
            result.state = State::NoNM;
            result.note = "NetworkManager is not running";
            publish(std::move(result));
            return;
        }
        std::string dev, dev_state;
        size_t pos = 0;
        while (pos < out.size() && dev.empty()) {
            size_t end = out.find('\n', pos);
            if (end == std::string::npos) end = out.size();
            const std::vector<std::string> f = split_terse(out.substr(pos, end - pos));
            pos = end + 1;
            if (f.size() >= 3 && f[1] == "wifi") {
                dev = f[0];
                dev_state = f[2];
            }
        }
        if (dev.empty() || !safe_ifname(dev)) {
            result.state = State::NoAdapter;
            result.note = "No Wi-Fi adapter found";
            publish(std::move(result));
            return;
        }
        result.iface = dev;
        if (dev_state.find("unavailable") != std::string::npos) {
            std::string radio;
            run_cmd(nm + "-t radio wifi 2>/dev/null", radio);
            if (radio.find("disabled") != std::string::npos) {
                result.state = State::WifiOff;
                result.note = "Wi-Fi is turned off";
                publish(std::move(result));
                return;
            }
        }

        // 2. the scan: ask for a fresh one; when the radio is busy (nmcli refuses a rescan that follows another
        // one too closely) fall back to NetworkManager's last results and say so.
        const std::string list = nm + "-t -f IN-USE,BSSID,SSID,MODE,CHAN,FREQ,RATE,SIGNAL,SECURITY device wifi list ifname " +
                                 dev;
        rc = run_cmd(list + " --rescan yes 2>/dev/null", out);
        if (rc != 0) {
            rc = run_cmd(list + " --rescan no 2>/dev/null", out);
            if (rc == 0) {
                result.cached = true;
                result.note = "scan busy, showing last results";
            }
        }
        if (rc != 0) {
            result.state = State::Error;
            result.note = "Scan failed (nmcli " + std::to_string(rc) + ")";
            publish(std::move(result));
            return;
        }
        result.aps = parse_wifi_list(out);
        for (AccessPoint &ap : result.aps) ap.vendor = vendor_for(ap.bssid);
        result.state = State::Ok;
        publish(std::move(result));
    }
};

Scanner::Scanner() : s_(std::make_shared<Impl>()) {}

Scanner::~Scanner() = default;   // a running worker keeps the shared state alive and ends by itself

void Scanner::request()
{
    {
        std::lock_guard<std::mutex> lock(s_->mutex);
        if (s_->running) return;
        s_->running = true;
        s_->snap.scanning = true;
    }
    std::shared_ptr<Impl> state = s_;
    std::thread([state] { state->scan_once(); }).detach();
}

Snapshot Scanner::snapshot() const
{
    std::lock_guard<std::mutex> lock(s_->mutex);
    return s_->snap;
}

} // namespace wifisurvey
