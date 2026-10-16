/*
 * SPDX-License-Identifier: MIT
 *
 * Runs the unmodified viz1090 (SDL2) on a Raspberry Pi with only a legacy framebuffer (no KMS):
 *   - SDL_CreateRenderer is forced to the software renderer (SDL_VIDEODRIVER=offscreen has no GPU),
 *   - every SDL_RenderPresent copies the finished frame to the panel's framebuffer, turned by the panel's rotation
 *     and converted to its depth (16 or 32 bpp) by cp0_fb_output.c (the launcher's own output stage),
 *   - SDL_PollEvent also reads the touch screen and a keyboard through evdev and turns them into the SDL
 *     events viz1090 handles (finger drag = pan, two-finger pinch = zoom, tap, + / - keys); the X button (top right) and Esc quit.
 *
 * The screen (size, rotation, density, devices) comes from the launcher's app contract, see viz_screen.h. The program
 * draws a window of any size (viz1090 --screensize W H, vizsetup: the screen size); it is centred on the panel when
 * smaller and cut at the right and bottom when bigger.
 *
 * Use:  SDL_VIDEODRIVER=offscreen LD_PRELOAD=libviz_fb.so ./viz1090 --screensize W H ...
 * Environment: VIZ_TOUCH (default: APPLAUNCH_SCREEN_TOUCH, else found by scanning /dev/input/event*), VIZ_KEYBOARD
 *              (default /dev/input/bt-keyboard), APPLAUNCH_TOUCH_ORIENT=buffer (touch axes follow the framebuffer)
 *              or APPLAUNCH_TOUCH_SWAP_XY / _INVERT_X / _INVERT_Y (as for the launcher), VIZ_FB (overrides the
 *              framebuffer), VIZ_STATUS_BAR=1 (the launcher's top bar), VIZ_CLOSE_BUTTON=1 (the X).
 * Test only (never set by the package): VIZ_FAKE_FB=WxHxBPP draws into memory instead of a device, VIZ_FAKE_TAPS="x,y;x,y"
 *              plays taps at logical pixels through the touch mapping, VIZ_DUMP=file.bmp writes the framebuffer (as
 *              the panel stores it, before any rotation is undone) after VIZ_DUMP_FRAME frames (default 5) and exits.
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

#include "cp0_fb_output.h"
#include "cp0_statusbar.h"
#include "viz_screen.h"

/* ------------------------------------------------------------ framebuffer */

static viz_screen_t screen;
static int fb_tried, fb_ok;
static cp0_fb_out_t fb_out;          /* rotation, depth and bounds of the panel */
static uint8_t *fb_map;
static size_t fb_size;
static uint8_t *frame;               /* the program's last frame, ARGB8888 */
static size_t frame_size;
static uint8_t scratch[64 * 1024];   /* work area of the 16 bpp conversion */
static int frame_w, frame_h;         /* size of the program's window */
static int frame_x, frame_y;         /* its top-left corner on the logical canvas */
static int placed_w, placed_h;       /* the window size the canvas was last cleared for */

static int canvas_w(void) { return fb_ok ? fb_out.lw : screen.w; }
static int canvas_h(void) { return fb_ok ? fb_out.lh : screen.h; }

static uint8_t fb_channel(uint32_t v) { return v > 255 ? 255 : (uint8_t)v; }

static void fb_init(void)
{
    if (fb_tried) return;
    fb_tried = 1;
    viz_screen_detect(&screen);
    frame_w = screen.w;
    frame_h = screen.h;
    int pw = 0, ph = 0, bpp = 0, stride = 0;
    cp0_fb_layout_t layout;
    const cp0_fb_layout_t *layout_p = NULL;
    const char *fake = getenv("VIZ_FAKE_FB");
    if (fake && fake[0]) {                            /* test: a framebuffer in memory */
        int bits = 32;
        if (sscanf(fake, "%dx%dx%d", &pw, &ph, &bits) < 2 || pw < 64 || ph < 64 || (bits != 16 && bits != 32)) {
            fprintf(stderr, "[viz_fb] VIZ_FAKE_FB=%s is not WxHx16|32\n", fake);
            return;
        }
        bpp = bits / 8;
        stride = pw * bpp;
        fb_size = (size_t)stride * ph;
        fb_map = calloc(1, fb_size);
        if (!fb_map) return;
    } else {
        const int fd = open(screen.fb, O_RDWR);
        if (fd < 0) { fprintf(stderr, "[viz_fb] open %s: %s\n", screen.fb, strerror(errno)); return; }
        struct fb_var_screeninfo var;
        struct fb_fix_screeninfo fix;
        if (ioctl(fd, FBIOGET_VSCREENINFO, &var) || ioctl(fd, FBIOGET_FSCREENINFO, &fix)) {
            fprintf(stderr, "[viz_fb] %s: not a framebuffer\n", screen.fb);
            close(fd);
            return;
        }
        pw = (int)var.xres;
        ph = (int)var.yres;
        bpp = (int)var.bits_per_pixel / 8;
        stride = (int)fix.line_length;
        fb_size = (size_t)stride * var.yres_virtual;
        layout = (cp0_fb_layout_t){{fb_channel(var.red.offset), fb_channel(var.red.length)},
                                   {fb_channel(var.green.offset), fb_channel(var.green.length)},
                                   {fb_channel(var.blue.offset), fb_channel(var.blue.length)}};
        layout_p = &layout;
        fb_map = mmap(NULL, fb_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        close(fd);                                    /* the mapping stays valid */
        if (fb_map == MAP_FAILED) { perror("[viz_fb] mmap"); fb_map = NULL; return; }
    }
    const int fix = cp0_fbo_setup(&fb_out, fb_map, fb_size, stride, bpp, pw, ph, screen.rot, screen.w, screen.h, layout_p);
    if (fix < 0) {
        fprintf(stderr, "[viz_fb] %s: unusable framebuffer %dx%d, %d bytes per pixel\n", screen.fb, pw, ph, bpp);
        return;
    }
    fb_ok = 1;
    fprintf(stderr, "[viz_fb] framebuffer %s %dx%d %d bpp, rotate %d, canvas %dx%d; screen %dx%d (%s), %d.%02d px/mm\n",
            fake && fake[0] ? "(test)" : screen.fb, fb_out.pw, fb_out.ph, bpp * 8, fb_out.rot, fb_out.lw, fb_out.lh,
            screen.w, screen.h, screen.source, screen.ppmm_x100 / 100, screen.ppmm_x100 % 100);
    if (fix & CP0_FBO_FIX_LOGICAL)
        fprintf(stderr, "[viz_fb] the screen size %dx%d does not match the framebuffer at rotate %d: drawing on %dx%d\n",
                screen.w, screen.h, screen.rot, fb_out.lw, fb_out.lh);
}

/* Design scale of the overlays: 1 on the deck (640x480), more on bigger screens, less on smaller ones. */
static float overlay_scale(void)
{
    const float sx = frame_w / 640.0f, sy = frame_h / 480.0f;
    const float s = sx < sy ? sx : sy;
    return s < 0.5f ? 0.5f : (s > 3.0f ? 3.0f : s);
}

/* close button (top right): drawn into every frame; a tap on it quits. 56 px with a 6 px margin on the deck. */
static int btn_size(void) { const int b = (int)lroundf(56 * overlay_scale()); return b < 40 ? 40 : b; }
static int btn_margin(void) { return (int)lroundf(6 * overlay_scale()); }

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
    const int size = btn_size();
    const int x0 = w - size - btn_margin(), y0 = btn_margin();
    if (x0 < 0 || y0 + size > h) return;
    const int border = size / 28 > 2 ? size / 28 : 2;
    for (int y = 0; y < size; ++y)
        for (int x = 0; x < size; ++x) {
            const int edge = x < border || y < border || x >= size - border || y >= size - border;
            p[(size_t)(y0 + y) * w + x0 + x] = edge ? 0xFFB0B0B0u : 0xFF1C1C1Cu;
        }
    const int inset = size * 12 / 56, half = size / 28 > 2 ? size / 28 : 2;   /* the X: two diagonals, 4 px thick on the deck */
    for (int i = inset; i < size - inset; ++i)
        for (int t = -half; t < half; ++t) {
            p[(size_t)(y0 + i) * w + x0 + i + t] = 0xFFFFFFFFu;
            p[(size_t)(y0 + i) * w + x0 + (size - 1 - i) + t] = 0xFFFFFFFFu;
        }
}

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
    /* the home grid's size from 480 rows up (as the launcher); scaled down on a shorter screen.
     * A dim backing only over the map (next to the close button); plain screens look exactly like the home grid */
    const int pct = h >= 480 ? 100 : (h * 100 / 480 < 50 ? 50 : h * 100 / 480);
    cp0_statusbar_render_scaled(status_bar, p, w, w, shift_left, top, close_button_enabled() ? 0x99 : 0, &status_state, pct);
}

/* ------------------------------------------------------------------ test dump */

static int dump_frame = -1;           /* VIZ_DUMP: the frame to write, -1 = off */
static int presented;                 /* frames copied to the framebuffer */

static void put_le(uint8_t *p, uint32_t v, int n) { for (int i = 0; i < n; ++i) p[i] = (uint8_t)(v >> (8 * i)); }

/* The framebuffer as stored (buffer orientation), as a 24-bit BMP. */
static void dump_framebuffer(const char *path)
{
    const int w = fb_out.pw, h = fb_out.ph, row = (w * 3 + 3) & ~3;
    FILE *file = fopen(path, "wb");
    if (!file) { fprintf(stderr, "[viz_fb] dump %s: %s\n", path, strerror(errno)); return; }
    uint8_t header[54] = {'B', 'M'};
    put_le(header + 2, 54u + (uint32_t)row * h, 4);
    put_le(header + 10, 54, 4);
    put_le(header + 14, 40, 4);
    put_le(header + 18, (uint32_t)w, 4);
    put_le(header + 22, (uint32_t)h, 4);
    put_le(header + 26, 1, 2);
    put_le(header + 28, 24, 2);
    put_le(header + 34, (uint32_t)row * h, 4);
    fwrite(header, 1, sizeof(header), file);
    uint8_t *line = calloc(1, (size_t)row);
    const cp0_fb_layout_t *l = &fb_out.layout;
    for (int y = h - 1; line && y >= 0; --y) {
        const uint8_t *src = fb_out.base + (size_t)y * fb_out.stride;
        for (int x = 0; x < w; ++x) {
            uint32_t r, g, b;
            if (fb_out.bpp == 4) {
                const uint32_t v = ((const uint32_t *)src)[x];
                r = (v >> 16) & 255; g = (v >> 8) & 255; b = v & 255;
            } else {
                const uint32_t v = ((const uint16_t *)src)[x];
                r = ((v >> l->r.off) & ((1u << l->r.len) - 1)) << (8 - l->r.len);
                g = ((v >> l->g.off) & ((1u << l->g.len) - 1)) << (8 - l->g.len);
                b = ((v >> l->b.off) & ((1u << l->b.len) - 1)) << (8 - l->b.len);
            }
            line[x * 3] = (uint8_t)b; line[x * 3 + 1] = (uint8_t)g; line[x * 3 + 2] = (uint8_t)r;
        }
        fwrite(line, 1, (size_t)row, file);
    }
    free(line);
    fclose(file);
    fprintf(stderr, "[viz_fb] dump: %s (%dx%d buffer, window %dx%d at %d,%d)\n", path, w, h, frame_w, frame_h, frame_x, frame_y);
}

static void present_to_fb(SDL_Renderer *renderer)
{
    fb_init();
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
    if (close_button_enabled())                       /* beside the close button, centred on it */
        draw_status_bar(frame, w, h, btn_size() + btn_margin() - 4, btn_margin() + (btn_size() - 40) / 2);
    else draw_status_bar(frame, w, h, 0, 8);
    draw_close_button(frame, w, h);
    if (!fb_ok) return;
    /* centred when smaller than the canvas; cut at the right and bottom when bigger */
    frame_x = w < fb_out.lw ? (fb_out.lw - w) / 2 : 0;
    frame_y = h < fb_out.lh ? (fb_out.lh - h) / 2 : 0;
    if (w != placed_w || h != placed_h) {             /* new window size: clear what it does not cover */
        if (w < fb_out.lw || h < fb_out.lh) cp0_fbo_fill_black(&fb_out, 0, 0, fb_out.lw, fb_out.lh);
        placed_w = w;
        placed_h = h;
    }
    if (fb_out.bpp == 4) cp0_fbo_blit(&fb_out, frame_x, frame_y, w, h, frame, w * 4);
    else cp0_fbo_blit_xrgb_scaled(&fb_out, frame_x, frame_y, w, h, frame, w * 4, 1, 0, 0, 0, 0, scratch, sizeof(scratch));

    ++presented;
    if (dump_frame < 0) {
        const char *n = getenv("VIZ_DUMP_FRAME");
        dump_frame = getenv("VIZ_DUMP") && getenv("VIZ_DUMP")[0] ? (n && atoi(n) > 0 ? atoi(n) : 5) : 0;
    }
    if (dump_frame > 0 && presented == dump_frame) {
        dump_framebuffer(getenv("VIZ_DUMP"));
        exit(0);
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
static int buffer_axes;             /* APPLAUNCH_TOUCH_ORIENT=buffer: the axes follow the framebuffer */
static float pinch_prev = -1;
static int fingers_down;
static int on_button[SLOTS];        /* the finger went down on the close button */

static float norm(int v, int lo, int hi)
{
    if (hi <= lo) return 0;
    float f = (float)(v - lo) / (float)(hi - lo);
    return f < 0 ? 0 : (f > 1 ? 1 : f);
}

static float clamp01(float f) { return f < 0 ? 0 : (f > 1 ? 1 : f); }

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

static void touch_axes(void)
{
    const char *s;
    const char *orient = getenv("APPLAUNCH_TOUCH_ORIENT");
    buffer_axes = orient && strcmp(orient, "buffer") == 0;
    swap_xy = (s = getenv("APPLAUNCH_TOUCH_SWAP_XY")) ? atoi(s) : 1;     /* the deck's axes when nothing is set */
    inv_x = (s = getenv("APPLAUNCH_TOUCH_INVERT_X")) ? atoi(s) : 0;
    inv_y = (s = getenv("APPLAUNCH_TOUCH_INVERT_Y")) ? atoi(s) : 1;
}

static void touch_open(void)
{
    const time_t now = time(NULL);
    if (now == touch_retry) return;                 /* try at most once a second, and only while it is missing */
    touch_retry = now;
    fb_init();                                      /* the rotation maps buffer-oriented touch */
    const char *path = getenv("VIZ_TOUCH");
    if (path && path[0]) {
        touch_fd = open(path, O_RDONLY | O_NONBLOCK);   /* explicit override */
    } else {
        touch_fd = -1;
        if (screen.touch[0]) {                      /* the launcher's own touch device, if it still is one */
            touch_fd = open(screen.touch, O_RDONLY | O_NONBLOCK);
            if (touch_fd >= 0 && !touch_score(touch_fd)) { close(touch_fd); touch_fd = -1; }
        }
        if (touch_fd < 0) touch_fd = touch_find();
    }
    if (touch_fd < 0) { touch_fd = -1; return; }
    memset(slots, 0, sizeof(slots));
    memset(prev_slots, 0, sizeof(prev_slots));
    memset(on_button, 0, sizeof(on_button));
    pinch_prev = -1;
    struct input_absinfo info;
    if (ioctl(touch_fd, EVIOCGABS(ABS_MT_POSITION_X), &info) == 0) { min_x = info.minimum; max_x = info.maximum; }
    if (ioctl(touch_fd, EVIOCGABS(ABS_MT_POSITION_Y), &info) == 0) { min_y = info.minimum; max_y = info.maximum; }
    touch_axes();
    fprintf(stderr, "[viz_fb] touch screen opened (%s axes)\n", buffer_axes ? "buffer" : "legacy");
}

static int button_hit(float nx, float ny)
{
    if (!close_button_enabled()) return 0;
    const float px = nx * frame_w, py = ny * frame_h;
    const int pad = btn_size() * 10 / 56;
    return px >= frame_w - btn_size() - btn_margin() - pad && py <= btn_size() + btn_margin() + pad;
}

/* A raw touch position -> the program's window, 0..1 on each axis. */
static void touch_to_window(const slot_t *s, float *nx, float *ny)
{
    float lx, ly;                                   /* logical canvas pixels */
    if (buffer_axes && fb_ok) {
        int X = 0, Y = 0;
        cp0_fbo_touch_to_logical(&fb_out, s->x, s->y, min_x, max_x, min_y, max_y, &X, &Y);
        lx = X + 0.5f;
        ly = Y + 0.5f;
    } else {
        float rx = norm(s->x, min_x, max_x), ry = norm(s->y, min_y, max_y);
        if (swap_xy) { const float t = rx; rx = ry; ry = t; }
        lx = (inv_x ? 1 - rx : rx) * canvas_w();
        ly = (inv_y ? 1 - ry : ry) * canvas_h();
    }
    *nx = frame_w > 0 ? clamp01((lx - frame_x) / frame_w) : 0;
    *ny = frame_h > 0 ? clamp01((ly - frame_y) / frame_h) : 0;
}

static void touch_report(void)
{
    for (int i = 0; i < SLOTS; ++i)
        if (slots[i].active) touch_to_window(&slots[i], &slots[i].nx, &slots[i].ny);
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

/* Test: VIZ_FAKE_TAPS="x,y;x,y" (logical pixels) turned into raw positions of a touch screen whose range is the
 * framebuffer's pixels, then through the same mapping as a real touch. One press or release per frame shown, so
 * the program has drawn (and laid out) its screen before the first one. */
static const char *fake_taps;
static int fake_down;
static int fake_frame;

static int fake_tap_step(void)
{
    if (!fake_taps) {
        static int checked;
        if (checked) return 0;
        checked = 1;
        const char *v = getenv("VIZ_FAKE_TAPS");
        if (!v || !v[0]) return 0;
        fake_taps = v;
        fb_init();
        touch_axes();
        min_x = min_y = 0;
        max_x = (fb_ok ? fb_out.pw : screen.w) - 1;
        max_y = (fb_ok ? fb_out.ph : screen.h) - 1;
    }
    if (presented == fake_frame) return 1;          /* one step per frame */
    fake_frame = presented;
    if (fake_down) {                                /* release */
        slots[0].active = 0;
        fake_down = 0;
        touch_report();
        return 1;
    }
    int X, Y, used = 0;
    if (!*fake_taps || sscanf(fake_taps, "%d,%d%n", &X, &Y, &used) != 2) return 1;
    fake_taps += used;
    while (*fake_taps == ';' || *fake_taps == ' ') ++fake_taps;
    int rx, ry;
    if (buffer_axes && fb_ok) {
        cp0_fbo_logical_to_buffer(&fb_out, X, Y, &rx, &ry);
    } else {                                        /* undo the legacy swap and inversions */
        float nx = (X + 0.5f) / canvas_w(), ny = (Y + 0.5f) / canvas_h();
        if (inv_x) nx = 1 - nx;
        if (inv_y) ny = 1 - ny;
        if (swap_xy) { const float t = nx; nx = ny; ny = t; }
        rx = (int)lroundf(nx * max_x);
        ry = (int)lroundf(ny * max_y);
    }
    slots[0].active = 1;
    slots[0].x = rx;
    slots[0].y = ry;
    fake_down = 1;
    touch_report();
    return 1;
}

static void touch_poll(void)
{
    if (fake_tap_step()) return;
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
