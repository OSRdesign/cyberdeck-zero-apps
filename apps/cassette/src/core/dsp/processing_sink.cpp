/* SPDX-License-Identifier: MIT */
#include "processing_sink.hpp"

#include "pcm.hpp"

#include <cstring>

namespace cassette {

ProcessingSink::ProcessingSink(const AudioFormat &fmt, FrameSink *next, size_t chunk_frames)
    : fmt_(fmt), next_(next), chunk_(chunk_frames ? chunk_frames : kDefaultChunkFrames),
      bpf_(pcm::bytes_per_frame(fmt)), bytes_(chunk_ * bpf_), work_(chunk_ * fmt.channels),
      dc_(fmt.rate, fmt.channels), hpf_(fmt.rate, fmt.channels), lim_(fmt.rate, fmt.channels), meter_(fmt) {}

size_t ProcessingSink::write(const void *frames, size_t nframes) {
    if (next_ == nullptr) return 0;
    if (!pcm::supported(fmt_.bits) || fmt_.channels == 0 || frames == nullptr) return next_->write(frames, nframes);

    const uint8_t *src = static_cast<const uint8_t *>(frames);
    const uint16_t bits = fmt_.bits;
    const size_t bps = pcm::bytes_per_sample(bits);
    const float inv = 1.0f / pcm::scale(bits);
    size_t accepted = 0;
    while (nframes > 0) {
        const size_t k = nframes < chunk_ ? nframes : chunk_;
        const size_t ns = k * fmt_.channels;

        const bool dc = dc_on_.load(std::memory_order_relaxed);
        const bool hpf = hpf_on_.load(std::memory_order_relaxed);
        const bool lim = lim_on_.load(std::memory_order_relaxed);
        if (dc && !dc_was_) dc_.reset();
        if (hpf && !hpf_was_) hpf_.reset();
        if (lim && !lim_was_) lim_.reset();
        dc_was_ = dc;
        hpf_was_ = hpf;
        lim_was_ = lim;

        float *w = work_.data();
        for (size_t i = 0; i < ns; ++i) w[i] = float(pcm::read(src + i * bps, bits)) * inv;
        if (dc) dc_.process(w, k);
        gain_.process(w, ns);
        if (hpf) hpf_.process(w, k);
        if (lim) lim_.process(w, k);

        uint8_t *out = bytes_.data();
        uint64_t clipped = 0;
        for (size_t i = 0; i < ns; ++i) {
            bool c = false;
            pcm::write(out + i * bps, bits, pcm::quantize(w[i], bits, c));
            clipped += c;
        }
        if (clipped) clips_.fetch_add(clipped, std::memory_order_relaxed);

        meter_.feed(out, k);
        if (FrameSink *t = tap_.load(std::memory_order_acquire)) t->write(out, k);
        const size_t got = next_->write(out, k);
        accepted += got;
        if (got < k) break;
        src += k * bpf_;
        nframes -= k;
    }
    return accepted;
}

}  // namespace cassette
