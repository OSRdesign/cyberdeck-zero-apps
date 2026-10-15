/*
 * SPDX-License-Identifier: MIT
 *
 * Net Toolbox: the graphic panels (LVGL thread only). Not interactive: the list under them keeps every key and tap.
 * Sizes come from the caller (layout service tokens and fonts); colours from style.hpp.
 *
 *   HealthLadder  the Diagnosis page: six rungs, each a state mark (hollow circle: waiting; spinning arc: being
 *                 checked; green disc with a check: pass; red disc with a cross: fail; a dash: skipped), the step's
 *                 name and its detail at the right. The mark is a shape and a colour, and the detail says it in words.
 *   PingPanel     the Ping page: the headline, the latency line graph of the last 120 packets (time on x, ms on y with
 *                 an automatic scale, the jitter band shaded behind the line, lost packets as red ticks on the baseline
 *                 and gaps in the line, slow ones as amber dots), the packet strip (one cell per packet, tall for a lost
 *                 one, medium for a slow one, short for a good one), its legend and the statistics.
 *   ResultBadge   the Port check page: a large mark and the result word, the host, what it means.
 *   NoticeBox     a bordered block of wrapped text (ping unavailable).
 */
#pragma once

#include "view_model.hpp"

#include "lvgl/lvgl.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace nt {
namespace ui {

/* the objects a panel built are deleted with it (and the panel forgets them when the page went first) */
class Owned {
public:
    Owned(const Owned &) = delete;
    Owned &operator=(const Owned &) = delete;
    lv_obj_t *root() const { return root_; }

protected:
    Owned() = default;
    ~Owned();
    void adopt(lv_obj_t *root);

private:
    static void deleted_cb(lv_event_t *event);
    lv_obj_t *root_ = nullptr;
};

/* draws its own shape: draw(layer, area) runs when LVGL paints the object */
class Painter {
public:
    using Draw = std::function<void(lv_layer_t *layer, const lv_area_t &area)>;
    Painter(lv_obj_t *parent, Draw draw);
    ~Painter();
    Painter(const Painter &) = delete;
    Painter &operator=(const Painter &) = delete;
    lv_obj_t *obj() const { return obj_; }
    void place(int x, int y, int w, int h);
    void invalidate();

private:
    static void draw_cb(lv_event_t *event);
    static void deleted_cb(lv_event_t *event);
    lv_obj_t *obj_ = nullptr;
    Draw draw_;
};

/* the drawing primitives of charts.cpp, for the other panels (chain.cpp): a filled or ringed rectangle (radius
 * LV_RADIUS_CIRCLE for a disc), a line, a dot, the glyph of a mark in a disc of radius r */
void paint_fill(lv_layer_t *layer, const lv_area_t &a, std::uint32_t rgb, int radius = 0, int opa = LV_OPA_COVER);
void paint_ring(lv_layer_t *layer, const lv_area_t &a, std::uint32_t rgb, int width, int radius);
void paint_line(lv_layer_t *layer, int x1, int y1, int x2, int y2, std::uint32_t rgb, int width, bool round = false);
void paint_dot(lv_layer_t *layer, int cx, int cy, int r, std::uint32_t rgb);
void paint_glyph(lv_layer_t *layer, int cx, int cy, int r, Mark mark, std::uint32_t rgb);

/* a state mark drawn in a w x w box: a disc of `rgb` with the glyph of `mark` in ink, or (Mark::None) a ring */
void draw_mark(lv_layer_t *layer, int cx, int cy, int r, Mark mark, std::uint32_t rgb, int flash = 0);
/* a spinning arc of `rgb` in a ring of the track colour; phase in degrees */
void draw_spinner(lv_layer_t *layer, int cx, int cy, int r, std::uint32_t rgb, int phase);

class HealthLadder : public Owned {
public:
    /* the ladder at x, y, w wide, rung_h per rung; font: the name and the detail text */
    HealthLadder(lv_obj_t *parent, int x, int y, int w, int rung_h, const lv_font_t *font);
    ~HealthLadder();
    int height() const { return rung_h_ * kStepCount; }
    void update(const DiagReport &report, std::int64_t now_ms);
    /* the spinner turns and the pass flash fades: call every 100 ms */
    void tick(std::int64_t now_ms);
    /* the mark of rung i (tests, the scene's checks) */
    StepState state(int i) const { return states_[static_cast<std::size_t>(i)]; }

private:
    struct Rung {
        std::unique_ptr<Painter> mark;
        lv_obj_t *name = nullptr;
        lv_obj_t *detail = nullptr;
    };
    int rung_h_ = 0, mark_ = 0, w_ = 0;
    std::vector<Rung> rungs_;
    std::unique_ptr<Painter> track_;
    std::vector<StepState> states_;
    std::vector<std::int64_t> flash_until_;
    std::int64_t now_ = 0;
};

class PingPanel : public Owned {
public:
    PingPanel(lv_obj_t *parent, int x, int y, int w, int h, const lv_font_t *font);
    ~PingPanel();
    void update(const PingSnapshot &snapshot);
    int graph_h() const { return graph_h_; }
    /* the panel's height the caller must give for a graph of at least graph_min px */
    static int height_for(int graph_min, const lv_font_t *font);

private:
    void layout();
    int w_ = 0, h_ = 0, graph_h_ = 0, strip_h_ = 0, line_h_ = 0, gap_ = 0;
    const lv_font_t *font_ = nullptr;
    lv_obj_t *head_ = nullptr, *scale_ = nullptr, *stats_ = nullptr;
    std::vector<lv_obj_t *> legend_;
    std::unique_ptr<Painter> graph_, strip_, swatches_;
    PingSnapshot snap_;
    double top_ms_ = 10;
};

class ResultBadge : public Owned {
public:
    ResultBadge(lv_obj_t *parent, int x, int y, int w, int h, const lv_font_t *caption, const lv_font_t *big);
    ~ResultBadge();
    void update(const PortSummary &summary);
    static int height_for(const lv_font_t *caption, const lv_font_t *big, int gap);

private:
    lv_obj_t *host_ = nullptr, *word_ = nullptr, *detail_ = nullptr;
    std::unique_ptr<Painter> mark_;
    PortSummary shown_;
    int w_ = 0, mark_d_ = 0, gap_ = 0;
};

class NoticeBox : public Owned {
public:
    NoticeBox(lv_obj_t *parent, int x, int y, int w, int h, const lv_font_t *font, const std::string &title,
              const std::string &text);
    ~NoticeBox();
};

} // namespace ui
} // namespace nt
