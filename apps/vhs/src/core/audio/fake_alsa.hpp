/*
 * SPDX-License-Identifier: MIT
 *
 * VHS: the fake camera microphone (the scenes and the controller tests; VHS_FAKE=1 with VHS_FAKE_MIC). One USB card
 * like the FineCam's ("CM969", S16 16000 Hz stereo, on the fake camera's USB port 1-1.2) whose capture PCM delivers
 * synthetic samples at the real-time pace:
 *   tone    a 440 Hz sine at -20 dBFS (the Sound-on lamp lights)
 *   quiet   +-1 LSB noise, about -90 dBFS (the lamp stays off; not digital silence)
 *   silent  exact zeros (the silence toast after 3 s)
 *   busy    the open fails with EBUSY (another program records from it)
 *   none    no card at all (a camera without a microphone)
 * lost_after_ms > 0: the reads fail with -ENODEV from then on (the camera unplugged). LVGL-free.
 */
#pragma once

#include "alsa/alsa_api.hpp"

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace vhs {

struct FakeMicOptions {
    std::string mode = "tone";  /* tone | quiet | silent | busy | none */
    std::string card_id = "CM969";
    std::string usb_port = "1-1.2";
    std::uint32_t rate = 16000;
    unsigned channels = 2;
    bool pace = true;           /* reads wait for the audio's real time */
    int lost_after_ms = 0;      /* > 0: -ENODEV after this long */
};

/* "tone", "silent" ... (VHS_FAKE_MIC); unknown words mean "tone". */
FakeMicOptions fake_mic_options(const std::string &mode);

class FakeAlsa : public AlsaApi {
public:
    explicit FakeAlsa(FakeMicOptions opt = FakeMicOptions()) : opt_(std::move(opt)) {}
    bool available(std::string *why) override;
    std::vector<CardInfo> cards() override;
    int query_capture(const std::string &device, CaptureOffer &out) override;
    int open_capture(const std::string &device, const HwRequest &req, std::unique_ptr<AlsaPcm> &pcm,
                     HwGranted &granted) override;
    int mixer_list(const std::string &card, std::vector<CtlInfo> &out) override;
    int mixer_get(const std::string &card, const std::string &name, long &value) override;
    int mixer_set(const std::string &card, const std::string &name, long value) override;

    const FakeMicOptions &options() const { return opt_; }
    void set_mode(const std::string &mode) { opt_.mode = mode; }
    int opens() const { return opens_.load(); }

private:
    FakeMicOptions opt_;
    std::atomic<int> opens_{0};
};

} // namespace vhs
