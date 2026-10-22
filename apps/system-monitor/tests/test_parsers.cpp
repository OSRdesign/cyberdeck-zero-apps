/*
 * SPDX-License-Identifier: MIT
 *
 * System Monitor tests: the parsers on fixture files of a Pi Zero 2 W, a Pi 3A+ and a Pi 5, and the number formats.
 */
#include "check.hpp"

#include "parsers.hpp"
#include "stats.hpp"
#include "text.hpp"

using namespace sm;

static void test_stat()
{
    const StatInfo a = parse_stat(fixture("pi-zero-2w", "proc/stat"));
    const StatInfo b = parse_stat(fixture("pi-zero-2w", "proc/stat-1s"));
    CHECK(a.ok && b.ok);
    CHECK_EQ(a.cores.size(), static_cast<std::size_t>(4));
    CHECK_EQ(a.core_ids[3], 3);
    CHECK_EQ(a.cores[0].user, static_cast<std::uint64_t>(10000));
    CHECK_EQ(a.cores[0].idle_all(), static_cast<std::uint64_t>(80500));
    // one second at 100 Hz: cores 30 / 10 / 55 / 5 % busy -> 25 % of the whole CPU
    CHECK_NEAR(busy_pct(a.cores[0], b.cores[0]), 30.0, 0.01);
    CHECK_NEAR(busy_pct(a.cores[1], b.cores[1]), 10.0, 0.01);
    CHECK_NEAR(busy_pct(a.cores[2], b.cores[2]), 55.0, 0.01);
    CHECK_NEAR(busy_pct(a.cores[3], b.cores[3]), 5.0, 0.01);
    CHECK_NEAR(busy_pct(a.all, b.all), 25.0, 0.01);
    CHECK_EQ(b.all.total() - a.all.total(), static_cast<std::uint64_t>(400));
    // no time passed, counters going back: no value
    CHECK(busy_pct(a.all, a.all) < 0);
    CHECK(busy_pct(b.all, a.all) < 0);
    // the Pi 3A+ under stress-ng: 100 / 100 / 98 / 100
    const StatInfo c = parse_stat(fixture("pi-3a-plus", "proc/stat"));
    const StatInfo d = parse_stat(fixture("pi-3a-plus", "proc/stat-1s"));
    CHECK_NEAR(busy_pct(c.all, d.all), 99.5, 0.01);
    CHECK_NEAR(busy_pct(c.cores[2], d.cores[2]), 98.0, 0.01);
    // an old kernel's short line, garbage, an offline core
    const StatInfo e = parse_stat("cpu  10 0 5 100\ncpu0 10 0 5 100\nnonsense\ncpu2 1 2 3 4 5 6 7 8\ncpux 1 2\n");
    CHECK(e.ok);
    CHECK_EQ(e.cores.size(), static_cast<std::size_t>(2));
    CHECK_EQ(e.core_ids[1], 2);
    CHECK(!parse_stat("").ok);
}

static void test_meminfo()
{
    const MemInfo m = parse_meminfo(fixture("pi-zero-2w", "proc/meminfo"));
    CHECK(m.ok && m.has_available);
    CHECK_EQ(m.total, static_cast<std::uint64_t>(427168));
    CHECK_EQ(m.used(), static_cast<std::uint64_t>(427168 - 226760));
    CHECK_EQ(m.cache(), static_cast<std::uint64_t>(142584 + 14876));
    CHECK_EQ(m.swap_used(), static_cast<std::uint64_t>(524284 - 498172));
    CHECK_EQ(m.shmem, static_cast<std::uint64_t>(6020));
    // an old kernel without MemAvailable: used = total - free - buffers - cache
    const MemInfo old = parse_meminfo("MemTotal: 1000 kB\nMemFree: 300 kB\nBuffers: 100 kB\nCached: 150 kB\r\n");
    CHECK(old.ok && !old.has_available);
    CHECK_EQ(old.used(), static_cast<std::uint64_t>(450));
    CHECK(!parse_meminfo("Bogus: 1\n").ok);
    const MemInfo p5 = parse_meminfo(fixture("pi-5", "proc/meminfo"));
    CHECK_EQ(p5.swap_used(), static_cast<std::uint64_t>(0));
}

static void test_load_uptime()
{
    const LoadAvg l = parse_loadavg(fixture("pi-zero-2w", "proc/loadavg"));
    CHECK(l.ok);
    CHECK_NEAR(l.l1, 0.84, 1e-9);
    CHECK_NEAR(l.l15, 0.48, 1e-9);
    CHECK_EQ(l.running, 2);
    CHECK_EQ(l.tasks, 181);
    CHECK(!parse_loadavg("0.1 0.2\n").ok);
    double up = 0;
    CHECK(parse_uptime(fixture("pi-zero-2w", "proc/uptime"), up));
    CHECK_NEAR(up, 93784.52, 1e-6);
    CHECK(!parse_uptime("", up));
}

static void test_network()
{
    const std::vector<NetDev> a = parse_net_dev(fixture("pi-zero-2w", "proc/net/dev"));
    const std::vector<NetDev> b = parse_net_dev(fixture("pi-zero-2w", "proc/net/dev-1s"));
    CHECK_EQ(a.size(), static_cast<std::size_t>(2));
    CHECK_EQ(a[1].name, std::string("wlan0"));
    CHECK_EQ(a[1].rx_bytes, static_cast<std::uint64_t>(98765432));
    CHECK_EQ(a[1].tx_packets, static_cast<std::uint64_t>(9876));
    CHECK_NEAR(counter_rate(a[1].rx_bytes, b[1].rx_bytes, 1.0), 12800.0, 1e-9);
    CHECK_NEAR(counter_rate(a[1].tx_bytes, b[1].tx_bytes, 1.0), 2048.0, 1e-9);
    // a name glued to its first number (big counters)
    const std::vector<NetDev> glued = parse_net_dev("eth0:12345678901 2 0 0 0 0 0 0 333 4 0 0 0 0 0 0\n");
    CHECK_EQ(glued.size(), static_cast<std::size_t>(1));
    CHECK_EQ(glued[0].rx_bytes, static_cast<std::uint64_t>(12345678901ull));
    CHECK_EQ(parse_default_route(fixture("pi-zero-2w", "proc/net/route")), std::string("wlan0"));
    CHECK_EQ(parse_default_route(fixture("pi-5", "proc/net/route")), std::string("eth0"));
    CHECK_EQ(parse_default_route("Iface\tDestination\tGateway\tFlags\tRefCnt\tUse\tMetric\tMask\n"), std::string());
    // two default routes: the lower metric wins
    CHECK_EQ(parse_default_route("wlan0\t00000000\t0101A8C0\t0003\t0\t0\t600\t00000000\t0\t0\t0\n"
                                 "eth0\t00000000\t0101A8C0\t0003\t0\t0\t100\t00000000\t0\t0\t0\n"),
             std::string("eth0"));
    const std::vector<WirelessLevel> w = parse_net_wireless(fixture("pi-zero-2w", "proc/net/wireless"));
    CHECK_EQ(w.size(), static_cast<std::size_t>(1));
    CHECK_EQ(w[0].iface, std::string("wlan0"));
    CHECK_NEAR(w[0].level_dbm, -52.0, 1e-9);
    CHECK_NEAR(w[0].quality, 58.0, 1e-9);
    CHECK(parse_net_wireless(fixture("pi-5", "proc/net/wireless")).empty());
    const IwLink link = parse_iw_link(fixture("pi-zero-2w", "cmd/iw_dev_wlan0_link"));
    CHECK(link.connected);
    CHECK_EQ(link.ssid, std::string("DeckNet"));
    CHECK(link.has_signal && link.signal_dbm == -52);
    CHECK(link.has_bitrate);
    CHECK_NEAR(link.tx_mbit, 72.2, 1e-9);
    CHECK_EQ(link.freq_mhz, 2437);
    CHECK(!parse_iw_link(fixture("pi-5", "cmd/iw_dev_wlan0_link")).connected);
}

static void test_disks()
{
    const std::vector<DiskStat> d = parse_diskstats(fixture("pi-zero-2w", "proc/diskstats"));
    CHECK_EQ(d.size(), static_cast<std::size_t>(4));
    CHECK_EQ(d[1].name, std::string("mmcblk0"));
    CHECK_EQ(d[3].major, 179u);
    CHECK_EQ(d[3].minor, 2u);
    CHECK_EQ(d[1].sectors_read, static_cast<std::uint64_t>(2400000));
    CHECK_EQ(d[1].sectors_written, static_cast<std::uint64_t>(960000));
    const std::vector<DiskStat> e = parse_diskstats(fixture("pi-zero-2w", "proc/diskstats-1s"));
    CHECK_NEAR(counter_rate(d[1].sectors_read * 512, e[1].sectors_read * 512, 1.0), 409600.0, 1e-9);
    CHECK_EQ(disk_of_partition("mmcblk0p2"), std::string("mmcblk0"));
    CHECK_EQ(disk_of_partition("mmcblk0"), std::string("mmcblk0"));
    CHECK_EQ(disk_of_partition("nvme0n1p2"), std::string("nvme0n1"));
    CHECK_EQ(disk_of_partition("nvme0n1"), std::string("nvme0n1"));
    CHECK_EQ(disk_of_partition("sda1"), std::string("sda"));
    CHECK_EQ(disk_of_partition("sdb"), std::string("sdb"));
    CHECK_EQ(disk_of_partition("dm-0"), std::string("dm-0"));

    const std::vector<Mount> all = parse_mounts(fixture("pi-3a-plus", "proc/self/mounts"));
    CHECK(all.size() > 10);
    const std::vector<Mount> real = real_mounts(all);
    CHECK_EQ(real.size(), static_cast<std::size_t>(3)); /* /, /boot/firmware, the USB stick; the bind mount left out */
    CHECK_EQ(real[0].dir, std::string("/"));
    CHECK_EQ(real[1].dir, std::string("/boot/firmware"));
    CHECK_EQ(real[2].dir, std::string("/media/pi/USB STICK")); /* \040 decoded */
    CHECK_EQ(real[2].type, std::string("vfat"));
    const std::vector<Mount> zero = real_mounts(parse_mounts(fixture("pi-zero-2w", "proc/self/mounts")));
    CHECK_EQ(zero.size(), static_cast<std::size_t>(2));
    CHECK(real_mounts(parse_mounts("/dev/loop0 /snap/x squashfs ro 0 0\n/dev/zram0 /var/log ext4 rw 0 0\n")).empty());
    CHECK(parse_mounts("/dev/sda1 /mnt ext4 ro,noatime 0 0\n")[0].read_only);
}

static void test_sysfs()
{
    double c = 0;
    CHECK(parse_millicelsius(fixture("pi-zero-2w", "sys/class/thermal/thermal_zone0/temp"), c));
    CHECK_NEAR(c, 48.312, 1e-9);
    CHECK(!parse_millicelsius("hello", c));
    CHECK(!parse_millicelsius("900000", c)); /* not a temperature */
    int mhz = 0;
    CHECK(parse_khz_as_mhz(fixture("pi-3a-plus", "sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq"), mhz));
    CHECK_EQ(mhz, 600);
    CHECK(parse_khz_as_mhz("1499999\n", mhz) && mhz == 1500);
    CHECK(!parse_khz_as_mhz("0", mhz));
    CHECK_EQ(parse_cpu_count("0-3\n"), 4);
    CHECK_EQ(parse_cpu_count("0,2-3"), 3);
    CHECK_EQ(parse_cpu_count("0"), 1);
    CHECK_EQ(parse_cpu_count(""), 0);
}

static void test_firmware()
{
    std::uint32_t bits = 1;
    CHECK(parse_throttled(fixture("pi-3a-plus", "cmd/vcgencmd_get_throttled"), bits));
    CHECK_EQ(bits, 0x50005u);
    CHECK(parse_throttled("throttled=0x0\n", bits) && bits == 0);
    CHECK(parse_throttled("50005\n", bits) && bits == 0x50005u); /* the firmware's sysfs node */
    CHECK(parse_throttled("0x80008", bits) && bits == 0x80008u);
    CHECK(!parse_throttled("error=1 error_msg=\"Command not registered\"", bits));
    CHECK(!parse_throttled("", bits));
    double v = 0;
    CHECK(parse_volts(fixture("pi-zero-2w", "cmd/vcgencmd_measure_volts_core"), v));
    CHECK_NEAR(v, 1.2, 1e-9);
    CHECK(parse_volts(fixture("pi-5", "cmd/vcgencmd_pmic_read_adc_EXT5V_V"), v));
    CHECK_NEAR(v, 5.134712, 1e-6);
    CHECK(!parse_volts("volt=", v));
    CHECK_EQ(parse_firmware_version(fixture("pi-zero-2w", "cmd/vcgencmd_version")), std::string("2025-03-17 (2ab6c7e)"));
    CHECK_EQ(parse_firmware_version(fixture("pi-5", "cmd/vcgencmd_version")), std::string("2025-03-10 (2bb2ae6)"));
    CHECK_EQ(parse_firmware_version("something else\n"), std::string("something else"));
    CHECK_EQ(parse_firmware_version(""), std::string());
    CHECK(vcgencmd_access_denied("Can't open device file: /dev/vcio\nTry creating a device file with: sudo mknod ..."));
    CHECK(vcgencmd_access_denied("* failed to open vchiq instance"));
    CHECK(!vcgencmd_access_denied("throttled=0x0"));
}

static void test_procs()
{
    ProcStat p;
    CHECK(parse_proc_stat("1203 (chromium (gpu)) R 1 1203 1203 0 -1 4194560 9000 0 20 0 3000 400 0 0 20 0 9 0 9000 "
                          "900000000 21000",
                          p));
    CHECK_EQ(p.pid, 1203);
    CHECK_EQ(p.comm, std::string("chromium (gpu)"));
    CHECK_EQ(p.state, 'R');
    CHECK_EQ(p.utime, static_cast<std::uint64_t>(3000));
    CHECK_EQ(p.stime, static_cast<std::uint64_t>(400));
    CHECK_EQ(p.rss_pages, static_cast<std::int64_t>(21000));
    CHECK(parse_proc_stat("42 (a b) S 1 1 1 0 -1 0 0 0 0 0 7 8 0 0 20 0 1 0 1 1 5\n", p));
    CHECK_EQ(p.comm, std::string("a b"));
    CHECK(!parse_proc_stat("42 (short) S 1 2 3", p));
    CHECK(!parse_proc_stat("garbage", p));
    CHECK_EQ(std::string(proc_state_name('S')), std::string("sleeping"));
    CHECK_EQ(std::string(proc_state_name('D')), std::string("waiting for I/O"));
}

static void test_facts()
{
    const std::string zero = parse_model(fixture("pi-zero-2w", "proc/device-tree/model"));
    CHECK_EQ(zero, std::string("Raspberry Pi Zero 2 W Rev 1.0"));
    CHECK_EQ(model_without_rev(zero), std::string("Raspberry Pi Zero 2 W"));
    CHECK_EQ(short_model(zero), std::string("Pi Zero 2 W"));
    CHECK_EQ(short_model(parse_model(fixture("pi-3a-plus", "proc/device-tree/model"))), std::string("Pi 3 Model A Plus"));
    CHECK_EQ(short_model("Some Board"), std::string("Some Board"));
    CHECK_EQ(os_pretty_name(fixture("pi-5", "etc/os-release")), std::string("Debian GNU/Linux 13 (trixie)"));
    CHECK_EQ(os_pretty_name("PRETTY_NAME=Plain\n"), std::string("Plain"));
    CHECK_EQ(serial_tail(fixture("pi-zero-2w", "proc/device-tree/serial-number"), ""), std::string("a1b2c3d4"));
    CHECK_EQ(serial_tail("", fixture("pi-3a-plus", "proc/cpuinfo")), std::string("e5f6a7b8"));
    CHECK_EQ(cpu_description(fixture("pi-zero-2w", "proc/cpuinfo"), 4), std::string("4 x Cortex-A53"));
    CHECK_EQ(cpu_description(fixture("pi-5", "proc/cpuinfo"), 0), std::string("4 x Cortex-A76"));
    CHECK_EQ(cpu_description("processor : 0\n", 0), std::string("1 core"));
    // x86: the model name, cleaned
    CHECK_EQ(cpu_description(fixture("x86-fz-m1", "proc/cpuinfo"), 4), std::string("4 x Intel Core m5-6Y57"));
    CHECK_EQ(cpu_description("processor : 0\nmodel name : 11th Gen Intel(R) Core(TM) i5-1135G7 @ 2.40GHz\n", 8),
             std::string("8 x 11th Gen Intel Core i5-1135G7"));
    CHECK_EQ(cpu_description("processor : 0\nmodel name : AMD Ryzen 5 5600U with Radeon Graphics\n", 0),
             std::string("1 x AMD Ryzen 5 5600U with Radeon Graphics"));
    // the model of a PC from DMI
    CHECK(is_raspberry_model("Raspberry Pi 5 Model B Rev 1.0") && is_raspberry_model("Raspberry Pi Compute Module 5"));
    CHECK(!is_raspberry_model("") && !is_raspberry_model("Some Board"));
    CHECK_EQ(dmi_model("Panasonic Corporation", "FZ-M1CDEAAVJ"), std::string("Panasonic FZ-M1CDEAAVJ"));
    CHECK_EQ(dmi_model("Dell Inc.", "XPS 13 9310"), std::string("Dell XPS 13 9310"));
    CHECK_EQ(dmi_model("Dell Inc.", "Dell Pro 14"), std::string("Dell Pro 14")); /* the product has it */
    CHECK_EQ(dmi_model("ASUSTeK COMPUTER INC.", "UX305"), std::string("ASUSTeK UX305"));
    CHECK_EQ(dmi_model("LENOVO", "20XW0055GE\n"), std::string("LENOVO 20XW0055GE"));
    CHECK_EQ(dmi_model("To Be Filled By O.E.M.", "To Be Filled By O.E.M."), std::string());
    CHECK_EQ(dmi_model("System manufacturer", "System Product Name"), std::string());
    CHECK_EQ(dmi_model("Default string", "MiniPC N100"), std::string("MiniPC N100"));
    CHECK_EQ(dmi_model("Micro-Star International Co., Ltd.", "MS-7C94"), std::string("MS-7C94")); /* long vendor */
    CHECK_EQ(dmi_model("QEMU", ""), std::string("QEMU"));
    CHECK_EQ(dmi_model("", ""), std::string());
}

void test_parsers()
{
    test_stat();
    test_meminfo();
    test_load_uptime();
    test_network();
    test_disks();
    test_sysfs();
    test_firmware();
    test_procs();
    test_facts();
}

void test_format()
{
    CHECK_EQ(fmt_bytes(0), std::string("0 B"));
    CHECK_EQ(fmt_bytes(512), std::string("512 B"));
    CHECK_EQ(fmt_bytes(10035), std::string("9.8 kB"));
    CHECK_EQ(fmt_kb(200408), std::string("196 MB"));
    CHECK_EQ(fmt_kb(427168), std::string("417 MB"));
    CHECK_EQ(fmt_bytes(1288490188ull), std::string("1.2 GB"));
    CHECK_EQ(fmt_bytes(31104831488ull), std::string("29 GB"));
    CHECK_EQ(fmt_used_of(200408ull * 1024, 427168ull * 1024), std::string("196 / 417 MB"));
    CHECK_EQ(fmt_used_of(5634138112ull, 31104831488ull), std::string("5.2 / 29 GB"));
    CHECK_EQ(fmt_used_of(0, 104857600ull), std::string("0 / 100 MB"));
    CHECK_EQ(fmt_rate(0), std::string("0 B/s"));
    CHECK_EQ(fmt_rate(12800), std::string("12 kB/s"));
    CHECK_EQ(fmt_rate(2048), std::string("2.0 kB/s"));
    CHECK_EQ(fmt_rate(409600), std::string("400 kB/s"));
    CHECK_EQ(fmt_rate(1.5 * 1048576), std::string("1.5 MB/s"));
    CHECK_EQ(fmt_pct(24.6), std::string("25%"));
    CHECK_EQ(fmt_pct(-3), std::string("0%"));
    CHECK_EQ(fmt_temp(48.312), std::string("48.3 \xC2\xB0" "C"));
    CHECK_EQ(fmt_temp_short(82.6), std::string("83 \xC2\xB0" "C"));
    CHECK_EQ(fmt_mhz(1400), std::string("1400 MHz"));
    CHECK_EQ(fmt_volts(5.134712), std::string("5.13 V"));
    CHECK_EQ(fmt_uptime(45), std::string("45 s"));
    CHECK_EQ(fmt_uptime(720), std::string("12 min"));
    CHECK_EQ(fmt_uptime(2 * 3600 + 5 * 60), std::string("2 h 05 min"));
    CHECK_EQ(fmt_uptime(93784.52), std::string("1 d 2 h"));
    CHECK_EQ(fmt_since_boot(2472), std::string("0:41:12"));
    CHECK_EQ(fmt_since_boot(86400 + 2472), std::string("1 d 00:41:12"));
    CHECK_EQ(fmt_window(120, 1), std::string("2 min"));
    CHECK_EQ(fmt_window(120, 5), std::string("10 min"));
    CHECK_EQ(fmt_window(90, 1), std::string("90 s"));
    std::uint64_t u = 0;
    CHECK(parse_u64(" 123 ", u) && u == 123);
    CHECK(!parse_u64("12a", u));
    CHECK(!parse_u64("99999999999999999999999", u));
    double d = 0;
    CHECK(parse_double("-52.", d) && d == -52.0);
    std::uint32_t h = 0;
    CHECK(parse_hex("0x1F", h) && h == 31);
    CHECK(!parse_hex("0x123456789", h));
    CHECK_EQ(split_lines("a\r\nb\n\nc").size(), static_cast<std::size_t>(4));
}
