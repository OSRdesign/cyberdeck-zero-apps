/*
 * SPDX-License-Identifier: MIT
 *
 * Runs the unmodified viz1090 (SDL2) on a Raspberry Pi with only a legacy framebuffer (no KMS):
 *   - SDL_CreateRenderer is forced to the software renderer (SDL_VIDEODRIVER=offscreen has no GPU),
 *   - every SDL_RenderPresent copies the finished frame to the panel's framebuffer, turned by the panel's rotation
 *     and converted to its depth (16 or 32 bpp) by cp0_fb_output.c (the launcher's own output stage),
 *   - with VIZ_TITLE set, the app header of the launcher's native pages is drawn above it (title, the on-screen Esc
 *     button, the status strip), with the geometry of the launcher's layout service; the program's window goes under
 *     it,
 *   - SDL_PollEvent also reads the touch screen and a keyboard through evdev and turns them into the SDL
 *     events viz1090 handles (finger drag = pan, two-finger pinch = zoom, tap, + / - keys); the header's Esc button,
 *     the Esc key and SIGTERM / SIGINT / SIGHUP quit.
 *
 * The screen (size, rotation, density, devices) comes from the launcher's app contract, see viz_screen.h. The program
 * draws a window of any size; it is centred in its area (the screen under the header) when smaller and cut at the
 * right and bottom when bigger. Its area: VIZ_PRINT_MAP=1 LD_PRELOAD=libviz_fb.so <the program> prints
 * "WIDTH HEIGHT HEADER" (px) and exits before the program starts (run_viz1090.sh asks it for --screensize).
 *
 * Use:  SDL_VIDEODRIVER=offscreen VIZ_TITLE=viz1090 LD_PRELOAD=libviz_fb.so ./viz1090 --screensize W H ...
 * Environment: VIZ_TITLE (the header and its title; unset: no header, the window fills the screen), VIZ_FONT and
 *              VIZ_ICON_FONT (Montserrat-Medium.ttf and the FontAwesome font of the launcher; default /opt/viz1090/font),
 *              VIZ_TOUCH (default: APPLAUNCH_SCREEN_TOUCH, else found by scanning /dev/input/event*), VIZ_KEYBOARD
 *              (default /dev/input/bt-keyboard), APPLAUNCH_TOUCH_ORIENT=buffer (touch axes follow the framebuffer)
 *              or APPLAUNCH_TOUCH_SWAP_XY / _INVERT_X / _INVERT_Y (as for the launcher), VIZ_FB (overrides the
 *              framebuffer).
 * Test only (never set by the package): VIZ_FAKE_FB=WxHxBPP draws into memory instead of a device, VIZ_FAKE_TAPS="x,y;x,y"
 *              plays taps at logical pixels (header included) through the touch mapping, VIZ_DUMP=file.bmp writes the
 *              framebuffer (as the panel stores it, before any rotation is undone) after VIZ_DUMP_FRAME frames
 *              (default 5) and exits.
 */
#define _GNU_SOURCE
#include <SDL2/SDL.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <linux/input.h>
#include <math.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include <ft2build.h>
#include FT_FREETYPE_H

#include "cp0_app_contract.h"
#include "cp0_fb_output.h"
#include "cp0_power_supply.h"
#include "cp0_statusbar.h"
#include "cp0_ui_metrics.h"
#include "cp0_ui_roller.h"
#include "cp0_ui_status_strip.h"
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

/* ---------------------------------------------------------------- the app header */

/* The header of the launcher's native pages (cp0::ui::Page in cp0_lvgl, as the intro screen of this app draws it):
 * the title at the home grid's place, size and colour, the on-screen Esc button, the shared status strip. Every number
 * comes from the launcher's layout service for this canvas and density (cp0_ui_metrics_compute + cp0_ui_roller_page,
 * the calls the toolkit makes; copied files, see README.md), the strip from its renderer (cp0_statusbar.c). */
#define HDR_GOLD 0xF0B400u        /* the home grid's title (cp0_ui_page.cpp kGold) */
#define HDR_KEY_BG 0x2A2A2Au      /* the Esc button (kKeyBg) ... */
#define HDR_KEY_PRESSED 0x2D9CDBu /* ... while it is held (kKeyPressed) */
#define HDR_KEY_TEXT 0xFFFFFFu

static int layout_ready;
static int header_on;                 /* VIZ_TITLE is set and the screen has room for the header */
static cp0_ui_metrics_t metrics;      /* the layout service's numbers for the canvas */
static cp0_ui_roller_page_t page;     /* the native page: header_h, title_*, esc_*, strip_* */
static cp0_ui_status_strip_geom_t strip;
static int area_y, area_h;            /* the program's area: full width, from area_y to the bottom */

static const char *title_text(void)
{
    const char *t = getenv("VIZ_TITLE");
    return t && t[0] ? t : NULL;
}

static const char *font_file(const char *env, const char *fallback)
{
    const char *v = getenv(env);
    return v && v[0] ? v : fallback;
}

/* The status strip's renderer (the launcher's own), made on first use. */
static cp0_statusbar_t *status_bar;
static int status_failed;

static cp0_statusbar_t *strip_renderer(void)
{
    if (!status_bar && !status_failed) {
        status_bar = cp0_statusbar_create(font_file("VIZ_FONT", "/opt/viz1090/font/Montserrat-Medium.ttf"),
                                          font_file("VIZ_ICON_FONT", "/opt/viz1090/font/FontAwesome5-Solid+Brands+Regular.woff"));
        if (!status_bar) {
            status_failed = 1;
            fprintf(stderr, "[viz_fb] no status strip: the fonts (VIZ_FONT, VIZ_ICON_FONT) do not load\n");
        }
    }
    return status_bar;
}

/* The strip drawn in `state` into the scratch canvas: its first column holding a pixel. */
static int strip_first_column(cp0_statusbar_t *bar, uint32_t *pixels, int rows, const cp0_statusbar_state_t *state)
{
    memset(pixels, 0, (size_t)strip.w * rows * sizeof(uint32_t));
    cp0_statusbar_render_scaled(bar, pixels, strip.w, strip.w, 0, strip.top, 0, state, strip.pct);
    return cp0_ui_status_first_column(pixels, strip.w, rows);
}

/* cp0::ui::status_strip_left(): the strip drawn in its fullest state (the SSH indicator's slot, Bluetooth glyph, every
 * bar, a clock) into a scratch canvas; its first column holding a pixel is the leftmost pixel the strip can draw. The
 * SSH indicator's slot is always kept (launcher 3f6579f), so the Esc button and the title never move when a session
 * starts or ends; with a battery its "100%" yields to the indicator, so the leftmost of both states counts. */
static int strip_left_px(void)
{
    cp0_statusbar_t *bar = strip_renderer();
    if (!bar || strip.w <= 0) return strip.x;
    const int rows = strip.rows + 16;
    uint32_t *pixels = calloc((size_t)strip.w * rows, sizeof(uint32_t));
    if (!pixels) return strip.x;
    cp0_statusbar_state_t full;
    memset(&full, 0, sizeof(full));
    full.wifi_up = 1;
    full.wifi_pct = 100;
    full.bt_on = 1;
    full.bt_connected = 1;
    snprintf(full.clock, sizeof(full.clock), "%s", "00:00");
    cp0_ui_status_set_ssh(&full, 1, 0);
    cp0_power_battery_t battery;
    const int has_battery = cp0_power_supply_battery(&battery);
    if (has_battery) {                              /* a battery at 100 % when the board has one now */
        battery.percent = 100;
        cp0_ui_status_set_battery(&full, &battery);
    }
    int first = strip_first_column(bar, pixels, rows, &full);
    if (has_battery) {
        cp0_ui_status_set_ssh(&full, 0, 0);         /* the percentage drawn */
        const int other = strip_first_column(bar, pixels, rows, &full);
        if (other < first) first = other;
    }
    free(pixels);
    return strip.x + (first < strip.w ? first : 0);
}

static void layout_init(void)
{
    if (layout_ready) return;
    layout_ready = 1;
    fb_init();
    const int w = canvas_w(), h = canvas_h();
    area_y = 0;
    area_h = h;
    if (!title_text()) return;
    if (cp0_ui_metrics_compute(w, h, screen.rot, screen.ppmm_x100, CP0_UI_DENSITY_PANEL_MM, 0, &metrics) != 0) return;
    cp0_ui_status_strip_geom(&metrics, w, &strip);
    if (cp0_ui_roller_page(&metrics, strip_left_px(), &page) != 0) {
        fprintf(stderr, "[viz_fb] %dx%d has no room for the header: none\n", w, h);
        return;
    }
    header_on = 1;
    area_y = page.header_h;
    area_h = h - page.header_h;
    frame_w = w;                                    /* until the first frame says otherwise */
    frame_h = area_h;
    frame_y = area_y;
    fprintf(stderr, "[viz_fb] header %d px (%s class), title at %d,%d %d px, Esc %dx%d at %d,%d, strip from x %d; "
            "the program's area %dx%d at 0,%d\n", page.header_h, cp0_ui_class_name(metrics.cls), page.title_x,
            page.title_y, page.title_px, page.esc_w, page.esc_h, page.esc_x, page.esc_y, page.strip_left, w, area_h,
            area_y);
}

/* Text of the header in the look of the launcher's builtin Montserrat (LVGL lv_font_montserrat_N, made by lv_font_conv
 * from the same Montserrat-Medium.ttf with FreeType's light autohinting at 4 bpp): the same glyph rendering, LVGL's
 * advances (the unhinted width plus the pair kerning, in 1/16 px, rounded to whole pixels) and the line height and
 * baseline of the builtin size, so that a label's text lands on the pixels LVGL draws. */
static const struct { uint8_t px, line_h, base; } k_builtin[] = {
    {8, 10, 2},   {10, 11, 2},  {12, 15, 3},  {14, 16, 3},  {16, 18, 3},  {18, 21, 4},  {20, 22, 4},
    {22, 24, 4},  {24, 27, 5},  {26, 29, 5},  {28, 30, 5},  {30, 33, 6},  {32, 35, 6},  {34, 38, 7},
    {36, 40, 7},  {38, 41, 7},  {40, 44, 8},  {42, 46, 8},  {44, 49, 9},  {46, 50, 9},  {48, 52, 9},
};

static FT_Library text_lib;
static FT_Face text_face;
static int text_failed;

/* The font's GPOS table: lv_font_conv takes the kerning from its 'kern' feature (FreeType reads only the old 'kern'
 * table, which Montserrat does not have). */
static uint8_t *font_bytes;
static const uint8_t *gpos;
static uint32_t gpos_len;

static uint32_t be(const uint8_t *p, int n) { uint32_t v = 0; while (n--) v = (v << 8) | *p++; return v; }

static void gpos_load(const char *path)
{
    FILE *file = fopen(path, "rb");
    if (!file) return;
    long size = -1;
    if (fseek(file, 0, SEEK_END) == 0) size = ftell(file);
    if (size >= 12 && size < 32L * 1024 * 1024 && fseek(file, 0, SEEK_SET) == 0 && (font_bytes = malloc((size_t)size)) &&
        fread(font_bytes, 1, (size_t)size, file) == (size_t)size) {
        const uint32_t tables = be(font_bytes + 4, 2);
        for (uint32_t i = 0; i < tables && 12 + 16 * (i + 1) <= (uint32_t)size; ++i) {
            const uint8_t *rec = font_bytes + 12 + 16 * i;
            const uint32_t off = be(rec + 8, 4), len = be(rec + 12, 4);
            if (memcmp(rec, "GPOS", 4) == 0 && off < (uint32_t)size && len <= (uint32_t)size - off) {
                gpos = font_bytes + off;
                gpos_len = len;
            }
        }
    }
    fclose(file);
}

static uint32_t g16(uint32_t o) { return o <= gpos_len && gpos_len - o >= 2 ? be(gpos + o, 2) : 0; }

static int coverage_index(uint32_t cov, uint32_t glyph)
{
    const uint32_t fmt = g16(cov), count = g16(cov + 2);
    for (uint32_t i = 0; i < count; ++i) {
        if (fmt == 1 && g16(cov + 4 + 2 * i) == glyph) return (int)i;
        if (fmt == 2 && glyph >= g16(cov + 4 + 6 * i) && glyph <= g16(cov + 6 + 6 * i))
            return (int)(g16(cov + 8 + 6 * i) + glyph - g16(cov + 4 + 6 * i));
        if (fmt != 1 && fmt != 2) break;
    }
    return -1;
}

static uint32_t glyph_class(uint32_t def, uint32_t glyph)
{
    const uint32_t fmt = g16(def);
    if (fmt == 1) {
        const uint32_t first = g16(def + 2), count = g16(def + 4);
        return glyph >= first && glyph < first + count ? g16(def + 6 + 2 * (glyph - first)) : 0;
    }
    for (uint32_t i = 0, count = fmt == 2 ? g16(def + 2) : 0; i < count; ++i)
        if (glyph >= g16(def + 4 + 6 * i) && glyph <= g16(def + 6 + 6 * i)) return g16(def + 8 + 6 * i);
    return 0;
}

static uint32_t value_size(uint32_t format) { return 2 * (uint32_t)__builtin_popcount(format & 0xFF); }

static int x_advance(uint32_t value, uint32_t format)   /* the XAdvance of a ValueRecord, font units */
{
    return format & 4 ? (int16_t)g16(value + 2 * (uint32_t)__builtin_popcount(format & 3)) : 0;
}

/* Kerning of a glyph pair in font units, as opentype.js finds it: the first pair-positioning subtable of the 'kern'
 * lookups whose coverage holds the left glyph decides. */
static int gpos_kern(uint32_t left, uint32_t right)
{
    if (!gpos || !left || !right) return 0;
    const uint32_t features = g16(6), lookups = g16(8);
    for (uint32_t f = 0, nf = g16(features); f < nf; ++f) {
        const uint32_t rec = features + 2 + 6 * f;
        if (rec + 6 > gpos_len || memcmp(gpos + rec, "kern", 4) != 0) continue;
        const uint32_t feature = features + g16(rec + 4);
        for (uint32_t l = 0, nl = g16(feature + 2); l < nl; ++l) {
            const uint32_t lookup = lookups + g16(lookups + 2 + 2 * g16(feature + 4 + 2 * l));
            for (uint32_t t = 0, nt = g16(lookup + 4); t < nt; ++t) {
                uint32_t sub = lookup + g16(lookup + 6 + 2 * t), type = g16(lookup);
                if (type == 9) {                    /* extension: the real subtable further on */
                    type = g16(sub + 2);
                    sub += (g16(sub + 4) << 16) | g16(sub + 6);
                }
                if (type != 2) continue;
                const int index = coverage_index(sub + g16(sub + 2), left);
                if (index < 0) continue;
                const uint32_t vf1 = g16(sub + 4), size = value_size(vf1) + value_size(g16(sub + 6));
                if (g16(sub) == 1) {                /* pairs, glyph by glyph */
                    if ((uint32_t)index >= g16(sub + 8)) continue;
                    const uint32_t set = sub + g16(sub + 10 + 2 * (uint32_t)index);
                    for (uint32_t i = 0, n = g16(set); i < n; ++i)
                        if (g16(set + 2 + i * (2 + size)) == right) return x_advance(set + 4 + i * (2 + size), vf1);
                    continue;
                }
                if (g16(sub) == 2) {                /* pairs of classes */
                    const uint32_t c1 = glyph_class(sub + g16(sub + 8), left), c2 = glyph_class(sub + g16(sub + 10), right);
                    const uint32_t n1 = g16(sub + 12), n2 = g16(sub + 14);
                    return c1 < n1 && c2 < n2 ? x_advance(sub + 16 + (c1 * n2 + c2) * size, vf1) : 0;
                }
            }
        }
    }
    return 0;
}

/* The face at the builtin size LVGL would use for px (cp0_ui_font_px: even, 8..48); line height and baseline. */
static FT_Face text_font(int px, int *line_h, int *base)
{
    const int snapped = cp0_ui_font_snap(px);
    *line_h = snapped + snapped / 8;
    *base = snapped / 5;
    for (size_t i = 0; i < sizeof(k_builtin) / sizeof(k_builtin[0]); ++i)
        if (k_builtin[i].px == snapped) { *line_h = k_builtin[i].line_h; *base = k_builtin[i].base; }
    if (!text_face && !text_failed) {
        const char *path = font_file("VIZ_FONT", "/opt/viz1090/font/Montserrat-Medium.ttf");
        if (FT_Init_FreeType(&text_lib) != 0 || FT_New_Face(text_lib, path, 0, &text_face) != 0) {
            text_failed = 1;
            text_face = NULL;
            fprintf(stderr, "[viz_fb] %s does not load: the header has no text\n", path);
        } else {
            gpos_load(path);
        }
    }
    if (text_face) FT_Set_Pixel_Sizes(text_face, 0, (FT_UInt)snapped);
    return text_face;
}

/* Next code point of a UTF-8 string (a stray byte stands for itself). */
static unsigned long utf8_next(const char **s)
{
    const unsigned char *p = (const unsigned char *)*s;
    unsigned long c = *p++;
    int more = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : 0;
    if (more) c &= 0x3F >> more;
    while (more-- > 0 && (*p & 0xC0) == 0x80) c = (c << 6) | (*p++ & 0x3F);
    *s = (const char *)p;
    return c;
}

/* LVGL's advance of glyph c followed by next (0: none): (width + kerning) in 1/16 px, rounded to whole pixels. */
static int glyph_advance(FT_Face face, unsigned long c, unsigned long next)
{
    if (FT_Load_Char(face, c, FT_LOAD_NO_HINTING) != 0) return 0;
    long adv16 = (face->glyph->linearHoriAdvance * 16 + 32768) >> 16;
    const int kern = next ? gpos_kern(FT_Get_Char_Index(face, c), FT_Get_Char_Index(face, next)) : 0;
    if (kern && face->units_per_EM)
        adv16 += (long)floor(kern * 16.0 * face->size->metrics.x_ppem / face->units_per_EM + 0.5);
    return (int)((adv16 + 8) >> 4);
}

#define TEXT_MAX 64   /* code points of a header text */

/* The code points of a text and their advances; returns the count. */
static int text_layout(FT_Face face, const char *text, unsigned long *cps, int *advs)
{
    int n = 0;
    while (*text && n < TEXT_MAX) cps[n++] = utf8_next(&text);
    for (int i = 0; i < n; ++i) advs[i] = glyph_advance(face, cps[i], i + 1 < n ? cps[i + 1] : 0);
    return n;
}

static int text_width(FT_Face face, const char *text)
{
    unsigned long cps[TEXT_MAX];
    int advs[TEXT_MAX], w = 0;
    const int n = text_layout(face, text, cps, advs);
    for (int i = 0; i < n; ++i) w += advs[i];
    return w;
}

static void blend_opaque(uint32_t *d, uint32_t rgb, int a)
{
    if (a <= 0) return;
    if (a >= 255) { *d = 0xFF000000u | rgb; return; }
    uint32_t out = 0xFF000000u;
    for (int shift = 0; shift <= 16; shift += 8) {
        const uint32_t s = (rgb >> shift) & 255, b = (*d >> shift) & 255;
        out |= ((s * a + b * (255 - a) + 127) / 255) << shift;
    }
    *d = out;
}

static void draw_glyph_at(uint32_t *p, int stride, int rows, int clip_w, int pen, int baseline, FT_Face face,
                          unsigned long c, uint32_t rgb)
{
    if (FT_Load_Char(face, c, FT_LOAD_RENDER | FT_LOAD_FORCE_AUTOHINT | FT_LOAD_TARGET_LIGHT) != 0) return;
    const FT_GlyphSlot g = face->glyph;
    for (unsigned row = 0; row < g->bitmap.rows; ++row)
        for (unsigned col = 0; col < g->bitmap.width; ++col) {
            const int x = pen + g->bitmap_left + (int)col, y = baseline - g->bitmap_top + (int)row;
            if (x < 0 || y < 0 || x >= clip_w || y >= rows) continue;
            const int a = g->bitmap.buffer[row * g->bitmap.pitch + col];
            blend_opaque(&p[(size_t)y * stride + x], rgb, (a * 15 + 127) / 255 * 17);   /* 4 bpp, as the builtin fonts */
        }
}

/* Draws text from pen x (the label's left edge) on a baseline; cut with dots when it would pass x_max. */
static void draw_text(uint32_t *p, int stride, int rows, int clip_w, int pen, int baseline, int x_max, FT_Face face,
                      const char *text, uint32_t rgb)
{
    unsigned long cps[TEXT_MAX];
    int advs[TEXT_MAX], width = 0;
    int n = text_layout(face, text, cps, advs);
    for (int i = 0; i < n; ++i) width += advs[i];
    int dots = 0;
    if (pen + width > x_max) {                      /* LVGL's dots mode: as many glyphs as fit with "..." */
        const int dot = glyph_advance(face, '.', '.');
        int w = 0, keep = 0;
        while (keep < n && pen + w + advs[keep] + 3 * dot <= x_max) w += advs[keep++];
        n = keep;
        dots = 3;
    }
    for (int i = 0; i < n; ++i) {
        draw_glyph_at(p, stride, rows, clip_w, pen, baseline, face, cps[i], rgb);
        pen += advs[i];
    }
    for (int i = 0; i < dots; ++i) {
        draw_glyph_at(p, stride, rows, clip_w, pen, baseline, face, '.', rgb);
        pen += glyph_advance(face, '.', '.');
    }
}

/* Anti-aliased rounded rectangle on an opaque buffer (cp0_statusbar's corner sampling, 4x4). */
static void fill_round_rect(uint32_t *p, int stride, int rows, int x0, int y0, int w, int h, int r, uint32_t rgb)
{
    if (r > w / 2) r = w / 2;
    if (r > h / 2) r = h / 2;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const int px = x0 + x, py = y0 + y;
            if (px < 0 || py < 0 || px >= stride || py >= rows) continue;
            const int cx = x < r ? r : (x >= w - r ? w - r - 1 : -1);
            const int cy = y < r ? r : (y >= h - r ? h - r - 1 : -1);
            int coverage = 16;
            if (cx >= 0 && cy >= 0) {
                coverage = 0;
                for (int sy = 0; sy < 4; ++sy)
                    for (int sx = 0; sx < 4; ++sx) {
                        const float fx = x + (sx + 0.5f) / 4 - (cx + 0.5f);
                        const float fy = y + (sy + 0.5f) / 4 - (cy + 0.5f);
                        if (fx * fx + fy * fy <= (float)r * r) ++coverage;
                    }
            }
            blend_opaque(&p[(size_t)py * stride + px], rgb, 255 * coverage / 16);
        }
}

static uint32_t *hdr;                 /* the header's pixels, ARGB8888, opaque: canvas width x hdr_rows */
static int hdr_w, hdr_rows;
static int esc_pressed;               /* a finger holds the Esc button */
static int shown_pressed = -1;
static cp0_statusbar_state_t status_state, shown_state;
static Uint32 status_read_at, ssh_tick_at;

/* Brings the header's pixels up to date (status every 2 s, the clock every frame, the SSH indicator's blink every
 * second); 1 when they changed. */
static int header_update(void)
{
    const Uint32 now = SDL_GetTicks();
    if (!status_read_at || now - status_read_at >= 2000) {
        /* what cp0::ui::StatusStrip does (cp0_ui_status_strip_widget.cpp): the battery from power_supply, then the
         * launcher's status.state while its process is alive (Wi-Fi, Bluetooth, battery and the SSH indicator as the
         * home grid shows them), else sysfs / procfs (no SSH indicator) */
        cp0_power_battery_t battery;
        cp0_ui_status_set_battery(&status_state, cp0_power_supply_battery(&battery) ? &battery : NULL);
        cp0_app_status_t feed;
        if (cp0_app_status_read(NULL, &feed) == 0) {
            cp0_ui_status_set_wifi(&status_state, feed.wifi, feed.wifi_pct);
            status_state.bt_on = feed.bt != CP0_APP_BT_OFF;
            status_state.bt_connected = feed.bt == CP0_APP_BT_CONNECTED;
            if (feed.battery >= 0) {
                status_state.battery = 1;
                status_state.battery_pct = feed.battery > 100 ? 100 : feed.battery;
                status_state.battery_charging = feed.battery_charging;
            }
            cp0_ui_status_set_ssh(&status_state, feed.ssh, feed.ssh_act);
        } else {
            cp0_statusbar_state_t read;
            cp0_statusbar_read_state(&read);
            cp0_ui_status_set_wifi(&status_state, read.wifi_up, read.wifi_pct);
            status_state.bt_on = read.bt_on;
            status_state.bt_connected = read.bt_connected;
            cp0_ui_status_set_ssh(&status_state, 0, 0);     /* no launcher, no indicator */
        }
        status_read_at = now ? now : 1;
    }
    if (!ssh_tick_at) {
        ssh_tick_at = now ? now : 1;
    } else if (now - ssh_tick_at >= 1000) {              /* the strip's 1 s tick: the glyph blinks on traffic */
        cp0_ui_status_ssh_tick(&status_state, 1);
        ssh_tick_at = now ? now : 1;
    }
    const time_t t = time(NULL);
    struct tm local;
    localtime_r(&t, &local);
    strftime(status_state.clock, sizeof(status_state.clock), "%H:%M", &local);
    /* only what is drawn counts: ssh_active itself is not (its blink phase ssh_bright is) */
    cp0_statusbar_state_t drawn = status_state;
    drawn.ssh_active = 0;
    if (hdr && shown_pressed == esc_pressed && memcmp(&shown_state, &drawn, sizeof(drawn)) == 0) return 0;

    if (!hdr) {
        hdr_w = canvas_w();
        hdr_rows = page.header_h > CP0_STATUSBAR_HEIGHT + 16 ? page.header_h : CP0_STATUSBAR_HEIGHT + 16;   /* the strip's reach */
        hdr = malloc((size_t)hdr_w * hdr_rows * sizeof(uint32_t));
        if (!hdr) { header_on = 0; return 0; }
    }
    for (size_t i = 0; i < (size_t)hdr_w * hdr_rows; ++i) hdr[i] = 0xFF000000u;
    int line_h, base;
    FT_Face face = text_font(page.title_px, &line_h, &base);
    if (face)                                       /* a label at title_x, title_y */
        draw_text(hdr, hdr_w, hdr_rows, hdr_w, page.title_x, page.title_y + line_h - base, page.title_x + page.title_max_w,
                  face, title_text(), HDR_GOLD);
    fill_round_rect(hdr, hdr_w, hdr_rows, page.esc_x, page.esc_y, page.esc_w, page.esc_h, page.esc_radius,
                    esc_pressed ? HDR_KEY_PRESSED : HDR_KEY_BG);
    face = text_font(page.esc_text_px, &line_h, &base);
    if (face) {                                     /* "Esc" centred in the button as lv_obj_center does */
        const int x = page.esc_x + page.esc_w / 2 - text_width(face, "Esc") / 2;
        const int y = page.esc_y + page.esc_h / 2 - line_h / 2;
        draw_text(hdr, hdr_w, hdr_rows, page.esc_x + page.esc_w, x, y + line_h - base, page.esc_x + page.esc_w, face,
                  "Esc", HDR_KEY_TEXT);
    }
    cp0_statusbar_t *bar = strip_renderer();
    if (bar && strip.x >= 0 && strip.x < hdr_w)     /* the strip's canvas: strip.w wide at strip.x */
        cp0_statusbar_render_scaled(bar, hdr + strip.x, hdr_w - strip.x < strip.w ? hdr_w - strip.x : strip.w, hdr_w, 0,
                                    strip.top, 0, &drawn, strip.pct);
    shown_state = drawn;
    shown_pressed = esc_pressed;
    return 1;
}

/* run_viz1090.sh asks for the program's window size before starting it:
 *   VIZ_TITLE=viz1090 VIZ_PRINT_MAP=1 LD_PRELOAD=libviz_fb.so /opt/viz1090/viz1090  ->  "WIDTH HEIGHT HEADER"
 * (the area under the header and the header's height, px), printed before the program's main() runs. */
__attribute__((constructor)) static void print_map_area(void)
{
    const char *v = getenv("VIZ_PRINT_MAP");
    if (!v || !v[0] || v[0] == '0') return;
    layout_init();
    printf("%d %d %d\n", canvas_w(), area_h, header_on ? page.header_h : 0);
    fflush(stdout);
    _exit(0);
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

/* Copies a logical block of ARGB8888 pixels to the framebuffer in its depth. */
static void put_block(int x, int y, int w, int h, const uint8_t *argb, int stride)
{
    if (fb_out.bpp == 4) cp0_fbo_blit(&fb_out, x, y, w, h, argb, stride);
    else cp0_fbo_blit_xrgb_scaled(&fb_out, x, y, w, h, argb, stride, 1, 0, 0, 0, 0, scratch, sizeof(scratch));
}

static void present_to_fb(SDL_Renderer *renderer)
{
    layout_init();
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
    if (!fb_ok) return;
    /* in its area (under the header): centred when smaller, cut at the right and bottom when bigger */
    frame_x = w < fb_out.lw ? (fb_out.lw - w) / 2 : 0;
    frame_y = area_y + (h < area_h ? (area_h - h) / 2 : 0);
    if (w != placed_w || h != placed_h) {             /* new window size: clear what it does not cover */
        if (w < fb_out.lw || h < area_h) cp0_fbo_fill_black(&fb_out, 0, area_y, fb_out.lw, area_h);
        placed_w = w;
        placed_h = h;
        if (header_on && h > area_h)
            fprintf(stderr, "[viz_fb] the window is %d px tall, its area under the header %d: the bottom is cut\n", h,
                    area_h);
    }
    put_block(frame_x, frame_y, w, h, frame, w * 4);
    if (header_on) {                                  /* opaque, every frame */
        header_update();
        if (hdr) put_block(0, 0, hdr_w, page.header_h, (const uint8_t *)hdr, hdr_w * 4);
    }

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

static void push_quit(const char *why)
{
    SDL_Event e;
    memset(&e, 0, sizeof(e));
    e.type = SDL_QUIT;
    fprintf(stderr, "[viz_fb] %s: quit\n", why);
    push(&e);
}

/* touch */
#define SLOTS 5
typedef struct { int active; int x, y; float lx, ly; float nx, ny; } slot_t;   /* raw, logical, window 0..1 */
enum { OWNER_NONE = 0, OWNER_MAP, OWNER_HEADER };
static int touch_fd = -1;
static slot_t slots[SLOTS], prev_slots[SLOTS];
static int cur_slot;
static int min_x, max_x, min_y, max_y;
static int swap_xy, inv_x, inv_y;
static int buffer_axes;             /* APPLAUNCH_TOUCH_ORIENT=buffer: the axes follow the framebuffer */
static float pinch_prev = -1;
static int owner[SLOTS];            /* where the finger went down: the program's window or the header */
static int on_esc[SLOTS];           /* ... on the Esc button */

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
    layout_init();                                  /* the rotation maps buffer-oriented touch; the header */
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
    memset(owner, 0, sizeof(owner));
    memset(on_esc, 0, sizeof(on_esc));
    esc_pressed = 0;
    pinch_prev = -1;
    struct input_absinfo info;
    if (ioctl(touch_fd, EVIOCGABS(ABS_MT_POSITION_X), &info) == 0) { min_x = info.minimum; max_x = info.maximum; }
    if (ioctl(touch_fd, EVIOCGABS(ABS_MT_POSITION_Y), &info) == 0) { min_y = info.minimum; max_y = info.maximum; }
    touch_axes();
    fprintf(stderr, "[viz_fb] touch screen opened (%s axes)\n", buffer_axes ? "buffer" : "legacy");
}

/* The Esc button's touch area: the button's columns widened by SPACE_S on each side, the header's full height. */
static int esc_hit(float lx, float ly)
{
    if (!header_on) return 0;
    const int slack = metrics.tok[CP0_TOK_SPACE_S];
    return ly >= 0 && ly < page.header_h && lx >= page.esc_x - slack && lx < page.esc_x + page.esc_w + slack;
}

/* A raw touch position -> logical canvas pixels, and the program's window, 0..1 on each axis. */
static void touch_map(slot_t *s)
{
    float lx, ly;
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
    s->lx = lx;
    s->ly = ly;
    s->nx = frame_w > 0 ? clamp01((lx - frame_x) / frame_w) : 0;
    s->ny = frame_h > 0 ? clamp01((ly - frame_y) / frame_h) : 0;
}

static void touch_report(void)
{
    for (int i = 0; i < SLOTS; ++i)
        if (slots[i].active) touch_map(&slots[i]);
    int pressed = 0;
    for (int i = 0; i < SLOTS; ++i) {
        const slot_t *s = &slots[i], *p = &prev_slots[i];
        SDL_Event e;
        memset(&e, 0, sizeof(e));
        e.tfinger.touchId = 1;
        e.tfinger.fingerId = i;
        if (s->active && !p->active) {    /* a new finger: the header's when it lands there */
            owner[i] = header_on && s->ly < area_y ? OWNER_HEADER : OWNER_MAP;
            on_esc[i] = owner[i] == OWNER_HEADER && esc_hit(s->lx, s->ly);
        }
        if (owner[i] == OWNER_HEADER) {   /* the program never sees it; Esc: a click when let go on the button */
            if (s->active) {
                if (on_esc[i] && esc_hit(s->lx, s->ly)) pressed = 1;
            } else if (p->active) {
                if (on_esc[i] && esc_hit(p->lx, p->ly)) push_quit("Esc button");
                owner[i] = OWNER_NONE;
                on_esc[i] = 0;
            }
            continue;
        }
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
            owner[i] = OWNER_NONE;
        }
    }
    esc_pressed = pressed;
    /* two fingers on the map: pinch */
    int a = -1, b = -1;
    for (int i = 0; i < SLOTS; ++i)
        if (slots[i].active && owner[i] == OWNER_MAP) { if (a < 0) a = i; else if (b < 0) b = i; }
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
        layout_init();
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
    if (owner[0] == OWNER_HEADER)
        fprintf(stderr, "[viz_fb] test tap at %d,%d: %s\n", X, Y, on_esc[0] ? "the Esc button" : "the header");
    else
        fprintf(stderr, "[viz_fb] test tap at %d,%d: the program, finger at %.3f,%.3f of its %dx%d window\n", X, Y,
                slots[0].nx, slots[0].ny, frame_w, frame_h);
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
        case KEY_ESC:                                    /* viz1090 quits on Esc (a short press is enough) */
            if (ev.value == 1) fprintf(stderr, "[viz_fb] Esc key: quit\n");
            push_key(SDLK_ESCAPE, SDL_SCANCODE_ESCAPE);
            continue;
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

/* SIGTERM (the launcher's hold-Esc watchdog, sent to the whole group), SIGINT and SIGHUP end the program by its own
 * way out (viz1090: SDL_QUIT -> exit(0)). SDL2 already turns SIGTERM into SDL_QUIT; viz1090 resets SIGINT to the
 * default after its start: the signals still at their default when events are first read get a handler here, one
 * shot (a second signal ends the program at once). */
static volatile sig_atomic_t quit_signal;
static int quit_sent;

static void on_quit_signal(int sig) { quit_signal = sig; }

static void signals_install(void)
{
    static int done;
    if (done) return;
    done = 1;
    const int sigs[] = {SIGTERM, SIGINT, SIGHUP};
    for (size_t i = 0; i < sizeof(sigs) / sizeof(sigs[0]); ++i) {
        struct sigaction old;
        if (sigaction(sigs[i], NULL, &old) != 0 || (old.sa_flags & SA_SIGINFO) || old.sa_handler != SIG_DFL) continue;
        struct sigaction sa;
        memset(&sa, 0, sizeof(sa));
        sa.sa_handler = on_quit_signal;
        sigemptyset(&sa.sa_mask);
        sa.sa_flags = SA_RESETHAND;
        sigaction(sigs[i], &sa, NULL);
    }
}

int SDL_PollEvent(SDL_Event *event)
{
    if (!real_poll) real_poll = dlsym(RTLD_NEXT, "SDL_PollEvent");
    signals_install();
    if (quit_signal && !quit_sent) {
        quit_sent = 1;
        char why[32];
        snprintf(why, sizeof(why), "signal %d", (int)quit_signal);
        push_quit(why);
    }
    touch_poll();
    kbd_poll();
    if (q_head != q_tail) {
        if (event) {
            *event = queue[q_head];
            q_head = (q_head + 1) % QUEUE;
        }
        return 1;
    }
    return real_poll(event);
}
