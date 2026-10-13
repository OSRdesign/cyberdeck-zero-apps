/*
 * SPDX-License-Identifier: MIT
 *
 * System Monitor: the pages' texts (see monitor_core.hpp).
 */
#include "monitor_core.hpp"

#include "text.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace sm {
namespace {

constexpr const char *kWait = "...";

Row info(const std::string &label, std::vector<std::string> values)
{
    Row r;
    r.type = RowType::Info;
    r.label = label;
    r.values = std::move(values);
    return r;
}

Row heading(const std::string &label) { return info(label, {}); }

Row nav(const std::string &label, std::vector<std::string> values, int id, Tone tone = Tone::Normal)
{
    Row r;
    r.type = RowType::Nav;
    r.label = label;
    r.values = std::move(values);
    r.id = id;
    r.tone = tone;
    return r;
}

Row busy(const std::string &label)
{
    Row r;
    r.type = RowType::Busy;
    r.label = label;
    return r;
}

std::string address_only(const std::string &cidr)
{
    const std::size_t slash = cidr.find('/');
    return slash == std::string::npos ? cidr : cidr.substr(0, slash);
}

const FsInfo *root_fs(const Snapshot &s)
{
    for (const FsInfo &f : s.fs)
        if (f.dir == "/") return &f;
    return s.fs.empty() ? nullptr : &s.fs.front();
}

const NetInfo *main_net(const Snapshot &s)
{
    for (const NetInfo &n : s.net)
        if (n.name == s.main_iface) return &n;
    return nullptr;
}

std::vector<std::string> freq_values(const CpuInfo &c)
{
    if (c.cur_mhz > 0 && c.max_mhz > 0)
        return {std::to_string(c.cur_mhz) + " / " + fmt_mhz(c.max_mhz), fmt_mhz(c.cur_mhz)};
    if (c.cur_mhz > 0) return {fmt_mhz(c.cur_mhz)};
    return {"Unknown"};
}

std::string load_text(const LoadAvg &l)
{
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.2f %.2f %.2f", l.l1, l.l5, l.l15);
    return buf;
}

std::string load_short(const LoadAvg &l)
{
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.2f", l.l1);
    return buf;
}

std::vector<std::string> used_of_values(std::uint64_t used_kb, std::uint64_t total_kb)
{
    return {fmt_kb(used_kb) + " of " + fmt_kb(total_kb), fmt_used_of(used_kb * 1024ull, total_kb * 1024ull),
            fmt_kb(used_kb)};
}

std::string pct_of(std::uint64_t part, std::uint64_t whole)
{
    return whole ? fmt_pct(100.0 * static_cast<double>(part) / static_cast<double>(whole)) : std::string("0%");
}

const char *bt_text(BtState b)
{
    switch (b) {
    case BtState::On: return "On";
    case BtState::Off: return "Off";
    case BtState::Connected: return "On, connected";
    default: return "No adapter";
    }
}

} // namespace

/* ---------------------------------------------------------------- navigation */

const char *view_title(View view)
{
    switch (view) {
    case View::Cpu: return "CPU";
    case View::Memory: return "Memory";
    case View::Thermal: return "Temp & power";
    case View::Storage: return "Storage";
    case View::Network: return "Network";
    case View::Processes: return "Processes";
    case View::About: return "System";
    case View::Period: return "Sampling";
    default: return "";
    }
}

Back Nav::back(int *from_row)
{
    if (stack_.size() <= 1) return Back::Quit;
    if (from_row) *from_row = stack_.back().second;
    stack_.pop_back();
    return Back::Popped;
}

bool view_wants_procs(View view) { return view == View::Cpu || view == View::Memory || view == View::Processes; }
bool view_wants_link(View view) { return view == View::Network; }

View overview_target(int id)
{
    switch (id) {
    case kOvCpu: case kOvFreq: case kOvLoad: return View::Cpu;
    case kOvTemp: case kOvPower: return View::Thermal;
    case kOvMem: case kOvSwap: return View::Memory;
    case kOvStorage: return View::Storage;
    case kOvNet: return View::Network;
    case kOvProcs: return View::Processes;
    case kOvUptime: case kOvSystem: return View::About;
    case kOvPeriod: return View::Period;
    default: return View::Overview;
    }
}

/* ---------------------------------------------------------------- overview */

std::vector<Row> overview_rows(const Snapshot *s, int period_s)
{
    std::vector<Row> rows;
    if (!s) {
        for (const auto &p : std::vector<std::pair<const char *, int>>{
                 {"CPU", kOvCpu}, {"Temperature", kOvTemp}, {"Frequency", kOvFreq}, {"Memory", kOvMem},
                 {"Swap", kOvSwap}, {"Load", kOvLoad}, {"Uptime", kOvUptime}, {"Storage", kOvStorage},
                 {"Network", kOvNet}, {"Processes", kOvProcs}, {"System", kOvSystem}})
            rows.push_back(nav(p.first, {kWait}, p.second));
    } else {
        rows.push_back(nav("CPU", {s->cpu.ok ? fmt_pct(s->cpu.total) : kWait}, kOvCpu));
        if (s->temp.ok) {
            const TempState st = temp_state(s->temp.c);
            rows.push_back(nav("Temperature", {fmt_temp(s->temp.c), fmt_temp_short(s->temp.c)}, kOvTemp, temp_tone(st)));
        } else {
            rows.push_back(nav("Temperature", {"Unknown"}, kOvTemp));
        }
        rows.push_back(nav("Frequency", freq_values(s->cpu), kOvFreq));
        if (s->mem.ok) {
            rows.push_back(nav("Memory", {fmt_used_of(s->mem.used() * 1024ull, s->mem.total * 1024ull),
                                          fmt_kb(s->mem.used())},
                               kOvMem));
            if (s->mem.swap_total)
                rows.push_back(nav("Swap", {fmt_used_of(s->mem.swap_used() * 1024ull, s->mem.swap_total * 1024ull),
                                            fmt_kb(s->mem.swap_used())},
                                   kOvSwap));
            else rows.push_back(nav("Swap", {"Off"}, kOvSwap));
        } else {
            rows.push_back(nav("Memory", {"Unknown"}, kOvMem));
        }
        rows.push_back(nav("Load", {s->load.ok ? load_text(s->load) : kWait, s->load.ok ? load_short(s->load) : kWait},
                           kOvLoad));
        rows.push_back(nav("Uptime", {s->uptime_ok ? fmt_uptime(s->uptime_s) : "Unknown"}, kOvUptime));
        if (const FsInfo *f = root_fs(*s)) {
            const std::string pct = fmt_pct(f->pct());
            rows.push_back(nav("Storage", {pct + " of " + fmt_bytes(f->total), pct}, kOvStorage,
                               f->pct() >= 95 ? Tone::Error : f->pct() >= 85 ? Tone::Warn : Tone::Normal));
        } else {
            rows.push_back(nav("Storage", {"Unknown"}, kOvStorage));
        }
        if (s->power.source != PowerSource::None) {
            Tone tone = Tone::Normal;
            std::vector<std::string> values = power_values(s->power, tone);
            rows.push_back(nav("Power", values, kOvPower, tone));
        }
        if (const NetInfo *n = main_net(*s)) {
            if (!n->ipv4.empty())
                rows.push_back(nav("Network", {n->name + " " + address_only(n->ipv4), address_only(n->ipv4), n->name},
                                   kOvNet));
            else rows.push_back(nav("Network", {n->name + " " + n->state, "Not connected", "Off"}, kOvNet, Tone::Warn));
        } else {
            rows.push_back(nav("Network", {"No interface", "None"}, kOvNet, Tone::Warn));
        }
        rows.push_back(nav("Processes", {s->procs_valid || s->proc_count ? std::to_string(s->proc_count) : kWait},
                           kOvProcs));
        const std::string model = model_without_rev(s->sys.model);
        rows.push_back(nav("System", model.empty() ? std::vector<std::string>{"Linux"}
                                                   : std::vector<std::string>{model, short_model(s->sys.model)},
                           kOvSystem));
    }
    rows.push_back(nav("Sampling", {"Every " + std::to_string(period_s) + " s", std::to_string(period_s) + " s"},
                       kOvPeriod));
    return rows;
}

/* ---------------------------------------------------------------- CPU */

std::vector<Row> cpu_rows(const Snapshot *s)
{
    std::vector<Row> rows;
    if (!s) return {busy("Reading")};
    rows.push_back(info("Total", {s->cpu.ok ? fmt_pct(s->cpu.total) : kWait}));
    for (std::size_t i = 0; i < s->cpu.cores.size(); ++i)
        rows.push_back(info("Core " + std::to_string(i), {fmt_pct(s->cpu.cores[i])}));
    rows.push_back(info("Frequency", freq_values(s->cpu)));
    if (!s->cpu.governor.empty()) rows.push_back(info("Governor", {s->cpu.governor}));
    if (s->load.ok) {
        rows.push_back(info("Load", {load_text(s->load), load_short(s->load)}));
        rows.push_back(info("Tasks", {std::to_string(s->load.running) + " running of " + std::to_string(s->load.tasks),
                                      std::to_string(s->load.running) + " / " + std::to_string(s->load.tasks)}));
    }
    rows.push_back(heading("Top processes by CPU"));
    if (!s->procs_valid) {
        rows.push_back(busy("Reading processes"));
    } else {
        for (std::size_t i = 0; i < s->top_cpu.size() && i < static_cast<std::size_t>(kTopDetail); ++i)
            rows.push_back(info(s->top_cpu[i].name, {fmt_proc_cpu(s->top_cpu[i].cpu_pct)}));
    }
    rows.push_back(nav("All processes", {"by CPU"}, kRowAllProcs));
    return rows;
}

/* ---------------------------------------------------------------- memory */

std::vector<Row> memory_rows(const Snapshot *s)
{
    std::vector<Row> rows;
    if (!s) return {busy("Reading")};
    if (s->mem.ok) {
        const MemInfo &m = s->mem;
        std::vector<std::string> used = used_of_values(m.used(), m.total);
        used.insert(used.begin(), fmt_kb(m.used()) + " of " + fmt_kb(m.total) + " (" + pct_of(m.used(), m.total) + ")");
        rows.push_back(info("Used", used));
        rows.push_back(info("Available", {fmt_kb(m.has_available ? m.available : m.free + m.cache() + m.buffers)}));
        rows.push_back(info("Cache", {fmt_kb(m.cache())}));
        rows.push_back(info("Buffers", {fmt_kb(m.buffers)}));
        rows.push_back(info("Free", {fmt_kb(m.free)}));
        rows.push_back(info("Shared", {fmt_kb(m.shmem)}));
        if (m.swap_total) rows.push_back(info("Swap", used_of_values(m.swap_used(), m.swap_total)));
        else rows.push_back(info("Swap", {"Off"}));
    } else {
        rows.push_back(info("Memory", {"Unknown"}));
    }
    rows.push_back(heading("Top processes by memory"));
    if (!s->procs_valid) {
        rows.push_back(busy("Reading processes"));
    } else {
        for (std::size_t i = 0; i < s->top_mem.size() && i < static_cast<std::size_t>(kTopDetail); ++i)
            rows.push_back(info(s->top_mem[i].name, {fmt_bytes(s->top_mem[i].rss_bytes)}));
    }
    rows.push_back(nav("All processes", {"by memory"}, kRowAllProcs));
    return rows;
}

/* ---------------------------------------------------------------- temperature and power */

std::vector<Row> thermal_rows(const Snapshot *s)
{
    std::vector<Row> rows;
    if (!s) return {busy("Reading")};
    const bool hot = s->temp.ok && temp_state(s->temp.c) == TempState::Hot;
    const Status p = power_status(s->power, hot);
    if (!p.text.empty()) rows.push_back(heading(p.text));
    if (s->temp.ok) {
        const TempState st = temp_state(s->temp.c);
        rows.push_back(info("CPU temperature", {fmt_temp(s->temp.c)}));
        switch (st) {
        case TempState::Ok: rows.push_back(info("State", {"OK (warm from 70 \xC2\xB0" "C)", "OK"})); break;
        case TempState::Warm: rows.push_back(info("State", {"Warm (hot from 80 \xC2\xB0" "C)", "Warm"})); break;
        case TempState::Hot: rows.push_back(info("State", {"Hot: the Pi slows down", "Hot"})); break;
        }
        if (s->hist.temp.size() > 1) {
            const std::string window = fmt_window(static_cast<int>(s->hist.temp.size()), s->period_s);
            rows.push_back(info("Highest", {fmt_temp(s->hist.temp.max()) + " in " + window, fmt_temp(s->hist.temp.max())}));
        }
    } else {
        rows.push_back(info("CPU temperature", {"Unknown"}));
    }
    if (s->fan.present) {
        std::vector<std::string> v;
        const std::string level = s->fan.state >= 0 && s->fan.max_state > 0
                                      ? std::to_string(s->fan.state) + " of " + std::to_string(s->fan.max_state)
                                      : std::string();
        const std::string rpm = s->fan.rpm >= 0 ? std::to_string(s->fan.rpm) + " rpm" : std::string();
        if (!level.empty() && !rpm.empty()) v.push_back("Level " + level + ", " + rpm);
        if (!rpm.empty()) v.push_back(rpm);
        if (!level.empty()) v.push_back("Level " + level);
        if (v.empty()) v.push_back("Present");
        rows.push_back(info("Fan", v));
    }
    if (s->power.source == PowerSource::Firmware) {
        for (int f = 0; f < kFlagCount; ++f) rows.push_back(info(flag_name(f), flag_values(s->power.flags[f], false)));
    } else if (s->power.source == PowerSource::Alarm) {
        rows.push_back(info(flag_name(kUnderVoltage), flag_values(s->power.flags[kUnderVoltage], true)));
    }
    if (s->power.supply_ok) rows.push_back(info("Supply (5 V input)", {fmt_volts(s->power.supply_v)}));
    if (s->power.core_ok) rows.push_back(info("Core voltage", {fmt_volts(s->power.core_v)}));
    if (s->power.source == PowerSource::Firmware) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "0x%x", s->power.bits);
        rows.push_back(info("Firmware flags", {std::string(buf) + " (" + s->power.via + ")", buf}));
    } else if (s->power.source == PowerSource::Alarm) {
        rows.push_back(info("Source", {"rpi_volt alarm only", "hwmon"}));
    }
    return rows;
}

/* ---------------------------------------------------------------- storage */

std::vector<Row> storage_rows(const Snapshot *s)
{
    std::vector<Row> rows;
    if (!s) return {busy("Reading")};
    for (const FsInfo &f : s->fs) {
        const std::string pct = fmt_pct(f.pct());
        const std::string used = fmt_used_of(f.used, f.total);
        rows.push_back(info(f.dir, {used + " (" + pct + ")", used, pct}));
    }
    if (s->fs.empty()) rows.push_back(info("Filesystems", {"None found"}));
    if (s->disk.ok) {
        rows.push_back(info("Disk", {s->disk.name}));
        rows.push_back(info("Reading", {fmt_rate(s->disk.read_bps)}));
        rows.push_back(info("Writing", {fmt_rate(s->disk.write_bps)}));
        rows.push_back(info("Read since boot", {fmt_bytes(s->disk.read_total)}));
        rows.push_back(info("Written since boot", {fmt_bytes(s->disk.written_total)}));
    }
    return rows;
}

/* ---------------------------------------------------------------- network */

std::vector<Row> network_rows(const Snapshot *s)
{
    std::vector<Row> rows;
    if (!s) return {busy("Reading")};
    for (const NetInfo &n : s->net) {
        rows.push_back(heading(n.name + (n.wireless ? " (Wi-Fi)" : "")));
        rows.push_back(info("State", {n.state}));
        rows.push_back(info("Address", {n.ipv4.empty() ? "None" : n.ipv4}));
        if (n.wireless) {
            if (n.link_valid) rows.push_back(info("Network", {n.link.connected ? n.link.ssid : "Not connected"}));
            const bool sig = n.link_valid && n.link.has_signal;
            if (sig || n.has_level)
                rows.push_back(info("Signal", {std::to_string(sig ? n.link.signal_dbm : static_cast<int>(n.level_dbm)) + " dBm"}));
            if (n.link_valid && n.link.has_bitrate) {
                char buf[32];
                std::snprintf(buf, sizeof(buf), "%.1f Mbit/s", n.link.tx_mbit);
                rows.push_back(info("Bit rate", {buf}));
            }
        }
        rows.push_back(info("Receiving", {fmt_rate(n.rx_bps)}));
        rows.push_back(info("Sending", {fmt_rate(n.tx_bps)}));
        rows.push_back(info("Received", {fmt_bytes(n.rx_total)}));
        rows.push_back(info("Sent", {fmt_bytes(n.tx_total)}));
    }
    if (s->net.empty()) rows.push_back(info("Interfaces", {"None"}));
    rows.push_back(info("Bluetooth", {bt_text(s->bt)}));
    return rows;
}

/* ---------------------------------------------------------------- processes */

std::string fmt_proc_cpu(double pct)
{
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%.1f%%", pct > 0 ? pct : 0.0);
    return buf;
}

std::vector<Row> process_rows(const Snapshot *s, SortBy by)
{
    std::vector<Row> rows;
    rows.push_back(nav("Sort by", {by == SortBy::Cpu ? "CPU" : "Memory"}, kRowSort));
    if (!s || !s->procs_valid) {
        rows.push_back(busy("Reading processes"));
        return rows;
    }
    const std::vector<ProcRow> &list = by == SortBy::Cpu ? s->top_cpu : s->top_mem;
    for (std::size_t i = 0; i < list.size() && i < static_cast<std::size_t>(kTopProcs); ++i) {
        const ProcRow &p = list[i];
        const std::string cpu = fmt_proc_cpu(p.cpu_pct), mem = fmt_bytes(p.rss_bytes);
        Row r = info(p.name, {cpu + "  " + mem, by == SortBy::Cpu ? cpu : mem});
        r.id = static_cast<int>(i);
        rows.push_back(r);
    }
    return rows;
}

std::string process_caption(const ProcRow &p)
{
    return "pid " + std::to_string(p.pid) + ", " + proc_state_name(p.state) + ", " + fmt_bytes(p.rss_bytes) + ", " +
           fmt_proc_cpu(p.cpu_pct) + " CPU";
}

/* ---------------------------------------------------------------- about, period */

std::vector<Row> about_rows(const Snapshot *s,
                            const std::vector<std::pair<std::string, std::vector<std::string>>> &extra)
{
    std::vector<Row> rows;
    if (s) {
        const SystemFacts &f = s->sys;
        const auto add = [&rows](const char *key, const std::string &v, const std::string &short_v = std::string()) {
            if (v.empty()) return;
            std::vector<std::string> values{v};
            if (!short_v.empty() && short_v != v) values.push_back(short_v);
            rows.push_back(info(key, values));
        };
        add("Model", model_without_rev(f.model), short_model(f.model));
        add("Kernel", f.kernel);
        add("OS", f.os);
        add("Hostname", f.hostname);
        add("Serial (last 8)", f.serial);
        add("Firmware", f.firmware);
        add("CPU", f.cpu);
        if (f.ram_kb) add("RAM", fmt_kb(f.ram_kb));
        if (s->uptime_ok) add("Uptime", fmt_uptime(s->uptime_s));
    }
    for (const auto &kv : extra) rows.push_back(info(kv.first, kv.second));
    return rows;
}

std::vector<Row> period_rows(int period_s)
{
    std::vector<Row> rows;
    for (const auto &p : std::vector<std::pair<int, const char *>>{{1, "Every second"}, {2, "Every 2 seconds"},
                                                                    {5, "Every 5 seconds"}}) {
        Row r;
        r.type = RowType::Choice;
        r.label = p.second;
        r.id = p.first;
        r.on = p.first == period_s;
        rows.push_back(r);
    }
    return rows;
}

/* ---------------------------------------------------------------- charts */

namespace {

std::string window_of(const Ring &ring, int period_s)
{
    (void)ring;
    return "last " + fmt_window(static_cast<int>(kHistory), period_s);
}

} // namespace

ChartText cpu_chart(const Snapshot &s)
{
    ChartText t;
    t.title[0] = "CPU " + (s.cpu.ok ? fmt_pct(s.cpu.total) : std::string(kWait));
    t.range = "0-100%, " + window_of(s.hist.cpu, s.period_s);
    return t;
}

ChartText memory_chart(const Snapshot &s)
{
    ChartText t;
    if (s.mem.ok) {
        t.title[0] = "RAM " + pct_of(s.mem.used(), s.mem.total);
        t.title[1] = s.mem.swap_total ? "Swap " + pct_of(s.mem.swap_used(), s.mem.swap_total) : "Swap off";
    } else {
        t.title[0] = "RAM ...";
    }
    t.range = "0-100%, " + window_of(s.hist.mem, s.period_s);
    return t;
}

ChartText thermal_chart(const Snapshot &s, int lo, int hi)
{
    ChartText t;
    t.title[0] = "CPU " + (s.temp.ok ? fmt_temp(s.temp.c) : std::string("Unknown"));
    if (s.temp.ok) t.tone = temp_tone(temp_state(s.temp.c));
    t.range = std::to_string(lo) + "-" + std::to_string(hi) + " \xC2\xB0" "C, " + window_of(s.hist.temp, s.period_s);
    return t;
}

ChartText storage_chart(const Snapshot &s, double top_bps)
{
    ChartText t;
    t.title[0] = "Read " + fmt_rate(s.disk.read_bps);
    t.title[1] = "Write " + fmt_rate(s.disk.write_bps);
    t.range = "0-" + fmt_rate(top_bps) + ", " + window_of(s.hist.rd, s.period_s);
    return t;
}

ChartText network_chart(const Snapshot &s, double top_bps)
{
    ChartText t;
    const NetInfo *n = main_net(s);
    t.title[0] = (n ? n->name + " in " : std::string("In ")) + fmt_rate(n ? n->rx_bps : 0);
    t.title[1] = "Out " + fmt_rate(n ? n->tx_bps : 0);
    t.range = "0-" + fmt_rate(top_bps) + ", " + window_of(s.hist.rx, s.period_s);
    return t;
}

void temp_range(const Ring &temps, int &lo, int &hi)
{
    lo = 20;
    hi = 90;
    if (temps.size() == 0) return;
    const double mn = temps.min(), mx = temps.max();
    while (mn < lo + 2 && lo > -40) lo -= 10;
    while (mx > hi - 2 && hi < 150) hi += 10;
}

double rate_ceiling(double max_bps)
{
    const double kb = max_bps / 1024.0;
    if (kb < 1000.0) return nice_ceiling(kb, 10.0) * 1024.0;
    return nice_ceiling(max_bps / (1024.0 * 1024.0), 1.0) * 1024.0 * 1024.0;
}

} // namespace sm
