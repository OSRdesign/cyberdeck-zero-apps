/*
 * SPDX-License-Identifier: MIT
 *
 * Cassette app tests (task T8): the launcher's read-only Bluetooth audio facts in the whole controller (bt_rig.hpp: FakePulse with the
 * Evolve 65), the launcher's two state files on FakeFactsIo. Without the facts (no provider, no files, a file without the keys) every
 * list and text is as before T8: the earlier groups (test_bt, test_bt_out, test_bt_reconnect) run without a provider and are the
 * proof; here the same snapshot is compared with and without an empty provider.
 */
#include "bt_rig.hpp"
#include "fake_facts.hpp"

using namespace ta;
using namespace tbt;

namespace {

const std::string kSpk = "9E:0C:81:E6:5D:1A";
const std::string kSpkId = "bt:9E:0C:81:E6:5D:1A";
const std::string kBeats = "F8:66:5A:18:D3:69";
const std::string kBeatsId = "bt:F8:66:5A:18:D3:69";
const char *kPairedReason = "Not connected: connect it in Settings > Bluetooth";
const char *kStatusPath = "/run/user/1000/applaunch/status.state";
const char *kBoardPath = "/run/user/1000/applaunch/board.state";

/* the launcher's files, a provider on them, a rig: the files and the provider outlive the controller */
struct FactsRig {
    FakeFiles files;
    LauncherFactsProvider prov;
    BtRig b;
    explicit FactsRig(std::vector<FakeCard> usb = {}, bool with_headset = true, bool provider = true)
        : files(), prov(std::unique_ptr<FactsIo>(new FakeFactsIo(files)), kStatusPath, kBoardPath),
          b(std::move(usb), "a2dp-sink", with_headset) {
        files.alive = {801};
        b.r.cfg.server_running = [] { return true; };
        if (provider) b.r.cfg.facts = &prov;
    }
    void status(const std::string &dev) { files.put(kStatusPath, fake_status_text(dev)); }
    void board(const std::string &keys) { files.put(kBoardPath, fake_board_text(keys)); }
    /* the files changed: let the provider's second pass */
    void tick() { b.r.clock.advance(1100); }
};

const InputSnap *find_in(const Snapshot &s, const std::string &id) {
    for (const InputSnap &i : s.card.inputs)
        if (i.id == id) return &i;
    return nullptr;
}
const OutputSnap *find_out(const Snapshot &s, const std::string &id) {
    for (const OutputSnap &o : s.card.outputs)
        if (o.id == id) return &o;
    return nullptr;
}
bool has_text(const Snapshot &s, const std::string &text) {
    for (const Notice &n : s.notices)
        if (n.text == text) return true;
    return false;
}
bool wait_in(FactsRig &f, const std::string &id, bool present = true, int ms = 4000) {
    return f.b.r.wait([&](const Snapshot &s) { return (find_in(s, id) != nullptr) == present; }, ms);
}
bool wait_out(FactsRig &f, const std::string &id, bool present = true, int ms = 4000) {
    return f.b.r.wait([&](const Snapshot &s) { return (find_out(s, id) != nullptr) == present; }, ms);
}
void add_speaker_card(FakePulse &p, bool call_offered) {
    {
        std::lock_guard<std::recursive_mutex> g(p.lock());
        PulseCard c = fake_bt_card(kSpk, 71, true, "a2dp-sink", call_offered);
        c.description = "XWF-M18-M28-M38";
        p.cards.push_back(c);
    }
    p.add_sink(fake_bt_sink(kSpk, 71, "speaker"));
}

/* the lists as text, for the comparisons with and without the facts */
std::string lists_text(const Snapshot &s) {
    std::string t = s.card.bt_status + "|";
    for (const InputSnap &i : s.card.inputs)
        t += i.id + "," + i.label + "," + i.kind + "," + std::to_string(i.rate) + "," + (i.connected ? "c" : "-") + (i.usable ? "u" : "-") +
             i.hint_profile + "," + i.reason + ";";
    t += "|";
    for (const OutputSnap &o : s.card.outputs)
        t += o.id + "," + o.label + "," + o.kind + "," + std::to_string(o.rate) + "," + (o.connected ? "c" : "-") + (o.usable ? "u" : "-") +
             o.hint_profile + "," + o.reason + ";";
    return t;
}

/* ---- the fallback: no facts, or no keys ---- */

void t_fallback_identical() {
    std::string without, with_empty, with_nokeys;
    {
        FactsRig f(one_card(), true, false);
        f.b.r.open();
        CHECK(f.b.wait_inputs(2));
        CHECK(wait_out(f, kId));
        without = lists_text(f.b.r.snap());
    }
    {
        FactsRig f(one_card(), true, true); /* a provider, no files: an older launcher or none */
        f.b.r.open();
        CHECK(f.b.wait_inputs(2));
        CHECK(wait_out(f, kId));
        with_empty = lists_text(f.b.r.snap());
    }
    {
        FactsRig f(one_card(), true, true); /* the files of an older launcher: no Bluetooth keys at all */
        f.files.put(kStatusPath, fake_status_text(""));
        f.files.put(kBoardPath, fake_board_text("sound=1\nsound_kind=bluetooth\n"));
        f.b.r.open();
        CHECK(f.b.wait_inputs(2));
        CHECK(wait_out(f, kId));
        with_nokeys = lists_text(f.b.r.snap());
    }
    CHECK(!without.empty());
    CHECK_EQ(with_empty, without);
    CHECK_EQ(with_nokeys, without);
}

/* ---- paired devices that are not connected ---- */

void t_paired_rows() {
    FactsRig f(one_card());
    add_speaker_card(f.b.pulse, false); /* the speaker connected: its sink is listed (output only) */
    f.status("70_BF_92_53_6B_8B:headset:1:a2dp;9E_0C_81_E6_5D_1A:speaker:1;F8_66_5A_18_D3_69:headset:0;11_22_33_44_55_66:speaker:0");
    f.board("bt_audio=pipewire\nbt_hfp=1\nbt_le_audio=0\n");
    f.b.r.open();
    Controller &c = f.b.r.ctl();
    CHECK(wait_in(f, kBeatsId));
    CHECK(wait_out(f, "bt:11:22:33:44:55:66"));
    Snapshot s = f.b.r.snap();
    /* inputs: the USB card, the Evolve (connected), the Beats headset dimmed; the speakers are not inputs */
    const InputSnap *ev = find_in(s, kId);
    const InputSnap *bt = find_in(s, kBeatsId);
    CHECK(ev && bt);
    CHECK(!find_in(s, kSpkId));
    CHECK(!find_in(s, "bt:11:22:33:44:55:66"));
    if (ev && bt) {
        CHECK(ev->connected && ev->usable);
        CHECK(bt->bluetooth && !bt->connected && !bt->usable);
        CHECK_EQ(bt->label, "Bluetooth device D3:69");
        CHECK_EQ(bt->kind, "headset");
        CHECK_EQ(bt->reason, kPairedReason);
        CHECK_EQ(bt->rate, 0u);
        CHECK(!bt->hw_level);
    }
    CHECK_EQ(s.card.bt_status, "");
    /* outputs: the Evolve and the connected speaker, then the two paired devices dimmed, then Default output */
    const OutputSnap *ob = find_out(s, kBeatsId);
    const OutputSnap *os = find_out(s, "bt:11:22:33:44:55:66");
    const OutputSnap *oe = find_out(s, kId);
    const OutputSnap *ok = find_out(s, kSpkId);
    CHECK(ob && os && oe && ok);
    if (ob && os && oe && ok) {
        CHECK(oe->usable && ok->usable);
        CHECK_EQ(ok->kind, "speaker");
        CHECK(!ob->usable && !ob->connected);
        CHECK_EQ(ob->reason, kPairedReason);
        CHECK_EQ(ob->kind, "headset");
        CHECK_EQ(os->label, "Bluetooth device 55:66");
        CHECK_EQ(os->kind, "speaker");
        CHECK_EQ(s.card.outputs.back().id, "default");
    }
    CHECK_EQ(s.card.outputs.size(), s.card.output_labels.size());
    /* choosing a dimmed device is refused with the reason, nothing changes */
    const int bi = [&] {
        for (size_t i = 0; i < s.card.inputs.size(); ++i)
            if (s.card.inputs[i].id == kBeatsId) return static_cast<int>(i);
        return -1;
    }();
    CHECK(bi >= 0);
    const std::string card_before = s.settings.card;
    CHECK(!c.select_card(bi));
    s = f.b.r.snap();
    CHECK(has_text(s, kPairedReason));
    CHECK_EQ(s.settings.card, card_before);
    CHECK(!c.select_output(kBeatsId));
    CHECK_EQ(f.b.r.snap().settings.output_device, "");
    CHECK(!c.set_monitor_output(kBeatsId));
    CHECK_EQ(f.b.r.snap().settings.monitor_output, "");
    /* a connected one can still be chosen */
    CHECK(c.select_output(kSpkId));
    CHECK_EQ(f.b.r.snap().settings.output_device, kSpkId);
}

/* the Beats pair is switched on later: the dimmed rows follow the launcher's file, and go when the device connects */
void t_paired_follow_file() {
    FactsRig f(one_card());
    f.status("70_BF_92_53_6B_8B:headset:1:a2dp");
    f.board("bt_audio=pipewire\n");
    f.b.r.open();
    CHECK(f.b.wait_inputs(2));
    CHECK(!find_in(f.b.r.snap(), kBeatsId));
    f.status("70_BF_92_53_6B_8B:headset:1:a2dp;F8_66_5A_18_D3_69:headset:0");
    f.tick();
    CHECK(wait_in(f, kBeatsId));
    CHECK(wait_out(f, kBeatsId));
    /* it connects: the server lists it, the dimmed row is gone and the live one is there */
    f.b.pulse.add_source(fake_bt_source(kBeats, 16000, 90));
    {
        std::lock_guard<std::recursive_mutex> g(f.b.pulse.lock());
        PulseCard c = fake_bt_card(kBeats, 90, true, "headset-head-unit");
        c.description = "Beats Fit Pro";
        f.b.pulse.cards.push_back(c);
    }
    f.b.pulse.add_sink(fake_bt_sink(kBeats, 90));
    f.status("70_BF_92_53_6B_8B:headset:1:a2dp;F8_66_5A_18_D3_69:headset:1:hfp");
    f.tick();
    CHECK(f.b.r.wait([&](const Snapshot &s) {
        const InputSnap *i = find_in(s, kBeatsId);
        return i && i->connected && i->usable;
    }));
    const Snapshot s = f.b.r.snap();
    size_t n = 0;
    for (const InputSnap &i : s.card.inputs) n += i.id == kBeatsId;
    CHECK_EQ(n, 1u);
    /* the launcher file goes away (the launcher stopped): the dimmed rows go with it, the live ones stay */
    f.status("70_BF_92_53_6B_8B:headset:1:a2dp;11_22_33_44_55_66:speaker:0");
    f.tick();
    CHECK(wait_out(f, "bt:11:22:33:44:55:66"));
    f.files.alive.clear();
    f.tick();
    CHECK(wait_out(f, "bt:11:22:33:44:55:66", false));
    CHECK(find_in(f.b.r.snap(), kId) != nullptr);
}

/* no Bluetooth audio stack on the board: no dimmed rows (they could never connect to anything Cassette can use) */
void t_no_stack_no_rows() {
    FactsRig f(one_card());
    f.status("F8_66_5A_18_D3_69:headset:0");
    f.board("bt_audio=none\n");
    f.b.r.open();
    CHECK(f.b.wait_inputs(2));
    f.tick();
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    CHECK(!find_in(f.b.r.snap(), kBeatsId));
    CHECK(!find_out(f.b.r.snap(), kBeatsId));
    /* the launcher's probe finishes later: the stack is there now */
    f.board("bt_audio=pipewire\n");
    f.tick();
    CHECK(wait_in(f, kBeatsId));
}

/* ---- kinds ---- */

void t_kinds() {
    /* the server calls a device a headset (a card with a call profile) that the launcher knows as a speaker: no input with the facts */
    FactsRig a(one_card(), true, false);
    add_speaker_card(a.b.pulse, true);
    a.b.pulse.add_source(fake_bt_source(kSpk, 16000, 71));
    a.b.r.open();
    CHECK(wait_in(a, kSpkId));
    CHECK_EQ(find_in(a.b.r.snap(), kSpkId)->kind, "headset"); /* as before T8 */

    FactsRig f(one_card());
    add_speaker_card(f.b.pulse, true);
    f.b.pulse.add_source(fake_bt_source(kSpk, 16000, 71));
    f.status("70_BF_92_53_6B_8B:headset:1;9E_0C_81_E6_5D_1A:speaker:1");
    f.board("bt_audio=pipewire\n");
    f.b.r.open();
    CHECK(wait_out(f, kSpkId));
    CHECK(f.b.wait_inputs(2));
    Snapshot s = f.b.r.snap();
    CHECK(!find_in(s, kSpkId));                 /* never an input */
    CHECK(find_in(s, kId) != nullptr);         /* the headset is */
    const OutputSnap *o = find_out(s, kSpkId); /* but it is an output, with the launcher's kind */
    CHECK(o && o->usable);
    if (o) CHECK_EQ(o->kind, "speaker");
    /* the facts change: the same device is a headset now (the launcher learned its profiles): the input appears */
    f.status("70_BF_92_53_6B_8B:headset:1;9E_0C_81_E6_5D_1A:headset:1");
    f.tick();
    CHECK(wait_in(f, kSpkId));
    /* and headphones: an output, no input */
    f.status("70_BF_92_53_6B_8B:headset:1;9E_0C_81_E6_5D_1A:headphones:1");
    f.tick();
    CHECK(wait_in(f, kSpkId, false));
    CHECK(find_out(f.b.r.snap(), kSpkId) != nullptr);
    CHECK_EQ(find_out(f.b.r.snap(), kSpkId)->kind, "headphones");
}

/* ---- the profile hint ---- */

void t_hint_profile() {
    /* a headset the server lists only by a sink, no card yet: the launcher's profile is the hint */
    FactsRig f(one_card(), false);
    PulseDevice sink = fake_bt_sink(kMac, kPulseNoIndex);
    sink.bt_profile.clear(); /* the node does not say its profile */
    f.b.pulse.add_sink(sink);
    f.status("70_BF_92_53_6B_8B:headset:1:hfp");
    f.board("bt_audio=pipewire\n");
    f.b.r.open();
    CHECK(wait_out(f, kId));
    CHECK_EQ(find_out(f.b.r.snap(), kId)->hint_profile, "call");
    f.status("70_BF_92_53_6B_8B:headset:1:a2dp");
    f.tick();
    CHECK(f.b.r.wait([&](const Snapshot &s) {
        const OutputSnap *o = find_out(s, kId);
        return o && o->hint_profile == "music";
    }));
    /* the card arrives and says call: the card wins over the launcher's a2dp */
    {
        std::lock_guard<std::recursive_mutex> g(f.b.pulse.lock());
        f.b.pulse.cards.push_back(fake_bt_card(kMac, 70, true, "headset-head-unit"));
    }
    f.b.pulse.add_sink(fake_bt_sink("01:02:03:04:05:06", 5)); /* a server change event: the controller lists again */
    CHECK(f.b.r.wait([&](const Snapshot &s) {
        const OutputSnap *o = find_out(s, kId);
        return o && o->hint_profile == "call";
    }));
}

/* ---- the not-set-up texts ---- */

void t_notsetup_text() {
    /* no sound server answers; the controller has no connection to make */
    auto run = [&](const std::string &board_keys, const std::string &expect, bool have_board = true) {
        FactsRig f(one_card());
        f.b.r.cfg.server_running = [] { return false; };
        f.status("");
        if (have_board) f.board(board_keys);
        f.b.r.open();
        CHECK(f.b.r.wait([&](const Snapshot &s) { return s.card.bt_status == expect; }, 3000));
        CHECK_EQ(f.b.r.snap().card.bt_status, expect);
    };
    run("bt_audio=none\n", "Bluetooth audio is not set up");
    run("bt_audio=pipewire\n", "The sound server is not running");
    run("bt_audio=pulseaudio\n", "The sound server is not running");
    run("bt_audio=bluealsa\n", "Bluetooth audio is not set up");
    run("sound=1\n", "Bluetooth audio is not set up"); /* an older board.state: today's text */
    run("", "Bluetooth audio is not set up", false);   /* no board.state at all */
    /* the text follows the file while the controller runs */
    FactsRig f(one_card());
    f.b.r.cfg.server_running = [] { return false; };
    f.board("bt_audio=none\n");
    f.b.r.open();
    CHECK(f.b.r.wait([](const Snapshot &s) { return s.card.bt_status == "Bluetooth audio is not set up"; }, 3000));
    f.board("bt_audio=pipewire\n");
    f.tick();
    CHECK(f.b.r.wait([](const Snapshot &s) { return s.card.bt_status == "The sound server is not running"; }, 3000));
    /* the server answers: no text, whatever the launcher says */
    FactsRig g(one_card());
    g.board("bt_audio=none\n");
    g.b.r.open();
    CHECK(g.b.wait_inputs(2));
    CHECK_EQ(g.b.r.snap().card.bt_status, "");
}

/* ---- remembered names, a chosen device that is paired but not connected ---- */

void t_names_and_chosen() {
    std::string home;
    tp::TempDir keep; /* the rig's own folder goes with it: the settings of the first run are kept here */
    {
        FactsRig f(one_card());
        f.status("70_BF_92_53_6B_8B:headset:1:a2dp");
        f.board("bt_audio=pipewire\n");
        f.b.r.open();
        CHECK(f.b.wait_inputs(2));
        CHECK(f.b.r.wait([&](const Snapshot &s) { return s.settings.bt_names.count(kMac) != 0; }));
        CHECK_EQ(f.b.r.snap().settings.bt_names.at(kMac), "Evolve 65");
        CHECK(f.b.r.ctl().select_output(kId)); /* chosen: an output that will be paired but not connected after the restart */
        home = f.b.r.home;
        const std::string text = slurp(home + "/.config/cassette/settings.json");
        CHECK(text.find("\"bt_names\"") != std::string::npos);
        CHECK(text.find("\"70:BF:92:53:6B:8B\": \"Evolve 65\"") != std::string::npos);
        f.b.r.close();
        FILE *o = std::fopen((keep.path + "/settings.json").c_str(), "wb");
        if (o) {
            std::fputs(text.c_str(), o);
            std::fclose(o);
        }
    }
    {
        /* a new run: the headset is off (the server lists nothing of it), the launcher says paired, not connected */
        FactsRig f(one_card(), false);
        {
            posix_file_ops().mkdir(f.b.r.home + "/.config");
            posix_file_ops().mkdir(f.b.r.home + "/.config/cassette");
            FILE *o = std::fopen((f.b.r.home + "/.config/cassette/settings.json").c_str(), "wb");
            std::string text = slurp(keep.path + "/settings.json");
            if (o) {
                std::fputs(text.c_str(), o);
                std::fclose(o);
            }
        }
        f.status("70_BF_92_53_6B_8B:headset:0");
        f.board("bt_audio=pipewire\n");
        f.b.r.open();
        CHECK(wait_in(f, kId));
        const Snapshot s = f.b.r.snap();
        const InputSnap *i = find_in(s, kId);
        CHECK(i && !i->connected && !i->usable);
        if (i) {
            CHECK_EQ(i->label, "Evolve 65"); /* the remembered name, from the settings */
            CHECK_EQ(i->reason, kPairedReason);
        }
        /* the chosen output is that paired, not connected device: the same text */
        const OutputSnap *o = find_out(s, kId);
        CHECK(o && !o->usable && !o->connected);
        if (o) {
            CHECK_EQ(o->label, "Evolve 65");
            CHECK_EQ(o->reason, kPairedReason);
        }
        CHECK_EQ(s.card.output_devices[static_cast<size_t>(s.card.output_selected < 0 ? 0 : s.card.output_selected)], kId);
        /* without the facts file the same chosen output is the placeholder of before: "Not connected", the generic name */
        f.files.alive.clear();
        f.tick();
        CHECK(f.b.r.wait([&](const Snapshot &x) {
            const OutputSnap *p = find_out(x, kId);
            return p && p->reason == "Not connected";
        }));
        CHECK_EQ(find_out(f.b.r.snap(), kId)->label, "Bluetooth device");
    }
}

/* an old settings file (no bt_names) loads unchanged; a file with a bad bt_names keeps the rest */
void t_settings_roundtrip() {
    {
        BtRig b(one_card(), "a2dp-sink", false);
        const std::string dir = b.r.home + "/.config/cassette";
        posix_file_ops().mkdir(b.r.home + "/.config");
        posix_file_ops().mkdir(dir);
        FILE *o = std::fopen((dir + "/settings.json").c_str(), "wb");
        if (o) {
            std::fputs("{\"version\":1,\"format\":\"wav\",\"bt_names\":{\"70:bf:92:53:6b:8b\":\"Evolve 65\",\"nonsense\":\"x\",\"01:02:03:04:05:06\":5,"
                       "\"AA:BB:CC:DD:EE:FF\":\"\"}, \"future_key\": 7}",
                       o);
            std::fclose(o);
        }
        b.r.open();
        const Snapshot s = b.r.snap();
        CHECK_EQ(s.settings.bt_names.size(), 1u);
        CHECK_EQ(s.settings.bt_names.count(kMac), 1u);
        b.r.ctl().set_wave_fit(true); /* any change writes the file */
        const std::string text = slurp(dir + "/settings.json");
        CHECK(text.find("future_key") != std::string::npos);
        CHECK(text.find("\"70:BF:92:53:6B:8B\": \"Evolve 65\"") != std::string::npos);
        CHECK(text.find("nonsense") == std::string::npos);
    }
    {
        BtRig b(one_card(), "a2dp-sink", false);
        b.r.open();
        b.r.ctl().set_wave_fit(true);
        CHECK(slurp(b.r.home + "/.config/cassette/settings.json").find("bt_names") == std::string::npos); /* absent when empty */
    }
}

}  // namespace

void test_bt_facts_app() {
    t_fallback_identical();
    t_paired_rows();
    t_paired_follow_file();
    t_no_stack_no_rows();
    t_kinds();
    t_hint_profile();
    t_notsetup_text();
    t_names_and_chosen();
    t_settings_roundtrip();
}
