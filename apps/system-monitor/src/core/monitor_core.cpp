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

/* use thresholds (%) */
constexpr double kCpuWarn = 80, kCpuError = 95;
constexpr double kFullWarn = 85, kFullError = 95; /* memory, swap, filesystems (the Storage tile's) */

Row info(const std::string &label, std::vector<std::string> values, Tone tone = Tone::Normal)
{
    Row r;
    r.type = RowType::Item;
    r.label = label;
    r.values = std::move(values);
    r.tone = tone;
    return r;
}

Row section(const std::string &label)
{
    Row r;
    r.type = RowType::Section;
    r.label = label;
    return r;
}

/* a row that opens another page (All processes) */
Row nav(const std::string &label, std::vector<std::string> values, int id)
{
    Row r = info(label, std::move(values));
    r.id = id;
    r.aff = Aff::Open;
    return r;
}

/* the processes are read only while a page shows them: their place says so until the first table */
Row reading_processes() { return info("Reading processes", {kWait}); }

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

double pct_value(std::uint64_t part, std::uint64_t whole)
{
    return whole ? 100.0 * static_cast<double>(part) / static_cast<double>(whole) : 0.0;
}

std::string fmt_watts(double w)
{
    char buf[24];
    std::snprintf(buf, sizeof(buf), "%.1f W", w);
    return buf;
}

std::string fmt_hm(int minutes)
{
    char buf[24];
    std::snprintf(buf, sizeof(buf), "%d:%02d", minutes / 60, minutes % 60);
    return buf;
}

/* The battery's colour: green while charging, full or well charged, amber under 20 %, red under 10 % (not charging). */
Tone battery_tone(const BatteryInfo &b)
{
    if (b.charging() || b.status == "Full" || b.status == "Not charging" || b.percent < 0) return Tone::Good;
    if (b.percent < 10) return Tone::Error;
    if (b.percent < 20) return Tone::Warn;
    return Tone::Good;
}

/* The Temp & power page's power row: the power_status() sentence first, then shorter wordings for a narrow row.
 * Under-voltage now: the short state only; the advice is a row of its own under it (thermal_rows). */
Row power_row(const PowerInfo &p, bool hot)
{
    const Status st = power_status(p, hot);
    const FlagState *f = p.flags;
    std::vector<std::string> v{st.text};
    if (p.source == PowerSource::NoAccess) v = {"No access: needs the video group", "No access"};
    else if (f[kUnderVoltage].now) v = {"Under-voltage now", "UV now"};
    else if (f[kThrottled].now) v.insert(v.end(), {hot ? "Throttled now: too hot" : "Throttled now", "Throttled now"});
    else if (f[kSoftTempLimit].now) v.insert(v.end(), {"Temp limit now"});
    else if (f[kFreqCapped].now) v.insert(v.end(), {"Capped now"});
    else if (f[kUnderVoltage].occurred) v.insert(v.end(), {"Under-voltage since boot", "UV since boot"});
    else if (f[kThrottled].occurred) v.insert(v.end(), {"Throttled since boot"});
    else if (f[kSoftTempLimit].occurred) v.insert(v.end(), {"Temp limit since boot"});
    else if (f[kFreqCapped].occurred) v.insert(v.end(), {"Capped since boot"});
    else v = {"OK"};
    return info("Power", v, st.tone == Tone::Normal ? Tone::Good : st.tone);
}

/* A power flag's value colour: now red (under-voltage, throttled) or amber (capped, soft limit); seen or since boot
 * amber; never light green. */
Tone flag_tone(int flag, const FlagState &s)
{
    if (s.now) return flag == kUnderVoltage || flag == kThrottled ? Tone::Error : Tone::Warn;
    if (s.occurred || s.seen) return Tone::Warn;
    return Tone::Good;
}

} // namespace

/* ---------------------------------------------------------------- list rows */

std::string fit_value(const std::string &label, const std::vector<std::string> &values, int room, int gap,
                      const TextWidth &width)
{
    if (values.empty()) return std::string();
    if (!width) return values.front();
    const int label_w = width(label, false);
    for (const std::string &v : values)
        if (label_w + gap + width(v, true) <= room) return v;
    return values.back();
}

std::vector<ListItem> list_items(const std::vector<Row> &rows, int room, int gap, const TextWidth &width)
{
    return list_items(rows, {}, {}, room, gap, width);
}

std::vector<ListItem> list_items(const std::vector<Row> &rows, const std::vector<Row> &old_rows,
                                 const std::vector<ListItem> &old_items, int room, int gap, const TextWidth &width)
{
    std::vector<ListItem> items;
    items.reserve(rows.size());
    for (std::size_t i = 0; i < rows.size(); ++i) {
        const Row &r = rows[i];
        if (i < old_rows.size() && i < old_items.size() && old_rows[i] == r) {
            items.push_back(old_items[i]);
            continue;
        }
        ListItem it;
        it.section = r.type == RowType::Section;
        it.label = r.label;
        it.tone = r.tone;
        it.id = r.id;
        it.aff = r.aff;
        if (r.type == RowType::Item) it.value = fit_value(r.label, r.values, room, gap, width);
        items.push_back(it);
    }
    return items;
}

Tone use_tone(double pct, double warn, double error)
{
    if (pct >= error) return Tone::Error;
    if (pct >= warn) return Tone::Warn;
    return Tone::Normal;
}

const char *view_empty_text(View view) { return view == View::Processes ? "Reading processes..." : "Reading..."; }

bool view_read_only(View view)
{
    return view == View::Thermal || view == View::Storage || view == View::Network || view == View::About;
}

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

View tile_target(int id)
{
    switch (id) {
    case kTilePower: return View::Thermal;
    case kTileCpu: return View::Cpu;
    case kTileTemp: return View::Thermal;
    case kTileMem: return View::Memory;
    case kTileStorage: return View::Storage;
    case kTileNet: return View::Network;
    default: return View::Overview;
    }
}

View footer_target(int id)
{
    switch (id) {
    case kFootProcs: return View::Processes;
    case kFootSystem: return View::About;
    case kFootPeriod: return View::Period;
    default: return View::Overview;
    }
}

/* ---------------------------------------------------------------- overview */

namespace {

TileText tile(int id, const char *title)
{
    TileText t;
    t.id = id;
    t.title = title;
    t.value = {kWait};
    t.value_dim = true;
    return t;
}

/* Power: the most important tile. Its words: "Power OK" (green), "Under-voltage" now (red) or since boot (amber),
 * "Throttled", "Temp limit", "Capped"; its frame in the same colour. */
TileText power_tile(const Snapshot &s)
{
    TileText t = tile(kTilePower, "Power");
    t.frame = true;
    const PowerInfo &p = s.power;
    const FlagState *f = p.flags;
    const bool hot = s.temp.ok && temp_state(s.temp.c) == TempState::Hot;
    t.value_dim = false;
    if (!s.sys.pi && p.source == PowerSource::None) { /* a PC: the battery, else nothing to say */
        const BatteryInfo &b = s.battery;
        if (!b.present) {
            t.value = {"No battery", "None"};
            t.value_dim = true;
            t.detail = {"Mains power", "Mains"};
            return t;
        }
        const std::string pct = b.percent >= 0 ? std::to_string(b.percent) + "%" : std::string("?");
        t.value = {"Battery " + pct, pct};
        t.value_tone = t.frame_tone = battery_tone(b);
        std::vector<std::string> d;
        const std::string what = b.status.empty() ? std::string("Battery") : b.status;
        const std::string w = b.has_power ? fmt_watts(b.power_w) : std::string();
        const std::string left = b.minutes >= 0 ? fmt_hm(b.minutes) + (b.charging() ? " to full" : " left") : std::string();
        if (!w.empty() && !left.empty()) d.push_back(what + ", " + w + ", " + left);
        if (!w.empty()) d.push_back(what + ", " + w);
        d.push_back(what);
        t.detail = d;
        t.detail_tone = t.value_tone == Tone::Good ? Tone::Normal : t.value_tone;
        return t;
    }
    switch (p.source) {
    case PowerSource::None:
        t.value = {"Unknown"};
        t.value_dim = true;
        t.detail = {"No firmware flags", "Not available"};
        return t;
    case PowerSource::NoAccess:
        t.value = {"No access"};
        t.value_tone = t.frame_tone = t.detail_tone = Tone::Warn;
        t.detail = {"Needs the video group", "video group"};
        return t;
    default: break;
    }
    if (f[kUnderVoltage].now) {
        t.value = {"Under-voltage", "UV now"};
        t.value_tone = t.frame_tone = t.detail_tone = Tone::Error;
        t.detail = {"Now: check the power supply", "Check the supply", "Now"};
    } else if (f[kThrottled].now) {
        t.value = {"Throttled"};
        t.value_tone = t.frame_tone = t.detail_tone = Tone::Error;
        t.detail = {hot ? "Now: the CPU is too hot" : "Now: the CPU runs slower", "Now"};
    } else if (f[kSoftTempLimit].now) {
        t.value = {"Temp limit"};
        t.value_tone = t.frame_tone = t.detail_tone = Tone::Warn;
        t.detail = {"Now: the CPU runs slower", "Now"};
    } else if (f[kFreqCapped].now) {
        t.value = {"Capped"};
        t.value_tone = t.frame_tone = t.detail_tone = Tone::Warn;
        t.detail = {"Now: frequency capped", "Now"};
    } else if (f[kUnderVoltage].occurred) {
        t.value = {"Under-voltage", "UV earlier"};
        t.value_tone = t.frame_tone = t.detail_tone = Tone::Warn;
        const FlagState &uv = f[kUnderVoltage];
        if (uv.seen && uv.last_s >= 0)
            t.detail = {"Happened since boot, last " + fmt_since_boot(uv.last_s), "Happened since boot", "Since boot"};
        else t.detail = {"Happened since boot", "Since boot"};
    } else if (f[kThrottled].occurred || f[kSoftTempLimit].occurred || f[kFreqCapped].occurred) {
        t.value = {"Throttled", "Slowed"};
        t.value_tone = t.frame_tone = t.detail_tone = Tone::Warn;
        t.detail = {"Happened since boot", "Since boot"};
    } else {
        t.value = {"Power OK", "OK"};
        t.value_tone = t.frame_tone = Tone::Good;
        const bool alarm = p.source == PowerSource::Alarm;
        if (p.supply_ok)
            t.detail = {fmt_volts(p.supply_v) + " input, no flags since boot", fmt_volts(p.supply_v) + " input",
                        fmt_volts(p.supply_v)};
        else if (alarm) t.detail = {"No under-voltage alarm", "No alarm"};
        else t.detail = {"No flags since boot", "No flags"};
    }
    return t;
}

} // namespace

std::vector<TileText> overview_tiles(const Snapshot *s)
{
    std::vector<TileText> tiles;
    tiles.push_back(tile(kTilePower, "Power"));
    tiles.push_back(tile(kTileCpu, "CPU"));
    tiles.push_back(tile(kTileTemp, "Temperature"));
    tiles.push_back(tile(kTileMem, "Memory"));
    tiles.push_back(tile(kTileStorage, "Storage"));
    tiles.push_back(tile(kTileNet, "Network"));
    tiles[kTileCpu].spark = Spark::Cpu;
    tiles[kTileTemp].spark = Spark::Temp;
    tiles[kTileMem].spark = Spark::Mem;
    tiles[kTileStorage].spark = Spark::Bar;
    tiles[kTileNet].spark = Spark::Rx;
    tiles[kTilePower].frame = true;
    if (!s) return tiles;

    tiles[kTilePower] = power_tile(*s);

    TileText &cpu = tiles[kTileCpu];
    if (s->cpu.ok) {
        cpu.value = {fmt_pct(s->cpu.total)};
        cpu.value_dim = false;
    }
    if (s->sys.cores > 0) cpu.hint = std::to_string(s->sys.cores) + (s->sys.cores == 1 ? " core" : " cores");
    {
        const std::string load = s->load.ok ? "load " + load_short(s->load) : std::string();
        std::vector<std::string> d;
        if (s->cpu.cur_mhz > 0 && s->cpu.max_mhz > 0 && !load.empty())
            d.push_back(std::to_string(s->cpu.cur_mhz) + " / " + fmt_mhz(s->cpu.max_mhz) + ", " + load);
        if (s->cpu.cur_mhz > 0 && !load.empty()) d.push_back(fmt_mhz(s->cpu.cur_mhz) + ", " + load);
        if (s->cpu.cur_mhz > 0) d.push_back(fmt_mhz(s->cpu.cur_mhz));
        else if (!load.empty()) d.push_back(load);
        cpu.detail = d;
    }

    TileText &temp = tiles[kTileTemp];
    if (s->temp.ok) {
        const TempState st = temp_state(s->temp.c);
        temp.value = {fmt_temp(s->temp.c), fmt_temp_short(s->temp.c)};
        temp.value_dim = false;
        temp.value_tone = st == TempState::Ok ? Tone::Good : temp_tone(st);
        switch (st) {
        case TempState::Ok: temp.detail = {"OK, warm from 70 \xC2\xB0" "C", "OK"}; break;
        case TempState::Warm: temp.detail = {"Warm, hot from 80 \xC2\xB0" "C", "Warm"}; break;
        case TempState::Hot: temp.detail = {s->sys.pi ? "Hot: the Pi slows down" : "Hot: the CPU slows down", "Hot"}; break;
        }
        temp.detail_tone = st == TempState::Ok ? Tone::Normal : temp_tone(st);
    } else {
        temp.value = {"Unknown"};
        temp.detail = {"No thermal zone"};
        temp.spark = Spark::None;
    }
    if (s->fan.present) {
        if (s->fan.rpm >= 0) temp.hint = "fan " + std::to_string(s->fan.rpm) + " rpm";
        else if (s->fan.state >= 0 && s->fan.max_state > 0)
            temp.hint = "fan " + std::to_string(s->fan.state) + "/" + std::to_string(s->fan.max_state);
    }

    TileText &mem = tiles[kTileMem];
    if (s->mem.ok) {
        const MemInfo &m = s->mem;
        mem.value = {pct_of(m.used(), m.total)};
        mem.value_dim = false;
        const std::string used = fmt_used_of(m.used() * 1024ull, m.total * 1024ull);
        if (m.swap_total)
            mem.detail = {used + (m.swap_used() ? ", swap " + fmt_kb(m.swap_used()) : std::string(", swap unused")), used,
                          fmt_kb(m.used())};
        else mem.detail = {used + ", no swap", used, fmt_kb(m.used())};
    }

    TileText &st = tiles[kTileStorage];
    if (const FsInfo *f = root_fs(*s)) {
        const double pct = f->pct();
        st.value = {fmt_pct(pct)};
        st.value_dim = false;
        st.value_tone = st.bar_tone = pct >= 95 ? Tone::Error : pct >= 85 ? Tone::Warn : Tone::Normal;
        st.bar_pct = pct;
        st.hint = f->dir;
        st.detail = {fmt_used_of(f->used, f->total) + " used", fmt_used_of(f->used, f->total)};
    } else {
        st.value = {"Unknown"};
        st.spark = Spark::None;
    }

    TileText &net = tiles[kTileNet];
    net.value_dim = false;
    if (const NetInfo *n = main_net(*s)) {
        net.hint = n->name;
        if (n->wireless && (n->has_level || (n->link_valid && n->link.has_signal)))
            net.hint += " " + std::to_string(n->link_valid && n->link.has_signal ? n->link.signal_dbm
                                                                                 : static_cast<int>(n->level_dbm)) +
                        " dBm";
        if (!n->ipv4.empty()) {
            net.value = {address_only(n->ipv4)};
        } else {
            net.value = {"Not connected", "Offline"};
            net.value_tone = Tone::Warn;
        }
        net.detail = {"In " + fmt_rate(n->rx_bps) + ", out " + fmt_rate(n->tx_bps), "In " + fmt_rate(n->rx_bps)};
    } else {
        net.value = {"No network", "None"};
        net.value_tone = Tone::Warn;
        net.detail = {"No interface"};
        net.spark = Spark::None;
    }
    return tiles;
}

std::vector<std::vector<std::string>> overview_footer(const Snapshot *s, int period_s)
{
    std::vector<std::vector<std::string>> f(kFootCount);
    if (s && s->proc_count > 0) f[kFootProcs] = {"Processes " + std::to_string(s->proc_count), "Processes"};
    else f[kFootProcs] = {"Processes"};
    f[kFootSystem] = {"System"};
    f[kFootPeriod] = {"Every " + std::to_string(period_s) + " s", std::to_string(period_s) + " s"};
    return f;
}

/* ---------------------------------------------------------------- CPU */

std::vector<Row> cpu_rows(const Snapshot *s)
{
    std::vector<Row> rows;
    if (!s) return rows;
    rows.push_back(info("Total", {s->cpu.ok ? fmt_pct(s->cpu.total) : kWait},
                        s->cpu.ok ? use_tone(s->cpu.total, kCpuWarn, kCpuError) : Tone::Normal));
    for (std::size_t i = 0; i < s->cpu.cores.size(); ++i)
        rows.push_back(info("Core " + std::to_string(i), {fmt_pct(s->cpu.cores[i])},
                            use_tone(s->cpu.cores[i], kCpuWarn, kCpuError)));
    rows.push_back(info("Frequency", freq_values(s->cpu)));
    if (!s->cpu.governor.empty()) rows.push_back(info("Governor", {s->cpu.governor}));
    if (s->load.ok) {
        rows.push_back(info("Load", {load_text(s->load), load_short(s->load)}));
        rows.push_back(info("Tasks", {std::to_string(s->load.running) + " running of " + std::to_string(s->load.tasks),
                                      std::to_string(s->load.running) + " / " + std::to_string(s->load.tasks)}));
    }
    if (s->uptime_ok) rows.push_back(info("Uptime", {fmt_uptime(s->uptime_s)}));
    rows.push_back(section("Top processes by CPU"));
    if (!s->procs_valid) {
        rows.push_back(reading_processes());
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
    if (!s) return rows;
    if (s->mem.ok) {
        const MemInfo &m = s->mem;
        std::vector<std::string> used = used_of_values(m.used(), m.total);
        used.insert(used.begin(), fmt_kb(m.used()) + " of " + fmt_kb(m.total) + " (" + pct_of(m.used(), m.total) + ")");
        rows.push_back(info("Used", used, use_tone(pct_value(m.used(), m.total), kFullWarn, kFullError)));
        rows.push_back(info("Available", {fmt_kb(m.has_available ? m.available : m.free + m.cache() + m.buffers)}));
        rows.push_back(info("Cache", {fmt_kb(m.cache())}));
        rows.push_back(info("Buffers", {fmt_kb(m.buffers)}));
        rows.push_back(info("Free", {fmt_kb(m.free)}));
        rows.push_back(info("Shared", {fmt_kb(m.shmem)}));
        if (m.swap_total)
            rows.push_back(info("Swap", used_of_values(m.swap_used(), m.swap_total),
                                use_tone(pct_value(m.swap_used(), m.swap_total), kFullWarn, kFullError)));
        else rows.push_back(info("Swap", {"Off"}));
    } else {
        rows.push_back(info("Memory", {"Unknown"}));
    }
    rows.push_back(section("Top processes by memory"));
    if (!s->procs_valid) {
        rows.push_back(reading_processes());
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
    if (!s) return rows;
    const bool hot = s->temp.ok && temp_state(s->temp.c) == TempState::Hot;
    if (s->power.source != PowerSource::None) rows.push_back(power_row(s->power, hot));
    if (s->power.source != PowerSource::None && s->power.source != PowerSource::NoAccess &&
        s->power.flags[kUnderVoltage].now)
        rows.push_back(info("Check the power supply", {})); /* the advice the short Power value leaves out */
    if (s->power.source == PowerSource::NoAccess) rows.push_back(info("Fix", {"sudo usermod -aG video $USER"}));
    if (s->battery.present) { /* laptops, tablets: only when there is a battery */
        const BatteryInfo &b = s->battery;
        const std::string pct = b.percent >= 0 ? std::to_string(b.percent) + "%" : std::string("Unknown");
        std::vector<std::string> v;
        if (!b.status.empty()) v.push_back(pct + ", " + (b.status == "Not charging" ? "not charging" : b.status == "Full"
                                                           ? "full" : b.charging() ? "charging" : "discharging"));
        v.push_back(pct);
        rows.push_back(info("Battery", v, battery_tone(b)));
        if (b.has_power) rows.push_back(info(b.charging() ? "Charging power" : "Power draw", {fmt_watts(b.power_w)}));
        if (b.minutes >= 0) rows.push_back(info(b.charging() ? "Time to full" : "Time left", {fmt_hm(b.minutes)}));
    }
    if (s->temp.ok) {
        const TempState st = temp_state(s->temp.c);
        const Tone tone = temp_tone(st);
        rows.push_back(info("CPU temperature", {fmt_temp(s->temp.c)}, tone));
        switch (st) {
        case TempState::Ok: rows.push_back(info("State", {"OK (warm from 70 \xC2\xB0" "C)", "OK"})); break;
        case TempState::Warm: rows.push_back(info("State", {"Warm (hot from 80 \xC2\xB0" "C)", "Warm"}, tone)); break;
        case TempState::Hot:
            rows.push_back(info("State", {s->sys.pi ? "Hot: the Pi slows down" : "Hot: the CPU slows down", "Hot"}, tone));
            break;
        }
        if (s->hist.temp.size() > 1) {
            const std::string window = fmt_window(static_cast<int>(s->hist.temp.size()), s->period_s);
            const double top = s->hist.temp.max();
            rows.push_back(info("Highest", {fmt_temp(top) + " in " + window, fmt_temp(top)}, temp_tone(temp_state(top))));
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
        for (int f = 0; f < kFlagCount; ++f)
            rows.push_back(info(flag_name(f), flag_values(s->power.flags[f], false), flag_tone(f, s->power.flags[f])));
    } else if (s->power.source == PowerSource::Alarm) {
        const FlagState &uv = s->power.flags[kUnderVoltage];
        rows.push_back(info(flag_name(kUnderVoltage), flag_values(uv, true), flag_tone(kUnderVoltage, uv)));
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
    if (!s) return rows;
    for (const FsInfo &f : s->fs) {
        const std::string pct = fmt_pct(f.pct());
        const std::string used = fmt_used_of(f.used, f.total);
        rows.push_back(info(f.dir, {used + " (" + pct + ")", used, pct}, use_tone(f.pct(), kFullWarn, kFullError)));
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
    if (!s) return rows;
    for (const NetInfo &n : s->net) {
        rows.push_back(section(n.name + (n.wireless ? " (Wi-Fi)" : "")));
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
    if (!s || !s->procs_valid) return rows;
    Row sort = nav("Sort by", {by == SortBy::Cpu ? "CPU" : "Memory"}, kRowSort);
    sort.aff = Aff::Edit; /* changes its own value in place */
    rows.push_back(sort);
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

std::vector<std::string> self_cost_values(const Snapshot &s)
{
    const std::string mem = s.self_rss ? fmt_bytes(s.self_rss) : std::string("?");
    if (!s.self_ok) return {"measuring, " + mem, mem};
    char cpu[16];
    std::snprintf(cpu, sizeof(cpu), "%.1f%%", s.self_cpu > 0 ? s.self_cpu : 0.0);
    return {std::string(cpu) + " CPU, " + mem, std::string(cpu) + ", " + mem};
}

std::string process_caption(const ProcRow &p)
{
    return "pid " + std::to_string(p.pid) + ", " + proc_state_name(p.state) + ", " + fmt_bytes(p.rss_bytes) + ", " +
           fmt_proc_cpu(p.cpu_pct) + " CPU";
}

std::string process_summary(const Snapshot &s)
{
    return "Top " + std::to_string(std::min<std::size_t>(kTopProcs, s.top_cpu.size())) + " of " +
           std::to_string(s.proc_count) + " processes, %CPU of one core";
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
        rows.push_back(info("This app", self_cost_values(*s)));
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
