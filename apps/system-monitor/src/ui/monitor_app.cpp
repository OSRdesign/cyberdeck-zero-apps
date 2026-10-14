/*
 * SPDX-License-Identifier: MIT
 *
 * System Monitor: the app's screen (see monitor_app.hpp). Layout from the layout service only (the roller page, the
 * list layout, the tokens and the text sizes); the texts and the logic are in core/.
 */
#include "monitor_app.hpp"

#include "app.hpp"
#include "build_info.hpp"

#include "cp0_app_contract.h"
#include "cp0_font_service.hpp"
#include "cp0_ui_metrics_lvgl.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>

namespace sm {
namespace {

constexpr int kTickMs = 200;

cp0::ui::ValueTone value_tone(Tone tone)
{
    switch (tone) {
    case Tone::Good: return cp0::ui::ValueTone::Ok;
    case Tone::Warn: return cp0::ui::ValueTone::Warn;
    case Tone::Error: return cp0::ui::ValueTone::Bad;
    default: return cp0::ui::ValueTone::Normal;
    }
}

} // namespace

MonitorApp::MonitorApp(lv_obj_t *screen)
{
    std::string fixture;
    std::unique_ptr<Source> source = source_from_env(&fixture);
    // settings: the user's file; in a fixture run only when SYSTEM_MONITOR_CONFIG names one (scenes write nothing)
    const char *over = std::getenv("SYSTEM_MONITOR_CONFIG");
    if (!source || (over && over[0])) config_path_ = settings_path();
    settings_ = load_settings(config_path_);
    sampler_ = std::make_unique<Sampler>(std::move(source), settings_.period_s);

    cp0_ui_roller_page(cp0_ui_metrics_get(), cp0::ui::status_strip_left(), &layout_);
    // the compact list's text sizes and insets (the same on every page): the fonts the values are fitted with
    cp0_ui_list_page_layout(cp0_ui_metrics_get(), &layout_, CP0_UI_LIST_COMPACT, &list_layout_);
    label_font_ = cp0_fonts().get("Montserrat-Bold.ttf", static_cast<uint16_t>(list_layout_.text_px));
    value_font_ = cp0_fonts().get("Montserrat-Medium.ttf", static_cast<uint16_t>(list_layout_.value_px));
    if (!label_font_) label_font_ = cp0_ui_font_px(list_layout_.text_px);
    if (!value_font_) value_font_ = cp0_ui_font_px(list_layout_.value_px);
    page_ = std::make_unique<cp0::ui::Page>(screen, layout_, cp0::ui::add_title, [this] {
        post([this] { back(); });
    });
    cp0::ui::StatusStrip::add(page_->header());

    std::fprintf(stderr, "[system-monitor] %s, every %d s, screen %dx%d\n", sampler_->describe().c_str(),
                 settings_.period_s, layout_.w, layout_.h);
    build_view(0);
    sampler_->start();
    timer_ = lv_timer_create(tick_cb, kTickMs, this);
}

MonitorApp::~MonitorApp()
{
    if (timer_) lv_timer_delete(timer_);
    lv_async_call_cancel(posted_cb, this);
    sampler_.reset(); /* the thread stopped and joined (a running vcgencmd ends within its 1 s timeout) */
    list_.reset();
    roller_.reset();
    grid_.reset();
    chart_.reset();
    status_.reset();
    page_.reset();
}

void MonitorApp::posted_cb(void *data)
{
    auto *self = static_cast<MonitorApp *>(data);
    self->post_armed_ = false;
    std::vector<std::function<void()>> fns;
    fns.swap(self->posted_);
    for (auto &fn : fns) fn();
}

/* List, roller and Esc callbacks run inside an LVGL event of the widget: what replaces the widget runs after it. */
void MonitorApp::post(std::function<void()> fn)
{
    posted_.push_back(std::move(fn));
    if (!post_armed_) post_armed_ = lv_async_call(posted_cb, this) == LV_RESULT_OK;
}

void MonitorApp::tick_cb(lv_timer_t *timer)
{
    auto *self = static_cast<MonitorApp *>(lv_timer_get_user_data(timer));
    if (self) self->tick();
}

void MonitorApp::tick()
{
    const std::uint64_t seq = sampler_->seq();
    if (seq == shown_seq_) return;
    shown_seq_ = seq;
    snap_ = sampler_->latest();
    refresh();
}

void MonitorApp::focus(lv_obj_t *obj)
{
    lv_group_t *group = cdk::input_group();
    if (!group || !obj) return;
    if (lv_obj_get_group(obj) != group) lv_group_add_obj(group, obj);
    lv_group_focus_obj(obj);
}

void MonitorApp::back()
{
    int from = 0;
    if (nav_.back(&from) == Back::Quit) {
        cdk::request_quit(0); /* a short Esc at the overview ends the app (report 024) */
        return;
    }
    build_view(from);
}

void MonitorApp::open(View view, int from_row)
{
    nav_.open(view, from_row);
    build_view(0);
}

/* The current page: the panel at the top of the body (the chart on the detail pages, the status line on Processes,
 * none on System), the list in the whole room under it, the rows of the last sample; Sampling: the roller. */
void MonitorApp::build_view(int select)
{
    const View view = nav_.current();
    sampler_->set_want_procs(view_wants_procs(view));
    sampler_->set_want_link(view_wants_link(view));
    list_.reset();
    roller_.reset();
    grid_.reset();
    chart_.reset();
    status_.reset();
    rows_.clear();
    items_.clear();
    page_->set_caption("", cp0::ui::CaptionTone::Normal);
    page_->set_title(view == View::Overview ? std::vector<std::string>{kAppTitle}
                                            : std::vector<std::string>{kAppTitle, view_title(view)});
    if (view == View::Overview) { /* the tile grid (overview.hpp) */
        OverviewGrid::Events events;
        events.open = [this](int index) { post([this, index] { open_item(index); }); };
        events.back = [this] { post([this] { back(); }); };
        grid_ = std::make_unique<OverviewGrid>(page_->root(), layout_, std::move(events), select);
        grid_->update(snap_.get(), sampler_->period());
        focus(grid_->focus_object());
        return;
    }
    if (view == View::Period) { /* a choice of three: the roller (the Settings look, a check on the current one) */
        cp0::ui::RollerEvents events;
        events.activate = [this](int row) { post([this, row] { activate(row); }); };
        events.back = [this] { post([this] { back(); }); };
        roller_ = std::make_unique<cp0::ui::Roller>(page_->root(), layout_, std::move(events));
        rows_ = rows_now();
        std::vector<cp0::ui::RowSpec> specs;
        for (const Row &r : rows_) {
            cp0::ui::RowSpec spec;
            spec.kind = cp0::ui::RowKind::Choice;
            spec.label = r.label;
            spec.on = r.on;
            specs.push_back(spec);
        }
        roller_->set_rows(specs, std::clamp(select, 0, std::max(0, static_cast<int>(specs.size()) - 1)), false);
        focus(roller_->focus_object());
        return;
    }

    // the panel on top: the chart (detail pages) or the status line (Processes)
    const int s = cp0_ui_tok(CP0_TOK_SPACE_S);
    Box box;
    box.x = layout_.roller_x;
    box.y = layout_.body_y + s;
    box.w = layout_.roller_w;
    switch (view) {
    case View::Cpu:
    case View::Memory:
    case View::Thermal:
    case View::Storage:
    case View::Network: {
        box.h = HistoryPanel::height_for(layout_);
        const int series = view == View::Cpu || view == View::Thermal ? 1 : 2;
        chart_ = std::make_unique<HistoryPanel>(page_->root(), box, series, view == View::Thermal ? 2 : 0, layout_);
        if (view == View::Thermal) {
            chart_->set_mark(0, static_cast<std::int32_t>(kWarmC * 10), kRgbWarn);
            chart_->set_mark(1, static_cast<std::int32_t>(kHotC * 10), kRgbError);
        }
        break;
    }
    case View::Processes:
        box.h = StatusLine::height();
        status_ = std::make_unique<StatusLine>(page_->root(), box);
        break;
    default: box.h = 0; break;
    }

    // the list: the room under the panel (the whole body without one), top-aligned
    cp0_ui_list_layout_t l{};
    if (box.h > 0) {
        const int y = box.y + box.h + s;
        cp0_ui_list_layout(cp0_ui_metrics_get(), CP0_UI_LIST_COMPACT, layout_.roller_x, y, layout_.roller_w,
                           layout_.body_y + layout_.body_h - y, &l);
    } else {
        cp0_ui_list_page_layout(cp0_ui_metrics_get(), &layout_, CP0_UI_LIST_COMPACT, &l);
    }
    cp0::ui::CompactListEvents events;
    events.activate = [this](int index, std::intptr_t) { post([this, index] { activate(index); }); };
    events.selected = [this](int) { update_status(); };
    events.back = [this] { post([this] { back(); }); };
    cp0::ui::CompactListOptions options;
    options.read_only = view_read_only(view);
    options.empty_text = view_empty_text(view);
    list_ = std::make_unique<cp0::ui::CompactList>(page_->root(), l, std::move(events), options);

    rows_ = rows_now();
    items_ = list_items(rows_, l.w - 2 * l.inset, l.gap, [this](const std::string &t, bool value) {
        return text_width(t.c_str(), value ? value_font_ : label_font_);
    });
    list_->reset_rows(list_rows(), select);
    refresh_panel();
    update_status();
    focus(list_->focus_object());
}

std::vector<Row> MonitorApp::rows_now() const
{
    const Snapshot *s = snap_.get();
    switch (nav_.current()) {
    case View::Overview: return {}; /* the tile grid draws itself */
    case View::Cpu: return cpu_rows(s);
    case View::Memory: return memory_rows(s);
    case View::Thermal: return thermal_rows(s);
    case View::Storage: return storage_rows(s);
    case View::Network: return network_rows(s);
    case View::Processes: return process_rows(s, sort_);
    case View::About: return about_rows(s, about_extra());
    case View::Period: return period_rows(sampler_->period());
    }
    return {};
}

std::vector<cp0::ui::ListRow> MonitorApp::list_rows() const
{
    std::vector<cp0::ui::ListRow> rows;
    rows.reserve(items_.size());
    for (const ListItem &it : items_)
        rows.push_back(it.section ? cp0::ui::ListRow::section(it.label)
                                  : cp0::ui::ListRow::item(it.label, it.value, value_tone(it.tone), it.id));
    return rows;
}

/* A new sample: the rows again; a row whose texts did not change keeps its fitted value (no text measuring), and the
 * list touches only what changed (set_rows: scroll offset and highlight kept). Then the panel. */
void MonitorApp::refresh()
{
    if (grid_) {
        grid_->update(snap_.get(), sampler_->period());
        return;
    }
    if (!list_) return; /* Sampling: nothing live */
    std::vector<Row> rows = rows_now();
    const cp0_ui_list_layout_t &l = list_->layout();
    items_ = list_items(rows, rows_, items_, l.w - 2 * l.inset, l.gap, [this](const std::string &t, bool value) {
        return text_width(t.c_str(), value ? value_font_ : label_font_);
    });
    rows_ = std::move(rows);
    list_->set_rows(list_rows());
    refresh_panel();
    update_status();
}

void MonitorApp::refresh_panel()
{
    const Snapshot *s = snap_.get();
    if (!chart_ || !s) return;
    switch (nav_.current()) {
    case View::Cpu: {
        const ChartText t = cpu_chart(*s);
        chart_->set_text(t.title[0], kRgbValue, "", t.range);
        chart_->set_range(0, 1000);
        chart_->set_series(0, s->hist.cpu, 10.0);
        break;
    }
    case View::Memory: {
        const ChartText t = memory_chart(*s);
        chart_->set_text(t.title[0], kRgbValue, t.title[1], t.range);
        chart_->set_range(0, 1000);
        chart_->set_series(0, s->hist.mem, 10.0);
        chart_->set_series(1, s->hist.swap, 10.0);
        break;
    }
    case View::Thermal: {
        int lo = 20, hi = 90;
        temp_range(s->hist.temp, lo, hi);
        const ChartText t = thermal_chart(*s, lo, hi);
        chart_->set_text(t.title[0], tone_rgb(t.tone, kRgbValue), "", t.range);
        chart_->set_range(lo * 10, hi * 10);
        chart_->set_series(0, s->hist.temp, 10.0);
        break;
    }
    case View::Storage: {
        const double top = rate_ceiling(std::max(s->hist.rd.max(), s->hist.wr.max()));
        const ChartText t = storage_chart(*s, top);
        chart_->set_text(t.title[0], kRgbValue, t.title[1], t.range);
        chart_->set_range(0, static_cast<std::int32_t>(std::min(top, 2.0e9)));
        chart_->set_series(0, s->hist.rd, 1.0);
        chart_->set_series(1, s->hist.wr, 1.0);
        break;
    }
    case View::Network: {
        const double top = rate_ceiling(std::max(s->hist.rx.max(), s->hist.tx.max()));
        const ChartText t = network_chart(*s, top);
        chart_->set_text(t.title[0], kRgbValue, t.title[1], t.range);
        chart_->set_range(0, static_cast<std::int32_t>(std::min(top, 2.0e9)));
        chart_->set_series(0, s->hist.rx, 1.0);
        chart_->set_series(1, s->hist.tx, 1.0);
        break;
    }
    default: break;
    }
    chart_->refresh();
}

/* The Processes page's status line: the highlighted process (pid, state, memory, CPU), or what the list is. */
void MonitorApp::update_status()
{
    if (!status_ || !list_) return;
    const Snapshot *s = snap_.get();
    const int sel = list_->selected();
    std::string text;
    if (s && s->procs_valid) {
        const std::vector<ProcRow> &procs = sort_ == SortBy::Cpu ? s->top_cpu : s->top_mem;
        const int i = sel >= 0 && sel < static_cast<int>(rows_.size()) ? rows_[static_cast<std::size_t>(sel)].id : -1;
        if (i >= 0 && i < static_cast<int>(procs.size())) text = process_caption(procs[static_cast<std::size_t>(i)]);
        else text = process_summary(*s);
    }
    status_->set_text(text);
}

/* A tile (0 .. kTileCount - 1) or a footer item of the overview: its page; the focus comes back to it. */
void MonitorApp::open_item(int index)
{
    if (nav_.current() != View::Overview) return;
    if (index == kTileCount + kFootProcs) sort_ = SortBy::Cpu;
    open(index < kTileCount ? tile_target(index) : footer_target(index - kTileCount), index);
}

void MonitorApp::activate(int row)
{
    if (row < 0 || row >= static_cast<int>(rows_.size())) return;
    const Row r = rows_[static_cast<std::size_t>(row)];
    switch (nav_.current()) {
    case View::Cpu:
    case View::Memory:
        if (r.id == kRowAllProcs) {
            sort_ = nav_.current() == View::Cpu ? SortBy::Cpu : SortBy::Memory;
            open(View::Processes, row);
        }
        break;
    case View::Processes:
        if (r.id == kRowSort) {
            sort_ = sort_ == SortBy::Cpu ? SortBy::Memory : SortBy::Cpu;
            refresh();
        }
        break;
    case View::Period:
        if (r.type == RowType::Choice && r.id > 0) {
            if (r.id != settings_.period_s) {
                settings_.period_s = r.id;
                sampler_->set_period(r.id);
                if (!config_path_.empty() && !save_settings(config_path_, settings_))
                    std::fprintf(stderr, "[system-monitor] cannot save %s\n", config_path_.c_str());
                std::fprintf(stderr, "[system-monitor] sampling every %d s\n", r.id);
            }
            back();
        }
        break;
    default: break;
    }
}

std::vector<std::pair<std::string, std::vector<std::string>>> MonitorApp::about_extra() const
{
    std::vector<std::pair<std::string, std::vector<std::string>>> extra;
    const cp0_ui_metrics_t *m = cp0_ui_metrics_get();
    if (m) {
        char size[32], density[32];
        std::snprintf(size, sizeof(size), "%dx%d", m->w, m->h);
        std::snprintf(density, sizeof(density), "%.1f px/mm", m->ppmm_x100 / 100.0);
        extra.push_back({"Screen", {std::string(size) + " " + cp0_ui_class_name(m->cls) + ", " + density,
                                    std::string(size) + ", " + density, size}});
    }
    cp0_app_env_t env;
    const bool contract = cp0_app_env_read(&env) == 0;
    extra.push_back({"Launcher", {contract ? "contract " + std::to_string(env.contract) + ", " + cp0_app_mode_name(env.mode)
                                           : std::string("no contract"),
                                  contract ? "contract " + std::to_string(env.contract) : std::string("none")}});
    const std::string ref = std::string(CDK_LAUNCHER_REF).substr(0, 7);
    extra.push_back({"App", {std::string(CDK_APP_VERSION) + " (launcher " + ref + ")", CDK_APP_VERSION}});
    return extra;
}

} // namespace sm
