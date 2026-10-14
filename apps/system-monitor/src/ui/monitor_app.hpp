/*
 * SPDX-License-Identifier: MIT
 *
 * System Monitor: the app's screen (LVGL thread only). One cp0::ui::Page (title, the shared status strip, the
 * on-screen Esc) and, per page, an optional panel at the top of the body (panels.hpp) with a cp0::ui::CompactList
 * (compact density: top-aligned rows, no arrows, a thin scrollbar while scrolling) filling the room under it, all laid
 * out by cp0_lvgl's layout service:
 *
 *   Overview   the tile grid (overview.hpp): Power, CPU, Temperature, Memory, Storage, Network and a footer
 *              (Processes, System, Sampling); a tile or an item opens its page
 *   CPU, Memory
 *              the history chart, then the rows, the section "Top processes by ..." and "All processes" (selectable)
 *   Temperature and power, Storage, Network
 *              the history chart, then the rows (read-only: Up / Down scroll)
 *   Processes  the status line (the highlighted process), then "Sort by" and the top 15
 *   System     the rows (read-only)
 *   Sampling   a cp0::ui::Roller of Choice rows 1 / 2 / 5 s (saved)
 *
 * The values come from the Sampler's thread (core/sampler.hpp): a timer looks at its sequence number five times a
 * second and refreshes the page when a new sample is there: CompactList::set_rows() touches only the texts and
 * colours that changed and keeps the scroll offset and the highlight (no flicker, no jump). Nothing waits on the
 * LVGL thread. Esc / Left / the header's Esc go one level up; at the overview they end the app (cdk::request_quit).
 */
#pragma once

#include "monitor_core.hpp"
#include "overview.hpp"
#include "panels.hpp"
#include "sampler.hpp"
#include "settings.hpp"

#include "cp0_ui_list.h"
#include "cp0_ui_roller.h"
#include "cp0_ui_widgets.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace sm {

class MonitorApp {
public:
    explicit MonitorApp(lv_obj_t *screen);
    ~MonitorApp();
    MonitorApp(const MonitorApp &) = delete;
    MonitorApp &operator=(const MonitorApp &) = delete;

private:
    static void posted_cb(void *data);
    static void tick_cb(lv_timer_t *timer);
    void post(std::function<void()> fn);
    void tick();
    void focus(lv_obj_t *obj);
    void back();
    void open(View view, int from_row);
    void build_view(int select);
    void refresh();
    void refresh_panel();
    void update_status();
    void activate(int row);
    void open_item(int index);
    std::vector<Row> rows_now() const;
    std::vector<cp0::ui::ListRow> list_rows() const;
    std::vector<std::pair<std::string, std::vector<std::string>>> about_extra() const;

    cp0_ui_roller_page_t layout_{};
    Nav nav_;
    std::unique_ptr<Sampler> sampler_;
    std::string config_path_;
    Settings settings_;

    std::unique_ptr<cp0::ui::Page> page_;
    std::unique_ptr<cp0::ui::CompactList> list_; /* every detail page */
    std::unique_ptr<cp0::ui::Roller> roller_;    /* Sampling */
    std::unique_ptr<OverviewGrid> grid_;
    std::unique_ptr<HistoryPanel> chart_;
    std::unique_ptr<StatusLine> status_;         /* Processes */
    lv_timer_t *timer_ = nullptr;
    std::vector<std::function<void()>> posted_;
    bool post_armed_ = false;

    std::shared_ptr<const Snapshot> snap_;
    std::uint64_t shown_seq_ = 0;
    std::vector<Row> rows_;
    std::vector<ListItem> items_;                /* rows_ as the list shows them (the fitted values) */
    SortBy sort_ = SortBy::Cpu;
    cp0_ui_list_layout_t list_layout_{};         /* the compact list's text sizes, insets (any area: same tokens) */
    const lv_font_t *label_font_ = nullptr;      /* the list's selected label (bold) and value fonts: what fits */
    const lv_font_t *value_font_ = nullptr;
};

} // namespace sm
