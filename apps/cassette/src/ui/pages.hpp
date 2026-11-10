/*
 * SPDX-License-Identifier: MIT
 *
 * Cassette UI: the pages under the toolkit's header (CassetteUi, cassette_ui.hpp, owns them and switches them).
 * Each page is a full-screen transparent container on the toolkit Page's root, built once in board coordinates through
 * its own Fit (its seams share the height a larger screen leaves), and updated from its part of the view model; a
 * widget touches LVGL only when its value changed.
 *
 *   RecorderPage  the cassette window, the meter panel (counter, PEAK, the Recordings and Settings buttons, the LED
 *                 bar, the TARGET bracket and the scale), the info strip with its lamps (landscape) or the info box
 *                 (portrait), the CARD and SOFT steppers, the five keys
 *   SettingsPage  the setting rows in a scrolling list with the boards' scroll bar; a choice row opens its picker in
 *                 the same list (the chosen entry in the selected-row look)
 *   ListPage      the strip, the take rows (label sticker, name, details, duration, chevron; the selected row
 *                 darker), PLAY, RENAME, TRIM, DELETE, the hint
 *   TrimPage      the strip, the waveform window (overview, main bars, handles, flags, marker, ruler), ZOOM and
 *                 POSITION, SET IN, SET OUT, SPLIT / REMOVE, PLAY, SAVE
 *   SaveBox, ConfirmBox  the dim overlay over the whole screen and the cream box
 */
#pragma once

#include "cassette_window.hpp"
#include "fit.hpp"
#include "meter.hpp"
#include "sq_geom.hpp"
#include "view_model.hpp"
#include "widgets.hpp"
#include "wave_geom.hpp"

#include "cp0_ui_roller.h"
#include "lvgl/lvgl.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace cassette {
namespace ui {

/* What a page asks of the root (navigation and the text boxes it owns). */
struct Nav {
    std::function<void(Screen)> go;
    std::function<void(int index)> rename;                                 /* the list's RENAME */
    std::function<void(const std::string &id, const std::string &label, const std::string &value)> edit_text;
    std::function<void(const std::string &title)> set_title;               /* the settings picker's title */
};

/* The geometry every page shares: the screen, the header's bottom, portrait or not, the narrow shell. */
struct Frame {
    int w = 480, h = 320, body_y = 40;
    bool portrait = false;
    /* a (nearly) square screen (width / height 0.9 .. 1.1, fit.hpp square_screen): the recorder gets its own single-column
     * layout; the other pages keep the landscape board scaled (portrait is false then) */
    bool square = false;
    /* the toolkit's touch sizes for this panel (cp0_ui_metrics: TARGET and ROW 7 mm, LIST_ROW 5 mm, with floors):
     * the minimum height of the keys, steppers and buttons, of a row that acts on a tap, of a row a tap selects */
    int target = 44, row = 40, list_row = 24;
    Fit fit(std::vector<double> seams_landscape, std::vector<double> seams_portrait, std::vector<Band> bands_landscape = {},
            std::vector<Band> bands_portrait = {}) const;
    /* the scale a page will get (for computing band minimums) */
    double scale() const;
};

class PageBase {
public:
    PageBase(lv_obj_t *parent, const Frame &frame);
    virtual ~PageBase() = default;
    void set_shown(bool shown);
    bool shown() const { return shown_; }
    lv_obj_t *root() const { return root_; }

protected:
    lv_obj_t *root_ = nullptr;
    Frame frame_;
    bool shown_ = true;
};

class RecorderPage : public PageBase {
public:
    RecorderPage(lv_obj_t *parent, const Frame &frame, const UiActions &actions, const Nav &nav, AnimOptions anim);
    /* `notice`: the timed notice (ViewModel::notice); it replaces the info strip (landscape) or covers the counter row
     * (portrait: the counter, the two buttons and the info box) while it shows. */
    /* `rec` must stay alive until the next update (the load animation's landing updates the strip from it). */
    void update(const RecorderVm &rec, const std::string &notice = std::string());
    void set_shown_page(bool shown);
    /* A deck key held on the keyboard: drawn pressed until released. */
    void press_key(int key, bool down);
    const CassetteWindow &window() const { return *window_; }
    CassetteWindow &window() { return *window_; }

private:
    void update_deck(const RecorderVm &rec);
    void build_landscape();
    void build_portrait();
    void build_square();
    void square_stereo(bool stereo);
    void place_strip_right(const std::string &state);
    void place_steppers(bool card);

    const UiActions &act_;
    Nav nav_;
    Fit fit_;
    std::unique_ptr<CassetteWindow> window_;
    std::unique_ptr<LedBar> bar_;
    std::unique_ptr<Counter> counter_;
    std::unique_ptr<IconButton> list_btn_, gear_btn_;
    std::unique_ptr<Stepper> card_, soft_;
    std::unique_ptr<PianoKey> keys_[5];
    std::unique_ptr<Lamp> lim_, cut_, state_lamp_;
    lv_obj_t *peak_ = nullptr;
    bool peak_lit_ = false;
    lv_obj_t *mon_lamp_ = nullptr, *mon_label_ = nullptr; /* the MON lamp (live monitor running), bottom left of the meter */
    bool mon_shown_ = false, mon_lit_ = false;
    lv_obj_t *in_label_ = nullptr, *ch_l_ = nullptr, *ch_r_ = nullptr; /* IN (mono), L / R (stereo) */
    lv_obj_t *strip_text_ = nullptr, *strip_lim_ = nullptr, *strip_cut_ = nullptr, *strip_state_ = nullptr;
    lv_obj_t *box_title_ = nullptr, *box_detail_ = nullptr;
    std::string state_shown_ = "?";
    int strip_x_ = 0, strip_w_ = 0, strip_y_ = 0, strip_h_ = 0;
    bool card_shown_ = true;
    bool limiter_ = false, low_cut_ = false;
    double btn_h_ = 44; /* the Recordings / Settings buttons' height (landscape) */
    bool first_ = true;
    /* the info box (title, detail, state lamp: portrait and square) instead of the strip with its lamps (landscape) */
    bool info_box_ = false;
    double title_px_ = 15, title_min_ = 11, title_ls_ = 0.5, detail_px_ = 13, detail_min_ = 10, mon_glow_ = 4;
    /* the square layout: the meter panel is as tall as two channels need; one channel moves its contents (square_stereo) */
    bool square_ = false, sq_stereo_ = false;
    lv_obj_t *sq_panel_ = nullptr;
    SqGeo g_;
    struct Moved {
        lv_obj_t *obj;
        int y;
        double mono_dy; /* design px it moves with one channel */
    };
    std::vector<Moved> sq_moved_;
    std::unique_ptr<NoticeBox> notice_;
    std::vector<lv_obj_t *> notice_covers_; /* hidden while the notice shows (the parts under it) */
    bool notice_on_ = false;
    const RecorderVm *last_rec_ = nullptr; /* the model of the last update (the landing of a load updates from it) */
};

class SettingsPage : public PageBase {
public:
    SettingsPage(lv_obj_t *parent, const Frame &frame, const UiActions &actions, const Nav &nav);
    void update(const SettingsVm &s);
    /* Keys: Up / Down move the highlight, Enter acts, Left / Right change a slider. True when used. */
    bool on_key(std::uint32_t lv_key);
    bool on_left_right(int dir);
    /* Right: steps a slider row up, else changes / opens the highlighted row (as Enter). */
    bool on_right();
    /* Esc: closes the picker (or the Take number page, nothing applied) first; false when there was none. */
    bool back();
    void reset_view();
    /* The notice replaces the 'more below' line (landscape); narrow: a strip at the bottom of the list area. */
    void set_notice(const std::string &text);

private:
    struct Row {
        lv_obj_t *obj = nullptr, *label = nullptr, *value = nullptr, *chevron = nullptr, *toggle = nullptr,
                 *slider = nullptr;
        int index = -1;           /* the setting (rows) or the choice (picker) */
    };
    static void row_cb(lv_event_t *e);
    static void scroll_cb(lv_event_t *e);
    void refresh();
    void rebuild();
    void fill_row(Row &r, const SettingRow &s);
    void activate(int i, int touch_x);
    void update_scrollbar();
    void update_hint();
    void highlight(int i);
    int row_h() const;
    /* The Take number page (a sub-page of the same list, like a picker): the draft number large, Reset / Set, and the
     * -1 / +1, -10 / +10, -100 / +100 steppers. Esc or back() drops the draft. */
    struct NumBtn {
        lv_obj_t *obj = nullptr, *label = nullptr;
        int code = 0;             /* NumCode */
        int line = 0;             /* 0 Reset / Set, 1 +-1, 2 +-10, 3 +-100 (the keyboard highlight row) */
    };
    void build_number_page();
    void number_refresh();
    void number_act(int code);
    bool number_key(std::uint32_t k);
    static void num_cb(lv_event_t *e);

    const UiActions &act_;
    Nav nav_;
    Fit fit_;
    std::unique_ptr<NoticeBox> notice_;
    int notice_h_ = 0;
    bool numpage_ = false;        /* picker_ is then the Number row */
    int draft_ = 1;
    lv_obj_t *num_big_ = nullptr;
    std::vector<NumBtn> nbtn_;
    SettingsVm vm_;
    bool have_ = false;
    lv_obj_t *list_ = nullptr, *track_ = nullptr, *thumb_ = nullptr, *hint_ = nullptr;
    std::vector<Row> rows_;
    int picker_ = -1;             /* the setting whose choices show, -1: the settings */
    int sel_ = -1;                /* the keyboard highlight, -1: none (touch) */
    std::string layout_key_;
    int list_x_ = 0, list_y_ = 0, list_w_ = 0, list_h_ = 0, row_w_ = 0;
    /* a press on a slider row: drag sideways to set */
    int drag_row_ = -1;
};

class ListPage : public PageBase {
public:
    ListPage(lv_obj_t *parent, const Frame &frame, const UiActions &actions, const Nav &nav);
    void update(const ListVm &l);
    bool on_key(std::uint32_t lv_key);
    int selected() const { return vm_.selected; }
    /* The notice replaces the hint line (landscape) or the top strip (narrow; the list moves down for a taller one). */
    void set_notice(const std::string &text);
    /* The four keys on the selected take: 0 LOAD, 1 RENAME, 2 TRIM, 3 DELETE. LOAD does nothing while the model says
     * can_load = false (recording); otherwise it loads the take and shows the recorder. */
    void key(int which);
    /* Enter or a second tap on the selected row: PREVIEW the take (a second one on the previewing row stops it). */
    void activate();

private:
    struct Row {
        lv_obj_t *obj = nullptr, *sticker = nullptr, *name = nullptr, *detail = nullptr, *dur = nullptr,
                 *chevron = nullptr;
    };
    static void row_cb(lv_event_t *e);
    static void scroll_cb(lv_event_t *e);
    void rebuild();
    void style_row(int i);
    void select(int i, bool from_touch);
    void update_scrollbar();

    const UiActions &act_;
    Nav nav_;
    Fit fit_;
    ListVm vm_;
    bool have_ = false;
    lv_obj_t *strip_left_ = nullptr, *strip_right_ = nullptr, *list_ = nullptr, *track_ = nullptr, *thumb_ = nullptr,
             *hint_ = nullptr;
    std::vector<Row> rows_;
    std::unique_ptr<PianoKey> keys_[4];
    int row_h_ = 42, row_gap_ = 4;
    std::string rows_key_;
    std::unique_ptr<NoticeBox> notice_;
    int list_y0_ = 0, list_h0_ = 0, track_y0_ = 0, track_h0_ = 0, shift_ = 0;
};

class TrimPage : public PageBase {
public:
    TrimPage(lv_obj_t *parent, const Frame &frame, const UiActions &actions, const Nav &nav);
    void update(const TrimVm &t);
    /* Left / Right: one POSITION step (100 ms, Shift 1 s). */
    void nudge(int dir, bool shift);
    /* The keyboard's commands (the keys' and steppers' own actions; NextFlag selects the flag after the selected one). */
    enum Command { SetIn, SetOut, Split, Play, Save, ZoomIn, ZoomOut, NextFlag };
    void press(Command c);
    /* The notice replaces the top strip (take label, IN / OUT / LEN). */
    void set_notice(const std::string &text);

private:
    static void wave_draw_cb(lv_event_t *e);
    static void wave_event_cb(lv_event_t *e);
    void draw_wave(lv_layer_t *layer);

    const UiActions &act_;
    Nav nav_;
    Fit fit_;
    TrimVm vm_;
    bool have_ = false;
    std::vector<float> main_bars_, over_bars_;
    lv_obj_t *sticker_ = nullptr, *take_ = nullptr, *info_ = nullptr, *wave_ = nullptr;
    lv_obj_t *bottom_left_ = nullptr, *bottom_right_ = nullptr;
    std::unique_ptr<Stepper> zoom_, pos_;
    std::unique_ptr<PianoKey> keys_[5];
    std::unique_ptr<NoticeBox> notice_;
    /* the waveform's parts in screen px (relative to the screen) */
    int over_x_ = 0, over_y_ = 0, over_w_ = 0, over_h_ = 0;
    int main_x_ = 0, main_y_ = 0, main_w_ = 0, main_h_ = 0;
    int ruler_y_ = 0;
    double pitch_ = 4, bar_w_ = 3;
    /* the drag in progress */
    bool dragging_ = false;
    lv_point_t press_{};
    std::int64_t press_marker_ = 0;
};

class SaveBox {
public:
    SaveBox(lv_obj_t *parent, const Frame &frame, const UiActions &actions);
    void update(const SaveVm &s, const TrimVm &t);
    bool shown() const { return shown_; }
    lv_obj_t *root() const { return dim_; }
    /* Keys: Left / Right / Tab move the focus from the default (outlined from the first move), Enter chooses (SAVE AS
     * NEW FILES when nothing was moved). */
    void key_move(int dir);
    void key_choose();

private:
    static void button_cb(lv_event_t *e);
    lv_obj_t *dim_ = nullptr, *title_ = nullptr, *summary_ = nullptr, *files_ = nullptr;
    std::vector<lv_obj_t *> names_, lengths_, buttons_;
    int focus_ = 0;
    bool focus_shown_ = false;
    const UiActions &act_;
    Fit fit_;
    bool narrow_ = false, shown_ = false;
    std::string key_;
};

class ConfirmBox {
public:
    ConfirmBox(lv_obj_t *parent, const Frame &frame, const UiActions &actions);
    void update(const ConfirmVm &c);
    bool shown() const { return shown_; }
    lv_obj_t *root() const { return dim_; }
    /* Keys as the save box; Enter alone answers CANCEL (the safe default). */
    void key_move(int dir);
    void key_choose();

private:
    static void button_cb(lv_event_t *e);
    lv_obj_t *dim_ = nullptr, *title_ = nullptr, *body_ = nullptr, *yes_label_ = nullptr;
    std::vector<lv_obj_t *> buttons_;
    int focus_ = 1;
    bool focus_shown_ = false;
    const UiActions &act_;
    Fit fit_;
    bool shown_ = false;
};

} // namespace ui
} // namespace cassette
