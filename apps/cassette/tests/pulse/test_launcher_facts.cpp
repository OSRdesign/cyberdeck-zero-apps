/*
 * SPDX-License-Identifier: MIT
 *
 * Cassette sound-server tests: the launcher's Bluetooth audio facts (launcher_facts.*, task T8): the tolerant parser of
 * status.state's bt_audio_dev and board.state's keys, the provider on a fake file system with a fake clock (re-read on a change,
 * once a second, the pid check, missing files), the state file path rule, and the merge into the Bluetooth input and output lists
 * (kind wins, profile hint, a speaker is no input, paired devices that are not connected as dimmed rows).
 */
#include "check.hpp"
#include "fake_facts.hpp"
#include "fake_pulse.hpp"
#include "launcher_facts.hpp"

#include <map>
#include <memory>
#include <string>
#include <vector>

using namespace cassette;

namespace {

const std::string kEvolve = "70:BF:92:53:6B:8B";
const std::string kSpeaker = "9E:0C:81:E6:5D:1A";
const std::string kBeats = "F8:66:5A:18:D3:69";

const char *kStatusPath = "/run/user/1000/applaunch/status.state";
const char *kBoardPath = "/run/user/1000/applaunch/board.state";

inline std::string status_text(const std::string &d, long pid = 801) { return fake_status_text(d, pid); }
inline std::string board_text(const std::string &e, long pid = 801) { return fake_board_text(e, pid); }

BtInput bt_in(const std::string &mac, const std::string &label, const std::string &kind, const std::string &hint = "unknown") {
    BtInput b;
    b.mac = mac;
    b.id = "bt:" + mac;
    b.label = label;
    b.kind = kind;
    b.connected = true;
    b.usable = true;
    b.hint_profile = hint;
    return b;
}

BtOutput bt_out(const std::string &mac, const std::string &label, const std::string &kind, const std::string &hint = "unknown") {
    BtOutput b;
    b.mac = mac;
    b.id = "bt:" + mac;
    b.label = label;
    b.kind = kind;
    b.connected = true;
    b.usable = true;
    b.hint_profile = hint;
    return b;
}

void test_parser() {
    /* the contract's example: a headset on A2DP, a speaker paired and off */
    {
        const std::vector<LauncherBtDev> v = parse_bt_audio_dev("70_BF_92_53_6B_8B:headset:1:a2dp;9E_0C_81_E6_5D_1A:speaker:0");
        CHECK_EQ(v.size(), 2u);
        if (v.size() == 2) {
            CHECK_EQ(v[0].mac, kEvolve);
            CHECK_EQ(v[0].kind, "headset");
            CHECK(v[0].connected);
            CHECK_EQ(v[0].profile, "a2dp");
            CHECK_EQ(v[1].mac, kSpeaker);
            CHECK_EQ(v[1].kind, "speaker");
            CHECK(!v[1].connected);
            CHECK_EQ(v[1].profile, "");
        }
    }
    /* hfp, headphones, lower case MAC letters give upper case with colons */
    {
        const std::vector<LauncherBtDev> v = parse_bt_audio_dev("70_bf_92_53_6b_8b:headset:1:hfp;f8_66_5a_18_d3_69:headphones:0");
        CHECK_EQ(v.size(), 2u);
        if (v.size() == 2) {
            CHECK_EQ(v[0].mac, kEvolve);
            CHECK_EQ(v[0].profile, "hfp");
            CHECK_EQ(v[1].mac, kBeats);
            CHECK_EQ(v[1].kind, "headphones");
        }
    }
    /* unknown trailing fields and unknown words are ignored; a new kind is kept as a word */
    {
        const std::vector<LauncherBtDev> v = parse_bt_audio_dev("70_BF_92_53_6B_8B:headset:1:a2dp:aac:99;9E_0C_81_E6_5D_1A:soundbar:1:xyz:extra");
        CHECK_EQ(v.size(), 2u);
        if (v.size() == 2) {
            CHECK_EQ(v[0].profile, "a2dp");
            CHECK_EQ(v[1].kind, "soundbar");
            CHECK_EQ(v[1].profile, "");
            CHECK(v[1].connected);
        }
    }
    /* malformed entries are skipped, the good ones stay */
    {
        const std::vector<LauncherBtDev> v = parse_bt_audio_dev(
            "garbage;70_BF_92_53_6B_8B:headset;70_BF_92_53_6B_8B:headset:2;:headset:1;ZZ_BF_92_53_6B_8B:headset:1;;"
            "9E_0C_81_E6_5D_1A:speaker:1;70_BF_92_53_6B_8:speaker:1;70_BF_92_53_6B_8B::1");
        CHECK_EQ(v.size(), 1u);
        if (v.size() == 1) CHECK_EQ(v[0].mac, kSpeaker);
    }
    /* empty and whitespace-free nothing */
    CHECK(parse_bt_audio_dev("").empty());
    CHECK(parse_bt_audio_dev(";;;").empty());
    /* at most 8 entries */
    {
        std::string v;
        for (int i = 0; i < 12; ++i) {
            char b[40];
            std::snprintf(b, sizeof b, "%s00_00_00_00_00_%02X:speaker:0", i ? ";" : "", i);
            v += b;
        }
        const std::vector<LauncherBtDev> p = parse_bt_audio_dev(v);
        CHECK_EQ(p.size(), 8u);
        if (p.size() == 8) CHECK_EQ(p[7].mac, "00:00:00:00:00:07");
    }
    /* a MAC written with colons is accepted too */
    {
        const std::vector<LauncherBtDev> v = parse_bt_audio_dev("70:BF:92:53:6B:8B:headset:1:a2dp");
        CHECK_EQ(v.size(), 1u);
        if (v.size() == 1) CHECK_EQ(v[0].profile, "a2dp");
    }
    CHECK_EQ(launcher_mac("70_BF_92_53_6B_8B"), kEvolve);
    CHECK_EQ(launcher_mac("70:bf:92:53:6b:8b"), kEvolve);
    CHECK_EQ(launcher_mac("70_BF_92_53_6B"), "");
    CHECK_EQ(launcher_mac("70-BF-92-53-6B-8B"), "");

    /* the state file: v=1 and pid, CRLF tolerated, a repeated key keeps the last, v=2 is not trusted */
    {
        const StateText st = parse_state_text("v=1\r\ngen=3\r\npid=801\r\nbt_audio=pipewire\r\n");
        CHECK(st.ok);
        CHECK_EQ(st.pid, 801L);
        CHECK_EQ(st.kv.at("bt_audio"), "pipewire");
        CHECK(!parse_state_text("v=2\npid=1\n").ok);
        CHECK(!parse_state_text("pid=1\n").ok);
        CHECK(!parse_state_text("").ok);
        CHECK_EQ(parse_state_text("v=1\npid=abc\n").pid, -1L);
        CHECK_EQ(parse_state_text("v=1\npid=1\npid=2\n").pid, 2L);
    }
    /* board.state: the three keys; an unknown word and a value other than 0 or 1 are absent */
    {
        LauncherFacts f;
        apply_board_text(parse_state_text(board_text("bt_audio=pipewire\nbt_hfp=1\nbt_le_audio=0\n")), f);
        CHECK_EQ(f.bt_audio, "pipewire");
        CHECK_EQ(f.bt_hfp, 1);
        CHECK_EQ(f.bt_le_audio, 0);
        CHECK(f.stack_server());
        CHECK(!f.stack_none());
        apply_board_text(parse_state_text(board_text("bt_audio=none\n")), f);
        CHECK(f.stack_none());
        CHECK(!f.stack_server());
        CHECK_EQ(f.bt_hfp, -1);
        CHECK_EQ(f.bt_le_audio, -1);
        apply_board_text(parse_state_text(board_text("bt_audio=jack\nbt_hfp=2\n")), f);
        CHECK_EQ(f.bt_audio, "");
        CHECK_EQ(f.bt_hfp, -1);
        apply_board_text(parse_state_text(board_text("bt_audio=pulseaudio\n")), f);
        CHECK(f.stack_server());
        apply_board_text(parse_state_text(board_text("bt_audio=bluealsa\n")), f);
        CHECK_EQ(f.bt_audio, "bluealsa");
        CHECK(!f.stack_server());
    }
    /* the real files of the HackBerry under launcher 0.10.0 (read 2026-11-12, before the Bluetooth keys): v=1, a pid, no key of ours:
     * parsed, trusted while the pid lives, every Bluetooth fact absent */
    {
        const char *status = "v=1\ngen=1324\npid=67399\nwifi=1\nwifi_pct=58\neth=0\nbt=connected\nbattery=90\nbattery_state=Discharging\n"
                             "volume=40\nmute=0\nbrightness_pct=-1\nssh=0\nssh_act=0\n";
        const char *board = "v=1\ngen=16\npid=67399\nlauncher=0.10.0\nboard=hackberry-cm5\narch=aarch64\nram_mb=4048\nbattery=1\n"
                            "battery_source=gauge:max17048\nbacklight=none\nbacklight_steps=0\nsound=1\nsound_kind=bluetooth\nsound_guess=1\n"
                            "requests=hello,caps,toast,beep,volume\nbacklight_note=use the button on the case\n"
                            "sound_note=pair XWF-M18-M28-M38 in Bluetooth\n";
        FakeFiles ff;
        ff.alive = {67399};
        ff.put(kStatusPath, status);
        ff.put(kBoardPath, board);
        LauncherFactsProvider p(std::unique_ptr<FactsIo>(new FakeFactsIo(ff)), kStatusPath, kBoardPath);
        p.poll(0);
        CHECK(p.facts().have_status);
        CHECK(p.facts().have_board);
        CHECK(p.facts().devices.empty());
        CHECK_EQ(p.facts().bt_audio, "");
        CHECK_EQ(p.facts().bt_hfp, -1);
        CHECK_EQ(p.facts().bt_le_audio, -1);
        CHECK(!p.facts().stack_none() && !p.facts().stack_server());
    }
    /* the path rule is the SDK's */
    CHECK_EQ(launcher_state_path("status.state", "/s", "/run/user/1"), "/s/status.state");
    CHECK_EQ(launcher_state_path("status.state", "", "/run/user/1"), "/run/user/1/applaunch/status.state");
    CHECK_EQ(launcher_state_path("board.state", "", ""), "/tmp/applaunch-board.state");
}

void test_provider() {
    FakeFiles ff;
    ff.alive = {801};
    LauncherFactsProvider p(std::unique_ptr<FactsIo>(new FakeFactsIo(ff)), kStatusPath, kBoardPath);
    /* no files: no facts, nothing changed */
    CHECK(!p.poll(0));
    CHECK(!p.facts().have_status);
    CHECK(!p.facts().have_board);
    CHECK(p.facts().devices.empty());
    CHECK_EQ(p.facts().bt_audio, "");
    CHECK_EQ(ff.reads, 0);
    /* within the second nothing is even looked at */
    const int stats0 = ff.stats;
    ff.put(kStatusPath, status_text("70_BF_92_53_6B_8B:headset:1;9E_0C_81_E6_5D_1A:speaker:0"));
    ff.put(kBoardPath, board_text("bt_audio=pipewire\nbt_hfp=1\nbt_le_audio=0\n"));
    CHECK(!p.poll(500));
    CHECK_EQ(ff.stats, stats0);
    CHECK(!p.facts().have_status);
    /* after a second: both read */
    CHECK(p.poll(1000));
    CHECK(p.facts().have_status);
    CHECK(p.facts().have_board);
    CHECK_EQ(p.facts().devices.size(), 2u);
    CHECK_EQ(p.facts().bt_audio, "pipewire");
    CHECK_EQ(p.facts().bt_hfp, 1);
    CHECK_EQ(ff.reads, 2);
    /* nothing changed: looked at (stat) but not read again, no change reported */
    CHECK(!p.poll(2000));
    CHECK_EQ(ff.reads, 2);
    CHECK(ff.stats > stats0);
    /* the launcher rewrites status.state: a profile appears */
    ff.put(kStatusPath, status_text("70_BF_92_53_6B_8B:headset:1:a2dp;9E_0C_81_E6_5D_1A:speaker:0"));
    CHECK(!p.poll(2500)); /* not yet: once a second */
    CHECK(p.poll(3000));
    CHECK_EQ(p.facts().devices[0].profile, "a2dp");
    CHECK_EQ(ff.reads, 3);
    /* a rewrite with the same content: read, no change */
    ff.put(kStatusPath, status_text("70_BF_92_53_6B_8B:headset:1:a2dp;9E_0C_81_E6_5D_1A:speaker:0"));
    CHECK(!p.poll(4000));
    CHECK_EQ(ff.reads, 4);
    /* the key vanishes (Bluetooth off): no devices, the file is still trusted */
    ff.put(kStatusPath, status_text(""));
    CHECK(p.poll(5000));
    CHECK(p.facts().have_status);
    CHECK(p.facts().devices.empty());
    /* the launcher dies: its pid is gone, the files are not trusted even though they did not change */
    ff.put(kStatusPath, status_text("70_BF_92_53_6B_8B:headset:1"));
    CHECK(p.poll(6000));
    ff.alive.clear();
    CHECK(p.poll(7000));
    CHECK(!p.facts().have_status);
    CHECK(!p.facts().have_board);
    CHECK(p.facts().devices.empty());
    CHECK_EQ(p.facts().bt_audio, "");
    /* a new launcher (another pid) writes both again */
    ff.alive = {905};
    ff.put(kStatusPath, status_text("70_BF_92_53_6B_8B:headset:0", 905));
    ff.put(kBoardPath, board_text("bt_audio=none\n", 905));
    CHECK(p.poll(8000));
    CHECK(p.facts().have_status);
    CHECK(p.facts().stack_none());
    CHECK(!p.facts().devices[0].connected);
    /* a file that is removed: the facts go */
    ff.remove(kBoardPath);
    CHECK(p.poll(9000));
    CHECK(!p.facts().have_board);
    CHECK_EQ(p.facts().bt_audio, "");
    CHECK(p.facts().have_status);
    /* a file that is not v=1 (an older launcher's, or garbage) is ignored */
    ff.put(kStatusPath, "pid=905\nbt_audio_dev=70_BF_92_53_6B_8B:headset:1\n");
    CHECK(p.poll(10000));
    CHECK(!p.facts().have_status);
    CHECK(p.facts().devices.empty());
    /* the same without a pid */
    ff.put(kStatusPath, "v=1\nbt_audio_dev=70_BF_92_53_6B_8B:headset:1\n");
    p.poll(11000);
    CHECK(!p.facts().have_status);
    CHECK_EQ(p.status_path(), kStatusPath);
    CHECK_EQ(p.board_path(), kBoardPath);
}

void test_merge() {
    LauncherFacts f;
    f.have_status = true;
    f.devices = parse_bt_audio_dev("70_BF_92_53_6B_8B:headset:1:a2dp;9E_0C_81_E6_5D_1A:speaker:1;F8_66_5A_18_D3_69:headset:0;"
                                   "11_22_33_44_55_66:speaker:0;AA_BB_CC_DD_EE_FF:headphones:0;00_11_22_33_44_55:soundbar:0");
    /* inputs: the headset keeps its entry (kind from the launcher, hint from the profile while unknown); the connected speaker is no input */
    {
        std::vector<BtInput> in = {bt_in(kEvolve, "Evolve 65", "other"), bt_in(kSpeaker, "XWF Speaker", "headset")};
        bt_facts_apply_inputs(in, f);
        CHECK_EQ(in.size(), 1u);
        if (in.size() == 1) {
            CHECK_EQ(in[0].mac, kEvolve);
            CHECK_EQ(in[0].kind, "headset");
            CHECK_EQ(in[0].hint_profile, "music");
        }
    }
    /* the card's profile wins over the launcher's */
    {
        std::vector<BtInput> in = {bt_in(kEvolve, "Evolve 65", "headset", "call")};
        bt_facts_apply_inputs(in, f);
        CHECK_EQ(in[0].hint_profile, "call");
        std::vector<BtOutput> out = {bt_out(kEvolve, "Evolve 65", "headset", "call")};
        bt_facts_apply_outputs(out, f);
        CHECK_EQ(out[0].hint_profile, "call");
    }
    /* hfp from the launcher is a call hint */
    {
        LauncherFacts g = f;
        g.devices = parse_bt_audio_dev("70_BF_92_53_6B_8B:headset:1:hfp");
        std::vector<BtOutput> out = {bt_out(kEvolve, "Evolve 65", "other")};
        bt_facts_apply_outputs(out, g);
        CHECK_EQ(out[0].hint_profile, "call");
        CHECK_EQ(out[0].kind, "headset");
    }
    /* outputs: the speaker keeps its place with the launcher's kind; a device the launcher does not know is untouched */
    {
        std::vector<BtOutput> out = {bt_out(kSpeaker, "XWF Speaker", "headset"), bt_out("01:02:03:04:05:06", "Other", "other")};
        bt_facts_apply_outputs(out, f);
        CHECK_EQ(out.size(), 2u);
        CHECK_EQ(out[0].kind, "speaker");
        CHECK_EQ(out[0].hint_profile, "unknown"); /* no profile in the facts */
        CHECK_EQ(out[1].kind, "other");
    }
    /* without status facts nothing changes, and no paired rows appear */
    {
        LauncherFacts none;
        std::vector<BtInput> in = {bt_in(kSpeaker, "XWF Speaker", "headset")};
        bt_facts_apply_inputs(in, none);
        CHECK_EQ(in.size(), 1u);
        CHECK_EQ(in[0].kind, "headset");
        CHECK(bt_paired_inputs(in, none, {}).empty());
        CHECK(bt_paired_outputs({}, none, {}).empty());
    }
    /* paired, not connected: inputs list the headsets only; outputs headset, headphones and speaker; an unknown kind is left out */
    {
        const std::vector<BtInput> listed_in = {bt_in(kEvolve, "Evolve 65", "headset"), bt_in(kSpeaker, "XWF Speaker", "headset")};
        const std::vector<BtOutput> listed_out = {bt_out(kEvolve, "Evolve 65", "headset"), bt_out(kSpeaker, "XWF Speaker", "speaker")};
        std::map<std::string, std::string> names = {{kBeats, "Beats Fit Pro"}};
        const std::vector<BtInput> pi = bt_paired_inputs(listed_in, f, names);
        CHECK_EQ(pi.size(), 1u);
        if (pi.size() == 1) {
            CHECK_EQ(pi[0].mac, kBeats);
            CHECK_EQ(pi[0].id, "bt:F8:66:5A:18:D3:69");
            CHECK_EQ(pi[0].label, "Beats Fit Pro");
            CHECK_EQ(pi[0].kind, "headset");
            CHECK(!pi[0].connected);
            CHECK(!pi[0].usable);
            CHECK(pi[0].paired);
            CHECK_EQ(pi[0].reason, "Not connected: connect it in Settings > Bluetooth");
            CHECK_EQ(pi[0].rate, 0u);
        }
        const std::vector<BtOutput> po = bt_paired_outputs(listed_out, f, names);
        CHECK_EQ(po.size(), 3u);
        if (po.size() == 3) {
            CHECK_EQ(po[0].mac, kBeats);
            CHECK_EQ(po[0].label, "Beats Fit Pro");
            CHECK_EQ(po[1].mac, "11:22:33:44:55:66");
            CHECK_EQ(po[1].label, "Bluetooth device 55:66"); /* no remembered name: the end of the address */
            CHECK_EQ(po[1].kind, "speaker");
            CHECK_EQ(po[2].kind, "headphones");
            for (const BtOutput &b : po) {
                CHECK(!b.connected && !b.usable && b.paired);
                CHECK_EQ(b.reason, kBtPairedReason);
            }
        }
        /* a paired device the server lists (the facts lag): not a second row */
        const std::vector<BtOutput> listed2 = {bt_out(kBeats, "Beats Fit Pro", "headset")};
        CHECK_EQ(bt_paired_outputs(listed2, f, names).size(), 2u);
    }
    CHECK_EQ(bt_generic_name(kEvolve), "Bluetooth device 6B:8B");
}

}  // namespace

void test_launcher_facts() {
    test_parser();
    test_provider();
    test_merge();
}
