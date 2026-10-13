/*
 * SPDX-License-Identifier: MIT
 *
 * Serial Console: port I/O on worker threads (see serial_io.hpp).
 */
#include "serial_io.hpp"

#include "fsutil.hpp"

#include <fcntl.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#include <cerrno>
#include <utility>

namespace sc {

namespace tty2 {
int set_speed(int fd, int baud); /* tty_speed.cpp */
int get_speed(int fd);
} // namespace tty2

namespace {

constexpr std::size_t kReadChunk = 4096;
constexpr std::size_t kReadBurst = 64 * 1024; /* at most this much per poll round: the queue and the log keep up */
constexpr int kPollMs = 200;
constexpr int kResetPulseMs = 100;
constexpr int kSettleMs = 60; /* SpeedFinder: after a speed change, before listening */

speed_t speed_constant(int baud)
{
    switch (baud) {
    case 50: return B50;
    case 75: return B75;
    case 110: return B110;
    case 300: return B300;
    case 600: return B600;
    case 1200: return B1200;
    case 2400: return B2400;
    case 4800: return B4800;
    case 9600: return B9600;
    case 19200: return B19200;
    case 38400: return B38400;
    case 57600: return B57600;
    case 115200: return B115200;
    case 230400: return B230400;
    case 460800: return B460800;
    case 500000: return B500000;
    case 576000: return B576000;
    case 921600: return B921600;
    case 1000000: return B1000000;
    case 1500000: return B1500000;
    case 2000000: return B2000000;
    default: return B0;
    }
}

bool write_all(int fd, const char *data, std::size_t size)
{
    while (size > 0) {
        const ssize_t n = write(fd, data, size);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return false;
        data += n;
        size -= static_cast<std::size_t>(n);
    }
    return true;
}

void sleep_ms(int ms)
{
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = static_cast<long>(ms % 1000) * 1000000L;
    while (nanosleep(&ts, &ts) != 0 && errno == EINTR) {
    }
}

/* errors after which the port is gone (unplugged, hung up) */
bool port_gone(int err)
{
    return err == EIO || err == ENXIO || err == ENODEV || err == EBADF || err == EPIPE || err == ENOENT;
}

} // namespace

std::int64_t now_ms()
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<std::int64_t>(ts.tv_sec) * 1000 + ts.tv_nsec / 1000000;
}

int open_port(const std::string &path, int *err)
{
    const int fd = open(path.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) {
        if (err) *err = errno;
        return -1;
    }
    if (!isatty(fd)) {
        if (err) *err = ENOTTY;
        close(fd);
        return -1;
    }
    ioctl(fd, TIOCEXCL); /* no second program on the same port (EBUSY for it); a pseudo-terminal may refuse: fine */
    if (err) *err = 0;
    return fd;
}

void close_port(int fd)
{
    if (fd < 0) return;
    ioctl(fd, TIOCNXCL);
    close(fd);
}

bool configure_port(int fd, int baud, const Frame &frame)
{
    struct termios tio;
    if (tcgetattr(fd, &tio) != 0) return false;
    cfmakeraw(&tio);
    tio.c_iflag &= ~static_cast<tcflag_t>(IXON | IXOFF | IXANY | IGNPAR | PARMRK | INPCK);
    if (frame.parity != 'N') tio.c_iflag |= INPCK; /* a parity error arrives as NUL (no PARMRK, no IGNPAR) */
    tio.c_cflag &= ~static_cast<tcflag_t>(CSIZE | PARENB | PARODD | CSTOPB | CRTSCTS);
    tio.c_cflag |= CLOCAL | CREAD;
    switch (frame.data) {
    case 5: tio.c_cflag |= CS5; break;
    case 6: tio.c_cflag |= CS6; break;
    case 7: tio.c_cflag |= CS7; break;
    default: tio.c_cflag |= CS8; break;
    }
    if (frame.parity == 'E') tio.c_cflag |= PARENB;
    if (frame.parity == 'O') tio.c_cflag |= PARENB | PARODD;
    if (frame.stop == 2) tio.c_cflag |= CSTOPB;
    /* VMIN 1: with O_NONBLOCK an empty queue reads -1 EAGAIN and 0 means a hang-up (VMIN 0 would read 0 for both) */
    tio.c_cc[VMIN] = 1;
    tio.c_cc[VTIME] = 0;
    const speed_t constant = speed_constant(baud);
    if (constant != B0) {
        cfsetispeed(&tio, constant);
        cfsetospeed(&tio, constant);
    }
    if (tcsetattr(fd, TCSANOW, &tio) != 0) return false;
    if (constant == B0 && tty2::set_speed(fd, baud) != 0) return false;
    tcflush(fd, TCIFLUSH);
    return true;
}

int port_speed(int fd)
{
    return tty2::get_speed(fd);
}

bool pulse_reset(int fd)
{
    int lines = 0;
    if (ioctl(fd, TIOCMGET, &lines) != 0) return false;
    int pulse = (lines & ~TIOCM_DTR) | TIOCM_RTS;
    if (ioctl(fd, TIOCMSET, &pulse) != 0) return false;
    sleep_ms(kResetPulseMs);
    return ioctl(fd, TIOCMSET, &lines) == 0;
}

const char *link_state_name(LinkState state)
{
    switch (state) {
    case LinkState::Opening: return "Opening";
    case LinkState::Open: return "Connected";
    case LinkState::Lost: return "Port lost";
    case LinkState::Failed: return "Failed";
    case LinkState::Closed: return "Closed";
    }
    return "";
}

/* ---------------------------------------------------------------- Session */

Session::Session(SessionOptions options) : options_(std::move(options))
{
    log_wanted_ = options_.log;
    log_path_wanted_ = options_.log_path;
    log_changed_ = options_.log;
}

Session::~Session()
{
    stop();
}

void Session::start()
{
    if (thread_.joinable()) return;
    if (pipe2(wake_, O_NONBLOCK | O_CLOEXEC) != 0) wake_[0] = wake_[1] = -1;
    thread_ = std::thread([this] { run(); });
}

void Session::stop()
{
    stop_.store(true);
    wake();
    if (thread_.joinable()) thread_.join();
    for (int &fd : wake_) {
        if (fd >= 0) close(fd);
        fd = -1;
    }
}

void Session::wake()
{
    if (wake_[1] >= 0) {
        const char c = 1;
        ssize_t n = write(wake_[1], &c, 1);
        (void)n;
    }
}

void Session::send(const std::string &bytes)
{
    if (bytes.empty()) return;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (status_.state != LinkState::Open) return; /* nothing is sent into a lost port later by surprise */
        tx_queue_ += bytes;
    }
    wake();
}

void Session::set_log(bool on, const std::string &path)
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        log_wanted_ = on;
        if (!path.empty()) log_path_wanted_ = path;
        log_changed_ = true;
    }
    wake();
}

void Session::reset_board()
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        reset_wanted_ = true;
    }
    wake();
}

std::size_t Session::take(std::string &out, std::size_t max)
{
    std::lock_guard<std::mutex> lock(mutex_);
    const std::size_t n = std::min(max, rx_queue_.size());
    out.append(rx_queue_, 0, n);
    rx_queue_.erase(0, n);
    return n;
}

SessionStatus Session::status() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return status_;
}

bool Session::try_open(const std::string &path, int *err)
{
    const int fd = open_port(path, err);
    if (fd < 0) return false;
    if (!configure_port(fd, options_.baud, options_.frame)) {
        if (err) *err = errno ? errno : EIO;
        close_port(fd);
        return false;
    }
    fd_ = fd;
    std::lock_guard<std::mutex> lock(mutex_);
    status_.state = LinkState::Open;
    status_.path = path;
    status_.error = 0;
    return true;
}

void Session::lose_port()
{
    close_port(fd_);
    fd_ = -1;
    std::lock_guard<std::mutex> lock(mutex_);
    status_.state = LinkState::Lost;
    tx_queue_.clear();
}

/* Log on / off as asked (worker thread): a new file each time it is switched on. */
void Session::apply_log()
{
    bool want = false;
    std::string path;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!log_changed_) return;
        log_changed_ = false;
        want = log_wanted_;
        path = log_path_wanted_;
    }
    if (log_fd_ >= 0) close(log_fd_);
    log_fd_ = -1;
    bool error = false;
    if (want && !path.empty()) {
        fs::make_dirs(fs::parent(path));
        log_fd_ = open(path.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
        error = log_fd_ < 0;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    status_.log_on = log_fd_ >= 0;
    status_.log_error = error;
    if (want) status_.log_path = path;
}

void Session::wait_ms(int ms)
{
    struct pollfd p;
    p.fd = wake_[0];
    p.events = POLLIN;
    if (poll(&p, wake_[0] >= 0 ? 1 : 0, ms) > 0) {
        char buf[64];
        while (read(wake_[0], buf, sizeof(buf)) > 0) {
        }
    }
}

void Session::run()
{
    apply_log();
    std::string path = options_.path.empty() ? resolve_port(options_.roots, options_.id) : options_.path;
    int err = ENOENT;
    if (path.empty() || !try_open(path, &err)) {
        if (!port_gone(err)) { /* no permission, busy, not a tty: waiting will not help */
            std::lock_guard<std::mutex> lock(mutex_);
            status_.state = LinkState::Failed;
            status_.error = err;
            return;
        }
        lose_port();
    }

    char buf[kReadChunk];
    while (!stop_.load()) {
        apply_log();
        if (fd_ < 0) { /* lost: look for the same port again */
            wait_ms(options_.retry_ms);
            if (stop_.load()) break;
            const std::string again = options_.id.empty() ? options_.path : resolve_port(options_.roots, options_.id);
            int e = 0;
            if (!again.empty() && try_open(again, &e)) {
                std::lock_guard<std::mutex> lock(mutex_);
                ++status_.reconnects;
            }
            continue;
        }

        bool reset = false, has_tx = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            reset = reset_wanted_;
            reset_wanted_ = false;
            has_tx = !tx_queue_.empty();
        }
        if (reset) {
            const bool ok = pulse_reset(fd_);
            std::lock_guard<std::mutex> lock(mutex_);
            status_.note = ok ? "Reset pulse sent (DTR / RTS)" : "No DTR / RTS lines on this port";
        }

        struct pollfd fds[2];
        fds[0].fd = fd_;
        fds[0].events = static_cast<short>(POLLIN | (has_tx ? POLLOUT : 0));
        fds[0].revents = 0;
        fds[1].fd = wake_[0];
        fds[1].events = POLLIN;
        fds[1].revents = 0;
        const int ready = poll(fds, wake_[0] >= 0 ? 2 : 1, kPollMs);
        if (ready < 0 && errno != EINTR) {
            lose_port();
            continue;
        }
        if (fds[1].revents & POLLIN) {
            char drain[64];
            while (read(wake_[0], drain, sizeof(drain)) > 0) {
            }
        }
        bool lost = false;
        if (fds[0].revents & POLLIN) {
            std::size_t burst = 0;
            while (burst < kReadBurst) {
                const ssize_t n = read(fd_, buf, sizeof(buf));
                if (n > 0) {
                    burst += static_cast<std::size_t>(n);
                    if (log_fd_ >= 0 && !write_all(log_fd_, buf, static_cast<std::size_t>(n))) {
                        close(log_fd_);
                        log_fd_ = -1;
                        std::lock_guard<std::mutex> lock(mutex_);
                        status_.log_on = false;
                        status_.log_error = true;
                    }
                    std::lock_guard<std::mutex> lock(mutex_);
                    status_.rx += static_cast<std::uint64_t>(n);
                    rx_queue_.append(buf, static_cast<std::size_t>(n));
                    if (rx_queue_.size() > options_.ui_cap) { /* the display is behind: it skips the oldest */
                        const std::size_t extra = rx_queue_.size() - options_.ui_cap;
                        rx_queue_.erase(0, extra);
                        status_.ui_dropped += extra;
                    }
                    continue;
                }
                if (n < 0 && errno == EINTR) continue;
                if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
                lost = true; /* 0 = hang-up, or EIO / ENXIO: unplugged */
                break;
            }
        } else if (fds[0].revents & (POLLHUP | POLLERR | POLLNVAL)) {
            lost = true;
        }
        if (!lost && (fds[0].revents & POLLOUT)) {
            std::string chunk;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                chunk = tx_queue_.substr(0, kReadChunk);
            }
            const ssize_t n = chunk.empty() ? 0 : write(fd_, chunk.data(), chunk.size());
            if (n > 0) {
                std::lock_guard<std::mutex> lock(mutex_);
                tx_queue_.erase(0, static_cast<std::size_t>(n));
                status_.tx += static_cast<std::uint64_t>(n);
            } else if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
                lost = true;
            }
        }
        if (lost) lose_port();
    }
    close_port(fd_);
    fd_ = -1;
    if (log_fd_ >= 0) close(log_fd_);
    log_fd_ = -1;
    std::lock_guard<std::mutex> lock(mutex_);
    if (status_.state != LinkState::Failed) status_.state = LinkState::Closed;
    status_.log_on = false;
}

/* ---------------------------------------------------------------- SpeedFinder */

SpeedFinder::SpeedFinder(std::string path, Frame frame, int recommended, bool enter_pass, int listen_ms)
    : path_(std::move(path)), frame_(frame), recommended_(recommended), enter_pass_(enter_pass), listen_ms_(listen_ms)
{
}

SpeedFinder::~SpeedFinder()
{
    cancel();
    if (thread_.joinable()) thread_.join();
}

void SpeedFinder::start()
{
    if (thread_.joinable()) return;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        progress_.running = true;
        Detector d(recommended_, enter_pass_);
        progress_.text = d.progress_text();
        progress_.count = d.count();
        progress_.rate = d.rate();
    }
    thread_ = std::thread([this] { run(); });
}

void SpeedFinder::cancel()
{
    cancel_.store(true);
}

FinderProgress SpeedFinder::progress() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return progress_;
}

void SpeedFinder::run()
{
    Detector detector(recommended_, enter_pass_);
    int err = 0;
    const int fd = open_port(path_, &err);
    if (fd < 0) {
        std::lock_guard<std::mutex> lock(mutex_);
        progress_.running = false;
        progress_.done = true;
        progress_.error = err;
        return;
    }
    int failure = 0;
    char buf[1024];
    while (!detector.done() && !cancel_.load()) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            progress_.text = detector.progress_text();
            progress_.index = detector.index();
            progress_.count = detector.count();
            progress_.rate = detector.rate();
            progress_.pass = detector.pass();
        }
        if (!configure_port(fd, detector.rate(), frame_)) {
            failure = errno ? errno : EIO;
            break;
        }
        // a character on the wire while the speed changed arrives broken: let it pass, then start clean
        for (int waited = 0; waited < kSettleMs && !cancel_.load(); waited += 10) sleep_ms(10);
        tcflush(fd, TCIFLUSH);
        if (detector.send_enter()) {
            const char cr = '\r';
            ssize_t n = write(fd, &cr, 1);
            (void)n;
        }
        std::string got;
        const std::int64_t end = now_ms() + listen_ms_;
        while (!cancel_.load() && got.size() < 8192) {
            const std::int64_t left = end - now_ms();
            if (left <= 0) break;
            struct pollfd p;
            p.fd = fd;
            p.events = POLLIN;
            p.revents = 0;
            const int ready = poll(&p, 1, static_cast<int>(left < 50 ? left : 50));
            if (ready < 0 && errno == EINTR) continue;
            if (ready <= 0) continue;
            if (p.revents & POLLIN) {
                const ssize_t n = read(fd, buf, sizeof(buf));
                if (n > 0) got.append(buf, static_cast<std::size_t>(n));
                else if (n == 0 || (errno != EAGAIN && errno != EINTR)) {
                    failure = ENODEV;
                    break;
                }
            } else if (p.revents & (POLLHUP | POLLERR | POLLNVAL)) {
                failure = ENODEV;
                break;
            }
        }
        if (failure) break;
        if (cancel_.load()) detector.cancel();
        else detector.record(got);
    }
    if (cancel_.load()) detector.cancel();
    close_port(fd);
    std::lock_guard<std::mutex> lock(mutex_);
    progress_.running = false;
    progress_.done = true;
    progress_.error = failure;
    progress_.result = detector.result();
    progress_.text = failure ? "The port went away" : detector.progress_text();
}

} // namespace sc
