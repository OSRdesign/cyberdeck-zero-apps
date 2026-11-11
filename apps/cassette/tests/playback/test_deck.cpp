/* SPDX-License-Identifier: MIT */
/*
 * The Deck: a table of EVERY (state, event) pair with the next state, the commands and whether the event was consumed.
 * The table is indexed [scenario][event]; each cell names its event, so a missing or misplaced cell fails at run time, and
 * the array sizes come from the enum counts, so a new state or event that the table does not cover fails as well.
 */
#include "check.hpp"
#include "deck.hpp"
#include "support.hpp"

#include <array>
#include <set>
#include <string>
#include <vector>

using namespace cassette;
using namespace tp;

namespace {

using C = DeckCommand;
using E = DeckEvent;
using S = DeckState;
constexpr Direction F = Direction::Forward, R = Direction::Reverse;

struct Rec : DeckCommands {
    std::vector<DeckCommand> v;
    void command(const DeckCommand &c) override { v.push_back(c); }
};

DeckCommand k(C::Kind kind, Direction d = F, unsigned s = 0) {
    DeckCommand c;
    c.kind = kind;
    c.dir = d;
    c.seconds = s;
    return c;
}

std::string text(const std::vector<DeckCommand> &v) {
    std::string s = "[";
    for (const auto &c : v) {
        if (s.size() > 1) s += ", ";
        s += deck_command_name(c.kind);
        if (c.kind == C::BeginScan || c.kind == C::Jump) s += c.dir == F ? "(F)" : "(R)";
        if (c.kind == C::StartCountdown) s += "(" + std::to_string(c.seconds) + ")";
    }
    return s + "]";
}

/* Scenarios: a way to reach each state (some states twice: the scan's return target, a countdown at zero). */
enum Scn {
    IdleTake, IdleNoTake, CountdownRun, CountdownDue, Recording, RecordingPaused, Playing, PlayingPaused,
    ScanFwdPlay, ScanFwdPause, ScanRevPlay, ScanRevPause, kScn
};
const char *kScnName[kScn] = {"IdleTake", "IdleNoTake", "CountdownRun", "CountdownDue", "Recording", "RecordingPaused",
                              "Playing", "PlayingPaused", "ScanFwdPlay", "ScanFwdPause", "ScanRevPlay", "ScanRevPause"};

struct Fixture {
    FakeClock clk;
    Rec rec;
    Deck deck{clk, rec};
};

/* Brings the deck into the scenario with FastScan, timer Off (the countdown scenarios set 3 s to enter, then Off). */
S setup(Fixture &f, Scn s) {
    Deck &d = f.deck;
    d.set_rew_fwd_mode(RewFwdMode::FastScan);
    d.set_has_last_take(s != IdleNoTake);
    switch (s) {
    case IdleTake:
    case IdleNoTake: break;
    case CountdownRun:
    case CountdownDue:
        d.set_record_timer(RecordTimer::S3);
        d.handle(E::Rec);
        d.set_record_timer(RecordTimer::Off);
        if (s == CountdownDue) f.clk.ms += 3000;
        break;
    case Recording: d.handle(E::Rec); break;
    case RecordingPaused:
        d.handle(E::Rec);
        d.handle(E::PlayPause);
        break;
    case Playing: d.handle(E::PlayPause); break;
    case PlayingPaused:
        d.handle(E::PlayPause);
        d.handle(E::PlayPause);
        break;
    case ScanFwdPlay:
    case ScanRevPlay:
        d.handle(E::PlayPause);
        d.handle(s == ScanFwdPlay ? E::FwdDown : E::RewDown);
        break;
    case ScanFwdPause:
    case ScanRevPause:
        d.handle(E::PlayPause);
        d.handle(E::PlayPause);
        d.handle(s == ScanFwdPause ? E::FwdDown : E::RewDown);
        break;
    case kScn: break;
    }
    f.rec.v.clear();
    return d.state();
}

struct Cell {
    E ev = E::kCount; /* kCount: the cell was never filled */
    S next = S::kCount;
    std::vector<DeckCommand> cmds;
    bool consumed = false;
};

constexpr size_t kEv = static_cast<size_t>(E::kCount);
using Row = std::array<Cell, kEv>;

Cell c(E e, S n, std::vector<DeckCommand> cmds, bool consumed) {
    Cell x;
    x.ev = e;
    x.next = n;
    x.cmds = std::move(cmds);
    x.consumed = consumed;
    return x;
}

/* A row where nothing happens except the listed overrides. */
Row quiet(S self) {
    Row r;
    for (size_t i = 0; i < kEv; ++i) r[i] = c(static_cast<E>(i), self, {}, false);
    return r;
}
void set(Row &r, Cell x) { r[static_cast<size_t>(x.ev)] = std::move(x); }

std::array<Row, kScn> build_table() {
    std::array<Row, kScn> t;
    /* Idle with a last take */
    t[IdleTake] = quiet(S::Idle);
    set(t[IdleTake], c(E::Rec, S::Recording, {k(C::StartRecording)}, true));
    set(t[IdleTake], c(E::PlayPause, S::Playing, {k(C::PlayLast)}, true));
    set(t[IdleTake], c(E::RewDown, S::ScanningReverse, {k(C::CueLast), k(C::BeginScan, R)}, true));
    set(t[IdleTake], c(E::FwdDown, S::ScanningForward, {k(C::CueLast), k(C::BeginScan, F)}, true));
    /* Idle without a take: only REC does something */
    t[IdleNoTake] = quiet(S::Idle);
    set(t[IdleNoTake], c(E::Rec, S::Recording, {k(C::StartRecording)}, true));
    /* Countdown, not yet at zero */
    for (Scn s : {CountdownRun, CountdownDue}) {
        t[s] = quiet(S::Countdown);
        for (E e : {E::Stop, E::Esc, E::TapOverlay, E::AppLeave}) set(t[s], c(e, S::Idle, {k(C::CancelCountdown)}, true));
        set(t[s], c(E::DeviceLost, S::Idle, {k(C::CancelCountdown), k(C::DeviceLostNotice)}, true));
        set(t[s], c(E::TimerTick, S::Countdown, {}, true));
    }
    set(t[CountdownDue], c(E::TimerTick, S::Recording, {k(C::StartRecording)}, true));
    /* Recording and paused recording: REW/FWD disabled, REC latched (ignored) */
    t[Recording] = quiet(S::Recording);
    set(t[Recording], c(E::Stop, S::Idle, {k(C::StopRecording)}, true));
    set(t[Recording], c(E::AppLeave, S::Idle, {k(C::StopRecording)}, true));
    set(t[Recording], c(E::PlayPause, S::RecordingPaused, {k(C::PauseRecording)}, true));
    set(t[Recording], c(E::DeviceLost, S::Idle, {k(C::StopRecording), k(C::DeviceLostNotice)}, true));
    t[RecordingPaused] = quiet(S::RecordingPaused);
    set(t[RecordingPaused], c(E::Stop, S::Idle, {k(C::StopRecording)}, true));
    set(t[RecordingPaused], c(E::AppLeave, S::Idle, {k(C::StopRecording)}, true));
    set(t[RecordingPaused], c(E::PlayPause, S::Recording, {k(C::ResumeRecording)}, true));
    set(t[RecordingPaused], c(E::DeviceLost, S::Idle, {k(C::StopRecording), k(C::DeviceLostNotice)}, true));
    /* Playing and paused playback */
    for (Scn s : {Playing, PlayingPaused}) {
        const S self = s == Playing ? S::Playing : S::PlayingPaused;
        t[s] = quiet(self);
        set(t[s], c(E::Rec, S::Recording, {k(C::StopPlayback), k(C::StartRecording)}, true));
        set(t[s], c(E::Stop, S::Idle, {k(C::StopPlayback)}, true));
        set(t[s], c(E::AppLeave, S::Idle, {k(C::StopPlayback)}, true));
        set(t[s], c(E::RewDown, S::ScanningReverse, {k(C::BeginScan, R)}, true));
        set(t[s], c(E::FwdDown, S::ScanningForward, {k(C::BeginScan, F)}, true));
        set(t[s], c(E::RewUp, self, {}, true));
        set(t[s], c(E::FwdUp, self, {}, true));
        set(t[s], c(E::TimerTick, self, {}, true));
        set(t[s], c(E::TakeEnded, S::Idle, {}, true));
    }
    set(t[Playing], c(E::PlayPause, S::PlayingPaused, {k(C::PausePlayback)}, true));
    set(t[Playing], c(E::DeviceLost, S::PlayingPaused, {k(C::PausePlayback), k(C::DeviceLostNotice)}, true));
    set(t[PlayingPaused], c(E::PlayPause, S::Playing, {k(C::ResumePlayback)}, true));
    set(t[PlayingPaused], c(E::DeviceLost, S::PlayingPaused, {}, true));
    /* Scanning (held REW or FWD) */
    for (Scn s : {ScanFwdPlay, ScanFwdPause, ScanRevPlay, ScanRevPause}) {
        const bool fwd = s == ScanFwdPlay || s == ScanFwdPause;
        const bool from_pause = s == ScanFwdPause || s == ScanRevPause;
        const S self = fwd ? S::ScanningForward : S::ScanningReverse;
        const S back = from_pause ? S::PlayingPaused : S::Playing;
        t[s] = quiet(self);
        set(t[s], c(E::Rec, S::Recording, {k(C::StopPlayback), k(C::StartRecording)}, true));
        set(t[s], c(E::Stop, S::Idle, {k(C::StopPlayback)}, true));
        set(t[s], c(E::AppLeave, S::Idle, {k(C::StopPlayback)}, true));
        set(t[s], c(E::PlayPause, from_pause ? S::Playing : S::PlayingPaused,
                    {k(C::EndScan), k(from_pause ? C::ResumePlayback : C::PausePlayback)}, true));
        for (E e : {E::RewDown, E::RewUp, E::FwdDown, E::FwdUp, E::TimerTick}) set(t[s], c(e, self, {}, true));
        set(t[s], c(fwd ? E::FwdUp : E::RewUp, back, {k(C::EndScan)}, true));
        set(t[s], c(E::TakeEnded, S::Idle, {}, true));
        set(t[s], c(E::DeviceLost, S::PlayingPaused, {k(C::PausePlayback), k(C::DeviceLostNotice)}, true));
    }
    return t;
}

S scenario_state(Scn s) {
    switch (s) {
    case IdleTake:
    case IdleNoTake: return S::Idle;
    case CountdownRun:
    case CountdownDue: return S::Countdown;
    case Recording: return S::Recording;
    case RecordingPaused: return S::RecordingPaused;
    case Playing: return S::Playing;
    case PlayingPaused: return S::PlayingPaused;
    case ScanFwdPlay:
    case ScanFwdPause: return S::ScanningForward;
    case ScanRevPlay:
    case ScanRevPause: return S::ScanningReverse;
    case kScn: break;
    }
    return S::kCount;
}

void table_test() {
    const auto table = build_table();
    std::set<int> covered;
    int cells = 0;
    for (int s = 0; s < kScn; ++s) {
        for (size_t e = 0; e < kEv; ++e) {
            const Cell &want = table[s][e];
            if (want.ev != static_cast<E>(e)) {
                std::fprintf(stderr, "table: %s x event %zu missing or misplaced\n", kScnName[s], e);
                CHECK(false);
                continue;
            }
            Fixture f;
            const S start = setup(f, static_cast<Scn>(s));
            CHECK(start == scenario_state(static_cast<Scn>(s)));
            covered.insert(static_cast<int>(start));
            const bool consumed = f.deck.handle(want.ev);
            const bool ok = f.deck.state() == want.next && f.rec.v == want.cmds && consumed == want.consumed;
            if (!ok)
                std::fprintf(stderr, "table: %s + %s -> %s %s consumed=%d; want %s %s consumed=%d\n", kScnName[s],
                             deck_event_name(want.ev), deck_state_name(f.deck.state()), text(f.rec.v).c_str(), consumed,
                             deck_state_name(want.next), text(want.cmds).c_str(), want.consumed);
            CHECK(ok);
            ++cells;
        }
    }
    CHECK_EQ(cells, kScn * static_cast<int>(kEv));
    CHECK_EQ(covered.size(), static_cast<size_t>(S::kCount)); /* every DeckState reached by a scenario */
}

void timer_and_mode_tests() {
    /* REC with a timer from Idle and from playback */
    {
        Fixture f;
        f.deck.set_has_last_take(true);
        f.deck.set_record_timer(RecordTimer::S10);
        f.deck.handle(E::Rec);
        CHECK(f.rec.v == std::vector<DeckCommand>{k(C::StartCountdown, F, 10)});
        CHECK(f.deck.state() == S::Countdown);
        Fixture g;
        g.deck.set_has_last_take(true);
        g.deck.set_record_timer(RecordTimer::S3);
        g.deck.handle(E::PlayPause);
        g.rec.v.clear();
        g.deck.handle(E::Rec);
        CHECK(g.rec.v == (std::vector<DeckCommand>{k(C::StopPlayback), k(C::StartCountdown, F, 3)}));
        CHECK(g.deck.state() == S::Countdown);
    }
    /* jump 5 s: each press one jump; held, one more every 300 ms; release stops it */
    {
        Fixture f;
        f.deck.set_has_last_take(true);
        f.deck.set_rew_fwd_mode(RewFwdMode::Jump5s);
        f.deck.handle(E::PlayPause);
        f.rec.v.clear();
        f.deck.handle(E::FwdDown);
        CHECK(f.rec.v == std::vector<DeckCommand>{k(C::Jump, F)});
        CHECK(f.deck.state() == S::Playing);
        CHECK(f.deck.key_active(DeckKey::Fwd));
        f.deck.handle(E::RewDown); /* the other key while one is held: ignored */
        CHECK_EQ(f.rec.v.size(), 1u);
        for (int i = 0; i < 20; ++i) { /* 1000 ms held, ticks every 50 ms */
            f.clk.ms += 50;
            f.deck.handle(E::TimerTick);
        }
        CHECK_EQ(f.rec.v.size(), 1u + 3u); /* at 300, 600, 900 ms */
        f.deck.handle(E::FwdUp);
        CHECK(!f.deck.key_active(DeckKey::Fwd));
        for (int i = 0; i < 20; ++i) {
            f.clk.ms += 50;
            f.deck.handle(E::TimerTick);
        }
        CHECK_EQ(f.rec.v.size(), 4u);
        f.deck.handle(E::RewDown); /* a tap */
        f.deck.handle(E::RewUp);
        CHECK_EQ(f.rec.v.size(), 5u);
        CHECK(f.rec.v.back() == k(C::Jump, R));
        /* paused: jumps keep it paused */
        f.deck.handle(E::PlayPause);
        f.rec.v.clear();
        f.deck.handle(E::RewDown);
        CHECK(f.rec.v == std::vector<DeckCommand>{k(C::Jump, R)});
        CHECK(f.deck.state() == S::PlayingPaused);
        f.deck.handle(E::Stop); /* stop clears the hold */
        f.clk.ms += 1000;
        f.rec.v.clear();
        f.deck.handle(E::TimerTick);
        CHECK(f.rec.v.empty());
    }
    /* jump from Idle acts on the last take: cued, paused */
    {
        Fixture f;
        f.deck.set_has_last_take(true);
        f.deck.set_rew_fwd_mode(RewFwdMode::Jump5s);
        f.deck.handle(E::FwdDown);
        CHECK(f.rec.v == (std::vector<DeckCommand>{k(C::CueLast), k(C::Jump, F)}));
        CHECK(f.deck.state() == S::PlayingPaused);
        f.deck.handle(E::FwdUp);
        f.rec.v.clear();
        f.deck.handle(E::PlayPause);
        CHECK(f.rec.v == std::vector<DeckCommand>{k(C::ResumePlayback)});
    }
    /* REW/FWD disabled while recording or counting down, in both modes */
    for (RewFwdMode m : {RewFwdMode::FastScan, RewFwdMode::Jump5s}) {
        Fixture f;
        f.deck.set_has_last_take(true);
        f.deck.set_rew_fwd_mode(m);
        f.deck.handle(E::Rec);
        f.rec.v.clear();
        for (E e : {E::RewDown, E::RewUp, E::FwdDown, E::FwdUp}) f.deck.handle(e);
        f.clk.ms += 2000;
        f.deck.handle(E::TimerTick);
        CHECK(f.rec.v.empty());
        CHECK(!f.deck.key_enabled(DeckKey::Rew));
        CHECK(!f.deck.key_enabled(DeckKey::Fwd));
    }
    /* a take recorded makes PLAY/PAUSE play it */
    {
        Fixture f;
        CHECK(!f.deck.key_enabled(DeckKey::PlayPause));
        f.deck.handle(E::Rec);
        f.deck.handle(E::Stop);
        CHECK(f.deck.has_last_take());
        CHECK(f.deck.key_enabled(DeckKey::PlayPause));
    }
}

void key_view_tests() {
    struct Want {
        Scn s;
        bool rew, play, fwd, stop, rec; /* enabled */
        bool rec_latched, pause_icon;
    } wants[] = {
        {IdleTake, true, true, true, false, true, false, false},
        {IdleNoTake, false, false, false, false, true, false, false},
        {CountdownRun, false, false, false, true, false, true, false},
        {Recording, false, true, false, true, false, true, true},
        {RecordingPaused, false, true, false, true, false, true, false},
        {Playing, true, true, true, true, true, false, true},
        {PlayingPaused, true, true, true, true, true, false, false},
        {ScanFwdPlay, true, true, true, true, true, false, true},
        {ScanRevPause, true, true, true, true, true, false, true},
    };
    for (const Want &w : wants) {
        Fixture f;
        setup(f, w.s);
        const Deck &d = f.deck;
        const bool ok = d.key_enabled(DeckKey::Rew) == w.rew && d.key_enabled(DeckKey::PlayPause) == w.play &&
                        d.key_enabled(DeckKey::Fwd) == w.fwd && d.key_enabled(DeckKey::Stop) == w.stop &&
                        d.key_enabled(DeckKey::Rec) == w.rec && d.rec_latched() == w.rec_latched &&
                        d.key_active(DeckKey::Rec) == w.rec_latched && d.shows_pause_icon() == w.pause_icon &&
                        d.key_active(DeckKey::PlayPause) == w.pause_icon;
        if (!ok) std::fprintf(stderr, "keys: %s\n", kScnName[w.s]);
        CHECK(ok);
    }
    Fixture f;
    setup(f, ScanRevPlay);
    CHECK(f.deck.key_active(DeckKey::Rew));
    CHECK(!f.deck.key_active(DeckKey::Fwd));
    CHECK(!f.deck.scan_returns_to_pause());
    CHECK(!f.deck.key_active(DeckKey::Stop));
}

} // namespace

void test_deck() {
    table_test();
    timer_and_mode_tests();
    key_view_tests();
}
