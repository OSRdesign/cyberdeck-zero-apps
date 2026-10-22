/*
 * SPDX-License-Identifier: MIT
 *
 * System Monitor: the sampler thread (see sampler.hpp).
 */
#include "sampler.hpp"

#include "text.hpp"

#include <fcntl.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cmath>
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

std::int64_t real_ms()
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<std::int64_t>(ts.tv_sec) * 1000 + ts.tv_nsec / 1000000;
}

/* A small file of this process (/proc/self/...), always the real one (a fixture run measures itself too). */
bool read_own(const char *path, char *buf, std::size_t size)
{
    const int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return false;
    const ssize_t n = read(fd, buf, size - 1);
    close(fd);
    if (n <= 0) return false;
    buf[n] = '\0';
    return true;
}

} // namespace

Sampler::Sampler(std::unique_ptr<Source> source, int period_s) : src_(std::move(source))
{
    if (!src_) src_ = make_system_source();
    source_desc_ = src_->describe();
    period_.store(clamp_period(period_s));
    page_size_ = src_->page_size();
    clk_tck_ = sysconf(_SC_CLK_TCK) > 0 ? sysconf(_SC_CLK_TCK) : 100;
    own_page_ = sysconf(_SC_PAGESIZE) > 0 ? sysconf(_SC_PAGESIZE) : 4096;

    // the facts that do not change
    // the model: the device tree's (a Pi, another ARM board), else the PC's DMI name
    const std::string dt_model = parse_model(src_->read("/proc/device-tree/model"));
    pi_ = is_raspberry_model(dt_model);
    facts_.pi = pi_;
    facts_.model = dt_model;
    if (dt_model.empty())
        facts_.model = dmi_model(first_line(src_->read("/sys/class/dmi/id/sys_vendor")),
                                 first_line(src_->read("/sys/class/dmi/id/product_name")));
    if (!pi_) vc_ = Vc::Absent; // no vcgencmd, no firmware flags, no error about it: not asked at all
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
    find_battery();
}

Sampler::~Sampler() { stop(); }

void Sampler::discover()
{
    // the CPU's thermal zone (cpu-thermal), else the first one; a fan among the cooling devices
    // (a PC: find_thermal_pc() chooses among the zones and the hwmon temperatures)
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
    if (!pi_) find_thermal_pc();
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

/* A PC: the CPU's temperature from the package sensor (thermal zone x86_pkg_temp), else a zone with "cpu" in its type,
 * else the hwmon coretemp / k10temp (temp1_input), else acpitz, else the hottest zone that reads sensibly. */
void Sampler::find_thermal_pc()
{
    struct Cand {
        int rank;
        double c;
        std::string path;
    };
    std::vector<Cand> cands;
    const auto sane = [this](const std::string &path, double &c) {
        std::string text;
        return src_->read_into(path, text) && parse_millicelsius(text, c) && c > 0.0 && c < 130.0;
    };
    for (const std::string &n : src_->list("/sys/class/thermal")) {
        if (!starts_with(n, "thermal_zone")) continue;
        const std::string base = "/sys/class/thermal/" + n;
        const std::string type = trim(src_->read(base + "/type"));
        double c = 0;
        if (!sane(base + "/temp", c)) continue;
        int rank = 4;
        if (type == "x86_pkg_temp") rank = 0;
        else if (type.find("cpu") != std::string::npos) rank = 1;
        else if (type == "acpitz") rank = 3;
        cands.push_back({rank, c, base + "/temp"});
    }
    for (const std::string &n : src_->list("/sys/class/hwmon")) {
        const std::string base = "/sys/class/hwmon/" + n;
        const std::string name = trim(src_->read(base + "/name"));
        double c = 0;
        if ((name == "coretemp" || name == "k10temp") && sane(base + "/temp1_input", c))
            cands.push_back({2, c, base + "/temp1_input"});
    }
    const Cand *best = nullptr;
    for (const Cand &c : cands)
        if (!best || c.rank < best->rank || (c.rank == best->rank && c.rank == 4 && c.c > best->c)) best = &c;
    if (best) temp_path_ = best->path;
}

/* The first real battery: type Battery (a device's own battery, scope Device, is not the machine's). */
void Sampler::find_battery()
{
    bat_dir_.clear();
    for (const std::string &n : src_->list("/sys/class/power_supply")) {
        const std::string base = "/sys/class/power_supply/" + n;
        if (trim(src_->read(base + "/type")) != "Battery") continue;
        if (trim(src_->read(base + "/scope")) == "Device") continue;
        bat_dir_ = base;
        break;
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

void Sampler::wake()
{
    {
        std::lock_guard<std::mutex> lk(mu_);
        wake_ = true;
    }
    cv_.notify_all();
}

void Sampler::set_period(int period_s)
{
    const int p = clamp_period(period_s);
    if (period_.exchange(p) == p) return;
    period_changed_.store(true);
    wake();
}

void Sampler::set_want_procs(bool want)
{
    if (want_procs_.exchange(want) == want || !want) return;
    procs_kick_.store(true);
    wake();
}

void Sampler::set_want_link(bool want)
{
    if (want_link_.exchange(want) == want || !want) return;
    wake();
}

std::shared_ptr<const Snapshot> Sampler::latest() const
{
    std::lock_guard<std::mutex> lk(mu_);
    return latest_;
}

void Sampler::step()
{
    if (period_changed_.exchange(false)) hist_ = History();
    procs_kick_.store(false);
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

bool Sampler::due(std::int64_t &next_ms, std::int64_t now, int every_ms)
{
    if (now < next_ms) return false;
    next_ms = now + every_ms;
    return true;
}

void Sampler::sample(bool publish)
{
    const int period = period_.load();
    src_->begin_sample(period * 1000);
    const std::int64_t now = src_->now_ms();
    const double dt = have_prev_ ? static_cast<double>(now - prev_ms_) / 1000.0 : 0.0;

    auto s = std::make_shared<Snapshot>();
    s->period_s = period;
    s->uptime_ok = src_->read_into("/proc/uptime", buf_) && parse_uptime(buf_, s->uptime_s);
    read_cpu(*s);
    read_memory(*s);
    read_thermal(*s);
    read_power(*s);
    read_battery(*s);
    read_storage(*s, dt);
    read_network(*s, dt);
    read_bluetooth(*s);
    read_procs(*s);
    read_self(*s);

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

void Sampler::read_cpu(Snapshot &s)
{
    if (src_->read_into("/proc/stat", buf_) && parse_stat_into(buf_, cur_stat_)) {
        if (prev_stat_.ok) {
            const double total = busy_pct(prev_stat_.all, cur_stat_.all);
            if (total >= 0) {
                s.cpu.ok = true;
                s.cpu.total = total;
                s.cpu.cores.reserve(cur_stat_.cores.size());
                for (std::size_t i = 0; i < cur_stat_.cores.size(); ++i) {
                    double pct = 0;
                    for (std::size_t j = 0; j < prev_stat_.core_ids.size(); ++j)
                        if (prev_stat_.core_ids[j] == cur_stat_.core_ids[i])
                            pct = std::max(0.0, busy_pct(prev_stat_.cores[j], cur_stat_.cores[i]));
                    s.cpu.cores.push_back(pct);
                }
            }
        }
        cur_total_ = cur_stat_.all.total();
        cur_cores_ = static_cast<int>(cur_stat_.cores.size());
        std::swap(prev_stat_, cur_stat_); /* the two buffers trade places: no allocation */
    }
    // the frequency every sample; its maximum and the governor once in a while (they rarely change)
    const char *f = "/sys/devices/system/cpu/cpu0/cpufreq/";
    path_.assign(f).append("scaling_cur_freq");
    if (src_->read_into(path_, buf_)) parse_khz_as_mhz(buf_, cur_mhz_);
    if (due(next_cpufreq_ms_, src_->now_ms(), kSlowMs)) {
        path_.assign(f).append("cpuinfo_max_freq");
        if (!src_->read_into(path_, buf_) || !parse_khz_as_mhz(buf_, max_mhz_)) {
            path_.assign(f).append("scaling_max_freq");
            if (src_->read_into(path_, buf_)) parse_khz_as_mhz(buf_, max_mhz_);
        }
        path_.assign(f).append("scaling_governor");
        governor_ = src_->read_into(path_, buf_) ? trim(buf_) : std::string();
    }
    s.cpu.cur_mhz = cur_mhz_;
    s.cpu.max_mhz = max_mhz_;
    s.cpu.governor = governor_;
    if (src_->read_into("/proc/loadavg", buf_)) s.load = parse_loadavg(buf_);
}

void Sampler::read_memory(Snapshot &s)
{
    if (src_->read_into("/proc/meminfo", buf_)) s.mem = parse_meminfo(buf_);
}

void Sampler::read_thermal(Snapshot &s)
{
    if (!temp_path_.empty() && src_->read_into(temp_path_, buf_)) s.temp.ok = parse_millicelsius(buf_, s.temp.c);
    if (!fan_dev_.empty()) {
        std::int64_t cur = -1, max = -1;
        path_.assign(fan_dev_).append("/cur_state");
        const bool c = src_->read_into(path_, buf_) && parse_i64(buf_, cur);
        path_.assign(fan_dev_).append("/max_state");
        if (c && src_->read_into(path_, buf_) && parse_i64(buf_, max)) {
            s.fan.present = true;
            s.fan.state = static_cast<int>(cur);
            s.fan.max_state = static_cast<int>(max);
        }
    }
    if (!fan_rpm_path_.empty()) {
        std::int64_t rpm = -1;
        if (src_->read_into(fan_rpm_path_, buf_) && parse_i64(buf_, rpm) && rpm >= 0) {
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
        if (src_->read_into(throttle_node_, buf_) && parse_throttled(buf_, bits)) {
            tracker_.update(bits, up);
            power_.source = PowerSource::Firmware;
            power_.via = "sysfs";
            flags = true;
        }
    }
    // vcgencmd (a process start each time): its answer is kept between the calls, at most one call every 2 s
    if (!flags && (vc_ == Vc::Unknown || vc_ == Vc::Works) && due(next_throttle_ms_, now, kThrottleMs)) {
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
        std::int64_t alarm = 0;
        if (!uv_alarm_path_.empty() && src_->read_into(uv_alarm_path_, buf_) && parse_i64(buf_, alarm)) {
            tracker_.update_alarm(alarm != 0, up);
            power_.source = PowerSource::Alarm;
            power_.via = "hwmon";
        } else if (uv_alarm_path_.empty() && vc_ == Vc::Denied) {
            power_.source = PowerSource::NoAccess;
        } else if (uv_alarm_path_.empty() && (vc_ == Vc::Absent || vc_ == Vc::Failing)) {
            power_.source = PowerSource::None;
        }
    }
    // the voltages: the 5 V input (Pi 5 / CM5) every 10 s, the core voltage every 30 s
    if (vc_ == Vc::Works && pi5_ && due(next_supply_ms_, now, kSupplyMs)) {
        double v = 0;
        const CmdResult r = vcgencmd({"pmic_read_adc", "EXT5V_V"});
        if (r.ok() && parse_volts(r.output, v)) {
            power_.supply_ok = true;
            power_.supply_v = v;
        }
    }
    if (vc_ == Vc::Works && due(next_core_volts_ms_, now, kCoreVoltsMs)) {
        double v = 0;
        const CmdResult r = vcgencmd({"measure_volts", "core"});
        if (r.ok() && parse_volts(r.output, v)) {
            power_.core_ok = true;
            power_.core_v = v;
        }
    }
    tracker_.fill(power_);
    s.power = power_;
}

/* The battery: percent, state, power and the time left. Looked for again every 10 s while there is none (a battery
 * can be swapped) or its files stopped answering. */
void Sampler::read_battery(Snapshot &s)
{
    if ((bat_dir_.empty() || bat_failed_) && due(next_bat_scan_ms_, src_->now_ms(), kSlowMs)) {
        find_battery();
        bat_failed_ = false;
    }
    if (bat_dir_.empty()) return;
    const auto num = [&](const char *file, std::int64_t &v) {
        path_.assign(bat_dir_).append("/").append(file);
        return src_->read_into(path_, buf_) && parse_i64(trim(buf_), v);
    };
    BatteryInfo &b = s.battery;
    std::int64_t cap = -1, e_now = -1, e_full = -1, p_now = -1, v_now = -1, i_now = -1;
    path_.assign(bat_dir_).append("/status");
    if (!src_->read_into(path_, buf_)) {
        bat_failed_ = true;
        return;
    }
    b.present = true;
    b.status = trim(buf_);
    if (b.status == "Unknown") b.status.clear();
    const bool have_cap = num("capacity", cap);
    const bool have_e = num("energy_now", e_now) && num("energy_full", e_full);
    if (have_cap && cap >= 0 && cap <= 100) b.percent = static_cast<int>(cap);
    else if (have_e && e_full > 0) b.percent = static_cast<int>(std::min<std::int64_t>(100, 100 * e_now / e_full));
    // power: power_now (uW), else current_now (uA) x voltage_now (uV)
    if (num("power_now", p_now) && p_now > 0) {
        b.has_power = true;
        b.power_w = static_cast<double>(p_now) / 1e6;
    } else if (num("current_now", i_now) && num("voltage_now", v_now) && i_now != 0 && v_now > 0) {
        b.has_power = true;
        b.power_w = std::abs(static_cast<double>(i_now)) * static_cast<double>(v_now) / 1e12;
    }
    // the time left (energy in uWh over power in uW), only with a draw that means something
    if (have_e && b.has_power && b.power_w >= 0.3) {
        double hours = -1;
        if (b.discharging()) hours = static_cast<double>(e_now) / 1e6 / b.power_w;
        else if (b.charging() && e_full > e_now) hours = static_cast<double>(e_full - e_now) / 1e6 / b.power_w;
        if (hours >= 0 && hours < 100) b.minutes = static_cast<int>(hours * 60.0 + 0.5);
    }
}

void Sampler::read_storage(Snapshot &s, double dt)
{
    const std::int64_t now = src_->now_ms();
    // the list of real filesystems every 10 s (mounts are rare); their use (statvfs) every sample
    if (due(next_mounts_ms_, now, kSlowMs)) {
        fs_.clear();
        for (const Mount &m : real_mounts(parse_mounts(src_->read("/proc/self/mounts")))) {
            FsInfo f;
            f.dir = m.dir;
            f.device = m.device;
            f.type = m.type;
            fs_.push_back(f);
        }
    }
    for (const FsInfo &f : fs_) {
        FsInfo u = f;
        if (src_->fs_usage(f.dir, u.total, u.used, u.avail)) s.fs.push_back(u);
    }
    if (root_disk_.empty() || !src_->read_into("/proc/diskstats", buf_)) return;
    DiskStat d;
    if (!find_diskstat(buf_, root_disk_, d)) return;
    const std::uint64_t rd = d.sectors_read * 512ull, wr = d.sectors_written * 512ull;
    s.disk.ok = true;
    s.disk.name = root_disk_;
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

void Sampler::read_network(Snapshot &s, double dt)
{
    const std::int64_t now = src_->now_ms();
    if (!src_->read_into("/proc/net/dev", buf_) || !parse_net_dev_into(buf_, devs_)) devs_.clear();
    // the addresses (getifaddrs) and the default route every 5 s, or when the interfaces change
    bool changed = devs_.size() != prev_net_.size();
    for (const NetDev &d : devs_) changed = changed || !prev_net_.count(d.name);
    if (changed || due(next_addr_ms_, now, kMediumMs)) {
        addrs_ = src_->ipv4();
        route_ = src_->read_into("/proc/net/route", buf_) ? parse_default_route(buf_) : std::string();
    }
    std::vector<WirelessLevel> levels;
    if (src_->read_into("/proc/net/wireless", buf_)) levels = parse_net_wireless(buf_);
    const bool want_link = want_link_.load();
    if (!want_link) links_.clear();
    const bool ask_iw = want_link && !iw_absent_ && (links_.empty() || now >= next_link_ms_);
    if (ask_iw) next_link_ms_ = now + 5000;
    for (const NetDev &d : devs_) {
        if (d.name == "lo") continue;
        NetInfo n;
        n.name = d.name;
        path_.assign("/sys/class/net/").append(d.name);
        const std::size_t base = path_.size();
        path_.append("/operstate");
        n.state = src_->read_into(path_, buf_) ? trim(buf_) : std::string();
        if (n.state.empty()) n.state = "unknown";
        const auto known = wireless_.find(d.name);
        if (known != wireless_.end()) {
            n.wireless = known->second;
        } else { /* looked up once per interface */
            path_.resize(base);
            path_.append("/wireless");
            bool w = src_->exists(path_);
            path_.resize(base);
            path_.append("/phy80211");
            w = w || src_->exists(path_);
            wireless_[d.name] = w;
            n.wireless = w;
        }
        for (const WirelessLevel &w : levels)
            if (w.iface == d.name) {
                n.wireless = true;
                n.has_level = true;
                n.level_dbm = w.level_dbm;
            }
        const auto a = addrs_.find(d.name);
        if (a != addrs_.end()) n.ipv4 = a->second;
        n.rx_total = d.rx_bytes;
        n.tx_total = d.tx_bytes;
        const auto p = prev_net_.find(d.name);
        if (p != prev_net_.end() && dt > 0) {
            n.rx_bps = counter_rate(p->second.first, d.rx_bytes, dt);
            n.tx_bps = counter_rate(p->second.second, d.tx_bytes, dt);
        }
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
    if (changed) prev_net_.clear();
    for (const NetDev &d : devs_) prev_net_[d.name] = {d.rx_bytes, d.tx_bytes};
    for (const NetInfo &n : s.net)
        if (n.name == route_) s.main_iface = n.name;
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
    if (due(next_bt_ms_, src_->now_ms(), kMediumMs)) { /* sysfs directories: every 5 s */
        bool present = false, connected = false;
        for (const std::string &n : src_->list("/sys/class/bluetooth")) {
            if (!starts_with(n, "hci")) continue;
            present = true;
            if (n.find(':') != std::string::npos) connected = true;
        }
        bool blocked = false;
        if (present)
            for (const std::string &n : src_->list("/sys/class/rfkill")) {
                const std::string base = "/sys/class/rfkill/" + n;
                if (trim(src_->read(base + "/type")) != "bluetooth") continue;
                if (trim(src_->read(base + "/soft")) == "1" || trim(src_->read(base + "/hard")) == "1") blocked = true;
            }
        bt_ = !present ? BtState::Absent : blocked ? BtState::Off : connected ? BtState::Connected : BtState::On;
    }
    s.bt = bt_;
}

void Sampler::read_procs(Snapshot &s)
{
    const std::int64_t now = src_->now_ms();
    const bool want = want_procs_.load();
    if (!want) {
        procs_baseline_ = false;
        prev_proc_ticks_.clear();
        last_rows_valid_ = false;
        // the count for the overview: one directory read every 5 s
        if (due(next_count_ms_, now, kMediumMs)) {
            src_->list_into("/proc", names_);
            proc_count_ = static_cast<int>(std::count_if(names_.begin(), names_.end(), all_digits));
        }
        s.proc_count = proc_count_;
        return;
    }
    // a page shows processes: every /proc/<pid>/stat at most every 2 s (and a second scan soon after the first)
    const bool scan = !procs_baseline_ || now - last_scan_ms_ >= kProcScanMs ||
                      (procs_kick_pending_ && now - last_scan_ms_ >= 400);
    if (scan) {
        src_->list_into("/proc", names_);
        std::size_t n = 0;
        ProcStat st;
        for (const std::string &name : names_) {
            if (!all_digits(name)) continue;
            path_.assign("/proc/").append(name).append("/stat");
            if (!src_->read_into(path_, buf_) || !parse_proc_stat(buf_, st)) continue;
            if (n == samples_.size()) samples_.emplace_back();
            ProcSample &p = samples_[n++];
            p.pid = st.pid;
            p.name = st.comm;
            p.state = st.state;
            p.ticks = st.utime + st.stime;
            p.rss_bytes = static_cast<std::uint64_t>(st.rss_pages) * static_cast<std::uint64_t>(page_size_);
        }
        samples_.resize(n);
        proc_count_ = static_cast<int>(n);
        if (procs_baseline_) {
            const int cores = cur_cores_ > 0 ? cur_cores_ : 1;
            const double per_core = cur_total_ > prev_proc_total_
                                        ? static_cast<double>(cur_total_ - prev_proc_total_) / cores
                                        : 0.0;
            const std::vector<ProcRow> rows = proc_rows(prev_proc_ticks_, samples_, per_core);
            last_top_cpu_ = top_n(rows, SortBy::Cpu, 15);
            last_top_mem_ = top_n(rows, SortBy::Memory, 15);
            last_rows_valid_ = true;
            procs_kick_pending_ = false;
        } else {
            procs_kick_pending_ = true;
        }
        prev_proc_ticks_.clear();
        for (const ProcSample &p : samples_) prev_proc_ticks_[p.pid] = p.ticks;
        prev_proc_total_ = cur_total_;
        procs_baseline_ = true;
        last_scan_ms_ = now;
    }
    s.proc_count = proc_count_;
    s.procs_valid = last_rows_valid_;
    if (last_rows_valid_) {
        s.top_cpu = last_top_cpu_;
        s.top_mem = last_top_mem_;
    }
}

/* This app's own cost, from /proc/self: CPU of the last 10 s (its vcgencmd and iw children included) and RSS. */
void Sampler::read_self(Snapshot &s)
{
    char buf[1024];
    const std::int64_t now = real_ms();
    if (read_own("/proc/self/stat", buf, sizeof(buf))) {
        const std::string_view t(buf);
        const std::size_t close = t.rfind(')');
        if (close != std::string_view::npos) {
            Fields f(t.substr(close + 1));
            std::string_view v[15];
            int k = 0;
            while (k < 15 && f.next(v[k])) ++k;
            std::uint64_t ut = 0, st = 0, cut = 0, cst = 0;
            if (k == 15 && to_u64(v[11], ut) && to_u64(v[12], st) && to_u64(v[13], cut) && to_u64(v[14], cst)) {
                self_ms_[self_head_] = now;
                self_ticks_[self_head_] = ut + st + cut + cst;
                const int newest = self_head_;
                self_head_ = (self_head_ + 1) % kSelfWindow;
                if (self_count_ < kSelfWindow) ++self_count_;
                const int oldest = (newest - (self_count_ - 1) + kSelfWindow) % kSelfWindow;
                const std::int64_t span = self_ms_[newest] - self_ms_[oldest];
                if (self_count_ > 1 && span > 0) {
                    s.self_ok = true;
                    s.self_cpu = 100.0 * static_cast<double>(self_ticks_[newest] - self_ticks_[oldest]) /
                                 static_cast<double>(clk_tck_) / (static_cast<double>(span) / 1000.0);
                }
            }
        }
    }
    if (read_own("/proc/self/statm", buf, sizeof(buf))) {
        Fields f{std::string_view(buf)};
        std::string_view size, resident;
        std::uint64_t pages = 0;
        if (f.next(size) && f.next(resident) && to_u64(resident, pages))
            s.self_rss = pages * static_cast<std::uint64_t>(own_page_);
    }
}

} // namespace sm
