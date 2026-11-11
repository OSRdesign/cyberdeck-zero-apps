/* SPDX-License-Identifier: MIT */
#include "check.hpp"
#include "countdown.hpp"
#include "deck.hpp"
#include "support.hpp"

#include <vector>

using namespace cassette;
using namespace tp;

namespace {

struct Rec : DeckCommands {
    std::vector<DeckCommand> v;
    void command(const DeckCommand &c) override { v.push_back(c); }
    size_t count(DeckCommand::Kind k) const {
        size_t n = 0;
        for (auto &c : v) n += c.kind == k;
        return n;
    }
};

} // namespace

void test_countdown() {
    /* 3, 10, 20 s on a fake clock: the number shown, the ring fraction, Fired exactly once */
    for (RecordTimer t : {RecordTimer::S3, RecordTimer::S10, RecordTimer::S20}) {
        const unsigned n = timer_seconds(t);
        FakeClock clk;
        Countdown cd(clk);
        CHECK(cd.tick() == Countdown::Tick::Idle);
        CHECK(cd.start(n));
        CHECK(cd.active());
        CHECK_EQ(cd.total_seconds(), n);
        CHECK_EQ(cd.remaining().seconds, n);
        CHECK(cd.remaining().fraction == 1.0f);
        unsigned fired = 0;
        unsigned last_shown = n + 1;
        bool monotonic = true;
        for (unsigned ms = 0; ms <= n * 1000 + 500; ms += 50) {
            Countdown::Remaining r = cd.remaining();
            if (cd.active()) {
                if (r.seconds > last_shown) monotonic = false;
                last_shown = r.seconds;
                if (ms < n * 1000) {
                    CHECK_EQ(r.seconds, (n * 1000 - ms + 999) / 1000);
                    CHECK(r.fraction > 0.f && r.fraction <= 1.f);
                }
            }
            Countdown::Tick k = cd.tick();
            if (k == Countdown::Tick::Fired) {
                ++fired;
                CHECK_EQ(clk.ms - 1000, static_cast<uint64_t>(n) * 1000); /* at zero, not before */
            }
            if (ms < n * 1000) CHECK(k == Countdown::Tick::Running);
            clk.ms += 50;
        }
        CHECK_EQ(fired, 1u);
        CHECK(monotonic);
        CHECK(!cd.active());
        CHECK_EQ(cd.remaining().seconds, 0u);
        CHECK(cd.tick() == Countdown::Tick::Idle);
        /* shown 1 just before zero */
        CHECK(cd.start(n));
        clk.ms += n * 1000 - 1;
        CHECK_EQ(cd.remaining().seconds, 1u);
        CHECK(cd.remaining().fraction < 0.01f);
        cd.cancel();
        CHECK(!cd.active());
        clk.ms += 5000;
        CHECK(cd.tick() == Countdown::Tick::Idle); /* a cancelled countdown never fires */
    }
    /* Off: no countdown */
    {
        FakeClock clk;
        Countdown cd(clk);
        CHECK(!cd.start(timer_seconds(RecordTimer::Off)));
        CHECK(!cd.active());
        CHECK(cd.tick() == Countdown::Tick::Idle);
        Rec r;
        Deck deck(clk, r);
        deck.set_record_timer(RecordTimer::Off);
        deck.handle(DeckEvent::Rec);
        CHECK(deck.state() == DeckState::Recording);
        CHECK(r.v.size() == 1 && r.v[0].kind == DeckCommand::StartRecording);
    }
    /* through the deck: cancelled by STOP, ESC, TAP_OVERLAY, APP_LEAVE (nothing written: no StartRecording ever) */
    for (DeckEvent cancel : {DeckEvent::Stop, DeckEvent::Esc, DeckEvent::TapOverlay, DeckEvent::AppLeave}) {
        for (RecordTimer t : {RecordTimer::S3, RecordTimer::S10, RecordTimer::S20}) {
            FakeClock clk;
            Rec r;
            Deck deck(clk, r);
            deck.set_record_timer(t);
            CHECK(deck.handle(DeckEvent::Rec));
            CHECK(deck.state() == DeckState::Countdown);
            CHECK(r.v.size() == 1 && r.v[0].kind == DeckCommand::StartCountdown && r.v[0].seconds == timer_seconds(t));
            CHECK(deck.rec_latched());
            CHECK(!deck.key_enabled(DeckKey::Rew) && !deck.key_enabled(DeckKey::Fwd));
            clk.ms += 1000;
            deck.handle(DeckEvent::TimerTick);
            CHECK_EQ(deck.countdown().remaining().seconds, timer_seconds(t) - 1);
            CHECK(deck.handle(cancel));
            CHECK(deck.state() == DeckState::Idle);
            CHECK_EQ(r.count(DeckCommand::CancelCountdown), 1u);
            for (int i = 0; i < 50; ++i) {
                clk.ms += 1000;
                deck.handle(DeckEvent::TimerTick);
            }
            CHECK_EQ(r.count(DeckCommand::StartRecording), 0u);
        }
    }
    /* through the deck: fires once at zero */
    {
        FakeClock clk;
        Rec r;
        Deck deck(clk, r);
        deck.set_record_timer(RecordTimer::S10);
        deck.handle(DeckEvent::Rec);
        for (int i = 0; i < 400; ++i) {
            clk.ms += 50;
            deck.handle(DeckEvent::TimerTick);
            if (clk.ms - 1000 < 10000) CHECK(deck.state() == DeckState::Countdown);
        }
        CHECK(deck.state() == DeckState::Recording);
        CHECK_EQ(r.count(DeckCommand::StartRecording), 1u);
        CHECK(!deck.countdown().active());
    }
}
