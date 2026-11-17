/*
 * SPDX-License-Identifier: MIT
 *
 * VHS UI: the Scenes page (canvas Scenes-480x320, Scenes-320x480; brief 9.1). In the dark panel under the toolkit's
 * header: "SCENES, 14", the All / Movies / Photos chips, the rows (thumbnail, name with its "locked" mark, meta, date in
 * landscape, chevron) newest first, built only for the rows in view (a folder can hold thousands); the selected row has
 * the canvas's lighter band. A tap selects a row, a tap on the selected row opens it (the toolkit's list rule); a press
 * held 1.5 s opens the options sheet (scene_sheet.hpp), and the tap of that release is dropped. The empty folder shows
 * one grey line (provisional text). The toast line sits at the panel's bottom (provisional place). Geometry from
 * core/layout/scenes_layout.hpp. LVGL thread only.
 */
#pragma once

#include "core/app/controller.hpp"
#include "core/layout/scenes_layout.hpp"
#include "notices.hpp"

#include "lvgl/lvgl.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace vhs {
namespace ui {

constexpr const char *kScenesEmpty = "No scenes yet. Record with R or take a photo with Space."; /* provisional */

struct ScenesActions {
    std::function<void(int index)> open;     /* a tap on the selected row (Enter: the app) */
    std::function<void(int index)> hold;     /* held 1.5 s: the options sheet */
    std::function<void(SceneFilter)> filter; /* a chip */
};

class ScenesPage {
public:
    ScenesPage(lv_obj_t *root, const ScenesLayout &geo, ScenesActions act);
    ~ScenesPage();
    ScenesPage(const ScenesPage &) = delete;
    ScenesPage &operator=(const ScenesPage &) = delete;

    void show(bool on);
    bool shown() const { return shown_; }
    /* The model (on the status timer); total: the folder's files for the head line. */
    void update(const ScenesView &v, const Notice &notice);
    /* The frame timer: the hold's progress. */
    void tick(std::int64_t now_ms);
    void select(int index);
    int selected() const { return selected_; }
    void move(int delta);
    void cancel_hold();

private:
    struct RowW {
        lv_obj_t *row = nullptr, *thumb_box = nullptr, *img = nullptr, *name = nullptr, *lock = nullptr, *meta = nullptr,
                 *date = nullptr, *chev = nullptr;
        lv_image_dsc_t dsc{};
        int index = -1;
        std::uint64_t filled_gen = 0;
        const Picture *pic = nullptr;
    };
    RowW make_row();
    void fill(RowW &w, int index);
    void layout_rows(bool force);
    void scroll_into_view(int index);
    static void scroll_cb(lv_event_t *e);
    static void row_cb(lv_event_t *e);
    static void chip_cb(lv_event_t *e);

    lv_obj_t *root_ = nullptr;
    ScenesLayout geo_;
    ScenesActions act_;
    lv_obj_t *panel_ = nullptr, *head_ = nullptr, *list_ = nullptr, *spacer_ = nullptr, *empty_ = nullptr;
    lv_obj_t *chip_[3] = {nullptr, nullptr, nullptr}, *chip_text_[3] = {nullptr, nullptr, nullptr};
    lv_obj_t *chip_hit_[3] = {nullptr, nullptr, nullptr};
    lv_obj_t *hint_l_ = nullptr, *hint_r_ = nullptr; /* the Square class's hint line */
    std::unique_ptr<Toast> toast_;
    std::vector<RowW> pool_;
    std::vector<SceneRow> rows_;
    std::uint64_t gen_ = ~0ull;
    int selected_ = 0;
    int filter_ = -1;
    bool shown_ = false;
    lv_point_precise_t chev_pts_[3] = {};
    /* the hold (brief 9.2: 1.5 s; a move beyond 16 px or a release gives it up) */
    int hold_index_ = -1;
    std::int64_t hold_t0_ = 0;
    lv_point_t hold_at_{};
    bool hold_fired_ = false;
};

} // namespace ui
} // namespace vhs
