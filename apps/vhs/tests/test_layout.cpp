/*
 * SPDX-License-Identifier: MIT
 *
 * VHS tests: the Capture page's geometry (core/layout/capture_layout.hpp) in every screen class: the canvas's numbers at
 * 480x320 and 320x480 with a 44 px TARGET (picture 352x198 / 288x162, the 88 px column, the 72 / 84 px shutter, the
 * NoCamera block), the 16:9 picture box with a width that is a multiple of 16, the column 2 x TARGET wide at the right
 * (landscape, square) or the rows under the picture (portrait), buttons at least TARGET high where they fit, nothing
 * outside the panel or the screen, no overlaps.
 */
#include "check.hpp"

#include "layout/capture_layout.hpp"

using namespace vhs;

namespace {

bool inside(const Rect &in, const Rect &out)
{
    return in.x >= out.x && in.y >= out.y && in.right() <= out.right() && in.bottom() <= out.bottom();
}

bool overlap(const Rect &a, const Rect &b)
{
    return a.x < b.right() && b.x < a.right() && a.y < b.bottom() && b.y < a.bottom();
}

CaptureLayoutIn input(int w, int h, int body_y, int target)
{
    CaptureLayoutIn in;
    in.w = w;
    in.h = h;
    in.body_y = body_y;
    in.target = target;
    /* the layout service's Square class (cp0_ui_classify) */
    in.square = 5 * w >= 4 * h && 4 * w <= 5 * h && h >= 400;
    return in;
}

CaptureLayout make(int w, int h, int body_y, int target)
{
    return capture_layout(input(w, h, body_y, target));
}

void common(const CaptureLayout &l, int w, int h, int target, bool expect_target)
{
    const Rect screen{0, 0, w, h};
    CHECK(inside(l.panel, screen));
    CHECK(inside(l.picture, l.panel));
    CHECK_EQ(l.picture.w % 16, 0);
    CHECK_EQ(l.picture.h, l.picture.w * 9 / 16);
    for (const Rect &r : {l.shutter, l.toggle, l.scenes, l.settings}) {
        CHECK(inside(r, l.panel));
        CHECK(!overlap(r, l.picture));
        if (expect_target) CHECK(r.h >= target);
    }
    CHECK(!overlap(l.shutter, l.toggle));
    CHECK(!overlap(l.toggle, l.scenes));
    CHECK(!overlap(l.scenes, l.settings));
    CHECK(!overlap(l.shutter, l.settings));
    CHECK_EQ(l.shutter.w, l.shutter.h);
    CHECK(l.title.y >= l.picture.bottom());
    CHECK(inside(l.toast, l.panel));
    CHECK(!overlap(l.toast, l.picture));
    if (l.column) {
        CHECK_EQ(l.toggle.w, 2 * target);
        CHECK(l.toggle.x >= l.picture.right());
        CHECK(!overlap(l.title, l.toggle) && !overlap(l.title, l.scenes) && !overlap(l.title, l.settings));
    } else if (l.square) {
        /* one row under the picture, every button TARGET high, the shutter TARGET round */
        CHECK(l.toggle.y >= l.picture.bottom());
        CHECK_EQ(l.toggle.y, l.settings.y);
        CHECK_EQ(l.shutter.y, l.toggle.y);
        CHECK_EQ(l.shutter.w, target);
        CHECK(l.toggle.w >= target && l.scenes.w >= target && l.settings.w >= target);
        CHECK(l.title.y >= l.toggle.bottom());
    } else {
        CHECK(l.shutter.y >= l.picture.bottom());
        CHECK(l.settings.y >= l.shutter.bottom());
        CHECK(l.title.y >= l.settings.bottom());
    }
    for (const Rect &r : {l.nc_title, l.nc_text, l.nc_scenes, l.nch_title, l.nch_text, l.nc_hint, l.nch_scenes})
        CHECK(inside(r, l.panel));
    CHECK(l.nc_scenes.h >= target);
    CHECK(l.nc_scenes.y >= l.nc_text.bottom());
    CHECK(l.nch_scenes.y >= l.nc_hint.bottom());
}

void canvas_sizes()
{
    /* 480x320 at a 44 px TARGET (the canvas's): its pixels */
    CaptureLayout l = make(480, 320, 40, 44);
    CHECK(l.column);
    CHECK(l.panel == (Rect{8, 44, 464, 268}));
    CHECK(l.picture == (Rect{16, 52, 352, 198}));
    CHECK(l.shutter == (Rect{384, 52, 72, 72})); /* canvas: panel (376, 8) */
    CHECK_EQ(l.toggle.x, 376);
    CHECK_EQ(l.toggle.w, 88);
    CHECK_EQ(l.toggle.y, 52 + 72 + 6);
    CHECK_EQ(l.toggle.h, 44); /* the canvas's 34, raised to TARGET */
    CHECK_EQ(l.title.x, 20);
    CHECK_EQ(l.title.y, 258); /* canvas: panel y 214 */
    CHECK_EQ(l.note.y, 278);  /* canvas: panel y 234 */
    CHECK_EQ(l.note.w, 340);
    CHECK_EQ(l.title_px, 14);
    CHECK_EQ(l.note_px, 11);
    CHECK_EQ(l.chip_px, 11);
    CHECK_EQ(l.toast_h, 30);
    CHECK_EQ(l.nc_title_px, 18);
    CHECK_EQ(l.nc_text.w, 360);
    CHECK_EQ(l.nc_scenes.w, 160);
    CHECK_EQ(l.nc_scenes.x, 8 + 152);
    common(l, 480, 320, 44, true);
    /* 320x480 */
    l = make(320, 480, 40, 44);
    CHECK(!l.column);
    CHECK(l.panel == (Rect{8, 44, 304, 428}));
    CHECK(l.picture == (Rect{16, 52, 288, 162}));
    CHECK_EQ(l.shutter.w, 84);                   /* canvas: 84 */
    CHECK_EQ(l.shutter.x, 8 + 110);              /* canvas: panel x 110 */
    CHECK_EQ(l.shutter.y, 52 + 162 + 30);        /* canvas: panel y 200 */
    CHECK(l.toggle == (Rect{20, 244 + 42 - 28, 88, 56}));
    CHECK_EQ(l.scenes.x, 8 + 204);
    CHECK(l.settings == (Rect{20, 334, 280, 44})); /* canvas: 280 x 40 at panel y 290 */
    common(l, 320, 480, 44, true);
}

void square()
{
    /* G1 review: 480x480 gave a picture a column and a text block left about 240 px wide; the row gives the width */
    const CaptureLayout a = make(480, 480, 64, 56); /* a 480x480 panel at 8.0 px/mm (Vinyl's np-square) */
    CHECK(a.square && !a.column);
    CHECK_EQ(a.picture.w, 432);
    CHECK_EQ(a.picture.h, 243);
    common(a, 480, 480, 56, true);
    const CaptureLayout b = make(720, 720, 64, 70); /* the HackBerry, 10 px/mm */
    CHECK(b.square);
    CHECK_EQ(b.picture.w, 672);
    common(b, 720, 720, 70, true);
    const CaptureLayout c = make(480, 480, 64, 79); /* the harness's default 11.3 px/mm: height-limited, still wider */
    CHECK(c.picture.w >= 288);
    common(c, 480, 480, 79, true);
    /* 640x480 is not square (4:3): the column stays (the row would leave it smaller) */
    CHECK(make(640, 480, 56, 63).column);
    CHECK(make(640, 480, 56, 63).picture.w == 448);
    /* the canvas sizes are not square: unchanged */
    CHECK(make(480, 320, 40, 44).column);
    CHECK(!make(320, 480, 40, 44).square);
}

void countdown()
{
    const CaptureLayout l = make(480, 320, 40, 44);
    CHECK_EQ(l.cd_d, 96);   /* the canvas's ring */
    CHECK_EQ(l.cd_border, 6);
    CHECK_EQ(l.cd_px, 52);
    const CaptureLayout p = make(320, 480, 40, 44);
    CHECK(p.cd_d <= p.picture.h);
}

void review()
{
    /* 480x320, the canvas's PhotoReview board: the picture, Previous at panel (368, 8), Next (368, 56), Send to Notes
     * (368, 112), Delete (368, 160): 88 wide; TARGET high here (the canvas draws 40) */
    ReviewLayout r = review_layout(input(480, 320, 40, 44));
    CHECK(r.column);
    CHECK(r.picture == (Rect{16, 52, 352, 198}));
    CHECK(r.prev == (Rect{376, 52, 88, 44}));
    CHECK_EQ(r.next.y, 52 + 44 + 8);
    CHECK_EQ(r.notes.y, r.next.bottom() + 16);
    CHECK_EQ(r.del.y, r.notes.bottom() + 8);
    CHECK(inside(r.del, r.panel));
    CHECK_EQ(r.title.y, 258);
    struct Case {
        int w, h, body_y, target;
    };
    for (const Case &c : {Case{320, 480, 40, 47}, Case{800, 480, 56, 64}, Case{480, 480, 64, 56}, Case{720, 720, 64, 70},
                          Case{480, 800, 56, 60}, Case{640, 480, 56, 79}, Case{480, 320, 40, 45}}) {
        r = review_layout(input(c.w, c.h, c.body_y, c.target));
        const Rect screen{0, 0, c.w, c.h};
        CHECK(inside(r.panel, screen));
        CHECK(inside(r.picture, r.panel));
        CHECK_EQ(r.picture.w % 16, 0);
        for (const Rect &b : {r.prev, r.next, r.del}) {
            CHECK(inside(b, r.panel));
            CHECK(!overlap(b, r.picture));
            CHECK(b.h >= c.target || c.target == 79);
        }
        CHECK(!overlap(r.prev, r.next) && !overlap(r.next, r.del));
        CHECK(r.title.y >= r.picture.bottom());
        if (!r.column) CHECK(r.title.y >= r.prev.bottom());
    }
}

void classes()
{
    /* the classes of D27 at the harness densities (TARGET = 7 mm): body_y from the class header */
    struct Case {
        int w, h, body_y, target;
        bool fits;
    };
    const Case cases[] = {
        {480, 320, 40, 45, true},    /* qa-small, 6.3 px/mm */
        {320, 480, 40, 47, true},    /* zero8 upright */
        {640, 480, 56, 63, true},    /* Medium, 9.0 px/mm */
        {800, 480, 56, 64, true},    /* the 7" DSI boards */
        {480, 480, 64, 52, true},    /* Square: the row */
        {720, 720, 64, 70, true},    /* Square */
        {480, 800, 56, 60, true},    /* Portrait tall */
        {480, 640, 56, 63, true},    /* Portrait */
        {1280, 720, 64, 81, true},   /* Large */
        {1024, 600, 64, 79, true},
        {640, 480, 56, 79, false},   /* the deck: 11.3 px/mm, the column too short for three TARGET buttons and the
                                        shutter: the shutter shrinks first */
    };
    for (const Case &c : cases) {
        const CaptureLayout l = make(c.w, c.h, c.body_y, c.target);
        common(l, c.w, c.h, c.target, c.fits);
        CHECK(l.picture.w >= 256);
    }
    /* 800x480: a picture box near the brief's "about 640x360" (the text block under it costs some height) */
    const CaptureLayout w = make(800, 480, 56, 64);
    CHECK(w.picture.w >= 512);
    CHECK_EQ(floor16(527), 512);
    CHECK_EQ(floor16(-3), 0);
    /* the deck: buttons still TARGET high, the shutter at least TARGET */
    const CaptureLayout d = make(640, 480, 56, 79);
    CHECK(d.toggle.h == 79 && d.shutter.w >= 79);
}

} // namespace

void test_layout()
{
    canvas_sizes();
    classes();
    square();
    countdown();
    review();
}
