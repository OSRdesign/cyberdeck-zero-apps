/*
 * SPDX-License-Identifier: MIT
 *
 * System Monitor tests: the sampler end to end on the fixture folders (the parsers, the rates, the power sources, the
 * process table), with a missing and a refused vcgencmd, and a scenario on the sampler's own thread.
 */
#include "check.hpp"

#include "monitor_core.hpp"
#include "sampler.hpp"
#include "source.hpp"
#include "text.hpp"

#include <chrono>
#include <thread>

using namespace sm;

namespace {

/* A fixture folder whose counters move once: after flip(), a file X is read from X-1s when that exists. */
class Flipping : public Source {
public:
    explicit Flipping(const std::string &board) : dir_(fixture_dir(board.c_str())), inner_(make_fixture_source(dir_, "")) {}
    void flip() { flipped_ = true; }
    std::string read(const std::string &path, bool *ok) override
    {
        if (flipped_) {
            bool there = false;
            const std::string text = inner_->read(path + "-1s", &there);
            if (there) {
                if (ok) *ok = true;
                return text;
            }
        }
        return inner_->read(path, ok);
    }
    bool exists(const std::string &path) override { return hide(path) ? false : inner_->exists(path); }
    std::vector<std::string> list(const std::string &dir) override
    {
        return hide(dir) ? std::vector<std::string>() : inner_->list(dir);
    }
    CmdResult run(const std::vector<std::string> &argv, int timeout_ms) override
    {
        if (!argv.empty() && argv[0] == "vcgencmd") {
            ++vc_calls;
            if (vc_mode == 1) return CmdResult(); /* not installed */
            if (vc_mode == 2) {               /* installed, refused */
                CmdResult r;
                r.started = true;
                r.exit_code = 255;
                r.output = "Can't open device file: /dev/vcio\nTry creating a device file with: sudo mknod /dev/vcio c 100 0\n";
                return r;
            }
        }
        return inner_->run(argv, timeout_ms);
    }
    bool fs_usage(const std::string &d, std::uint64_t &t, std::uint64_t &u, std::uint64_t &a) override
    {
        return inner_->fs_usage(d, t, u, a);
    }
    std::map<std::string, std::string> ipv4() override { return inner_->ipv4(); }
    bool root_dev(unsigned &maj, unsigned &min) override { return inner_->root_dev(maj, min); }
    long page_size() override { return inner_->page_size(); }
    std::int64_t now_ms() override { return inner_->now_ms(); }
    void begin_sample(int period_ms) override { inner_->begin_sample(period_ms); }
    std::string describe() const override { return "flipping " + dir_; }

    int vc_mode = 0;          /* 0 the fixture's, 1 missing, 2 refused */
    bool no_hwmon = false;    /* hide /sys/class/hwmon (no rpi_volt alarm) */
    int vc_calls = 0;

private:
    bool hide(const std::string &path) const { return no_hwmon && path.find("/sys/class/hwmon") == 0; }
    std::string dir_;
    std::unique_ptr<Source> inner_;
    bool flipped_ = false;
};

std::shared_ptr<const Snapshot> two_samples(Sampler &s, Flipping *f)
{
    s.step();
    f->flip();
    s.step();
    return s.latest();
}

const Row *find_row(const std::vector<Row> &rows, const std::string &label)
{
    for (const Row &r : rows)
        if (r.label == label) return &r;
    return nullptr;
}

void zero()
{
    auto src = std::make_unique<Flipping>("pi-zero-2w");
    Flipping *f = src.get();
    Sampler s(std::move(src), 1);
    CHECK_EQ(s.facts().cores, 4);
    CHECK_EQ(s.facts().cpu, std::string("4 x Cortex-A53"));
    s.set_want_link(true);
    const auto snap = two_samples(s, f);
    CHECK(snap != nullptr);
    CHECK_EQ(snap->seq, static_cast<std::uint64_t>(2));
    CHECK(snap->cpu.ok);
    CHECK_NEAR(snap->cpu.total, 25.0, 0.01);
    CHECK_EQ(snap->cpu.cores.size(), static_cast<std::size_t>(4));
    CHECK_NEAR(snap->cpu.cores[2], 55.0, 0.01);
    CHECK_EQ(snap->cpu.cur_mhz, 1000);
    CHECK_EQ(snap->cpu.max_mhz, 1000);
    CHECK_EQ(snap->cpu.governor, std::string("ondemand"));
    CHECK(snap->mem.ok);
    CHECK(snap->temp.ok);
    CHECK_NEAR(snap->temp.c, 48.312, 1e-6);
    CHECK(!snap->fan.present);
    CHECK(snap->power.source == PowerSource::Firmware);
    CHECK_EQ(snap->power.via, std::string("vcgencmd"));
    CHECK(snap->power.core_ok);
    CHECK_NEAR(snap->power.core_v, 1.2, 1e-9);
    CHECK(!snap->power.supply_ok); /* not a Pi 5: no 5 V reading */
    CHECK_EQ(snap->sys.firmware, std::string("2025-03-17 (2ab6c7e)"));
    CHECK_EQ(snap->sys.serial, std::string("a1b2c3d4"));
    CHECK_EQ(snap->sys.hostname, std::string("deck"));
    CHECK_EQ(snap->fs.size(), static_cast<std::size_t>(2));
    CHECK_EQ(snap->fs[0].dir, std::string("/"));
    CHECK_NEAR(snap->fs[0].pct(), 18.9, 0.1);
    CHECK(snap->disk.ok);
    CHECK_EQ(snap->disk.name, std::string("mmcblk0"));
    CHECK_NEAR(snap->disk.read_bps, 409600.0, 1e-6);
    CHECK_NEAR(snap->disk.write_bps, 204800.0, 1e-6);
    CHECK_EQ(snap->net.size(), static_cast<std::size_t>(1)); /* lo left out */
    CHECK_EQ(snap->main_iface, std::string("wlan0"));
    const NetInfo &w = snap->net[0];
    CHECK(w.wireless);
    CHECK_EQ(w.ipv4, std::string("192.168.1.23/24"));
    CHECK_NEAR(w.rx_bps, 12800.0, 1e-6);
    CHECK_NEAR(w.tx_bps, 2048.0, 1e-6);
    CHECK(w.has_level && w.level_dbm == -52.0);
    CHECK(w.link_valid && w.link.ssid == "DeckNet");
    CHECK(snap->bt == BtState::On);
    CHECK_EQ(snap->hist.cpu.size(), static_cast<std::size_t>(1)); /* the first sample has no CPU % */
    CHECK_EQ(snap->hist.temp.size(), static_cast<std::size_t>(2));
    CHECK_EQ(snap->hist.rx.size(), static_cast<std::size_t>(1));
    CHECK(!snap->procs_valid);

    // the overview of this snapshot: the six tiles and the footer
    const std::vector<TileText> ov = overview_tiles(snap.get());
    CHECK_EQ(ov.size(), static_cast<std::size_t>(kTileCount));
    CHECK_EQ(ov[kTileCpu].value[0], std::string("25%"));
    CHECK_EQ(ov[kTileCpu].detail[0], std::string("1000 / 1000 MHz, load 0.84"));
    CHECK_EQ(ov[kTileCpu].hint, std::string("4 cores"));
    CHECK_EQ(ov[kTileTemp].value[0], std::string(fmt_temp(48.312)));
    CHECK(ov[kTileTemp].value_tone == Tone::Good);
    CHECK_EQ(ov[kTileMem].value[0], std::string("47%"));
    CHECK_EQ(ov[kTileMem].detail[0], std::string("196 / 417 MB, swap 26 MB"));
    CHECK_EQ(ov[kTileStorage].value[0], std::string("19%"));
    CHECK_EQ(ov[kTileStorage].detail[0], std::string("5.2 / 29 GB used"));
    CHECK_NEAR(ov[kTileStorage].bar_pct, 18.9, 0.1);
    CHECK_EQ(ov[kTilePower].value[0], std::string("Power OK"));
    CHECK(ov[kTilePower].frame_tone == Tone::Good);
    CHECK_EQ(ov[kTileNet].value[0], std::string("192.168.1.23"));
    CHECK_EQ(ov[kTileNet].hint, std::string("wlan0 -52 dBm"));
    CHECK_EQ(ov[kTileNet].detail[0], std::string("In 12 kB/s, out 2.0 kB/s"));
    const std::vector<std::vector<std::string>> foot = overview_footer(snap.get(), 1);
    CHECK_EQ(foot[kFootPeriod][0], std::string("Every 1 s"));
    CHECK_EQ(foot[kFootSystem][0], std::string("System"));

    // the pages' rows of this snapshot (what the lists show)
    const std::vector<Row> cpu = cpu_rows(snap.get());
    CHECK_EQ(cpu[0].label, std::string("Total"));
    CHECK_EQ(cpu[0].values[0], std::string("25%"));
    CHECK(cpu[3].label == "Core 2" && cpu[3].tone == Tone::Normal); /* 55 % */
    const Row *heading = find_row(cpu, "Top processes by CPU");
    CHECK(heading && heading->type == RowType::Section);
    CHECK(find_row(cpu, "Reading processes") != nullptr); /* no process table yet: it says so */
    CHECK_EQ(cpu.back().id, kRowAllProcs);
    CHECK(process_rows(snap.get(), SortBy::Cpu).empty()); /* the list shows "Reading processes..." */
    const std::vector<Row> th = thermal_rows(snap.get());
    CHECK_EQ(th[0].label, std::string("Power"));
    CHECK_EQ(th[0].values[0], std::string("OK"));
    CHECK(th[0].tone == Tone::Good);
    CHECK(find_row(th, "Check the power supply") == nullptr); /* only while under-voltage now */
    CHECK(find_row(th, "CPU temperature")->tone == Tone::Normal); /* 48 °C */
    CHECK(find_row(th, "Under-voltage")->tone == Tone::Good);     /* never since boot */
    const std::vector<Row> st = storage_rows(snap.get());
    CHECK(st[0].label == "/" && st[0].tone == Tone::Normal);      /* 19 % */
    CHECK_EQ(st[0].values[0], std::string("5.2 / 29 GB (19%)"));
    const std::vector<Row> net = network_rows(snap.get());
    CHECK(net[0].type == RowType::Section && net[0].label == "wlan0 (Wi-Fi)");
    CHECK_EQ(find_row(net, "Network")->values[0], std::string("DeckNet"));
    CHECK_EQ(net.back().label, std::string("Bluetooth"));
    // what the list shows at the deck's 640x480 (about 15 px per label character, 13 per value character, 528 px)
    const TextWidth deck = [](const std::string &t, bool value) { return static_cast<int>(t.size()) * (value ? 13 : 15); };
    const std::vector<ListItem> items = list_items(th, 528, 8, deck);
    CHECK_EQ(items.size(), th.size());
    CHECK(items[0].label == "Power" && items[0].value == "OK" && items[0].tone == Tone::Good);

    // the period: the history starts again
    s.set_period(5);
    s.step();
    CHECK_EQ(s.latest()->period_s, 5);
    CHECK_EQ(s.latest()->hist.temp.size(), static_cast<std::size_t>(1));
}

void pi3a()
{
    auto src = std::make_unique<Flipping>("pi-3a-plus");
    Flipping *f = src.get();
    Sampler s(std::move(src), 1);
    s.set_want_link(true);
    const auto snap = two_samples(s, f);
    CHECK_NEAR(snap->cpu.total, 99.5, 0.01);
    CHECK_EQ(snap->cpu.cur_mhz, 600);
    CHECK_EQ(snap->cpu.max_mhz, 1400);
    CHECK(snap->power.source == PowerSource::Firmware);
    CHECK_EQ(snap->power.bits, 0x50005u);
    CHECK(snap->power.flags[kUnderVoltage].now);
    CHECK(snap->power.flags[kThrottled].now);
    CHECK(snap->net[0].link_valid == false); /* iw not installed: no link, no error */
    CHECK_EQ(snap->fs.size(), static_cast<std::size_t>(3));
    CHECK_EQ(snap->fs[2].dir, std::string("/media/pi/USB STICK"));
    CHECK(snap->bt == BtState::Absent);
    const std::vector<TileText> ov = overview_tiles(snap.get());
    CHECK_EQ(ov[kTilePower].value[0], std::string("Under-voltage"));
    CHECK_EQ(ov[kTilePower].detail[0], std::string("Now: check the power supply"));
    CHECK(ov[kTilePower].value_tone == Tone::Error && ov[kTilePower].frame_tone == Tone::Error);
    CHECK(ov[kTileStorage].value_tone == Tone::Warn); /* 93 % full */
    CHECK_EQ(ov[kTileStorage].value[0], std::string("93%"));
    CHECK_EQ(ov[kTileCpu].detail[0], std::string("600 / 1400 MHz, load 4.21"));
    CHECK_EQ(ov[kTileNet].hint, std::string("wlan0 -69 dBm"));
    const std::vector<Row> th = thermal_rows(snap.get());
    CHECK_EQ(th[0].label, std::string("Power"));
    CHECK_EQ(th[0].values[0], std::string("Under-voltage now"));
    CHECK(th[0].tone == Tone::Error);
    // the advice: an item of its own right under the Power row, no value
    CHECK(th[1].type == RowType::Item && th[1].label == "Check the power supply" && th[1].values.empty());
    CHECK_EQ(find_row(th, "Under-voltage")->values[0], std::string("now"));
    CHECK(find_row(th, "Under-voltage")->tone == Tone::Error);
    CHECK_EQ(find_row(th, "Throttled")->values[0], std::string("now"));
    CHECK(find_row(th, "Throttled")->tone == Tone::Error);
    CHECK_EQ(find_row(th, "Soft temp limit")->values[0], std::string("never since boot"));
    CHECK(find_row(th, "Soft temp limit")->tone == Tone::Good);
    CHECK_EQ(find_row(th, "Firmware flags")->values[0], std::string("0x50005 (vcgencmd)"));
    // the 93 % full card: amber; the boot partition: not
    const std::vector<Row> st = storage_rows(snap.get());
    CHECK(st[0].label == "/" && st[0].tone == Tone::Warn);
    CHECK(find_row(st, "/boot/firmware") && find_row(st, "/boot/firmware")->tone == Tone::Normal);
    CHECK(find_row(st, "/media/pi/USB STICK") != nullptr);
    // the deck's row: the power state shortened so that "Power" stays whole, in red
    const TextWidth deck = [](const std::string &t, bool value) { return static_cast<int>(t.size()) * (value ? 13 : 15); };
    const std::vector<ListItem> items = list_items(th, 528, 8, deck);
    CHECK_EQ(items[0].value, std::string("Under-voltage now"));
    CHECK(items[0].tone == Tone::Error);
    // the advice row fits the deck's row whole ("Check the power supply": 22 x 15 = 330 of 528 px)
    CHECK(items[1].label == "Check the power supply" && items[1].value.empty());
    CHECK(deck(items[1].label, false) <= 528);
    // a wide screen: the same short state (the advice is its own row)
    CHECK_EQ(list_items(th, 1000, 8, deck)[0].value, std::string("Under-voltage now"));
}

void pi5()
{
    auto src = std::make_unique<Flipping>("pi-5");
    Flipping *f = src.get();
    Sampler s(std::move(src), 2);
    CHECK_EQ(s.facts().cpu, std::string("4 x Cortex-A76"));
    const auto snap = two_samples(s, f);
    CHECK_EQ(snap->period_s, 2);
    CHECK_NEAR(snap->cpu.total, 4.0, 0.01);
    CHECK(snap->fan.present);
    CHECK_EQ(snap->fan.state, 2);
    CHECK_EQ(snap->fan.max_state, 4);
    CHECK_EQ(snap->fan.rpm, 3120);
    CHECK(snap->power.supply_ok);
    CHECK_NEAR(snap->power.supply_v, 5.134712, 1e-6);
    CHECK_NEAR(snap->power.core_v, 0.72, 1e-9);
    CHECK_EQ(snap->disk.name, std::string("nvme0n1"));
    CHECK_EQ(snap->main_iface, std::string("eth0"));
    CHECK_EQ(snap->net.size(), static_cast<std::size_t>(2));
    CHECK(snap->net[1].wireless);
    CHECK_EQ(snap->net[1].state, std::string("down"));
    CHECK(snap->bt == BtState::Off);
    CHECK_EQ(snap->sys.firmware, std::string("2025-03-10 (2bb2ae6)"));
    const std::vector<Row> th = thermal_rows(snap.get());
    CHECK_EQ(th[0].label, std::string("Power"));
    CHECK_EQ(th[0].values[0], std::string("OK"));
    CHECK_EQ(find_row(th, "Fan")->values[0], std::string("Level 2 of 4, 3120 rpm"));
    CHECK_EQ(find_row(th, "Supply (5 V input)")->values[0], std::string("5.13 V"));
    const std::vector<Row> net = network_rows(snap.get());
    CHECK_EQ(net[0].label, std::string("eth0"));
    CHECK(net[0].type == RowType::Section);
    int sections = 0;
    for (const Row &r : net) sections += r.type == RowType::Section;
    CHECK_EQ(sections, 2); /* eth0, wlan0 */
    CHECK_EQ(find_row(net, "Bluetooth")->values[0], std::string("Off"));
}

void missing_vcgencmd()
{
    // vcgencmd not installed: the rpi_volt alarm still tells under-voltage
    {
        auto src = std::make_unique<Flipping>("pi-3a-plus");
        src->vc_mode = 1;
        Flipping *f = src.get();
        Sampler s(std::move(src), 1);
        const auto snap = two_samples(s, f);
        CHECK(snap->power.source == PowerSource::Alarm);
        CHECK(snap->power.flags[kUnderVoltage].now);
        CHECK(!snap->power.core_ok);
        CHECK(snap->sys.firmware.empty());
        CHECK_EQ(f->vc_calls, 1); /* asked once, never again */
        const std::vector<Row> th = thermal_rows(snap.get());
        CHECK_EQ(th[0].label, std::string("Power"));
        CHECK_EQ(th[0].values[0], std::string("Under-voltage now"));
        CHECK_EQ(th[1].label, std::string("Check the power supply"));
        CHECK(find_row(th, "Throttled") == nullptr);
        CHECK(find_row(th, "Under-voltage")->tone == Tone::Error);
        CHECK_EQ(find_row(th, "Source")->values[0], std::string("rpi_volt alarm only"));
    }
    // neither vcgencmd nor the alarm: silently no power row
    {
        auto src = std::make_unique<Flipping>("pi-zero-2w");
        src->vc_mode = 1;
        src->no_hwmon = true;
        Flipping *f = src.get();
        Sampler s(std::move(src), 1);
        const auto snap = two_samples(s, f);
        CHECK(snap->power.source == PowerSource::None);
        const std::vector<TileText> ov = overview_tiles(snap.get());
        CHECK_EQ(ov[kTilePower].value[0], std::string("Unknown"));
        CHECK(ov[kTilePower].value_dim);
        CHECK(find_row(thermal_rows(snap.get()), "Power") == nullptr);
    }
    // vcgencmd refused (not in the video group): said once, with the fix
    {
        auto src = std::make_unique<Flipping>("pi-zero-2w");
        src->vc_mode = 2;
        src->no_hwmon = true;
        Flipping *f = src.get();
        Sampler s(std::move(src), 1);
        const auto snap = two_samples(s, f);
        CHECK(snap->power.source == PowerSource::NoAccess);
        CHECK_EQ(f->vc_calls, 1);
        const std::vector<TileText> ov = overview_tiles(snap.get());
        CHECK_EQ(ov[kTilePower].value[0], std::string("No access"));
        CHECK(ov[kTilePower].frame_tone == Tone::Warn);
        const std::vector<Row> th = thermal_rows(snap.get());
        CHECK_EQ(th[0].label, std::string("Power"));
        CHECK_EQ(th[0].values[0], std::string("No access: needs the video group"));
        CHECK(th[0].tone == Tone::Warn);
        CHECK_EQ(find_row(th, "Fix")->values[0], std::string("sudo usermod -aG video $USER"));
    }
}

void scenario()
{
    // the Pi 3A+ scene: the history is filled at once (virtual time), under-voltage now, stress-ng on top
    Sampler s(make_fixture_source(fixture_dir("pi-3a-plus"), "undervoltage"), 1);
    s.set_want_procs(true);
    s.start();
    for (int i = 0; i < 300 && s.seq() < 2; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    const auto snap = s.latest();
    s.stop();
    CHECK(snap != nullptr);
    if (!snap) return;
    CHECK(snap->hist.cpu.size() >= kHistory - 1);
    CHECK(snap->hist.temp.size() == kHistory);
    CHECK(snap->cpu.total > 70);
    CHECK(snap->power.flags[kUnderVoltage].now);
    CHECK(snap->power.flags[kUnderVoltage].seen);
    CHECK(snap->procs_valid);
    CHECK(!snap->top_cpu.empty() && snap->top_cpu[0].name == "stress-ng");
    CHECK(snap->top_cpu[0].cpu_pct > 50);
    CHECK_EQ(snap->top_mem[0].name, std::string("applaunch"));
    const std::vector<Row> pr = process_rows(snap.get(), SortBy::Cpu);
    CHECK_EQ(pr[0].label, std::string("Sort by"));
    CHECK_EQ(pr[1].label, std::string("stress-ng"));
    CHECK(pr.size() == 9); /* Sort by + the 8 processes of the scenario */
    for (std::size_t i = 1; i < pr.size(); ++i) CHECK_EQ(pr[i].id, static_cast<int>(i - 1));
    // the CPU page: the section, then the top 5 by CPU, then All processes
    const std::vector<Row> cpu = cpu_rows(snap.get());
    std::size_t at = 0;
    while (at < cpu.size() && cpu[at].type != RowType::Section) ++at;
    CHECK(at + 7 == cpu.size()); /* section, 5 processes, All processes */
    if (at + 7 == cpu.size()) {
        CHECK_EQ(cpu[at + 1].label, std::string("stress-ng"));
        CHECK(cpu[at + 6].id == kRowAllProcs);
    }
    CHECK(cpu[0].tone == use_tone(snap->cpu.total, 80, 95));
    const std::vector<Row> mem = memory_rows(snap.get());
    CHECK_EQ(find_row(mem, "Top processes by memory")->type == RowType::Section, true);
    CHECK(find_row(mem, "applaunch") != nullptr);
    // the hot Pi 5
    Sampler h(make_fixture_source(fixture_dir("pi-5"), "hot"), 1);
    h.start();
    for (int i = 0; i < 300 && h.seq() < 1; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    const auto hot = h.latest();
    h.stop();
    CHECK(hot && hot->temp.ok && hot->temp.c >= 80);
    if (hot) {
        CHECK(hot->power.flags[kSoftTempLimit].now);
        const std::vector<TileText> ov = overview_tiles(hot.get());
        CHECK(ov[kTileTemp].value_tone == Tone::Error);
        CHECK_EQ(ov[kTileTemp].detail[0], std::string("Hot: the Pi slows down"));
        CHECK_EQ(ov[kTileTemp].hint, std::string("fan 3120 rpm"));
        CHECK_EQ(ov[kTilePower].value[0], std::string("Throttled"));
        CHECK(ov[kTilePower].frame_tone == Tone::Error);
        CHECK(hot->hist.rx.size() >= kHistory - 1);
        // Temp & power: red temperature and state, the throttled sentence shortened on the deck
        const std::vector<Row> th = thermal_rows(hot.get());
        CHECK_EQ(th[0].values[0], std::string("Throttled now: the CPU is too hot"));
        CHECK(th[0].tone == Tone::Error);
        CHECK(find_row(th, "Check the power supply") == nullptr);
        CHECK(find_row(th, "CPU temperature")->tone == Tone::Error);
        CHECK(find_row(th, "State")->tone == Tone::Error);
        CHECK(find_row(th, "Soft temp limit")->tone == Tone::Warn);
        const TextWidth deck = [](const std::string &t, bool value) {
            return static_cast<int>(t.size()) * (value ? 13 : 15);
        };
        CHECK_EQ(list_items(th, 528, 8, deck)[0].value, std::string("Throttled now: the CPU is too hot"));
        CHECK_EQ(list_items(th, 480, 8, deck)[0].value, std::string("Throttled now: too hot")); /* a narrower row */
    }
}

} // namespace

void test_sampler()
{
    zero();
    pi3a();
    pi5();
    missing_vcgencmd();
    scenario();
}
