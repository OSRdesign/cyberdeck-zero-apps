/*
 * SPDX-License-Identifier: MIT
 *
 * The template app's screen (see template_app.hpp).
 */
#include "template_app.hpp"

#include "app.hpp"
#include "build_info.hpp"
#include "osk_policy.hpp"
#include "status_strip.hpp"

#include "cp0_app_contract.h"
#include "cp0_font_service.hpp"
#include "cp0_ui_metrics_lvgl.h"

#include <algorithm>
#include <ctime>
#include <utility>

namespace tpl {
namespace {

constexpr const char *kAppTitle = "Template";
constexpr int kErrorRgb = 0xFF6666;   /* the shell's error colour (status line, failed values) */
constexpr int kLabelColumnPct = 22;   /* form labels beside their fields (the launcher's SSH form, Controller design) */

std::int64_t monotonic_ms()
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<std::int64_t>(ts.tv_sec) * 1000 + ts.tv_nsec / 1000000;
}

} // namespace

TemplateApp::TemplateApp(lv_obj_t *screen)
{
    cp0_ui_roller_page(cp0_ui_metrics_get(), cdk::status_strip::left(), &layout_);
    page_ = std::make_unique<cp0::ui::Page>(screen, layout_, cdk::status_strip::title_factory(), [this] {
        post([this] { back(); });
    });
    cdk::status_strip::attach(page_->header());

    cp0::ui::RollerEvents events;
    events.activate = [this](int row) { post([this, row] { activate(row); }); };
    events.back = [this] { post([this] { back(); }); };
    roller_ = std::make_unique<cp0::ui::Roller>(page_->root(), layout_, std::move(events));
    roller_->set_rows(root_rows(), 0, true);
    focus(roller_->focus_object());
    show_title();
}

TemplateApp::~TemplateApp()
{
    lv_async_call_cancel(posted_cb, this);
    if (job_) cdk::async_run::cancel(job_);
    dialog_.reset();
    field_.reset(); /* stops editing: input context, keypad intercept and the keyboard request are restored */
    info_.reset();
    roller_.reset();
    page_.reset();
}

void TemplateApp::posted_cb(void *data)
{
    auto *self = static_cast<TemplateApp *>(data);
    self->post_armed_ = false;
    std::vector<std::function<void()>> fns;
    fns.swap(self->posted_);
    for (auto &fn : fns) fn();
}

void TemplateApp::post(std::function<void()> fn)
{
    posted_.push_back(std::move(fn));
    if (!post_armed_) post_armed_ = lv_async_call(posted_cb, this) == LV_RESULT_OK;
}

void TemplateApp::focus(lv_obj_t *obj)
{
    lv_group_t *group = cdk::input_group();
    if (!group || !obj) return;
    if (lv_obj_get_group(obj) != group) lv_group_add_obj(group, obj);
    lv_group_focus_obj(obj);
}

void TemplateApp::show_title()
{
    page_->set_title(nav_.title_path(kAppTitle));
}

Facts TemplateApp::facts() const
{
    Facts f;
    const cp0_ui_metrics_t *m = cp0_ui_metrics_get();
    f.w = m->w;
    f.h = m->h;
    f.cls = cp0_ui_class_name(m->cls);
    f.ppmm_x100 = m->ppmm_x100;
    f.row = m->tok[CP0_TOK_ROW_H];
    f.target = m->tok[CP0_TOK_TARGET];
    f.header = m->tok[CP0_TOK_HEADER_H];
    cp0_app_env_t env;
    f.contract = cp0_app_env_read(&env) == 0;
    f.mode = cp0_app_mode_name(env.mode);
    if (cdk::osk_policy::Policy *policy = cdk::osk_policy::installed()) {
        f.osk_allowed = policy->allowed(monotonic_ms());
        f.input_file = policy->from_file();
        f.keyboards = policy->input().keyboards;
        f.vkb = cp0_app_vkb_name(policy->input().vkb);
    }
    f.version = CDK_APP_VERSION;
    f.launcher_ref = std::string(CDK_LAUNCHER_REF).substr(0, 7) + " (" + CDK_LAUNCHER_PIN + ")";
    return f;
}

cp0::ui::RowSpec TemplateApp::command_row() const
{
    cp0::ui::RowSpec row;
    if (run_.busy()) {
        row.kind = cp0::ui::RowKind::Busy;
        row.label = run_.busy_label();
        return row;
    }
    row.kind = cp0::ui::RowKind::Nav;
    row.label = run_.label();
    row.value = run_.value();
    if (run_.failed()) row.value_rgb = kErrorRgb;
    return row;
}

std::vector<cp0::ui::RowSpec> TemplateApp::root_rows() const
{
    std::vector<cp0::ui::RowSpec> rows(kScreen + 1);
    rows[kInfo].kind = cp0::ui::RowKind::Nav;
    rows[kInfo].label = "Device info";
    rows[kName].kind = cp0::ui::RowKind::Nav;
    rows[kName].label = "Name";
    rows[kName].value = name_;
    rows[kSwitch].kind = cp0::ui::RowKind::Toggle;
    rows[kSwitch].label = "Example switch";
    rows[kSwitch].on = switch_on_;
    rows[kReset].kind = cp0::ui::RowKind::Nav;
    rows[kReset].label = "Reset";
    rows[kCommand] = command_row();
    rows[kScreen].kind = cp0::ui::RowKind::Info;
    rows[kScreen].label = "Screen";
    rows[kScreen].value = screen_summary(facts());
    return rows;
}

void TemplateApp::activate(int row)
{
    if (dialog_ || nav_.current() != View::Root) return;
    switch (row) {
    case kInfo: open_info(); break;
    case kName: open_form(); break;
    case kSwitch: {
        switch_on_ = !switch_on_;
        cp0::ui::RowSpec spec = root_rows()[kSwitch];
        roller_->update_row(kSwitch, spec);
        break;
    }
    case kReset: open_dialog(); break;
    case kCommand:
        if (run_.busy()) cdk::async_run::cancel(job_);
        else start_command();
        break;
    default: break; /* Info rows have no action */
    }
}

void TemplateApp::back()
{
    if (dialog_) {
        close_dialog(false);
        return;
    }
    if (nav_.back() == Back::Quit) {
        cdk::request_quit(0); /* a short Esc at the root ends the app (report 024, Controller decision) */
        return;
    }
    close_view();
}

/* The page that was open goes; the root roller comes back with its highlight. */
void TemplateApp::close_view()
{
    info_.reset();
    field_.reset();
    form_status_ = nullptr; /* deleted with the body */
    page_->clear_body();
    roller_->update_row(kName, root_rows()[kName]);
    roller_->set_hidden(false);
    focus(roller_->focus_object());
    show_title();
}

void TemplateApp::open_info()
{
    nav_.open(View::Info);
    roller_->set_hidden(true);
    info_ = std::make_unique<cp0::ui::InfoTable>(page_->root(), layout_, [this] { post([this] { back(); }); });
    std::vector<cp0::ui::InfoItem> items;
    for (const auto &kv : info_items(facts())) items.push_back({kv.first, kv.second});
    info_->set_items(items);
    focus(info_->focus_object());
    show_title();
}

void TemplateApp::open_form()
{
    nav_.open(View::Form);
    roller_->set_hidden(true);
    cp0::ui::TextFieldSpec spec;
    spec.label = "Name";
    spec.type = cp0::ui::TextType::Text;
    spec.text = name_;
    spec.placeholder = "Type a name";
    spec.max_bytes = 4 * kNameMaxChars;
    /* the label beside the field (one ROW per field): a column of 22 % of the row, at least the label at the BODY size
     * plus a gap; with the on-screen keyboard up at 640x480 the field and its error line fit above it */
    spec.label_w = layout_.roller_w * kLabelColumnPct / 100;
    if (const lv_font_t *font = cp0_fonts().get("Montserrat-Medium.ttf",
                                                 static_cast<uint16_t>(cp0_ui_metrics_get()->text_px[CP0_TEXT_BODY]))) {
        lv_point_t size{};
        lv_text_get_size(&size, spec.label.c_str(), font, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
        spec.label_w = std::max(spec.label_w, static_cast<int>(size.x) + layout_.gap);
    }
    cp0::ui::TextFieldEvents events;
    events.submit = [this](const std::string &text) { post([this, text] { submit_name(text); }); };
    events.back = [this] { post([this] { back(); }); };
    field_ = std::make_unique<cp0::ui::TextField>(page_->body(), layout_, std::move(spec), std::move(events));
    /* one status line under the field, the caption size, empty until a submit is refused */
    form_status_ = lv_label_create(page_->body());
    const lv_font_t *font = cp0_ui_font_px(layout_.caption_px);
    lv_label_set_long_mode(form_status_, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_style_text_font(form_status_, font, 0);
    lv_obj_set_style_text_color(form_status_, lv_color_hex(kErrorRgb), 0);
    lv_obj_set_width(form_status_, layout_.roller_w);
    lv_obj_set_height(form_status_, lv_font_get_line_height(font));
    set_form_status("");
    show_title();
    focus(field_->focus_object()); /* focus starts editing: the on-screen keyboard shows when the rule allows it */
}

void TemplateApp::submit_name(const std::string &text)
{
    if (!field_) return;
    const std::string name = trim(text);
    const std::string error = name_error(name);
    if (!error.empty()) {
        set_form_status(error);
        if (!field_->editing()) field_->begin_edit();
        return;
    }
    name_ = name;
    nav_.back();
    close_view();
}

/* The error line shows right under the field; with it, the body loses its top padding so the field and the line fit
 * above the on-screen keyboard, and the field is scrolled into view (the rule of the launcher's SSH form). */
void TemplateApp::set_form_status(const std::string &text)
{
    if (!form_status_ || !field_) return;
    lv_label_set_text(form_status_, text.c_str());
    lv_obj_t *body = page_->body();
    if (text.empty()) {
        lv_obj_add_flag(form_status_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_pad_top(body, layout_.gap, 0);
        return;
    }
    lv_obj_remove_flag(form_status_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_pad_top(body, 0, 0);
    lv_obj_update_layout(body);
    lv_obj_scroll_to_view(form_status_, LV_ANIM_OFF);
    lv_obj_scroll_to_view(field_->row(), LV_ANIM_OFF);
}

void TemplateApp::open_dialog()
{
    cp0::ui::DialogSpec spec;
    spec.title = "Reset the example?";
    spec.page = page_.get();
    spec.buttons.push_back({"Cancel", cp0::ui::ButtonRole::Cancel, [this] { post([this] { close_dialog(false); }); }});
    spec.buttons.push_back({"Reset", cp0::ui::ButtonRole::Primary, [this] { post([this] { close_dialog(true); }); }});
    dialog_ = std::make_unique<cp0::ui::Dialog>(page_->root(), layout_, std::move(spec));
    dialog_->add_text("The name and the switch go back to their first values.");
    dialog_->relayout();
    focus(dialog_->focus_object());
}

void TemplateApp::close_dialog(bool reset)
{
    if (!dialog_) return;
    dialog_.reset();
    if (reset) {
        name_ = "Deck";
        switch_on_ = false;
        roller_->update_row(kName, root_rows()[kName]);
        roller_->update_row(kSwitch, root_rows()[kSwitch]);
    }
    focus(roller_->focus_object());
}

void TemplateApp::start_command()
{
    run_.begin();
    refresh_command();
    cdk::async_run::Spec spec;
    spec.argv = demo_command();
    spec.timeout_ms = kDemoTimeoutMs;
    spec.on_line = [this](const std::string &line) {
        run_.line(line);
        refresh_command();
    };
    spec.on_done = [this](const cdk::async_run::Result &r) {
        job_ = 0;
        run_.finish(r.started, r.exit_code, r.term_signal, r.timed_out, r.cancelled);
        refresh_command();
    };
    job_ = cdk::async_run::run(std::move(spec));
}

/* Busy <-> Nav changes the row's kind: the rows are set again (the highlight stays); a new line only relabels it. */
void TemplateApp::refresh_command()
{
    if (!roller_) return;
    const cp0::ui::RowSpec spec = command_row();
    if (spec.kind == command_kind_) {
        roller_->update_row(kCommand, spec);
        return;
    }
    command_kind_ = spec.kind; /* update_row() keeps a row's kind: set them all again */
    roller_->set_rows(root_rows(), roller_->selected(), true);
}

} // namespace tpl
