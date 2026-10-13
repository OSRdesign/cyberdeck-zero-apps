/*
 * SPDX-License-Identifier: MIT
 *
 * System Monitor: the app's screen (see monitor_app.hpp). Layout from the layout service only (the roller page, the
 * tokens and the text sizes); the texts and the logic are in core/.
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
    label_font_ = cp0_fonts().get("Montserrat-Bold.ttf", static_cast<uint16_t>(layout_.text_px));
    value_font_ = cp0_fonts().get("Montserrat-Medium.ttf", static_cast<uint16_t>(layout_.value_px));
    if (!label_font_) label_font_ = cp0_ui_font_px(layout_.text_px);
    if (!value_font_) value_font_ = cp0_ui_font_px(layout_.value_px);
    page_ = std::make_unique<cp0::ui::Page>(screen, layout_, cp0::ui::add_title, [this] {
        post([this] { back(); });
    });
    cp0::ui::StatusStrip::add(page_->header());
    page_->set_caption_wrap(true); /* the Processes page's line: up to 3 lines above the centre row, never dots */

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
    roller_.reset();
    meters_.reset();
    chart_.reset();
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

/* Roller and Esc callbacks run inside an LVGL event of the roller: what replaces the roller runs after it. */
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
    if (seq != shown_seq_) {
        shown_seq_ = seq;
        snap_ = sampler_->latest();
        refresh();
    }
    update_caption(false);
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

/* The current page: the panel at the top of the body (none on Processes, System, Sampling), the roller in the room
 * under it, the rows of the last sample. */
void MonitorApp::build_view(int select)
{
    const View view = nav_.current();
    sampler_->set_want_procs(view_wants_procs(view));
    sampler_->set_want_link(view_wants_link(view));
    roller_.reset();
    meters_.reset();
    chart_.reset();
    specs_.clear();
    rows_.clear();
    caption_row_ = -2;
    page_->set_caption("", cp0::ui::CaptionTone::Normal);
    page_->set_title(view == View::Overview ? std::vector<std::string>{kAppTitle}
                                            : std::vector<std::string>{kAppTitle, view_title(view)});

    const int s = cp0_ui_tok(CP0_TOK_SPACE_S);
    Box box;
    box.x = layout_.roller_x;
    box.y = layout_.body_y + s;
    box.w = layout_.roller_w;
    switch (view) {
    case View::Overview:
        box.h = MetersPanel::height_for(sampler_->facts().cores, layout_);
        meters_ = std::make_unique<MetersPanel>(page_->root(), box, sampler_->facts().cores, layout_);
        break;
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
    default: box.h = 0; break;
    }

    cp0_ui_roller_page_t sub = layout_;
    if (box.h > 0) {
        sub.body_y = box.y + box.h + s;
        sub.body_h = layout_.body_y + layout_.body_h - sub.body_y;
        sub.edge_pad = cp0_ui_roller_edge_pad(sub.body_h, sub.row_h);
        sub.list_h = sub.row_h + 2 * sub.edge_pad;
    }
    cp0::ui::RollerEvents events;
    events.activate = [this](int row) { post([this, row] { activate(row); }); };
    events.back = [this] { post([this] { back(); }); };
    roller_ = std::make_unique<cp0::ui::Roller>(page_->root(), sub, std::move(events));

    rows_ = rows_now();
    for (const Row &r : rows_) specs_.push_back(spec_of(r));
    roller_->set_rows(specs_, std::clamp(select, 0, std::max(0, static_cast<int>(specs_.size()) - 1)),
                      view != View::Period);
    refresh_panel();
    update_caption(true);
    focus(roller_->focus_object());
}

std::vector<Row> MonitorApp::rows_now() const
{
    const Snapshot *s = snap_.get();
    switch (nav_.current()) {
    case View::Overview: return overview_rows(s, sampler_->period());
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

/* The first value that fits beside the label on the centre row (the roller's own fonts), else the shortest. */
std::string MonitorApp::fit(const std::string &label, const std::vector<std::string> &values) const
{
    if (values.empty()) return std::string();
    const int room = layout_.roller_w - 2 * layout_.inset - layout_.gap - text_width(label.c_str(), label_font_);
    for (const std::string &v : values)
        if (text_width(v.c_str(), value_font_) <= room) return v;
    return values.back();
}

cp0::ui::RowSpec MonitorApp::spec_of(const Row &row) const
{
    cp0::ui::RowSpec spec;
    spec.label = row.label;
    switch (row.type) {
    case RowType::Nav:
        spec.kind = cp0::ui::RowKind::Nav;
        spec.value = fit(row.label, row.values);
        spec.value_rgb = row.tone == Tone::Normal ? -1 : static_cast<int>(tone_rgb(row.tone, kRgbValue));
        break;
    case RowType::Info:
        spec.kind = cp0::ui::RowKind::Info;
        spec.value = fit(row.label, row.values);
        break;
    case RowType::Choice:
        spec.kind = cp0::ui::RowKind::Choice;
        spec.on = row.on;
        break;
    case RowType::Busy: spec.kind = cp0::ui::RowKind::Busy; break;
    }
    return spec;
}

/* A new sample: the rows whose text changed, or all of them when the list changed; then the panel. */
void MonitorApp::refresh()
{
    if (!roller_) return;
    std::vector<Row> rows = rows_now();
    std::vector<cp0::ui::RowSpec> specs;
    specs.reserve(rows.size());
    for (const Row &r : rows) specs.push_back(spec_of(r));
    bool same_shape = specs.size() == specs_.size();
    for (std::size_t i = 0; same_shape && i < specs.size(); ++i) same_shape = specs[i].kind == specs_[i].kind;
    if (!same_shape) {
        const int sel = roller_->selected();
        roller_->set_rows(specs, std::clamp(sel, 0, std::max(0, static_cast<int>(specs.size()) - 1)),
                          nav_.current() != View::Period);
    } else {
        for (std::size_t i = 0; i < specs.size(); ++i) {
            const cp0::ui::RowSpec &a = specs[i], &b = specs_[i];
            if (a.label != b.label || a.value != b.value || a.value_rgb != b.value_rgb || a.on != b.on)
                roller_->update_row(static_cast<int>(i), a);
        }
    }
    rows_ = std::move(rows);
    specs_ = std::move(specs);
    refresh_panel();
    update_caption(true);
}

void MonitorApp::refresh_panel()
{
    const Snapshot *s = snap_.get();
    if (meters_) {
        meters_->set(s && s->cpu.ok ? s->cpu.cores : std::vector<double>());
        if (s) {
            const Status st = overview_status(s->power, s->temp.ok, s->temp.c);
            meters_->set_status(st.text, st.tone);
        }
    }
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
void MonitorApp::update_caption(bool force)
{
    if (nav_.current() != View::Processes || !roller_) return;
    const int sel = roller_->selected();
    if (!force && sel == caption_row_) return;
    caption_row_ = sel;
    const Snapshot *s = snap_.get();
    std::string text;
    if (s && s->procs_valid && sel >= 0 && sel < static_cast<int>(rows_.size()) && rows_[sel].type == RowType::Info) {
        const std::vector<ProcRow> &list = sort_ == SortBy::Cpu ? s->top_cpu : s->top_mem;
        const int i = rows_[sel].id;
        if (i >= 0 && i < static_cast<int>(list.size())) text = process_caption(list[static_cast<std::size_t>(i)]);
    } else if (s && s->procs_valid) {
        text = "Top " + std::to_string(std::min<std::size_t>(kTopProcs, s->top_cpu.size())) + " of " +
               std::to_string(s->proc_count) + " processes, %CPU of one core";
    }
    page_->set_caption(text, cp0::ui::CaptionTone::Normal);
}

void MonitorApp::activate(int row)
{
    if (row < 0 || row >= static_cast<int>(rows_.size())) return;
    const Row r = rows_[static_cast<std::size_t>(row)];
    switch (nav_.current()) {
    case View::Overview:
        if (r.type == RowType::Nav) open(overview_target(r.id), row);
        break;
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
