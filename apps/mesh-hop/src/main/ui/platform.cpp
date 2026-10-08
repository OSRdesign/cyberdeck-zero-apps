/*
 * SPDX-License-Identifier: MIT
 */

#include "platform.hpp"

#include "png_writer.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <linux/fb.h>
#include <linux/input.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

namespace meshhop {

uint64_t mono_ms()
{
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}

namespace {

constexpr int kBufRows = 48;       // rows of one partial render buffer

uint32_t tick_cb()
{
    return static_cast<uint32_t>(mono_ms());
}

void flush_cb(lv_display_t *d, const lv_area_t *area, uint8_t *px)
{
    static_cast<Platform *>(lv_display_get_user_data(d))->flush(area, px);
    lv_display_flush_ready(d);
}

void pointer_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    static_cast<Platform *>(lv_indev_get_user_data(indev))->read_pointer(data);
}

#define BIT_SET(bits, n) (((bits)[(n) / (8 * sizeof(unsigned long))] >> ((n) % (8 * sizeof(unsigned long)))) & 1UL)

long now_s()
{
    return static_cast<long>(std::time(nullptr));
}

/* Is this evdev node a touch screen? Multitouch X and Y, and not a keyboard. 2: its name says so, 1: any other, 0: no. */
int touch_score(int fd)
{
    unsigned long abs_bits[(ABS_MAX + 8 * sizeof(unsigned long)) / (8 * sizeof(unsigned long))];
    unsigned long key_bits[(KEY_MAX + 8 * sizeof(unsigned long)) / (8 * sizeof(unsigned long))];
    std::memset(abs_bits, 0, sizeof(abs_bits));
    std::memset(key_bits, 0, sizeof(key_bits));
    if (ioctl(fd, EVIOCGBIT(EV_ABS, sizeof(abs_bits)), abs_bits) < 0) return 0;
    if (!BIT_SET(abs_bits, ABS_MT_POSITION_X) || !BIT_SET(abs_bits, ABS_MT_POSITION_Y)) return 0;
    if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(key_bits)), key_bits) >= 0 && BIT_SET(key_bits, KEY_A)) return 0;
    char name[128] = "";
    if (ioctl(fd, EVIOCGNAME(sizeof(name) - 1), name) > 0) {
        for (char *c = name; *c; ++c)
            if (*c >= 'A' && *c <= 'Z') *c = static_cast<char>(*c + 32);
        if (std::strstr(name, "goodix") || std::strstr(name, "touchscreen")) return 2;
    }
    return 1;
}

/* A real keyboard: letters, Enter and Space, not the launcher's virtual keyboard and not a virtual device. */
bool is_keyboard(int fd)
{
    unsigned long key_bits[(KEY_MAX + 8 * sizeof(unsigned long)) / (8 * sizeof(unsigned long))];
    std::memset(key_bits, 0, sizeof(key_bits));
    if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(key_bits)), key_bits) < 0) return false;
    if (!BIT_SET(key_bits, KEY_A) || !BIT_SET(key_bits, KEY_Z) || !BIT_SET(key_bits, KEY_ENTER) || !BIT_SET(key_bits, KEY_SPACE))
        return false;
    char name[128] = "";
    if (ioctl(fd, EVIOCGNAME(sizeof(name) - 1), name) > 0 && std::strstr(name, "applaunch")) return false;
    struct input_id id;
    if (ioctl(fd, EVIOCGID, &id) == 0 && id.bustype == BUS_VIRTUAL) return false;
    return true;
}

} // namespace

Platform::Platform() : keys_(detect_layout()) {}

Platform::~Platform()
{
    if (touch_fd_ >= 0) close(touch_fd_);
    if (kbd_fd_ >= 0) close(kbd_fd_);
    if (fb_map_) munmap(fb_map_, fb_size_);
    if (fb_fd_ >= 0) close(fb_fd_);
    std::free(buf1_);
    std::free(buf2_);
}

bool Platform::open_fb()
{
    const char *path = std::getenv("MESHHOP_FB");
    fb_fd_ = open(path && path[0] ? path : "/dev/fb0", O_RDWR);
    if (fb_fd_ < 0) {
        std::fprintf(stderr, "mesh-hop: cannot open the framebuffer: %s\n", std::strerror(errno));
        return false;
    }
    struct fb_var_screeninfo var;
    struct fb_fix_screeninfo fix;
    if (ioctl(fb_fd_, FBIOGET_VSCREENINFO, &var) || ioctl(fb_fd_, FBIOGET_FSCREENINFO, &fix)) {
        std::fprintf(stderr, "mesh-hop: framebuffer info: %s\n", std::strerror(errno));
        return false;
    }
    fb_w_ = static_cast<int>(var.xres);
    fb_h_ = static_cast<int>(var.yres);
    fb_bytes_ = static_cast<int>(var.bits_per_pixel) / 8;
    fb_stride_ = static_cast<int>(fix.line_length);
    if (fb_bytes_ != 4 && fb_bytes_ != 2) {
        std::fprintf(stderr, "mesh-hop: unsupported framebuffer depth %d\n", static_cast<int>(var.bits_per_pixel));
        return false;
    }
    fb_size_ = static_cast<size_t>(fb_stride_) * var.yres_virtual;
    void *m = mmap(nullptr, fb_size_, PROT_READ | PROT_WRITE, MAP_SHARED, fb_fd_, 0);
    if (m == MAP_FAILED) {
        std::fprintf(stderr, "mesh-hop: framebuffer mmap: %s\n", std::strerror(errno));
        return false;
    }
    fb_map_ = static_cast<uint8_t *>(m);
    std::fprintf(stderr, "mesh-hop: framebuffer %dx%d, %d bytes per pixel\n", fb_w_, fb_h_, fb_bytes_);
    return true;
}

bool Platform::init(bool headless)
{
    headless_ = headless;
    if (!headless_ && !open_fb()) return false;
    if (headless_) shadow_.assign(static_cast<size_t>(kScreenW) * kScreenH, 0);

    lv_init();
    lv_tick_set_cb(tick_cb);

    disp_ = lv_display_create(kScreenW, kScreenH);
    lv_display_set_color_format(disp_, LV_COLOR_FORMAT_XRGB8888);
    const size_t bytes = static_cast<size_t>(kScreenW) * kBufRows * 4;
    buf1_ = static_cast<uint8_t *>(std::malloc(bytes));
    buf2_ = static_cast<uint8_t *>(std::malloc(bytes));
    lv_display_set_buffers(disp_, buf1_, buf2_, static_cast<uint32_t>(bytes), LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(disp_, flush_cb);
    lv_display_set_user_data(disp_, this);

    indev_ = lv_indev_create();
    lv_indev_set_type(indev_, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(indev_, pointer_cb);
    lv_indev_set_user_data(indev_, this);
    lv_indev_set_scroll_limit(indev_, 12);
    lv_indev_set_scroll_throw(indev_, 20);
    return true;
}

void Platform::flush(const lv_area_t *area, const uint8_t *px)
{
    const int w = area->x2 - area->x1 + 1;
    const int rows = area->y2 - area->y1 + 1;
    if (headless_) {
        for (int y = 0; y < rows; ++y) {
            const int sy = area->y1 + y;
            if (sy < 0 || sy >= kScreenH) continue;
            std::memcpy(&shadow_[static_cast<size_t>(sy) * kScreenW + static_cast<size_t>(area->x1)], px + static_cast<size_t>(y) * w * 4,
                        static_cast<size_t>(w) * 4);
        }
        return;
    }
    if (!fb_map_) return;
    for (int y = 0; y < rows; ++y) {
        const int sy = area->y1 + y;
        if (sy < 0 || sy >= fb_h_) continue;
        const uint8_t *src = px + static_cast<size_t>(y) * w * 4;
        uint8_t *dst = fb_map_ + static_cast<size_t>(sy) * fb_stride_ + static_cast<size_t>(area->x1) * fb_bytes_;
        const int cw = std::min(w, fb_w_ - area->x1);
        if (cw <= 0) continue;
        if (fb_bytes_ == 4) {
            std::memcpy(dst, src, static_cast<size_t>(cw) * 4);
        } else {
            for (int x = 0; x < cw; ++x, src += 4) {
                const uint16_t v = static_cast<uint16_t>(((src[2] >> 3) << 11) | ((src[1] >> 2) << 5) | (src[0] >> 3));
                std::memcpy(dst + static_cast<size_t>(x) * 2, &v, 2);
            }
        }
    }
}

bool Platform::screenshot(const std::string &path) const
{
    if (headless_) return write_png(path, shadow_.data(), kScreenW, kScreenH, kScreenW);
    // the real framebuffer: read it back
    if (!fb_map_ || fb_bytes_ != 4) return false;
    std::vector<uint32_t> img(static_cast<size_t>(kScreenW) * kScreenH, 0);
    for (int y = 0; y < kScreenH && y < fb_h_; ++y)
        std::memcpy(&img[static_cast<size_t>(y) * kScreenW], fb_map_ + static_cast<size_t>(y) * fb_stride_,
                    static_cast<size_t>(std::min(kScreenW, fb_w_)) * 4);
    return write_png(path, img.data(), kScreenW, kScreenH, kScreenW);
}

/* ------------------------------------------------------------------ touch */

void Platform::push_sample(bool pressed, int x, int y)
{
    x = std::clamp(x, 0, kScreenW - 1);
    y = std::clamp(y, 0, kScreenH - 1);
    if (samples_.size() > 256) samples_.pop_front();
    samples_.push_back({pressed, x, y});
    p_pressed_ = pressed;
    p_x_ = x;
    p_y_ = y;
}

void Platform::inject_touch(bool pressed, int x, int y)
{
    push_sample(pressed, x, y);
}

void Platform::inject_key(const KeyEvent &e)
{
    if (on_key) on_key(e);
}

void Platform::read_pointer(lv_indev_data_t *data)
{
    if (!samples_.empty()) {
        const Sample s = samples_.front();
        samples_.pop_front();
        last_pressed_ = s.pressed;
        last_x_ = s.x;
        last_y_ = s.y;
    }
    data->point.x = last_x_;
    data->point.y = last_y_;
    data->state = last_pressed_ ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
    data->continue_reading = !samples_.empty();
}

void Platform::open_touch()
{
    const long now = now_s();
    if (now == touch_retry_) return;                    // at most once a second while it is missing
    touch_retry_ = now;
    const char *path = std::getenv("MESHHOP_TOUCH");
    if (path && path[0]) {
        touch_fd_ = open(path, O_RDONLY | O_NONBLOCK);
    } else {
        int best_fd = -1, best = 0;
        for (int i = 0; i < 32; ++i) {
            char p[48];
            std::snprintf(p, sizeof(p), "/dev/input/event%d", i);
            const int fd = open(p, O_RDONLY | O_NONBLOCK);
            if (fd < 0) continue;
            const int score = touch_score(fd);
            if (score > best) {
                if (best_fd >= 0) close(best_fd);
                best_fd = fd;
                best = score;
                if (score == 2) break;
            } else {
                close(fd);
            }
        }
        touch_fd_ = best_fd;
    }
    if (touch_fd_ < 0) return;
    struct input_absinfo info;
    if (ioctl(touch_fd_, EVIOCGABS(ABS_MT_POSITION_X), &info) == 0) { min_x_ = info.minimum; max_x_ = info.maximum; }
    if (ioctl(touch_fd_, EVIOCGABS(ABS_MT_POSITION_Y), &info) == 0) { min_y_ = info.minimum; max_y_ = info.maximum; }
    const char *s;
    swap_xy_ = (s = std::getenv("APPLAUNCH_TOUCH_SWAP_XY")) ? std::atoi(s) != 0 : true;
    inv_x_ = (s = std::getenv("APPLAUNCH_TOUCH_INVERT_X")) ? std::atoi(s) != 0 : false;
    inv_y_ = (s = std::getenv("APPLAUNCH_TOUCH_INVERT_Y")) ? std::atoi(s) != 0 : true;
    slot_ = 0;
    t_active_ = false;
}

void Platform::read_touch()
{
    if (touch_fd_ < 0) open_touch();
    if (touch_fd_ < 0) return;
    struct input_event ev;
    ssize_t n;
    while ((n = read(touch_fd_, &ev, sizeof(ev))) == static_cast<ssize_t>(sizeof(ev))) {
        if (ev.type == EV_ABS) {
            if (ev.code == ABS_MT_SLOT) slot_ = ev.value;
            else if (slot_ != 0) continue;                  // one finger: the first slot only
            else if (ev.code == ABS_MT_TRACKING_ID) t_active_ = ev.value >= 0;
            else if (ev.code == ABS_MT_POSITION_X) t_x_ = ev.value;
            else if (ev.code == ABS_MT_POSITION_Y) t_y_ = ev.value;
        } else if (ev.type == EV_SYN && ev.code == SYN_REPORT) {
            auto norm = [](int v, int lo, int hi) {
                if (hi <= lo) return 0.0;
                return std::clamp(static_cast<double>(v - lo) / static_cast<double>(hi - lo), 0.0, 1.0);
            };
            double rx = norm(t_x_, min_x_, max_x_), ry = norm(t_y_, min_y_, max_y_);
            if (swap_xy_) std::swap(rx, ry);
            if (inv_x_) rx = 1 - rx;
            if (inv_y_) ry = 1 - ry;
            const int x = static_cast<int>(rx * (kScreenW - 1) + 0.5), y = static_cast<int>(ry * (kScreenH - 1) + 0.5);
            if (t_active_ != p_pressed_ || (t_active_ && (x != p_x_ || y != p_y_))) {
                // the queue keeps every press / move / release in order; last_* are updated when LVGL reads them
                push_sample(t_active_, x, y);
            }
        }
    }
    if (n < 0 && errno != EAGAIN && errno != EINTR) {       // the device went away
        close(touch_fd_);
        touch_fd_ = -1;
        if (p_pressed_) push_sample(false, p_x_, p_y_);
    }
}

/* ------------------------------------------------------------------ keyboard */

void Platform::open_keyboard()
{
    const long now = now_s();
    if (now == kbd_retry_) return;
    kbd_retry_ = now;
    const char *path = std::getenv("MESHHOP_KEYBOARD");
    kbd_fd_ = open(path && path[0] ? path : "/dev/input/bt-keyboard", O_RDONLY | O_NONBLOCK);
    if (kbd_fd_ < 0 && !(path && path[0])) {
        // no udev alias: any real keyboard (a USB one, for example)
        for (int i = 0; i < 32 && kbd_fd_ < 0; ++i) {
            char p[48];
            std::snprintf(p, sizeof(p), "/dev/input/event%d", i);
            const int fd = open(p, O_RDONLY | O_NONBLOCK);
            if (fd < 0) continue;
            if (is_keyboard(fd)) kbd_fd_ = fd;
            else close(fd);
        }
    }
    if (kbd_fd_ >= 0) keys_.reset();
}

void Platform::read_keyboard()
{
    if (kbd_fd_ < 0) open_keyboard();
    if (kbd_fd_ < 0) return;
    struct input_event ev;
    ssize_t n;
    while ((n = read(kbd_fd_, &ev, sizeof(ev))) == static_cast<ssize_t>(sizeof(ev))) {
        if (ev.type != EV_KEY) continue;
        KeyEvent k;
        if (keys_.feed(ev.code, ev.value, k) && on_key) on_key(k);
    }
    if (n < 0 && errno != EAGAIN && errno != EINTR) {       // the keyboard went to sleep
        close(kbd_fd_);
        kbd_fd_ = -1;
        keys_.reset();
    }
}

void Platform::poll_input()
{
    if (headless_) return;
    read_keyboard();
    read_touch();
}

void Platform::wait(int ms)
{
    if (ms <= 0) return;
    struct pollfd fds[2];
    int n = 0;
    if (touch_fd_ >= 0) fds[n++] = {touch_fd_, POLLIN, 0};
    if (kbd_fd_ >= 0) fds[n++] = {kbd_fd_, POLLIN, 0};
    if (n == 0) {
        usleep(static_cast<useconds_t>(ms) * 1000);
        return;
    }
    ::poll(fds, static_cast<nfds_t>(n), ms);
}

} // namespace meshhop
