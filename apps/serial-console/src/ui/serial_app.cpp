/*
 * SPDX-License-Identifier: MIT
 *
 * Serial Console: the app's screen (see serial_app.hpp). Layout from the layout service only (the roller page, the
 * tokens and the text sizes); the logic is in core/.
 */
#include "serial_app.hpp"

#include "app.hpp"

#include "cp0_ui_metrics_lvgl.h"

#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <utility>

namespace sc {
namespace {

constexpr int kTickMs = 50;
constexpr int kScanMs = 1000;
constexpr int kErrorRgb = 0xFF6666;       /* the shell's error colour (failed values) */
constexpr std::uint32_t kCardBg = 0x1C1C1C;     /* the text fields' colours */
constexpr std::uint32_t kCardBorder = 0x3A3A3A;
constexpr std::uint32_t kCardTitle = 0xFFFFFF;
constexpr std::uint32_t kCardText = 0xE0E0E0;

const char *choice_title(int which)
{
    switch (which) {
    case 0: return "Speed";
    case 1: return "Frame";
    case 2: return "Enter sends";
    default: return "Backspace sends";
    }
}

} // namespace

SerialApp::SerialApp(lv_obj_t *screen)
{
    world_ = demo::World::from_env();
    if (world_) { /* a demonstration keeps its settings and logs inside its own folder */
        roots_ = world_->roots();
        config_path_ = world_->home() + "/.config/serial-console.conf";
        log_dir_ = world_->home() + "/serial-logs";
    } else {
        config_path_ = config_path();
        log_dir_ = log_dir();
    }
    bool migrated = false;
    config_ = load_config(config_path_, &migrated);
    if (migrated && save_config(config_path_, config_))
        std::fprintf(stderr, "[serial-console] the 0.1.0 settings of %s were kept (%s)\n", config_.last_port.c_str(),
                     config_path_.c_str());

    cp0_ui_roller_page(cp0_ui_metrics_get(), cp0::ui::status_strip_left(), &layout_);
    page_ = std::make_unique<cp0::ui::Page>(screen, layout_, cp0::ui::add_title, [this] {
        post([this] { back(); });
    });
    cp0::ui::StatusStrip::add(page_->header());
    page_->set_caption_wrap(true); /* the status line takes up to 3 lines over the rows above the centre */

    cp0::ui::RollerEvents events;
    events.activate = [this](int row) {
        post([this, row] {
            if (dialog_) return;
            switch (nav_.current()) {
            case View::Ports: activate_ports(row); break;
            case View::Port: activate_port(row); break;
            case View::Choice: activate_choice(row); break;
            case View::Terminal: break;
            }
        });
    };
    events.back = [this] { post([this] { back(); }); };
    roller_ = std::make_unique<cp0::ui::Roller>(page_->root(), layout_, std::move(events));

    ports_ = scan_ports(roots_, config_.show_onboard);
    next_scan_ = now_ms() + kScanMs;
    show_ports(config_.last_port, false);
    focus(roller_->focus_object());
    show_title();
    timer_ = lv_timer_create(tick_cb, kTickMs, this);
}

SerialApp::~SerialApp()
{
    if (timer_) lv_timer_delete(timer_);
    lv_async_call_cancel(posted_cb, this);
    term_.reset();   /* the connection closed, its worker joined */
    finder_.reset();
    dialog_.reset();
    roller_.reset();
    page_.reset();   /* the card goes with the page */
    card_ = nullptr;
    world_.reset();
}

void SerialApp::posted_cb(void *data)
{
    auto *self = static_cast<SerialApp *>(data);
    self->post_armed_ = false;
    std::vector<std::function<void()>> fns;
    fns.swap(self->posted_);
    for (auto &fn : fns) fn();
}

void SerialApp::post(std::function<void()> fn)
{
    posted_.push_back(std::move(fn));
    if (!post_armed_) post_armed_ = lv_async_call(posted_cb, this) == LV_RESULT_OK;
}

void SerialApp::tick_cb(lv_timer_t *timer)
{
    auto *self = static_cast<SerialApp *>(lv_timer_get_user_data(timer));
    if (self) self->tick();
}

void SerialApp::tick()
{
    if (term_) term_->tick();
    check_finder();
    const std::int64_t now = now_ms();
    if (now >= next_scan_) {
        next_scan_ = now + kScanMs;
        scan();
    }
    if (nav_.current() == View::Ports && !dialog_) update_ports_caption(false);
    if (nav_.current() == View::Port && !dialog_) update_port_caption(false);
}

void SerialApp::focus(lv_obj_t *obj)
{
    lv_group_t *group = cdk::input_group();
    if (!group || !obj) return;
    if (lv_obj_get_group(obj) != group) lv_group_add_obj(group, obj);
    lv_group_focus_obj(obj);
}

void SerialApp::show_title()
{
    switch (nav_.current()) {
    case View::Ports: page_->set_title({kAppTitle}); break;
    case View::Port: page_->set_title({kAppTitle, port_.tty}); break;
    case View::Choice: page_->set_title({kAppTitle, port_.tty, choice_title(static_cast<int>(choice_))}); break;
    case View::Terminal: break; /* the terminal view sets "ttyUSB0 115200 8N1" */
    }
}

GroupState SerialApp::groups() const
{
    GroupState g = dialout_state();
    if (world_ && world_->no_group()) g.root = g.active = g.listed = false;
    return g;
}

bool SerialApp::can_open(const PortInfo &port) const
{
    return access(port.path.c_str(), R_OK | W_OK) == 0;
}

void SerialApp::back()
{
    if (dialog_) {
        close_dialog();
        return;
    }
    switch (nav_.current()) {
    case View::Terminal: close_terminal(); return;
    case View::Choice: {
        nav_.back();
        const PortRow rows[] = {PortRow::Speed, PortRow::Frame, PortRow::Ending, PortRow::Backspace};
        show_port_rows(port_row_index(rows[static_cast<int>(choice_)]));
        show_title();
        update_port_caption(true);
        return;
    }
    case View::Port:
        if (finder_) { /* Esc stops the detection; the page stays */
            finder_->cancel();
            return;
        }
        nav_.back();
        show_ports(port_.id, false);
        show_title();
        return;
    case View::Ports: cdk::request_quit(0); return; /* a short Esc at the root ends the app (report 024) */
    }
}

/* ------------------------------------------------------------------ the ports page */

void SerialApp::scan()
{
    if (nav_.current() == View::Terminal) return; /* the session follows its own port */
    if (nav_.current() == View::Ports && dialog_) return; /* the rows under a dialog stay as they are */
    std::vector<PortInfo> all = scan_ports(roots_, true);
    // the port page's port: still there? (it may come back on another tty number: its path follows)
    if (nav_.current() == View::Port || nav_.current() == View::Choice) {
        bool present = false;
        for (const PortInfo &p : all)
            if (p.id == port_.id) {
                present = true;
                port_.path = p.path;
                port_.tty = p.tty;
            }
        if (present != port_present_) {
            port_present_ = present;
            if (nav_.current() == View::Port) update_port_caption(true);
        }
    }
    std::vector<PortInfo> found;
    for (PortInfo &p : all)
        if (!p.onboard || config_.show_onboard) found.push_back(std::move(p));
    if (nav_.current() != View::Ports) {
        ports_ = std::move(found);
        list_.clear(); /* show_ports() builds the rows again when the page comes back */
        return;
    }
    bool same_list = found.size() == ports_.size();
    for (std::size_t i = 0; same_list && i < found.size(); ++i) same_list = found[i].id == ports_[i].id;
    if (!same_list) { /* plugged or unplugged: the rows again, the highlight stays on its port */
        std::string selected;
        const int sel = roller_->selected();
        if (sel >= 0 && sel < static_cast<int>(list_.size()) && list_[static_cast<std::size_t>(sel)].kind == ListRow::Port)
            selected = ports_[static_cast<std::size_t>(list_[static_cast<std::size_t>(sel)].port)].id;
        const bool on_toggle = sel >= 0 && sel < static_cast<int>(list_.size()) &&
                               list_[static_cast<std::size_t>(sel)].kind == ListRow::Onboard;
        if (selected.empty()) /* the highlight was not on a port: it goes to a port just plugged in */
            for (const PortInfo &p : found) {
                const bool is_new = std::none_of(ports_.begin(), ports_.end(),
                                                 [&p](const PortInfo &old) { return old.id == p.id; });
                if (is_new && !p.onboard) {
                    selected = p.id;
                    break;
                }
            }
        ports_ = std::move(found);
        show_ports(selected, on_toggle && selected.empty());
        return;
    }
    for (std::size_t i = 0; i < found.size(); ++i) { /* only the rows that changed */
        if (same_port(found[i], ports_[i])) continue;
        ports_[i] = found[i];
        cp0::ui::RowSpec spec;
        spec.kind = cp0::ui::RowKind::Nav;
        spec.label = port_row_text(ports_[i]).first;
        spec.value = port_row_text(ports_[i]).second;
        roller_->update_row(static_cast<int>(i), spec);
        caption_row_ = -2;
    }
}

std::vector<cp0::ui::RowSpec> SerialApp::ports_rows()
{
    list_.clear();
    std::vector<cp0::ui::RowSpec> rows;
    for (std::size_t i = 0; i < ports_.size(); ++i) {
        ListRow lr;
        lr.kind = ListRow::Port;
        lr.port = static_cast<int>(i);
        list_.push_back(lr);
        cp0::ui::RowSpec spec;
        spec.kind = cp0::ui::RowKind::Nav;
        spec.label = port_row_text(ports_[i]).first;
        spec.value = port_row_text(ports_[i]).second;
        rows.push_back(spec);
    }
    ListRow toggle;
    toggle.kind = ListRow::Onboard;
    list_.push_back(toggle);
    cp0::ui::RowSpec onboard;
    onboard.kind = cp0::ui::RowKind::Toggle;
    onboard.label = "Show on-board UARTs";
    onboard.on = config_.show_onboard;
    rows.push_back(onboard);
    if (!group_ok(groups())) {
        ListRow access_row;
        access_row.kind = ListRow::Access;
        list_.push_back(access_row);
        cp0::ui::RowSpec fix;
        fix.kind = cp0::ui::RowKind::Nav;
        fix.label = "Serial port access";
        fix.value = "How to fix";
        fix.value_rgb = kErrorRgb;
        rows.push_back(fix);
    }
    return rows;
}

void SerialApp::show_ports(const std::string &select_id, bool select_toggle)
{
    const std::vector<cp0::ui::RowSpec> rows = ports_rows();
    int select = 0;
    for (std::size_t i = 0; i < list_.size(); ++i) {
        const ListRow &lr = list_[i];
        if (select_toggle && lr.kind == ListRow::Onboard) select = static_cast<int>(i);
        if (!select_toggle && lr.kind == ListRow::Port && ports_[static_cast<std::size_t>(lr.port)].id == select_id)
            select = static_cast<int>(i);
    }
    roller_->set_rows(rows, select, true);
    roller_->set_hidden(false);
    caption_row_ = -2;
    update_ports_caption(true);
}

void SerialApp::update_ports_caption(bool force)
{
    const int sel = roller_->selected();
    if (!force && sel == caption_row_) return;
    caption_row_ = sel;
    const bool valid = sel >= 0 && sel < static_cast<int>(list_.size());
    if (valid && list_[static_cast<std::size_t>(sel)].kind == ListRow::Port) {
        const PortInfo &p = ports_[static_cast<std::size_t>(list_[static_cast<std::size_t>(sel)].port)];
        page_->set_caption(port_caption(p, config_.settings(p.id)), cp0::ui::CaptionTone::Normal);
    } else {
        page_->set_caption("", cp0::ui::CaptionTone::Normal);
    }
    // the empty list's card, while nothing else is over the centre row's upper half
    show_card(ports_.empty() && valid && list_[static_cast<std::size_t>(sel)].kind == ListRow::Onboard);
}

/* The empty list: a card above the centre row, "No serial adapter found." and what to do, at the BODY size (the
 * CAPTION size when that does not fit). */
void SerialApp::show_card(bool show)
{
    if (!show) {
        if (card_) lv_obj_add_flag(card_, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    if (!card_) {
        const int s = cp0_ui_tok(CP0_TOK_SPACE_S);
        const int pad = layout_.gap;
        const int room = layout_.edge_pad - 2 * layout_.gap; /* above the centre row, a gap each side */
        card_ = lv_obj_create(page_->root());
        lv_obj_remove_style_all(card_);
        lv_obj_remove_flag(card_, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_remove_flag(card_, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_bg_color(card_, lv_color_hex(kCardBg), 0);
        lv_obj_set_style_bg_opa(card_, LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(card_, lv_color_hex(kCardBorder), 0);
        lv_obj_set_style_border_width(card_, std::max(1, s / 4), 0);
        lv_obj_set_style_radius(card_, cp0_ui_tok(CP0_TOK_RADIUS_M), 0);
        lv_obj_set_style_pad_all(card_, pad, 0);
        lv_obj_set_style_pad_row(card_, s, 0);
        lv_obj_set_flex_flow(card_, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_width(card_, layout_.roller_w);
        lv_obj_set_height(card_, LV_SIZE_CONTENT);
        lv_obj_t *title = lv_label_create(card_);
        lv_label_set_text(title, kEmptyTitle);
        lv_obj_set_style_text_color(title, lv_color_hex(kCardTitle), 0);
        lv_obj_t *text = lv_label_create(card_);
        lv_label_set_text(text, kEmptyText);
        lv_label_set_long_mode(text, LV_LABEL_LONG_MODE_WRAP);
        lv_obj_set_width(text, layout_.roller_w - 2 * pad);
        lv_obj_set_style_text_color(text, lv_color_hex(kCardText), 0);
        for (const cp0_ui_text_t size : {CP0_TEXT_BODY, CP0_TEXT_CAPTION}) {
            lv_obj_set_style_text_font(title, cp0_ui_font(size), 0);
            lv_obj_set_style_text_font(text, cp0_ui_font(size), 0);
            lv_obj_update_layout(card_);
            if (lv_obj_get_height(card_) <= room) break;
        }
        lv_obj_set_pos(card_, layout_.roller_x, layout_.body_y + layout_.gap);
    }
    lv_obj_remove_flag(card_, LV_OBJ_FLAG_HIDDEN);
}

void SerialApp::activate_ports(int row)
{
    if (row < 0 || row >= static_cast<int>(list_.size())) return;
    const ListRow lr = list_[static_cast<std::size_t>(row)];
    switch (lr.kind) {
    case ListRow::Port: open_port_page(ports_[static_cast<std::size_t>(lr.port)]); break;
    case ListRow::Onboard: {
        config_.show_onboard = !config_.show_onboard;
        save_config(config_path_, config_);
        std::vector<PortInfo> found;
        for (PortInfo &p : scan_ports(roots_, true))
            if (!p.onboard || config_.show_onboard) found.push_back(std::move(p));
        ports_ = std::move(found);
        show_ports(std::string(), true);
        break;
    }
    case ListRow::Access: show_problem(group_problem(groups())); break;
    }
}

/* ------------------------------------------------------------------ the port page */

void SerialApp::open_port_page(const PortInfo &port)
{
    port_ = port;
    port_present_ = true;
    settings_ = config_.settings(port.id);
    find_value_.clear();
    find_rgb_ = -1;
    port_note_.clear();
    show_card(false);
    nav_.open(View::Port);
    show_port_rows(-1);
    show_port_rows(port_row_index(PortRow::Connect));
    show_title();
    update_port_caption(true);
}

std::vector<cp0::ui::RowSpec> SerialApp::port_rows()
{
    port_map_.clear();
    std::vector<cp0::ui::RowSpec> rows;
    const auto add = [&](PortRow which, cp0::ui::RowSpec spec) {
        port_map_.push_back(which);
        rows.push_back(std::move(spec));
    };
    for (const auto &kv : port_info_items(port_, can_open(port_) && group_ok(groups()))) {
        cp0::ui::RowSpec spec;
        spec.kind = cp0::ui::RowKind::Info;
        spec.label = kv.first;
        spec.value = kv.second;
        add(PortRow::Info, spec);
    }
    const int baud = effective_baud(settings_, recommended());
    cp0::ui::RowSpec spec;
    spec.kind = cp0::ui::RowKind::Nav;
    spec.label = "Speed";
    spec.value = speed_value(baud, recommended());
    add(PortRow::Speed, spec);
    spec.label = "Frame";
    spec.value = settings_.frame;
    add(PortRow::Frame, spec);
    cp0::ui::RowSpec echo;
    echo.kind = cp0::ui::RowKind::Toggle;
    echo.label = "Local echo";
    echo.on = settings_.echo;
    add(PortRow::Echo, echo);
    spec.label = "Enter sends";
    spec.value = ending_name(settings_.ending);
    add(PortRow::Ending, spec);
    spec.label = "Backspace sends";
    spec.value = backspace_name(settings_.backspace);
    add(PortRow::Backspace, spec);
    cp0::ui::RowSpec log = echo;
    log.label = "Log to file";
    log.on = settings_.log;
    add(PortRow::Log, log);
    cp0::ui::RowSpec find;
    if (finder_) {
        find.kind = cp0::ui::RowKind::Busy;
        find.label = finder_shown_;
    } else {
        find.kind = cp0::ui::RowKind::Nav;
        find.label = "Find the speed";
        find.value = find_value_;
        find.value_rgb = find_rgb_;
    }
    add(PortRow::Find, find);
    cp0::ui::RowSpec enter = echo;
    enter.label = "Send Enter while finding";
    enter.on = settings_.detect_enter;
    add(PortRow::FindEnter, enter);
    spec.label = "Connect";
    spec.value = std::to_string(baud) + " " + settings_.frame;
    add(PortRow::Connect, spec);
    return rows;
}

int SerialApp::port_row_index(PortRow which) const
{
    for (std::size_t i = 0; i < port_map_.size(); ++i)
        if (port_map_[i] == which) return static_cast<int>(i);
    return 0;
}

/* select < 0: the rows are only mapped (port_map_), nothing is shown */
void SerialApp::show_port_rows(int select)
{
    const std::vector<cp0::ui::RowSpec> rows = port_rows();
    if (select < 0) return;
    roller_->set_rows(rows, select, true);
    roller_->set_hidden(false);
}

void SerialApp::update_port_row(PortRow which)
{
    const std::vector<cp0::ui::RowSpec> rows = port_rows();
    const int index = port_row_index(which);
    roller_->update_row(index, rows[static_cast<std::size_t>(index)]);
    if (which == PortRow::Speed || which == PortRow::Frame) { /* Connect shows them too */
        const int connect = port_row_index(PortRow::Connect);
        roller_->update_row(connect, rows[static_cast<std::size_t>(connect)]);
    }
}

/* The port page's status line: why the recommended settings, or the result of Find the speed. It shows while the
 * highlight is on a setting (not over the "What is connected" rows, which it would cover); a port that went away is
 * said whatever the highlight. */
void SerialApp::update_port_caption(bool force)
{
    const int sel = roller_->selected();
    if (!force && sel == caption_row_) return;
    caption_row_ = sel;
    if (!port_present_) {
        page_->set_caption(port_.tty + " is unplugged: plug it in again", cp0::ui::CaptionTone::Warn);
        return;
    }
    const bool info = sel >= 0 && sel < static_cast<int>(port_map_.size()) &&
                      port_map_[static_cast<std::size_t>(sel)] == PortRow::Info;
    if (info) page_->set_caption("", cp0::ui::CaptionTone::Normal);
    else if (finder_) page_->set_caption("Esc stops", cp0::ui::CaptionTone::Normal);
    else if (!port_note_.empty())
        page_->set_caption(port_note_, port_note_warn_ ? cp0::ui::CaptionTone::Warn : cp0::ui::CaptionTone::Normal);
    else page_->set_caption(port_.rec.reason, cp0::ui::CaptionTone::Normal);
}

void SerialApp::save_port()
{
    config_.set(port_.id, settings_);
    save_config(config_path_, config_);
}

void SerialApp::activate_port(int row)
{
    if (row < 0 || row >= static_cast<int>(port_map_.size())) return;
    const PortRow which = port_map_[static_cast<std::size_t>(row)];
    if (finder_) { /* while it listens, only its own row (stop) */
        if (which == PortRow::Find) finder_->cancel();
        return;
    }
    switch (which) {
    case PortRow::Info: break;
    case PortRow::Speed: open_choice(Choice::Speed); break;
    case PortRow::Frame: open_choice(Choice::Frame); break;
    case PortRow::Ending: open_choice(Choice::Ending); break;
    case PortRow::Backspace: open_choice(Choice::Backspace); break;
    case PortRow::Echo:
        settings_.echo = !settings_.echo;
        save_port();
        update_port_row(which);
        break;
    case PortRow::Log:
        settings_.log = !settings_.log;
        save_port();
        update_port_row(which);
        break;
    case PortRow::FindEnter:
        settings_.detect_enter = !settings_.detect_enter;
        save_port();
        update_port_row(which);
        break;
    case PortRow::Find: start_finder(); break;
    case PortRow::Connect: connect(); break;
    }
}

void SerialApp::start_finder()
{
    if (!port_present_) {
        show_problem(open_problem(ENOENT, groups(), port_.tty));
        return;
    }
    Frame frame;
    parse_frame(settings_.frame, frame);
    finder_ = std::make_unique<SpeedFinder>(port_.path, frame, recommended(), settings_.detect_enter);
    finder_->start();
    finder_shown_ = finder_->progress().text;
    port_note_.clear();
    show_port_rows(port_row_index(PortRow::Find)); /* the row turns Busy: its kind changes, so all rows again */
    update_port_caption(true);
}

void SerialApp::check_finder()
{
    if (!finder_) return;
    const FinderProgress p = finder_->progress();
    if (!p.done) {
        if (p.text != finder_shown_ && nav_.current() == View::Port) {
            finder_shown_ = p.text;
            const int index = port_row_index(PortRow::Find);
            cp0::ui::RowSpec spec;
            spec.kind = cp0::ui::RowKind::Busy;
            spec.label = finder_shown_;
            roller_->update_row(index, spec);
        }
        return;
    }
    finder_.reset();
    find_rgb_ = -1;
    port_note_warn_ = false;
    port_note_.clear();
    Problem notice;
    if (p.error && p.error != ENODEV) {
        find_value_ = "Failed";
        find_rgb_ = kErrorRgb;
        notice = open_problem(p.error, groups(), port_.tty);
    } else if (p.error == ENODEV) {
        find_value_ = "Port went away";
        find_rgb_ = kErrorRgb;
        port_note_ = port_.tty + " went away while listening";
        port_note_warn_ = true;
    } else if (p.result.found) {
        settings_.baud = p.result.baud == recommended() ? 0 : p.result.baud;
        save_port();
        find_value_ = "Found " + std::to_string(p.result.baud);
        port_note_ = p.result.message;
    } else if (p.result.message == "Stopped") {
        find_value_ = "Stopped";
    } else { /* the advice is a sentence too long for the status line: a notice */
        find_value_ = "No text found";
        find_rgb_ = kErrorRgb;
        notice.title = "No readable text";
        notice.text = p.result.message;
    }
    if (nav_.current() == View::Port) {
        show_port_rows(port_row_index(PortRow::Find));
        update_port_caption(true);
    }
    if (!notice.title.empty()) show_problem(notice);
}

/* ------------------------------------------------------------------ value lists */

void SerialApp::open_choice(Choice which)
{
    choice_ = which;
    choice_values_.clear();
    std::vector<cp0::ui::RowSpec> rows;
    int current = 0;
    const auto add = [&](const std::string &value, const std::string &label, bool on) {
        if (on) current = static_cast<int>(rows.size());
        choice_values_.push_back(value);
        cp0::ui::RowSpec spec;
        spec.kind = cp0::ui::RowKind::Choice;
        spec.label = label;
        spec.on = on;
        rows.push_back(spec);
    };
    switch (which) {
    case Choice::Speed: {
        const int baud = effective_baud(settings_, recommended());
        std::vector<int> speeds = speed_list();
        if (std::find(speeds.begin(), speeds.end(), baud) == speeds.end()) speeds.push_back(baud);
        if (std::find(speeds.begin(), speeds.end(), recommended()) == speeds.end()) speeds.push_back(recommended());
        std::sort(speeds.begin(), speeds.end());
        for (const int s : speeds) add(std::to_string(s), speed_value(s, recommended()), s == baud);
        break;
    }
    case Choice::Frame:
        for (const std::string &f : frame_list())
            add(f, f == port_.rec.frame ? f + " (recommended)" : f, f == settings_.frame);
        break;
    case Choice::Ending:
        for (const Ending e : {Ending::CR, Ending::LF, Ending::CRLF})
            add(std::to_string(static_cast<int>(e)), ending_name(e), e == settings_.ending);
        break;
    case Choice::Backspace:
        for (const Backspace b : {Backspace::Del, Backspace::Bs})
            add(std::to_string(static_cast<int>(b)), backspace_name(b), b == settings_.backspace);
        break;
    }
    nav_.open(View::Choice);
    roller_->set_rows(rows, current, true);
    page_->set_caption("", cp0::ui::CaptionTone::Normal);
    show_title();
}

void SerialApp::activate_choice(int row)
{
    if (row < 0 || row >= static_cast<int>(choice_values_.size())) return;
    const std::string &value = choice_values_[static_cast<std::size_t>(row)];
    switch (choice_) {
    case Choice::Speed: {
        const int baud = std::atoi(value.c_str());
        settings_.baud = baud == recommended() ? 0 : baud; /* the recommended speed follows the device */
        break;
    }
    case Choice::Frame: settings_.frame = value; break;
    case Choice::Ending: settings_.ending = static_cast<Ending>(std::atoi(value.c_str())); break;
    case Choice::Backspace: settings_.backspace = static_cast<Backspace>(std::atoi(value.c_str())); break;
    }
    save_port();
    back(); /* to the port page, on the row just changed */
}

/* ------------------------------------------------------------------ the terminal */

void SerialApp::connect()
{
    config_.last_port = port_.id;
    save_port();
    TerminalSpec spec;
    spec.port = port_;
    spec.settings = settings_;
    spec.baud = effective_baud(settings_, recommended());
    spec.roots = roots_;
    spec.log_dir = log_dir_;
    spec.leave = [this] {
        post([this] {
            if (nav_.current() == View::Terminal && !dialog_) close_terminal();
        });
    };
    spec.failed = [this](int err) {
        post([this, err] {
            if (nav_.current() == View::Terminal) close_terminal();
            show_problem(open_problem(err, groups(), port_.tty));
        });
    };
    nav_.open(View::Terminal);
    roller_->set_hidden(true);
    page_->set_caption("", cp0::ui::CaptionTone::Normal);
    term_ = std::make_unique<TerminalView>(*page_, layout_, std::move(spec)); /* it sets the page title */
    focus(term_->focus_object());
}

void SerialApp::close_terminal()
{
    term_.reset();
    nav_.back();
    roller_->set_hidden(false);
    show_title();
    update_port_caption(true);
    next_scan_ = 0; /* the port may have gone meanwhile */
    focus(roller_->focus_object());
}

/* ------------------------------------------------------------------ dialogs */

void SerialApp::show_problem(const Problem &problem)
{
    dialog_.reset();
    cp0::ui::DialogSpec spec;
    spec.title = problem.title;
    spec.page = page_.get();
    spec.buttons.push_back({"OK", cp0::ui::ButtonRole::Primary, [this] { post([this] { close_dialog(); }); }});
    dialog_ = std::make_unique<cp0::ui::Dialog>(page_->root(), layout_, std::move(spec));
    dialog_->add_text(problem.text);
    dialog_->relayout();
    focus(dialog_->focus_object());
}

void SerialApp::close_dialog()
{
    if (!dialog_) return;
    dialog_.reset();
    focus(term_ ? term_->focus_object() : roller_->focus_object());
}

} // namespace sc
