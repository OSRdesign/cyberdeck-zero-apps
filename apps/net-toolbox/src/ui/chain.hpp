/*
 * SPDX-License-Identifier: MIT
 *
 * Net Toolbox 0.2: the blocks of the new pages that RichList carries as Custom rows (LVGL thread only).
 *
 *   HopRowView      one hop of the traceroute chain: the line down the left and the node (its look says the state by shape
 *                   and colour, see HopLook), the hop number, the name and the address beside it (smaller), a latency bar
 *                   on the chain's shared scale with the best-to-worst range as a lighter extension, the average in ms and
 *                   the loss tag; the word under the name (reached, probably rate limiting); "no reply" in the bar's place
 *                   for a silent hop. Opened in place (Enter or a tap on the selected row) it shows all the addresses seen,
 *                   last / avg / best / worst, jitter, sent / received and the plain-words reading of the node.
 *   BadgeBlock      the HTTP status badge (a ResultBadge in a row)
 *   WaterfallBlock  the HTTP timing: one bar split into DNS, Connect, TLS, Waiting, Transfer on one scale, each segment in
 *                   its own tone and named below with its milliseconds
 *   CertBlock       the certificate badge: a mark, "expires in 83 days", the subject
 */
#pragma once

#include "charts.hpp"
#include "rich_list.hpp"

#include <memory>
#include <string>
#include <vector>

namespace nt {
namespace ui {

/* the columns of a hop row, from the row's width and the fonts */
struct ChainStyle {
    const lv_font_t *main = nullptr;
    const lv_font_t *small = nullptr;
    int w = 0;          /* the row's width (the list's) */
    int row_h = 0;      /* the row without its details */
    int inset = 0;      /* the list's side inset */
    int gap = 0;
    int lh = 0, slh = 0;
    int y0 = 0;         /* the top of the main line */
    int node_cx = 0, node_cy = 0, node_r = 0;
    int num_x = 0, num_w = 0;
    int name_x = 0, name_w = 0;
    int ip_x = 0, ip_w = 0;
    int bar_x = 0, bar_w = 0, bar_h = 0, bar_full = 0, stub = 0;
    int ms_x = 0, ms_w = 0;
    int tag_x = 0, tag_w = 0;
    int right = 0;      /* the right edge of the content */

    /* The least row height that holds the main line and the note line. */
    static int min_row_h(const lv_font_t *main, const lv_font_t *small);
    static ChainStyle make(int w, int row_h, int inset, const lv_font_t *main, const lv_font_t *small);
};

/* the node of a hop, drawn at (cx, cy) with radius r */
void draw_hop_node(lv_layer_t *layer, int cx, int cy, int r, HopLook look, bool destination);

class HopRowView : public RowView {
public:
    explicit HopRowView(const ChainStyle &style);
    ~HopRowView() override;
    /* the data of the hop; first / last: where the chain's line starts and ends */
    void set(const TraceHop &hop, double scale_ms, bool first, bool last);
    void set_expanded(bool expanded);
    bool expanded() const { return expanded_; }
    int height() const { return expanded_ ? style_.row_h + details_h_ : style_.row_h; }
    int visible_note_lines() const { return note_.empty() ? 0 : 1; }
    const HopTexts &texts() const { return texts_; }
    /* the objects (tests of the layout): the name label, the note label */
    lv_obj_t *name_label() const { return name_; }

    void attach(lv_obj_t *host, int w, int h) override;
    void detach() override;
    void resized(int w, int h) override;

private:
    void measure_details();
    void apply();
    void layout_details();
    void paint(lv_layer_t *layer, const lv_area_t &a);

    ChainStyle style_;
    HopTexts texts_;
    HopDetail detail_;
    std::string note_;
    double scale_ms_ = 1;
    bool first_ = true, last_ = true, expanded_ = false;
    int details_h_ = 0;
    int h_ = 0;
    int heights_[4] = {};
    lv_obj_t *host_ = nullptr;
    lv_obj_t *num_ = nullptr, *name_ = nullptr, *ip_ = nullptr, *avg_ = nullptr, *loss_ = nullptr, *bar_word_ = nullptr,
             *note_label_ = nullptr;
    lv_obj_t *detail_label_[4] = {};
    std::unique_ptr<Painter> painter_;
};

class BadgeBlock : public RowView {
public:
    BadgeBlock(int inset, const lv_font_t *caption, const lv_font_t *big);
    ~BadgeBlock() override;
    void set(const PortSummary &summary);
    int height() const { return height_; }
    /* does the badge's detail fit its two lines? (the scenes check it) */
    bool detail_fits() const;
    void attach(lv_obj_t *host, int w, int h) override;
    void detach() override;

private:
    int inset_ = 0, pad_ = 0, height_ = 0, w_ = 0;
    const lv_font_t *caption_ = nullptr, *big_ = nullptr;
    PortSummary shown_;
    std::unique_ptr<ResultBadge> badge_;
};

class WaterfallBlock : public RowView {
public:
    /* the block for these timings in a row w wide */
    WaterfallBlock(const HttpTimings &timings, int w, int inset, const lv_font_t *font, const lv_font_t *bold);
    ~WaterfallBlock() override;
    int height() const { return height_; }
    const std::vector<WaterSeg> &segments() const { return segs_; }
    void attach(lv_obj_t *host, int w, int h) override;
    void detach() override;

    static std::uint32_t part_rgb(WaterPart part);

private:
    struct Legend {
        std::string text;
        int x = 0, y = 0;
        WaterPart part = WaterPart::Dns;
    };
    HttpTimings t_;
    std::vector<WaterSeg> segs_;
    std::vector<Legend> legend_;
    const lv_font_t *font_ = nullptr, *bold_ = nullptr;
    int inset_ = 0, pad_ = 0, gap_ = 0, lh_ = 0, bar_h_ = 0, bar_y_ = 0, legend_y_ = 0, sw_ = 0, height_ = 0, bar_w_ = 0;
    std::string total_;
    std::unique_ptr<Painter> bar_, swatches_;
};

class CertBlock : public RowView {
public:
    CertBlock(const CertBadge &badge, int inset, const lv_font_t *caption, const lv_font_t *big);
    ~CertBlock() override;
    int height() const { return height_; }
    void attach(lv_obj_t *host, int w, int h) override;
    void detach() override;

private:
    CertBadge badge_;
    const lv_font_t *caption_ = nullptr, *big_ = nullptr;
    int inset_ = 0, pad_ = 0, gap_ = 0, height_ = 0, mark_d_ = 0;
    std::unique_ptr<Painter> mark_;
};

} // namespace ui
} // namespace nt
