/*
 * SPDX-License-Identifier: MIT
 *
 * Runs the unmodified viz1090 (SDL2) on a Raspberry Pi with only a legacy framebuffer (/dev/fb0, no KMS):
 *   - SDL_CreateRenderer is forced to the software renderer (SDL_VIDEODRIVER=offscreen has no GPU),
 *   - every SDL_RenderPresent copies the finished frame to /dev/fb0,
 *   - SDL_PollEvent also reads the touch screen and a keyboard through evdev and turns them into the SDL
 *     events viz1090 handles (finger drag = pan, two-finger pinch = zoom, tap, + / - keys); the X button (top right) and Esc quit.
 *
 * Use:  SDL_VIDEODRIVER=offscreen LD_PRELOAD=libviz_fb.so ./viz1090 --screensize 640 480 ...
 * Environment: VIZ_TOUCH (default: found by scanning /dev/input/event*), VIZ_KEYBOARD (default /dev/input/bt-keyboard),
 *              APPLAUNCH_TOUCH_SWAP_XY / _INVERT_X / _INVERT_Y (as for the launcher), VIZ_FB (default /dev/fb0).
 */
#define _GNU_SOURCE
#include <SDL2/SDL.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <linux/input.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

/* ------------------------------------------------------------ framebuffer */

static int fb_fd = -1;
static uint8_t *fb_map;
static size_t fb_size;
static int fb_w, fb_h, fb_stride, fb_bytes;   /* visible size, bytes per row, bytes per pixel */
static int fb_tried;
static uint8_t *frame;
static size_t frame_size;

static void fb_init(void)
{
    if (fb_tried) return;
    fb_tried = 1;
    const char *path = getenv("VIZ_FB");
    fb_fd = open(path && path[0] ? path : "/dev/fb0", O_RDWR);
    if (fb_fd < 0) { perror("[viz_fb] open framebuffer"); return; }
    struct fb_var_screeninfo var;
    struct fb_fix_screeninfo fix;
    if (ioctl(fb_fd, FBIOGET_VSCREENINFO, &var) || ioctl(fb_fd, FBIOGET_FSCREENINFO, &fix)) {
        perror("[viz_fb] framebuffer info");
        close(fb_fd);
        fb_fd = -1;
        return;
    }
    fb_w = (int)var.xres;
    fb_h = (int)var.yres;
    fb_bytes = (int)var.bits_per_pixel / 8;
    fb_stride = (int)fix.line_length;
    fb_size = (size_t)fb_stride * var.yres_virtual;
    fb_map = mmap(NULL, fb_size, PROT_READ | PROT_WRITE, MAP_SHARED, fb_fd, 0);
    if (fb_map == MAP_FAILED) { perror("[viz_fb] mmap"); fb_map = NULL; close(fb_fd); fb_fd = -1; }
    else fprintf(stderr, "[viz_fb] framebuffer %dx%d, %d bytes per pixel\n", fb_w, fb_h, fb_bytes);
}

/* close button (top right): drawn into every frame; a tap on it quits */
static int frame_w = 640, frame_h = 480;
#define BTN_SIZE 56
#define BTN_MARGIN 6

static int close_button_enabled(void)
{
    static int enabled = -1;
    if (enabled < 0) {
        const char *v = getenv("VIZ_CLOSE_BUTTON");
        enabled = v && v[0] && v[0] != '0';
    }
    return enabled;
}

static void draw_close_button(uint8_t *pixels, int w, int h)
{
    if (!close_button_enabled()) return;
    uint32_t *p = (uint32_t *)pixels;
    const int x0 = w - BTN_SIZE - BTN_MARGIN, y0 = BTN_MARGIN;
    if (x0 < 0 || y0 + BTN_SIZE > h) return;
    for (int y = 0; y < BTN_SIZE; ++y)
        for (int x = 0; x < BTN_SIZE; ++x) {
            const int border = x < 2 || y < 2 || x >= BTN_SIZE - 2 || y >= BTN_SIZE - 2;
            p[(size_t)(y0 + y) * w + x0 + x] = border ? 0xFFB0B0B0u : 0xFF1C1C1Cu;
        }
    for (int i = 12; i < BTN_SIZE - 12; ++i)          /* the X: two diagonals, 4 px thick */
        for (int t = -2; t <= 1; ++t) {
            p[(size_t)(y0 + i) * w + x0 + i + t] = 0xFFFFFFFFu;
            p[(size_t)(y0 + i) * w + x0 + (BTN_SIZE - 1 - i) + t] = 0xFFFFFFFFu;
        }
}

#include "cp0_statusbar.h"

/* The status bar is the launcher's own (cp0_statusbar.c, shared with the launcher's native screens). */
static cp0_statusbar_t *status_bar;
static cp0_statusbar_state_t status_state;
static Uint32 status_read_at;
static int status_failed;

static int status_bar_enabled(void)
{
    static int enabled = -1;
    if (enabled < 0) {
        const char *v = getenv("VIZ_STATUS_BAR");
        enabled = v && v[0] && v[0] != '0';
    }
    return enabled && !status_failed;
}

static void draw_status_bar(uint8_t *pixels, int w, int h, int shift_left, int top)
{
    (void)h;
    if (!status_bar_enabled()) return;
    if (!status_bar) {
        const char *text_font = getenv("VIZ_FONT");
        const char *icon_font = getenv("VIZ_ICON_FONT");
        status_bar = cp0_statusbar_create(text_font && text_font[0] ? text_font : "/opt/viz1090/font/Montserrat-Medium.ttf",
                                          icon_font && icon_font[0] ? icon_font : "/opt/viz1090/font/FontAwesome5-Solid+Brands+Regular.woff");
        if (!status_bar) { status_failed = 1; return; }
    }
    const Uint32 now = SDL_GetTicks();
    if (!status_read_at || now - status_read_at > 2000) {
        cp0_statusbar_read_state(&status_state);
        status_read_at = now;
    } else {                                   /* the clock follows the system time every frame */
        const time_t t = time(NULL);
        strftime(status_state.clock, sizeof(status_state.clock), "%H:%M", localtime(&t));
    }
    uint32_t *p = (uint32_t *)pixels;
    for (int i = 0; i < w * CP0_STATUSBAR_HEIGHT && i < w * h; ++i) p[i] |= 0xFF000000u;   /* the strip is opaque */
    /* a dim backing only over the map (next to the close button); plain screens look exactly like the home grid */
    cp0_statusbar_render(status_bar, p, w, w, shift_left, top, close_button_enabled() ? 0x99 : 0, &status_state);
}

static void present_to_fb(SDL_Renderer *renderer)
{
    fb_init();
    if (!fb_map) return;
    int w = 0, h = 0;
    if (SDL_GetRendererOutputSize(renderer, &w, &h) != 0 || w <= 0 || h <= 0) return;
    const size_t need = (size_t)w * h * 4;
    if (need > frame_size) {
        free(frame);
        frame = malloc(need);
        frame_size = frame ? need : 0;
        if (!frame) return;
    }
    if (SDL_RenderReadPixels(renderer, NULL, SDL_PIXELFORMAT_ARGB8888, frame, w * 4) != 0) return;
    frame_w = w;
    frame_h = h;
    if (close_button_enabled()) draw_status_bar(frame, w, h, BTN_SIZE + BTN_MARGIN + 12 - 16, 14);   /* beside the close button */
    else draw_status_bar(frame, w, h, 0, 8);
    draw_close_button(frame, w, h);
    const int cw = w < fb_w ? w : fb_w;
    const int ch = h < fb_h ? h : fb_h;
    const int ox = (fb_w - cw) / 2, oy = (fb_h - ch) / 2;
    for (int y = 0; y < ch; ++y) {
        const uint8_t *src = frame + (size_t)y * w * 4;
        uint8_t *dst = fb_map + (size_t)(y + oy) * fb_stride + (size_t)ox * fb_bytes;
        if (fb_bytes == 4) {
            memcpy(dst, src, (size_t)cw * 4);
        } else if (fb_bytes == 2) {
            for (int x = 0; x < cw; ++x, src += 4) {
                const uint16_t v = (uint16_t)(((src[2] >> 3) << 11) | ((src[1] >> 2) << 5) | (src[0] >> 3));
                memcpy(dst + (size_t)x * 2, &v, 2);
            }
        }
    }
}

/* ---------------------------------------------------------------- SDL hooks */

static SDL_Renderer *(*real_create)(SDL_Window *, int, Uint32);
static void (*real_present)(SDL_Renderer *);
static int (*real_poll)(SDL_Event *);

SDL_Renderer *SDL_CreateRenderer(SDL_Window *window, int index, Uint32 flags)
{
    (void)index; (void)flags;
    if (!real_create) real_create = dlsym(RTLD_NEXT, "SDL_CreateRenderer");
    return real_create(window, -1, SDL_RENDERER_SOFTWARE);
}

void SDL_RenderPresent(SDL_Renderer *renderer)
{
    if (!real_present) real_present = dlsym(RTLD_NEXT, "SDL_RenderPresent");
    real_present(renderer);
    present_to_fb(renderer);
}

/* ------------------------------------------------------------------ events */

#define QUEUE 128
static SDL_Event queue[QUEUE];
static int q_head, q_tail;

static void push(const SDL_Event *event)
{
    const int next = (q_tail + 1) % QUEUE;
    if (next == q_head) return;   /* full: drop */
    queue[q_tail] = *event;
    q_tail = next;
}

static void push_key(SDL_Keycode sym, SDL_Scancode scan)
{
    SDL_Event e;
    memset(&e, 0, sizeof(e));
    e.type = SDL_KEYDOWN;
    e.key.state = SDL_PRESSED;
    e.key.keysym.sym = sym;
    e.key.keysym.scancode = scan;
    push(&e);
}

/* touch */
#define SLOTS 5
typedef struct { int active; int x, y; float nx, ny; } slot_t;
static int touch_fd = -1;
static slot_t slots[SLOTS], prev_slots[SLOTS];
static int cur_slot;
static int min_x, max_x, min_y, max_y;
static int swap_xy, inv_x, inv_y;
static float pinch_prev = -1;
static int fingers_down;
static int on_button[SLOTS];        /* the finger went down on the close button */

static float norm(int v, int lo, int hi)
{
    if (hi <= lo) return 0;
    float f = (float)(v - lo) / (float)(hi - lo);
    return f < 0 ? 0 : (f > 1 ? 1 : f);
}

/* Is this evdev node a touch screen? It must report multitouch X and Y and must not be a keyboard.
 * Returns 2 for a name that says so (Goodix / TouchScreen), 1 for any other multitouch device, 0 for none. */
static int touch_score(int fd)
{
    unsigned long abs_bits[(ABS_MAX + 8 * sizeof(unsigned long)) / (8 * sizeof(unsigned long))];
    unsigned long key_bits[(KEY_MAX + 8 * sizeof(unsigned long)) / (8 * sizeof(unsigned long))];
    memset(abs_bits, 0, sizeof(abs_bits));
    memset(key_bits, 0, sizeof(key_bits));
#define HAS(bits, n) (((bits)[(n) / (8 * sizeof(unsigned long))] >> ((n) % (8 * sizeof(unsigned long)))) & 1UL)
    if (ioctl(fd, EVIOCGBIT(EV_ABS, sizeof(abs_bits)), abs_bits) < 0) return 0;
    if (!HAS(abs_bits, ABS_MT_POSITION_X) || !HAS(abs_bits, ABS_MT_POSITION_Y)) return 0;
    if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(key_bits)), key_bits) >= 0 && HAS(key_bits, KEY_A)) return 0;   /* a keyboard */
#undef HAS
    char name[128] = "";
    if (ioctl(fd, EVIOCGNAME(sizeof(name) - 1), name) > 0) {
        for (char *c = name; *c; ++c) if (*c >= 'A' && *c <= 'Z') *c = (char)(*c + 32);
        if (strstr(name, "goodix") || strstr(name, "touchscreen")) return 2;
    }
    return 1;
}

/* The event number depends on the connect order (a Bluetooth keyboard can take any number), so look for the
 * touch screen by what it reports. Returns an open descriptor or -1. */
static int touch_find(void)
{
    int best_fd = -1, best = 0;
    for (int i = 0; i < 32; ++i) {
        char path[48];
        snprintf(path, sizeof(path), "/dev/input/event%d", i);
        const int fd = open(path, O_RDONLY | O_NONBLOCK);
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
    return best_fd;
}

static time_t touch_retry;

static void touch_open(void)
{
    const time_t now = time(NULL);
    if (now == touch_retry) return;                 /* try at most once a second, and only while it is missing */
    touch_retry = now;
    const char *path = getenv("VIZ_TOUCH");
    if (path && path[0]) touch_fd = open(path, O_RDONLY | O_NONBLOCK);   /* explicit override */
    else touch_fd = touch_find();
    if (touch_fd < 0) { touch_fd = -1; return; }
    memset(slots, 0, sizeof(slots));
    memset(prev_slots, 0, sizeof(prev_slots));
    memset(on_button, 0, sizeof(on_button));
    pinch_prev = -1;
    struct input_absinfo info;
    if (ioctl(touch_fd, EVIOCGABS(ABS_MT_POSITION_X), &info) == 0) { min_x = info.minimum; max_x = info.maximum; }
    if (ioctl(touch_fd, EVIOCGABS(ABS_MT_POSITION_Y), &info) == 0) { min_y = info.minimum; max_y = info.maximum; }
    const char *s;
    swap_xy = (s = getenv("APPLAUNCH_TOUCH_SWAP_XY")) ? atoi(s) : 1;
    inv_x = (s = getenv("APPLAUNCH_TOUCH_INVERT_X")) ? atoi(s) : 0;
    inv_y = (s = getenv("APPLAUNCH_TOUCH_INVERT_Y")) ? atoi(s) : 1;
    fprintf(stderr, "[viz_fb] touch screen opened\n");
}

static int button_hit(float nx, float ny)
{
    if (!close_button_enabled()) return 0;
    const float px = nx * frame_w, py = ny * frame_h;
    return px >= frame_w - BTN_SIZE - BTN_MARGIN - 10 && py <= BTN_SIZE + BTN_MARGIN + 10;
}

static void touch_report(void)
{
    for (int i = 0; i < SLOTS; ++i) {
        slot_t *s = &slots[i];
        if (!s->active) continue;
        float rx = norm(s->x, min_x, max_x), ry = norm(s->y, min_y, max_y);
        if (swap_xy) { const float t = rx; rx = ry; ry = t; }
        s->nx = inv_x ? 1 - rx : rx;
        s->ny = inv_y ? 1 - ry : ry;
    }
    int down = 0;
    for (int i = 0; i < SLOTS; ++i) {
        const slot_t *s = &slots[i], *p = &prev_slots[i];
        SDL_Event e;
        memset(&e, 0, sizeof(e));
        e.tfinger.touchId = 1;
        e.tfinger.fingerId = i;
        if (s->active && !p->active) on_button[i] = button_hit(s->nx, s->ny);
        if (on_button[i]) {               /* the close button: the map never sees this touch */
            if (!s->active && p->active) {
                on_button[i] = 0;
                if (button_hit(p->nx, p->ny)) { e.type = SDL_QUIT; push(&e); }
            }
            continue;
        }
        if (s->active) ++down;
        if (s->active && !p->active) {
            e.type = SDL_FINGERDOWN;
            e.tfinger.x = s->nx; e.tfinger.y = s->ny;
            push(&e);
        } else if (s->active && p->active && (s->nx != p->nx || s->ny != p->ny)) {
            e.type = SDL_FINGERMOTION;
            e.tfinger.x = s->nx; e.tfinger.y = s->ny;
            e.tfinger.dx = s->nx - p->nx; e.tfinger.dy = s->ny - p->ny;
            push(&e);
        } else if (!s->active && p->active) {
            e.type = SDL_FINGERUP;
            e.tfinger.x = p->nx; e.tfinger.y = p->ny;
            push(&e);
        }
    }
    /* two fingers: pinch */
    int a = -1, b = -1;
    for (int i = 0; i < SLOTS; ++i)
        if (slots[i].active) { if (a < 0) a = i; else if (b < 0) b = i; }
    if (a >= 0 && b >= 0) {
        const float dist = hypotf(slots[a].nx - slots[b].nx, slots[a].ny - slots[b].ny);
        if (pinch_prev >= 0 && dist != pinch_prev) {
            SDL_Event e;
            memset(&e, 0, sizeof(e));
            e.type = SDL_MULTIGESTURE;
            e.mgesture.touchId = 1;
            e.mgesture.dDist = dist - pinch_prev;
            e.mgesture.numFingers = 2;
            e.mgesture.x = (slots[a].nx + slots[b].nx) / 2;
            e.mgesture.y = (slots[a].ny + slots[b].ny) / 2;
            push(&e);
        }
        pinch_prev = dist;
    } else {
        pinch_prev = -1;
    }
    fingers_down = down;
    memcpy(prev_slots, slots, sizeof(slots));
}

static void touch_poll(void)
{
    if (touch_fd < 0) touch_open();                 /* absent (or just unplugged): look again now and then */
    if (touch_fd < 0) return;
    struct input_event ev;
    ssize_t n;
    while ((n = read(touch_fd, &ev, sizeof(ev))) == (ssize_t)sizeof(ev)) {
        if (ev.type == EV_ABS) {
            if (ev.code == ABS_MT_SLOT) cur_slot = ev.value < 0 ? 0 : (ev.value >= SLOTS ? SLOTS - 1 : ev.value);
            else if (ev.code == ABS_MT_TRACKING_ID) slots[cur_slot].active = ev.value >= 0;
            else if (ev.code == ABS_MT_POSITION_X) slots[cur_slot].x = ev.value;
            else if (ev.code == ABS_MT_POSITION_Y) slots[cur_slot].y = ev.value;
        } else if (ev.type == EV_SYN && ev.code == SYN_REPORT) {
            touch_report();
        }
    }
    if (n < 0 && errno != EAGAIN && errno != EINTR) { close(touch_fd); touch_fd = -1; }   /* device went away */
}

/* keyboard (it may be asleep: retry now and then) */
static int kbd_fd = -1;
static time_t kbd_retry;
static int shift_down;

/* evdev key code -> unshifted character (0 when the key types nothing) */
static char key_char(int code)
{
    static const char row1[] = "qwertyuiop";   /* KEY_Q .. KEY_P = 16..25 */
    static const char row2[] = "asdfghjkl";    /* KEY_A .. KEY_L = 30..38 */
    static const char row3[] = "zxcvbnm";      /* KEY_Z .. KEY_M = 44..50 */
    if (code >= 16 && code <= 25) return row1[code - 16];
    if (code >= 30 && code <= 38) return row2[code - 30];
    if (code >= 44 && code <= 50) return row3[code - 44];
    if (code >= 2 && code <= 10) return (char)('1' + code - 2);
    switch (code) {
    case KEY_0: return '0';
    case KEY_MINUS: case KEY_KPMINUS: return '-';
    case KEY_EQUAL: return '=';
    case KEY_COMMA: return ',';
    case KEY_DOT: case KEY_KPDOT: return '.';
    case KEY_SLASH: return '/';
    case KEY_SEMICOLON: return ';';
    case KEY_APOSTROPHE: return '\'';
    case KEY_SPACE: return ' ';
    default: return 0;
    }
}

static void push_text(char ch)
{
    SDL_Event e;
    memset(&e, 0, sizeof(e));
    e.type = SDL_TEXTINPUT;
    e.text.text[0] = ch;
    push(&e);
}

static void push_wheel(int y)
{
    SDL_Event e;
    memset(&e, 0, sizeof(e));
    e.type = SDL_MOUSEWHEEL;
    e.wheel.y = y;
    push(&e);
}

static void kbd_poll(void)
{
    if (kbd_fd < 0) {
        const time_t now = time(NULL);
        if (now == kbd_retry) return;
        kbd_retry = now;
        const char *path = getenv("VIZ_KEYBOARD");
        kbd_fd = open(path && path[0] ? path : "/dev/input/bt-keyboard", O_RDONLY | O_NONBLOCK);
        if (kbd_fd < 0) return;
    }
    struct input_event ev;
    ssize_t n;
    while ((n = read(kbd_fd, &ev, sizeof(ev))) == (ssize_t)sizeof(ev)) {
        if (ev.type != EV_KEY) continue;
        if (ev.code == KEY_LEFTSHIFT || ev.code == KEY_RIGHTSHIFT) { shift_down = ev.value != 0; continue; }
        if (ev.value == 0) continue;                     /* key releases are not needed */
        switch (ev.code) {
        case KEY_ESC: push_key(SDLK_ESCAPE, SDL_SCANCODE_ESCAPE); continue;
        case KEY_ENTER: case KEY_KPENTER: if (ev.value == 1) push_key(SDLK_RETURN, SDL_SCANCODE_RETURN); continue;
        case KEY_BACKSPACE: push_key(SDLK_BACKSPACE, SDL_SCANCODE_BACKSPACE); continue;
        case KEY_TAB: if (ev.value == 1) push_key(SDLK_TAB, SDL_SCANCODE_TAB); continue;
        case KEY_UP: push_key(SDLK_UP, SDL_SCANCODE_UP); push_wheel(-1); continue;        /* viz1090: zoom in */
        case KEY_DOWN: push_key(SDLK_DOWN, SDL_SCANCODE_DOWN); push_wheel(1); continue;   /* viz1090: zoom out */
        case KEY_LEFT: push_key(SDLK_LEFT, SDL_SCANCODE_LEFT); continue;
        case KEY_RIGHT: push_key(SDLK_RIGHT, SDL_SCANCODE_RIGHT); continue;
        case KEY_PAGEUP: case KEY_KPPLUS: push_wheel(-1); continue;
        case KEY_PAGEDOWN: push_wheel(1); continue;
        default: break;
        }
        char ch = key_char(ev.code);
        if (!ch) continue;
        if (ev.code == KEY_MINUS || ev.code == KEY_KPMINUS) push_key(SDLK_MINUS, SDL_SCANCODE_MINUS);    /* viz1090: zoom out */
        else if (ev.code == KEY_EQUAL) push_key(SDLK_EQUALS, SDL_SCANCODE_EQUALS);                      /* viz1090: zoom in */
        if (shift_down && ch >= 'a' && ch <= 'z') ch = (char)(ch - 'a' + 'A');
        push_text(ch);
    }
    if (n < 0 && errno != EAGAIN && errno != EINTR) { close(kbd_fd); kbd_fd = -1; }
}

int SDL_PollEvent(SDL_Event *event)
{
    if (!real_poll) real_poll = dlsym(RTLD_NEXT, "SDL_PollEvent");
    touch_poll();
    kbd_poll();
    if (q_head != q_tail) {
        *event = queue[q_head];
        q_head = (q_head + 1) % QUEUE;
        return 1;
    }
    return real_poll(event);
}
