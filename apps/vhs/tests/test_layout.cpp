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
#include "layout/playback_layout.hpp"
#include "layout/scenes_layout.hpp"

#include <algorithm>

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

void common_square(const CaptureLayout &l, int w, int h, int target);

void common(const CaptureLayout &l, int w, int h, int target, bool expect_target)
{
    if (l.square) {
        common_square(l, w, h, target);
        return;
    }
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

/* The Square class (the owner's canvas, 720x720; capture_layout.hpp): the invariants at any size and density. */
void common_square(const CaptureLayout &l, int w, int h, int target)
{
    const Rect screen{0, 0, w, h};
    const int minpx = w >= 720 ? 20 : 12; /* the type's floor: 20 px at 720, proportional below, never under 12 */
    CHECK(l.square && !l.column);
    CHECK(inside(l.panel, screen));
    CHECK(inside(l.picture, l.panel));
    CHECK_EQ(l.picture.w, l.panel.w - 2 * std::max(1, static_cast<int>(8 * l.k + 0.5)));
    /* the row: Photo / Video stacked left, the shutter in the middle, Scenes / Settings stacked right */
    const Rect row[] = {l.photo_btn, l.video_btn, l.shutter, l.scenes, l.settings};
    for (const Rect &r : row) {
        CHECK(inside(r, l.panel));
        CHECK(!overlap(r, l.picture));
        CHECK(r.y >= l.picture.bottom());
        CHECK(r.h >= target);
        CHECK(r.w >= target);
        CHECK(!overlap(r, l.status));
    }
    for (std::size_t i = 0; i < 5; ++i)
        for (std::size_t j = i + 1; j < 5; ++j) CHECK(!overlap(row[i], row[j]));
    CHECK_EQ(l.shutter.w, l.shutter.h);
    CHECK(inside(l.photo_btn, l.toggle) && inside(l.video_btn, l.toggle)); /* "Photo now" takes the column while recording */
    CHECK(!overlap(l.toggle, l.shutter) && !overlap(l.toggle, l.scenes) && !overlap(l.toggle, l.settings));
    CHECK_EQ(l.photo_btn.w, l.scenes.w);
    const int mid = l.shutter.x + l.shutter.w / 2;
    CHECK(mid >= l.panel.x + l.panel.w / 2 - 1 && mid <= l.panel.x + l.panel.w / 2 + 1);
    CHECK(l.toggle.h >= target);
    CHECK(l.status.y >= l.toggle.bottom());
    CHECK(inside(l.status, l.panel));
    CHECK(l.status.h >= l.status_px);
    CHECK(inside(l.toast, l.panel) && inside(l.toast, l.picture)); /* over the picture's bottom */
    CHECK(!overlap(l.toast, l.toggle) && !overlap(l.toast, l.status));
    /* the type */
    for (int px : {l.chip_px, l.status_px, l.button_px, l.mode_px, l.toggle_px, l.sub_px, l.tag_px, l.toast_px, l.nc_hint_px,
                   l.nc_text_px, l.title_px, l.photo_now_px, l.rec_tag_px, l.overlay_px})
        CHECK(px >= minpx);
    /* the recording's tags sit inside the picture, the lamp at the right */
    CHECK(l.chip_y + l.rec_tag_h <= l.picture.h && l.lamp_inset >= 0 && l.lamp_y + l.tag_h <= l.picture.h);
    CHECK(l.cd_d <= l.picture.h && l.cd_px >= minpx);
    /* the NoCamera page */
    for (const Rect &r : {l.nc_title, l.nc_text, l.nc_scenes, l.nch_title, l.nch_text, l.nc_hint, l.nch_scenes})
        CHECK(inside(r, l.panel));
    CHECK(l.nc_scenes.h >= target && l.nch_scenes.h >= target);
    CHECK(l.nc_scenes.y >= l.nc_text.bottom());
    CHECK(l.nch_scenes.y >= l.nc_hint.bottom());
}

void square_pages(int w, int h, int body_y, int target, int row, int list_row)
{
    const CaptureLayoutIn in = input(w, h, body_y, target);
    const Rect screen{0, 0, w, h};
    const int minpx = w >= 720 ? 20 : 12;
    /* Scenes */
    ScenesLayoutIn si;
    si.base = in;
    si.row = row;
    si.list_row = list_row;
    const ScenesLayout s = scenes_layout(si);
    CHECK(s.square && !s.portrait && s.head_right && s.chev == 0);
    CHECK(inside(s.panel, screen));
    for (int i = 0; i < 3; ++i) {
        CHECK(inside(s.chip[i], s.panel));
        CHECK(s.chip_hit[i].h >= row && s.chip_hit[i].w >= target);
        CHECK(!overlap(s.chip[i], s.list));
        if (i > 0) CHECK(!overlap(s.chip[i - 1], s.chip[i]));
        CHECK(!overlap(s.chip[i], s.head));
    }
    CHECK(inside(s.head, s.panel) && inside(s.list, s.panel) && inside(s.hint, s.panel));
    CHECK(s.list.y >= s.chip[0].bottom() && s.hint.y >= s.list.bottom() && !overlap(s.list, s.hint));
    CHECK(s.row_h >= list_row);
    CHECK(s.thumb.y >= 0 && s.thumb.bottom() <= s.row_h);
    CHECK(s.name_x >= s.thumb.right());
    CHECK(s.name_x + s.name_w + s.date_r <= s.list.w);
    CHECK(s.name_y + s.name_line <= s.meta_y && s.meta_y + s.meta_line <= s.row_h);
    CHECK(inside(s.toast, s.panel));
    for (int px : {s.chip_px, s.head_px, s.hint_px, s.name_px, s.meta_px, s.date_px, s.lock_px, s.empty_px, s.toast_px,
                   s.sheet_label_px, s.sheet_key_px, s.sheet_head_px, s.sheet_detail_px})
        CHECK(px >= minpx);
    CHECK(inside(s.sheet, s.panel));
    CHECK(s.sheet_row_h >= list_row && s.sheet_rows_h >= s.sheet_row_h);
    CHECK(s.sheet_row_y + s.sheet_rows_h <= s.sheet.h);
    CHECK(s.sheet_head.y >= 0 && s.sheet_head.bottom() <= s.sheet_row_y && s.sheet_head.right() <= s.sheet.w);
    CHECK_EQ(s.sheet_note.h, 0);
    /* Playback */
    const PlaybackLayout p = playback_layout(in, row);
    CHECK(p.square && !p.column);
    CHECK(inside(p.picture, p.panel));
    const Rect line1[] = {p.name, p.info};
    for (const Rect &r : line1) CHECK(inside(r, p.panel) && r.y >= p.picture.bottom());
    CHECK(!overlap(p.name, p.info));
    CHECK(inside(p.bar_hit, p.panel) && p.bar_hit.y >= p.name.bottom() && p.bar_hit.h >= row);
    CHECK(p.time_left.right() <= p.bar.x && p.bar.right() <= p.time_right.x);
    CHECK(p.bar.y >= p.bar_hit.y && p.bar.bottom() <= p.bar_hit.bottom());
    CHECK(p.knob >= 2 * p.bar.h);
    const Rect ctl[] = {p.pause, p.minus, p.volume, p.plus};
    for (std::size_t i = 0; i < 4; ++i) {
        CHECK(inside(ctl[i], p.panel));
        CHECK(ctl[i].y >= p.bar_hit.bottom());
        for (std::size_t j = i + 1; j < 4; ++j) CHECK(!overlap(ctl[i], ctl[j]));
    }
    CHECK(p.pause.h >= target && p.minus.h >= target && p.plus.h >= target && p.minus.w >= target);
    CHECK(p.pause.w >= 2 * target);
    CHECK(inside(p.toast, p.picture));
    for (int px : {p.pause_px, p.pm_px, p.volume_px, p.time_px, p.name_px, p.info_px, p.toast_px}) CHECK(px >= minpx);
    CHECK_EQ(seek_ms_at(p, p.bar.x, 10000), 0ll);
    CHECK_EQ(seek_ms_at(p, p.bar.right(), 10000), 10000ll);
    /* the photo review */
    const ReviewLayout r = review_layout(in);
    CHECK(r.square && !r.column);
    CHECK(inside(r.picture, r.panel));
    for (const Rect &b : {r.prev, r.next, r.del}) {
        CHECK(inside(b, r.panel) && !overlap(b, r.picture));
        CHECK(b.h >= target && b.y >= r.picture.bottom());
    }
    CHECK(!overlap(r.prev, r.next) && !overlap(r.next, r.del));
    CHECK(r.title.y >= r.prev.bottom() && r.note.y >= r.title.bottom() && inside(r.title, r.panel));
    CHECK(r.note.bottom() <= r.panel.bottom());
    CHECK(r.button_px >= minpx && r.title_px >= minpx && r.note_px >= minpx);
}

void square()
{
    /* the HackBerry (720x720, 6.68 px/mm: TARGET 47, header 64): the canvas's pixels */
    const CaptureLayout b = make(720, 720, 64, 47);
    CHECK(b.square && !b.column);
    CHECK(b.panel == (Rect{8, 64, 704, 648}));
    CHECK(b.picture == (Rect{16, 72, 688, 440}));
    CHECK(b.shutter == (Rect{300, 520, 120, 120}));
    CHECK(b.photo_btn == (Rect{16, 520, 270, 56}));
    CHECK(b.video_btn == (Rect{16, 584, 270, 56}));
    CHECK(b.toggle == (Rect{16, 520, 270, 120}));
    CHECK(b.scenes == (Rect{434, 520, 270, 56}));
    CHECK(b.settings == (Rect{434, 584, 270, 56}));
    CHECK(b.status == (Rect{22, 648, 676, 40}));
    CHECK_EQ(b.title_px, 28);
    CHECK_EQ(b.status_px, 22);
    CHECK_EQ(b.chip_px, 20);
    CHECK_EQ(b.button_px, 26);
    CHECK_EQ(b.photo_now_px, 28);
    CHECK_EQ(b.rec_tag_px, 34);
    CHECK_EQ(b.tag_px, 22);
    CHECK_EQ(b.toast_px, 24);
    CHECK_EQ(b.lamp_inset, 12);
    CHECK_EQ(b.rec_tag_dot, 26);
    CHECK_EQ(b.u, 2.0);
    common(b, 720, 720, 47, true);
    square_pages(720, 720, 64, 47, 47, 36);
    /* the canvases of Scenes, Playback and the sheet at 720x720 */
    ScenesLayoutIn si;
    si.base = input(720, 720, 64, 47);
    si.row = 47;
    si.list_row = 36;
    const ScenesLayout s = scenes_layout(si);
    CHECK(s.chip[0] == (Rect{16, 72, 152, 56}));
    CHECK_EQ(s.list.y, 72 + 56 + 8);
    CHECK_EQ(s.list.h, 520);
    CHECK_EQ(s.row_h, 96);
    CHECK(s.thumb == (Rect{8, 9, 104, 78}));
    CHECK_EQ(s.name_px, 26);
    CHECK_EQ(s.meta_px, 20);
    CHECK_EQ(s.sheet_row_h, 76);
    CHECK_EQ(s.sheet_label_px, 28);
    CHECK_EQ(s.sheet_key_px, 22);
    CHECK_EQ(s.sheet.h, 64 + 5 * 76 + 12);
    CHECK_EQ(s.sheet.bottom(), s.panel.bottom());
    const PlaybackLayout p = playback_layout(si.base, 47);
    CHECK(p.picture == (Rect{16, 72, 688, 400}));
    CHECK_EQ(p.pause.w, 230);
    CHECK_EQ(p.pause.h, 80);
    CHECK_EQ(p.minus.w, 80);
    CHECK_EQ(p.volume.w, 190);
    CHECK_EQ(p.bar_hit.h, 56);
    CHECK_EQ(p.knob, 32);
    CHECK_EQ(p.time_px, 24);
    CHECK_EQ(p.name_px, 28);
    /* other squares: the type scales with the panel (never under 12 px, 20 px at 720); the targets stay */
    for (int side : {480, 640, 720, 800, 1024}) {
        for (int target : {47, 56, 64, 70, 79}) {
            const int body = side >= 720 ? 64 : 56;
            const CaptureLayout c = make(side, side, body, target);
            common(c, side, side, target, true);
            square_pages(side, side, body, target, target, std::max(24, target * 3 / 4));
        }
    }
    CHECK_EQ(make(480, 480, 56, 47).chip_px, 13);
    CHECK_EQ(make(640, 640, 56, 47).status_px, 20);
    CHECK_EQ(make(1024, 1024, 64, 47).status_px, 31);
    CHECK_EQ(square_scale(720, 720), 1.0);
    CHECK_EQ(square_scale(480, 480), 480 / 720.0);
    CHECK_EQ(square_scale(100, 100), 0.4);
    /* 640x480 is not square (4:3): the column stays (the row would leave it smaller) */
    CHECK(make(640, 480, 56, 63).column);
    CHECK(make(640, 480, 56, 63).picture.w == 448);
    /* the canvas sizes are not square: unchanged */
    CHECK(make(480, 320, 40, 44).column);
    CHECK(!make(320, 480, 40, 44).square);
    CHECK_EQ(make(480, 320, 40, 44).u, 1.0);
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
        if (!r.square) CHECK_EQ(r.picture.w % 16, 0);
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
