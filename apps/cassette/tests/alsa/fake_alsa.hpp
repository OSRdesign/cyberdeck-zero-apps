/*
 * SPDX-License-Identifier: MIT
 *
 * A scriptable fake sound system for the Cassette tests (and for `cassette-probe --fake`): cards with controls, an open that can
 * fail with EBUSY, and scripted reads: synthetic samples (a counter pattern, never a recording), an xrun at frame N, a lost
 * card (-ENODEV or -EBADFD) at frame N, short reads. It also keeps a fake clock that advances with the frames read.
 */
#pragma once

#include <map>
#include <string>
#include <vector>

#include "alsa_api.hpp"

namespace cassette {

struct FakeEvent {
    enum Kind { Xrun, Error } kind = Xrun;
    uint64_t at_frame = 0; /* fires when this many frames have been delivered by the PCM */
    int err = 0;           /* Error: the permanent error returned from then on (-ENODEV, -EBADFD ...) */
    double gap_s = 0;      /* Xrun: seconds the fake clock jumps (the stall that caused the overrun) */
};

struct FakeCard {
    CardInfo info;
    CaptureOffer offer;
    std::vector<CtlInfo> ctls;
    std::map<std::string, long> values;
    bool present = true;
    bool busy = false;          /* a direct open fails with EBUSY (a sound server client is recording) */
    int open_error = 0;         /* a negative error for every open/query, when not 0 */
    std::vector<FakeEvent> events; /* sorted by at_frame; consumed by the PCM that is open */
    unsigned short_read_every = 0; /* every Nth read returns half a period */
    bool zero_reads = false;    /* every read returns 0 frames (a stuck card) */
};

class FakeAlsa : public AlsaApi {
public:
    bool lib_available = true;
    std::vector<FakeCard> cards_;
    int server_error = 0;               /* error opening "default" */
    bool pace = false;                  /* sleep for the audio duration on each read (the probe uses it) */
    double clock = 0;                   /* the fake clock, seconds */
    std::vector<std::string> opened;    /* every device string passed to open_capture */
    std::vector<std::string> queried;
    std::vector<HwRequest> requests;
    std::vector<std::pair<std::string, long>> mixer_writes; /* name, value, in order */
    unsigned reads_after_error = 0;     /* reads still made after a fatal error was returned */

    bool available(std::string *why) override;
    std::vector<CardInfo> cards() override;
    int query_capture(const std::string &device, CaptureOffer &out) override;
    int open_capture(const std::string &device, const HwRequest &req, std::unique_ptr<AlsaPcm> &pcm,
                     HwGranted &granted) override;
    int mixer_list(const std::string &card, std::vector<CtlInfo> &out) override;
    int mixer_get(const std::string &card, const std::string &name, long &value) override;
    int mixer_set(const std::string &card, const std::string &name, long value) override;

    FakeCard *find_id(const std::string &id);
    /* Renumbers the cards as a reboot would (rotates the indexes). */
    void reboot_shuffle();

};

/* Presets from the P0 measurements (synthetic data only). */
FakeCard fake_pcm2902(int index, const std::string &id = "Device", const std::string &port = "1-1");
FakeCard fake_uc02(int index, const std::string &port = "1-1.2");
FakeCard fake_card_no_controls(int index); /* a capture card with no mixer controls at all */
FakeAlsa fake_demo();                      /* what the probe shows with --fake */

} // namespace cassette
