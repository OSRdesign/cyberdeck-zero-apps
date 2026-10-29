/*
 * SPDX-License-Identifier: MIT
 *
 * viz_screen.h: which screen viz1090 runs on. Shared by the display bridge (build/viz_fb_shim.c, built with
 * -I src/core) and the intro screen's `vizsetup --screen` for run_viz1090.sh (src/main.cpp), so the map, the bridge and
 * the script agree on the size. (The intro screen itself sizes from the launcher's layout service.)
 *
 * Order (the launcher's app contract, docs/APP-CONTRACT.md of cyberdeck-zero-launcher, section 7):
 *   size    VIZ_SCREEN=WxH (manual override) > APPLAUNCH_CANVAS (contract 1, fullscreen: the logical panel)
 *           > APPLAUNCH_SCREEN_W/H > screen.state w/h > the framebuffer (FBIOGET_VSCREENINFO, turned by the rotation)
 *           > 640x480 (documented fallback, the deck)
 *   density APPLAUNCH_SCREEN_PPMM / APPLAUNCH_CANVAS_SCALE > screen.state ppmm > the framebuffer's mm > 11.3 px/mm
 *   rotation APPLAUNCH_SCREEN_ROTATE > screen.state rot > APPLAUNCH_ROTATE (board profile) > 0
 *   device  VIZ_FB > APPLAUNCH_SCREEN_FB > screen.state fb > APPLAUNCH_FB (board profile) > /dev/fb0
 *
 * Header-only, plain C99 + Linux fbdev. Never fails: the worst case is the 640x480 fallback.
 */
#pragma once

#include <fcntl.h>
#include <linux/fb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#define VIZ_FALLBACK_W 640          /* the deck, when nothing else is known */
#define VIZ_FALLBACK_H 480
#define VIZ_DEFAULT_PPMM_X100 1130  /* the launcher's assumed density (11.3 px/mm) */

typedef struct {
    int w, h;              /* logical (landscape) canvas the app draws, px */
    int ppmm_x100;         /* density of that canvas, px per mm x100 */
    int rot;               /* clockwise pre-rotation of the picture into the framebuffer: 0, 90, 180, 270 */
    char fb[128];          /* framebuffer device */
    char touch[128];       /* touch device the launcher reads ("" = unknown) */
    const char *source;    /* where the size came from: override, contract, panel, screen.state, framebuffer, fallback */
} viz_screen_t;

static inline int viz_parse_int(const char *v, int *out)
{
    if (!v || !v[0]) return -1;
    char *end = NULL;
    const long x = strtol(v, &end, 10);
    if (!end || *end != '\0') return -1;
    *out = (int)x;
    return 0;
}

static inline int viz_parse_wxh(const char *v, int *w, int *h)
{
    if (!v || !v[0]) return -1;
    char *end = NULL;
    const long a = strtol(v, &end, 10);
    if (end == v || (*end != 'x' && *end != 'X')) return -1;
    const char *p = end + 1;
    const long b = strtol(p, &end, 10);
    if (end == p || *end != '\0' || a < 64 || b < 64 || a > 8192 || b > 8192) return -1;
    *w = (int)a;
    *h = (int)b;
    return 0;
}

static inline int viz_valid_rot(int r) { return r == 0 || r == 90 || r == 180 || r == 270; }

/* screen.state of the launcher: v=1, w, h, rot, ppmm, fb. Returns 0 when read. */
static inline int viz_read_screen_state(int *w, int *h, int *rot, int *ppmm, char *fb, size_t fb_size)
{
    char path[512];
    const char *dir = getenv("APPLAUNCH_STATE_DIR");
    const char *run = getenv("XDG_RUNTIME_DIR");
    if (dir && dir[0]) snprintf(path, sizeof(path), "%s/screen.state", dir);
    else if (run && run[0]) snprintf(path, sizeof(path), "%s/applaunch/screen.state", run);
    else snprintf(path, sizeof(path), "/tmp/applaunch-screen.state");
    FILE *file = fopen(path, "r");
    if (!file) return -1;
    char line[256];
    int version = 0;
    *w = *h = *ppmm = 0;
    *rot = -1;
    fb[0] = '\0';
    while (fgets(line, sizeof(line), file)) {
        line[strcspn(line, "\r\n")] = '\0';
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = '\0';
        const char *key = line, *value = eq + 1;
        int n = 0;
        if (!strcmp(key, "v") && !viz_parse_int(value, &n)) version = n;
        else if (!strcmp(key, "w") && !viz_parse_int(value, &n)) *w = n;
        else if (!strcmp(key, "h") && !viz_parse_int(value, &n)) *h = n;
        else if (!strcmp(key, "rot") && !viz_parse_int(value, &n)) *rot = n;
        else if (!strcmp(key, "ppmm") && !viz_parse_int(value, &n)) *ppmm = n;
        else if (!strcmp(key, "fb")) snprintf(fb, fb_size, "%s", value);
    }
    fclose(file);
    return version == 1 ? 0 : -1;
}

static inline void viz_screen_detect(viz_screen_t *s)
{
    memset(s, 0, sizeof(*s));
    s->rot = -1;
    s->source = "fallback";

    int st_w = 0, st_h = 0, st_rot = -1, st_ppmm = 0;
    char st_fb[128] = "";
    const int have_state = viz_read_screen_state(&st_w, &st_h, &st_rot, &st_ppmm, st_fb, sizeof(st_fb)) == 0;

    /* framebuffer device and rotation */
    const char *fb_candidates[] = {getenv("VIZ_FB"), getenv("APPLAUNCH_SCREEN_FB"), have_state ? st_fb : NULL,
                                   getenv("APPLAUNCH_FB")};
    snprintf(s->fb, sizeof(s->fb), "/dev/fb0");
    for (size_t i = 0; i < sizeof(fb_candidates) / sizeof(fb_candidates[0]); ++i)
        if (fb_candidates[i] && fb_candidates[i][0] && strcmp(fb_candidates[i], "0") != 0) {
            snprintf(s->fb, sizeof(s->fb), "%s", fb_candidates[i]);
            break;
        }
    int r = 0;
    if (!viz_parse_int(getenv("APPLAUNCH_SCREEN_ROTATE"), &r) && viz_valid_rot(r)) s->rot = r;
    else if (have_state && viz_valid_rot(st_rot)) s->rot = st_rot;
    else if (!viz_parse_int(getenv("APPLAUNCH_ROTATE"), &r) && viz_valid_rot(r)) s->rot = r;
    else s->rot = 0;
    const char *touch = getenv("APPLAUNCH_SCREEN_TOUCH");
    if (touch && touch[0] && strcmp(touch, "0") != 0) snprintf(s->touch, sizeof(s->touch), "%s", touch);

    /* size */
    const char *contract = getenv("APPLAUNCH_CONTRACT");
    const int have_contract = contract && contract[0] && strcmp(contract, "0") != 0;
    int scale = 1;
    if (have_contract && (viz_parse_int(getenv("APPLAUNCH_CANVAS_SCALE"), &scale) || scale < 1)) scale = 1;
    int w = 0, h = 0;
    if (!viz_parse_wxh(getenv("VIZ_SCREEN"), &w, &h)) {
        s->source = "override";
    } else if (have_contract && !viz_parse_wxh(getenv("APPLAUNCH_CANVAS"), &w, &h)) {
        s->source = "contract";
    } else if (!viz_parse_int(getenv("APPLAUNCH_SCREEN_W"), &w) && !viz_parse_int(getenv("APPLAUNCH_SCREEN_H"), &h) &&
               w >= 64 && h >= 64) {
        s->source = "panel";
    } else if (have_state && st_w >= 64 && st_h >= 64) {
        w = st_w;
        h = st_h;
        s->source = "screen.state";
    } else {
        w = h = 0;
    }

    /* density */
    int ppmm = 0;
    if (!viz_parse_int(getenv("APPLAUNCH_SCREEN_PPMM"), &ppmm) && ppmm > 0) ppmm = (ppmm + scale / 2) / scale;
    else if (have_state && st_ppmm > 0) ppmm = st_ppmm;
    else ppmm = 0;

    /* the framebuffer itself: size when nothing else said it, density when its driver knows the mm */
    if (w <= 0 || ppmm <= 0) {
        const int fd = open(s->fb, O_RDONLY);
        struct fb_var_screeninfo var;
        if (fd >= 0 && ioctl(fd, FBIOGET_VSCREENINFO, &var) == 0 && var.xres >= 64 && var.yres >= 64) {
            const int swap = s->rot == 90 || s->rot == 270;
            if (w <= 0) {
                w = (int)(swap ? var.yres : var.xres);
                h = (int)(swap ? var.xres : var.yres);
                s->source = "framebuffer";
            }
            if (ppmm <= 0 && (int)var.width > 0 && (int)var.width < 2000) ppmm = (int)(var.xres * 100 / var.width);
        }
        if (fd >= 0) close(fd);
    }
    if (w <= 0 || h <= 0) {
        w = VIZ_FALLBACK_W;
        h = VIZ_FALLBACK_H;
        s->source = "fallback";
    }
    if (ppmm < 200 || ppmm > 6000) ppmm = VIZ_DEFAULT_PPMM_X100;
    s->w = w;
    s->h = h;
    s->ppmm_x100 = ppmm;
}

/* viz1090's --uiscale (its fonts are 12 px x uiscale): about the deck's look (uiscale 2 at 11.3 px/mm) in
 * millimetres, never so big that a short screen fills up with labels. 1 below 7.0 px/mm, 2 up to 14, then 3. */
static inline int viz_screen_uiscale(const viz_screen_t *s)
{
    int u = s->ppmm_x100 < 700 ? 1 : (s->ppmm_x100 < 1400 ? 2 : 3);
    /* by the SHORT side of the canvas, so that a portrait screen (480x800) is capped like its landscape twin */
    const int shorter = s->w > 0 && s->w < s->h ? s->w : s->h;
    const int cap = shorter / 240 > 1 ? shorter / 240 : 1;   /* 320 short rows: 1, 480: 2, 720: 3 */
    return u < cap ? u : cap;
}
