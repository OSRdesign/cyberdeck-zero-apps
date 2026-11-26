/*
 * SPDX-License-Identifier: MIT
 *
 * The Reticulum app: life cycle, the service, navigation and the page frame (see ret_app.hpp). The pages' rows are in
 * ret_pages.cpp, the editing and the dialogs in ret_edit.cpp.
 */
#include "ret_app.hpp"

#include "app.hpp"
#include "build_info.hpp"
#include "osk_policy.hpp"

#include "cp0_ui_list.h"
#include "cp0_ui_metrics_lvgl.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <spawn.h>
#include <unistd.h>

extern char **environ;

namespace retui {

namespace {

constexpr std::int64_t kStartTimeoutMs = 60000; /* the service has this long to answer after the start command */
constexpr std::int64_t kProbeEveryMs = 500;
constexpr std::int64_t kPollEveryMs = 3000;
constexpr int kTickMs = 100;

/* The first non-empty line of a command's output, cut to a length a notice can show. */
std::string first_line(const std::string &text)
{
    std::size_t a = 0;
    while (a < text.size() && (text[a] == '\n' || text[a] == '\r' || text[a] == ' ')) ++a;
    std::size_t b = text.find('\n', a);
    std::string line = text.substr(a, b == std::string::npos ? std::string::npos : b - a);
    if (line.size() > 160) line.resize(160);
    return line;
}

/* Starts a command and does not wait for it: the app is ending (a `systemctl --no-block stop` returns at once). posix_spawn
 * in a new session: no fork in a process with other threads, and the launcher's SIGTERM to the app's group cannot reach it. */
void spawn_detached(const std::vector<std::string> &argv)
{
    if (argv.empty()) return;
    std::vector<char *> args;
    for (const std::string &a : argv) args.push_back(const_cast<char *>(a.c_str()));
    args.push_back(nullptr);
    posix_spawnattr_t attr;
    if (posix_spawnattr_init(&attr) != 0) return;
    posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETSID);
    pid_t pid = 0;
    posix_spawnp(&pid, args[0], nullptr, &attr, args.data(), environ);
    posix_spawnattr_destroy(&attr);
}

} // namespace

App::App(lv_obj_t *screen)
{
    cp0_ui_roller_page(cp0_ui_metrics_get(), cp0::ui::status_strip_left(), &layout_);
    page_ = std::make_unique<cp0::ui::Page>(screen, layout_, cp0::ui::add_title, [this] {
        post([this] { back(); });
    });
    cp0::ui::StatusStrip::add(page_->header());

    const char *sc = std::getenv("RETICULUM_SYSTEMCTL");
    systemctl_bin_ = sc ? sc : "";
    const std::string dir = ret::run_dir(std::getenv("RETICULUM_BRIDGE_RUN"), std::getenv("XDG_RUNTIME_DIR"),
                                         std::getenv("APPLAUNCH_CANVAS"));
    link_ = std::make_unique<ret::Link>(dir.empty() ? std::string() : dir + "/bridge.sock");
    link_->set_subscribe(true);
    std::fprintf(stderr, "[reticulum] app %s, socket %s\n", CDK_APP_VERSION,
                 dir.empty() ? "(no run folder)" : (dir + "/bridge.sock").c_str());

    timer_ = lv_timer_create(tick_cb, kTickMs, this);
    begin_service();
}

App::~App()
{
    lv_async_call_cancel(posted_cb, this);
    if (timer_) lv_timer_delete(timer_);
    timer_ = nullptr;
    if (start_job_) cdk::async_run::cancel(start_job_);
    if (unit_job_) cdk::async_run::cancel(unit_job_);
    /* The service runs only while the app does, unless Keep running is on (owner decision D6). */
    if (!keep_running_) {
        std::fprintf(stderr, "[reticulum] leaving: stopping the service (Keep running is off)\n");
        spawn_detached(ret::systemctl_argv(systemctl_bin_.c_str(), {"--no-block", "stop", "reticulum"}));
    } else {
        std::fprintf(stderr, "[reticulum] leaving: the service keeps running\n");
    }
    link_.reset();
    release_dialog_keys();
    dialog_.reset();
    teardown_conversation();
    rich_.reset();
    field_.reset();
    text_.reset();
    list_.reset();
    page_.reset();
}

std::int64_t App::now_ms() const
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<std::int64_t>(ts.tv_sec) * 1000 + ts.tv_nsec / 1000000;
}

void App::tick_cb(lv_timer_t *timer)
{
    static_cast<App *>(lv_timer_get_user_data(timer))->tick();
}

void App::posted_cb(void *data)
{
    auto *self = static_cast<App *>(data);
    self->post_armed_ = false;
    std::vector<std::function<void()>> fns;
    fns.swap(self->posted_);
    for (auto &fn : fns) fn();
}

void App::post(std::function<void()> fn)
{
    posted_.push_back(std::move(fn));
    if (!post_armed_) post_armed_ = lv_async_call(posted_cb, this) == LV_RESULT_OK;
}

void App::focus(lv_obj_t *obj)
{
    lv_group_t *group = cdk::input_group();
    if (!group || !obj) return;
    if (lv_obj_get_group(obj) != group) lv_group_add_obj(group, obj);
    lv_group_focus_obj(obj);
}

/* ------------------------------------------------------------------ the timer */

void App::tick()
{
    link_->poll();
    bool heard_changed = false;
    for (const ret::json::Value &ev : link_->take_events()) {
        ret::Heard h;
        const std::string name = ev["event"].as_string();
        if (name == "announce" && ret::parse_heard(ev, h)) { // vocab-ok: the bridge's event name
            heard_.add(h);
            heard_changed = true;
        } else if (svc_ == Svc::Up) {
            handle_chat_event(ev);
        }
    }
    if (link_->take_resubscribed() && svc_ == Svc::Up) request_all_chat(); /* events may have been missed */
    if (heard_changed) refresh_view();
    const std::int64_t now = now_ms();
    /* "heard 5 min ago" ages while a contact page or the contacts list stays open: one redraw a minute */
    if (svc_ == Svc::Up && (nav_.current() == ret::View::Contact || nav_.current() == ret::View::Contacts)) {
        if (now - last_age_ms_ >= 60000) {
            last_age_ms_ = now;
            refresh_view();
        }
    } else {
        last_age_ms_ = now;
    }
    if (svc_ == Svc::Up) {
        if (convs_dirty_ && !convs_busy_ && now - last_convs_ms_ >= 400) request_convs();
        if (contacts_dirty_ && !contacts_busy_) request_contacts();
    }
    if (svc_ == Svc::Starting || svc_ == Svc::Checking) {
        if (svc_ == Svc::Starting && !probe_busy_ && now - last_probe_ms_ >= kProbeEveryMs) probe();
        if (svc_ == Svc::Starting && now - svc_since_ms_ > kStartTimeoutMs) {
            service_failed("The service did not answer within a minute. In a terminal, "
                           "\"systemctl --user status reticulum\" tells why.");
        } else if (nav_.current() == ret::View::Home && list_ && !dialog_) {
            list_->set_rows(rows_home()); /* the seconds counter */
        }
    } else if (svc_ == Svc::Up && !cdk::hidden() && !poll_busy_ && !busy_ && now - last_poll_ms_ >= kPollEveryMs) {
        send_poll();
    }
}

/* ------------------------------------------------------------------ the service */

void App::begin_service()
{
    svc_ = Svc::Checking;
    svc_why_.clear();
    svc_since_ms_ = now_ms();
    start_cmd_done_ = false;
    render();
    probe();
}

void App::probe()
{
    probe_busy_ = true;
    last_probe_ms_ = now_ms();
    link_->request("hello", ret::json::Value::object(), 2.0, [this](const ret::Response &r) {
        probe_busy_ = false;
        if (r.ok) {
            if (svc_ == Svc::Checking || svc_ == Svc::Starting) service_up();
        } else if (svc_ == Svc::Checking) {
            start_service();
        }
    });
}

void App::start_service()
{
    svc_ = Svc::Starting;
    svc_since_ms_ = now_ms();
    if (link_->path().empty()) {
        service_failed("There is no user session: XDG_RUNTIME_DIR is not set, so the service cannot run. "
                       "Start the app from the deck's own session.");
        return;
    }
    std::fprintf(stderr, "[reticulum] service not running: starting it\n");
    cdk::async_run::Spec spec;
    spec.argv = ret::systemctl_argv(systemctl_bin_.c_str(), {"start", "reticulum"});
    spec.timeout_ms = 30000;
    auto out = std::make_shared<std::string>();
    spec.on_line = [out](const std::string &line) {
        if (out->size() < 1024) *out += line + "\n";
    };
    spec.on_done = [this, out](const cdk::async_run::Result &r) {
        start_job_ = 0;
        if (r.ok()) {
            start_cmd_done_ = true;
            return;
        }
        std::string why = "Could not start the service";
        if (!r.started || r.exit_code == 127) why = "systemctl was not found on this machine";
        else if (r.timed_out) why = "Starting the service took too long";
        const std::string detail = ret::plain_systemctl(first_line(*out));
        service_failed(why + (detail.empty() ? std::string(".") : ": " + detail));
    };
    start_job_ = cdk::async_run::run(std::move(spec));
    if (nav_.current() == ret::View::Home) render();
}

void App::service_failed(const std::string &why, Svc as)
{
    if (start_job_) {
        cdk::async_run::cancel(start_job_);
        start_job_ = 0;
    }
    std::fprintf(stderr, "[reticulum] service %s: %s\n", as == Svc::Stopped ? "stopped" : "not running", why.c_str());
    svc_ = as;
    svc_why_ = why;
    busy_ = false;
    close_dialog();
    nav_.reset();
    saved_sel_.clear();
    edit_ = Edit::None;
    render();
}

void App::service_up()
{
    std::fprintf(stderr, "[reticulum] service answers\n");
    svc_ = Svc::Up;
    svc_why_.clear();
    last_poll_ms_ = now_ms();
    poll_busy_ = true;
    check_keep_running();
    send_poll();
    request_all_chat();
    render();
    request_notices();
}

void App::on_link_lost(const ret::Response &r)
{
    if (svc_ != Svc::Up) return;
    if (r.transport && r.code != "no_service" && r.code != "lost") return;
    if (r.code == "no_answer") return; /* slow, not gone: the next poll asks again */
    service_failed("The service stopped answering.");
}

void App::send_poll()
{
    poll_busy_ = true;
    last_poll_ms_ = now_ms();
    link_->request("status", ret::json::Value::object(), 5.0, [this](const ret::Response &r) {
        if (r.ok) {
            ret::parse_deck(r.result, deck_);
            if (deck_.unread != unread_seen_) { /* a message came while no event reached us, or one was read elsewhere */
                if (unread_seen_ >= 0) convs_dirty_ = true;
                unread_seen_ = deck_.unread;
            }
        } else if (!r.transport) on_link_lost(r);
        else if (r.code == "no_service" || r.code == "lost") on_link_lost(r);
    });
    if (!catalog_.loaded) {
        link_->request("iface.types", ret::json::Value::object(), 5.0, [this](const ret::Response &r) {
            if (r.ok) ret::parse_catalog(r.result, catalog_);
        });
    }
    link_->request("ifaces", ret::json::Value::object(), 5.0, [this](const ret::Response &r) {
        poll_busy_ = false;
        if (svc_ != Svc::Up) return;
        if (r.ok && ret::parse_ifaces(r.result, ifaces_)) ifaces_known_ = true;
        else if (!r.ok) on_link_lost(r);
        if (svc_ == Svc::Up) refresh_view();
    });
}

void App::request_ifaces(std::function<void()> then)
{
    link_->request("ifaces", ret::json::Value::object(), 8.0, [this, then](const ret::Response &r) {
        if (svc_ != Svc::Up) return;
        if (r.ok && ret::parse_ifaces(r.result, ifaces_)) ifaces_known_ = true;
        else if (!r.ok) on_link_lost(r);
        if (svc_ == Svc::Up) {
            if (then) then();
            else refresh_view();
        }
    });
}

void App::request_status()
{
    link_->request("status", ret::json::Value::object(), 5.0, [this](const ret::Response &r) {
        if (r.ok) ret::parse_deck(r.result, deck_);
        if (svc_ == Svc::Up) refresh_view();
    });
}

void App::check_keep_running()
{
    if (unit_job_) return;
    cdk::async_run::Spec spec;
    spec.argv = ret::systemctl_argv(systemctl_bin_.c_str(), {"is-enabled", "reticulum"});
    spec.timeout_ms = 8000;
    auto out = std::make_shared<std::string>();
    spec.on_line = [out](const std::string &line) { *out += line + "\n"; };
    spec.on_done = [this, out](const cdk::async_run::Result &) {
        unit_job_ = 0;
        keep_known_ = true;
        keep_running_ = ret::unit_is_enabled(*out);
        refresh_view();
    };
    unit_job_ = cdk::async_run::run(std::move(spec));
}

void App::set_keep_running(bool on)
{
    if (keep_busy_) return;
    keep_busy_ = true;
    cdk::async_run::Spec spec;
    spec.argv = ret::systemctl_argv(systemctl_bin_.c_str(), {on ? "enable" : "disable", "reticulum"});
    spec.timeout_ms = 15000;
    auto out = std::make_shared<std::string>();
    spec.on_line = [out](const std::string &line) {
        if (out->size() < 1024) *out += line + "\n";
    };
    spec.on_done = [this, out, on](const cdk::async_run::Result &r) {
        keep_busy_ = false;
        if (r.ok()) {
            keep_running_ = on;
            keep_known_ = true;
            std::fprintf(stderr, "[reticulum] keep running %s\n", on ? "on" : "off");
        } else {
            const std::string detail = ret::plain_systemctl(first_line(*out));
            notice("Could not change it", detail.empty() ? "The system did not accept the change." : detail);
        }
        refresh_view();
    };
    cdk::async_run::run(std::move(spec));
    refresh_view();
}

void App::stop_service()
{
    busy_ = true;
    link_->request("service.stop", ret::json::Value::object(), 10.0, [this](const ret::Response &r) {
        busy_ = false;
        std::fprintf(stderr, "[reticulum] service stop asked: %s\n", r.ok ? "ok" : r.code.c_str());
        service_failed("You stopped it.", Svc::Stopped);
    });
}

/* ------------------------------------------------------------------ navigation and frames */

int App::line_h(int px) const
{
    return lv_font_get_line_height(cp0_ui_font_px(px));
}

void App::clear_view()
{
    teardown_conversation();
    list_.reset();
    rich_.reset();
    field_.reset();
    text_.reset();
    form_status_ = nullptr; /* deleted with the body */
    page_->clear_body();
    if (top_) lv_obj_delete(top_);
    if (foot_) lv_obj_delete(foot_);
    top_ = foot_ = nullptr;
}

void App::set_top(const std::string &text, ret::Tone tone)
{
    if (!top_) return;
    lv_label_set_text(top_, text.c_str());
    static const cp0_ui_list_tone_t map[] = {CP0_UI_LIST_TONE_OK, CP0_UI_LIST_TONE_OK, CP0_UI_LIST_TONE_WARN,
                                             CP0_UI_LIST_TONE_BAD, CP0_UI_LIST_TONE_MUTED};
    lv_obj_set_style_text_color(top_, lv_color_hex(cp0_ui_list_tone_rgb(map[static_cast<int>(tone)])), 0);
}

cp0::ui::CompactListEvents App::list_events()
{
    cp0::ui::CompactListEvents ev;
    ev.activate = [this](int, std::intptr_t id) {
        post([this, id] {
            if (dialog_ || busy_) return;
            activate(id);
        });
    };
    ev.back = [this] { post([this] { back(); }); };
    return ev;
}

/* Clears the page and draws the top line and the hint around the list area; returns the list's layout. */
cp0_ui_list_layout_t App::prepare_frame(const std::string &top, ret::Tone top_tone, const std::string &foot, bool read_only)
{
    /* a read-only page is data: compact rows (5 mm), more of it in view; every page with a row to act on has 7 mm rows */
    clear_view();
    const cp0_ui_metrics_t *m = cp0_ui_metrics_get();
    const int x = layout_.roller_x, w = layout_.roller_w;
    int y = layout_.body_y;
    int bottom = layout_.h;
    const lv_font_t *cap = cp0_ui_font_px(layout_.caption_px);
    const int lh = lv_font_get_line_height(cap);
    const int text_w = std::max(1, w - 2 * layout_.inset);

    if (!top.empty()) {
        top_ = lv_label_create(page_->root());
        lv_label_set_long_mode(top_, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_style_text_font(top_, cap, 0);
        lv_obj_set_width(top_, text_w);
        lv_obj_set_height(top_, lh);
        lv_obj_set_pos(top_, x + layout_.inset, y + layout_.gap / 2);
        set_top(top, top_tone);
        y += lh + layout_.gap;
    }
    if (!foot.empty()) {
        lv_point_t size{};
        lv_text_get_size(&size, foot.c_str(), cap, 0, 0, text_w, LV_TEXT_FLAG_NONE);
        const int hh = std::min<int>(size.y, 3 * lh);
        foot_ = lv_label_create(page_->root());
        lv_label_set_long_mode(foot_, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_style_text_font(foot_, cap, 0);
        lv_obj_set_style_text_color(foot_, lv_color_hex(CP0_UI_LIST_SECTION_RGB), 0);
        lv_obj_set_width(foot_, text_w);
        lv_obj_set_height(foot_, hh);
        lv_label_set_text(foot_, foot.c_str());
        bottom -= hh + layout_.gap;
        lv_obj_set_pos(foot_, x + layout_.inset, bottom);
        /* opaque, so a row the list cuts at its edge never shows through the hint */
        lv_obj_set_style_bg_color(foot_, lv_color_hex(0x000000), 0);
        lv_obj_set_style_bg_opa(foot_, LV_OPA_COVER, 0);
        /* a full gap of black between the list's cut edge and the hint's first line (board QA of 0.3.2, D5 and D7: the
         * last Home row and a section header touched the hint at 480x320 with half a gap) */
        lv_obj_set_style_pad_top(foot_, layout_.gap, 0);
        lv_obj_set_pos(foot_, x + layout_.inset, bottom - layout_.gap);
        lv_obj_set_height(foot_, hh + layout_.gap);
        bottom -= layout_.gap;
    }
    cp0_ui_list_layout_t ll{};
    cp0_ui_list_layout(m, read_only ? CP0_UI_LIST_COMPACT : CP0_UI_LIST_COMFORTABLE, x, y, w, std::max(1, bottom - y), &ll);
    return ll;
}

void App::frame(const std::string &top, ret::Tone top_tone, const std::string &foot, bool read_only,
                const std::vector<cp0::ui::ListRow> &rows, int selected)
{
    const cp0_ui_list_layout_t ll = prepare_frame(top, top_tone, foot, read_only);
    cp0::ui::CompactListOptions opt;
    opt.read_only = read_only;
    list_ = std::make_unique<cp0::ui::CompactList>(page_->root(), ll, list_events(), opt);
    list_->reset_rows(rows, selected);
    focus(list_->focus_object());
    if (foot_) lv_obj_move_foreground(foot_);
}

void App::frame_rich(const std::string &foot, const std::vector<RichRow> &rows, int selected)
{
    const cp0_ui_list_layout_t ll = prepare_frame("", ret::Tone::Muted, foot, false);
    rich_ = std::make_unique<RichList>(
        page_->root(), ll,
        [this](int, std::intptr_t id) {
            post([this, id] {
                if (dialog_ || busy_) return;
                activate_rich(id);
            });
        },
        [this] { post([this] { back(); }); });
    rich_->reset_rows(rows, selected);
    focus(rich_->focus_object());
    if (foot_) lv_obj_move_foreground(foot_);
}

void App::set_title()
{
    std::vector<std::string> path = {"Reticulum"};
    for (const ret::View v : nav_.stack()) {
        switch (v) {
        case ret::View::Connections: path.push_back("Connections"); break;
        case ret::View::Connection: {
            const ret::Iface *i = current_iface();
            path.push_back(i ? ret::conn_title(*i) : cur_name_);
            break;
        }
        case ret::View::AddKind: path.push_back("Add a connection"); break;
        case ret::View::AddMore: path.push_back("More"); break;
        case ret::View::AddForm: path.push_back(ret::kind_info(draft_type_).title); break;
        case ret::View::Option:
        case ret::View::Choice:
        case ret::View::Name: path.push_back(edit_title_); break;
        case ret::View::Status: path.push_back("Status"); break;
        case ret::View::Settings: path.push_back("Settings"); break;
        case ret::View::Address: path.push_back("My address"); break;
        case ret::View::Messages: path.push_back("Messages"); break;
        case ret::View::Conversation:
            conv_title_clipped_ = clip_conv_title(current_name());
            if (!nav_.has(ret::View::Contact)) path.push_back(conv_title_clipped_);
            break;
        case ret::View::ConvOptions:
            if (!nav_.has(ret::View::Contact)) path.push_back(current_name());
            break;
        case ret::View::Contacts: path.push_back(pick_ ? "New message" : "Contacts"); break;
        case ret::View::Contact:
            /* opened from a conversation's options: the conversation and its options (the same name) are not crumbs here, so
             * the title is "Reticulum > Contacts > <Name>" however the page was reached, before and after a Rename
             * (QA of 0.3.1, F3: it read "Palmpi > Palmpi > Palmpi") */
            if (nav_.has(ret::View::Conversation) || nav_.has(ret::View::ConvOptions)) path = {"Reticulum", "Contacts"};
            path.push_back(current_name());
            break;
        case ret::View::AddContact: path.push_back("Add a contact"); break;
        case ret::View::Heard: path.push_back("Heard recently"); break;
        case ret::View::AnnounceBlocked:  // vocab-ok: the owner's button word
            /* the header cuts a title that does not fit in the middle of a word (board QA of 0.3.2, D4 at 480x320): the
             * short form is a whole word */  // vocab-ok: the owner's button word
            path.push_back(title_fits("Announce myself") ? "Announce myself" : "Announce");  // vocab-ok: the owner's button word
            break;
        case ret::View::Home: break;
        }
    }
    /* the owner's boards 20 and 31: a conversation and its options are titled with the name alone */
    if (nav_.current() == ret::View::Conversation) path = {conv_title_clipped_};
    else if (nav_.current() == ret::View::ConvOptions) path = {current_name()};
    page_->set_title(path);
}

void App::render(int selected)
{
    set_title();
    rendered_svc_ = static_cast<int>(svc_);
    ret::Tone tone = ret::Tone::Muted;
    switch (nav_.current()) {
    case ret::View::Home: {
        const std::string top = home_top(&tone);
        const std::string foot = svc_ == Svc::Up ? home_foot()
                                 : (svc_ == Svc::Failed || svc_ == Svc::Stopped) ? svc_why_
                                                                                  : std::string();
        frame(top, tone, foot, false, rows_home(), selected);
        break;
    }
    case ret::View::Connections:
        frame("", tone, "Off means nothing is sent.", false, rows_connections(), selected);
        break;
    case ret::View::Connection: {
        const std::string top = connection_top(&tone);
        /* QA of 0.3.1, F4: a kind that is only on or off says so in the footer (a section row is one clipped line) */
        const ret::Iface *shown = current_iface();
        /* the listen-only note of a kind that opens a link is here too, for the same reason (QA of 0.3.5) */
        conn_foot_ = shown ? ret::connection_foot(*shown) : std::string();
        frame(top, tone, conn_foot_, false, rows_connection(), selected);
        break;
    }
    case ret::View::AddKind:
        frame("", tone, "New connections start switched off.", false, rows_add_kind(false),
              selected);
        break;
    case ret::View::AddMore:
        frame("", tone, "New connections start switched off.", false, rows_add_kind(true),
              selected);
        break;
    case ret::View::AddForm:
        frame("", tone, "Saved switched off; you turn it on yourself.", false, rows_form(),
              selected);
        break;
    case ret::View::Choice: frame("", tone, "", false, rows_choice(), selected); break;
    case ret::View::Status: frame("", tone, "", true, rows_status(), selected); break;
    case ret::View::Settings:
        frame("", tone, "Alerts show a message and beep. They only work while Keep running is on.", false, rows_settings(),
              selected);
        break;
    case ret::View::Messages: frame_rich(footer_for(ret::View::Messages), rich_messages(), selected); break;
    case ret::View::Contacts: frame("", tone, footer_for(ret::View::Contacts), false, rows_contacts(), selected); break;
    case ret::View::Contact: {
        const ret::Contact *c = current_contact();
        std::string iface;
        if (c)
            for (const ret::Person &p : people_)
                if (p.address == c->address) iface = p.iface;
        frame(c ? ret::contact_heard_line(*c, iface, static_cast<double>(std::time(nullptr))) : "", ret::Tone::Ok,
              footer_for(ret::View::Contact), false, rows_contact(), selected);
        break;
    }
    case ret::View::AddContact: frame("", tone, footer_for(ret::View::AddContact), false, rows_add_contact(), selected); break;
    case ret::View::Heard: frame_rich(footer_for(ret::View::Heard), rich_heard(), selected); break;
    case ret::View::Address: frame_rich(footer_for(ret::View::Address), rich_address(), selected); break;
    case ret::View::AnnounceBlocked:
        frame_rich(footer_for(ret::View::AnnounceBlocked), rich_announce_blocked(), selected);
        break;
    case ret::View::ConvOptions:
        frame("", tone, footer_for(ret::View::ConvOptions), false, rows_conv_options(), selected);
        break;
    case ret::View::Conversation: build_conversation(); break;
    case ret::View::Option:
    case ret::View::Name: break; /* built by their own open_ functions */
    }
}

void App::push_view(ret::View v)
{
    saved_sel_.push_back(list_ ? std::max(0, list_->selected()) : rich_ ? std::max(0, rich_->selected()) : 0);
    nav_.open(v);
}

void App::open(ret::View v)
{
    push_view(v);
    render();
}

void App::back()
{
    if (dialog_) {
        close_dialog();
        return;
    }
    /* also while a request waits for the bridge: its reply finds the page gone and only refreshes the data */
    if (nav_.back() == ret::Back::Quit) {
        cdk::request_quit(0); /* a short Esc at the root ends the app (owner decision D5) */
        return;
    }
    int sel = 0;
    if (!saved_sel_.empty()) {
        sel = saved_sel_.back();
        saved_sel_.pop_back();
    }
    edit_ = Edit::None;
    if (!nav_.has(ret::View::Contacts)) pick_ = false;
    if (nav_.current() == ret::View::Connection && !current_iface()) { /* it was removed meanwhile */
        nav_.back();
        if (!saved_sel_.empty()) saved_sel_.pop_back();
        sel = 0;
    }
    render(sel);
}

/* The poll brought new data: the visible page follows it in place (the highlight and the scroll stay). */
void App::refresh_view()
{
    if (!page_) return;
    if (!list_ && !rich_ && !(nav_.current() == ret::View::Conversation && convo_.built)) return;
    if (rendered_svc_ != static_cast<int>(svc_)) {
        render();
        return;
    }
    /* a name changed (Rename, a heard name): the header's title follows it (QA F6: it kept the old name) */
    if (nav_.current() == ret::View::Contact || nav_.current() == ret::View::ConvOptions ||
        nav_.current() == ret::View::Conversation)
        set_title();
    switch (nav_.current()) {
    case ret::View::Messages: rich_->set_rows(rich_messages()); return;
    case ret::View::Heard: rich_->set_rows(rich_heard()); return;
    case ret::View::Address: rich_->set_rows(rich_address()); return;
    case ret::View::AnnounceBlocked: rich_->set_rows(rich_announce_blocked()); return;
    case ret::View::Conversation:
        show_messages();
        update_compose();
        return;
    default: break;
    }
    if (!list_) return;
    ret::Tone tone = ret::Tone::Muted;
    switch (nav_.current()) {
    case ret::View::Home:
        {
            const std::string top = home_top(&tone); /* tone is set by the call: not in the same expression */
            set_top(top, tone);
            if (svc_ == Svc::Up) {
                const std::string hint = home_foot();
                if (!foot_ && !hint.empty()) { /* first answer about the connections: the hint appears */
                    render(std::max(0, list_->selected()));
                    return;
                }
                if (foot_) lv_label_set_text(foot_, hint.c_str());
            }
        }
        list_->set_rows(rows_home());
        break;
    case ret::View::Connections: list_->set_rows(rows_connections()); break;
    case ret::View::Connection: {
        if (const ret::Iface *shown = current_iface()) {
            /* the hint under the page depends on the state (the listen-only note): a different hint redraws the page */
            if (ret::connection_foot(*shown) != conn_foot_) {
                render(std::max(0, list_->selected()));
                break;
            }
        }
        if (!current_iface()) { /* removed meanwhile (by hand, or by another program): leave its page */
            nav_.back();
            int sel = 0;
            if (!saved_sel_.empty()) {
                sel = saved_sel_.back();
                saved_sel_.pop_back();
            }
            render(sel);
            break;
        }
        {
            const std::string top = connection_top(&tone);
            set_top(top, tone);
        }
        list_->set_rows(rows_connection());
        break;
    }
    case ret::View::Status: list_->set_rows(rows_status()); break;
    case ret::View::Settings: list_->set_rows(rows_settings()); break;
    case ret::View::Contacts: list_->set_rows(rows_contacts()); break;
    case ret::View::Contact: {
        /* the line under the title says when it was last heard: it follows the data (a heard event, the clock) */
        const ret::Contact *c = current_contact();
        std::string iface;
        if (c)
            for (const ret::Person &p : people_)
                if (p.address == c->address) iface = p.iface;
        if (c)
            set_top(ret::contact_heard_line(*c, iface, static_cast<double>(std::time(nullptr))), ret::Tone::Ok);
        list_->set_rows(rows_contact());
        break;
    }
    case ret::View::AddContact: list_->set_rows(rows_add_contact()); break;
    case ret::View::ConvOptions: list_->set_rows(rows_conv_options()); break;
    default: break;
    }
}

} // namespace retui
