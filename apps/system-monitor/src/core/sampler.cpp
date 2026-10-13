/*
 * SPDX-License-Identifier: MIT
 *
 * System Monitor: the sampler thread (see sampler.hpp).
 */
#include "sampler.hpp"

#include "text.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>

namespace sm {
namespace {

int clamp_period(int s) { return s >= 5 ? 5 : s >= 2 ? 2 : 1; }

std::string first_line(const std::string &text)
{
    const std::vector<std::string> lines = split_lines(text);
    return lines.empty() ? std::string() : trim(lines[0]);
}

bool all_digits(const std::string &s)
{
    if (s.empty()) return false;
    for (const char c : s)
        if (c < '0' || c > '9') return false;
    return true;
}

} // namespace

Sampler::Sampler(std::unique_ptr<Source> source, int period_s) : src_(std::move(source))
{
    if (!src_) src_ = make_system_source();
    source_desc_ = src_->describe();
    period_.store(clamp_period(period_s));
    page_size_ = src_->page_size();

    // the facts that do not change
    facts_.model = parse_model(src_->read("/proc/device-tree/model"));
    facts_.kernel = trim(src_->read("/proc/sys/kernel/osrelease"));
    bool ok = false;
    std::string os = src_->read("/etc/os-release", &ok);
    if (!ok) os = src_->read("/usr/lib/os-release");
    facts_.os = os_pretty_name(os);
    facts_.hostname = trim(src_->read("/proc/sys/kernel/hostname"));
    const std::string cpuinfo = src_->read("/proc/cpuinfo");
    facts_.serial = serial_tail(src_->read("/proc/device-tree/serial-number"), cpuinfo);
    const StatInfo stat = parse_stat(src_->read("/proc/stat"));
    facts_.cores = static_cast<int>(stat.cores.size());
    if (facts_.cores == 0) facts_.cores = parse_cpu_count(src_->read("/sys/devices/system/cpu/online"));
    facts_.cpu = cpu_description(cpuinfo, facts_.cores);
    facts_.ram_kb = parse_meminfo(src_->read("/proc/meminfo")).total;
    const std::string m = facts_.model;
    pi5_ = m.find("Raspberry Pi 5") != std::string::npos || m.find("Compute Module 5") != std::string::npos ||
           m.find("Raspberry Pi 500") != std::string::npos;
    discover();
}

Sampler::~Sampler() { stop(); }

void Sampler::discover()
{
    // the CPU's thermal zone (cpu-thermal), else the first one; a fan among the cooling devices
    std::string first_zone;
    for (const std::string &n : src_->list("/sys/class/thermal")) {
        const std::string base = "/sys/class/thermal/" + n;
        if (starts_with(n, "thermal_zone")) {
            if (first_zone.empty()) first_zone = base;
            const std::string type = trim(src_->read(base + "/type"));
            if (temp_path_.empty() && type.find("cpu") != std::string::npos) temp_path_ = base + "/temp";
        } else if (starts_with(n, "cooling_device") && fan_dev_.empty()) {
            const std::string type = trim(src_->read(base + "/type"));
            if (type.find("fan") != std::string::npos) fan_dev_ = base;
        }
    }
    if (temp_path_.empty() && !first_zone.empty()) temp_path_ = first_zone + "/temp";
    // hwmon: the under-voltage alarm (rpi_volt), a fan's speed
    for (const std::string &n : src_->list("/sys/class/hwmon")) {
        const std::string base = "/sys/class/hwmon/" + n;
        const std::string name = trim(src_->read(base + "/name"));
        if (name == "rpi_volt" && src_->exists(base + "/in0_lcrit_alarm")) uv_alarm_path_ = base + "/in0_lcrit_alarm";
        if (name.find("fan") != std::string::npos && src_->exists(base + "/fan1_input")) fan_rpm_path_ = base + "/fan1_input";
    }
    // the firmware's get_throttled node (some kernels: /sys/devices/platform/soc/soc:firmware/get_throttled)
    for (const std::string &n : src_->list("/sys/devices/platform")) {
        const std::string base = "/sys/devices/platform/" + n;
        if (n.find("firmware") != std::string::npos && src_->exists(base + "/get_throttled")) {
            throttle_node_ = base + "/get_throttled";
            break;
        }
        if (!starts_with(n, "soc")) continue;
        for (const std::string &c : src_->list(base)) {
            if (c.find("firmware") != std::string::npos && src_->exists(base + "/" + c + "/get_throttled")) {
                throttle_node_ = base + "/" + c + "/get_throttled";
                break;
            }
        }
        if (!throttle_node_.empty()) break;
    }
    // the root filesystem's disk in /proc/diskstats
    unsigned maj = 0, min = 0;
    if (src_->root_dev(maj, min)) {
        const std::vector<DiskStat> disks = parse_diskstats(src_->read("/proc/diskstats"));
        std::string part;
        for (const DiskStat &d : disks)
            if (d.major == maj && d.minor == min) part = d.name;
        if (!part.empty()) {
            const std::string disk = disk_of_partition(part);
            root_disk_ = part;
            for (const DiskStat &d : disks)
                if (d.name == disk) root_disk_ = disk;
        }
    }
}

void Sampler::start()
{
    if (thread_.joinable()) return;
    stop_.store(false);
    thread_ = std::thread([this] { loop(); });
}

void Sampler::stop()
{
    {
        std::lock_guard<std::mutex> lk(mu_);
        stop_.store(true);
        wake_ = true;
    }
    cv_.notify_all();
    if (thread_.joinable()) thread_.join();
}

void Sampler::set_period(int period_s)
{
    const int p = clamp_period(period_s);
    if (period_.exchange(p) == p) return;
    period_changed_.store(true);
    {
        std::lock_guard<std::mutex> lk(mu_);
        wake_ = true;
    }
    cv_.notify_all();
}

void Sampler::set_want_procs(bool want)
{
    if (want_procs_.exchange(want) == want || !want) return;
    procs_kick_.store(true);
    {
        std::lock_guard<std::mutex> lk(mu_);
        wake_ = true;
    }
    cv_.notify_all();
}

void Sampler::set_want_link(bool want)
{
    if (want_link_.exchange(want) == want || !want) return;
    {
        std::lock_guard<std::mutex> lk(mu_);
        wake_ = true;
    }
    cv_.notify_all();
}

std::shared_ptr<const Snapshot> Sampler::latest() const
{
    std::lock_guard<std::mutex> lk(mu_);
    return latest_;
}

void Sampler::step()
{
    if (period_changed_.exchange(false)) hist_ = History();
    sample(true);
}

void Sampler::loop()
{
    using clock = std::chrono::steady_clock;
    // a fixture with a scenario: its time is virtual, the history is filled at once
    if (src_->virtual_time())
        for (std::size_t i = 0; i + 1 < kHistory && !stop_.load(); ++i) sample(false);
    sample(true);
    // the first rates need a second sample: soon after the first, then once per period
    clock::time_point next = clock::now() + std::chrono::milliseconds(src_->virtual_time() ? 1000 * period_.load() : 300);
    while (!stop_.load()) {
        {
            std::unique_lock<std::mutex> lk(mu_);
            cv_.wait_until(lk, next, [this] { return stop_.load() || wake_; });
            wake_ = false;
        }
        if (stop_.load()) break;
        if (period_changed_.exchange(false)) hist_ = History();
        const bool kick = procs_kick_.exchange(false);
        sample(true);
        // the process table needs two scans: the second one half a second after the first
        next = clock::now() + std::chrono::milliseconds(kick ? 500 : 1000 * period_.load());
    }
}

CmdResult Sampler::vcgencmd(const std::vector<std::string> &args)
{
    std::vector<std::string> argv{"vcgencmd"};
    argv.insert(argv.end(), args.begin(), args.end());
    return src_->run(argv, 1000);
}

void Sampler::sample(bool publish)
{
    const int period = period_.load();
    src_->begin_sample(period * 1000);
    const std::int64_t now = src_->now_ms();
    const double dt = have_prev_ ? static_cast<double>(now - prev_ms_) / 1000.0 : 0.0;

    auto s = std::make_shared<Snapshot>();
    s->period_s = period;
    s->uptime_ok = parse_uptime(src_->read("/proc/uptime"), s->uptime_s);
    read_cpu(*s, dt);
    read_memory(*s);
    read_thermal(*s);
    read_power(*s);
    read_storage(*s, dt);
    read_network(*s, dt);
    read_bluetooth(*s);
    read_procs(*s);

    // the history (a value not known in this sample is left out of its ring)
    if (s->cpu.ok) hist_.cpu.push(static_cast<float>(s->cpu.total));
    if (s->mem.ok) {
        hist_.mem.push(static_cast<float>(100.0 * static_cast<double>(s->mem.used()) / static_cast<double>(s->mem.total)));
        hist_.swap.push(s->mem.swap_total
                            ? static_cast<float>(100.0 * static_cast<double>(s->mem.swap_used()) /
                                                 static_cast<double>(s->mem.swap_total))
                            : 0.0f);
    }
    if (s->temp.ok) hist_.temp.push(static_cast<float>(s->temp.c));
    if (dt > 0) {
        for (const NetInfo &n : s->net)
            if (n.name == s->main_iface) {
                hist_.rx.push(static_cast<float>(n.rx_bps));
                hist_.tx.push(static_cast<float>(n.tx_bps));
            }
        if (s->disk.ok) {
            hist_.rd.push(static_cast<float>(s->disk.read_bps));
            hist_.wr.push(static_cast<float>(s->disk.write_bps));
        }
    }
    s->hist = hist_;
    s->sys = facts_;
    s->sys.firmware = firmware_;
    prev_ms_ = now;
    have_prev_ = true;
    if (!publish) return;
    std::lock_guard<std::mutex> lk(mu_);
    s->seq = seq_.load() + 1;
    latest_ = s;
    seq_.store(s->seq);
}

void Sampler::read_cpu(Snapshot &s, double)
{
    const StatInfo stat = parse_stat(src_->read("/proc/stat"));
    if (stat.ok && prev_stat_.ok) {
        const double total = busy_pct(prev_stat_.all, stat.all);
        if (total >= 0) {
            s.cpu.ok = true;
            s.cpu.total = total;
            for (std::size_t i = 0; i < stat.cores.size(); ++i) {
                double pct = 0;
                for (std::size_t j = 0; j < prev_stat_.core_ids.size(); ++j)
                    if (prev_stat_.core_ids[j] == stat.core_ids[i]) pct = std::max(0.0, busy_pct(prev_stat_.cores[j], stat.cores[i]));
                s.cpu.cores.push_back(pct);
            }
        }
    }
    if (stat.ok) {
        cur_total_ = stat.all.total();
        cur_cores_ = static_cast<int>(stat.cores.size());
        prev_stat_ = stat;
    }
    const std::string f = "/sys/devices/system/cpu/cpu0/cpufreq/";
    parse_khz_as_mhz(src_->read(f + "scaling_cur_freq"), s.cpu.cur_mhz);
    if (!parse_khz_as_mhz(src_->read(f + "cpuinfo_max_freq"), s.cpu.max_mhz))
        parse_khz_as_mhz(src_->read(f + "scaling_max_freq"), s.cpu.max_mhz);
    s.cpu.governor = trim(src_->read(f + "scaling_governor"));
    s.load = parse_loadavg(src_->read("/proc/loadavg"));
}

void Sampler::read_memory(Snapshot &s) { s.mem = parse_meminfo(src_->read("/proc/meminfo")); }

void Sampler::read_thermal(Snapshot &s)
{
    if (!temp_path_.empty()) s.temp.ok = parse_millicelsius(src_->read(temp_path_), s.temp.c);
    if (!fan_dev_.empty()) {
        std::int64_t cur = -1, max = -1;
        if (parse_i64(src_->read(fan_dev_ + "/cur_state"), cur) && parse_i64(src_->read(fan_dev_ + "/max_state"), max)) {
            s.fan.present = true;
            s.fan.state = static_cast<int>(cur);
            s.fan.max_state = static_cast<int>(max);
        }
    }
    if (!fan_rpm_path_.empty()) {
        std::int64_t rpm = -1;
        if (parse_i64(src_->read(fan_rpm_path_), rpm) && rpm >= 0) {
            s.fan.present = true;
            s.fan.rpm = static_cast<int>(rpm);
        }
    }
}

void Sampler::read_power(Snapshot &s)
{
    const std::int64_t now = src_->now_ms();
    const double up = s.uptime_ok ? s.uptime_s : static_cast<double>(now) / 1000.0;
    if (firmware_.empty() && vc_ != Vc::Absent && vc_ != Vc::Denied && !version_asked_) {
        version_asked_ = true;
        const CmdResult r = vcgencmd({"version"});
        if (r.not_found()) {
            vc_ = Vc::Absent;
            std::fprintf(stderr, "[system-monitor] vcgencmd not found: no firmware flags from it\n");
        } else if (vcgencmd_access_denied(r.output)) {
            vc_ = Vc::Denied;
            std::fprintf(stderr, "[system-monitor] vcgencmd refused (not in the video group?): %s\n",
                         first_line(r.output).c_str());
        } else if (r.ok()) {
            vc_ = Vc::Works;
            firmware_ = parse_firmware_version(r.output);
        }
    }
    bool flags = false;
    if (!throttle_node_.empty()) {
        std::uint32_t bits = 0;
        if (parse_throttled(src_->read(throttle_node_), bits)) {
            tracker_.update(bits, up);
            power_.source = PowerSource::Firmware;
            power_.via = "sysfs";
            flags = true;
        }
    }
    if (!flags && (vc_ == Vc::Unknown || vc_ == Vc::Works) && now >= next_throttle_ms_) {
        next_throttle_ms_ = now + 2000;
        const CmdResult r = vcgencmd({"get_throttled"});
        std::uint32_t bits = 0;
        if (r.not_found()) {
            if (vc_ != Vc::Absent)
                std::fprintf(stderr, "[system-monitor] vcgencmd not found: no firmware flags from it\n");
            vc_ = Vc::Absent;
        } else if (vcgencmd_access_denied(r.output)) {
            vc_ = Vc::Denied;
            std::fprintf(stderr, "[system-monitor] vcgencmd refused (not in the video group?): %s\n",
                         first_line(r.output).c_str());
        } else if (r.ok() && parse_throttled(r.output, bits)) {
            vc_ = Vc::Works;
            vc_failures_ = 0;
            tracker_.update(bits, up);
            power_.source = PowerSource::Firmware;
            power_.via = "vcgencmd";
        } else if (++vc_failures_ >= 3) {
            vc_ = Vc::Failing;
            std::fprintf(stderr, "[system-monitor] vcgencmd get_throttled fails (%s): given up\n",
                         r.timed_out ? "timed out" : first_line(r.output).c_str());
        }
    }
    if (power_.source != PowerSource::Firmware) {
        if (!uv_alarm_path_.empty()) {
            std::int64_t alarm = 0;
            if (parse_i64(src_->read(uv_alarm_path_), alarm)) {
                tracker_.update_alarm(alarm != 0, up);
                power_.source = PowerSource::Alarm;
                power_.via = "hwmon";
            }
        } else if (vc_ == Vc::Denied) {
            power_.source = PowerSource::NoAccess;
        } else if (vc_ == Vc::Absent || vc_ == Vc::Failing) {
            power_.source = PowerSource::None;
        }
    }
    if (vc_ == Vc::Works && now >= next_volts_ms_) {
        next_volts_ms_ = now + 10000;
        double v = 0;
        if (pi5_) {
            const CmdResult r = vcgencmd({"pmic_read_adc", "EXT5V_V"});
            if (r.ok() && parse_volts(r.output, v)) {
                power_.supply_ok = true;
                power_.supply_v = v;
            }
        }
        const CmdResult r = vcgencmd({"measure_volts", "core"});
        if (r.ok() && parse_volts(r.output, v)) {
            power_.core_ok = true;
            power_.core_v = v;
        }
    }
    tracker_.fill(power_);
    s.power = power_;
}

void Sampler::read_storage(Snapshot &s, double dt)
{
    const std::vector<Mount> mounts = real_mounts(parse_mounts(src_->read("/proc/self/mounts")));
    for (const Mount &m : mounts) {
        FsInfo f;
        f.dir = m.dir;
        f.device = m.device;
        f.type = m.type;
        if (src_->fs_usage(m.dir, f.total, f.used, f.avail)) s.fs.push_back(f);
    }
    if (root_disk_.empty()) return;
    for (const DiskStat &d : parse_diskstats(src_->read("/proc/diskstats"))) {
        if (d.name != root_disk_) continue;
        const std::uint64_t rd = d.sectors_read * 512ull, wr = d.sectors_written * 512ull;
        s.disk.ok = true;
        s.disk.name = d.name;
        s.disk.read_total = rd;
        s.disk.written_total = wr;
        if (have_disk_ && dt > 0) {
            s.disk.read_bps = counter_rate(prev_rd_, rd, dt);
            s.disk.write_bps = counter_rate(prev_wr_, wr, dt);
        }
        prev_rd_ = rd;
        prev_wr_ = wr;
        have_disk_ = true;
    }
}

void Sampler::read_network(Snapshot &s, double dt)
{
    const std::vector<NetDev> devs = parse_net_dev(src_->read("/proc/net/dev"));
    const std::map<std::string, std::string> addrs = src_->ipv4();
    const std::vector<WirelessLevel> levels = parse_net_wireless(src_->read("/proc/net/wireless"));
    const std::string route = parse_default_route(src_->read("/proc/net/route"));
    const std::int64_t now = src_->now_ms();
    const bool want_link = want_link_.load();
    if (!want_link) links_.clear();
    const bool ask_iw = want_link && !iw_absent_ && (links_.empty() || now >= next_link_ms_);
    if (ask_iw) next_link_ms_ = now + 5000;
    std::map<std::string, std::pair<std::uint64_t, std::uint64_t>> counters;
    for (const NetDev &d : devs) {
        if (d.name == "lo") continue;
        NetInfo n;
        n.name = d.name;
        const std::string base = "/sys/class/net/" + d.name;
        n.state = trim(src_->read(base + "/operstate"));
        if (n.state.empty()) n.state = "unknown";
        n.wireless = src_->exists(base + "/wireless") || src_->exists(base + "/phy80211");
        for (const WirelessLevel &w : levels)
            if (w.iface == d.name) {
                n.wireless = true;
                n.has_level = true;
                n.level_dbm = w.level_dbm;
            }
        const auto a = addrs.find(d.name);
        if (a != addrs.end()) n.ipv4 = a->second;
        n.rx_total = d.rx_bytes;
        n.tx_total = d.tx_bytes;
        const auto p = prev_net_.find(d.name);
        if (p != prev_net_.end() && dt > 0) {
            n.rx_bps = counter_rate(p->second.first, d.rx_bytes, dt);
            n.tx_bps = counter_rate(p->second.second, d.tx_bytes, dt);
        }
        counters[d.name] = {d.rx_bytes, d.tx_bytes};
        if (n.wireless && ask_iw) {
            const CmdResult r = src_->run({"iw", "dev", d.name, "link"}, 1000);
            if (r.not_found()) {
                iw_absent_ = true;
                std::fprintf(stderr, "[system-monitor] iw not found: no Wi-Fi name or bit rate\n");
            } else if (r.ok()) {
                links_[d.name] = parse_iw_link(r.output);
            }
        }
        const auto l = links_.find(d.name);
        if (l != links_.end()) {
            n.link_valid = true;
            n.link = l->second;
        }
        s.net.push_back(n);
    }
    prev_net_ = counters;
    for (const NetInfo &n : s.net)
        if (n.name == route) s.main_iface = n.name;
    if (s.main_iface.empty())
        for (const NetInfo &n : s.net)
            if (n.state == "up" && !n.ipv4.empty()) {
                s.main_iface = n.name;
                break;
            }
    if (s.main_iface.empty())
        for (const NetInfo &n : s.net)
            if (n.wireless) {
                s.main_iface = n.name;
                break;
            }
    if (s.main_iface.empty() && !s.net.empty()) s.main_iface = s.net.front().name;
}

void Sampler::read_bluetooth(Snapshot &s)
{
    bool present = false, connected = false;
    for (const std::string &n : src_->list("/sys/class/bluetooth")) {
        if (!starts_with(n, "hci")) continue;
        present = true;
        if (n.find(':') != std::string::npos) connected = true;
    }
    if (!present) {
        s.bt = BtState::Absent;
        return;
    }
    bool blocked = false;
    for (const std::string &n : src_->list("/sys/class/rfkill")) {
        const std::string base = "/sys/class/rfkill/" + n;
        if (trim(src_->read(base + "/type")) != "bluetooth") continue;
        if (trim(src_->read(base + "/soft")) == "1" || trim(src_->read(base + "/hard")) == "1") blocked = true;
    }
    s.bt = blocked ? BtState::Off : connected ? BtState::Connected : BtState::On;
}

void Sampler::read_procs(Snapshot &s)
{
    const std::vector<std::string> names = src_->list("/proc"); /* one directory read: the count for the overview */
    if (!want_procs_.load()) {
        procs_baseline_ = false;
        prev_proc_ticks_.clear();
        s.proc_count = static_cast<int>(std::count_if(names.begin(), names.end(), all_digits));
        return;
    }
    std::vector<ProcSample> samples;
    for (const std::string &n : names) {
        if (!all_digits(n)) continue;
        bool ok = false;
        const std::string text = src_->read("/proc/" + n + "/stat", &ok);
        ProcStat st;
        if (ok && parse_proc_stat(text, st)) samples.push_back(proc_sample(st, page_size_));
    }
    s.proc_count = static_cast<int>(samples.size());
    if (procs_baseline_) {
        const int cores = cur_cores_ > 0 ? cur_cores_ : 1;
        const double per_core = cur_total_ > prev_proc_total_
                                    ? static_cast<double>(cur_total_ - prev_proc_total_) / cores
                                    : 0.0;
        const std::vector<ProcRow> rows = proc_rows(prev_proc_ticks_, samples, per_core);
        s.procs_valid = true;
        s.top_cpu = top_n(rows, SortBy::Cpu, 15);
        s.top_mem = top_n(rows, SortBy::Memory, 15);
    }
    prev_proc_ticks_.clear();
    for (const ProcSample &p : samples) prev_proc_ticks_[p.pid] = p.ticks;
    prev_proc_total_ = cur_total_;
    procs_baseline_ = true;
}

} // namespace sm
