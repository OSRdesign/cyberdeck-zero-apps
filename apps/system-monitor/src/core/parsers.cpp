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

/* the numbers after "cpu" / "cpuN": at least 4 (older kernels have fewer than 8) */
bool parse_cpu_fields(Fields &f, CpuTimes &t)
{
    std::uint64_t v[8] = {};
    int n = 0;
    std::string_view field;
    while (n < 8 && f.next(field)) {
        if (!to_u64(field, v[n])) return false;
        ++n;
    }
    if (n < 4) return false;
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

bool parse_stat_into(const std::string &text, StatInfo &info)
{
    info.ok = false;
    info.all = CpuTimes();
    info.cores.clear();   /* the capacity stays: no allocation once warm */
    info.core_ids.clear();
    Lines lines(text);
    std::string_view line;
    while (lines.next(line)) {
        if (line.size() < 4 || line.compare(0, 3, "cpu") != 0) continue;
        Fields f(line);
        std::string_view name;
        if (!f.next(name)) continue;
        CpuTimes t;
        if (!parse_cpu_fields(f, t)) continue;
        if (name == "cpu") {
            info.all = t;
            info.ok = true;
        } else {
            std::uint64_t id = 0;
            if (!to_u64(name.substr(3), id)) continue;
            info.cores.push_back(t);
            info.core_ids.push_back(static_cast<int>(id));
        }
    }
    return info.ok;
}

StatInfo parse_stat(const std::string &text)
{
    StatInfo info;
    parse_stat_into(text, info);
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
    Lines lines(text);
    std::string_view line;
    while (lines.next(line)) {
        const std::size_t colon = line.find(':');
        if (colon == std::string_view::npos) continue;
        const std::string_view key = line.substr(0, colon);
        Fields f(line.substr(colon + 1));
        std::string_view first;
        std::uint64_t v = 0;
        if (!f.next(first) || !to_u64(first, v)) continue;
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
    Fields f(trim_view(text));
    std::string_view a, b, c, d;
    if (!f.next(a) || !f.next(b) || !f.next(c) || !f.next(d)) return l;
    if (!to_double(a, l.l1) || !to_double(b, l.l5) || !to_double(c, l.l15)) return l;
    const std::size_t slash = d.find('/');
    if (slash != std::string_view::npos) {
        std::uint64_t r = 0, t = 0;
        if (to_u64(d.substr(0, slash), r) && to_u64(d.substr(slash + 1), t)) {
            l.running = static_cast<int>(r);
            l.tasks = static_cast<int>(t);
        }
    }
    l.ok = true;
    return l;
}

bool parse_uptime(const std::string &text, double &uptime_s)
{
    Fields f(trim_view(text));
    std::string_view first;
    double v = 0;
    if (!f.next(first) || !to_double(first, v) || v < 0) return false;
    uptime_s = v;
    return true;
}

/* ---------------------------------------------------------------- network */

bool parse_net_dev_into(const std::string &text, std::vector<NetDev> &out)
{
    std::size_t n = 0; /* the entries are reused: a name of up to 15 bytes stays in the string's own buffer */
    Lines lines(text);
    std::string_view line;
    while (lines.next(line)) {
        const std::size_t colon = line.find(':');
        if (colon == std::string_view::npos) continue;
        const std::string_view name = trim_view(line.substr(0, colon));
        if (name.empty() || name.find('|') != std::string_view::npos) continue;
        Fields f(line.substr(colon + 1));
        std::string_view v[10];
        int k = 0;
        while (k < 10 && f.next(v[k])) ++k;
        if (k < 10) continue;
        NetDev d;
        if (!to_u64(v[0], d.rx_bytes) || !to_u64(v[1], d.rx_packets) || !to_u64(v[8], d.tx_bytes) ||
            !to_u64(v[9], d.tx_packets))
            continue;
        if (n == out.size()) out.emplace_back();
        NetDev &e = out[n++];
        e.name.assign(name.data(), name.size());
        e.rx_bytes = d.rx_bytes;
        e.rx_packets = d.rx_packets;
        e.tx_bytes = d.tx_bytes;
        e.tx_packets = d.tx_packets;
    }
    out.resize(n);
    return n > 0;
}

std::vector<NetDev> parse_net_dev(const std::string &text)
{
    std::vector<NetDev> out;
    parse_net_dev_into(text, out);
    return out;
}

std::string parse_default_route(const std::string &text)
{
    std::string_view best;
    std::uint64_t best_metric = 0;
    Lines lines(text);
    std::string_view line;
    while (lines.next(line)) {
        Fields f(line);
        std::string_view v[8];
        int k = 0;
        while (k < 8 && f.next(v[k])) ++k;
        if (k < 8 || v[0] == "Iface") continue;
        std::uint32_t dest = 1, flags = 0, mask = 1;
        if (!parse_hex(std::string(v[1]), dest) || !parse_hex(std::string(v[3]), flags) ||
            !parse_hex(std::string(v[7]), mask))
            continue;
        if (dest != 0 || mask != 0 || !(flags & 0x1)) continue; /* RTF_UP */
        std::uint64_t metric = 0;
        to_u64(v[6], metric);
        if (best.empty() || metric < best_metric) {
            best = v[0];
            best_metric = metric;
        }
    }
    return std::string(best);
}

std::vector<WirelessLevel> parse_net_wireless(const std::string &text)
{
    std::vector<WirelessLevel> out;
    Lines lines(text);
    std::string_view line;
    while (lines.next(line)) {
        const std::size_t colon = line.find(':');
        if (colon == std::string_view::npos) continue;
        const std::string_view name = trim_view(line.substr(0, colon));
        if (name.empty() || name.find('|') != std::string_view::npos) continue;
        Fields f(line.substr(colon + 1));
        std::string_view status, quality, level_text;
        if (!f.next(status) || !f.next(quality) || !f.next(level_text)) continue;
        WirelessLevel w;
        double level = 0;
        if (!to_double(quality, w.quality) || !to_double(level_text, level)) continue;
        if (level >= 0) continue; /* not dBm (a relative level, or "0" while not associated) */
        w.iface.assign(name.data(), name.size());
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

bool find_diskstat(const std::string &text, const std::string &name, DiskStat &out)
{
    Lines lines(text);
    std::string_view line;
    while (lines.next(line)) {
        Fields f(line);
        std::string_view v[10];
        int k = 0;
        while (k < 10 && f.next(v[k])) ++k;
        if (k < 10 || v[2] != name) continue;
        std::uint64_t major = 0, minor = 0;
        if (!to_u64(v[0], major) || !to_u64(v[1], minor) || !to_u64(v[3], out.reads) ||
            !to_u64(v[5], out.sectors_read) || !to_u64(v[7], out.writes) || !to_u64(v[9], out.sectors_written))
            return false;
        out.major = static_cast<unsigned>(major);
        out.minor = static_cast<unsigned>(minor);
        out.name = name;
        return true;
    }
    return false;
}

std::vector<DiskStat> parse_diskstats(const std::string &text)
{
    std::vector<DiskStat> out;
    Lines lines(text);
    std::string_view line;
    while (lines.next(line)) {
        Fields f(line);
        std::string_view v[10];
        int k = 0;
        while (k < 10 && f.next(v[k])) ++k;
        if (k < 10) continue;
        DiskStat d;
        std::uint64_t major = 0, minor = 0;
        if (!to_u64(v[0], major) || !to_u64(v[1], minor)) continue;
        d.major = static_cast<unsigned>(major);
        d.minor = static_cast<unsigned>(minor);
        d.name.assign(v[2].data(), v[2].size());
        if (!to_u64(v[3], d.reads) || !to_u64(v[5], d.sectors_read) || !to_u64(v[7], d.writes) ||
            !to_u64(v[9], d.sectors_written))
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
    const std::string_view t(text);
    const std::size_t open = t.find('(');
    const std::size_t close = t.rfind(')');
    if (open == std::string_view::npos || close == std::string_view::npos || close < open) return false;
    std::uint64_t pid = 0;
    if (!to_u64(t.substr(0, open), pid) || pid == 0) return false;
    // after the name: field 3 (state) .. field 24 (rss); utime 14, stime 15
    Fields f(t.substr(close + 1));
    std::string_view v[22];
    int k = 0;
    while (k < 22 && f.next(v[k])) ++k;
    if (k < 22 || v[0].empty()) return false;
    std::uint64_t utime = 0, stime = 0;
    std::int64_t rss = 0;
    if (!to_u64(v[11], utime) || !to_u64(v[12], stime) || !to_i64(v[21], rss)) return false;
    out.pid = static_cast<int>(pid);
    const std::string_view comm = t.substr(open + 1, close - open - 1);
    out.comm.assign(comm.data(), comm.size()); /* up to 15 bytes (the kernel's limit): no allocation */
    out.state = v[0][0];
    out.utime = utime;
    out.stime = stime;
    out.rss_pages = rss < 0 ? 0 : rss;
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

bool is_raspberry_model(const std::string &dt_model)
{
    return dt_model.find("Raspberry") != std::string::npos || dt_model.find("Compute Module") != std::string::npos;
}

namespace {

std::string lower_ascii(std::string s)
{
    for (char &c : s)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return s;
}

bool dmi_placeholder(const std::string &v)
{
    const std::string l = lower_ascii(v);
    static const char *const bad[] = {"to be filled by o.e.m.", "to be filled by oem", "system product name",
                                      "system manufacturer",     "default string",       "not specified",
                                      "not applicable",          "unknown",              "none",
                                      "o.e.m.",                  "oem",                  "type1productconfigid"};
    for (const char *b : bad)
        if (l == b) return true;
    return l.empty();
}

} // namespace

std::string dmi_model(const std::string &vendor, const std::string &product)
{
    std::string v = trim(vendor), p = trim(product);
    if (dmi_placeholder(v)) v.clear();
    if (dmi_placeholder(p)) p.clear();
    // "Panasonic Corporation" -> "Panasonic", "Dell Inc." -> "Dell", "ASUSTeK COMPUTER INC." -> "ASUSTeK"
    static const char *const tails[] = {" co., ltd.", " co.,ltd.", " co., ltd", " corporation", " computer inc.", " computer",
                                        " corp.",     " corp",     " inc.",     " inc",          " ltd.",          " ltd",
                                        " gmbh",      " ag"};
    for (bool again = true; again && !v.empty();) {
        again = false;
        const std::string l = lower_ascii(v);
        for (const char *t : tails) {
            const std::string tail = t;
            if (l.size() > tail.size() && l.compare(l.size() - tail.size(), tail.size(), tail) == 0) {
                v = trim(v.substr(0, v.size() - tail.size()));
                again = true;
                break;
            }
        }
    }
    if (p.empty()) return v;
    if (v.empty() || v.size() > 14) return p;
    const std::string lp = lower_ascii(p), lv = lower_ascii(v);
    if (lp.compare(0, lv.size(), lv) == 0) return p; // "Dell XPS" with vendor "Dell"
    return v + " " + p;
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

namespace {

/* "Intel(R) Core(TM) m5-6Y57 CPU @ 1.10GHz" -> "Intel Core m5-6Y57" */
std::string x86_cpu_name(std::string n)
{
    for (const char *drop : {"(R)", "(TM)", "(tm)", "(r)"})
        for (std::size_t at; (at = n.find(drop)) != std::string::npos;) n.erase(at, std::string(drop).size());
    const std::size_t clock = n.find(" @ ");
    if (clock != std::string::npos) n.erase(clock);
    for (const char *drop : {" CPU", " Processor"})
        for (std::size_t at; (at = n.find(drop)) != std::string::npos;) n.erase(at, std::string(drop).size());
    return trim(n);
}

} // namespace

std::string cpu_description(const std::string &cpuinfo, int cores)
{
    static const std::map<std::string, std::string> parts = {
        {"0xb76", "ARM1176"},    {"0xc07", "Cortex-A7"},  {"0xd03", "Cortex-A53"}, {"0xd04", "Cortex-A35"},
        {"0xd05", "Cortex-A55"}, {"0xd07", "Cortex-A57"}, {"0xd08", "Cortex-A72"}, {"0xd0b", "Cortex-A76"}};
    std::string name, model_name;
    int processors = 0;
    for (const std::string &line : split_lines(cpuinfo)) {
        const std::size_t colon = line.find(':');
        if (colon == std::string::npos) continue;
        const std::string key = trim(line.substr(0, colon));
        if (key == "processor") ++processors;
        if (key == "model name" && model_name.empty()) model_name = x86_cpu_name(trim(line.substr(colon + 1)));
        if (key == "CPU part" && name.empty()) {
            const auto it = parts.find(trim(line.substr(colon + 1)));
            if (it != parts.end()) name = it->second;
        }
    }
    if (name.empty()) name = model_name; /* x86: "Intel Core m5-6Y57" */
    const int n = cores > 0 ? cores : processors;
    if (n <= 0) return name;
    if (name.empty()) return std::to_string(n) + (n == 1 ? " core" : " cores");
    return std::to_string(n) + " x " + name;
}

} // namespace sm
