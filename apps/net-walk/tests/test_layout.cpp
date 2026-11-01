/*
 * SPDX-License-Identifier: MIT
 *
 * Net Walk unit tests: the layout arithmetic (whole-number scale, everything inside the canvas, nothing overlapping,
 * hit testing) at the screens of the boards and the portrait canvases, and the bitmap font.
 */
#include "test_util.hpp"

#include "layout.hpp"
#include "pixfont.hpp"
#include "sprites.hpp"

#include <algorithm>
#include <cstring>

using namespace netwalk;

namespace {

struct Screen {
    int w, h;
};

/* the BODIES (screen minus the shared page header, as the harness measured it: 480x320 40 px, 640x480 / 800x480 / 1024x600 /
 * the portrait canvases 56 px, 720x720 and 1280x800 64 px) of the landscape screens of the boards and the portrait canvases
 * of PDA mode; 1280x720 and 320x480 are not measured (56 assumed) */
const Screen kScreens[] = {{480, 280}, {640, 424}, {800, 424}, {1024, 544}, {1280, 664}, {1280, 736}, {720, 656},
                           {480, 744}, {480, 584}, {800, 1224}, {320, 424}};

bool inside(const Rect &r, int lw, int lh)
{
    return r.x >= 0 && r.y >= 0 && r.x + r.w <= lw && r.y + r.h <= lh && r.w > 0 && r.h > 0;
}

bool apart(const Rect &a, const Rect &b)
{
    return a.x + a.w <= b.x || b.x + b.w <= a.x || a.y + a.h <= b.y || b.y + b.h <= a.y;
}

void test_layouts()
{
    for (const Screen &s : kScreens) {
        for (int n : {5, 7, 9}) {
            const Layout L = compute_layout(s.w, s.h, n);
            const bool portrait = s.h > s.w;
            const bool fits = portrait ? (s.w >= 160 * 2 && s.h >= 200 * 2) : true; /* 320x424 is under the portrait minimum */
            if (s.w == 320 && s.h == 424) {
                /* below the declared 480x640: the arithmetic still gives a drawable layout (scale 2 of 160x212) */
                CHECK(L.scale >= 1 && L.tile >= 4);
            }
            CHECK(L.scale >= 1);
            CHECK(L.lw * L.scale <= s.w && L.lh * L.scale <= s.h);
            CHECK(s.w - L.lw * L.scale < L.scale && s.h - L.lh * L.scale < L.scale); /* the letterbox is under one logical pixel */
            CHECK(L.ox == (s.w - L.lw * L.scale) / 2);
            if (std::max(s.w, s.h) * 4 > std::min(s.w, s.h) * 5) CHECK(L.portrait == portrait); /* a near-square body may take either */
            if (!fits) continue;
            CHECK(L.ok);
            CHECK(L.tile >= 14 || n < 9); /* the 9 x 9 board at the smallest supported body is 14 logical px a tile */
            CHECK(inside(L.frame, L.lw, L.lh) && inside(L.board, L.lw, L.lh) && inside(L.panel, L.lw, L.lh));
            CHECK(L.board.w == L.tile * n && L.board.h == L.tile * n);
            CHECK(L.frame.w == L.board.w + 2 * kBevel);
            CHECK(apart(L.frame, L.panel));
            for (int i = 0; i < 3; ++i) {
                CHECK(inside(L.button[i], L.lw, L.lh));
                CHECK(apart(L.button[i], L.frame));
                CHECK(L.button[i].h >= kButtonH);
                for (int j = i + 1; j < 3; ++j) CHECK(apart(L.button[i], L.button[j]));
            }
            /* the readout lines are inside the panel and the buttons are clear of them */
            for (int i = 0; i < 4; ++i) {
                CHECK(inside(L.line[i], L.lw, L.lh));
                for (int j = 0; j < 3; ++j) CHECK(apart(L.line[i], L.button[j]));
            }
            /* the text of the readouts fits its cell: Moves 999 and Time 100:00 */
            const int cell = L.line[1].w;
            CHECK(text_width("Moves") + 2 + text_width("999") <= cell);
            CHECK(text_width("Best") + 2 + text_width("999") <= L.line[3].w);
            CHECK(text_width("Time") + 2 + text_width("100:00") <= L.line[2].w);
            CHECK(text_width("Expert wrap") <= L.line[0].w);
            const int label_w = text_width(L.short_new ? "New" : "New game");
            CHECK(label_w + 6 <= L.button[0].w);
            CHECK(text_width("Scores") + 6 <= L.button[2].w && text_width("Size") + 8 + 6 <= L.button[1].w);
            /* the scale is the largest whole number that keeps the minimum canvas */
            const int min_w = portrait ? kMinPortraitW : kMinLandscapeW, min_h = portrait ? kMinPortraitH : kMinLandscapeH;
            CHECK(s.w / (L.scale + 1) < min_w || s.h / (L.scale + 1) < min_h || L.portrait != portrait);
            /* hit testing: the middle of every tile finds that tile, the frame and the panel find none */
            for (int i = 0; i < n * n; ++i) {
                const Rect r = L.tile_rect(i);
                const int px = L.ox + (r.x + r.w / 2) * L.scale, py = L.oy + (r.y + r.h / 2) * L.scale;
                CHECK(L.tile_at(px, py) == i);
            }
            CHECK(L.tile_at(L.ox + L.frame.x * L.scale, L.oy + L.frame.y * L.scale) == -1);
            CHECK(L.tile_at(L.ox + (L.panel.x + 1) * L.scale, L.oy + (L.panel.y + 1) * L.scale) == -1);
            CHECK(L.tile_at(-5, -5) == -1 && L.tile_at(s.w + 10, s.h + 10) == -1);
        }
    }
}

void test_known_values()
{
    /* 480x320 under a 40 px header: a 480x280 body, scale 2 of 240x140, a 9x9 board of 14 logical px tiles (28 px) */
    const Layout a = compute_layout(480, 280, 9);
    CHECK(a.scale == 2 && a.lw == 240 && a.lh == 140 && a.tile == 14 && !a.portrait && a.ox == 0 && a.oy == 0);
    CHECK(a.panel.w >= kPanelMinW);
    CHECK(a.tile * a.scale == 28);
    /* the smaller boards get bigger tiles on the same screen */
    CHECK(compute_layout(480, 280, 5).tile > compute_layout(480, 280, 7).tile);
    CHECK(compute_layout(480, 280, 7).tile > compute_layout(480, 280, 9).tile);
    /* the deck: 640x480 under a 56 px header is scale 2 of 320x212, the tile grows with the room */
    const Layout d = compute_layout(640, 424, 9);
    CHECK(d.scale == 2 && d.lw == 320 && d.lh == 212 && d.tile == 22);
    /* 800x480 and the tablet take scale 3 and 4 or more */
    CHECK(compute_layout(800, 424, 9).scale == 3);
    CHECK(compute_layout(1280, 736, 9).scale >= 4);
    /* portrait: 480x800 under 56 px is scale 3 of 160x248, the panel under the board */
    const Layout p = compute_layout(480, 744, 9);
    CHECK(p.portrait && p.scale == 3 && p.lw == 160 && p.lh == 248 && p.short_new);
    CHECK(p.panel.y > p.frame.y + p.frame.h);
    /* 480x640 under 56 px: 584 px is under 3 x 200, so scale 2 of 240x292 (tile 25 = 50 px, bigger than scale 3 would give) */
    const Layout q = compute_layout(480, 584, 9);
    CHECK(q.portrait && q.scale == 2 && q.lw == 240 && q.lh == 292 && q.tile == 25 && q.ok);
    /* a near-square body (720x720 under 64 px) takes the orientation with the bigger tiles: portrait, 17 x 3 = 51 px */
    const Layout sq = compute_layout(720, 656, 9);
    CHECK(sq.ok && sq.portrait && sq.tile * sq.scale >= 17 * 3);
    /* the same arguments, the same layout */
    const Layout a2 = compute_layout(480, 280, 9);
    CHECK(a2.tile == a.tile && a2.board.x == a.board.x && a2.panel.x == a.panel.x);
    /* below the smallest canvas: scale 1, not ok (the app shows its notice before it comes to this) */
    CHECK(!compute_layout(200, 100, 9).ok);
}

void test_sprites()
{
    for (SpriteKind k : {SpriteKind::Monitor, SpriteKind::Server})
        for (bool small : {true, false}) {
            const Sprite &s = sprite(k, small);
            CHECK(s.w > 0 && s.h > 0 && s.rows);
            for (int r = 0; r < s.h; ++r) {
                CHECK(static_cast<int>(std::strlen(s.rows[r])) == s.w);
                for (int c = 0; c < s.w; ++c) {
                    const char ch = s.rows[r][c];
                    CHECK(ch == '.' || sprite_color(k, ch, true) >= 0); /* every character has a colour */
                }
            }
            /* on the smallest tile of its level the device is inside with cable room (small: 12, normal: 19) */
            const int tile = small ? 12 : kSmallTile;
            CHECK(s.w + 2 <= tile && s.h + 2 <= tile);
            CHECK(sprite_factor(s, tile) == 1);
            CHECK(small == use_small_sprite(tile));
            /* the factor grows whole and leaves 3 px of cable on every side */
            for (int t = 12; t <= 80; ++t) {
                const int f = sprite_factor(s, t);
                CHECK(f >= 1 && (f == 1 || (s.w * f <= t - 6 && s.h * f <= t - 6)));
            }
        }
    CHECK(sprite(SpriteKind::Monitor, true).w == 9 && sprite(SpriteKind::Monitor, true).h == 8);
    CHECK(sprite(SpriteKind::Server, true).w == 7 && sprite(SpriteKind::Server, true).h == 10);
    /* a monitor with power shows a lime screen, one without a blue one, at both detail levels */
    for (bool small : {true, false}) {
        const Sprite &m = sprite(SpriteKind::Monitor, small);
        int screens = 0;
        for (int r = 0; r < m.h; ++r)
            for (int c = 0; c < m.w; ++c)
                if (m.rows[r][c] == 's') ++screens;
        CHECK(screens >= 8);
        CHECK(sprite_color(SpriteKind::Monitor, 's', true) == 0x00FF00 && sprite_color(SpriteKind::Monitor, 's', false) == 0x1818C8);
        CHECK(sprite_color(SpriteKind::Monitor, 'e', true) != sprite_color(SpriteKind::Monitor, 'e', false));
        CHECK(sprite_color(SpriteKind::Monitor, 'g', true) != sprite_color(SpriteKind::Monitor, 'g', false));
    }
    /* the server carries a red accent, an amber and a green LED; it has no off state */
    for (bool small : {true, false}) {
        const Sprite &sv = sprite(SpriteKind::Server, small);
        bool red = false, amber = false, green = false;
        for (int r = 0; r < sv.h; ++r)
            for (int c = 0; c < sv.w; ++c) {
                red = red || sv.rows[r][c] == 'R';
                amber = amber || sv.rows[r][c] == 'A';
                green = green || sv.rows[r][c] == 'G';
            }
        CHECK(red && amber && green);
    }
    for (char c : {'#', 'D', 'H', 'S', 'R'}) CHECK(sprite_color(SpriteKind::Server, c, true) == sprite_color(SpriteKind::Server, c, false));
}

} // namespace

void test_layout_group()
{
    test_layouts();
    test_known_values();
    test_sprites();
}

void test_font_group()
{
    CHECK(font_table_ok());
    /* every character the screens write has a glyph */
    for (const char *s : {"Net Walk", "Moves", "Time", "Best", "New game", "New", "Size", "Scores", "Easy", "Normal", "Hard",
                          "Expert", "Expert wrap", "Network up!", "OK", "Cancel", "Quit", "0123456789", "5x5 7x7 9x9 9x9 wrap",
                          "01:37", "--", "--:--", "New best moves!", "New best time!", "1 move, 00:09", "41 moves, 01:37"})
        CHECK(has_glyphs(s));
    CHECK(!has_glyphs("@"));
    CHECK(text_width("") == 0 && text_width("A") == 6 && text_width("AB") == 13);
    /* a glyph is 5 wide, at most 8 rows, and the rows beyond the capitals' 7 are only used by descenders */
    for (char c = 'A'; c <= 'Z'; ++c) CHECK(glyph_row(c, 7) == 0 && glyph_row(c, 0) != 0);
    for (char c : {'g', 'j', 'p', 'q', 'y'}) CHECK(glyph_row(c, 7) != 0);
    CHECK(glyph_row('x', 0) == 0 && glyph_row('x', 2) != 0);
    CHECK(glyph_row('A', -1) == 0 && glyph_row('A', 8) == 0);
    /* an unknown character draws as the question mark */
    for (int r = 0; r < kGlyphRows; ++r) CHECK(glyph_row('@', r) == glyph_row('?', r));
    /* the capitals are told apart (no two the same) */
    for (char a = 'A'; a <= 'Z'; ++a)
        for (char b = static_cast<char>(a + 1); b <= 'Z'; ++b) {
            bool same = true;
            for (int r = 0; r < kGlyphRows; ++r) same = same && glyph_row(a, r) == glyph_row(b, r);
            CHECK(!same);
        }
}
