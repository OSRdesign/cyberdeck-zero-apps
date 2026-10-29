/*
 * SPDX-License-Identifier: MIT
 *
 * Net Toolbox: the tool pages (see toolbox_app.hpp): Interfaces, Diagnosis, Ping, DNS lookup, Port check. Each builds
 * its panel and its list from the layout service's tokens, takes the snapshots of its runner on the 100 ms timer and
 * hands them to view_model.hpp for the words.
 */
#include "toolbox_app.hpp"

#include "style.hpp"
#include "toolbox.hpp"

#include "cp0_ui_metrics_lvgl.h"

#include "netaddr.hpp"
#include "text.hpp"

#include <algorithm>
#include <cstdio>

namespace nt {

namespace {

constexpr int kMsgLines = 4; /* lines of the explanation under the ladder */

/* a row of a CompactList page: Action (does it right here; ActionRunning while it runs), Edit (changes a value, in place
 * or through a picker) or Open (another page) */
cp0::ui::ListRow action(const std::string &label, const std::string &value, std::intptr_t id, ui::Aff aff)
{
    return cp0::ui::ListRow::item(label, value, cp0::ui::ValueTone::Normal, id, row_affordance(aff));
}

} // namespace

/* ---------------------------------------------------------------- Interfaces */

/* The answers of `iw` go on the Wi-Fi interfaces that are connected; the answer of one that is not (any more) is dropped, a
 * stale network name must not show. */
void ToolboxApp::apply_links()
{
    for (auto it = links_.begin(); it != links_.end();) {
        const Iface *i = net_.find(it->first);
        if (!i || !i->wireless || !i->connected()) it = links_.erase(it);
        else {
            apply_iw_link(net_, it->first, it->second);
            ++it;
        }
    }
}

/* The Wi-Fi network name and bit rate of every connected wireless interface: the fixture's recorded `iw` output, or `iw`
 * itself through async_run. One job at a time, never waited for: the page asks every interface once when it opens (one
 * after the other), then one interface per refresh in turn, so a second adapter costs one more `iw` every other refresh,
 * never a pile-up. */
void ToolboxApp::request_links(bool full_round)
{
    std::vector<std::string> names;
    for (const Iface &i : net_.ifaces)
        if (i.wireless && i.connected()) names.push_back(i.name);
    if (backend_.source) {
        for (const std::string &n : names) {
            IwLink link;
            if (backend_.fixture_link(n, link)) links_[n] = link;
            else links_.erase(n);
        }
        return;
    }
    if (iw_job_ || names.empty()) return;
    iw_round_left_ = full_round ? static_cast<int>(names.size()) : 1;
    ask_link();
}

void ToolboxApp::ask_link()
{
    std::vector<std::string> names;
    for (const Iface &i : net_.ifaces)
        if (i.wireless && i.connected()) names.push_back(i.name);
    if (iw_job_ || names.empty() || iw_round_left_ <= 0) return;
    const std::string name = names[iw_cursor_ % names.size()];
    ++iw_cursor_;
    --iw_round_left_;
    const std::shared_ptr<bool> alive = alive_;
    const auto finished = std::make_shared<bool>(false);
    const cdk::async_run::JobId id = request_wifi_link(name, [this, alive, finished, name](const IwLink &link, bool ran) {
        if (!*alive) return;
        *finished = true;
        iw_job_ = 0;
        if (ran) links_[name] = link;
        else links_.erase(name);
        if (stack_.back().view != View::Interfaces || !list_) {
            iw_round_left_ = 0;
            return;
        }
        apply_links();
        list_->set_rows(to_rows(ui::interface_rows(net_, scope_)));
        ask_link(); /* the next interface of the round */
    });
    if (!*finished) iw_job_ = id;
}

void ToolboxApp::build_interfaces()
{
    const Geo g = geo();
    read_net();
    request_links(true);
    apply_links();
    rows_density_ = CP0_UI_LIST_COMPACT;
    build_list(CP0_UI_LIST_COMPACT, g.top, g.bottom - g.top, true, to_rows(ui::interface_rows(net_, scope_)), 0);
    next_state_ms_ = now_ms() + 2000;
}

void ToolboxApp::refresh_interfaces()
{
    next_state_ms_ = now_ms() + 2000;
    read_net();
    request_links(false);
    apply_links();
    if (list_) list_->set_rows(to_rows(ui::interface_rows(net_, scope_)));
}

/* ---------------------------------------------------------------- Diagnosis */

void ToolboxApp::build_diagnosis()
{
    const Geo g = geo();
    const int y = build_band();
    const int avail = g.bottom - y;
    int msg_lines = kMsgLines;
    int msg_h = (1 + msg_lines) * g.lh; /* the head and the explanation, never cut */
    const int pref = g.lh * 7 / 4;
    const auto rung_for = [&](int list_h) { return (avail - msg_h - 2 * g.s - list_h) / kStepCount; };
    cp0_ui_list_density_t density = CP0_UI_LIST_COMFORTABLE;
    const cp0_ui_metrics_t *m = cp0_ui_metrics_get();
    const lv_font_t *font = ui::font_medium(m->text_px[CP0_TEXT_BODY]);
    const int x = g.x + g.inset, w = std::max(1, g.w - 2 * g.inset);
    int rung = 0;
    if (portrait()) {
        /* a narrow tall screen: each rung is two lines (the name, its detail under it over the whole width: an address, a
         * resolver and a time are never cut), and the explanation has two more lines when the screen has the room */
        const lv_font_t *caption = ui::caption_font();
        const int two_cap = ui::HealthLadder::two_line_rung_h(caption, caption);
        const int two_body = ui::HealthLadder::two_line_rung_h(font, caption);
        msg_lines = kMsgLines + 2;
        msg_h = (1 + msg_lines) * g.lh;
        rung = rung_for(2 * g.row);
        if (rung < two_cap) {
            density = CP0_UI_LIST_COMPACT;
            rung = rung_for(2 * g.crow);
        }
        if (rung < two_cap) {
            msg_lines = kMsgLines;
            msg_h = (1 + msg_lines) * g.lh;
            rung = rung_for(density == CP0_UI_LIST_COMPACT ? 2 * g.crow : 2 * g.row);
        }
        rung = std::clamp(rung, two_cap, two_body + g.s);
        if (rung < two_body) font = caption;
        ladder_ = std::make_unique<ui::HealthLadder>(page_->root(), x, y, w, rung, font, true, caption);
    } else {
        rung = rung_for(2 * g.row);
        if (rung < pref) {
            density = CP0_UI_LIST_COMPACT;
            rung = rung_for(2 * g.crow);
        }
        rung = std::clamp(rung, g.lh + 4, 2 * g.lh + g.s);
        if (ui::line_height(font) + 4 > rung) font = ui::caption_font();
        ladder_ = std::make_unique<ui::HealthLadder>(page_->root(), x, y, w, rung, font);
    }
    const int msg_y = y + rung * kStepCount + g.s;
    message_head_ = ui::make_label(page_->root(), ui::font_bold(ui::caption_px()), ui::kLabel);
    lv_label_set_long_mode(message_head_, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_pos(message_head_, x, msg_y);
    lv_obj_set_size(message_head_, w, g.lh);
    message_body_ = ui::make_label(page_->root(), ui::caption_font(), ui::kText);
    lv_label_set_long_mode(message_body_, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_pos(message_body_, x, msg_y + g.lh);
    lv_obj_set_size(message_body_, w, msg_lines * g.lh);

    const int list_y = msg_y + msg_h + g.s;
    build_list(density, list_y, g.bottom - list_y, false, diag_rows(), 0);
    backend_.diag->start(diag_options()); /* by itself on entry */
    fresh_ = true;
    poll_diagnosis(true);
}

/* the diagnosis checks the chosen network (Automatic: the main interface, as before) */
DiagOptions ToolboxApp::diag_options() const
{
    DiagOptions opt;
    opt.iface = scope_.iface;
    return opt;
}

std::vector<cp0::ui::ListRow> ToolboxApp::diag_rows() const
{
    const std::shared_ptr<const DiagReport> r = fresh_ ? backend_.diag->latest() : nullptr;
    const bool running = r && r->state == DiagReport::State::Running;
    return {action("Run again", running ? "Checking..." : "", kRunAgain, ui::action_aff(running)),
            action("Save log", "", kSaveLog, ui::Aff::Action)};
}

void ToolboxApp::poll_diagnosis(bool force)
{
    const std::shared_ptr<const DiagReport> snap = fresh_ ? backend_.diag->latest() : nullptr;
    ui::Band band;
    if (snap) {
        const std::uint64_t seq = backend_.diag->seq();
        if (force || seq != shown_seq_) {
            shown_seq_ = seq;
            if (ladder_) ladder_->update(*snap, now_ms());
            const ui::DiagMessage msg = ui::diag_message(*snap);
            ui::set_text_if(message_head_, msg.head);
            ui::set_color_if(message_head_, ui::tone_rgb(msg.tone, ui::kLabel));
            ui::set_text_if(message_body_, msg.body);
            /* the explanation is the point of the page: never cut. It takes the caption size, or a smaller one when it
             * would need more lines than its block has (a long sign-in address) */
            const int room_w = lv_obj_get_width(message_body_), room_h = lv_obj_get_height(message_body_);
            const lv_font_t *body_font = ui::caption_font();
            for (int px = ui::caption_px(); px >= 12; px -= 2) {
                body_font = ui::font_medium(px);
                lv_point_t size{};
                lv_text_get_size(&size, msg.body.c_str(), body_font, 0, 0, room_w, LV_TEXT_FLAG_NONE);
                if (size.y <= room_h) break;
            }
            lv_obj_set_style_text_font(message_body_, body_font, 0);
            msg_cut_ = false;
            if (backend_.demo) { /* the scenes: nothing of the message may be cut (never an ellipsis) */
                lv_point_t size{};
                lv_text_get_size(&size, msg.body.c_str(), body_font, 0, 0, room_w, LV_TEXT_FLAG_NONE);
                const int head_w = ui::text_width(msg.head.c_str(), ui::font_bold(ui::caption_px()));
                msg_cut_ = size.y > room_h || head_w > lv_obj_get_width(message_head_);
                if (msg_cut_) std::fprintf(stderr, "[net-toolbox] the diagnosis message does not fit its block\n");
            }
            if (list_) list_->set_rows(diag_rows());
        }
        if (!snap->resolver.empty()) band.text = "DNS asked " + snap->resolver;
        if (msg_cut_) {
            band.text = "Message cut off";
            band.tone = ui::Tone::Bad;
        }
    }
    show_band(band);
    if (msg_cut_ && band_) { /* a layout error wins over a note */
        ui::set_text_if(band_, "Message cut off");
        ui::set_color_if(band_, ui::kBad);
    }
}

void ToolboxApp::activate_diagnosis(std::intptr_t id)
{
    if (id == kRunAgain) {
        backend_.diag->start(diag_options());
        fresh_ = true;
        shown_seq_ = 0;
    } else if (id == kSaveLog) {
        save_result();
    }
    poll_diagnosis(true);
}

/* ---------------------------------------------------------------- Ping */

std::string ToolboxApp::ping_target_text() const
{
    if (!ping_target_.empty()) return ping_target_;
    return gateway().empty() ? std::string("Gateway (none)") : "Gateway " + gateway();
}

void ToolboxApp::build_ping(int select)
{
    const Geo g = geo();
    read_net();
    const PingAvailability avail_ping = backend_.ping_available();
    ping_ok_ = avail_ping.ipv4;
    ping_why_ = avail_ping.why;
    const int y = build_band();
    const int avail = g.bottom - y;
    const lv_font_t *font = ui::caption_font();
    const int panel_min = ui::PingPanel::height_for(4 * g.lh, font);
    cp0_ui_list_density_t density = CP0_UI_LIST_COMPACT;
    int row = g.crow;
    if (avail - panel_min - g.s >= 2 * g.row) {
        density = CP0_UI_LIST_COMFORTABLE;
        row = g.row;
    }
    const int list_h = std::max(row, std::min(4 * row, avail - panel_min - g.s));
    const int panel_h = avail - g.s - list_h;
    const int x = g.x + g.inset, w = std::max(1, g.w - 2 * g.inset);
    if (ping_ok_) ping_panel_ = std::make_unique<ui::PingPanel>(page_->root(), x, y, w, panel_h, font);
    else notice_ = std::make_unique<ui::NoticeBox>(page_->root(), x, y, w, panel_h, font, "Ping is not available here", ping_why_);
    rows_density_ = density;
    build_list(density, y + panel_h + g.s, list_h, false, ping_rows(), select);
    poll_ping(true);
}

std::vector<cp0::ui::ListRow> ToolboxApp::ping_rows() const
{
    const std::shared_ptr<const PingSnapshot> s = fresh_ ? backend_.ping->latest() : nullptr;
    const bool running = s && s->running();
    std::string target = ping_target_text();
    /* portrait: "Gateway 192.168.68.1" is said "Gateway" when the label would be cut (the status line has the address) */
    if (portrait() && ping_target_.empty() && !list_row_fits("Target", target, ui::Aff::Edit)) target = "Gateway";
    return {action(running ? "Stop" : "Start", running ? "pinging" : "", kRun, ui::action_aff(running)),
            action("Target", fit_value("Target", target, ui::Aff::Edit), kTarget, ui::Aff::Edit),
            action("Save log", "", kSaveLog, ui::Aff::Action),
            action("Save target", "", kSaveTarget, ui::Aff::Action)};
}

void ToolboxApp::poll_ping(bool force)
{
    const std::shared_ptr<const PingSnapshot> snap = fresh_ ? backend_.ping->latest() : nullptr;
    const std::uint64_t seq = backend_.ping->seq();
    if (force || seq != shown_seq_) {
        shown_seq_ = seq;
        if (ping_panel_) ping_panel_->update(snap ? *snap : PingSnapshot());
        if (list_) list_->set_rows(ping_rows());
    }
    ui::Band band;
    if (!ping_ok_) {
        band.text = "Ping needs an ICMP socket: see the notice";
        band.tone = ui::Tone::Warn;
    } else if (snap) {
        band = ui::ping_band(*snap);
    } else {
        band.text = "Press Start to ping " + ping_target_text();
    }
    show_band(band);
}

void ToolboxApp::start_ping()
{
    if (!ping_ok_) {
        note({"Ping is not available here: see the notice", ui::Tone::Warn});
        return;
    }
    const std::string host = ping_target_.empty() ? gateway() : ping_target_;
    if (host.empty()) {
        note({"No gateway: connect to a network first", ui::Tone::Warn});
        return;
    }
    PingConfig cfg;
    cfg.target = host;
    cfg.interval_ms = settings_.ping_interval_ms;
    cfg.timeout_ms = settings_.ping_timeout_ms;
    cfg.count = settings_.ping_count;
    cfg.resolve.server = pick_resolver("", dns_servers());
    backend_.ping->start(cfg);
    fresh_ = true;
    shown_seq_ = 0;
}

void ToolboxApp::open_ping_target()
{
    Choice c;
    c.title = "Target";
    c.labels.push_back(gateway().empty() ? "Gateway (none)" : "Gateway " + gateway());
    c.labels.push_back("1.1.1.1 Cloudflare");
    c.labels.push_back("8.8.8.8 Google");
    for (const std::string &t : settings_.targets) c.labels.push_back(t);
    c.labels.push_back("Type a host...");
    const int saved = static_cast<int>(settings_.targets.size());
    c.current = static_cast<int>(c.labels.size()) - 1;
    if (ping_target_.empty()) c.current = 0;
    else if (ping_target_ == "1.1.1.1") c.current = 1;
    else if (ping_target_ == "8.8.8.8") c.current = 2;
    else
        for (int i = 0; i < saved; ++i)
            if (settings_.targets[static_cast<std::size_t>(i)] == ping_target_) c.current = 3 + i;
    c.chosen = [this, saved](int i) {
        if (i == 0) ping_target_.clear();
        else if (i == 1) ping_target_ = "1.1.1.1";
        else if (i == 2) ping_target_ = "8.8.8.8";
        else if (i >= 3 && i < 3 + saved) ping_target_ = settings_.targets[static_cast<std::size_t>(i - 3)];
        else {
            Typed t;
            t.title = "Host";
            t.label = "Host";
            t.text = ping_target_;
            t.placeholder = "name or address";
            t.check = [](const std::string &text) {
                std::string why;
                return valid_host(text, &why) ? std::string() : why;
            };
            t.done = [this](const std::string &text) {
                ping_target_ = text;
                fresh_ = false;
                save_settings_now();
            };
            open_typed(std::move(t));
            return;
        }
        fresh_ = false;
        save_settings_now();
    };
    open_choice(std::move(c));
}

void ToolboxApp::activate_ping(std::intptr_t id)
{
    if (id == kRun) {
        const std::shared_ptr<const PingSnapshot> s = fresh_ ? backend_.ping->latest() : nullptr;
        if (s && s->running()) backend_.ping->stop();
        else start_ping();
    } else if (id == kTarget) {
        open_ping_target();
        return;
    } else if (id == kSaveLog) {
        save_result();
    } else if (id == kSaveTarget) {
        const std::string host = ping_target_;
        if (host.empty() || host == "1.1.1.1" || host == "8.8.8.8") {
            note({host.empty() ? "The gateway is always in the list" : host + " is always in the list", ui::Tone::Normal});
        } else if (std::find(settings_.targets.begin(), settings_.targets.end(), host) != settings_.targets.end()) {
            note({host + " is already saved", ui::Tone::Normal});
        } else if (settings_.add_target(host)) {
            save_settings_now();
            note({"Saved target " + host, ui::Tone::Ok});
        } else {
            note({"Cannot save " + host + " as a target", ui::Tone::Bad});
        }
    }
    poll_ping(true);
}

/* ---------------------------------------------------------------- DNS lookup */

/* "System default 192.168.1.1"; with a chosen network the label says whose servers they are: "wlan1's DNS 172.20.10.1",
 * "wlan1's gateway 172.20.10.1", "System default (not wlan1's) 192.168.1.1" */
std::string ToolboxApp::dns_resolver_label() const
{
    const std::string sys = system_resolver();
    const std::string name = scoped_dns().label;
    if (dns_resolver_.empty()) return sys.empty() ? name + " (none)" : name + " " + sys;
    return ui::resolver_label(dns_resolver_, sys, system_name());
}

void ToolboxApp::build_dns(int select)
{
    const Geo g = geo();
    read_net();
    const int y = build_band();
    const int h = g.bottom - y;
    rows_density_ = h >= 10 * g.row ? CP0_UI_LIST_COMFORTABLE : CP0_UI_LIST_COMPACT;
    build_list(rows_density_, y, h, false, dns_list(), select);
    poll_dns(true);
}

std::vector<cp0::ui::ListRow> ToolboxApp::dns_list() const
{
    const std::shared_ptr<const DnsSnapshot> s = fresh_ ? backend_.dns->latest() : nullptr;
    const bool running = s && s->state == DnsSnapshot::State::Running;
    std::vector<cp0::ui::ListRow> rows;
    rows.push_back(action("Look up", running ? "asking..." : "", kRun, ui::action_aff(running)));
    rows.push_back(action("Compare resolvers", "", kCompare, ui::Aff::Action));
    if (s) { /* the answers right under the two actions, the settings below them */
        std::vector<ui::RowDef> defs = ui::dns_rows(*s, system_resolver(), system_name());
        int k = 0;
        for (ui::RowDef &d : defs) {
            if (d.section) continue;
            d.id = kResult + k++;
            /* portrait: a Compare row whose resolver and time do not fit drops the resolver's address (its name stays) */
            if (portrait() && s->compare() && !list_row_fits(d.label, d.value, d.aff))
                for (const std::string &f : ui::resolver_label_forms(d.label, false)) {
                    d.label = f;
                    if (list_row_fits(f, d.value, d.aff)) break;
                }
        }
        for (cp0::ui::ListRow &r : to_rows(defs)) rows.push_back(std::move(r));
    }
    rows.push_back(action("Name", fit_value("Name", dns_name_, ui::Aff::Edit), kName, ui::Aff::Edit));
    rows.push_back(action("Type", dns_type_, kType, ui::Aff::Edit));
    std::string resolver = dns_resolver_label();
    if (portrait()) /* a narrow row: "System default 192.168.1.1" becomes "System 192.168.1.1", then the address alone */
        for (const std::string &f : ui::resolver_label_forms(resolver, true)) {
            resolver = f;
            if (list_row_fits("Resolver", f, ui::Aff::Edit)) break;
        }
    rows.push_back(action("Resolver", fit_value("Resolver", resolver, ui::Aff::Edit), kResolver, ui::Aff::Edit));
    rows.push_back(action("Save log", "", kSaveLog, ui::Aff::Action));
    return rows;
}

void ToolboxApp::poll_dns(bool force)
{
    const std::shared_ptr<const DnsSnapshot> snap = fresh_ ? backend_.dns->latest() : nullptr;
    const std::uint64_t seq = backend_.dns->seq();
    if (force || seq != shown_seq_) {
        shown_seq_ = seq;
        if (list_) list_->set_rows(dns_list());
    }
    show_band(snap ? ui::dns_status(*snap) : ui::Band{"Type a name and press Look up", ui::Tone::Normal});
}

void ToolboxApp::start_dns(bool compare)
{
    DnsRequest req;
    req.input = dns_name_;
    dns::type_from_name(dns_type_, req.type);
    const std::string sys = system_resolver();
    if (compare) {
        const auto add = [&req](const std::string &s) {
            if (!s.empty() && std::find(req.servers.begin(), req.servers.end(), s) == req.servers.end()) req.servers.push_back(s);
        };
        add(sys);
        for (const ResolverPreset &p : resolver_presets()) add(p.address);
        add(pick_resolver(dns_resolver_, dns_servers()));
    } else {
        const std::string server = pick_resolver(dns_resolver_, dns_servers());
        if (server.empty()) {
            note({"No resolver: the system has none, pick one", ui::Tone::Warn});
            return;
        }
        req.servers.push_back(server);
    }
    backend_.dns->start(req);
    fresh_ = true;
    shown_seq_ = 0;
}

void ToolboxApp::open_dns_type()
{
    Choice c;
    c.title = "Record type";
    const char *types[] = {"A", "AAAA", "MX", "TXT", "NS", "PTR"};
    for (int i = 0; i < 6; ++i) {
        c.labels.push_back(types[i]);
        if (dns_type_ == types[i]) c.current = i;
    }
    c.chosen = [this, types](int i) {
        if (i < 0 || i > 5) return;
        dns_type_ = types[i];
        fresh_ = false;
        save_settings_now();
    };
    open_choice(std::move(c));
}

void ToolboxApp::open_dns_resolver()
{
    Choice c;
    c.title = "Resolver";
    const std::string sys = system_resolver();
    const std::vector<ResolverPreset> &presets = resolver_presets();
    for (const ResolverPreset &p : presets) {
        if (!p.address[0]) c.labels.push_back(sys.empty() ? scoped_dns().label + " (none)" : scoped_dns().label + " " + sys);
        else c.labels.push_back(std::string(p.label) + " " + p.address);
    }
    c.labels.push_back("Type an address...");
    c.current = static_cast<int>(c.labels.size()) - 1;
    for (std::size_t i = 0; i < presets.size(); ++i)
        if (dns_resolver_ == presets[i].address) c.current = static_cast<int>(i);
    const int typed_index = static_cast<int>(presets.size());
    c.chosen = [this, typed_index](int i) {
        const std::vector<ResolverPreset> &p = resolver_presets();
        if (i >= 0 && i < typed_index) {
            dns_resolver_ = p[static_cast<std::size_t>(i)].address;
        } else {
            Typed t;
            t.title = "Resolver";
            t.label = "Address";
            t.text = dns_resolver_;
            t.placeholder = "for example 9.9.9.9";
            t.max_bytes = 64;
            t.check = [](const std::string &text) {
                return is_ip(text) ? std::string() : std::string("Type an IP address, for example 9.9.9.9");
            };
            t.done = [this](const std::string &text) {
                dns_resolver_ = text;
                fresh_ = false;
                save_settings_now();
            };
            open_typed(std::move(t));
            return;
        }
        fresh_ = false;
        save_settings_now();
    };
    open_choice(std::move(c));
}

void ToolboxApp::activate_dns(std::intptr_t id)
{
    if (id == kRun) {
        start_dns(false);
    } else if (id == kCompare) {
        start_dns(true);
    } else if (id == kName) {
        Typed t;
        t.title = "Name";
        t.label = "Name";
        t.text = dns_name_;
        t.placeholder = "example.com, or an address for PTR";
        t.check = [this](const std::string &text) {
            std::uint16_t type = 1;
            dns::type_from_name(dns_type_, type);
            std::string qname, why;
            return make_question(text, type, qname, &why) ? std::string() : why;
        };
        t.done = [this](const std::string &text) {
            dns_name_ = text;
            fresh_ = false;
            save_settings_now();
        };
        open_typed(std::move(t));
        return;
    } else if (id == kType) {
        open_dns_type();
        return;
    } else if (id == kResolver) {
        open_dns_resolver();
        return;
    } else if (id == kSaveLog) {
        save_result();
    }
    poll_dns(true);
}

/* ---------------------------------------------------------------- Port check */

void ToolboxApp::build_port(int select)
{
    const Geo g = geo();
    read_net();
    const int y = build_band();
    const lv_font_t *caption = ui::caption_font();
    const lv_font_t *big = ui::font_bold(cp0_ui_metrics_get()->text_px[CP0_TEXT_TITLE]);
    const int badge_h = ui::ResultBadge::height_for(caption, big, g.s);
    const int list_y = y + badge_h + g.s;
    const int list_h = g.bottom - list_y;
    badge_ = std::make_unique<ui::ResultBadge>(page_->root(), g.x + g.inset, y, std::max(1, g.w - 2 * g.inset), badge_h, caption, big);
    rows_density_ = list_h >= 2 * g.row ? CP0_UI_LIST_COMFORTABLE : CP0_UI_LIST_COMPACT;
    build_list(rows_density_, list_y, list_h, false, port_list(), select);
    poll_port(true);
}

std::vector<cp0::ui::ListRow> ToolboxApp::port_list() const
{
    const std::shared_ptr<const PortSnapshot> s = fresh_ ? backend_.port->latest() : nullptr;
    const bool running = s && (s->state == PortSnapshot::State::Running || s->state == PortSnapshot::State::Resolving);
    return {action("Run", running ? "checking..." : "", kRun, ui::action_aff(running)),
            action("Host", fit_value("Host", port_host_, ui::Aff::Edit), kHost, ui::Aff::Edit),
            action("Port", fit_value("Port", ui::port_choice_text(port_preset_, port_), ui::Aff::Edit), kPortRow, ui::Aff::Edit),
            action("Save log", "", kSaveLog, ui::Aff::Action)};
}

void ToolboxApp::poll_port(bool force)
{
    const std::shared_ptr<const PortSnapshot> snap = fresh_ ? backend_.port->latest() : nullptr;
    const std::uint64_t seq = backend_.port->seq();
    if (force || seq != shown_seq_) {
        shown_seq_ = seq;
        PortSnapshot idle;
        idle.host = port_host_;
        if (badge_) badge_->update(ui::port_summary(snap ? *snap : idle));
        if (list_) list_->set_rows(port_list());
    }
    ui::Band band;
    if (!snap) band.text = "Checks whether a TCP port answers; nothing is sent";
    show_band(band);
}

void ToolboxApp::start_port()
{
    PortRequest req;
    req.host = port_host_;
    req.ports.push_back(static_cast<std::uint16_t>(port_));
    if (!port_preset_.empty())
        for (const PortPreset &p : port_presets())
            if (port_preset_ == p.label) req.ports = p.ports;
    req.resolve.server = pick_resolver("", dns_servers());
    backend_.port->start(req);
    fresh_ = true;
    shown_seq_ = 0;
}

void ToolboxApp::open_port_choice()
{
    Choice c;
    c.title = "Port";
    const std::vector<PortPreset> &presets = port_presets();
    for (const PortPreset &p : presets) c.labels.push_back(ui::port_choice_text(p.label, 0));
    c.labels.push_back("Custom port...");
    c.current = static_cast<int>(c.labels.size()) - 1;
    for (std::size_t i = 0; i < presets.size(); ++i)
        if (port_preset_ == presets[i].label) c.current = static_cast<int>(i);
    const int custom = static_cast<int>(presets.size());
    c.chosen = [this, custom](int i) {
        const std::vector<PortPreset> &p = port_presets();
        if (i >= 0 && i < custom) {
            port_preset_ = p[static_cast<std::size_t>(i)].label;
            port_ = p[static_cast<std::size_t>(i)].ports[0];
        } else {
            Typed t;
            t.title = "Port";
            t.label = "Port";
            t.type = cp0::ui::TextType::Number;
            t.text = std::to_string(port_);
            t.placeholder = "1 to 65535";
            t.max_bytes = 5;
            t.check = [](const std::string &text) {
                int v = 0;
                return parse_int(text, v) && v >= 1 && v <= 65535 ? std::string() : std::string("A port is a number from 1 to 65535");
            };
            t.done = [this](const std::string &text) {
                int v = 0;
                if (!parse_int(text, v)) return;
                port_ = v;
                port_preset_.clear();
                fresh_ = false;
                save_settings_now();
            };
            open_typed(std::move(t));
            return;
        }
        fresh_ = false;
        save_settings_now();
    };
    open_choice(std::move(c));
}

void ToolboxApp::activate_port(std::intptr_t id)
{
    if (id == kRun) {
        start_port();
    } else if (id == kHost) {
        Typed t;
        t.title = "Host";
        t.label = "Host";
        t.text = port_host_;
        t.placeholder = "name or address";
        t.check = [](const std::string &text) {
            std::string why;
            return valid_host(text, &why) ? std::string() : why;
        };
        t.done = [this](const std::string &text) {
            port_host_ = text;
            fresh_ = false;
            save_settings_now();
        };
        open_typed(std::move(t));
        return;
    } else if (id == kPortRow) {
        open_port_choice();
        return;
    } else if (id == kSaveLog) {
        save_result();
    }
    poll_port(true);
}

} // namespace nt
