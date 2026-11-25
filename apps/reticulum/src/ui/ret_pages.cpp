/*
 * SPDX-License-Identifier: MIT
 *
 * The Reticulum app: the rows of every page and what a tap or Enter on a row does (see ret_app.hpp).
 */
#include "ret_app.hpp"

#include "ret_ids.hpp"

#include "app.hpp"
#include "build_info.hpp"
#include "osk_policy.hpp"

#include <algorithm>
#include <cstdio>
#include <ctime>

namespace retui {

namespace {

using cp0::ui::ListRow;
using cp0::ui::RowAffordance;
using cp0::ui::ValueTone;

ValueTone vt(ret::Tone t)
{
    switch (t) {
    case ret::Tone::Ok: return ValueTone::Ok;
    case ret::Tone::Warn: return ValueTone::Warn;
    case ret::Tone::Bad: return ValueTone::Bad;
    case ret::Tone::Muted: return ValueTone::Muted;
    default: return ValueTone::Normal;
    }
}

/* An Info row (read-only) draws its value grey, as the row rule says; only a warning or a failure keeps its colour. */
ValueTone info_vt(ret::Tone t)
{
    return (t == ret::Tone::Warn || t == ret::Tone::Bad) ? vt(t) : ValueTone::Normal;
}

double wall_s()
{
    return static_cast<double>(std::time(nullptr));
}

std::string pct_text(double v)
{
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.0f %%", v);
    return buf;
}

std::string dbm_text(double v)
{
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.0f dBm", v);
    return buf;
}

} // namespace

int App::total_unread() const
{
    int n = 0;
    for (const ret::Conversation &c : convs_) n += c.unread;
    return n;
}

const ret::Iface *App::current_iface() const
{
    for (const ret::Iface &i : ifaces_)
        if (i.name == cur_name_) return &i;
    return nullptr;
}

const ret::OptionDef *App::option_def(std::intptr_t id) const
{
    if (id < kOpt) return nullptr;
    const std::size_t idx = static_cast<std::size_t>(id - kOpt);
    return idx < form_opts_.size() ? &form_opts_[idx] : nullptr;
}

/* ------------------------------------------------------------------ rows */

std::string App::home_top(ret::Tone *tone) const
{
    switch (svc_) {
    case Svc::Up: *tone = ret::Tone::Ok; return "service: running  OK";
    case Svc::Checking: *tone = ret::Tone::Muted; return "service: checking...";
    case Svc::Starting: *tone = ret::Tone::Warn; return "service: starting...";
    case Svc::Failed: *tone = ret::Tone::Bad; return "service: not running";
    case Svc::Stopped: *tone = ret::Tone::Warn; return "service: stopped";
    }
    return std::string();
}

std::string App::home_foot() const
{
    if (!ifaces_known_) return std::string(); /* nothing is said before the service has told what is on */
    bool on = false, may_send = false;
    for (const ret::Iface &i : ifaces_) {
        if (!i.enabled) continue;
        on = true;
        may_send = may_send || i.outgoing || !ret::has_listen_choice(i);
    }
    if (!on) return "Nothing transmits until you switch a connection on.";
    if (may_send) return "A connection is on and may send. Messages and your name go out only when you send them.";
    return "Connections are on and only listen. Messages and your name are not sent.";
}

std::vector<ListRow> App::rows_home() const
{
    std::vector<ListRow> rows;
    if (svc_ == Svc::Up) {
        const int on = ret::count_on(ifaces_);
        /* Messages and Contacts first (the owner's board 9): a new message is what the user opens the app for */
        const int unread = std::max(deck_.unread, convs_known_ ? total_unread() : 0);
        std::string mv = ret::unread_text(unread);
        if (mv.empty()) mv = convs_known_ && convs_.empty() ? "none yet" : "";
        rows.push_back(ListRow::item("Messages", mv, unread > 0 ? ValueTone::Warn : ValueTone::Muted, kMessages, RowAffordance::Open));
        rows.push_back(ListRow::item("Contacts", contacts_known_ ? ret::contacts_summary(static_cast<int>(contacts_.size())) : "",
                                     ValueTone::Muted, kContacts, RowAffordance::Open));
        bool sending = false;
        for (const ret::Iface &i : ifaces_) sending = sending || (i.enabled && i.outgoing);
        rows.push_back(ListRow::item("Connections", ret::connections_value(ifaces_),
                                     vt(sending ? ret::Tone::Warn : on ? ret::Tone::Ok : ret::Tone::Muted), kConnections,
                                     RowAffordance::Open));
        const int heard = heard_.count_within(wall_s(), 3600);
        rows.push_back(ListRow::item("Status", ret::heard_hour_summary(heard), vt(heard ? ret::Tone::Normal : ret::Tone::Muted),
                                     kStatus, RowAffordance::Open));
        /* short: the label must stay whole on a 480 px screen (the design board says "keep running: off") */
        std::string keep = keep_known_ ? (keep_running_ ? "keeps running" : "stops on exit") : "";
        rows.push_back(ListRow::item("Settings", keep, vt(keep_known_ && keep_running_ ? ret::Tone::Ok : ret::Tone::Muted),
                                     kSettings, RowAffordance::Open));
    } else if (svc_ == Svc::Failed || svc_ == Svc::Stopped) {
        rows.push_back(ListRow::item(svc_ == Svc::Stopped ? "Start the service" : "Try again", "", ValueTone::Normal, kRetry,
                                     RowAffordance::Action));
    } else {
        const std::int64_t s = (now_ms() - svc_since_ms_) / 1000;
        rows.push_back(ListRow::item("Starting the service", s > 0 ? std::to_string(s) + " s" : "", ValueTone::Muted, kNone,
                                     RowAffordance::Info));
    }
    return rows;
}

std::vector<ListRow> App::rows_connections() const
{
    std::vector<ListRow> rows;
    rows.push_back(ListRow::section("Your connections"));
    int n = 0;
    for (const ret::Iface &i : ifaces_) {
        const ret::State st = ret::conn_state(i);
        rows.push_back(ListRow::item(ret::conn_title(i), st.word, vt(st.tone), kConn + n, RowAffordance::Open));
        ++n;
    }
    if (ifaces_.empty())
        rows.push_back(ListRow::item("No connections yet", "", ValueTone::Muted, kNone, RowAffordance::Info));
    rows.push_back(ListRow::section("Add"));
    rows.push_back(ListRow::item("Add a connection", "", ValueTone::Normal, kAdd, RowAffordance::Action));
    return rows;
}

void App::add_option_rows(std::vector<ListRow> &rows, std::size_t from, std::size_t to) const
{
    const ret::Iface *c = drafting_ ? nullptr : current_iface();
    for (std::size_t idx = from; idx < to && idx < form_opts_.size(); ++idx) {
        const ret::OptionDef &def = form_opts_[idx];
        std::string v;
        bool present = false, secret_set = false;
        if (drafting_) {
            const auto it = draft_vals_.find(def.name);
            if (it != draft_vals_.end() && !it->second.empty()) {
                v = it->second;
                present = true;
                secret_set = def.secret;
            }
        } else if (c) {
            v = c->value(def.name, &present);
            secret_set = c->secret_set(def.name);
        }
        const ret::OptText t = ret::option_text(def, present ? &v : nullptr, secret_set);
        rows.push_back(ListRow::item(ret::option_label(def.name), t.text, vt(t.tone), kOpt + static_cast<std::intptr_t>(idx),
                                     RowAffordance::Edit));
    }
}

std::string App::connection_top(ret::Tone *tone) const
{
    const ret::Iface *i = current_iface();
    if (!i) return std::string();
    const ret::State st = ret::conn_state(*i);
    *tone = st.tone;
    std::string t = "connection: " + st.word;
    return t;
}

std::vector<ListRow> App::rows_connection() const
{
    std::vector<ListRow> rows;
    const ret::Iface *i = current_iface();
    if (!i) return rows;
    rows.push_back(ListRow::item("Switch on", i->enabled ? "on" : "off", vt(i->enabled ? ret::Tone::Ok : ret::Tone::Muted),
                                 kSwitchOn, RowAffordance::Edit));
    /* a kind with no listen-only state (Wi-Fi / LAN) is on or off: there is no Allow sending to choose */
    if (ret::has_listen_choice(*i))
        rows.push_back(ListRow::item("Allow sending", i->outgoing ? "yes" : "no", vt(i->outgoing ? ret::Tone::Warn : ret::Tone::Muted),
                                     kAllowSend, RowAffordance::Edit));
    else
        rows.push_back(ListRow::item("Listen only", "not possible", ValueTone::Muted, kNone, RowAffordance::Info));
    if (!i->outgoing && i->enabled && !ret::listen_note(*i).empty()) rows.push_back(ListRow::section(ret::listen_note(*i)));
    if (i->enabled && !i->error.empty())
        rows.push_back(ListRow::item("Problem", "details", ValueTone::Bad, kProblem, RowAffordance::Open));
    if (i->supported) {
        if (form_type_n_ > 0) {
            rows.push_back(ListRow::section(ret::kind_info(i->type).section));
            if (!ret::kind_info(i->type).note.empty())
                rows.push_back(ListRow::section(ret::kind_info(i->type).note));
            add_option_rows(rows, 0, form_type_n_);
        }
        if (form_opts_.size() > form_type_n_) {
            rows.push_back(ListRow::section("More options"));
            add_option_rows(rows, form_type_n_, form_opts_.size());
        }
    } else {
        rows.push_back(ListRow::item("Set up by hand: not changed here", "", ValueTone::Muted, kNone, RowAffordance::Info));
    }
    rows.push_back(ListRow::section("Remove"));
    rows.push_back(ListRow::item("Remove connection", "", ValueTone::Normal, kRemove, RowAffordance::Action));
    return rows;
}

std::vector<ListRow> App::rows_add_kind(bool more) const
{
    std::vector<ListRow> rows;
    rows.push_back(ListRow::section(more ? "Other kinds" : "What do you want to connect with?"));
    if (!more) {
        std::intptr_t n = 0;
        for (const ret::KindInfo *k : ret::kinds_main())
            rows.push_back(ListRow::item(k->title, k->summary, ValueTone::Muted, kKind + n++, RowAffordance::Open));
        rows.push_back(ListRow::item("More...", "other kinds", ValueTone::Muted, kMore, RowAffordance::Open));
    } else {
        std::intptr_t n = 0;
        for (const ret::KindInfo *k : ret::kinds_more(catalog_))
            rows.push_back(ListRow::item(k->title, k->summary, ValueTone::Muted, kKind + n++, RowAffordance::Open));
    }
    return rows;
}

std::vector<ListRow> App::rows_form() const
{
    std::vector<ListRow> rows;
    rows.push_back(ListRow::section("New connection"));
    rows.push_back(ListRow::item("Name", draft_name_, ValueTone::Normal, kName, RowAffordance::Edit));
    if (!form_opts_.empty()) {
        rows.push_back(ListRow::section(ret::kind_info(draft_type_).section));
        if (!ret::kind_info(draft_type_).note.empty())
            rows.push_back(ListRow::section(ret::kind_info(draft_type_).note));
        add_option_rows(rows, 0, form_opts_.size());
    }
    rows.push_back(ListRow::section("Save"));
    rows.push_back(ListRow::item("Save connection", "", ValueTone::Normal, kSave, RowAffordance::Action));
    return rows;
}

std::vector<ListRow> App::rows_status() const
{
    std::vector<ListRow> rows;
    rows.push_back(ListRow::section("This deck"));
    rows.push_back(ListRow::item("Service", svc_ == Svc::Up ? "running" : "not running",
                                 info_vt(svc_ == Svc::Up ? ret::Tone::Ok : ret::Tone::Bad), kNone, RowAffordance::Info));
    rows.push_back(ListRow::item("My address", deck_.address.empty() ? "not available" : ret::short_address(deck_.address),
                                 ValueTone::Normal, kNone, RowAffordance::Info));
    rows.push_back(ListRow::section("Connections"));
    for (const ret::Iface &i : ifaces_) {
        const ret::State st = ret::conn_state(i);
        rows.push_back(ListRow::item(ret::conn_title(i), st.word, info_vt(st.tone), kNone, RowAffordance::Info));
        if (i.enabled && i.online && i.has_channel)
            rows.push_back(ListRow::item("  Channel busy", pct_text(i.channel_load), ValueTone::Normal, kNone, RowAffordance::Info));
        if (i.enabled && i.online && i.has_noise)
            rows.push_back(ListRow::item("  Noise", dbm_text(i.noise), ValueTone::Normal, kNone, RowAffordance::Info));
    }
    if (ifaces_.empty())
        rows.push_back(ListRow::item("No connections yet", "", ValueTone::Muted, kNone, RowAffordance::Info));
    rows.push_back(ListRow::section("Heard recently"));
    const double now = wall_s();
    if (heard_.items().empty()) {
        rows.push_back(ListRow::item("Nobody yet", "0 in the last hour", ValueTone::Muted, kNone, RowAffordance::Info));
    } else {
        int shown = 0;
        for (const ret::Heard &h : heard_.items()) {
            if (shown++ >= 8) break;
            std::string who = h.name.empty() ? ret::short_address(h.destination) : h.name;
            std::string how = h.hops >= 0 ? std::to_string(h.hops) + (h.hops == 1 ? " hop, " : " hops, ") : std::string();
            rows.push_back(ListRow::item(who, how + ret::ago_text(now - h.time), ValueTone::Normal, kNone,
                                         RowAffordance::Info));
        }
    }
    return rows;
}

std::vector<ListRow> App::rows_settings() const
{
    std::vector<ListRow> rows;
    rows.push_back(ListRow::section("Background"));
    std::string keep = keep_busy_ ? "working..." : (keep_known_ ? (keep_running_ ? "on" : "off") : "...");
    rows.push_back(ListRow::item("Keep running", keep, vt(keep_known_ && keep_running_ ? ret::Tone::Ok : ret::Tone::Muted),
                                 kKeep, RowAffordance::Edit));
    rows.push_back(ListRow::item("New message alerts", deck_.alerts ? "on" : "off", vt(deck_.alerts ? ret::Tone::Ok : ret::Tone::Muted),
                                 kAlerts, RowAffordance::Edit));
    rows.push_back(ListRow::section("Me"));
    rows.push_back(ListRow::item("Name", deck_.display_name.empty() ? "not set" : deck_.display_name,
                                 vt(deck_.display_name.empty() ? ret::Tone::Warn : ret::Tone::Normal), kDisplayName,
                                 RowAffordance::Edit));
    rows.push_back(ListRow::item("My address", deck_.address.empty() ? "not available" : ret::short_address(deck_.address),
                                 vt(deck_.address.empty() ? ret::Tone::Muted : ret::Tone::Ok), kAddress, RowAffordance::Open));
    rows.push_back(ListRow::section("Service"));
    rows.push_back(ListRow::item("Stop the service", "", ValueTone::Normal, kStop, RowAffordance::Action));
    rows.push_back(ListRow::section("Typing"));
    std::string osk = "unknown";
    if (cdk::osk_policy::Policy *p = cdk::osk_policy::installed()) {
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        const std::int64_t now = static_cast<std::int64_t>(ts.tv_sec) * 1000 + ts.tv_nsec / 1000000;
        if (p->allowed(now)) osk = "shows when you type";
        else if (p->input().keyboards > 0) osk = "a keyboard is connected";
        else osk = "off";
    }
    rows.push_back(ListRow::item("On-screen keyboard", osk, ValueTone::Normal, kOsk, RowAffordance::Info));
    rows.push_back(ListRow::item("Version", CDK_APP_VERSION, ValueTone::Normal, kVersion, RowAffordance::Info));
    return rows;
}

std::vector<ListRow> App::rows_choice() const
{
    std::vector<ListRow> rows;
    std::intptr_t n = 0;
    for (const Choice &c : choices_) {
        if (c.info) rows.push_back(ListRow::item(c.label, "", ValueTone::Muted, kNone, RowAffordance::Info));
        else
            rows.push_back(ListRow::item(c.label, c.current ? "current" : "", c.current ? ValueTone::Ok : ValueTone::Normal,
                                         kChoice + n, RowAffordance::Action));
        ++n;
    }
    return rows;
}

/* ------------------------------------------------------------------ what a row does */

void App::activate(std::intptr_t id)
{
    if (id == kNone) return;
    switch (nav_.current()) {
    case ret::View::Home: activate_home(id); break;
    case ret::View::Connections: activate_connections(id); break;
    case ret::View::Connection: activate_connection(id); break;
    case ret::View::AddKind:
    case ret::View::AddMore: activate_add_kind(id); break;
    case ret::View::AddForm: activate_form(id); break;
    case ret::View::Status: activate_status(id); break;
    case ret::View::Settings: activate_settings(id); break;
    case ret::View::Choice: activate_choice(id); break;
    case ret::View::Contacts: activate_contacts(id); break;
    case ret::View::Contact: activate_contact(id); break;
    case ret::View::AddContact: activate_add_contact(id); break;
    case ret::View::ConvOptions: activate_conv_options(id); break;
    default: break;
    }
}

void App::activate_home(std::intptr_t id)
{
    switch (id) {
    case kConnections: open(ret::View::Connections); break;
    case kMessages:
        request_convs();
        open(ret::View::Messages);
        break;
    case kContacts: open_contacts(false); break;
    case kStatus:
        request_status();
        open(ret::View::Status);
        break;
    case kSettings:
        request_status();
        check_keep_running();
        open(ret::View::Settings);
        break;
    case kRetry: begin_service(); break;
    default: break;
    }
}

void App::activate_connections(std::intptr_t id)
{
    if (id >= kConn && id < kConn + 100) {
        const std::size_t idx = static_cast<std::size_t>(id - kConn);
        if (idx < ifaces_.size()) open_connection(ifaces_[idx].name);
    } else if (id == kAdd) {
        if (!catalog_.loaded) {
            link_->request("iface.types", ret::json::Value::object(), 5.0, [this](const ret::Response &r) {
                if (r.ok) ret::parse_catalog(r.result, catalog_);
            });
            notice("Not ready yet", "The service has not sent its list of connection kinds yet. Try again in a moment.");
            return;
        }
        open(ret::View::AddKind);
    }
}

void App::open_connection(const std::string &name)
{
    cur_name_ = name;
    drafting_ = false;
    form_opts_.clear();
    form_type_n_ = 0;
    const ret::Iface *i = current_iface();
    if (i && i->supported) {
        if (const ret::TypeDef *t = catalog_.find(i->type)) {
            form_opts_ = t->options;
            form_type_n_ = form_opts_.size();
            for (const ret::OptionDef &c : catalog_.common) form_opts_.push_back(c);
        }
    }
    open(ret::View::Connection);
}

void App::activate_add_kind(std::intptr_t id)
{
    if (id == kMore) {
        open(ret::View::AddMore);
        return;
    }
    if (id < kKind) return;
    const std::size_t idx = static_cast<std::size_t>(id - kKind);
    const bool more = nav_.current() == ret::View::AddMore;
    const std::vector<const ret::KindInfo *> kinds = more ? ret::kinds_more(catalog_) : ret::kinds_main();
    if (idx >= kinds.size()) return;
    begin_form(kinds[idx]->type);
}

void App::activate_form(std::intptr_t id)
{
    if (id == kName) {
        open_text(Edit::DraftName, "Name", "Name", draft_name_, cp0::ui::TextType::Text, 4 * 48,
                  "Letters, digits, spaces and . ( ) + - _ (up to 48).");
    } else if (id == kSave) {
        save_draft();
    } else if (const ret::OptionDef *def = option_def(id)) {
        edit_option(*def);
    }
}

void App::activate_connection(std::intptr_t id)
{
    if (id == kSwitchOn) toggle_enabled();
    else if (id == kAllowSend) toggle_sending();
    else if (id == kRemove) confirm_remove();
    else if (id == kProblem) {
        if (const ret::Iface *i = current_iface()) notice("Problem", ret::plainify(i->error));
    } else if (const ret::OptionDef *def = option_def(id)) edit_option(*def);
}

void App::activate_status(std::intptr_t) {}

void App::activate_settings(std::intptr_t id)
{
    switch (id) {
    case kKeep:
        if (keep_known_ && !keep_busy_) set_keep_running(!keep_running_);
        break;
    case kAlerts: set_alerts(!deck_.alerts); break;
    case kDisplayName:
        open_text(Edit::DisplayName, "Name", "Name", deck_.display_name, cp0::ui::TextType::Text, 4 * 64,
                  "The name others see next to your address. Saving it sends nothing.");
        break;
    case kAddress:
        if (deck_.address.empty()) notice("No address yet", "The service has not given this deck an address.");
        else open_address();
        break;
    case kStop:
        confirm("Stop the service?", "Nothing is received while it is stopped. The app starts it again the next time you open it.",
                "Stop", [this] { stop_service(); });
        break;
    default: break;
    }
}

void App::activate_choice(std::intptr_t id)
{
    if (id < kChoice) return;
    const std::size_t idx = static_cast<std::size_t>(id - kChoice);
    if (idx < choices_.size() && !choices_[idx].info) apply_choice(choices_[idx]);
}

void App::open_address()
{
    request_status();
    open(ret::View::Address);
}

} // namespace retui
