/*
 * SPDX-License-Identifier: MIT
 *
 * mmWave Radar: the LD2451 driver and frame decoder (see ld2451.hpp).
 */
#include "ld2451.hpp"

#include <cmath>

namespace mmw {

namespace {

const std::uint8_t kHead[4] = {0xF4, 0xF3, 0xF2, 0xF1};
const std::uint8_t kFoot[4] = {0xF8, 0xF7, 0xF6, 0xF5};
constexpr double kPi = 3.14159265358979323846;
constexpr double kCmSPerKmH = 100.0 / 3.6;

std::size_t header_match(const std::uint8_t *p, std::size_t n)
{
    std::size_t i = 0;
    while (i < n && i < 4 && p[i] == kHead[i]) ++i;
    return i;
}

/* The count of a length field, or -1 when the length is not 0 or 2 + 5n (n <= the wire limit). */
int count_of_len(std::size_t len)
{
    if (len == 0) return 0;
    if (len < 2 || (len - 2) % 5 != 0) return -1;
    const std::size_t n = (len - 2) / 5;
    return n <= kLd2451MaxWireTargets ? static_cast<int>(n) : -1;
}

} // namespace

void ld2451_polar_to_xy(int distance_m, int angle_deg, int &x_mm, int &y_mm)
{
    const double d = distance_m * 1000.0;
    const double a = angle_deg * kPi / 180.0;
    x_mm = static_cast<int>(std::lround(d * std::sin(a)));
    y_mm = static_cast<int>(std::lround(d * std::cos(a)));
}

std::size_t ld2451_frame_size(const std::uint8_t *p)
{
    if (header_match(p, 4) != 4) return 0;
    const std::size_t len = static_cast<std::size_t>(p[4] | (p[5] << 8));
    return count_of_len(len) < 0 ? 0 : 10 + len;
}

bool ld2451_decode_frame(const std::uint8_t *p, std::size_t n, Frame &out)
{
    const std::size_t total = n >= 6 ? ld2451_frame_size(p) : 0;
    if (total == 0 || n != total) return false;
    const std::size_t len = total - 10;
    for (int k = 0; k < 4; ++k)
        if (p[6 + len + k] != kFoot[k]) return false;
    const int count = count_of_len(len);
    if (len > 0 && p[6] != count) return false; /* the count byte must agree with the length */
    out = Frame();
    for (std::size_t k = 0; k < sizeof(out.raw) && k < total; ++k) out.raw[k] = p[k];
    for (int i = 0; i < 3; ++i) out.target[i].slot = i + 1;
    for (int i = 0; i < count && i < 3; ++i) {
        const std::uint8_t *q = p + 8 + 5 * i;
        Target &t = out.target[i];
        const int angle = static_cast<int>(q[0]) - 0x80;
        const int dist = q[1];
        t.present = true;
        t.angle_deg = angle;
        t.distance_mm = dist * 1000;
        ld2451_polar_to_xy(dist, angle, t.x_mm, t.y_mm);
        t.resolution_mm = 1000;
        const int mag = static_cast<int>(std::lround(q[3] * kCmSPerKmH));
        t.direction = q[2] == 1 ? Direction::Approaching : q[2] == 0 ? Direction::Receding : Direction::Still;
        t.speed_cm_s = q[2] == 1 ? -mag : mag; /* approaching is negative */
        t.snr = q[4];
    }
    return true;
}

bool ld2451_plausible(const std::uint8_t *p, std::size_t n)
{
    const std::size_t total = n >= 6 ? ld2451_frame_size(p) : 0;
    if (total == 0 || n < total) return false;
    const std::size_t len = total - 10;
    if (len == 0) return true;
    if (p[7] > 1) return false; /* alarm */
    for (std::size_t i = 0; i < (len - 2) / 5; ++i) {
        const std::uint8_t *q = p + 8 + 5 * i;
        const int angle = static_cast<int>(q[0]) - 0x80;
        if (angle < -90 || angle > 90 || q[2] > 1) return false;
    }
    return true;
}

void Ld2451Decoder::reset()
{
    buf_.clear();
    pos_ = 0;
    stats_ = DecoderStats();
}

std::size_t Ld2451Decoder::feed(const std::uint8_t *data, std::size_t n, std::vector<Frame> *out)
{
    stats_.bytes += n;
    buf_.insert(buf_.end(), data, data + n);
    std::size_t added = 0;
    process(out, added);
    if (pos_ > 0) {
        buf_.erase(buf_.begin(), buf_.begin() + static_cast<std::ptrdiff_t>(pos_));
        pos_ = 0;
    }
    return added;
}

void Ld2451Decoder::process(std::vector<Frame> *out, std::size_t &added)
{
    for (;;) {
        const std::uint8_t *p = buf_.data() + pos_;
        const std::size_t have = buf_.size() - pos_;
        if (have == 0) return;
        const std::size_t hm = header_match(p, have);
        if (hm == have && hm < 4) return; /* the start of a header: wait */
        if (hm < 4) { /* not a header */
            stats_.good_run = 0;
            ++stats_.garbage_bytes;
            ++pos_;
            continue;
        }
        if (have < 6) return;
        const std::size_t len = static_cast<std::size_t>(p[4] | (p[5] << 8));
        if (count_of_len(len) < 0) { /* a header with an impossible length: drop it, look for the next one */
            ++stats_.frames_bad;
            stats_.good_run = 0;
            ++stats_.garbage_bytes;
            ++pos_;
            continue;
        }
        const std::size_t total = 10 + len;
        if (have < total) return; /* the rest has not arrived */
        Frame f;
        if (ld2451_decode_frame(p, total, f)) {
            ++stats_.frames_ok;
            stats_.good_run = ld2451_plausible(p, total) ? stats_.good_run + 1 : 0;
            if (out) out->push_back(f);
            ++added;
            pos_ += total;
        } else {
            ++stats_.frames_bad;
            stats_.good_run = 0;
            ++stats_.garbage_bytes;
            ++pos_;
        }
    }
}

namespace {

/* Stateless: Match when a complete valid and plausible frame (an empty one counts) is in the window; Maybe when the
 * window ends in what could be the start of one (a header prefix, or a header and a good length with the rest not yet
 * there); No otherwise. */
SniffResult sniff(const std::uint8_t *s, std::size_t n)
{
    bool maybe = false;
    for (std::size_t i = 0; i < n; ++i) {
        if (s[i] != kHead[0]) continue;
        const std::size_t avail = n - i;
        const std::size_t hm = header_match(s + i, avail);
        if (hm < 4 && hm < avail) continue; /* a wrong header byte */
        if (hm < 4 || avail < 6) {
            maybe = true;
            continue;
        }
        const std::size_t total = ld2451_frame_size(s + i);
        if (total == 0) continue; /* an impossible length: not ours (an LD2410's, say) */
        if (avail < total) {
            maybe = true;
            continue;
        }
        Frame f;
        if (ld2451_decode_frame(s + i, total, f) && ld2451_plausible(s + i, total)) return SniffResult::Match;
    }
    return maybe ? SniffResult::Maybe : SniffResult::No;
}

std::unique_ptr<FrameDecoder> make_decoder() { return std::unique_ptr<FrameDecoder>(new Ld2451Decoder()); }

/* 115200 is the sensor's default (document, p.3) and the speed of every recording. The document lets it be set to
 * 9600-460800 by a command; this app sends none, so no other speed is tried. */
const int kBauds[] = {115200};

} // namespace

const SensorDriver kDriverLd2451 = {
    "ld2451",
    "LD2451",
    "LD2451 target radar (angle, range, speed)",
    "mmWave radar (LD2451)",
    "",
    kBauds,
    sizeof(kBauds) / sizeof(kBauds[0]),
    &sniff,
    &make_decoder,
    {true, false, true},
    ViewKind::Tracking,
    5000, /* quiet_ms: an empty frame every 1.4 s, longest gap between any two frames in the recordings 2.8 s */
    3000, /* drop_ms: longest gap between two target frames inside one pass 2.65 s, nothing for up to 2.8 s */
    6000, /* listen_ms: three empty frames take up to 5 s (1.7 s apart at most) */
    2000, /* silent_ms: longer than the longest empty-frame interval seen (1.67 s) */
    100,  /* max_range_m */
};

} // namespace mmw
