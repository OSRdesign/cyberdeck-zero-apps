/*
 * SPDX-License-Identifier: MIT
 *
 * Net Toolbox 0.3: the blocks of the speed test, Wake-on-LAN and radar pages (LVGL thread only). Shapes are drawn on the
 * object's own paint event (charts.hpp's Painter), texts are ordinary labels that are made once; nothing here allocates
 * while it animates. The numbers and words come from view_model_03.hpp.
 *
 *   SpeedLiveBlock     the gauge (a 220 degree dial with ticks and numbers, an arc filled up to the value, a needle that eases
 *                      to the value in about 180 ms, the big value, its unit and the phase), the throughput chart of the
 *                      phase in progress on the dial's scale, the thin progress bar with its seconds. The dial's scale steps
 *                      up (10 .. 1000) and never comes back down during a run; the numbers cross-fade when it steps.
 *   SpeedResultsBlock  Download and Upload as two big values with the unit
 *   GradeBlock         the bufferbloat grade: a badge (a shape, the letter) and one plain-words line
 *   WakeBlock          the wake watch: a ring with its shape and word (Sent, Waking... N s, Awake after N s, No answer yet),
 *                      the detail line and the strip of one-second cells
 *   MachineRowView     a saved machine in the Wake-on-LAN list: name, MAC (and host), the last wake status
 *   RadarView          the Bluetooth radar: three rings (near, medium, far), the crosshair, the sweep and its trailing wedge
 *                      while scanning, the devices as blips (a stable angle, the ring from the signal class, brightness that
 *                      pulses when the sweep passes and fades with the time since the device was heard), the selected one
 *                      ringed and named
 */
#pragma once

#include "charts.hpp"
#include "rich_list.hpp"
#include "view_model_03.hpp"

#include <memory>
#include <string>
#include <vector>

namespace nt {
namespace ui {

class SpeedLiveBlock : public RowView {
public:
    /* a block w x h (the list's width and the height it keeps); inset: the list's side inset */
    SpeedLiveBlock(int w, int h, int inset, const lv_font_t *caption, const lv_font_t *small, const lv_font_t *big);
    ~SpeedLiveBlock() override;
    int height() const { return h_; }
    /* the snapshot of the runner; target_s: the seconds each direction lasts */
    void update(const SpeedSnapshot &s, double target_s);
    /* a new run: the scale goes back to 10 and the needle to 0 (update() does it by itself when a run starts) */
    void reset();
    const SpeedLayout &layout() const { return layout_; }
    double scale() const { return scaler_.scale; }
    /* the needle's value as a fraction of the scale, as drawn now (tests, scenes) */
    double shown_fraction() const { return shown_; }
    /* "" when the numbers of the dial and its texts have room, else what overlaps (the scenes check it) */
    std::string layout_problem() const;

    void attach(lv_obj_t *host, int w, int h) override;
    void detach() override;

private:
    static void anim_cb(lv_timer_t *timer);
    void step();
    void start_anim();
    void apply_texts();
    void place_numbers();
    void paint_gauge(lv_layer_t *layer, const lv_area_t &a);
    void paint_chart(lv_layer_t *layer, const lv_area_t &a);
    void paint_progress(lv_layer_t *layer, const lv_area_t &a);

    int w_, h_, inset_;
    const lv_font_t *caption_, *small_, *big_;
    SpeedLayout layout_;
    GaugeRadii radii_;
    GaugeScaler scaler_;
    SpeedSnapshot snap_;
    double target_s_ = 8;
    bool was_running_ = false;
    double shown_ = 0, from_ = 0, to_ = 0;       /* the needle, as a fraction of the scale */
    std::uint32_t anim_t0_ = 0;
    std::uint32_t fade_t0_ = 0;
    bool fading_ = false;
    double labels_scale_ = 10;                     /* the scale the numbers show */
    int fade_opa_ = 255;
    double progress_ = 0;
    std::string progress_text_;
    lv_timer_t *timer_ = nullptr;
    lv_obj_t *host_ = nullptr;
    std::vector<lv_obj_t *> ticks_;                /* the six numbers */
    lv_obj_t *phase_ = nullptr, *value_ = nullptr, *unit_ = nullptr;
    lv_obj_t *chart_top_ = nullptr, *chart_empty_ = nullptr, *chart_zero_ = nullptr, *progress_label_ = nullptr;
    std::unique_ptr<Painter> gauge_, chart_, progress_bar_;
    int num_w_[6] = {};
    int num_h_ = 0;
    int phase_y_ = 0, value_y_ = 0, unit_y_ = 0, centre_w_ = 0;
};

class SpeedResultsBlock : public RowView {
public:
    SpeedResultsBlock(int w, int inset, const lv_font_t *caption, const lv_font_t *big);
    ~SpeedResultsBlock() override;
    int height() const { return h_; }
    void set(const std::string &download, const std::string &upload);
    void attach(lv_obj_t *host, int w, int h) override;
    void detach() override;

private:
    int w_, inset_, h_ = 0, pad_ = 0;
    const lv_font_t *caption_, *big_;
    std::string down_ = "--", up_ = "--";
    lv_obj_t *down_label_ = nullptr, *up_label_ = nullptr;
};

class GradeBlock : public RowView {
public:
    GradeBlock(int w, int inset, const lv_font_t *caption, const lv_font_t *big);
    ~GradeBlock() override;
    int height() const { return h_; }
    void set(const SpeedGrade &grade);
    /* does the longest line fit its block? (the scenes check it) */
    bool fits() const { return fits_; }
    void attach(lv_obj_t *host, int w, int h) override;
    void detach() override;

private:
    int w_, inset_, h_ = 0, pad_ = 0, badge_ = 0, text_x_ = 0, text_w_ = 0, line_h_ = 0;
    bool fits_ = true;
    const lv_font_t *caption_, *big_;
    SpeedGrade shown_;
    lv_obj_t *letter_ = nullptr, *line_ = nullptr, *head_ = nullptr;
    std::unique_ptr<Painter> shape_;
};

class WakeBlock : public RowView {
public:
    WakeBlock(int w, int inset, const lv_font_t *caption, const lv_font_t *big);
    ~WakeBlock() override;
    int height() const { return h_; }
    void set(const WolSnapshot &snapshot, bool has_host);
    /* the spinner turns: call every 100 ms */
    void tick(std::int64_t now_ms);
    const WakeStatus &status() const { return status_; }
    bool detail_fits() const;
    void attach(lv_obj_t *host, int w, int h) override;
    void detach() override;

private:
    void paint_ring(lv_layer_t *layer, const lv_area_t &a);
    void paint_strip(lv_layer_t *layer, const lv_area_t &a);
    int w_, inset_, h_ = 0, pad_ = 0, ring_ = 0, strip_h_ = 0, text_x_ = 0, text_w_ = 0, strip_y_ = 0, lh_ = 0, big_h_ = 0;
    const lv_font_t *caption_, *big_;
    WakeStatus status_ = wake_status(WolSnapshot());
    WolSnapshot snap_;
    std::vector<StripCell> cells_;
    bool has_host_ = false;
    std::int64_t now_ = 0;
    lv_obj_t *word_ = nullptr, *detail_ = nullptr, *from_ = nullptr, *to_ = nullptr;
    std::unique_ptr<Painter> ring_painter_, strip_painter_;
};

class MachineRowView : public RowView {
public:
    MachineRowView(int w, int inset, const lv_font_t *main, const lv_font_t *small, int min_h);
    ~MachineRowView() override;
    int height() const { return h_; }
    void set(const std::string &name, const std::string &second, const std::string &status, Tone tone);
    void attach(lv_obj_t *host, int w, int h) override;
    void detach() override;

private:
    void apply();
    int w_, inset_, h_ = 0;
    const lv_font_t *main_, *small_;
    std::string name_, second_, status_;
    Tone tone_ = Tone::Normal;
    lv_obj_t *name_label_ = nullptr, *second_label_ = nullptr, *status_label_ = nullptr;
};

class RadarView : public Owned {
public:
    /* the radar in the box x, y, w, h of parent (the circle is as large as fits, centred in it) */
    RadarView(lv_obj_t *parent, const Box &box, const lv_font_t *font);
    ~RadarView();
    /* the devices to draw (the snapshot's), the time they are counted from, and whether the sweep runs; the selected device
     * keeps its address across updates */
    void set_devices(const std::vector<BleDevice> &devices, std::int64_t now_ms, bool scanning, int expiry_ms);
    void set_selected(const std::string &address);
    /* every 100 ms: the sweep moves (only while scanning); ms: the monotonic clock of the devices' times */
    void tick(std::int64_t ms);
    int radius() const { return r_; }
    int diameter() const { return 2 * r_; }
    int blip_count() const { return static_cast<int>(blips_.size()); }
    /* every blip and the selected name inside the box? (the scenes check it) */
    bool inside_box() const;

private:
    struct Blip {
        std::string address;
        std::string name;
        double angle = 0;     /* degrees clockwise from straight up */
        double frac = 0;      /* distance from the centre / radius */
        std::int64_t last_seen_ms = 0;
    };
    void paint(lv_layer_t *layer, const lv_area_t &a);
    void place_name();
    int w_, h_, cx_, cy_, r_;
    const lv_font_t *font_;
    std::vector<Blip> blips_;
    std::string selected_;
    bool scanning_ = false;
    std::int64_t now_ = 0;
    int expiry_ms_ = 20000;
    double sweep_ = -1;
    std::unique_ptr<Painter> painter_;
    lv_obj_t *ring_labels_[3] = {};
    lv_obj_t *name_ = nullptr;
};

} // namespace ui
} // namespace nt
