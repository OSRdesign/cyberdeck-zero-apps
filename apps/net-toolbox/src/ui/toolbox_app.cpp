/*
 * SPDX-License-Identifier: MIT
 *
 * Net Toolbox: the app's screen, the frame (see toolbox_app.hpp): the chrome of every page, the navigation, the Home,
 * Choice and Typed pages, saving. The tool pages are in tool_pages.cpp.
 */
#include "toolbox_app.hpp"

#include "app.hpp"
#include "build_info.hpp"
#include "style.hpp"
#include "toolbox.hpp"

#include "cp0_font_service.hpp"
#include "cp0_ui_metrics_lvgl.h"

#include "netlog.hpp"
#include "text.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <ctime>

#include <unistd.h>

namespace nt {

namespace {

/* The pages' column: the full width less the deck's side margin (the detail column of System Monitor and Serial
 * Console), never wider than 960 px. */
constexpr int kMarginPx = 40;
constexpr int kCapW = 960;

cp0::ui::ValueTone value_tone(ui::Tone t)
{
    switch (t) {
    case ui::Tone::Ok: return cp0::ui::ValueTone::Ok;
    case ui::Tone::Warn: return cp0::ui::ValueTone::Warn;
    case ui::Tone::Bad: return cp0::ui::ValueTone::Bad;
    default: return cp0::ui::ValueTone::Normal;
    }
}

} // namespace

ToolboxApp::ToolboxApp(lv_obj_t *screen)
{
    backend_ = ui::make_backend();
    const char *over = std::getenv("NET_TOOLBOX_CONFIG");
    if ((!backend_.demo && !backend_.source) || (over && over[0])) config_path_ = settings_path();
    if (!config_path_.empty()) settings_ = load_settings(config_path_);
    for (const std::string &t : backend_.saved_targets) settings_.add_target(t);
    dns_name_ = settings_.dns_name.empty() ? "example.com" : settings_.dns_name;
    dns_type_ = settings_.dns_type.empty() ? "A" : settings_.dns_type;
    dns_resolver_ = settings_.dns_resolver;
    port_host_ = settings_.port_host.empty() ? "example.com" : settings_.port_host;
    port_ = settings_.port;
    port_preset_ = settings_.port_preset;
    ping_target_ = settings_.ping_target;
    trace_target_ = settings_.trace_target;
    if (trace_target_ != "1.1.1.1" && trace_target_ != "8.8.8.8") trace_typed_ = trace_target_;
    http_url_ = settings_.http_url.empty() ? "https://example.com" : settings_.http_url;
    http_insecure_ = settings_.http_insecure;
    whois_query_ = settings_.whois_query;
    for (const auto &kv : backend_.seeds) { /* a demonstration starts with the values its scene names */
        if (kv.first == "ping_target") ping_target_ = kv.second;
        else if (kv.first == "dns_name") dns_name_ = kv.second;
        else if (kv.first == "dns_type") dns_type_ = kv.second;
        else if (kv.first == "dns_resolver") dns_resolver_ = kv.second;
        else if (kv.first == "port_host") port_host_ = kv.second;
        else if (kv.first == "port") port_ = std::atoi(kv.second.c_str());
        else if (kv.first == "port_preset") port_preset_ = kv.second;
        else if (kv.first == "trace_target") trace_target_ = kv.second;
        else if (kv.first == "http_url") http_url_ = kv.second;
        else if (kv.first == "http_method") http_head_ = kv.second == "HEAD";
        else if (kv.first == "http_insecure") http_insecure_ = kv.second == "1";
        else if (kv.first == "whois_query") whois_query_ = kv.second;
    }

    cp0_ui_roller_page(cp0_ui_metrics_get(), cp0::ui::status_strip_left(), &layout_);
    page_ = std::make_unique<cp0::ui::Page>(screen, layout_, cp0::ui::add_title, [this] {
        post([this] { back(); });
    });
    cp0::ui::StatusStrip::add(page_->header());
    std::fprintf(stderr, "[%s] %s, screen %dx%d\n", kAppId, backend_.description.c_str(), layout_.w, layout_.h);

    stack_.push_back(Frame{View::Home, 0});
    show(View::Home, 0);
    timer_ = lv_timer_create(tick_cb, 100, this);
    const char *selftest_env = std::getenv("NET_TOOLBOX_SELFTEST");
    if (selftest_env && selftest_env[0]) {
        const std::string result = selftest();
        std::fprintf(stderr, "[%s] %s\n", kAppId, result.c_str());
        const bool ok = result.rfind("leak check ok", 0) == 0;
        saved_note_ = ok ? "leak check ok" : result; /* the counts go to the log, the band stays the same text */
        saved_tone_ = ok ? ui::Tone::Ok : ui::Tone::Bad;
        saved_until_ = now_ms() + 3600 * 1000;
        refresh_home(true);
    }
}

ToolboxApp::~ToolboxApp()
{
    *alive_ = false;
    if (timer_) lv_timer_delete(timer_);
    lv_async_call_cancel(posted_cb, this);
    if (iw_job_) cdk::async_run::cancel(iw_job_);
    stop_tools(); /* the workers stopped and joined with the runners */
    teardown();
    page_.reset();
}

void ToolboxApp::posted_cb(void *data)
{
    auto *self = static_cast<ToolboxApp *>(data);
    self->post_armed_ = false;
    std::vector<std::function<void()>> fns;
    fns.swap(self->posted_);
    for (auto &fn : fns) fn();
}

/* List, Roller, field and Esc callbacks run inside an LVGL event of the widget: what replaces the widget runs after it. */
void ToolboxApp::post(std::function<void()> fn)
{
    posted_.push_back(std::move(fn));
    if (!post_armed_) post_armed_ = lv_async_call(posted_cb, this) == LV_RESULT_OK;
}

void ToolboxApp::tick_cb(lv_timer_t *timer)
{
    auto *self = static_cast<ToolboxApp *>(lv_timer_get_user_data(timer));
    if (self) self->tick();
}

void ToolboxApp::focus(lv_obj_t *obj)
{
    lv_group_t *group = cdk::input_group();
    if (!group || !obj) return;
    if (lv_obj_get_group(obj) != group) lv_group_add_obj(group, obj);
    lv_group_focus_obj(obj);
}

void ToolboxApp::tick()
{
    const std::int64_t now = now_ms();
    switch (stack_.back().view) {
    case View::Home:
        if (now >= next_state_ms_) refresh_home(false);
        break;
    case View::Interfaces:
        if (now >= next_state_ms_) refresh_interfaces();
        break;
    case View::Diagnosis:
        poll_diagnosis(false);
        if (ladder_) ladder_->tick(now);
        break;
    case View::Ping: poll_ping(false); break;
    case View::Dns: poll_dns(false); break;
    case View::Port: poll_port(false); break;
    case View::Trace: poll_trace(false); break;
    case View::Http: poll_http(false); break;
    case View::PublicIp: poll_publicip(false); break;
    case View::Whois: poll_whois(false); break;
    default: break;
    }
}

/* ---------------------------------------------------------------- navigation */

void ToolboxApp::back()
{
    if (stack_.size() <= 1) {
        cdk::request_quit(0); /* a short Esc at Home ends the app (report 024, Controller decision) */
        return;
    }
    stack_.pop_back();
    show(stack_.back().view, stack_.back().select);
}

void ToolboxApp::open(View view)
{
    if (list_) stack_.back().select = std::max(0, list_->selected());
    else if (rich_) stack_.back().select = rich_->selected_id(); /* the pages of rows of any height remember the row's id */
    if (view != View::Choice && view != View::Typed) fresh_ = false;
    stack_.push_back(Frame{view, 0});
    show(view, 0);
}

/* The page of a choice or a typed value is gone; `action` applies it (and may open another page); the page under it
 * comes back unless the action opened one. */
void ToolboxApp::finish_sub(const std::function<void()> &action)
{
    if (stack_.size() > 1) stack_.pop_back();
    const std::size_t depth = stack_.size();
    if (action) action();
    if (stack_.size() == depth) show(stack_.back().view, stack_.back().select);
}

void ToolboxApp::show(View view, int select)
{
    teardown();
    shown_seq_ = 0;
    next_state_ms_ = 0;
    show_title();
    switch (view) {
    case View::Home: build_home(select); break;
    case View::Interfaces: build_interfaces(); break;
    case View::Diagnosis: build_diagnosis(); break;
    case View::Ping: build_ping(select); break;
    case View::Dns: build_dns(select); break;
    case View::Port: build_port(select); break;
    case View::Trace: build_trace(); break;
    case View::Http: build_http(select); break;
    case View::PublicIp: build_publicip(); break;
    case View::Whois: build_whois(select); break;
    case View::Choice: build_choice(); break;
    case View::Typed: build_typed(); break;
    }
}

/* Everything of the page that was on screen goes; the workers stop first (nothing runs in the background). */
void ToolboxApp::teardown()
{
    stop_tools();
    if (iw_job_) {
        cdk::async_run::cancel(iw_job_);
        iw_job_ = 0;
    }
    rich_.reset(); /* the rows' views let go of their objects first */
    bar_.reset();
    hop_views_.clear();
    http_badge_.reset();
    http_water_.reset();
    http_cert_.reset();
    http_blocks_seq_ = 0;
    band_check_ = layout_cut_ = false;
    list_.reset();
    roller_.reset();
    field_.reset(); /* stops editing: the input context, the keypad intercept and the keyboard request come back */
    typed_status_ = nullptr;
    ladder_.reset();
    ping_panel_.reset();
    badge_.reset();
    notice_.reset();
    if (page_) page_->clear_body();
    if (band_) lv_obj_delete(band_);
    if (message_head_) lv_obj_delete(message_head_);
    if (message_body_) lv_obj_delete(message_body_);
    band_ = message_head_ = message_body_ = nullptr;
    if (page_) page_->set_caption("", cp0::ui::CaptionTone::Normal);
}

void ToolboxApp::stop_tools()
{
    if (backend_.diag) backend_.diag->stop();
    if (backend_.ping) backend_.ping->stop();
    if (backend_.dns) backend_.dns->stop();
    if (backend_.port) backend_.port->stop();
    if (backend_.trace) backend_.trace->stop();
    if (backend_.http) backend_.http->stop();
    if (backend_.pubip) backend_.pubip->stop();
    if (backend_.whois) backend_.whois->stop();
}

void ToolboxApp::show_title()
{
    std::vector<std::string> path{kAppTitle};
    const auto tool_name = [](View v) -> const char * {
        switch (v) {
        case View::Interfaces: return "Interfaces";
        case View::Diagnosis: return "Diagnosis";
        case View::Ping: return "Ping";
        case View::Dns: return "DNS lookup";
        case View::Port: return "Port check";
        case View::Trace: return "Traceroute";
        case View::Http: return "HTTP check";
        case View::PublicIp: return "Public IP";
        case View::Whois: return "Whois";
        default: return "";
        }
    };
    const View v = stack_.back().view;
    if (v == View::Choice || v == View::Typed) {
        if (stack_.size() >= 2) path.push_back(tool_name(stack_[stack_.size() - 2].view));
        path.push_back(v == View::Choice ? choice_.title : typed_.title);
    } else if (v != View::Home) {
        path.push_back(tool_name(v));
    }
    page_->set_title(path);
}

/* ---------------------------------------------------------------- geometry and the chrome */

ToolboxApp::Geo ToolboxApp::geo() const
{
    Geo g;
    const int margin = std::min(layout_.roller_x, kMarginPx);
    g.w = std::max(1, std::min(kCapW, layout_.w - 2 * margin));
    g.x = (layout_.w - g.w) / 2;
    g.s = cp0_ui_tok(CP0_TOK_SPACE_S);
    g.inset = cp0_ui_tok(CP0_TOK_SPACE_L);
    g.lh = ui::line_height(ui::caption_font());
    g.top = layout_.body_y + g.s;
    g.bottom = layout_.body_y + layout_.body_h;
    g.row = cp0_ui_tok(CP0_TOK_ROW_H);
    g.crow = cp0_ui_tok(CP0_TOK_LIST_ROW_H);
    return g;
}

/* The status band: one line of CAPTION text at the top of the column. Returns the y under it. */
int ToolboxApp::build_band()
{
    const Geo g = geo();
    band_ = ui::make_label(page_->root(), ui::caption_font(), ui::kOk);
    lv_label_set_long_mode(band_, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_pos(band_, g.x + g.inset, g.top);
    lv_obj_set_size(band_, std::max(1, g.w - 2 * g.inset), g.lh);
    return g.top + g.lh + g.s;
}

/* The band shows `dflt` unless a note (Saved ..., an error of Save) is still fresh. */
void ToolboxApp::show_band(const ui::Band &dflt)
{
    if (!band_) return;
    ui::Band b = dflt;
    bool noted = false;
    if (saved_until_ > now_ms() && !saved_note_.empty()) {
        b.text = saved_note_;
        b.tone = saved_tone_;
        noted = true; /* a note carries a path: it may be cut with dots */
    }
    if (band_check_ && backend_.demo && !noted) { /* the scenes: the status line of a 0.2 page is never cut (no ellipsis) */
        if (ui::text_width(b.text.c_str(), ui::caption_font()) > lv_obj_get_width(band_)) {
            std::fprintf(stderr, "[%s] the status line does not fit: %s\n", kAppId, b.text.c_str());
            layout_cut_ = true;
        }
        if (layout_cut_) {
            b.text = "Message cut off";
            b.tone = ui::Tone::Bad;
        }
    }
    ui::set_text_if(band_, b.text);
    ui::set_color_if(band_, ui::tone_rgb(b.tone, ui::kLabel));
}

void ToolboxApp::note(const ui::Band &b)
{
    saved_note_ = b.text;
    saved_tone_ = b.tone;
    saved_until_ = now_ms() + 6000;
}

std::vector<cp0::ui::ListRow> ToolboxApp::to_rows(const std::vector<ui::RowDef> &defs) const
{
    std::vector<cp0::ui::ListRow> rows;
    rows.reserve(defs.size());
    for (const ui::RowDef &d : defs)
        rows.push_back(d.section ? cp0::ui::ListRow::section(d.label)
                                 : cp0::ui::ListRow::item(d.label, d.value, value_tone(d.tone), d.id));
    return rows;
}

void ToolboxApp::build_list(cp0_ui_list_density_t density, int y, int h, bool read_only, std::vector<cp0::ui::ListRow> rows,
                            int select)
{
    const Geo g = geo();
    cp0_ui_list_layout_t l{};
    cp0_ui_list_layout(cp0_ui_metrics_get(), density, g.x, y, g.w, std::max(1, h), &l);
    cp0::ui::CompactListEvents events;
    events.activate = [this](int index, std::intptr_t id) { post([this, index, id] { activate(index, id); }); };
    events.back = [this] { post([this] { back(); }); };
    cp0::ui::CompactListOptions options;
    options.read_only = read_only;
    list_ = std::make_unique<cp0::ui::CompactList>(page_->root(), l, std::move(events), options);
    list_->reset_rows(rows, select);
    focus(list_->focus_object());
}

/* ---------------------------------------------------------------- Home */

void ToolboxApp::build_home(int select)
{
    const Geo g = geo();
    net_ = backend_.read_state();
    const int y = build_band();
    std::vector<cp0::ui::ListRow> rows;
    rows.push_back(cp0::ui::ListRow::item("Diagnose my connection", "", cp0::ui::ValueTone::Normal, kHomeDiag));
    rows.push_back(cp0::ui::ListRow::item("Ping", "", cp0::ui::ValueTone::Normal, kHomePing));
    rows.push_back(cp0::ui::ListRow::item("Traceroute", "", cp0::ui::ValueTone::Normal, kHomeTrace));
    rows.push_back(cp0::ui::ListRow::item("DNS lookup", "", cp0::ui::ValueTone::Normal, kHomeDns));
    rows.push_back(cp0::ui::ListRow::item("Port check", "", cp0::ui::ValueTone::Normal, kHomePort));
    rows.push_back(cp0::ui::ListRow::item("HTTP check", "", cp0::ui::ValueTone::Normal, kHomeHttp));
    rows.push_back(cp0::ui::ListRow::item("Public IP", "", cp0::ui::ValueTone::Normal, kHomePublicIp));
    rows.push_back(cp0::ui::ListRow::item("Whois", "", cp0::ui::ValueTone::Normal, kHomeWhois));
    rows.push_back(cp0::ui::ListRow::item("Interfaces", "", cp0::ui::ValueTone::Normal, kHomeIfaces));
    build_list(CP0_UI_LIST_COMFORTABLE, y, g.bottom - y, false, std::move(rows), select);
    show_band(ui::home_band(net_));
    next_state_ms_ = now_ms() + 2000;
}

void ToolboxApp::refresh_home(bool force)
{
    next_state_ms_ = now_ms() + 2000;
    if (!force || !band_) net_ = backend_.read_state();
    show_band(ui::home_band(net_));
}

/* ---------------------------------------------------------------- Choice and Typed */

void ToolboxApp::open_choice(Choice choice)
{
    choice_ = std::move(choice);
    open(View::Choice);
}

void ToolboxApp::build_choice()
{
    cp0::ui::RollerEvents events;
    events.activate = [this](int row) {
        post([this, row] {
            if (stack_.back().view != View::Choice) return;
            const std::function<void(int)> chosen = choice_.chosen;
            finish_sub([chosen, row] {
                if (chosen) chosen(row);
            });
        });
    };
    events.back = [this] { post([this] { back(); }); };
    roller_ = std::make_unique<cp0::ui::Roller>(page_->root(), layout_, std::move(events));
    std::vector<cp0::ui::RowSpec> specs;
    for (std::size_t i = 0; i < choice_.labels.size(); ++i) {
        cp0::ui::RowSpec spec;
        spec.kind = cp0::ui::RowKind::Choice;
        spec.label = choice_.labels[i];
        spec.on = static_cast<int>(i) == choice_.current;
        specs.push_back(spec);
    }
    const int start = std::clamp(choice_.current, 0, std::max(0, static_cast<int>(specs.size()) - 1));
    roller_->set_rows(specs, start, false);
    focus(roller_->focus_object());
}

void ToolboxApp::open_typed(Typed typed)
{
    typed_ = std::move(typed);
    open(View::Typed);
}

/* One text field (the label beside it), a status line under it for what is wrong. The field starts editing when it
 * takes the focus: the on-screen keyboard shows when the launcher's rule allows it, a physical keyboard types at once. */
void ToolboxApp::build_typed()
{
    cp0::ui::TextFieldSpec spec;
    spec.label = typed_.label;
    spec.type = typed_.type;
    spec.text = typed_.text;
    spec.placeholder = typed_.placeholder;
    spec.max_bytes = typed_.max_bytes;
    spec.label_w = layout_.roller_w * 22 / 100;
    if (const lv_font_t *font = cp0_fonts().get("Montserrat-Medium.ttf",
                                                 static_cast<uint16_t>(cp0_ui_metrics_get()->text_px[CP0_TEXT_BODY]))) {
        lv_point_t size{};
        lv_text_get_size(&size, spec.label.c_str(), font, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
        spec.label_w = std::max(spec.label_w, static_cast<int>(size.x) + layout_.gap);
    }
    cp0::ui::TextFieldEvents events;
    events.submit = [this](const std::string &text) {
        post([this, text] {
            if (stack_.back().view != View::Typed || !field_) return;
            const std::string value = trim(text);
            const std::string why = typed_.check ? typed_.check(value) : std::string();
            if (!why.empty()) {
                set_typed_status(why);
                if (field_ && !field_->editing()) field_->begin_edit();
                return;
            }
            const std::function<void(const std::string &)> done = typed_.done;
            finish_sub([done, value] {
                if (done) done(value);
            });
        });
    };
    events.back = [this] { post([this] { back(); }); };
    field_ = std::make_unique<cp0::ui::TextField>(page_->body(), layout_, std::move(spec), std::move(events));
    typed_status_ = lv_label_create(page_->body());
    const lv_font_t *font = cp0_ui_font_px(layout_.caption_px);
    lv_label_set_long_mode(typed_status_, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_style_text_font(typed_status_, font, 0);
    lv_obj_set_style_text_color(typed_status_, lv_color_hex(ui::kBad), 0);
    lv_obj_set_width(typed_status_, layout_.roller_w);
    lv_obj_set_height(typed_status_, lv_font_get_line_height(font));
    set_typed_status("");
    focus(field_->focus_object());
}

/* The error line shows right under the field; with it, the body loses its top padding so the field and the line fit
 * above the on-screen keyboard (the template's rule). */
void ToolboxApp::set_typed_status(const std::string &text)
{
    if (!typed_status_ || !field_) return;
    lv_label_set_text(typed_status_, text.c_str());
    lv_obj_t *body = page_->body();
    if (text.empty()) {
        lv_obj_add_flag(typed_status_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_pad_top(body, layout_.gap, 0);
        return;
    }
    lv_obj_remove_flag(typed_status_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_pad_top(body, 0, 0);
    lv_obj_update_layout(body);
    lv_obj_scroll_to_view(typed_status_, LV_ANIM_OFF);
    lv_obj_scroll_to_view(field_->row(), LV_ANIM_OFF);
}

/* ---------------------------------------------------------------- activation, saving */

void ToolboxApp::activate(int, std::intptr_t id)
{
    switch (stack_.back().view) {
    case View::Home:
        if (id == kHomeDiag) open(View::Diagnosis);
        else if (id == kHomePing) open(View::Ping);
        else if (id == kHomeDns) open(View::Dns);
        else if (id == kHomePort) open(View::Port);
        else if (id == kHomeIfaces) open(View::Interfaces);
        else if (id == kHomeTrace) open(View::Trace);
        else if (id == kHomeHttp) open(View::Http);
        else if (id == kHomePublicIp) open(View::PublicIp);
        else if (id == kHomeWhois) open(View::Whois);
        break;
    case View::Diagnosis: activate_diagnosis(id); break;
    case View::Ping: activate_ping(id); break;
    case View::Dns: activate_dns(id); break;
    case View::Port: activate_port(id); break;
    case View::Trace: activate_trace(id); break;
    case View::Http: activate_http(id); break;
    case View::PublicIp: activate_publicip(id); break;
    case View::Whois: activate_whois(id); break;
    default: break;
    }
}

void ToolboxApp::save_result()
{
    const View view = stack_.back().view;
    const std::time_t when = std::time(nullptr);
    std::string tool, title, body;
    bool have = false;
    switch (view) {
    case View::Diagnosis:
        if (fresh_ && backend_.diag->latest() && backend_.diag->latest()->state != DiagReport::State::Idle) {
            tool = "diagnosis";
            title = "Diagnosis";
            body = format_diagnosis(*backend_.diag->latest());
            have = true;
        }
        break;
    case View::Ping:
        if (fresh_ && backend_.ping->latest() && backend_.ping->latest()->state != PingSnapshot::State::Idle) {
            tool = "ping";
            title = "Ping";
            body = format_ping(*backend_.ping->latest());
            have = true;
        }
        break;
    case View::Dns:
        if (fresh_ && backend_.dns->latest() && backend_.dns->latest()->state != DnsSnapshot::State::Idle) {
            tool = "dns";
            title = "DNS lookup";
            body = format_dns(*backend_.dns->latest());
            have = true;
        }
        break;
    case View::Port:
        if (fresh_ && backend_.port->latest() && backend_.port->latest()->state != PortSnapshot::State::Idle) {
            tool = "port";
            title = "Port check";
            body = format_ports(*backend_.port->latest());
            have = true;
        }
        break;
    case View::Trace:
        if (fresh_ && backend_.trace->latest() && backend_.trace->latest()->state != TraceSnapshot::State::Idle &&
            backend_.trace->latest()->state != TraceSnapshot::State::Resolving) {
            tool = "traceroute";
            title = "Traceroute";
            body = format_trace(*backend_.trace->latest());
            have = true;
        }
        break;
    case View::Http:
        if (fresh_ && backend_.http->latest() && backend_.http->latest()->state == HttpCheckSnapshot::State::Done) {
            tool = "http";
            title = "HTTP check";
            body = format_httpcheck(backend_.http->latest()->result);
            have = true;
        }
        break;
    case View::PublicIp:
        if (fresh_ && backend_.pubip->latest() && backend_.pubip->latest()->state == PublicIpSnapshot::State::Done) {
            tool = "publicip";
            title = "Public IP";
            body = format_publicip(backend_.pubip->latest()->result, local_address());
            have = true;
        }
        break;
    case View::Whois:
        if (fresh_ && backend_.whois->latest() && backend_.whois->latest()->state == WhoisSnapshot::State::Done) {
            tool = "whois";
            title = "Whois";
            body = format_whois(backend_.whois->latest()->result);
            have = true;
        }
        break;
    default: break;
    }
    if (!have) {
        ui::Band b;
        b.text = "Nothing to save yet: run it first";
        b.tone = ui::Tone::Warn;
        note(b);
        return;
    }
    /* a demonstration (the scenes run several sizes at once) writes into a folder of its own process */
    const std::string dir = backend_.demo && !default_log_dir().empty() ? default_log_dir() + "/p" + std::to_string(getpid()) : default_log_dir();
    if (dir.empty()) {
        ui::Band b;
        b.text = "Cannot save: there is no home folder";
        b.tone = ui::Tone::Bad;
        note(b);
        return;
    }
    const SaveResult r = nt::save_log(dir, tool, log_heading(CDK_APP_VERSION, title, when) + "\n" + body, when);
    note(ui::save_band(r.ok, r.ok ? r.path : r.error));
}

void ToolboxApp::save_settings_now()
{
    settings_.ping_target = ping_target_;
    settings_.dns_name = dns_name_;
    settings_.dns_type = dns_type_;
    settings_.dns_resolver = dns_resolver_;
    settings_.port_host = port_host_;
    settings_.port = port_;
    settings_.port_preset = port_preset_;
    settings_.trace_target = trace_target_;
    settings_.http_url = http_url_;
    settings_.http_insecure = http_insecure_;
    settings_.whois_query = whois_query_;
    if (config_path_.empty()) return;
    if (!save_settings(config_path_, settings_)) std::fprintf(stderr, "[%s] cannot save %s\n", kAppId, config_path_.c_str());
}

std::string ToolboxApp::system_resolver() const
{
    return net_.dns.servers.empty() ? std::string() : net_.dns.servers[0];
}

std::string ToolboxApp::gateway() const
{
    return net_.route.ok ? net_.route.gateway : std::string();
}

/* ---------------------------------------------------------------- the page-switch check */

/* NET_TOOLBOX_SELFTEST: the pages are opened and left (Choice and Typed pages too); the objects and the timers of the
 * page must be what they were. The headless scenes show the answer in the Home band. */
std::string ToolboxApp::selftest()
{
    const auto snapshot = [this] { return std::make_pair(ui::count_objects(page_->root()), ui::count_timers()); };
    const auto base = snapshot();
    const View tools[] = {View::Interfaces, View::Diagnosis, View::Ping, View::Dns, View::Port, View::Trace, View::Http,
                          View::PublicIp, View::Whois};
    for (int round = 0; round < 3; ++round) {
        for (const View v : tools) {
            stack_.assign(1, Frame{View::Home, 0});
            open(v);
            if (v == View::Ping || v == View::Dns || v == View::Port || v == View::Trace || v == View::Http ||
                v == View::Whois) {
                Choice c;
                c.title = "Test";
                c.labels = {"One", "Two"};
                c.current = 0;
                open_choice(c);
                back();
                Typed t;
                t.title = "Test";
                t.label = "Value";
                open_typed(t);
                back();
            }
            back();
        }
    }
    stack_.assign(1, Frame{View::Home, 0});
    show(View::Home, 0);
    const auto after = snapshot();
    if (after != base)
        return "leak check FAILED: " + std::to_string(base.first) + " objects, " + std::to_string(base.second) + " timers -> " +
               std::to_string(after.first) + ", " + std::to_string(after.second);
    return "leak check ok: " + std::to_string(base.first) + " objects, " + std::to_string(base.second) + " timers";
}

} // namespace nt
