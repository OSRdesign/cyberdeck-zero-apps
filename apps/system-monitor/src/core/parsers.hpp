/*
 * SPDX-License-Identifier: MIT
 *
 * System Monitor: parsers of /proc, /sys and command output (pure functions on text, no LVGL, no system call;
 * tests/test_parsers.cpp runs them on fixture files of a Pi Zero 2 W, a Pi 3A+ and a Pi 5).
 *
 * Every parser takes the whole file text, tolerates "\r", a missing trailing newline and unknown lines, and reports
 * "not found" instead of throwing.
 */
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace sm {

/* ---------------------------------------------------------------- /proc/stat */

struct CpuTimes {
    std::uint64_t user = 0, nice = 0, system = 0, idle = 0, iowait = 0, irq = 0, softirq = 0, steal = 0;
    std::uint64_t total() const { return user + nice + system + idle + iowait + irq + softirq + steal; }
    std::uint64_t idle_all() const { return idle + iowait; }
    std::uint64_t busy() const { return total() - idle_all(); }
};

struct StatInfo {
    bool ok = false;
    CpuTimes all;                /* the "cpu" line */
    std::vector<CpuTimes> cores; /* "cpu0", "cpu1"... in order (an offline core's line is missing) */
    std::vector<int> core_ids;   /* the N of each "cpuN" line */
};

StatInfo parse_stat(const std::string &text);
/* Busy share of the time between two samples, 0..100; -1 when no time passed (or the counters went back). Guest time
 * is part of user time in /proc/stat and is not counted twice. */
double busy_pct(const CpuTimes &before, const CpuTimes &after);

/* ---------------------------------------------------------------- /proc/meminfo (kB) */

struct MemInfo {
    bool ok = false;
    std::uint64_t total = 0, free = 0, available = 0, buffers = 0, cached = 0, sreclaimable = 0, shmem = 0;
    std::uint64_t swap_total = 0, swap_free = 0;
    bool has_available = false;
    /* as `free` (procps-ng 4): used = total - available (total - free - buffers - cache without MemAvailable) */
    std::uint64_t used() const;
    std::uint64_t cache() const { return cached + sreclaimable; } /* `free`'s "cache" column */
    std::uint64_t swap_used() const { return swap_total > swap_free ? swap_total - swap_free : 0; }
};

MemInfo parse_meminfo(const std::string &text);

/* ---------------------------------------------------------------- /proc/loadavg, /proc/uptime */

struct LoadAvg {
    bool ok = false;
    double l1 = 0, l5 = 0, l15 = 0;
    int running = 0, tasks = 0; /* "2/143" */
};

LoadAvg parse_loadavg(const std::string &text);
bool parse_uptime(const std::string &text, double &uptime_s);

/* ---------------------------------------------------------------- /proc/net/dev, /proc/net/route, wireless */

struct NetDev {
    std::string name;
    std::uint64_t rx_bytes = 0, rx_packets = 0, tx_bytes = 0, tx_packets = 0;
};

std::vector<NetDev> parse_net_dev(const std::string &text);
/* The interface of the default route (destination 00000000, the lowest metric), "" when none. */
std::string parse_default_route(const std::string &text);

struct WirelessLevel {
    std::string iface;
    double quality = 0;  /* link quality, driver units (mostly /70) */
    double level_dbm = 0;
};

/* /proc/net/wireless: one entry per wireless interface (level in dBm; a positive level is taken as a relative 0..100
 * value and dropped) */
std::vector<WirelessLevel> parse_net_wireless(const std::string &text);

struct IwLink {
    bool connected = false;
    std::string ssid;
    bool has_signal = false;
    int signal_dbm = 0;
    bool has_bitrate = false;
    double tx_mbit = 0;
    int freq_mhz = 0;
};

/* `iw dev wlan0 link` (LC_ALL=C) */
IwLink parse_iw_link(const std::string &text);

/* ---------------------------------------------------------------- disks and mounts */

struct DiskStat {
    unsigned major = 0, minor = 0;
    std::string name;
    std::uint64_t reads = 0, sectors_read = 0, writes = 0, sectors_written = 0; /* sectors of 512 bytes */
};

std::vector<DiskStat> parse_diskstats(const std::string &text);
/* The disk of a partition name: mmcblk0p2 -> mmcblk0, nvme0n1p2 -> nvme0n1, sda2 -> sda; a disk stays itself. */
std::string disk_of_partition(const std::string &name);

struct Mount {
    std::string device, dir, type;
    bool read_only = false;
};

std::vector<Mount> parse_mounts(const std::string &text); /* \040 and the other octal escapes decoded */
/* The filesystems worth showing: on a block device (/dev/...), not a loop / squashfs / ram device, one entry per device
 * (the shortest mount point: bind mounts of the same device are left out), "/" first. Network filesystems are never
 * in it (statvfs on a dead server would hang). */
std::vector<Mount> real_mounts(const std::vector<Mount> &mounts);

/* ---------------------------------------------------------------- sysfs values */

bool parse_millicelsius(const std::string &text, double &celsius); /* thermal_zone<N>/temp: "54768" */
bool parse_khz_as_mhz(const std::string &text, int &mhz);          /* scaling_cur_freq: "1000000" */
int parse_cpu_count(const std::string &list);                      /* /sys/devices/system/cpu/online "0-3" -> 4 */

/* ---------------------------------------------------------------- firmware (vcgencmd and friends) */

/* "throttled=0x50005" (vcgencmd get_throttled), "50005" (the firmware's sysfs node) */
bool parse_throttled(const std::string &text, std::uint32_t &bits);
/* "volt=0.8563V" (measure_volts), "EXT5V_V volt(24)=5.13471200V" (pmic_read_adc on a Pi 5) */
bool parse_volts(const std::string &text, double &volts);
/* `vcgencmd version`: "Mar 17 2025 10:50:39 / version 2ab6...(clean)" -> "2025-03-17 (2ab6c75)"; the first line when
 * the format is unknown; "" for an empty text */
std::string parse_firmware_version(const std::string &text);
/* vcgencmd failed because the user may not open /dev/vcio or /dev/vchiq (not in the video group) */
bool vcgencmd_access_denied(const std::string &output);

/* ---------------------------------------------------------------- processes */

struct ProcStat {
    int pid = 0;
    std::string comm;   /* the name in parentheses (spaces and parentheses allowed) */
    char state = '?';
    std::uint64_t utime = 0, stime = 0; /* clock ticks */
    std::int64_t rss_pages = 0;
};

bool parse_proc_stat(const std::string &text, ProcStat &out);
/* "S" -> "sleeping" etc. */
const char *proc_state_name(char state);

/* ---------------------------------------------------------------- system facts */

std::string parse_model(const std::string &text);           /* /proc/device-tree/model: NUL and newline removed */
std::string short_model(const std::string &model);          /* "Raspberry Pi Zero 2 W Rev 1.0" -> "Pi Zero 2 W" */
std::string model_without_rev(const std::string &model);    /* -> "Raspberry Pi Zero 2 W" */
std::string os_pretty_name(const std::string &os_release);  /* PRETTY_NAME of /etc/os-release, quotes removed */
/* the board's serial number, last 8 hex digits: device-tree serial-number, else the cpuinfo "Serial" line */
std::string serial_tail(const std::string &dt_serial, const std::string &cpuinfo);
/* "4 x Cortex-A53" from the "CPU part" lines of /proc/cpuinfo ("4 cores" when the part is unknown) */
std::string cpu_description(const std::string &cpuinfo, int cores);

} // namespace sm
