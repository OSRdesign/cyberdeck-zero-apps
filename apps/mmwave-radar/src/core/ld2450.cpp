/*
 * SPDX-License-Identifier: MIT
 *
 * mmWave Radar: the LD2450-family frame decoder and driver (see ld2450.hpp).
 */
#include "ld2450.hpp"

#include <cmath>
#include <cstdlib>

namespace mmw {

namespace {

const std::uint8_t kHeader[4] = {0xAA, 0xFF, 0x03, 0x00};
constexpr std::size_t kMaxAnswerData = 64; /* the answers seen are 2..30 bytes; the documented buffer is 64 */

std::uint16_t le16(const std::uint8_t *p) { return static_cast<std::uint16_t>(p[0] | (p[1] << 8)); }

/* Number of leading bytes of p[0..n) that match the header (at most 4). */
std::size_t header_match(const std::uint8_t *p, std::size_t n)
{
    std::size_t i = 0;
    while (i < n && i < 4 && p[i] == kHeader[i]) ++i;
    return i;
}

bool is_fcfbfa(const std::uint8_t *p) { return p[0] == 0xFC && p[1] == 0xFB && p[2] == 0xFA; }

} // namespace

int sign_magnitude(std::uint16_t raw)
{
    const int v = raw & 0x7FFF;
    return (raw & 0x8000) ? v : -v;
}

bool decode_frame(const std::uint8_t *p, Frame &out)
{
    if (header_match(p, 4) != 4 || p[28] != 0x55 || p[29] != 0xCC) return false;
    for (int k = 0; k < 30; ++k) out.raw[k] = p[k];
    for (int i = 0; i < 3; ++i) {
        const std::uint8_t *q = p + 4 + 8 * i;
        Target &t = out.target[i];
        t = Target();
        t.slot = i + 1;
        bool zero = true;
        for (int k = 0; k < 8; ++k) zero = zero && q[k] == 0;
        if (zero) continue;
        t.present = true;
        t.x_mm = sign_magnitude(le16(q));
        t.y_mm = sign_magnitude(le16(q + 2));
        t.speed_cm_s = sign_magnitude(le16(q + 4));
        t.resolution_mm = le16(q + 6);
        const double x = t.x_mm, y = t.y_mm;
        t.distance_mm = static_cast<int>(std::lround(std::hypot(x, y)));
        t.angle_deg = std::atan2(x, y) * 180.0 / 3.14159265358979323846;
        t.direction = t.speed_cm_s < 0 ? Direction::Approaching : t.speed_cm_s > 0 ? Direction::Receding : Direction::Still;
    }
    return true;
}

bool plausible(const Frame &f)
{
    for (const Target &t : f.target) {
        if (!t.present) continue;
        if (std::abs(t.x_mm) > 8000 || t.y_mm <= 0 || t.y_mm > 8000) return false;
        if (std::abs(t.speed_cm_s) > 1000 || t.resolution_mm > 2000) return false;
    }
    return true;
}

void Decoder::reset()
{
    buf_.clear();
    pos_ = 0;
    stats_ = DecoderStats();
}

std::size_t Decoder::feed(const std::uint8_t *data, std::size_t n, std::vector<Frame> *out)
{
    stats_.bytes += n;
    buf_.insert(buf_.end(), data, data + n);
    std::size_t added = 0;
    process(out, added);
    if (pos_ > 0) { /* compact once per feed */
        buf_.erase(buf_.begin(), buf_.begin() + static_cast<std::ptrdiff_t>(pos_));
        pos_ = 0;
    }
    return added;
}

void Decoder::process(std::vector<Frame> *out, std::size_t &added)
{
    for (;;) {
        const std::uint8_t *p = buf_.data() + pos_;
        const std::size_t have = buf_.size() - pos_;
        if (have == 0) return;

        const std::size_t hm = header_match(p, have);
        if (hm == 4 || hm == have) {
            /* a data frame, or the (incomplete) start of one */
            if (hm < 4) return; /* header incomplete: wait for more */
            /* a command answer cutting the frame: FC FB FA at index k >= 5, its (damaged) first byte at k - 1 */
            std::size_t cut = 0;
            const std::size_t lim = have < kFrameSize ? have : kFrameSize;
            for (std::size_t k = 5; k + 3 <= lim; ++k) {
                if (is_fcfbfa(p + k)) {
                    cut = k - 1;
                    break;
                }
            }
            if (cut) {
                ++stats_.frames_cut;
                stats_.good_run = 0;
                pos_ += cut; /* the answer is now at the start */
                continue;
            }
            if (have < kFrameSize) return;
            Frame f;
            if (decode_frame(p, f)) {
                ++stats_.frames_ok;
                stats_.good_run = plausible(f) ? stats_.good_run + 1 : 0;
                if (out) out->push_back(f);
                ++added;
                pos_ += kFrameSize;
            } else {
                ++stats_.frames_bad;
                stats_.good_run = 0;
                stats_.garbage_bytes += 1;
                pos_ += 1;
            }
            continue;
        }

        /* a command answer, first byte any (damaged) value: ?? FC FB FA len(2) data 04 03 02 01 */
        bool maybe_answer = true;
        {
            static const std::uint8_t tail[3] = {0xFC, 0xFB, 0xFA};
            for (std::size_t i = 1; i < have && i < 4; ++i)
                if (p[i] != tail[i - 1]) maybe_answer = false;
        }
        if (maybe_answer) {
            if (have < 6) return; /* need the length */
            const std::size_t len = le16(p + 4);
            if (len <= kMaxAnswerData) {
                const std::size_t total = 6 + len + 4;
                if (have < total) return;
                if (p[total - 4] == 0x04 && p[total - 3] == 0x03 && p[total - 2] == 0x02 && p[total - 1] == 0x01) {
                    ++stats_.answers;
                    pos_ += total;
                    continue;
                }
            }
        }

        /* garbage */
        stats_.good_run = 0;
        stats_.garbage_bytes += 1;
        pos_ += 1;
    }
}

namespace {

/* Stateless: Match when a complete, valid and plausible tracking frame is in the window; Maybe when the window ends in
 * what could be the start of one (a header prefix, or the header with fewer than 30 bytes after it). */
SniffResult sniff(const std::uint8_t *s, std::size_t n)
{
    bool maybe = false;
    for (std::size_t i = 0; i < n; ++i) {
        if (s[i] != kHeader[0]) continue;
        const std::size_t avail = n - i;
        const std::size_t hm = header_match(s + i, avail);
        if (hm < 4 && hm < avail) continue; /* a wrong header byte */
        if (avail < kFrameSize) {
            maybe = true; /* the header so far, the frame incomplete */
        } else if (hm == 4) {
            Frame f;
            if (decode_frame(s + i, f) && plausible(f)) return SniffResult::Match;
        }
    }
    return maybe ? SniffResult::Maybe : SniffResult::No;
}

std::unique_ptr<FrameDecoder> make_decoder() { return std::unique_ptr<FrameDecoder>(new Decoder()); }

/* The speeds in the detector's order (the remembered one goes first, see detect.cpp). */
const int kBauds[] = {256000, 9600, 115200, 57600, 38400, 19200, 230400, 460800};

} // namespace

const SensorDriver kDriverLd2450 = {
    "ld2450",
    "LD2452 / LD2450",
    "LD2450-family tracking radar (LD2450, LD2452)",
    "mmWave radar (LD2450 family)",
    "",
    kBauds,
    sizeof(kBauds) / sizeof(kBauds[0]),
    &sniff,
    &make_decoder,
    {true, false, false},
    ViewKind::Tracking,
    2000, /* quiet_ms: 19 frames a second, 2 s without one is a lost sensor */
    500,  /* drop_ms */
    0,    /* listen_ms: the detector's own windows */
    0,    /* silent_ms */
    6,    /* max_range_m: 2, 4 or 6 m */
};

} // namespace mmw
