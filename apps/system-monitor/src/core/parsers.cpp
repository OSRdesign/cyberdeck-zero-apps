/*
 * SPDX-License-Identifier: MIT
 *
 * System Monitor: the parsers (see parsers.hpp).
 */
#include "parsers.hpp"

#include "text.hpp"

#include <algorithm>
#include <cstdio>
#include <map>

namespace sm {

/* ---------------------------------------------------------------- /proc/stat */

namespace {

bool parse_cpu_line(const std::vector<std::string> &f, CpuTimes &t)
{
    if (f.size() < 5) return false;
    std::uint64_t v[8] = {};
    for (std::size_t i = 0; i < 8; ++i) {
        if (i + 1 >= f.size()) break; /* older kernels: fewer columns */
        if (!parse_u64(f[i + 1], v[i])) return false;
    }
    t.user = v[0];
    t.nice = v[1];
    t.system = v[2];
    t.idle = v[3];
    t.iowait = v[4];
    t.irq = v[5];
    t.softirq = v[6];
    t.steal = v[7];
    return true;
}

} // namespace

StatInfo parse_stat(const std::string &text)
{
    StatInfo info;
    for (const std::string &line : split_lines(text)) {
        if (!starts_with(line, "cpu")) continue;
        const std::vector<std::string> f = split_ws(line);
        if (f.empty()) continue;
        CpuTimes t;
        if (!parse_cpu_line(f, t)) continue;
        if (f[0] == "cpu") {
            info.all = t;
            info.ok = true;
        } else {
            std::uint64_t id = 0;
            if (!parse_u64(f[0].substr(3), id)) continue;
            info.cores.push_back(t);
            info.core_ids.push_back(static_cast<int>(id));
        }
    }
    return info;
}

double busy_pct(const CpuTimes &before, const CpuTimes &after)
{
    const std::uint64_t t0 = before.total(), t1 = after.total();
    if (t1 <= t0) return -1;
    const double dt = static_cast<double>(t1 - t0);
    const std::uint64_t i0 = before.idle_all(), i1 = after.idle_all();
    const double didle = i1 > i0 ? static_cast<double>(i1 - i0) : 0.0;
    double pct = (dt - didle) / dt * 100.0;
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    return pct;
}

/* ---------------------------------------------------------------- /proc/meminfo */

std::uint64_t MemInfo::used() const
{
    if (has_available) return total > available ? total - available : 0;
    const std::uint64_t other = free + buffers + cache();
    return total > other ? total - other : 0;
}

MemInfo parse_meminfo(const std::string &text)
{
    MemInfo m;
    bool has_total = false;
    for (const std::string &line : split_lines(text)) {
        const std::size_t colon = line.find(':');
        if (colon == std::string::npos) continue;
        const std::string key = line.substr(0, colon);
        const std::vector<std::string> f = split_ws(line.substr(colon + 1));
        if (f.empty()) continue;
        std::uint64_t v = 0;
        if (!parse_u64(f[0], v)) continue;
        if (key == "MemTotal") {
            m.total = v;
            has_total = true;
        } else if (key == "MemFree") m.free = v;
        else if (key == "MemAvailable") {
            m.available = v;
            m.has_available = true;
        } else if (key == "Buffers") m.buffers = v;
        else if (key == "Cached") m.cached = v;
        else if (key == "SReclaimable") m.sreclaimable = v;
        else if (key == "Shmem") m.shmem = v;
        else if (key == "SwapTotal") m.swap_total = v;
        else if (key == "SwapFree") m.swap_free = v;
    }
    m.ok = has_total && m.total > 0;
    return m;
}

/* ---------------------------------------------------------------- loadavg, uptime */

LoadAvg parse_loadavg(const std::string &text)
{
    LoadAvg l;
    const std::vector<std::string> f = split_ws(trim(text));
    if (f.size() < 4) return l;
    if (!parse_double(f[0], l.l1) || !parse_double(f[1], l.l5) || !parse_double(f[2], l.l15)) return l;
    const std::size_t slash = f[3].find('/');
    if (slash != std::string::npos) {
        std::uint64_t r = 0, t = 0;
        if (parse_u64(f[3].substr(0, slash), r) && parse_u64(f[3].substr(slash + 1), t)) {
            l.running = static_cast<int>(r);
            l.tasks = static_cast<int>(t);
        }
    }
    l.ok = true;
    return l;
}

bool parse_uptime(const std::string &text, double &uptime_s)
{
    const std::vector<std::string> f = split_ws(trim(text));
    double v = 0;
    if (f.empty() || !parse_double(f[0], v) || v < 0) return false;
    uptime_s = v;
    return true;
}

/* ---------------------------------------------------------------- network */

std::vector<NetDev> parse_net_dev(const std::string &text)
{
    std::vector<NetDev> out;
    for (const std::string &line : split_lines(text)) {
        const std::size_t colon = line.find(':');
        if (colon == std::string::npos) continue;
        const std::string name = trim(line.substr(0, colon));
        if (name.empty() || name.find('|') != std::string::npos) continue;
        const std::vector<std::string> f = split_ws(line.substr(colon + 1));
        if (f.size() < 10) continue;
        NetDev d;
        d.name = name;
        if (!parse_u64(f[0], d.rx_bytes) || !parse_u64(f[1], d.rx_packets) || !parse_u64(f[8], d.tx_bytes) ||
            !parse_u64(f[9], d.tx_packets))
            continue;
        out.push_back(d);
    }
    return out;
}

std::string parse_default_route(const std::string &text)
{
    std::string best;
    std::uint64_t best_metric = 0;
    for (const std::string &line : split_lines(text)) {
        const std::vector<std::string> f = split_ws(line);
        if (f.size() < 8 || f[0] == "Iface") continue;
        std::uint32_t dest = 1, flags = 0, mask = 1;
        if (!parse_hex(f[1], dest) || !parse_hex(f[3], flags) || !parse_hex(f[7], mask)) continue;
        if (dest != 0 || mask != 0 || !(flags & 0x1)) continue; /* RTF_UP */
        std::uint64_t metric = 0;
        parse_u64(f[6], metric);
        if (best.empty() || metric < best_metric) {
            best = f[0];
            best_metric = metric;
        }
    }
    return best;
}

std::vector<WirelessLevel> parse_net_wireless(const std::string &text)
{
    std::vector<WirelessLevel> out;
    for (const std::string &line : split_lines(text)) {
        const std::size_t colon = line.find(':');
        if (colon == std::string::npos) continue;
        const std::string name = trim(line.substr(0, colon));
        if (name.empty() || name.find('|') != std::string::npos) continue;
        const std::vector<std::string> f = split_ws(line.substr(colon + 1));
        if (f.size() < 3) continue;
        WirelessLevel w;
        w.iface = name;
        double level = 0;
        if (!parse_double(f[1], w.quality) || !parse_double(f[2], level)) continue;
        if (level >= 0) continue; /* not dBm (a relative level, or "0" while not associated) */
        w.level_dbm = level;
        out.push_back(w);
    }
    return out;
}

IwLink parse_iw_link(const std::string &text)
{
    IwLink link;
    for (const std::string &raw : split_lines(text)) {
        const std::string line = trim(raw);
        if (starts_with(line, "Connected to")) link.connected = true;
        else if (starts_with(line, "SSID:")) link.ssid = trim(line.substr(5));
        else if (starts_with(line, "freq:")) {
            double f = 0;
            const std::vector<std::string> w = split_ws(line.substr(5));
            if (!w.empty() && parse_double(w[0], f)) link.freq_mhz = static_cast<int>(f);
        } else if (starts_with(line, "signal:")) {
            const std::vector<std::string> w = split_ws(line.substr(7));
            double s = 0;
            if (!w.empty() && parse_double(w[0], s)) {
                link.signal_dbm = static_cast<int>(s);
                link.has_signal = true;
            }
        } else if (starts_with(line, "tx bitrate:")) {
            const std::vector<std::string> w = split_ws(line.substr(11));
            double r = 0;
            if (!w.empty() && parse_double(w[0], r)) {
                link.tx_mbit = r;
                link.has_bitrate = true;
            }
        }
    }
    if (!link.connected) link = IwLink();
    return link;
}

/* ---------------------------------------------------------------- disks and mounts */

std::vector<DiskStat> parse_diskstats(const std::string &text)
{
    std::vector<DiskStat> out;
    for (const std::string &line : split_lines(text)) {
        const std::vector<std::string> f = split_ws(line);
        if (f.size() < 10) continue;
        DiskStat d;
        std::uint64_t major = 0, minor = 0;
        if (!parse_u64(f[0], major) || !parse_u64(f[1], minor)) continue;
        d.major = static_cast<unsigned>(major);
        d.minor = static_cast<unsigned>(minor);
        d.name = f[2];
        if (!parse_u64(f[3], d.reads) || !parse_u64(f[5], d.sectors_read) || !parse_u64(f[7], d.writes) ||
            !parse_u64(f[9], d.sectors_written))
            continue;
        out.push_back(d);
    }
    return out;
}

std::string disk_of_partition(const std::string &name)
{
    const auto is_digit = [](char c) { return c >= '0' && c <= '9'; };
    for (const char *prefix : {"mmcblk", "nvme", "loop", "nbd", "md"}) {
        if (!starts_with(name, prefix)) continue;
        const std::size_t p = name.rfind('p');
        if (p == std::string::npos || p + 1 >= name.size() || p == 0 || !is_digit(name[p - 1])) return name;
        for (std::size_t i = p + 1; i < name.size(); ++i)
            if (!is_digit(name[i])) return name;
        return name.substr(0, p);
    }
    for (const char *prefix : {"sd", "vd", "hd", "xvd"}) {
        if (!starts_with(name, prefix)) continue;
        std::size_t end = name.size();
        while (end > 0 && is_digit(name[end - 1])) --end;
        return end > std::string(prefix).size() ? name.substr(0, end) : name;
    }
    return name;
}

namespace {

std::string unescape_mount(const std::string &s)
{
    std::string out;
    for (std::size_t i = 0; i < s.size(); ++i) {
        const auto octal = [&s](std::size_t k) { return k < s.size() && s[k] >= '0' && s[k] <= '7'; };
        if (s[i] == '\\' && octal(i + 1) && octal(i + 2) && octal(i + 3)) {
            out += static_cast<char>((s[i + 1] - '0') * 64 + (s[i + 2] - '0') * 8 + (s[i + 3] - '0'));
            i += 3;
        } else {
            out += s[i];
        }
    }
    return out;
}

} // namespace

std::vector<Mount> parse_mounts(const std::string &text)
{
    std::vector<Mount> out;
    for (const std::string &line : split_lines(text)) {
        const std::vector<std::string> f = split_ws(line);
        if (f.size() < 4) continue;
        Mount m;
        m.device = unescape_mount(f[0]);
        m.dir = unescape_mount(f[1]);
        m.type = f[2];
        const std::string opts = "," + f[3] + ",";
        m.read_only = opts.find(",ro,") != std::string::npos;
        out.push_back(m);
    }
    return out;
}

std::vector<Mount> real_mounts(const std::vector<Mount> &mounts)
{
    std::vector<Mount> out;
    for (const Mount &m : mounts) {
        if (!starts_with(m.device, "/dev/")) continue;
        if (starts_with(m.device, "/dev/loop") || starts_with(m.device, "/dev/ram") ||
            starts_with(m.device, "/dev/zram"))
            continue;
        if (m.type == "squashfs" || m.type == "swap") continue;
        bool replaced = false, seen = false;
        for (Mount &o : out) {
            if (o.device != m.device) continue;
            seen = true;
            if (m.dir.size() < o.dir.size()) {
                o = m;
                replaced = true;
            }
        }
        if (!seen && !replaced) out.push_back(m);
    }
    std::stable_sort(out.begin(), out.end(), [](const Mount &a, const Mount &b) {
        if (a.dir == "/" || b.dir == "/") return a.dir == "/" && b.dir != "/";
        return a.dir < b.dir;
    });
    return out;
}

/* ---------------------------------------------------------------- sysfs values */

bool parse_millicelsius(const std::string &text, double &celsius)
{
    std::int64_t v = 0;
    if (!parse_i64(text, v)) return false;
    if (v < -60000 || v > 200000) return false; /* not a temperature */
    celsius = static_cast<double>(v) / 1000.0;
    return true;
}

bool parse_khz_as_mhz(const std::string &text, int &mhz)
{
    std::uint64_t v = 0;
    if (!parse_u64(text, v) || v == 0) return false;
    mhz = static_cast<int>((v + 500) / 1000);
    return true;
}

int parse_cpu_count(const std::string &list)
{
    int count = 0;
    std::string item;
    const std::string t = trim(list) + ",";
    for (const char c : t) {
        if (c != ',') {
            item += c;
            continue;
        }
        if (item.empty()) continue;
        const std::size_t dash = item.find('-');
        std::uint64_t a = 0, b = 0;
        if (dash == std::string::npos) {
            if (parse_u64(item, a)) ++count;
        } else if (parse_u64(item.substr(0, dash), a) && parse_u64(item.substr(dash + 1), b) && b >= a) {
            count += static_cast<int>(b - a + 1);
        }
        item.clear();
    }
    return count;
}

/* ---------------------------------------------------------------- firmware */

bool parse_throttled(const std::string &text, std::uint32_t &bits)
{
    std::string t = trim(text);
    const std::size_t eq = t.find('=');
    if (eq != std::string::npos) {
        if (trim(t.substr(0, eq)) != "throttled") return false;
        t = trim(t.substr(eq + 1));
    }
    const std::vector<std::string> lines = split_lines(t);
    return !lines.empty() && parse_hex(lines[0], bits);
}

bool parse_volts(const std::string &text, double &volts)
{
    const std::string t = trim(text);
    const std::size_t eq = t.rfind('=');
    if (eq == std::string::npos) return false;
    std::string v = trim(t.substr(eq + 1));
    if (!v.empty() && (v.back() == 'V' || v.back() == 'v')) v.pop_back();
    double d = 0;
    if (!parse_double(v, d) || d < 0 || d > 30) return false;
    volts = d;
    return true;
}

std::string parse_firmware_version(const std::string &text)
{
    const std::vector<std::string> lines = split_lines(text);
    if (lines.empty()) return std::string();
    const std::string first = trim(lines[0]);
    std::string date;
    // "Mar 17 2025 10:50:39" or "2025/03/10 17:10:37"
    static const char *const months[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                         "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    const std::vector<std::string> f = split_ws(first);
    if (f.size() >= 3) {
        for (int m = 0; m < 12; ++m) {
            std::uint64_t day = 0, year = 0;
            if (f[0] == months[m] && parse_u64(f[1], day) && parse_u64(f[2], year)) {
                char buf[16];
                std::snprintf(buf, sizeof(buf), "%04llu-%02d-%02llu", static_cast<unsigned long long>(year), m + 1,
                              static_cast<unsigned long long>(day));
                date = buf;
            }
        }
    }
    if (date.empty() && !f.empty() && f[0].size() == 10 && f[0][4] == '/' && f[0][7] == '/') {
        date = f[0];
        date[4] = '-';
        date[7] = '-';
    }
    std::string hash;
    for (const std::string &l : lines) {
        const std::vector<std::string> w = split_ws(l);
        if (w.size() >= 2 && w[0] == "version") {
            hash = w[1].substr(0, 7);
            break;
        }
    }
    if (date.empty()) return first;
    return hash.empty() ? date : date + " (" + hash + ")";
}

bool vcgencmd_access_denied(const std::string &output)
{
    for (const char *needle : {"Can't open device file", "failed to open vchiq", "VCHI initialization failed",
                               "Permission denied", "vcio"})
        if (output.find(needle) != std::string::npos) return true;
    return false;
}

/* ---------------------------------------------------------------- processes */

bool parse_proc_stat(const std::string &text, ProcStat &out)
{
    const std::size_t open = text.find('(');
    const std::size_t close = text.rfind(')');
    if (open == std::string::npos || close == std::string::npos || close < open) return false;
    std::uint64_t pid = 0;
    if (!parse_u64(text.substr(0, open), pid) || pid == 0) return false;
    const std::vector<std::string> f = split_ws(trim(text.substr(close + 1)));
    if (f.size() < 22 || f[0].empty()) return false; /* field 3 (state) .. field 24 (rss) */
    ProcStat p;
    p.pid = static_cast<int>(pid);
    p.comm = text.substr(open + 1, close - open - 1);
    p.state = f[0][0];
    if (!parse_u64(f[11], p.utime) || !parse_u64(f[12], p.stime) || !parse_i64(f[21], p.rss_pages)) return false;
    if (p.rss_pages < 0) p.rss_pages = 0;
    out = p;
    return true;
}

const char *proc_state_name(char state)
{
    switch (state) {
    case 'R': return "running";
    case 'S': return "sleeping";
    case 'D': return "waiting for I/O";
    case 'Z': return "zombie";
    case 'T': return "stopped";
    case 't': return "traced";
    case 'I': return "idle";
    case 'X': return "dead";
    default: return "unknown";
    }
}

/* ---------------------------------------------------------------- system facts */

std::string parse_model(const std::string &text)
{
    std::string t;
    for (const char c : text) {
        if (c == '\0' || c == '\n') break;
        t += c;
    }
    return trim(t);
}

std::string model_without_rev(const std::string &model)
{
    const std::size_t rev = model.find(" Rev ");
    return trim(rev == std::string::npos ? model : model.substr(0, rev));
}

std::string short_model(const std::string &model)
{
    std::string m = model_without_rev(model);
    if (starts_with(m, "Raspberry ")) m = m.substr(10);
    return m;
}

std::string os_pretty_name(const std::string &os_release)
{
    for (const std::string &line : split_lines(os_release)) {
        if (!starts_with(line, "PRETTY_NAME=")) continue;
        std::string v = trim(line.substr(12));
        if (v.size() >= 2 && (v.front() == '"' || v.front() == '\'') && v.back() == v.front())
            v = v.substr(1, v.size() - 2);
        return v;
    }
    return std::string();
}

std::string serial_tail(const std::string &dt_serial, const std::string &cpuinfo)
{
    std::string s = parse_model(dt_serial);
    if (s.empty()) {
        for (const std::string &line : split_lines(cpuinfo)) {
            const std::size_t colon = line.find(':');
            if (colon == std::string::npos || trim(line.substr(0, colon)) != "Serial") continue;
            s = trim(line.substr(colon + 1));
        }
    }
    if (s.size() > 8) s = s.substr(s.size() - 8);
    return s;
}

std::string cpu_description(const std::string &cpuinfo, int cores)
{
    static const std::map<std::string, std::string> parts = {
        {"0xb76", "ARM1176"},    {"0xc07", "Cortex-A7"},  {"0xd03", "Cortex-A53"}, {"0xd04", "Cortex-A35"},
        {"0xd05", "Cortex-A55"}, {"0xd07", "Cortex-A57"}, {"0xd08", "Cortex-A72"}, {"0xd0b", "Cortex-A76"}};
    std::string name;
    int processors = 0;
    for (const std::string &line : split_lines(cpuinfo)) {
        const std::size_t colon = line.find(':');
        if (colon == std::string::npos) continue;
        const std::string key = trim(line.substr(0, colon));
        if (key == "processor") ++processors;
        if (key == "CPU part" && name.empty()) {
            const auto it = parts.find(trim(line.substr(colon + 1)));
            if (it != parts.end()) name = it->second;
        }
    }
    const int n = cores > 0 ? cores : processors;
    if (n <= 0) return name;
    if (name.empty()) return std::to_string(n) + (n == 1 ? " core" : " cores");
    return std::to_string(n) + " x " + name;
}

} // namespace sm
