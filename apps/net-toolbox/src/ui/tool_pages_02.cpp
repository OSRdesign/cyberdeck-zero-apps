/*
 * SPDX-License-Identifier: MIT
 *
 * Net Toolbox 0.2: the new tool pages (see toolbox_app.hpp): Traceroute, HTTP check, Public IP, Whois. Like the 0.1 pages
 * each builds from the layout service's tokens, takes the snapshots of its runner on the 100 ms timer and hands them to
 * view_model.hpp for the words; the rows are RichList rows (rich_list.hpp) and the blocks chain.hpp's.
 */
#include "toolbox_app.hpp"

#include "app.hpp"
#include "style.hpp"
#include "toolbox.hpp"

#include "cp0_ui_metrics_lvgl.h"

#include "netaddr.hpp"
#include "text.hpp"

#include <algorithm>
#include <cstdio>

namespace nt {

namespace {

constexpr int kChainMinRows = 7; /* the chain shows at least this many hops at once on a 640x480 screen */

ui::RichRow action_row(const std::string &label, const std::string &value, std::intptr_t id)
{
    return ui::RichRow::item(label, value, ui::Tone::Normal, static_cast<int>(id));
}

} // namespace

/* ---------------------------------------------------------------- shared */

void ToolboxApp::focus_group(const std::vector<lv_obj_t *> &objs)
{
    lv_group_t *group = cdk::input_group();
    if (!group) return;
    for (lv_obj_t *o : objs)
        if (o && lv_obj_get_group(o) != group) lv_group_add_obj(group, o);
    if (!objs.empty() && objs[0]) lv_group_focus_obj(objs[0]);
}

void ToolboxApp::build_rich(int y, int h)
{
    const Geo g = geo();
    ui::RichListEvents events;
    events.activate = [this](int index, std::intptr_t id) { post([this, index, id] { activate(index, id); }); };
    events.back = [this] { post([this] { back(); }); };
    rich_ = std::make_unique<ui::RichList>(page_->root(), g.x, y, g.w, std::max(1, h), std::move(events));
    if (backend_.demo)
        std::fprintf(stderr, "[net-toolbox] list at y %d, %d high, rows %d px, screen %dx%d\n", y, h, rich_->layout().row_h,
                     layout_.w, layout_.h);
}

ui::RichRow ToolboxApp::rich_row(const ui::RowDef &def, int id) const
{
    if (def.section) return ui::RichRow::section(def.label);
    return ui::RichRow::item(def.label, def.value, def.tone, id);
}

std::string ToolboxApp::local_address() const
{
    if (!backend_.local_ip.empty()) return backend_.local_ip;
    const Iface *main = net_.main();
    return main ? main->first_ipv4() : std::string();
}

/* ---------------------------------------------------------------- Traceroute */

std::string ToolboxApp::trace_target_text() const
{
    if (!trace_target_.empty()) return trace_target_;
    return gateway().empty() ? std::string("Gateway (none)") : "Gateway " + gateway();
}

/* The page: the status line, a row of three large cells (Start / Stop, Target, Save log: ROW high, reachable with Tab, Left /
 * Right and a tap) and the chain under it. The actions take one short row, so the chain keeps the rest of the screen:
 * 7 hops at once on the 640x480 deck. Up from the first hop goes back to the cells, Down from them to the first hop. */
void ToolboxApp::build_trace()
{
    const Geo g = geo();
    net_ = backend_.read_state();
    band_check_ = true;
    const int y = build_band();
    const cp0_ui_metrics_t *m = cp0_ui_metrics_get();
    const lv_font_t *main = ui::caption_font();
    const lv_font_t *small = ui::font_medium(std::max(10, ui::caption_px() - 4));

    ui::ActionBarEvents bar_events;
    bar_events.activate = [this](int id) { post([this, id] { activate_trace(id); }); };
    bar_events.back = [this] { post([this] { back(); }); };
    bar_events.down = [this] {
        post([this] {
            if (rich_ && rich_->selected() >= 0) focus(rich_->focus_object());
        });
    };
    const std::vector<ui::ActionCell> cells = {{kRun, "Start", ""}, {kTarget, "Target", trace_target_text()}, {kSaveLog, "Save log", ""}};
    bar_ = std::make_unique<ui::ActionBar>(page_->root(), g.x, y, g.w, g.row, ui::font_medium(m->text_px[CP0_TEXT_BODY]), small, cells,
                                           std::move(bar_events));
    const int chain_y = y + g.row + g.s;
    build_rich(chain_y, g.bottom - chain_y);
    rich_->set_up_out([this] {
        post([this] {
            if (bar_) focus(bar_->cell_object(0));
        });
    });
    rich_->set_tab([this] {
        post([this] {
            if (bar_) focus(bar_->cell_object(0));
        });
    });
    /* the row: as low as the two lines of text need, as high as 7 rows leave room for, never over a compact list row */
    const int avail = g.bottom - chain_y;
    const int row_h = std::max(ui::ChainStyle::min_row_h(main, small), std::min(g.crow, avail / kChainMinRows));
    chain_style_ = ui::ChainStyle::make(rich_->layout().w, row_h, rich_->layout().inset, main, small);
    std::vector<lv_obj_t *> order = bar_->focus_objects();
    order.push_back(rich_->focus_object());
    focus_group(order);
    poll_trace(true);
}

void ToolboxApp::poll_trace(bool force)
{
    const std::shared_ptr<const TraceSnapshot> snap = fresh_ ? backend_.trace->latest() : nullptr;
    const std::uint64_t seq = backend_.trace->seq();
    if (force || seq != shown_seq_) {
        shown_seq_ = seq;
        const bool running = snap && snap->running();
        bar_->set_cell(0, running ? "Stop" : "Start", "");
        bar_->set_cell(1, "Target", trace_target_text());
        std::vector<ui::RichRow> rows;
        const std::size_t shown = snap ? std::min(snap->shown, snap->hops.size()) : 0;
        if (hop_views_.size() > shown) hop_views_.resize(shown);
        while (hop_views_.size() < shown) hop_views_.push_back(std::make_shared<ui::HopRowView>(chain_style_));
        if (hop_open_ >= static_cast<int>(shown)) hop_open_ = -1;
        const double scale = snap ? ui::chain_scale_ms(snap->hops, shown) : 1.0;
        for (std::size_t i = 0; i < shown; ++i) {
            const std::shared_ptr<ui::HopRowView> &v = hop_views_[i];
            v->set_expanded(static_cast<int>(i) == hop_open_);
            v->set(snap->hops[i], scale, i == 0, i + 1 == shown);
            rows.push_back(ui::RichRow::custom(v, v->height(), true, kHop + static_cast<int>(i)));
        }
        if (rows.empty() && snap && snap->state == TraceSnapshot::State::Failed)
            rows.push_back(ui::RichRow::text(snap->error, ui::Tone::Bad, false));
        else if (rows.empty())
            rows.push_back(ui::RichRow::text(running ? "Waiting for the first hop to answer."
                                                     : "The path to the target appears here, one hop at a time, when you press Start.",
                                             ui::Tone::Normal, false, true));
        if (rich_->count() == 0) rich_->reset_rows(rows, 0);
        else rich_->set_rows(rows);
        if (shown > 0 && rich_->selected() < 0) rich_->select(0);
        if (backend_.demo) { /* the scenes: 7 hops are in view at once, whatever the screen */
            if (shown >= static_cast<std::size_t>(kChainMinRows) && hop_open_ < 0 && rich_->scroll_y() == 0 &&
                rich_->rows_in_view() < kChainMinRows) {
                std::fprintf(stderr, "[net-toolbox] the chain shows only %d rows\n", rich_->rows_in_view());
                layout_cut_ = true;
            }
        }
    }
    ui::Band band = ui::trace_band(snap ? *snap : TraceSnapshot(), trace_target_text(), true);
    if (snap && snap->state == TraceSnapshot::State::Failed &&
        ui::text_width(band.text.c_str(), ui::caption_font()) > lv_obj_get_width(band_))
        band.text = "The trace could not start"; /* the whole reason is in the page */
    if (ui::text_width(band.text.c_str(), ui::caption_font()) > lv_obj_get_width(band_) && snap)
        band = ui::trace_band(*snap, trace_target_text(), false);
    show_band(band);
}

void ToolboxApp::start_trace()
{
    const std::string host = trace_target_.empty() ? gateway() : trace_target_;
    if (host.empty()) {
        note({"No gateway: connect to a network first", ui::Tone::Warn});
        return;
    }
    TraceConfig cfg;
    cfg.target = host;
    cfg.reverse_dns = true;
    cfg.resolve.server = pick_resolver("", net_.dns.servers);
    hop_views_.clear();
    hop_open_ = -1;
    backend_.trace->start(cfg);
    fresh_ = true;
    shown_seq_ = 0;
}

void ToolboxApp::open_trace_target()
{
    Choice c;
    c.title = "Target";
    c.labels.push_back(gateway().empty() ? "Gateway (none)" : "Gateway " + gateway());
    c.labels.push_back("1.1.1.1 Cloudflare");
    c.labels.push_back("8.8.8.8 Google");
    const bool has_typed = !trace_typed_.empty() && trace_typed_ != "1.1.1.1" && trace_typed_ != "8.8.8.8";
    if (has_typed) c.labels.push_back(trace_typed_ + " (last used)");
    c.labels.push_back("Type a host...");
    c.current = static_cast<int>(c.labels.size()) - 1;
    if (trace_target_.empty()) c.current = 0;
    else if (trace_target_ == "1.1.1.1") c.current = 1;
    else if (trace_target_ == "8.8.8.8") c.current = 2;
    else if (has_typed && trace_target_ == trace_typed_) c.current = 3;
    c.chosen = [this, has_typed](int i) {
        if (i == 0) trace_target_.clear();
        else if (i == 1) trace_target_ = "1.1.1.1";
        else if (i == 2) trace_target_ = "8.8.8.8";
        else if (has_typed && i == 3) trace_target_ = trace_typed_;
        else {
            Typed t;
            t.title = "Host";
            t.label = "Host";
            t.text = trace_typed_;
            t.placeholder = "name or address";
            t.check = [](const std::string &text) {
                std::string why;
                return valid_host(text, &why) ? std::string() : why;
            };
            t.done = [this](const std::string &text) {
                trace_target_ = trace_typed_ = text;
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

void ToolboxApp::activate_trace(std::intptr_t id)
{
    if (id == kRun) {
        const std::shared_ptr<const TraceSnapshot> s = fresh_ ? backend_.trace->latest() : nullptr;
        if (s && s->running()) backend_.trace->stop();
        else start_trace();
    } else if (id == kTarget) {
        open_trace_target();
        return;
    } else if (id == kSaveLog) {
        save_result();
    } else if (id >= kHop) { /* a hop: its details open in place, or close again */
        const int index = static_cast<int>(id - kHop);
        hop_open_ = hop_open_ == index ? -1 : index;
        poll_trace(true);
        if (rich_) rich_->select(index);
        return;
    }
    poll_trace(true);
}

/* ---------------------------------------------------------------- HTTP check */

void ToolboxApp::build_http(int select)
{
    net_ = backend_.read_state();
    band_check_ = true;
    const Geo g = geo();
    const int y = build_band();
    build_rich(y, g.bottom - y);
    const cp0_ui_metrics_t *m = cp0_ui_metrics_get();
    http_badge_ = std::make_shared<ui::BadgeBlock>(rich_->layout().inset, ui::caption_font(),
                                                   ui::font_bold(m->text_px[CP0_TEXT_TITLE]));
    rich_->reset_rows(http_list(), 0);
    if (select > 0) rich_->select(rich_->index_of_id(select));
    focus(rich_->focus_object());
    poll_http(true);
}

std::vector<ui::RichRow> ToolboxApp::http_list()
{
    const std::shared_ptr<const HttpCheckSnapshot> s = fresh_ ? backend_.http->latest() : nullptr;
    const bool running = s && s->running();
    std::vector<ui::RichRow> rows;
    rows.push_back(action_row(running ? "Stop" : "Run", running ? "checking..." : "", kRun));
    if (s && s->state != HttpCheckSnapshot::State::Idle) {
        http_badge_->set(ui::http_badge(*s));
        rows.push_back(ui::RichRow::custom(http_badge_, http_badge_->height(), false));
        if (s->state == HttpCheckSnapshot::State::Done) {
            const HttpCheckResult &r = s->result;
            const std::uint64_t seq = backend_.http->seq();
            if (http_blocks_seq_ != seq) { /* the blocks of a result are made once */
                http_blocks_seq_ = seq;
                http_water_.reset();
                http_cert_.reset();
                const int inset = rich_->layout().inset;
                if (r.timings.valid) {
                    auto w = std::make_shared<ui::WaterfallBlock>(r.timings, rich_->layout().w, inset, ui::caption_font(),
                                                                  ui::font_bold(ui::caption_px()));
                    if (!w->segments().empty()) http_water_ = w;
                }
                if (r.cert.present || r.answered)
                    http_cert_ = std::make_shared<ui::CertBlock>(ui::cert_badge(r), inset, ui::caption_font(),
                                                                 ui::font_bold(cp0_ui_metrics_get()->text_px[CP0_TEXT_BODY]));
            }
            for (const ui::RowDef &d : ui::http_final_rows(r)) rows.push_back(rich_row(d));
            if (http_water_) rows.push_back(ui::RichRow::custom(http_water_, http_water_->height(), false));
            if (http_cert_) rows.push_back(ui::RichRow::custom(http_cert_, http_cert_->height(), false));
            for (const ui::RowDef &d : ui::http_cert_rows(r)) rows.push_back(rich_row(d));
            for (const ui::RowDef &d : ui::http_info_rows(r)) rows.push_back(rich_row(d));
            if (backend_.demo && !http_badge_->detail_fits()) {
                std::fprintf(stderr, "[net-toolbox] the HTTP badge's text does not fit its block\n");
                layout_cut_ = true;
            }
        }
    }
    rows.push_back(action_row("URL", http_url_, kUrl));
    rows.push_back(action_row("Method", http_head_ ? "HEAD" : "GET", kMethod));
    ui::RichRow insecure = ui::RichRow::item("Ignore certificate errors", http_insecure_ ? "On" : "Off",
                                             http_insecure_ ? ui::Tone::Warn : ui::Tone::Normal, kInsecure);
    rows.push_back(insecure);
    if (http_insecure_)
        rows.push_back(ui::RichRow::text("Warning: the connection is not checked for who it talks to. Use it to look at a site "
                                         "whose certificate is wrong, not for anything private.",
                                         ui::Tone::Warn, false, true));
    rows.push_back(action_row("Save log", "", kSaveLog));
    return rows;
}

void ToolboxApp::poll_http(bool force)
{
    const std::shared_ptr<const HttpCheckSnapshot> snap = fresh_ ? backend_.http->latest() : nullptr;
    const std::uint64_t seq = backend_.http->seq();
    if (force || seq != shown_seq_) {
        shown_seq_ = seq;
        rich_->set_rows(http_list());
    }
    show_band(snap ? ui::http_band(*snap) : ui::Band{"Press Run to check an address", ui::Tone::Normal});
}

void ToolboxApp::start_http()
{
    HttpCheckRequest req;
    req.url = http_url_;
    req.head = http_head_;
    req.insecure = http_insecure_;
    http_blocks_seq_ = 0;
    backend_.http->start(req);
    fresh_ = true;
    shown_seq_ = 0;
}

void ToolboxApp::activate_http(std::intptr_t id)
{
    if (id == kRun) {
        const std::shared_ptr<const HttpCheckSnapshot> s = fresh_ ? backend_.http->latest() : nullptr;
        if (s && s->running()) backend_.http->stop();
        else start_http();
    } else if (id == kUrl) {
        Typed t;
        t.title = "URL";
        t.label = "URL";
        t.text = http_url_;
        t.placeholder = "https://example.com";
        t.max_bytes = 512;
        t.check = [](const std::string &text) {
            HttpTarget target;
            std::string why;
            return parse_http_target(text, target, &why) ? std::string() : why;
        };
        t.done = [this](const std::string &text) {
            http_url_ = text;
            fresh_ = false;
            save_settings_now();
        };
        open_typed(std::move(t));
        return;
    } else if (id == kMethod) {
        Choice c;
        c.title = "Method";
        c.labels = {"GET", "HEAD"};
        c.current = http_head_ ? 1 : 0;
        c.chosen = [this](int i) {
            http_head_ = i == 1;
            fresh_ = false;
        };
        open_choice(std::move(c));
        return;
    } else if (id == kInsecure) {
        http_insecure_ = !http_insecure_;
        fresh_ = false;
        save_settings_now();
    } else if (id == kSaveLog) {
        save_result();
    }
    poll_http(true);
}

/* ---------------------------------------------------------------- Public IP */

void ToolboxApp::build_publicip()
{
    net_ = backend_.read_state();
    band_check_ = true;
    const Geo g = geo();
    const int y = build_band();
    build_rich(y, g.bottom - y);
    backend_.pubip->start(ui::NoRequest()); /* by itself on entry */
    fresh_ = true;
    rich_->reset_rows(publicip_list(), 0);
    focus(rich_->focus_object());
    poll_publicip(true);
}

std::vector<ui::RichRow> ToolboxApp::publicip_list() const
{
    const std::shared_ptr<const PublicIpSnapshot> s = fresh_ ? backend_.pubip->latest() : nullptr;
    const bool running = s && s->running();
    std::vector<ui::RichRow> rows;
    if (s)
        for (const ui::RowDef &d : ui::publicip_rows(*s, local_address())) rows.push_back(rich_row(d));
    const std::string note = s ? ui::nat_note(local_address(), s->result) : std::string();
    if (!note.empty()) rows.push_back(ui::RichRow::text(note, ui::Tone::Warn, false));
    rows.push_back(action_row("Run again", running ? "asking Cloudflare..." : "", kRunAgain));
    rows.push_back(action_row("Whois of this address", s ? ui::publicip_address(*s) : std::string(), kWhoisOf));
    rows.push_back(action_row("Save log", "", kSaveLog));
    return rows;
}

void ToolboxApp::poll_publicip(bool force)
{
    const std::shared_ptr<const PublicIpSnapshot> snap = fresh_ ? backend_.pubip->latest() : nullptr;
    const std::uint64_t seq = backend_.pubip->seq();
    if (force || seq != shown_seq_) {
        shown_seq_ = seq;
        rich_->set_rows(publicip_list());
    }
    show_band(snap ? ui::publicip_band(*snap) : ui::Band());
}

void ToolboxApp::activate_publicip(std::intptr_t id)
{
    if (id == kRunAgain) {
        backend_.pubip->start(ui::NoRequest());
        fresh_ = true;
        shown_seq_ = 0;
    } else if (id == kWhoisOf) {
        const std::shared_ptr<const PublicIpSnapshot> s = fresh_ ? backend_.pubip->latest() : nullptr;
        const std::string address = s ? ui::publicip_address(*s) : std::string();
        if (address.empty()) {
            note({"No public address to look up yet", ui::Tone::Warn});
        } else {
            whois_query_ = address;
            whois_autorun_ = true;
            save_settings_now();
            open(View::Whois);
            return;
        }
    } else if (id == kSaveLog) {
        save_result();
    }
    poll_publicip(true);
}

/* ---------------------------------------------------------------- Whois */

void ToolboxApp::build_whois(int select)
{
    net_ = backend_.read_state();
    band_check_ = true;
    const Geo g = geo();
    const int y = build_band();
    build_rich(y, g.bottom - y);
    rich_->reset_rows(whois_list(), 0);
    if (select > 0) rich_->select(rich_->index_of_id(select));
    focus(rich_->focus_object());
    if (whois_autorun_) {
        whois_autorun_ = false;
        start_whois();
        rich_->set_rows(whois_list());
    }
    poll_whois(true);
}

std::vector<ui::RichRow> ToolboxApp::whois_list() const
{
    const std::shared_ptr<const WhoisSnapshot> s = fresh_ ? backend_.whois->latest() : nullptr;
    const bool running = s && s->running();
    std::vector<ui::RichRow> rows;
    rows.push_back(action_row("Query", whois_query_.empty() ? "none" : whois_query_, kQuery));
    rows.push_back(action_row(running ? "Stop" : "Run", running ? "asking " + (s->asking.empty() ? std::string(kWhoisIana) : s->asking) + "..." : "", kRun));
    rows.push_back(action_row("Save log", "", kSaveLog));
    if (s && s->state == WhoisSnapshot::State::Done) {
        const std::time_t now = backend_.wall_clock ? backend_.wall_clock() : std::time(nullptr);
        const std::vector<ui::RowDef> summary = ui::whois_rows(s->result, now);
        if (!summary.empty()) {
            rows.push_back(ui::RichRow::section("Summary"));
            for (const ui::RowDef &d : summary) rows.push_back(rich_row(d));
        }
        bool any_ok = false;
        for (const WhoisAnswer &a : s->result.answers) any_ok = any_ok || a.ok;
        if (!any_ok && !s->result.answers.empty() && !s->result.answers.back().error.empty())
            rows.push_back(ui::RichRow::text(s->result.answers.back().error, ui::Tone::Bad, false));
        std::size_t more = 0;
        const std::vector<std::string> raw = ui::whois_raw(s->result, 200, &more);
        if (!raw.empty()) {
            rows.push_back(ui::RichRow::section("Raw answer"));
            for (const std::string &line : raw) rows.push_back(ui::RichRow::text(line, ui::Tone::Normal, true, true, true));
            if (more > 0)
                rows.push_back(ui::RichRow::text(std::to_string(more) + " more lines are in the saved log", ui::Tone::Normal, true, true, true));
        }
    }
    return rows;
}

void ToolboxApp::poll_whois(bool force)
{
    const std::shared_ptr<const WhoisSnapshot> snap = fresh_ ? backend_.whois->latest() : nullptr;
    const std::uint64_t seq = backend_.whois->seq();
    if (force || seq != shown_seq_) {
        shown_seq_ = seq;
        rich_->set_rows(whois_list());
    }
    show_band(snap ? ui::whois_band(*snap) : ui::Band{"Type a domain or an address and press Run", ui::Tone::Normal});
}

void ToolboxApp::start_whois()
{
    if (trim(whois_query_).empty()) {
        note({"Type a domain or an address first", ui::Tone::Warn});
        return;
    }
    WhoisRequest req;
    req.query = whois_query_;
    req.resolve.server = pick_resolver("", net_.dns.servers);
    backend_.whois->start(req);
    fresh_ = true;
    shown_seq_ = 0;
}

void ToolboxApp::activate_whois(std::intptr_t id)
{
    if (id == kQuery) {
        Typed t;
        t.title = "Query";
        t.label = "Query";
        t.text = whois_query_;
        t.placeholder = "example.com or 8.8.8.8";
        t.check = [](const std::string &text) {
            std::string object, why;
            bool is_ip = false;
            return parse_whois_query(text, object, is_ip, &why) ? std::string() : why;
        };
        t.done = [this](const std::string &text) {
            whois_query_ = text;
            fresh_ = false;
            save_settings_now();
        };
        open_typed(std::move(t));
        return;
    } else if (id == kRun) {
        const std::shared_ptr<const WhoisSnapshot> s = fresh_ ? backend_.whois->latest() : nullptr;
        if (s && s->running()) backend_.whois->stop();
        else start_whois();
    } else if (id == kSaveLog) {
        save_result();
    }
    poll_whois(true);
}

} // namespace nt
