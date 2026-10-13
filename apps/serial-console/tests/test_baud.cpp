/*
 * SPDX-License-Identifier: MIT
 *
 * Tests of the auto-baud scorer and the detection walk (core/baud), with synthetic byte streams.
 */
#include "check.hpp"

#include "baud.hpp"

#include <cstdint>

using namespace sc;

namespace {

std::string noise(std::uint32_t seed, std::size_t size)
{
    std::string out;
    for (std::size_t i = 0; i < size; ++i) {
        seed = seed * 1103515245u + 12345u;
        out += static_cast<char>((seed >> 16) & 0xFF);
    }
    return out;
}

/* What a UART reading 8N1 text at the wrong speed mostly gives: runs of NUL, 0xFF, 0x80, 0xF8 and a few letters. */
std::string wrong_speed()
{
    std::string out;
    const char pattern[] = {'\x00', '\xf8', '\x80', '\xff', 'x', '\x00', '\xe0', '\xfe', '\x1c', '\x80'};
    for (int i = 0; i < 12; ++i) out.append(pattern, sizeof(pattern));
    return out;
}

} // namespace

void test_baud()
{
    const std::string text = "I (312) cpu_start: Starting scheduler on PRO CPU.\r\nTemperature: 21.5 C\r\n";
    TextScore s = score_text(text);
    CHECK(s.score > 0.99);
    CHECK_EQ(s.line_ends, static_cast<std::size_t>(4));
    CHECK(looks_clean(s));

    CHECK(looks_clean(score_text("\x1b[0;32mI (29) boot: ESP-IDF v5.1.2\x1b[0m\r\n")));
    CHECK(looks_clean(score_text("Temp\xc3\xa9rature: 21 \xc2\xb0" "C\r\n")));
    CHECK(looks_clean(score_text("I (600) app: city=Montr\xe9" "al, zone 2, everything else fine here\r\n")));

    const TextScore n = score_text(noise(42, 400));
    CHECK(n.score < 0.2);
    CHECK(!looks_clean(n));
    CHECK(!looks_clean(score_text(wrong_speed())));
    CHECK_EQ(score_text(std::string(64, '\0')).score, 0.0);
    CHECK_EQ(score_text(std::string(64, '\xff')).score, 0.0);
    CHECK(!looks_clean(score_text("ok\r\n")));               /* too little to judge */
    CHECK(!looks_clean(score_text("\r\n\r\n\r\n\r\n\r\n"))); /* no characters */
    CHECK(!looks_clean(score_text("")));
    CHECK(!looks_clean(score_text(text.substr(0, 40) + noise(3, 12)))); /* text with a burst of noise */

    // the order of the speeds
    const std::vector<int> o = detect_order(115200);
    CHECK(o == (std::vector<int>{115200, 9600, 57600, 38400, 19200, 230400, 460800, 921600, 4800, 2400, 1200}));
    const std::vector<int> o9 = detect_order(9600);
    CHECK(o9.size() == 11 && o9[0] == 9600 && o9[1] == 115200);
    const std::vector<int> o74 = detect_order(74880);
    CHECK(o74.size() == 12 && o74[0] == 74880 && o74[1] == 115200);

    // a board at 57600, recommended 115200: found at the third speed, nothing more is tried
    {
        Detector d(115200, false);
        CHECK_EQ(d.progress_text(), std::string("Trying 115200... (1 of 11)"));
        int listens = 0;
        while (!d.done()) {
            ++listens;
            d.record(d.rate() == 57600 ? text : wrong_speed());
            if (listens == 1) CHECK_EQ(d.progress_text(), std::string("Trying 9600... (2 of 11)"));
        }
        CHECK_EQ(listens, 3);
        const DetectResult r = d.result();
        CHECK(r.found && r.baud == 57600);
        CHECK_EQ(r.message, std::string("Found 57600 - text looks clean"));
        d.record(text); /* ignored once done */
        CHECK(d.result().baud == 57600);
    }
    // a silent board: every speed once, then the advice; with the Enter pass, every speed twice
    {
        Detector d(115200, false);
        int listens = 0;
        while (!d.done()) {
            CHECK(!d.send_enter());
            d.record("");
            ++listens;
        }
        CHECK_EQ(listens, 11);
        const DetectResult r = d.result();
        CHECK(!r.found && r.baud == 0);
        CHECK_EQ(r.message, std::string("No readable text at any speed; the board may be silent: try Connect and "
                                        "press its reset button"));
    }
    {
        Detector d(9600, true);
        int listens = 0, with_enter = 0;
        while (!d.done()) {
            if (d.send_enter()) {
                ++with_enter;
                CHECK(d.progress_text().find("with Enter") != std::string::npos);
            }
            d.record("");
            ++listens;
        }
        CHECK_EQ(listens, 22);
        CHECK_EQ(with_enter, 11);
        CHECK(!d.result().found);
    }
    // a router that only answers Enter (its login prompt), at 9600
    {
        Detector d(115200, true);
        while (!d.done()) d.record(d.send_enter() && d.rate() == 9600 ? std::string("\r\nrouter login: \r\n") : "");
        CHECK(d.result().found && d.result().baud == 9600 && d.pass() == 2);
    }
    // only noise everywhere: not found, the best speed with data is reported
    {
        Detector d(115200, false);
        std::uint32_t seed = 1;
        while (!d.done()) d.record(noise(seed++, 100));
        CHECK(!d.result().found);
        CHECK(d.result().baud != 0);
    }
    {
        Detector d(115200, false);
        d.record("");
        d.cancel();
        CHECK(d.done() && d.cancelled());
        CHECK_EQ(d.result().message, std::string("Stopped"));
    }
}
