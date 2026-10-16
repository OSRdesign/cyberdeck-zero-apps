/*
 * SPDX-License-Identifier: MIT
 *
 * Net Toolbox 0.4: the blocks of the Local devices, Wi-Fi networks and Wi-Fi walk test pages (LVGL thread only). Shapes are
 * drawn on the object's own paint event (charts.hpp's Painter), texts are ordinary labels that are made once. The numbers and
 * words come from view_model_04.hpp.
 *
 *   DeviceRowView   a device of the Local devices list, two lines: the name (or the address) with its tag (a shape and a word:
 *                   a diamond "router", a disc "this device") and the address at the right; the vendor, the MAC and the
 *                   round trip
 *   WifiRowView     a network of the Wi-Fi list, two lines: the name, a check and "connected" for the one in use, the signal
 *                   bars (1 to 4) and the dBm at the right; "ch 36, 5 GHz" and the security words (amber for Open and WEP)
 *   ChannelsBlock   the Channels view: the bars of 2.4 GHz and of 5 GHz with the count of networks over each bar, the channel
 *                   numbers under it, the busiest named (the sentence and the notes are rows of the list)
 *   WalkLiveBlock   the walk test: the big reading (dBm, the word with its shape, the weak zone), the line chart of the last
 *                   2.5 minutes (the weak threshold dashed, weak readings as squares in amber, access point changes as ticks
 *                   with a diamond) and its legend
 */
#pragma once

#include "charts.hpp"
#include "rich_list.hpp"
#include "view_model_04.hpp"

#include <memory>
#include <string>
#include <vector>

namespace nt {
namespace ui {

class DeviceRowView : public RowView {
public:
    DeviceRowView(int w, int inset, const lv_font_t *main, const lv_font_t *small, int min_h);
    ~DeviceRowView() override;
    int height() const { return h_; }
    void set(const DeviceRowTexts &texts);
    void attach(lv_obj_t *host, int w, int h) override;
    void detach() override;

private:
    void apply();
    int pill_width() const;
    int w_, inset_, h_ = 0;
    const lv_font_t *main_, *small_;
    DeviceRowTexts t_;
    lv_obj_t *title_ = nullptr, *ip_ = nullptr, *detail_ = nullptr, *rtt_ = nullptr, *tag_ = nullptr;
    std::unique_ptr<Painter> pill_;
    int pill_h_ = 0;
};

class WifiRowView : public RowView {
public:
    WifiRowView(int w, int inset, const lv_font_t *main, const lv_font_t *small, int min_h);
    ~WifiRowView() override;
    int height() const { return h_; }
    void set(const WifiRowTexts &texts);
    void attach(lv_obj_t *host, int w, int h) override;
    void detach() override;

private:
    void apply();
    void paint(lv_layer_t *layer, const lv_area_t &a);
    int w_, inset_, h_ = 0;
    const lv_font_t *main_, *small_;
    WifiRowTexts t_;
    lv_obj_t *title_ = nullptr, *dbm_ = nullptr, *connected_ = nullptr, *caption_ = nullptr, *security_ = nullptr;
    std::unique_ptr<Painter> glyphs_;
    int bars_x_ = 0, bars_y_ = 0, bar_w_ = 0, bar_gap_ = 0, bar_h_ = 0;
    int check_x_ = 0, check_y_ = 0, check_r_ = 0;
};

class ChannelsBlock : public RowView {
public:
    ChannelsBlock(int w, int inset, const lv_font_t *caption, const lv_font_t *small);
    ~ChannelsBlock() override;
    int height() const { return layout_.total_h; }
    /* the views of the bands (2.4 GHz first, then 5 GHz) */
    void set(const std::vector<ChannelsView> &parts);
    const ChannelsLayout &layout() const { return layout_; }
    /* does the "nothing seen" message have room (two lines)? the scenes check it */
    bool fits() const { return fits_; }
    void attach(lv_obj_t *host, int w, int h) override;
    void detach() override;

private:
    struct Part {
        lv_obj_t *title = nullptr, *info = nullptr;
        std::vector<lv_obj_t *> counts, numbers;
        std::unique_ptr<Painter> bars;
    };
    void build();
    void place();
    void paint_bars(int part, lv_layer_t *layer, const lv_area_t &a);
    Box part_box(int i) const;
    int w_, inset_;
    const lv_font_t *caption_, *small_;
    std::vector<ChannelsView> parts_;
    ChannelsLayout layout_;
    int bars_h_ = 0, pad_ = 0;
    bool fits_ = true;
    lv_obj_t *host_ = nullptr;
    std::vector<Part> ui_;
};

class WalkLiveBlock : public RowView {
public:
    /* a block w x h (the list's width and the height it keeps); inset: the list's side inset */
    WalkLiveBlock(int w, int h, int inset, const lv_font_t *caption, const lv_font_t *small, const lv_font_t *big);
    ~WalkLiveBlock() override;
    int height() const { return h_; }
    /* a shorter block when the summary card follows */
    void set_height(int h);
    void update(const WalkSnapshot &s);
    const WalkLayout &layout() const { return layout_; }
    /* "" when the texts have room, else what does not fit (the scenes check it) */
    std::string layout_problem() const;
    void attach(lv_obj_t *host, int w, int h) override;
    void detach() override;
    void resized(int w, int h) override;

private:
    void relayout();
    void apply_texts();
    void paint_mark(lv_layer_t *layer, const lv_area_t &a);
    void paint_chart(lv_layer_t *layer, const lv_area_t &a);
    int w_, h_, inset_;
    const lv_font_t *caption_, *small_, *big_;
    WalkLayout layout_;
    WalkSnapshot snap_;
    WalkLive live_;
    lv_obj_t *host_ = nullptr, *big_label_ = nullptr, *unit_ = nullptr, *word_ = nullptr, *zone_ = nullptr, *where_ = nullptr,
             *elapsed_ = nullptr, *legend_ = nullptr, *y_top_ = nullptr, *y_weak_ = nullptr, *y_bottom_ = nullptr, *x_left_ = nullptr,
             *x_right_ = nullptr;
    std::unique_ptr<Painter> mark_, chart_;
    int mark_d_ = 0, text_x_ = 0, text_w_ = 0, ylab_w_ = 0;
    std::string problem_;
};

} // namespace ui
} // namespace nt
