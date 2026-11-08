/* SPDX-License-Identifier: MIT */
#include <cstdio>

#include "capture.hpp"
#include "card.hpp"
#include "check.hpp"
#include "open_policy.hpp"

using namespace cassette;

namespace {

struct CountSink : FrameSink {
    size_t n = 0;
    size_t write(const void *, size_t k) override {
        n += k;
        return k;
    }
};

} // namespace

/* Real ALSA smoke test: listen-only, 0.5 s. SKIPPED when libasound.so.2 or a capture card is missing (or the card is busy
 * and the server route does not open either). Never writes a file and never touches the mixer. */
void test_real() {
    std::unique_ptr<AlsaApi> api = make_real_alsa();
    std::string why;
    bool lib = api->available(&why);
    /* card listing works without the library and must not crash on a machine without /proc/asound */
    std::vector<CaptureCard> cards = list_capture_cards(*api, false);
    CHECK(true);
    if (!lib) {
        std::printf("SKIP real ALSA smoke: %s\n", why.c_str());
        ++g_skip;
        return;
    }
    if (cards.empty()) {
        std::printf("SKIP real ALSA smoke: libasound loaded, no capture card present\n");
        ++g_skip;
        return;
    }
    const CardInfo &card = cards[0].info;
    OpenResult o = open_capture(*api, card);
    if (!o.ok()) {
        std::printf("SKIP real ALSA smoke: card \"%s\" did not open (%s)\n", cards[0].label.c_str(), o.message.c_str());
        ++g_skip;
        return;
    }
    CountSink sink;
    uint32_t rate = o.format.rate;
    Capture cap(std::move(o), sink);
    CHECK(cap.start());
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    cap.stop();
    CaptureStats s = cap.stats();
    std::printf("real ALSA smoke: card \"%s\" %u Hz, %llu frames in 0.5 s, overruns %u%s\n", cards[0].label.c_str(), rate,
                static_cast<unsigned long long>(s.frames_captured), s.overruns, cap.via_server() ? " (via server)" : "");
    CHECK(s.frames_captured > rate / 4);
}
