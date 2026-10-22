/*
 * SPDX-License-Identifier: MIT
 *
 * System Monitor: the sampler. A thread of its own reads the source once per period (1, 2 or 5 s), computes the
 * numbers and publishes a Snapshot (shared, immutable); the screen polls seq() and never waits for it.
 *
 * Kept cheap (the app's own cost is shown on the System page): every file is read into one reused buffer and parsed
 * in place (no allocation per sample once warm).
 *   each sample      /proc/stat, meminfo, loadavg, uptime, net/dev, net/wireless, diskstats, the CPU temperature and
 *                    frequency, the fan, the rpi_volt alarm, the battery (when there is one), statvfs of the real
 *                    filesystems, /proc/self
 *   every 5 s        the interfaces' addresses (getifaddrs) and the default route (at once when an interface comes
 *                    or goes), Bluetooth in sysfs, the number of processes (one directory read)
 *   every 10 s       the mount list, the maximum frequency and the governor
 *   on demand        the processes (/proc/<pid>/stat of each) at most every 2 s while a page that shows them is open;
 *                    `iw dev <wlan> link` every 5 s while the Network page is open
 * The firmware flags: the firmware's sysfs node when it exists (a file read), else `vcgencmd get_throttled` at most
 * every 2 s (1 s timeout; its answer is kept between the calls); the 5 V input every 10 s (`vcgencmd pmic_read_adc
 * EXT5V_V`, Pi 5 / CM5), the core voltage every 30 s. A missing vcgencmd is not retried (silently); a refused one (not
 * in the video group) says so once.
 */
#pragma once

#include "parsers.hpp"
#include "power.hpp"
#include "source.hpp"
#include "stats.hpp"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace sm {

constexpr std::size_t kHistory = 120; /* samples in the charts: 2 min at 1 s */

struct CpuInfo {
    bool ok = false;           /* a total percentage is known (needs two samples) */
    double total = 0;
    std::vector<double> cores; /* per core, % */
    int cur_mhz = 0, max_mhz = 0;
    std::string governor;
};

struct TempInfo {
    bool ok = false;
    double c = 0;
};

struct FanInfo {
    bool present = false;
    int state = -1, max_state = -1; /* cooling device */
    int rpm = -1;                   /* hwmon fan1_input */
};

struct FsInfo {
    std::string dir, device, type;
    std::uint64_t total = 0, used = 0, avail = 0;
    double pct() const { return total ? 100.0 * static_cast<double>(used) / static_cast<double>(used + avail) : 0; }
};

struct DiskInfo {
    bool ok = false;
    std::string name;
    double read_bps = 0, write_bps = 0;
    std::uint64_t read_total = 0, written_total = 0; /* bytes since boot */
};

struct NetInfo {
    std::string name;
    std::string state;       /* operstate: "up", "down", "dormant", "unknown" */
    bool wireless = false;
    std::string ipv4;        /* "192.168.1.23/24", "" */
    double rx_bps = 0, tx_bps = 0;
    std::uint64_t rx_total = 0, tx_total = 0;
    bool has_level = false;
    double level_dbm = 0;    /* /proc/net/wireless */
    bool link_valid = false; /* iw dev <if> link was read (the Network page) */
    IwLink link;
};

enum class BtState { Absent, Off, On, Connected };

/* The first battery of /sys/class/power_supply (type Battery, not a device's own such as a mouse): laptops and tablets. */
struct BatteryInfo {
    bool present = false;
    int percent = -1;          /* 0-100, -1 unknown */
    std::string status;        /* "Charging", "Discharging", "Full", "Not charging", "" */
    bool has_power = false;
    double power_w = 0;        /* what flows in or out, watts (always positive) */
    int minutes = -1;          /* to empty (discharging) or to full (charging), -1 unknown */
    bool charging() const { return status == "Charging"; }
    bool discharging() const { return status == "Discharging"; }
};

struct SystemFacts {
    std::string model, kernel, os, hostname, serial, firmware, cpu;
    std::uint64_t ram_kb = 0;
    int cores = 0;
    bool pi = true; /* a Raspberry Pi (its device-tree model says so): vcgencmd, firmware flags, "the Pi slows down" */
};

struct History {
    Ring cpu{kHistory}, mem{kHistory}, swap{kHistory}, temp{kHistory};
    Ring rx{kHistory}, tx{kHistory}, rd{kHistory}, wr{kHistory};
};

struct Snapshot {
    std::uint64_t seq = 0;
    int period_s = 1;
    bool uptime_ok = false;
    double uptime_s = 0;
    CpuInfo cpu;
    LoadAvg load;
    MemInfo mem;
    TempInfo temp;
    PowerInfo power;
    BatteryInfo battery;
    FanInfo fan;
    std::vector<FsInfo> fs;
    DiskInfo disk;
    std::vector<NetInfo> net;   /* lo left out */
    std::string main_iface;     /* the default route's, else the first one up with an address */
    BtState bt = BtState::Absent;
    bool procs_valid = false;   /* the process table is there (its page is open and two scans were made) */
    int proc_count = 0;
    std::vector<ProcRow> top_cpu, top_mem; /* 15 each */
    SystemFacts sys;
    History hist;
    bool self_ok = false;       /* this app's own cost (/proc/self): CPU % of one core over the last 10 s ... */
    double self_cpu = 0;        /* (its vcgencmd and iw children included) */
    std::uint64_t self_rss = 0; /* ... and its resident memory, bytes */
};

class Sampler {
public:
    /* Reads the static facts at once (small files: the model, the OS, the cores); the thread starts with start(). */
    Sampler(std::unique_ptr<Source> source, int period_s);
    ~Sampler(); /* stops and joins the thread */
    Sampler(const Sampler &) = delete;
    Sampler &operator=(const Sampler &) = delete;

    void start();
    void stop();

    /* 1, 2 or 5 s; the history is cleared (its samples would no longer be evenly spaced) and a sample follows. */
    void set_period(int period_s);
    int period() const { return period_.load(); }
    /* the processes are read while wanted (the first table follows within half a second, then every 2 s) */
    void set_want_procs(bool want);
    /* `iw` is asked while wanted (the Network page) */
    void set_want_link(bool want);

    std::uint64_t seq() const { return seq_.load(); }
    std::shared_ptr<const Snapshot> latest() const;
    const SystemFacts &facts() const { return facts_; } /* fixed after the constructor (the firmware comes later) */
    std::string describe() const { return source_desc_; }

    /* Tests: one sample now, on the calling thread (no thread running), published. */
    void step();

    static constexpr int kProcScanMs = 2000;   /* the process table at most this often */
    static constexpr int kThrottleMs = 2000;   /* vcgencmd get_throttled at most this often */
    static constexpr int kMediumMs = 5000;     /* addresses, route, Bluetooth, process count */
    static constexpr int kSlowMs = 10000;      /* mounts, maximum frequency, governor */
    static constexpr int kSupplyMs = 10000;    /* the 5 V input (Pi 5) */
    static constexpr int kCoreVoltsMs = 30000; /* the core voltage */

private:
    void loop();
    void wake();
    void sample(bool publish);
    void read_cpu(Snapshot &s);
    void read_memory(Snapshot &s);
    void read_thermal(Snapshot &s);
    void read_power(Snapshot &s);
    void read_battery(Snapshot &s);
    void find_battery();
    void find_thermal_pc();
    void read_storage(Snapshot &s, double dt);
    void read_network(Snapshot &s, double dt);
    void read_bluetooth(Snapshot &s);
    void read_procs(Snapshot &s);
    void read_self(Snapshot &s);
    void discover();
    CmdResult vcgencmd(const std::vector<std::string> &args);
    /* true when `now` reached next_ms (which then moves every_ms on) */
    static bool due(std::int64_t &next_ms, std::int64_t now, int every_ms);

    std::unique_ptr<Source> src_;
    std::string source_desc_;
    SystemFacts facts_;
    std::atomic<int> period_{1};
    std::atomic<bool> want_procs_{false};
    std::atomic<bool> want_link_{false};
    std::atomic<bool> stop_{false};
    std::atomic<bool> period_changed_{false};
    std::atomic<bool> procs_kick_{false};
    std::atomic<std::uint64_t> seq_{0};

    mutable std::mutex mu_;
    std::condition_variable cv_;
    bool wake_ = false;
    std::shared_ptr<const Snapshot> latest_;
    std::thread thread_;

    /* the sampler thread's state; buffers reused from sample to sample */
    std::string buf_, path_;
    std::vector<std::string> names_;
    std::vector<NetDev> devs_;
    std::vector<ProcSample> samples_;
    History hist_;
    bool have_prev_ = false;
    std::int64_t prev_ms_ = 0;
    StatInfo prev_stat_, cur_stat_;
    std::map<std::string, std::pair<std::uint64_t, std::uint64_t>> prev_net_;
    std::map<std::string, std::string> addrs_;
    std::map<std::string, bool> wireless_;
    std::string route_;
    bool have_disk_ = false;
    std::uint64_t prev_rd_ = 0, prev_wr_ = 0;
    std::vector<FsInfo> fs_;
    std::map<int, std::uint64_t> prev_proc_ticks_;
    std::uint64_t prev_proc_total_ = 0;
    std::uint64_t cur_total_ = 0;  /* /proc/stat ticks of all cores at this sample */
    int cur_cores_ = 0;
    int cur_mhz_ = 0, max_mhz_ = 0;
    std::string governor_;
    std::string firmware_;         /* vcgencmd version, asked once */
    bool version_asked_ = false;
    bool procs_baseline_ = false, procs_kick_pending_ = false, last_rows_valid_ = false;
    std::int64_t last_scan_ms_ = 0;
    std::vector<ProcRow> last_top_cpu_, last_top_mem_;
    int proc_count_ = 0;
    BtState bt_ = BtState::Absent;
    ThrottleTracker tracker_;
    std::map<std::string, IwLink> links_;
    std::int64_t next_link_ms_ = 0, next_cpufreq_ms_ = 0, next_mounts_ms_ = 0, next_addr_ms_ = 0, next_bt_ms_ = 0,
                 next_count_ms_ = 0;
    static constexpr int kSelfWindow = 11; /* /proc/self readings: CPU over the last 10 */
    std::int64_t self_ms_[kSelfWindow] = {};
    std::uint64_t self_ticks_[kSelfWindow] = {};
    int self_head_ = 0, self_count_ = 0;
    long clk_tck_ = 100, own_page_ = 4096;

    /* found once */
    std::string temp_path_;
    std::string throttle_node_;    /* the firmware's get_throttled sysfs node, "" */
    std::string uv_alarm_path_;    /* rpi_volt in0_lcrit_alarm, "" */
    std::string fan_dev_;          /* thermal cooling device of a fan, "" */
    std::string fan_rpm_path_;     /* hwmon fan1_input, "" */
    std::string bat_dir_;          /* /sys/class/power_supply/<battery>, "" */
    bool bat_failed_ = false;      /* the last read of it failed: look again at the next slow tick */
    std::int64_t next_bat_scan_ms_ = 0;
    std::string root_disk_;        /* diskstats name of the root disk */
    bool pi5_ = false;
    bool pi_ = true;               /* a Raspberry Pi: the firmware tools are asked only then */
    enum class Vc { Unknown, Works, Absent, Denied, Failing } vc_ = Vc::Unknown;
    int vc_failures_ = 0;
    bool iw_absent_ = false;
    std::int64_t next_throttle_ms_ = 0, next_supply_ms_ = 0, next_core_volts_ms_ = 0;
    PowerInfo power_;              /* the last values (kept between the polls) */
    long page_size_ = 4096;
};

} // namespace sm
