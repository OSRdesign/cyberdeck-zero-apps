/*
 * SPDX-License-Identifier: MIT
 *
 * Serial Console: the app's screen (see serial_app.hpp). Layout from the layout service only (the roller page, the
 * tokens and the text sizes); the logic is in core/.
 */
#include "serial_app.hpp"

#include "app.hpp"

#include "cp0_font_service.hpp"
#include "cp0_ui_metrics_lvgl.h"

#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cctype>
#include <cstdlib>
#include <utility>

namespace sc {
namespace {

constexpr int kTickMs = 50;
constexpr int kScanMs = 1000;
constexpr int kBandLines = 2;                    /* the status band: two lines of CAPTION text */
constexpr int kErrorRgb = 0xFF6666;              /* the shell error colour (failed values) */
constexpr std::uint32_t kBandNormal = 0x66CC88;  /* the shell's status line colours (cp0::ui::Page's) */
constexpr std::uint32_t kBandWarn = 0xF0B400;
constexpr std::uint32_t kBandError = 0xFF6666;
constexpr std::uint32_t kCardBg = 0x1C1C1C;     /* the text fields' colours */
constexpr std::uint32_t kCardBorder = 0x3A3A3A;
constexpr std::uint32_t kCardTitle = 0xFFFFFF;
constexpr std::uint32_t kCardText = 0xE0E0E0;
/* The lists' column: the full width less the deck's side margin, not the roller's narrow centred column (capped at
 * 560 px, so 120 px margins at 800 wide). 640x480 keeps the roller's own margin, 40 px. Wider than 960 px the column
 * stops at 960, centred: a label and its value stay within one glance. */
constexpr int kListMarginPx = 40;
constexpr int kListCapW = 960;

const char *choice_title(int which)
{
    switch (which) {
    case 0: return "Speed";
    case 1: return "Frame";
    case 2: return "Enter sends";
    default: return "Backspace sends";
    }
}

int tw(const std::string &text, const lv_font_t *font)
{
    if (text.empty() || !font) return 0;
    lv_point_t size{};
    lv_text_get_size(&size, text.c_str(), font, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    return size.x;
}

/* The text broken at spaces into lines of at most `room` px as `width` measures them (a word wider than the room
 * stays whole on its line). */
std::vector<std::string> wrap_lines(const std::string &text, const std::function<int(const std::string &)> &width,
                                    int room)
{
    std::vector<std::string> lines;
    std::string line;
    std::size_t pos = 0;
    while (pos < text.size()) {
        std::size_t end = text.find(' ', pos);
        if (end == std::string::npos) end = text.size();
        const std::string word = text.substr(pos, end - pos);
        const std::string joined = line.empty() ? word : line + " " + word;
        if (!line.empty() && width(joined) > room) {
            lines.push_back(line);
            line = word;
        } else {
            line = joined;
        }
        pos = end + 1;
    }
    if (!line.empty()) lines.push_back(line);
    return lines;
}

cp0::ui::ValueTone tone_of(bool bad) { return bad ? cp0::ui::ValueTone::Bad : cp0::ui::ValueTone::Normal; }

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

    // the column, the status band (two lines) and the list under it, for both densities
    const int s = cp0_ui_tok(CP0_TOK_SPACE_S);
    const int margin = std::min(layout_.roller_x, kListMarginPx);
    const int col_w = std::max(1, std::min(kListCapW, layout_.w - 2 * margin));
    const int col_x = (layout_.w - col_w) / 2;
    const lv_font_t *band_font = cp0_ui_font_px(layout_.caption_px);
    const int line_h = band_font ? lv_font_get_line_height(band_font) : layout_.caption_px;
    const int band_y = layout_.body_y + s;
    const int list_y = band_y + kBandLines * line_h + s;
    const int list_h = layout_.body_y + layout_.body_h - list_y;
    for (const Density d : {Density::Compact, Density::Comfortable}) {
        const int i = static_cast<int>(d);
        cp0_ui_list_layout(cp0_ui_metrics_get(), static_cast<cp0_ui_list_density_t>(i), col_x, list_y, col_w, list_h,
                           &lay_[i]);
        note_font_[i] = cp0_fonts().get("Montserrat-Medium.ttf", static_cast<uint16_t>(lay_[i].section_px));
        if (!note_font_[i]) note_font_[i] = cp0_ui_font_px(lay_[i].section_px);
        label_font_[i] = cp0_fonts().get("Montserrat-Bold.ttf", static_cast<uint16_t>(lay_[i].text_px));
        value_font_[i] = cp0_fonts().get("Montserrat-Medium.ttf", static_cast<uint16_t>(lay_[i].value_px));
        if (!label_font_[i]) label_font_[i] = cp0_ui_font_px(lay_[i].text_px);
        if (!value_font_[i]) value_font_[i] = cp0_ui_font_px(lay_[i].value_px);
    }
    band_ = lv_label_create(page_->root());
    lv_obj_remove_flag(band_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(band_, LV_OBJ_FLAG_CLICKABLE);
    lv_label_set_long_mode(band_, LV_LABEL_LONG_MODE_DOTS); /* wraps in the height of its lines, dots after the last */
    if (band_font) lv_obj_set_style_text_font(band_, band_font, 0);
    lv_obj_set_style_text_color(band_, lv_color_hex(kBandNormal), 0);
    lv_obj_set_pos(band_, col_x + lay_[0].inset, band_y);
    lv_obj_set_size(band_, std::max(1, col_w - 2 * lay_[0].inset), kBandLines * line_h);
    lv_label_set_text(band_, "");

    {
        std::vector<PortInfo> all = scan_all(true);
        boots_ = scan_boot_drives(roots_, all);
        for (PortInfo &p : all)
            if (!p.onboard || config_.show_onboard) ports_.push_back(std::move(p));
    }
    next_scan_ = now_ms() + kScanMs;
    show_ports(config_.last_port, false);
    show_title();
    timer_ = lv_timer_create(tick_cb, kTickMs, this);
}

SerialApp::~SerialApp()
{
    if (timer_) lv_timer_delete(timer_);
    lv_async_call_cancel(posted_cb, this);
    term_.reset();   /* the connection closed, its worker joined */
    finder_.reset();
    prober_.reset(); /* a probe stopped, its lines put back */
    detail_.reset();
    dialog_.reset();
    list_.reset();
    roller_.reset();
    page_.reset();   /* the card and the band go with the page */
    card_ = nullptr;
    band_ = nullptr;
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
    check_probe();
    const std::int64_t now = now_ms();
    if (now >= next_scan_) {
        next_scan_ = now + kScanMs;
        scan();
    }
    if (nav_.current() == View::Ports && !dialog_) update_ports_caption(false);
    if (nav_.current() == View::Port && !dialog_) update_port_caption(false);
    if (nav_.current() == View::Ask && !dialog_) update_ask_caption(false);
    if (nav_.current() == View::Card && !dialog_) update_card_caption(false);
}

void SerialApp::focus(lv_obj_t *obj)
{
    lv_group_t *group = cdk::input_group();
    if (!group || !obj) return;
    if (lv_obj_get_group(obj) != group) lv_group_add_obj(group, obj);
    lv_group_focus_obj(obj);
}

lv_obj_t *SerialApp::focus_target() const
{
    if (term_) return term_->focus_object();
    if (detail_) return detail_->focus_object();
    if (list_) return list_->focus_object();
    if (roller_) return roller_->focus_object();
    return nullptr;
}

/* The list and its status band belong to the four list pages (Ports, Port, Ask, Card): the Choice page has its
 * roller, the terminal and the detail page their own views. A dialog hides the band (the page under it gives way). */
void SerialApp::update_chrome()
{
    const View view = nav_.current();
    const bool pages = view == View::Ports || view == View::Port || view == View::Ask || view == View::Card;
    if (list_) {
        list_->set_hidden(!pages);
    }
    if (band_) {
        if (pages && !dialog_) lv_obj_remove_flag(band_, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(band_, LV_OBJ_FLAG_HIDDEN);
    }
}

/* The status band: what the highlighted row means (two lines, dots after them). */
void SerialApp::say(const std::string &text, cp0::ui::CaptionTone tone)
{
    if (!band_) return;
    lv_label_set_text(band_, text.c_str());
    const std::uint32_t rgb = tone == cp0::ui::CaptionTone::Error  ? kBandError
                              : tone == cp0::ui::CaptionTone::Warn ? kBandWarn
                                                                   : kBandNormal;
    lv_obj_set_style_text_color(band_, lv_color_hex(rgb), 0);
}

/* The list of the page the app is on, in the density that suits it (the same column and top for both): one list
 * serves the pages of a density, a pick-one Roller serves the Choice page. */
void SerialApp::build_list(Density density, bool wrap)
{
    roller_.reset();
    list_.reset();
    cp0::ui::CompactListEvents events;
    events.activate = [this](int index, std::intptr_t) {
        post([this, index] {
            if (dialog_) return;
            dispatch_activate(index);
        });
    };
    events.back = [this] { post([this] { back(); }); };
    cp0::ui::CompactListOptions options;
    options.wrap = wrap;
    list_ = std::make_unique<cp0::ui::CompactList>(page_->root(), lay_[static_cast<int>(density)], std::move(events),
                                                    options);
    list_density_ = density;
    list_wrap_ = wrap;
    update_chrome();
    if (!dialog_) focus(list_->focus_object());
}

void SerialApp::ensure_list(Density density, bool wrap)
{
    if (!list_ || list_density_ != density || list_wrap_ != wrap) build_list(density, wrap);
}

void SerialApp::make_roller()
{
    list_.reset();
    cp0::ui::RollerEvents events;
    events.activate = [this](int row) {
        post([this, row] {
            if (dialog_) return;
            if (nav_.current() == View::Choice) activate_choice(row);
        });
    };
    events.back = [this] { post([this] { back(); }); };
    roller_ = std::make_unique<cp0::ui::Roller>(page_->root(), layout_, std::move(events));
    if (!dialog_) focus(roller_->focus_object());
}

void SerialApp::dispatch_activate(int row)
{
    switch (nav_.current()) {
    case View::Ports: activate_ports(row); break;
    case View::Port: activate_port(row); break;
    case View::Choice: activate_choice(row); break;
    case View::Ask: activate_ask(row); break;
    case View::Card: activate_card(row); break;
    case View::Terminal:
    case View::Detail: break;
    }
}

/* A row for the lists. A value too wide for its row is cut with dots (the label keeps at least a third of the row),
 * and the whole "label: value" goes to `full` for the status band. A label with no value is cut by the list, or, with
 * `note`, is a note: section rows (grey capitals, never selected), as many as its text needs. `full` gets one entry
 * per row added. */
void SerialApp::add_rows(std::vector<cp0::ui::ListRow> &rows, std::vector<std::string> &full, Density density,
                         const std::string &label, const std::string &value, cp0::ui::ValueTone tone, bool note,
                         cp0::ui::RowAffordance aff) const
{
    const int i = static_cast<int>(density);
    const cp0_ui_list_layout_t &l = lay_[i];
    const int room = std::max(1, l.w - 2 * l.inset);
    if (value.empty()) {
        if (note) {
            const auto width = [this, i](const std::string &t) { /* the list draws sections in capitals, 1 px apart */
                std::string up = t;
                for (char &c : up) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
                return tw(up, note_font_[i]) + static_cast<int>(up.size());
            };
            for (const std::string &line : wrap_lines(label, width, room)) {
                rows.push_back(cp0::ui::ListRow::section(line));
                full.emplace_back();
            }
        } else {
            rows.push_back(cp0::ui::ListRow::item(label, std::string(), cp0::ui::ValueTone::Normal, 0, aff));
            full.push_back(tw(label, label_font_[i]) <= room ? std::string() : label);
        }
        return;
    }
    const int label_w = tw(label, label_font_[i]);
    const int value_w = tw(value, value_font_[i]);
    std::string shown = value;
    std::string whole;
    if (label_w + l.gap + value_w > room) {
        const int most = std::min(value_w, std::max(room - l.gap - label_w, room * 2 / 3));
        if (value_w > most) shown = fit_text(value, value_font_[i], most);
        whole = label + ": " + value;
    }
    rows.push_back(cp0::ui::ListRow::item(label, shown, tone, 0, aff));
    full.push_back(whole);
}

/* The text cut (at a UTF-8 character) with "..." to fit `room` px. */
std::string SerialApp::fit_text(const std::string &text, const lv_font_t *font, int room) const
{
    if (tw(text, font) <= room) return text;
    std::string cut = text;
    while (!cut.empty()) {
        do cut.pop_back();
        while (!cut.empty() && (static_cast<unsigned char>(cut.back()) & 0xC0) == 0x80);
        if (tw(cut + "...", font) <= room) break;
    }
    return cut + "...";
}

void SerialApp::show_title()
{
    switch (nav_.current()) {
    case View::Ports: page_->set_title({kAppTitle}); break;
    case View::Port: page_->set_title({kAppTitle, port_.tty}); break;
    case View::Choice: page_->set_title({kAppTitle, port_.tty, choice_title(static_cast<int>(choice_))}); break;
    case View::Ask: page_->set_title({kAppTitle, port_.tty, "Ask"}); break;
    case View::Card: page_->set_title({kAppTitle, port_.tty, "Behind it"}); break;
    case View::Detail: break; /* open_detail() sets it */
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

/* Every port, with the recommendation of what was identified behind it. */
std::vector<PortInfo> SerialApp::scan_all(bool with_onboard)
{
    std::vector<PortInfo> all = scan_ports(roots_, with_onboard);
    for (PortInfo &p : all) p.rec = identified_recommendation(p.rec, config_.identity(p.id));
    return all;
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
        roller_.reset();
        const PortRow rows[] = {PortRow::Speed, PortRow::Frame, PortRow::Ending, PortRow::Backspace};
        show_port_rows(port_row_index(rows[static_cast<int>(choice_)]));
        show_title();
        update_port_caption(true);
        return;
    }
    case View::Detail: close_detail(); return;
    case View::Card: close_card(); return;
    case View::Ask:
        if (prober_) { /* Esc stops the probe; the page stays */
            prober_->cancel();
            return;
        }
        nav_.back();
        show_port_rows(port_row_index(PortRow::IdAsk));
        show_title();
        update_port_caption(true);
        return;
    case View::Port:
        if (finder_) { /* Esc stops the detection; the page stays */
            finder_->cancel();
            return;
        }
        if (prober_) {
            prober_->cancel();
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
    std::vector<PortInfo> all = scan_all(true);
    std::vector<BootDrive> boots = scan_boot_drives(roots_, all);
    // the port page's port: still there? (it may come back on another tty number: its path follows)
    if (nav_.current() != View::Ports) {
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
        boots_ = std::move(boots);
        ports_map_.clear(); /* show_ports() builds the rows again when the page comes back */
        return;
    }
    bool same_list = found.size() == ports_.size() && boots.size() == boots_.size();
    for (std::size_t i = 0; same_list && i < found.size(); ++i) same_list = found[i].id == ports_[i].id;
    for (std::size_t i = 0; same_list && i < boots.size(); ++i) same_list = boots[i].usb_dir == boots_[i].usb_dir;
    if (!same_list) { /* plugged or unplugged: the rows again, the highlight stays on its port */
        std::string selected;
        const int sel_row = sel();
        if (sel_row >= 0 && sel_row < static_cast<int>(ports_map_.size()) &&
            ports_map_[static_cast<std::size_t>(sel_row)].kind == PortsRow::Port)
            selected = ports_[static_cast<std::size_t>(ports_map_[static_cast<std::size_t>(sel_row)].port)].id;
        const bool on_toggle = sel_row >= 0 && sel_row < static_cast<int>(ports_map_.size()) &&
                               ports_map_[static_cast<std::size_t>(sel_row)].kind == PortsRow::Onboard;
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
        boots_ = std::move(boots);
        show_ports(selected, on_toggle && selected.empty(), true);
        return;
    }
    bool changed = false;
    for (std::size_t i = 0; i < found.size(); ++i) { /* only the rows that changed */
        if (same_port(found[i], ports_[i])) continue;
        ports_[i] = found[i];
        changed = true;
    }
    if (changed && list_) {
        list_->set_rows(ports_rows()); /* the list touches only the texts that changed */
        caption_row_ = -2;
    }
}

std::vector<cp0::ui::ListRow> SerialApp::ports_rows()
{
    ports_map_.clear();
    row_full_.clear();
    std::vector<cp0::ui::ListRow> rows;
    const auto mapped = [this, &rows](PortsRow::Kind kind, int port) {
        while (ports_map_.size() < rows.size()) {
            PortsRow lr;
            lr.kind = kind;
            lr.port = port;
            ports_map_.push_back(lr);
        }
    };
    for (std::size_t i = 0; i < ports_.size(); ++i) {
        add_rows(rows, row_full_, Density::Comfortable, port_row_text(ports_[i]).first, port_row_text(ports_[i]).second,
                 cp0::ui::ValueTone::Normal, false, Aff::Open);
        mapped(PortsRow::Port, static_cast<int>(i));
    }
    for (std::size_t i = 0; i < boots_.size(); ++i) { /* a board in its USB boot loader: a drive, not a port */
        add_rows(rows, row_full_, Density::Comfortable, boots_[i].mode.title, std::string(), cp0::ui::ValueTone::Normal,
                 false, Aff::Info); /* the status line says it is a USB drive; a tap only explains */
        mapped(PortsRow::Boot, static_cast<int>(i));
    }
    rows.push_back(cp0::ui::ListRow::item("Show on-board UARTs", config_.show_onboard ? "On" : "Off",
                                          config_.show_onboard ? cp0::ui::ValueTone::Ok : cp0::ui::ValueTone::Normal,
                                          0, Aff::Edit));
    row_full_.emplace_back();
    mapped(PortsRow::Onboard, -1);
    if (!group_ok(groups())) {
        rows.push_back(cp0::ui::ListRow::item("Serial port access", "How to fix", cp0::ui::ValueTone::Bad, 0, Aff::Open));
        row_full_.emplace_back();
        mapped(PortsRow::Access, -1);
    }
    return rows;
}

/* keep: the page is already shown (a hot-plug, the on-board toggle): the rows follow in place, the scroll stays and
 * the highlight goes where it is asked (nowhere asked: it stays where it was). */
void SerialApp::show_ports(const std::string &select_id, bool select_toggle, bool keep)
{
    if (keep && (!list_ || list_density_ != Density::Comfortable)) keep = false;
    ensure_list(Density::Comfortable, true);
    const std::vector<cp0::ui::ListRow> rows = ports_rows();
    int select = -1;
    for (std::size_t i = 0; i < ports_map_.size(); ++i) {
        const PortsRow &lr = ports_map_[i];
        if (select_toggle && lr.kind == PortsRow::Onboard) select = static_cast<int>(i);
        if (!select_toggle && lr.kind == PortsRow::Port && ports_[static_cast<std::size_t>(lr.port)].id == select_id)
            select = static_cast<int>(i);
    }
    if (keep) {
        list_->set_rows(rows);
        if (select >= 0 && select != list_->selected()) list_->select(select, false);
    } else {
        list_->reset_rows(rows, std::max(0, select));
    }
    if (!dialog_) focus(list_->focus_object());
    caption_row_ = -2;
    update_ports_caption(true);
}

void SerialApp::update_ports_caption(bool force)
{
    const int row = sel();
    if (!force && row == caption_row_) return;
    caption_row_ = row;
    const bool valid = row >= 0 && row < static_cast<int>(ports_map_.size());
    if (valid && ports_map_[static_cast<std::size_t>(row)].kind == PortsRow::Port) {
        const PortInfo &p = ports_[static_cast<std::size_t>(ports_map_[static_cast<std::size_t>(row)].port)];
        say(port_caption(p, config_.settings(p.id)), cp0::ui::CaptionTone::Normal);
    } else if (valid && ports_map_[static_cast<std::size_t>(row)].kind == PortsRow::Boot) {
        const BootDrive &d = boots_[static_cast<std::size_t>(ports_map_[static_cast<std::size_t>(row)].port)];
        say(d.mode.hint, cp0::ui::CaptionTone::Warn);
    } else {
        say("", cp0::ui::CaptionTone::Normal);
    }
    show_empty_card(ports_.empty() && boots_.empty());
}

/* The empty list: a card under the rows, "No serial adapter found." and what to do, at the BODY size (the CAPTION
 * size when that does not fit). */
void SerialApp::show_empty_card(bool show)
{
    if (!show || nav_.current() != View::Ports) {
        if (card_) lv_obj_add_flag(card_, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    const cp0_ui_list_layout_t &l = lay_[static_cast<int>(Density::Comfortable)];
    const int s = cp0_ui_tok(CP0_TOK_SPACE_S);
    const int pad = layout_.gap;
    const int top = l.y + static_cast<int>(ports_map_.size()) * l.row_h + layout_.gap;
    const int room = l.y + l.h - top;
    if (!card_) {
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
        lv_obj_set_width(card_, l.w);
        lv_obj_set_height(card_, LV_SIZE_CONTENT);
        lv_obj_t *title = lv_label_create(card_);
        lv_label_set_text(title, kEmptyTitle);
        lv_obj_set_style_text_color(title, lv_color_hex(kCardTitle), 0);
        lv_obj_t *text = lv_label_create(card_);
        lv_label_set_text(text, kEmptyText);
        lv_label_set_long_mode(text, LV_LABEL_LONG_MODE_WRAP);
        lv_obj_set_width(text, l.w - 2 * pad);
        lv_obj_set_style_text_color(text, lv_color_hex(kCardText), 0);
        for (const cp0_ui_text_t size : {CP0_TEXT_BODY, CP0_TEXT_CAPTION}) {
            lv_obj_set_style_text_font(title, cp0_ui_font(size), 0);
            lv_obj_set_style_text_font(text, cp0_ui_font(size), 0);
            lv_obj_update_layout(card_);
            if (lv_obj_get_height(card_) <= room) break;
        }
    }
    lv_obj_set_pos(card_, l.x, top);
    lv_obj_remove_flag(card_, LV_OBJ_FLAG_HIDDEN);
}

void SerialApp::activate_ports(int row)
{
    if (row < 0 || row >= static_cast<int>(ports_map_.size())) return;
    const PortsRow lr = ports_map_[static_cast<std::size_t>(row)];
    switch (lr.kind) {
    case PortsRow::Port: open_port_page(ports_[static_cast<std::size_t>(lr.port)]); break;
    case PortsRow::Boot: {
        const BootDrive &d = boots_[static_cast<std::size_t>(lr.port)];
        Problem notice;
        notice.title = d.mode.title;
        notice.text = d.mode.advice + ".";
        show_problem(notice);
        break;
    }
    case PortsRow::Onboard: {
        config_.show_onboard = !config_.show_onboard;
        save_config(config_path_, config_);
        std::vector<PortInfo> all = scan_all(true);
        boots_ = scan_boot_drives(roots_, all);
        std::vector<PortInfo> found;
        for (PortInfo &p : all)
            if (!p.onboard || config_.show_onboard) found.push_back(std::move(p));
        ports_ = std::move(found);
        show_ports(std::string(), true, true);
        break;
    }
    case PortsRow::Access: show_problem(group_problem(groups())); break;
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
    probe_value_[0].clear();
    probe_value_[1].clear();
    ask_value_.clear();
    show_empty_card(false);
    nav_.open(View::Port);
    show_port_rows(-1);
    show_port_rows(port_row_index(PortRow::Connect));
    show_title();
    update_port_caption(true);
}

std::vector<cp0::ui::ListRow> SerialApp::port_rows()
{
    using cp0::ui::ListRow;
    using cp0::ui::ValueTone;
    port_map_.clear();
    row_full_.clear();
    std::vector<ListRow> rows;
    const auto synced = [this, &rows](PortRow which) { /* every row added since the last call is `which` */
        while (port_map_.size() < rows.size()) port_map_.push_back(which);
    };
    const auto add = [&](PortRow which, ListRow row) {
        rows.push_back(std::move(row));
        row_full_.emplace_back();
        synced(which);
    };
    const auto toggle = [&](PortRow which, const char *label, bool on) {
        add(which, ListRow::item(label, on ? "On" : "Off", on ? ValueTone::Ok : ValueTone::Normal, 0, Aff::Edit));
    };
    add(PortRow::Info, ListRow::section("What is connected"));
    for (const auto &kv : port_info_items(port_, can_open(port_) && group_ok(groups()))) {
        add_rows(rows, row_full_, Density::Compact, kv.first, kv.second, ValueTone::Normal, false, Aff::Info);
        synced(PortRow::Info);
    }
    // "Identify what is connected": a heading, the adapter note, the last result, the three levels
    const Identity known = port_identity();
    add(PortRow::Info, ListRow::section("Identify what is connected"));
    if (bare_adapter(port_) && !known.found()) {
        add_rows(rows, row_full_, Density::Compact, kAdapterNote, std::string(), ValueTone::Normal, true);
        synced(PortRow::Info);
    }
    if (known.kind != Kind::None) {
        add_rows(rows, row_full_, Density::Compact, "Behind it", behind_text(known), ValueTone::Normal, false, Aff::Open);
        synced(PortRow::Behind);
    }
    const PortRow level_rows[] = {PortRow::IdListen, PortRow::IdReset, PortRow::IdAsk};
    for (int i = 0; i < 3; ++i) {
        const bool busy = prober_ && probe_level(probe_) == i + 1 && i < 2;
        if (busy) {
            add(level_rows[i], ListRow::item(probe_shown_, "Stop", ValueTone::Warn, 0, Aff::ActionRunning));
            continue;
        }
        const std::string label = i == 0 ? "Listen" : i == 1 ? "Reset and listen" : "Ask the device";
        const std::string &outcome = i < 2 ? probe_value_[i] : std::string();
        const std::string value = !outcome.empty() ? outcome : i == 0 ? "sends nothing" : i == 1 ? "resets" : "sends";
        add(level_rows[i], ListRow::item(label, value, tone_of(outcome == "Stopped" || outcome == "Failed"), 0,
                                         i == 2 ? Aff::Open : Aff::Action)); /* Ask opens the Ask page */
    }
    add(PortRow::Info, ListRow::section("Settings"));
    const int baud = effective_baud(settings_, recommended());
    add(PortRow::Speed, ListRow::item("Speed", speed_value(baud, recommended()), ValueTone::Normal, 0, Aff::Edit));
    add(PortRow::Frame, ListRow::item("Frame", settings_.frame, ValueTone::Normal, 0, Aff::Edit));
    toggle(PortRow::Echo, "Local echo", settings_.echo);
    add(PortRow::Ending, ListRow::item("Enter sends", ending_name(settings_.ending), ValueTone::Normal, 0, Aff::Edit));
    add(PortRow::Backspace, ListRow::item("Backspace sends", backspace_name(settings_.backspace), ValueTone::Normal, 0, Aff::Edit));
    toggle(PortRow::Log, "Log to file", settings_.log);
    if (finder_) add(PortRow::Find, ListRow::item(finder_shown_, "Stop", ValueTone::Warn, 0, Aff::ActionRunning));
    else add(PortRow::Find, ListRow::item("Find the speed", find_value_, tone_of(find_rgb_ != -1), 0, Aff::Action));
    toggle(PortRow::FindEnter, "Send Enter while finding", settings_.detect_enter);
    add(PortRow::Connect, ListRow::item("Connect", std::to_string(baud) + " " + settings_.frame, ValueTone::Normal, 0,
                                  Aff::Action));
    return rows;
}

int SerialApp::port_row_index(PortRow which) const
{
    for (std::size_t i = 0; i < port_map_.size(); ++i)
        if (port_map_[i] == which) return static_cast<int>(i);
    return 0;
}

/* select < 0: the rows are only mapped (port_map_), nothing is shown. keep: the page is on screen already (a probe or
 * the speed finder started or ended): the rows follow in place, the scroll stays. */
void SerialApp::show_port_rows(int select, bool keep)
{
    const std::vector<cp0::ui::ListRow> rows = port_rows();
    if (select < 0) return;
    if (keep && (!list_ || list_density_ != Density::Compact || nav_.current() != View::Port)) keep = false;
    ensure_list(Density::Compact, true);
    if (keep) {
        list_->set_rows(rows);
        if (select != list_->selected()) list_->select(select, false);
    } else {
        list_->reset_rows(rows, select);
    }
    if (!dialog_) focus(list_->focus_object());
}

void SerialApp::update_port_row(PortRow which)
{
    const std::vector<cp0::ui::ListRow> rows = port_rows();
    const int index = port_row_index(which);
    list_->update_row(index, rows[static_cast<std::size_t>(index)]);
    if (which == PortRow::Speed || which == PortRow::Frame) { /* Connect shows them too */
        const int connect = port_row_index(PortRow::Connect);
        list_->update_row(connect, rows[static_cast<std::size_t>(connect)]);
    }
}

/* The port page's status band: why the recommended settings, or the result of Find the speed. A row cut to fit says
 * its whole text; a port that went away is said whatever the highlight. */
void SerialApp::update_port_caption(bool force)
{
    const int row = sel();
    if (!force && row == caption_row_) return;
    caption_row_ = row;
    if (!port_present_) {
        say(port_.tty + " is unplugged: plug it in again", cp0::ui::CaptionTone::Warn);
        return;
    }
    const bool mapped = row >= 0 && row < static_cast<int>(port_map_.size());
    const bool info = mapped && port_map_[static_cast<std::size_t>(row)] == PortRow::Info;
    const PortRow which = mapped ? port_map_[static_cast<std::size_t>(row)] : PortRow::Info;
    if (info) say(row < static_cast<int>(row_full_.size()) ? row_full_[static_cast<std::size_t>(row)] : std::string(),
                  cp0::ui::CaptionTone::Normal);
    else if (finder_ || prober_) say("Esc stops", cp0::ui::CaptionTone::Normal);
    else if (which == PortRow::IdListen) say(probe_caption(Probe::Listen, reset_style(port_)), cp0::ui::CaptionTone::Normal);
    else if (which == PortRow::IdReset) say(probe_caption(Probe::Reset, reset_style(port_)), cp0::ui::CaptionTone::Normal);
    else if (which == PortRow::IdAsk) say(probe_caption(Probe::At, reset_style(port_)), cp0::ui::CaptionTone::Normal);
    else if (which == PortRow::Behind) {
        const Identity known = port_identity();
        say(confidence_text(known) + ", " + when_text(known.when), cp0::ui::CaptionTone::Normal);
    }
    else if (!port_note_.empty())
        say(port_note_, port_note_warn_ ? cp0::ui::CaptionTone::Warn : cp0::ui::CaptionTone::Normal);
    else say(port_.rec.reason, cp0::ui::CaptionTone::Normal);
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
    if (prober_) { /* while a probe runs, only its own row (stop) */
        if ((which == PortRow::IdListen && probe_ == Probe::Listen) || (which == PortRow::IdReset && probe_ == Probe::Reset))
            prober_->cancel();
        return;
    }
    switch (which) {
    case PortRow::Info: break;
    case PortRow::Behind: {
        const Identity known = port_identity();
        card_note_.clear();
        open_card(known, probe_from_key(known.probe), -1);
        break;
    }
    case PortRow::IdListen: start_probe(Probe::Listen, false); break;
    case PortRow::IdReset: start_probe(Probe::Reset, false); break;
    case PortRow::IdAsk: open_ask(0); break;
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
    show_port_rows(port_row_index(PortRow::Find), true); /* the row says the progress */
    update_port_caption(true);
}

void SerialApp::check_finder()
{
    if (!finder_) return;
    const FinderProgress p = finder_->progress();
    if (!p.done) {
        if (p.text != finder_shown_ && nav_.current() == View::Port && list_) {
            finder_shown_ = p.text;
            const int index = port_row_index(PortRow::Find);
            list_->update_row(index, cp0::ui::ListRow::item(finder_shown_, "Stop", cp0::ui::ValueTone::Warn, 0, Aff::ActionRunning));
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
        show_port_rows(port_row_index(PortRow::Find), true);
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
    make_roller(); /* a pick-one list with a check on the current value: the Settings look */
    roller_->set_rows(rows, current, true);
    update_chrome();
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
    update_chrome(); /* the list and the band give way to the terminal */
    term_ = std::make_unique<TerminalView>(*page_, layout_, std::move(spec)); /* it sets the page title */
    focus(term_->focus_object());
}

void SerialApp::close_terminal()
{
    term_.reset();
    nav_.back();
    update_chrome();
    show_title();
    update_port_caption(true);
    next_scan_ = 0; /* the port may have gone meanwhile */
    focus(focus_target());
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
    update_chrome(); /* the band gives way to the dialog */
    focus(dialog_->focus_object());
}

void SerialApp::close_dialog()
{
    if (!dialog_) return;
    dialog_.reset();
    update_chrome();
    focus(focus_target());
}

} // namespace sc
