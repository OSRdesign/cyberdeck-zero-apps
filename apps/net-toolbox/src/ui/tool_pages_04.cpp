/*
 * SPDX-License-Identifier: MIT
 *
 * Net Toolbox 0.4: the pages of Local devices (the list and a device's page), Wi-Fi networks (the list, the Channels view and a
 * network's page) and the Wi-Fi walk test (see toolbox_app.hpp). Like the 0.3 pages each builds from the layout service's
 * tokens, takes the snapshots of its runner on the 100 ms timer and hands them to view_model_04.hpp for the words; the blocks
 * are widgets_04.hpp's. The scans run on the core sessions' own worker threads: nothing here waits on them. The devices scan
 * starts when you press Scan, the port scan when you press one of its rows, the walk when you press Start; the Wi-Fi list is
 * read by NetworkManager's own scan and starts when the page opens (as the public address and the diagnosis do). Leaving the
 * tool (Esc, Left, the header's Esc, the app) stops them (stop_tools); going from the list to a device's or a network's page
 * and back keeps the list's scan going.
 */
#include "toolbox_app.hpp"

#include "app.hpp"
#include "style.hpp"
#include "toolbox.hpp"

#include "cp0_ui_metrics_lvgl.h"

#include "dns_client.hpp"
#include "netaddr.hpp"
#include "netlog_wifi.hpp"
#include "oui_vendor.hpp"
#include "sock.hpp"
#include "text.hpp"

#include <algorithm>
#include <cstdio>

namespace nt {

namespace {

/* a row that does something (Action / ActionRunning), changes a value (Edit) or opens a page (Open) */
ui::RichRow action_row(const std::string &label, const std::string &value, std::intptr_t id, ui::Aff aff)
{
    return ui::RichRow::item(label, value, ui::Tone::Normal, static_cast<int>(id), aff);
}

constexpr int kMaxNetworkRows = 256;

} // namespace

/* ---------------------------------------------------------------- shared */

/* The sockets of the scans go out of the interface the subnet belongs to (scope.effective()) while the Local devices tool is
 * on screen; every other page binds as the chosen network says (update_scope). */
void ToolboxApp::apply_bind()
{
    if (backend_.source) return; /* a fixture board's interface names do not exist on this machine */
    set_active_interface(scan_bound_ ? scope_.effective() : scope_.iface);
}

/* the name of a Wi-Fi interface of the board, to say "choose wlan0 under Network" (the chosen one when it is one) */
std::string ToolboxApp::wifi_hint() const
{
    for (const Iface &i : net_.ifaces)
        if (i.wireless && !i.loopback) return i.name;
    return std::string();
}

/* ================================================================ Local devices */

void ToolboxApp::build_devices(int select)
{
    const Geo g = geo();
    read_net();
    band_check_ = true;
    int y = build_band();
    const cp0_ui_metrics_t *m = cp0_ui_metrics_get();
    ui::ActionBarEvents bar_events;
    bar_events.activate = [this](int id) { post([this, id] { activate_devices(id); }); };
    bar_events.back = [this] { post([this] { back(); }); };
    bar_events.down = [this] {
        post([this] {
            if (rich_ && rich_->selected() >= 0) focus(rich_->focus_object());
        });
    };
    const std::vector<ui::ActionCell> cells = {{kRun, "Scan", ""}, {kSaveLog, "Save log", ""}};
    bar_ = std::make_unique<ui::ActionBar>(page_->root(), g.x, y, g.w, g.row, ui::font_medium(m->text_px[CP0_TEXT_BODY]),
                                           ui::font_medium(std::max(10, ui::caption_px() - 4)), cells, std::move(bar_events));
    y += g.row + g.s;
    build_rich(y, g.bottom - y);
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
    std::vector<lv_obj_t *> order = bar_->focus_objects();
    order.push_back(rich_->focus_object());
    focus_group(order);
    device_views_.clear();
    poll_devices(true);
    if (select >= kDevice && select < kDevice + 1000) rich_->select(rich_->index_of_id(select));
}

std::vector<ui::RichRow> ToolboxApp::devices_list()
{
    const std::shared_ptr<const DevicesSnapshot> s = dev_fresh_ ? backend_.devices->latest() : nullptr;
    std::vector<ui::RichRow> rows;
    device_ips_.clear();
    if (s && !s->devices.empty()) {
        const int inset = rich_->layout().inset, w = rich_->layout().w - rich_->trail_w(ui::Aff::Open);
        const std::size_t n = s->devices.size();
        if (device_views_.size() > n) device_views_.resize(n);
        while (device_views_.size() < n)
            device_views_.push_back(std::make_shared<ui::DeviceRowView>(w, inset, ui::font_medium(rich_->layout().text_px),
                                                                       ui::caption_font(), rich_->layout().row_h));
        for (std::size_t i = 0; i < n; ++i) {
            const Device &d = s->devices[i];
            device_ips_.push_back(d.ip);
            device_views_[i]->set(ui::device_row_texts(d));
            rows.push_back(ui::RichRow::custom(device_views_[i], device_views_[i]->height(), true, kDevice + static_cast<int>(i), ui::Aff::Open));
        }
        return rows;
    }
    const ui::DevicesEmpty e = ui::devices_empty(s ? *s : DevicesSnapshot());
    rows.push_back(ui::RichRow::text(e.text, e.tone, true));
    if (!e.fix.empty()) rows.push_back(ui::RichRow::text(e.fix, ui::Tone::Normal, true, true));
    return rows;
}

void ToolboxApp::poll_devices(bool force)
{
    const std::shared_ptr<const DevicesSnapshot> snap = dev_fresh_ ? backend_.devices->latest() : nullptr;
    const std::uint64_t seq = backend_.devices->seq();
    const DevicesSnapshot idle;
    if (force || seq != shown_seq_) {
        shown_seq_ = seq;
        const bool running = snap && snap->running();
        bar_->set_cell(0, running ? "Stop" : (snap && !snap->devices.empty() ? "Rescan" : "Scan"), "");
        /* the highlight follows the device, not the row: the list is sorted by address and grows as devices are found */
        const int sid = rich_->selected_id();
        if (sid >= kDevice && sid - kDevice < static_cast<int>(device_ips_.size()))
            dev_selected_ip_ = device_ips_[static_cast<std::size_t>(sid - kDevice)];
        const std::vector<ui::RichRow> rows = devices_list();
        if (rich_->count() == 0) rich_->reset_rows(rows, 0);
        else rich_->set_rows(rows);
        if (!dev_selected_ip_.empty()) {
            for (std::size_t i = 0; i < device_ips_.size(); ++i)
                if (device_ips_[i] == dev_selected_ip_ && rich_->selected_id() != kDevice + static_cast<int>(i))
                    rich_->select(rich_->index_of_id(kDevice + static_cast<int>(i)));
        }
    }
    if (force) log_rows("devices");
    show_band(ui::devices_band(snap ? *snap : idle));
}

void ToolboxApp::start_devices()
{
    read_net();
    ui::DevicesRequest req;
    req.net = net_;
    req.iface = scope_.effective();
    req.resolver = pick_resolver("", dns_servers());
    const std::shared_ptr<const DevicesSnapshot> prev = dev_fresh_ ? backend_.devices->latest() : nullptr;
    req.rescan = prev && !prev->devices.empty();
    scan_bound_ = true;
    apply_bind();
    backend_.devices->start(req);
    dev_fresh_ = true;
    shown_seq_ = 0;
}

void ToolboxApp::activate_devices(std::intptr_t id)
{
    if (id == kRun) {
        const std::shared_ptr<const DevicesSnapshot> s = dev_fresh_ ? backend_.devices->latest() : nullptr;
        if (s && s->running()) backend_.devices->stop();
        else start_devices();
    } else if (id == kSaveLog) {
        save_result();
    } else if (id >= kDevice && id - kDevice < static_cast<int>(device_ips_.size())) {
        device_current_ = device_ips_[static_cast<std::size_t>(id - kDevice)];
        open(View::Device);
        return;
    }
    poll_devices(true);
}

/* ---------------------------------------------------------------- a device */

bool ToolboxApp::find_device(const std::string &ip, Device &out) const
{
    const std::shared_ptr<const DevicesSnapshot> s = backend_.devices->latest();
    if (!s) return false;
    for (const Device &d : s->devices)
        if (d.ip == ip) {
            out = d;
            return true;
        }
    return false;
}

void ToolboxApp::build_device(int select)
{
    if (!find_device(device_current_, device_)) {
        post([this] { back(); });
        return;
    }
    read_net();
    band_check_ = true;
    const Geo g = geo();
    const int y = build_band();
    build_rich(y, g.bottom - y);
    port_fresh_ = false; /* the results of an earlier visit are gone */
    shown_dev_seq_ = 0;
    rich_->reset_rows(device_list(), 0);
    const int at = rich_->index_of_id(select > 0 ? select : kActPing);
    rich_->select(at);
    const int fit = std::max(2, rich_->view_height() / std::max(1, rich_->layout().row_h));
    rich_->scroll_row_to_top(std::max(0, at - std::min(fit - 2, 2))); /* whole rows: the last facts, then the actions */
    focus(rich_->focus_object());
    poll_device(true);
}

std::vector<ui::RichRow> ToolboxApp::device_list()
{
    const std::shared_ptr<const PortScanSnapshot> p = port_fresh_ ? backend_.ports->latest() : nullptr;
    const bool scanning = p && p->running();
    std::vector<ui::RichRow> rows;
    for (const ui::RowDef &d : ui::device_info_rows(device_)) rows.push_back(rich_row(d));
    rows.push_back(action_row(ui::kActPing, "", kActPing, ui::Aff::Open));
    rows.push_back(action_row(ui::kActPort, "", kActPort, ui::Aff::Open));
    if (scanning) {
        rows.push_back(action_row(ui::kActStopScan, p->full_range ? "ports 1-1024" : "common ports", kActStopScan, ui::Aff::ActionRunning));
    } else {
        rows.push_back(action_row(ui::kActScanCommon, "", kActScanCommon, ui::Aff::Action));
        rows.push_back(action_row(ui::kActScanAll, "", kActScanAll, ui::Aff::Action));
    }
    if (!device_.mac.empty()) rows.push_back(action_row(ui::kActSaveWol, "", kActSaveWol, ui::Aff::Action));
    rows.push_back(action_row("Save log", "", kSaveLog, ui::Aff::Action));
    if (p && p->state != PortScanSnapshot::State::Idle) {
        rows.push_back(ui::RichRow::section("Open ports"));
        for (std::size_t i = 0; i < p->open.size(); ++i) rows.push_back(rich_row(ui::open_port_row(p->open[i]), kOpenPort + static_cast<int>(i)));
        const std::string none = ui::portscan_empty_text(*p);
        if (!none.empty()) rows.push_back(ui::RichRow::text(none, ui::Tone::Normal, true));
        if (p->state == PortScanSnapshot::State::Failed) rows.push_back(ui::RichRow::text(p->error, ui::Tone::Bad, true));
        if (scanning && p->open.empty()) rows.push_back(ui::RichRow::text("No open port yet.", ui::Tone::Normal, true));
    }
    return rows;
}

void ToolboxApp::poll_device(bool force)
{
    const std::shared_ptr<const PortScanSnapshot> snap = port_fresh_ ? backend_.ports->latest() : nullptr;
    const std::uint64_t seq = backend_.ports->seq();
    const std::uint64_t dseq = backend_.devices->seq();
    if (force || seq != shown_seq_ || dseq != shown_dev_seq_) {
        const bool first_results = snap && snap->state != PortScanSnapshot::State::Idle && !port_results_shown_;
        shown_seq_ = seq;
        shown_dev_seq_ = dseq;
        find_device(device_current_, device_); /* a rescan refreshes its values; the last ones stay when it is gone */
        rich_->set_rows(device_list());
        if (first_results) { /* the scan began: its actions at the top, the results under them */
            port_results_shown_ = true;
            rich_->scroll_row_to_top(rich_->index_of_id(snap->running() ? kActStopScan : kActScanCommon));
        }
        if (!snap || snap->state == PortScanSnapshot::State::Idle) port_results_shown_ = false;
    }
    ui::Band band;
    if (snap && snap->state != PortScanSnapshot::State::Idle) band = ui::portscan_band(*snap);
    else band.text = ui::device_band_text(device_);
    if (force) log_rows("device");
    show_band(band, false);
}

void ToolboxApp::start_port_scan(bool full)
{
    const std::shared_ptr<const DevicesSnapshot> d = backend_.devices->latest();
    ui::PortScanRequest req;
    if (d) req.range = d->range;
    if (!parse_ip(device_.ip, req.address)) {
        note({"This device has no address to scan", ui::Tone::Bad});
        return;
    }
    req.full_range = full;
    scan_bound_ = true;
    apply_bind();
    backend_.ports->start(req);
    port_fresh_ = true;
    port_results_shown_ = false;
    shown_seq_ = 0;
}

void ToolboxApp::save_device_for_wol()
{
    if (settings_.wol_machines.size() >= kMaxWolMachines) {
        note({ui::kWolFullText, ui::Tone::Warn});
        return;
    }
    WolMachine m;
    std::string why;
    if (!ui::wol_machine_of(device_, settings_, m, &why)) {
        note({why, ui::Tone::Warn});
        return;
    }
    const std::string existing = ui::wol_machine_with_mac(settings_, m.mac);
    if (!existing.empty()) {
        note({"Already saved as " + existing, ui::Tone::Warn});
        return;
    }
    if (!settings_.save_machine(m, &why)) {
        note({why.empty() ? std::string("This machine cannot be saved") : why, ui::Tone::Bad});
        return;
    }
    save_settings_now();
    note({ui::wol_save_text(m.name), ui::Tone::Ok});
}

void ToolboxApp::activate_device(std::intptr_t id)
{
    if (id == kActPing) {
        ping_target_ = device_.ip;
        save_settings_now();
        open(View::Ping);
        start_ping();
        return;
    }
    if (id == kActPort) {
        port_host_ = device_.ip;
        save_settings_now();
        open(View::Port);
        return;
    }
    if (id == kActScanCommon) start_port_scan(false);
    else if (id == kActScanAll) start_port_scan(true);
    else if (id == kActStopScan) backend_.ports->stop();
    else if (id == kActSaveWol) save_device_for_wol();
    else if (id == kSaveLog) save_result();
    else return;
    poll_device(true);
}

/* ================================================================ Wi-Fi networks */

void ToolboxApp::build_wifi(int select)
{
    const Geo g = geo();
    read_net();
    band_check_ = true;
    int y = build_band();
    const cp0_ui_metrics_t *m = cp0_ui_metrics_get();
    ui::ActionBarEvents bar_events;
    bar_events.activate = [this](int id) { post([this, id] { activate_wifi(id); }); };
    bar_events.back = [this] { post([this] { back(); }); };
    bar_events.down = [this] {
        post([this] {
            if (rich_ && rich_->selected() >= 0) focus(rich_->focus_object());
        });
    };
    const std::vector<ui::ActionCell> cells = {{kView, "View", wifi_channels_ ? "Channels" : "Networks"}, {kRescan, "Rescan", ""}, {kSaveLog, "Save log", ""}};
    bar_ = std::make_unique<ui::ActionBar>(page_->root(), g.x, y, g.w, g.row, ui::font_medium(m->text_px[CP0_TEXT_BODY]),
                                           ui::font_medium(std::max(10, ui::caption_px() - 4)), cells, std::move(bar_events));
    y += g.row + g.s;
    build_rich(y, g.bottom - y);
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
    std::vector<lv_obj_t *> order = bar_->focus_objects();
    order.push_back(rich_->focus_object());
    focus_group(order);
    wifi_views_.clear();
    const int inset = rich_->layout().inset, w = rich_->layout().w;
    channels_block_ = std::make_shared<ui::ChannelsBlock>(w, inset, ui::caption_font(), ui::font_medium(std::max(10, ui::caption_px() - 2)));
    if (!wifi_live_) start_wifi();
    poll_wifi(true);
    if (select >= kNetRow && select < kNetRow + kMaxNetworkRows) rich_->select(rich_->index_of_id(select));
}

void ToolboxApp::start_wifi()
{
    read_net();
    WifiScanConfig cfg;
    cfg.iface = wifi_scan_iface(scope_, net_);
    cfg.interval_ms = settings_.wifi_rescan_ms;
    cfg.vendor = [](const std::string &mac) { return ui::oui_vendor_of(mac); };
    backend_.wifi->start(cfg);
    wifi_live_ = true;
    shown_seq_ = 0;
}

std::vector<ui::RichRow> ToolboxApp::wifi_list()
{
    const std::shared_ptr<const WifiSnapshot> s = wifi_live_ ? backend_.wifi->latest() : nullptr;
    std::vector<ui::RichRow> rows;
    wifi_bssids_.clear();
    if (s && s->state == WifiSnapshot::State::Failed) {
        const ui::WifiProblem p = ui::wifi_problem(s->error, wifi_hint());
        rows.push_back(ui::RichRow::text(p.words, ui::Tone::Warn, true));
        if (!p.fix.empty()) rows.push_back(ui::RichRow::text(p.fix, ui::Tone::Normal, true, true));
        return rows;
    }
    if (!s || s->state == WifiSnapshot::State::Idle || s->state == WifiSnapshot::State::Scanning) {
        rows.push_back(ui::RichRow::text("Looking for Wi-Fi networks. The first scan takes a few seconds.", ui::Tone::Normal, true));
        return rows;
    }
    if (s->networks.empty()) {
        rows.push_back(ui::RichRow::text("No Wi-Fi network found. Move closer to an access point; the list refreshes by itself.",
                                         ui::Tone::Normal, true));
        return rows;
    }
    if (wifi_channels_) {
        const std::vector<ui::ChannelsView> parts = {ui::channels_view(s->channels.g24, WifiBand::G24),
                                                     ui::channels_view(s->channels.g5, WifiBand::G5)};
        channels_block_->set(parts);
        rows.push_back(ui::RichRow::text(ui::channels_sentence(s->channels), ui::Tone::Ok, true, true));
        rows.push_back(ui::RichRow::custom(channels_block_, channels_block_->height(), false));
        rows.push_back(ui::RichRow::text(ui::kChannelBarsNote, ui::Tone::Normal, true, true));
        if (ui::channels_have_149(parts[1])) rows.push_back(ui::RichRow::text(ui::kChannelNote149, ui::Tone::Normal, true, true));
        return rows;
    }
    const int inset = rich_->layout().inset, w = rich_->layout().w - rich_->trail_w(ui::Aff::Open);
    const std::size_t n = std::min<std::size_t>(s->networks.size(), kMaxNetworkRows);
    if (wifi_views_.size() > n) wifi_views_.resize(n);
    while (wifi_views_.size() < n)
        wifi_views_.push_back(std::make_shared<ui::WifiRowView>(w, inset, ui::font_medium(rich_->layout().text_px), ui::caption_font(),
                                                               rich_->layout().row_h));
    for (std::size_t i = 0; i < n; ++i) {
        wifi_bssids_.push_back(s->networks[i].bssid);
        wifi_views_[i]->set(ui::wifi_row_texts(s->networks[i]));
        rows.push_back(ui::RichRow::custom(wifi_views_[i], wifi_views_[i]->height(), true, kNetRow + static_cast<int>(i), ui::Aff::Open));
    }
    return rows;
}

void ToolboxApp::poll_wifi(bool force)
{
    const std::shared_ptr<const WifiSnapshot> snap = wifi_live_ ? backend_.wifi->latest() : nullptr;
    const std::uint64_t seq = backend_.wifi->seq();
    const WifiSnapshot idle;
    if (force || seq != shown_seq_) {
        shown_seq_ = seq;
        /* the highlight follows the network (the list is sorted by signal and reorders), not the row */
        const int sid = rich_->selected_id();
        if (sid >= kNetRow && sid - kNetRow < static_cast<int>(wifi_bssids_.size()))
            wifi_selected_ = wifi_bssids_[static_cast<std::size_t>(sid - kNetRow)];
        const std::vector<ui::RichRow> rows = wifi_list();
        const bool channels_changed = wifi_channels_ != wifi_channels_shown_;
        if (rich_->count() == 0 || channels_changed) rich_->reset_rows(rows, 0);
        else rich_->set_rows(rows);
        wifi_channels_shown_ = wifi_channels_;
        if (!wifi_channels_ && !wifi_selected_.empty()) {
            for (std::size_t i = 0; i < wifi_bssids_.size(); ++i)
                if (wifi_bssids_[i] == wifi_selected_ && rich_->selected_id() != kNetRow + static_cast<int>(i))
                    rich_->select(rich_->index_of_id(kNetRow + static_cast<int>(i)));
        }
        if (backend_.demo && wifi_channels_ && !channels_block_->fits()) {
            std::fprintf(stderr, "[net-toolbox] a channels message does not fit\n");
            layout_cut_ = true;
        }
        bar_->set_cell(0, "View", wifi_channels_ ? "Channels" : "Networks");
    }
    if (force) log_rows("wifi");
    show_band(ui::wifi_band(snap ? *snap : idle, clock_ms(), wifi_hint()), false);
}

void ToolboxApp::activate_wifi(std::intptr_t id)
{
    if (id == kView) {
        wifi_channels_ = !wifi_channels_;
    } else if (id == kRescan) {
        backend_.wifi->rescan();
    } else if (id == kSaveLog) {
        save_result();
    } else if (id >= kNetRow && id - kNetRow < static_cast<int>(wifi_bssids_.size())) {
        wifi_current_ = wifi_bssids_[static_cast<std::size_t>(id - kNetRow)];
        const std::shared_ptr<const WifiSnapshot> s = backend_.wifi->latest();
        if (s) {
            const int idx = WifiFollower::index_in(*s, wifi_current_);
            if (idx >= 0) {
                wifi_follower_.follow(s->networks[static_cast<std::size_t>(idx)]);
                wifi_title_ = s->networks[static_cast<std::size_t>(idx)].name();
            }
        }
        open(View::WifiDetail);
        return;
    }
    poll_wifi(true);
}

/* ---------------------------------------------------------------- a network */

void ToolboxApp::build_wifi_detail(int select)
{
    if (!wifi_follower_.active()) {
        post([this] { back(); });
        return;
    }
    read_net();
    band_check_ = true;
    const Geo g = geo();
    const int y = build_band();
    build_rich(y, g.bottom - y);
    shown_seq_ = 0;
    const std::shared_ptr<const WifiSnapshot> s = backend_.wifi->latest();
    if (s) wifi_follower_.update(*s);
    rich_->reset_rows(wifi_detail_list(), 0);
    (void)select;
    focus(rich_->focus_object());
    poll_wifi_detail(true);
}

std::vector<ui::RichRow> ToolboxApp::wifi_detail_list()
{
    std::vector<ui::RichRow> rows;
    const WifiNetwork &n = wifi_follower_.network();
    for (const ui::RowDef &d : ui::wifi_detail_rows(n, wifi_follower_.gone())) {
        rows.push_back(rich_row(d));
        if (d.label == "Security") rows.push_back(ui::RichRow::text(ui::security_meaning(n.security), security_weak(n.security) ? ui::Tone::Warn : ui::Tone::Normal, true, true));
    }
    const std::shared_ptr<const WifiSnapshot> live = backend_.wifi->latest();
    const bool scanning = live && live->state == WifiSnapshot::State::Scanning;
    rows.push_back(action_row("Scan now", "", kRescan, ui::action_aff(scanning)));
    rows.push_back(action_row("Save log", "", kSaveLog, ui::Aff::Action));
    return rows;
}

void ToolboxApp::poll_wifi_detail(bool force)
{
    const std::shared_ptr<const WifiSnapshot> snap = backend_.wifi->latest();
    const std::uint64_t seq = backend_.wifi->seq();
    if (force || seq != shown_seq_) {
        shown_seq_ = seq;
        if (snap) wifi_follower_.update(*snap);
        rich_->set_rows(wifi_detail_list());
    }
    const WifiSnapshot idle;
    if (force) log_rows("wifi-detail");
    show_band(ui::wifi_detail_band(wifi_follower_.network(), wifi_follower_.gone(), snap ? *snap : idle, clock_ms(), wifi_hint()), false);
}

void ToolboxApp::activate_wifi_detail(std::intptr_t id)
{
    if (id == kRescan) backend_.wifi->rescan();
    else if (id == kSaveLog) save_result();
    else return;
    poll_wifi_detail(true);
}

/* ================================================================ Wi-Fi walk test */

void ToolboxApp::build_walk()
{
    const Geo g = geo();
    read_net();
    band_check_ = true;
    int y = build_band();
    const cp0_ui_metrics_t *m = cp0_ui_metrics_get();
    ui::ActionBarEvents bar_events;
    bar_events.activate = [this](int id) { post([this, id] { activate_walk(id); }); };
    bar_events.back = [this] { post([this] { back(); }); };
    bar_events.down = [this] {
        post([this] {
            if (rich_ && rich_->selected() >= 0) focus(rich_->focus_object());
        });
    };
    const std::vector<ui::ActionCell> cells = {{kRun, "Start", ""}, {kReset, "Reset", ""}, {kSaveLog, "Save log", ""}};
    bar_ = std::make_unique<ui::ActionBar>(page_->root(), g.x, y, g.w, g.row, ui::font_medium(m->text_px[CP0_TEXT_BODY]),
                                           ui::font_medium(std::max(10, ui::caption_px() - 4)), cells, std::move(bar_events));
    y += g.row + g.s;
    int info_h = 0;
    info_ = build_info(y, ui::kWalkNote, 2, &info_h);
    y += info_h + g.s;
    build_rich(y, g.bottom - y);
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
    const int inset = rich_->layout().inset, w = rich_->layout().w;
    walk_full_h_ = std::max(8 * g.lh, rich_->view_height() - rich_->layout().row_h - g.s);
    const int big_px = std::clamp(2 * ui::caption_px(), 24, 56);
    walk_block_ = std::make_shared<ui::WalkLiveBlock>(w, walk_full_h_, inset, ui::caption_font(),
                                                      ui::font_medium(std::max(10, ui::caption_px() - 3)), ui::font_bold(big_px));
    walk_summary_shown_ = false;
    std::vector<lv_obj_t *> order = bar_->focus_objects();
    order.push_back(rich_->focus_object());
    focus_group(order);
    poll_walk(true);
}

std::vector<ui::RichRow> ToolboxApp::walk_list()
{
    const std::shared_ptr<const WalkSnapshot> s = walk_fresh_ ? backend_.walk->latest() : nullptr;
    std::vector<ui::RichRow> rows;
    std::string value = ui::weak_value(settings_.walk_weak_dbm);
    if (s && s->state != WalkSnapshot::State::Idle && s->state != WalkSnapshot::State::Failed && s->weak_dbm != settings_.walk_weak_dbm)
        value += ", next walk";
    if (s && s->state == WalkSnapshot::State::Failed) { /* no reading to draw: the problem and its fix instead of the chart */
        const ui::WifiProblem p = ui::wifi_problem(s->error, wifi_hint());
        rows.push_back(ui::RichRow::text(p.words, ui::Tone::Warn, true));
        if (!p.fix.empty()) rows.push_back(ui::RichRow::text(p.fix, ui::Tone::Normal, true, true));
        rows.push_back(action_row("Weak below", value, kWalkWeak, ui::Aff::Edit));
        return rows;
    }
    rows.push_back(ui::RichRow::custom(walk_block_, walk_block_->height(), false));
    rows.push_back(action_row("Weak below", value, kWalkWeak, ui::Aff::Edit));
    if (s && s->state == WalkSnapshot::State::Stopped && s->summary.samples > 0) {
        rows.push_back(ui::RichRow::section("Summary"));
        for (const ui::RowDef &d : ui::walk_summary_rows(*s)) rows.push_back(rich_row(d));
    }
    return rows;
}

void ToolboxApp::poll_walk(bool force)
{
    const std::shared_ptr<const WalkSnapshot> snap = walk_fresh_ ? backend_.walk->latest() : nullptr;
    const std::uint64_t seq = backend_.walk->seq();
    const WalkSnapshot idle;
    if (force || seq != shown_seq_) {
        shown_seq_ = seq;
        const bool running = snap && snap->running();
        bar_->set_cell(0, running ? "Stop" : "Start", "");
        const bool summary = snap && snap->state == WalkSnapshot::State::Stopped && snap->summary.samples > 0;
        const int compact = std::min(walk_full_h_, std::max(10 * ui::line_height(ui::caption_font()), walk_full_h_ * 60 / 100));
        walk_block_->set_height(summary ? compact : walk_full_h_);
        walk_block_->update(snap ? *snap : idle);
        const std::vector<ui::RichRow> rows = walk_list();
        if (rich_->count() == 0 || summary != walk_summary_shown_) {
            rich_->reset_rows(rows, 0);
            rich_->scroll_row_to_top(0); /* the chart first, the summary under it */
        } else {
            rich_->set_rows(rows);
        }
        walk_summary_shown_ = summary;
        if (backend_.demo) {
            const std::string problem = walk_block_->layout_problem();
            if (!problem.empty()) {
                std::fprintf(stderr, "[net-toolbox] the walk texts do not fit: %s\n", problem.c_str());
                layout_cut_ = true;
            }
        }
    }
    if (force) log_rows("walk");
    show_band(ui::walk_band(snap ? *snap : idle, wifi_hint()), false);
}

void ToolboxApp::start_walk()
{
    read_net();
    WalkConfig cfg;
    cfg.iface = wifi_scan_iface(scope_, net_);
    cfg.weak_dbm = settings_.walk_weak_dbm;
    backend_.walk->start(cfg);
    walk_fresh_ = true;
    shown_seq_ = 0;
}

void ToolboxApp::activate_walk(std::intptr_t id)
{
    if (id == kRun) {
        const std::shared_ptr<const WalkSnapshot> s = walk_fresh_ ? backend_.walk->latest() : nullptr;
        if (s && s->running()) backend_.walk->stop();
        else start_walk();
    } else if (id == kReset) {
        backend_.walk->reset();
        walk_fresh_ = false;
        shown_seq_ = 0;
    } else if (id == kWalkWeak) {
        settings_.walk_weak_dbm = ui::next_weak_dbm(settings_.walk_weak_dbm);
        save_settings_now();
    } else if (id == kSaveLog) {
        save_result();
    }
    poll_walk(true);
}

} // namespace nt
