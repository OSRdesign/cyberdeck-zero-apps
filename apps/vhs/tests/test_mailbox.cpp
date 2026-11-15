/*
 * SPDX-License-Identifier: MIT
 *
 * VHS tests: the latest-item mailbox (core/mailbox.hpp): newest wins, older items are dropped and counted, the
 * producer never waits for a slow consumer.
 */
#include "check.hpp"

#include "live/clock.hpp"
#include "live/mailbox.hpp"

#include <atomic>
#include <thread>

using namespace vhs;

namespace {

struct Item {
    int n = 0;
    std::vector<int> payload;
};

void basic()
{
    LatestMailbox<Item> box;
    CHECK(!box.take());
    box.back().n = 1;
    box.publish();
    CHECK_EQ(box.published(), 1ull);
    CHECK_EQ(box.drops(), 0ull);
    CHECK(box.take());
    CHECK_EQ(box.front().n, 1);
    CHECK(!box.take());           /* nothing new */
    CHECK_EQ(box.front().n, 1);   /* front stays valid */

    /* two publishes before a take: the older one is dropped, the newest is taken */
    box.back().n = 2;
    box.publish();
    box.back().n = 3;
    box.publish();
    CHECK_EQ(box.drops(), 1ull);
    CHECK(box.take());
    CHECK_EQ(box.front().n, 3);
    CHECK(!box.take());

    /* clear: a waiting item goes, not counted as a drop */
    box.back().n = 4;
    box.publish();
    box.clear();
    CHECK(!box.take());
    CHECK_EQ(box.drops(), 1ull);
    CHECK_EQ(box.published(), 4ull);
}

void buffers_are_reused()
{
    /* three items rotate: the producer's buffer keeps its capacity (no allocation per item) */
    LatestMailbox<Item> box;
    for (int i = 0; i < 3; ++i) {
        box.back().payload.reserve(1000);
        box.publish();
        box.take();
    }
    int with_capacity = 0;
    for (int i = 0; i < 6; ++i) {
        if (box.back().payload.capacity() >= 1000) ++with_capacity;
        box.publish();
        box.take();
    }
    CHECK_EQ(with_capacity, 6);
}

void wait_and_wake()
{
    LatestMailbox<Item> box;
    const std::int64_t t0 = mono_us();
    CHECK(!box.wait_take(50));
    const std::int64_t waited = mono_us() - t0;
    CHECK(waited >= 40000);
    CHECK(waited < 1000000);

    std::thread producer([&box] {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        box.back().n = 7;
        box.publish();
    });
    CHECK(box.wait_take(2000));
    CHECK_EQ(box.front().n, 7);
    producer.join();

    std::thread waker([&box] {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        box.wake();
    });
    const std::int64_t t1 = mono_us();
    CHECK(!box.wait_take(5000)); /* woken, nothing there */
    CHECK(mono_us() - t1 < 2000000);
    waker.join();
}

void slow_consumer()
{
    /* the producer publishes 2000 items as fast as it can; the consumer takes one every ms: it sees increasing numbers,
     * the producer is never held up, and published = taken + dropped (+ at most one waiting) */
    LatestMailbox<Item> box;
    std::atomic<bool> done{false};
    std::int64_t worst_publish_us = 0;
    std::thread producer([&] {
        for (int i = 1; i <= 2000; ++i) {
            box.back().n = i;
            const std::int64_t t = mono_us();
            box.publish();
            worst_publish_us = std::max(worst_publish_us, mono_us() - t);
            if (i % 50 == 0) std::this_thread::sleep_for(std::chrono::microseconds(200));
        }
        done = true;
    });
    int last = 0, taken = 0;
    bool increasing = true;
    auto consume = [&] {
        if (!box.take()) return;
        if (box.front().n <= last) increasing = false;
        last = box.front().n;
        ++taken;
    };
    while (!done) {
        consume();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    producer.join();
    consume();
    CHECK(increasing);
    CHECK_EQ(last, 2000);
    CHECK(box.drops() > 0);
    CHECK_EQ(box.drops() + static_cast<std::uint64_t>(taken), 2000ull); /* every item taken or dropped, none lost */
    CHECK(taken > 1);
    CHECK(worst_publish_us < 200000); /* a lock held only for a swap (generous: sanitizers, a busy box) */
}

} // namespace

void test_mailbox()
{
    basic();
    buffers_are_reused();
    wait_and_wake();
    slow_consumer();
}
