/* SPDX-License-Identifier: MIT */
#include "player.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <fcntl.h>

namespace cassette {

/* ---------------------------------------------------------------- WavReader */

int WavReader::open(const std::string &path) {
    close();
    HeaderInfo h = parse_header(ops_, path);
    if (!h.ok) return -EINVAL;
    const AudioFormat &f = h.fmt;
    if ((f.bits != 16 && f.bits != 24 && f.bits != 32) || f.channels < 1 || f.rate == 0) return -EINVAL;
    const uint32_t align = static_cast<uint32_t>(f.channels) * (f.bits / 8);
    if (h.block_align != 0 && h.block_align != align) return -EINVAL;
    if (h.data_offset > h.file_size) return -EINVAL;
    const uint64_t avail = h.file_size - h.data_offset;
    const bool distrust = h.suspicious() || h.declared_data > avail;
    const uint64_t bytes = distrust ? avail : h.declared_data;
    int fd = ops_.open(path, O_RDONLY, 0);
    if (fd < 0) return fd;
    fd_ = fd;
    fmt_ = f;
    align_ = align;
    data_offset_ = h.data_offset;
    frames_ = bytes / align;
    used_file_length_ = distrust;
    return 0;
}

void WavReader::close() {
    if (fd_ >= 0) ops_.close(fd_);
    fd_ = -1;
    frames_ = 0;
}

long WavReader::read(uint64_t frame, size_t n, void *buf) {
    if (fd_ < 0) return -EBADF;
    if (frame >= frames_) return 0;
    n = static_cast<size_t>(std::min<uint64_t>(n, frames_ - frame));
    const size_t want = n * align_;
    unsigned char *p = static_cast<unsigned char *>(buf);
    size_t got = 0;
    while (got < want) {
        ssize_t k = ops_.pread(fd_, p + got, want - got, data_offset_ + frame * align_ + got);
        if (k < 0) {
            if (k == -EINTR) continue;
            return static_cast<long>(k);
        }
        if (k == 0) break;
        got += static_cast<size_t>(k);
    }
    return static_cast<long>(got / align_);
}

/* ---------------------------------------------------------------- names */

const char *player_state_name(PlayerState s) {
    switch (s) {
    case PlayerState::Stopped: return "Stopped";
    case PlayerState::Playing: return "Playing";
    case PlayerState::Paused: return "Paused";
    case PlayerState::Scanning: return "Scanning";
    }
    return "?";
}

const char *player_event_name(PlayerEvent e) {
    switch (e) {
    case PlayerEvent::TakeEnded: return "TakeEnded";
    case PlayerEvent::ReachedStart: return "ReachedStart";
    case PlayerEvent::ReachedEnd: return "ReachedEnd";
    case PlayerEvent::DeviceLost: return "DeviceLost";
    }
    return "?";
}

/* ---------------------------------------------------------------- sample helpers */

namespace {

int32_t load_sample(const unsigned char *p, unsigned bytes) {
    switch (bytes) {
    case 2: return static_cast<int16_t>(static_cast<uint16_t>(p[0] | (p[1] << 8)));
    case 3: {
        uint32_t v = static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16);
        if (v & 0x800000u) v |= 0xFF000000u;
        return static_cast<int32_t>(v);
    }
    default:
        return static_cast<int32_t>(static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
                                    (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24));
    }
}

void store_sample(unsigned char *p, unsigned bytes, int32_t v) {
    const uint32_t u = static_cast<uint32_t>(v);
    p[0] = static_cast<unsigned char>(u & 0xFF);
    p[1] = static_cast<unsigned char>((u >> 8) & 0xFF);
    if (bytes >= 3) p[2] = static_cast<unsigned char>((u >> 16) & 0xFF);
    if (bytes >= 4) p[3] = static_cast<unsigned char>((u >> 24) & 0xFF);
}

/* Changes the bit depth of a sample (left shift up, arithmetic right shift down). */
int32_t rescale(int32_t v, int from_bits, int to_bits) {
    if (to_bits == from_bits) return v;
    if (to_bits > from_bits) return static_cast<int32_t>(static_cast<uint32_t>(v) << (to_bits - from_bits));
    return v >> (from_bits - to_bits);
}

/* The rounded mean (halves away from zero). */
int32_t mean(int64_t sum, int64_t k) {
    if (k <= 1) return static_cast<int32_t>(sum);
    return static_cast<int32_t>(sum >= 0 ? (sum + k / 2) / k : -((-sum + k / 2) / k));
}

} // namespace

/* ---------------------------------------------------------------- Player */

struct Player::Cmd {
    enum Kind { Open, Close, Play, Pause, Resume, Stop, Seek, Scan, EndScan, Jump } kind = Play;
    std::string path;
    uint64_t arg = 0;
    Direction dir = Direction::Forward;
    unsigned n = 0;
    bool done = false;
    int result = 0;
};

Player::Player(FileOps &ops, PlaybackSink &sink, Mode mode) : ops_(ops), sink_(sink), mode_(mode), reader_(ops) {
    if (mode_ == Mode::Threaded) th_ = std::thread([this] { run(); });
}

Player::~Player() {
    if (th_.joinable()) {
        {
            std::lock_guard<std::mutex> g(mu_);
            quit_ = true;
        }
        cv_.notify_all();
        th_.join();
    }
    {
        std::lock_guard<std::mutex> g(mu_);
        for (auto &c : queue_) {
            c->done = true;
            c->result = -ECANCELED;
        }
        queue_.clear();
    }
    done_cv_.notify_all();
    do_stop(0);
    reader_.close();
}

int Player::submit(std::shared_ptr<Cmd> c) {
    {
        std::lock_guard<std::mutex> g(mu_);
        queue_.push_back(c);
    }
    if (mode_ == Mode::Manual) {
        process_commands();
        return c->result;
    }
    cv_.notify_all();
    std::unique_lock<std::mutex> lk(mu_);
    if (!done_cv_.wait_for(lk, std::chrono::milliseconds(kCommandWaitMs), [&] { return c->done; })) return -ETIMEDOUT;
    return c->result;
}

void Player::process_commands() {
    for (;;) {
        std::shared_ptr<Cmd> c;
        {
            std::lock_guard<std::mutex> g(mu_);
            if (queue_.empty()) break;
            c = queue_.front();
            queue_.pop_front();
        }
        int r = apply(*c);
        publish();
        {
            std::lock_guard<std::mutex> g(mu_);
            c->result = r;
            c->done = true;
        }
        done_cv_.notify_all();
    }
}

void Player::run() {
    for (;;) {
        {
            std::unique_lock<std::mutex> lk(mu_);
            cv_.wait(lk, [&] { return quit_ || !queue_.empty() || running(); });
            if (quit_) break;
        }
        process_commands();
        pump();
    }
}

bool Player::step() {
    process_commands();
    return pump();
}

int Player::ensure_sink() {
    if (sink_open_) return 0;
    SinkFormat f;
    int r = sink_.open(reader_.format(), f);
    if (r < 0) return r;
    if (f.channels == 0) {
        sink_.close();
        return -EINVAL;
    }
    if (f.rate != 0 && f.rate != reader_.format().rate) { /* the Player sends source frames: another rate would play at the wrong speed */
        sink_.close();
        return -EINVAL;
    }
    if (f.period_frames == 0) f.period_frames = kPlaybackPeriod;
    sfmt_ = f;
    sink_open_ = true;
    return 0;
}

void Player::close_sink() {
    if (sink_open_) sink_.close();
    sink_open_ = false;
}

/* Drops what the device still holds and moves the position back to the frame being heard. */
void Player::drop_and_rewind() {
    if (!sink_open_) return;
    long d = (st_ == PlayerState::Playing || st_ == PlayerState::Scanning) ? sink_.delay() : 0;
    sink_.drop();
    if (d <= 0) return;
    const uint64_t ud = static_cast<uint64_t>(d);
    if (st_ == PlayerState::Playing) {
        pos_ -= std::min(pos_, ud);
    } else if (dir_ == Direction::Forward) {
        pos_ -= std::min(pos_, ud * speed_);
    } else {
        pos_ = std::min(len_, pos_ + ud * speed_);
    }
}

void Player::do_stop(uint64_t pos) {
    if (sink_open_) sink_.drop();
    close_sink();
    st_ = PlayerState::Stopped;
    held_ = false;
    pos_ = pos;
}

void Player::device_lost() {
    close_sink();
    st_ = PlayerState::Paused;
    held_ = false;
    post(PlayerEvent::DeviceLost);
}

void Player::post(PlayerEvent e) {
    std::lock_guard<std::mutex> g(mu_);
    events_.push_back(e);
}

void Player::publish() {
    uint64_t heard = pos_;
    if (sink_open_ && running()) {
        long d = sink_.delay();
        if (d > 0) {
            const uint64_t ud = static_cast<uint64_t>(d);
            if (st_ == PlayerState::Playing) heard -= std::min(heard, ud);
            else if (dir_ == Direction::Forward) heard -= std::min(heard, ud * speed_);
            else heard = std::min(len_, heard + ud * speed_);
        }
    }
    std::lock_guard<std::mutex> g(mu_);
    snap_.st = st_;
    snap_.prev = prev_;
    snap_.dir = dir_;
    snap_.held = held_;
    snap_.heard = heard;
    snap_.len = len_;
    snap_.fmt = reader_.format();
    snap_.sfmt = sfmt_;
    snap_.underruns = underruns_;
}

int Player::apply(Cmd &c) {
    switch (c.kind) {
    case Cmd::Open: {
        do_stop(0);
        reader_.close();
        int r = reader_.open(c.path);
        len_ = r == 0 ? reader_.frames() : 0;
        return r;
    }
    case Cmd::Close:
        do_stop(0);
        reader_.close();
        len_ = 0;
        return 0;
    case Cmd::Stop:
        do_stop(0);
        return 0;
    default: break;
    }
    if (!reader_.is_open()) return -ENOENT;
    switch (c.kind) {
    case Cmd::Play: {
        if (st_ == PlayerState::Playing) return 0;
        if (st_ == PlayerState::Stopped && pos_ >= len_) pos_ = 0;
        if (st_ == PlayerState::Scanning) {
            drop_and_rewind();
            held_ = false;
        }
        int r = ensure_sink();
        if (r < 0) return r;
        st_ = PlayerState::Playing;
        return 0;
    }
    case Cmd::Pause:
        if (st_ == PlayerState::Playing || st_ == PlayerState::Scanning) {
            drop_and_rewind();
            held_ = false;
            st_ = PlayerState::Paused;
        }
        return 0;
    case Cmd::Resume: {
        if (st_ == PlayerState::Playing) return 0;
        if (st_ == PlayerState::Scanning) return -EINVAL;
        if (st_ == PlayerState::Stopped && pos_ >= len_) pos_ = 0;
        int r = ensure_sink();
        if (r < 0) return r;
        st_ = PlayerState::Playing;
        return 0;
    }
    case Cmd::Seek:
        if (sink_open_ && running()) sink_.drop();
        held_ = false;
        pos_ = std::min(c.arg, len_);
        return 0;
    case Cmd::Jump: {
        drop_and_rewind();
        held_ = false;
        const uint64_t delta = static_cast<uint64_t>(c.n) * reader_.format().rate;
        if (c.dir == Direction::Forward) pos_ = std::min(len_, pos_ + delta);
        else pos_ -= std::min(pos_, delta);
        return 0;
    }
    case Cmd::Scan: {
        drop_and_rewind();
        if (st_ != PlayerState::Scanning) prev_ = st_ == PlayerState::Playing ? PlayerState::Playing : PlayerState::Paused;
        int r = ensure_sink();
        if (r < 0) return r; /* the state is left as it was */
        dir_ = c.dir;
        speed_ = std::max(2u, std::min(16u, c.n));
        held_ = false;
        st_ = PlayerState::Scanning;
        return 0;
    }
    case Cmd::EndScan: {
        if (st_ != PlayerState::Scanning) return 0;
        drop_and_rewind();
        held_ = false;
        st_ = prev_;
        if (st_ == PlayerState::Playing) {
            int r = ensure_sink();
            if (r < 0) {
                st_ = PlayerState::Paused;
                return r;
            }
        }
        return 0;
    }
    default: return -EINVAL;
    }
}

/* Writes `frames` frames of out_ (whole), recovering from underruns. false when the device is gone. */
bool Player::write_out(size_t frames, size_t *written) {
    const size_t fb = sfmt_.channels * format_bytes(sfmt_.format);
    size_t done = 0;
    unsigned zeros = 0;
    while (done < frames) {
        long w = sink_.write(out_.data() + done * fb, frames - done);
        if (w > 0) {
            done += static_cast<size_t>(w);
            zeros = 0;
            continue;
        }
        if (w == 0) {
            if (++zeros > 1000) break;
            continue;
        }
        if (w == -EPIPE || w == -ESTRPIPE || w == -EINTR) {
            if (w != -EINTR) ++underruns_;
            if (sink_.recover(static_cast<int>(w)) == 0) continue;
        }
        break;
    }
    *written = done;
    return done == frames;
}

bool Player::pump() {
    if (!running()) return false;
    const AudioFormat &f = reader_.format();
    const unsigned sch = f.channels, sbytes = f.bits / 8;
    const uint64_t period = sfmt_.period_frames ? sfmt_.period_frames : kPlaybackPeriod;
    const bool scanning = st_ == PlayerState::Scanning;
    const bool reverse = scanning && dir_ == Direction::Reverse;
    const unsigned g = scanning ? speed_ : 1;

    /* the ends */
    if (!reverse && pos_ >= len_) {
        sink_.drain();
        close_sink();
        st_ = PlayerState::Stopped;
        held_ = false;
        pos_ = len_;
        post(scanning ? PlayerEvent::ReachedEnd : PlayerEvent::TakeEnded);
        publish();
        return true;
    }
    if (reverse && pos_ == 0) { /* the start ends a reverse scan as the end ends a forward one: Stopped at 0 */
        sink_.drain();
        close_sink();
        st_ = PlayerState::Stopped;
        held_ = false;
        post(PlayerEvent::ReachedStart);
        publish();
        return true;
    }

    /* the source block */
    const uint64_t room = reverse ? pos_ : len_ - pos_;
    const uint64_t nout = std::min<uint64_t>(period, (room + g - 1) / g);
    const uint64_t src = std::min<uint64_t>(room, nout * g);
    const uint64_t start = reverse ? pos_ - src : pos_;
    in_.resize(static_cast<size_t>(src) * reader_.block_align());
    long got = reader_.read(start, static_cast<size_t>(src), in_.data());
    if (got < static_cast<long>(src)) { /* the file shrank or cannot be read: end the take */
        do_stop(len_);
        post(scanning ? PlayerEvent::ReachedEnd : PlayerEvent::TakeEnded);
        publish();
        return true;
    }

    /* averaging groups of g frames (g = 1 for normal play), channel mapping and format conversion */
    const unsigned och = sfmt_.channels, obytes = format_bytes(sfmt_.format), obits = obytes * 8;
    out_.resize(static_cast<size_t>(nout) * och * obytes);
    tmp_.resize(sch);
    int32_t lpk[2] = {0, 0}; /* the level tap: the sample peak of what is sent, per source channel (first two) */
    for (uint64_t j = 0; j < nout; ++j) {
        uint64_t a, b; /* source frames [a, b) of the block */
        if (!reverse) {
            a = j * g;
            b = std::min<uint64_t>(src, a + g);
        } else {
            b = src - j * g;
            a = b > g ? b - g : 0;
        }
        for (unsigned c = 0; c < sch; ++c) {
            int64_t sum = 0;
            for (uint64_t i = a; i < b; ++i) sum += load_sample(&in_[(i * sch + c) * sbytes], sbytes);
            const int32_t m = mean(sum, static_cast<int64_t>(b - a));
            if (c < 2) {
                const int32_t mag = m < 0 ? -m : m;
                if (mag > lpk[c]) lpk[c] = mag;
            }
            tmp_[c] = rescale(m, f.bits, static_cast<int>(obits));
        }
        unsigned char *o = &out_[static_cast<size_t>(j) * och * obytes];
        for (unsigned c = 0; c < och; ++c) {
            int32_t v;
            if (och == sch) v = tmp_[c];
            else if (sch == 1) v = tmp_[0]; /* mono to a stereo device: the channel duplicated */
            else if (och == 1) {
                int64_t s = 0;
                for (unsigned k = 0; k < sch; ++k) s += tmp_[k];
                v = mean(s, sch);
            } else v = c < sch ? tmp_[c] : 0;
            store_sample(o + c * obytes, obytes, v);
        }
    }

    size_t written = 0;
    bool ok = write_out(static_cast<size_t>(nout), &written);
    if (written > 0) {
        /* the level tap (lock-free, no allocation): the peak of this period as a fraction of full scale, then the count */
        const double full = static_cast<double>((int64_t{1} << (f.bits - 1)) - 1);
        for (unsigned c = 0; c < 2; ++c)
            level_peak_[c].store(c < sch ? static_cast<float>(std::min(1.0, lpk[c] / full)) : 0.0f, std::memory_order_relaxed);
        level_channels_.store(static_cast<uint16_t>(std::min(2u, sch)), std::memory_order_relaxed);
        level_seq_.fetch_add(1, std::memory_order_release);
    }
    const uint64_t moved = std::min<uint64_t>(src, static_cast<uint64_t>(written) * g);
    if (reverse) pos_ -= moved;
    else pos_ += moved;
    if (!ok) device_lost();
    publish();
    return true;
}

/* ---------------------------------------------------------------- commands and queries */

#define CASSETTE_CMD(kind_) \
    auto c = std::make_shared<Cmd>(); \
    c->kind = Cmd::kind_

int Player::open(const std::string &path) {
    CASSETTE_CMD(Open);
    c->path = path;
    return submit(c);
}
int Player::close() {
    CASSETTE_CMD(Close);
    return submit(c);
}
int Player::play() {
    CASSETTE_CMD(Play);
    return submit(c);
}
int Player::pause() {
    CASSETTE_CMD(Pause);
    return submit(c);
}
int Player::resume() {
    CASSETTE_CMD(Resume);
    return submit(c);
}
int Player::stop() {
    CASSETTE_CMD(Stop);
    return submit(c);
}
int Player::seek(uint64_t frame) {
    CASSETTE_CMD(Seek);
    c->arg = frame;
    return submit(c);
}
int Player::scan(Direction dir, unsigned speed) {
    CASSETTE_CMD(Scan);
    c->dir = dir;
    c->n = speed;
    return submit(c);
}
int Player::end_scan() {
    CASSETTE_CMD(EndScan);
    return submit(c);
}
int Player::jump(Direction dir, unsigned seconds) {
    CASSETTE_CMD(Jump);
    c->dir = dir;
    c->n = seconds;
    return submit(c);
}
#undef CASSETTE_CMD

PlayerState Player::state() const {
    std::lock_guard<std::mutex> g(mu_);
    return snap_.st;
}
PlayerState Player::state_before_scan() const {
    std::lock_guard<std::mutex> g(mu_);
    return snap_.prev;
}
Direction Player::scan_direction() const {
    std::lock_guard<std::mutex> g(mu_);
    return snap_.dir;
}
bool Player::at_start_hold() const {
    std::lock_guard<std::mutex> g(mu_);
    return snap_.st == PlayerState::Scanning && snap_.held;
}
uint64_t Player::position() const {
    std::lock_guard<std::mutex> g(mu_);
    return snap_.heard;
}
uint64_t Player::length() const {
    std::lock_guard<std::mutex> g(mu_);
    return snap_.len;
}
double Player::position_s() const {
    std::lock_guard<std::mutex> g(mu_);
    return snap_.fmt.rate ? static_cast<double>(snap_.heard) / snap_.fmt.rate : 0.0;
}
double Player::length_s() const {
    std::lock_guard<std::mutex> g(mu_);
    return snap_.fmt.rate ? static_cast<double>(snap_.len) / snap_.fmt.rate : 0.0;
}
AudioFormat Player::format() const {
    std::lock_guard<std::mutex> g(mu_);
    return snap_.fmt;
}
SinkFormat Player::sink_format() const {
    std::lock_guard<std::mutex> g(mu_);
    return snap_.sfmt;
}
unsigned Player::underruns() const {
    std::lock_guard<std::mutex> g(mu_);
    return snap_.underruns;
}
uint32_t Player::level(float peak[2], uint16_t *channels) const {
    const uint32_t q = level_seq_.load(std::memory_order_acquire);
    peak[0] = level_peak_[0].load(std::memory_order_relaxed);
    peak[1] = level_peak_[1].load(std::memory_order_relaxed);
    if (channels) *channels = level_channels_.load(std::memory_order_relaxed);
    return q;
}
bool Player::poll_event(PlayerEvent &e) {
    std::lock_guard<std::mutex> g(mu_);
    if (events_.empty()) return false;
    e = events_.front();
    events_.pop_front();
    return true;
}

} // namespace cassette
