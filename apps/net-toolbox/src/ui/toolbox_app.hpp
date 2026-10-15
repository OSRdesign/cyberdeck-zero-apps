/*
 * SPDX-License-Identifier: MIT
 *
 * Net Toolbox: the app's screen (LVGL thread only). One cp0::ui::Page (title, the shared status strip, the on-screen
 * Esc), a status band (one line of CAPTION text) over a panel (the ladder, the ping graph, the result badge) and one
 * cp0::ui::CompactList that carries the actions of the page; the choices are cp0::ui::Roller pages and the typed
 * values a cp0::ui::TextField page (the on-screen keyboard shows by the launcher's rule). All laid out by cp0_lvgl's
 * layout service; the texts are made in view_model.hpp from the core's snapshots, which a 100 ms timer takes from the
 * runners (backend.hpp: a worker thread each; nothing here waits on the network).
 *
 *   Home         Diagnose my connection, Ping, DNS lookup, Port check, Interfaces; the band says the active interface and
 *                its address (or "No network"), refreshed every 2 s
 *   Interfaces   a read-only compact list: a heading per interface (State, IPv4, IPv6, MAC, Speed; Wi-Fi: Network, Signal,
 *                Bit rate), then Network (gateway, DNS servers); every 2 s
 *   Diagnosis    starts by itself: the health ladder (six rungs), the message (the first failure in plain words, or
 *                "Everything works"), the actions Run again and Save log
 *   Ping         the graph and the packet strip over the actions Start / Stop, Target, Save log, Save target
 *   DNS lookup   the actions Look up, Compare resolvers, Name, Type, Resolver, Save log, then the result rows; the band
 *                is the status line (response code in words, time, resolver)
 *   Port check   the result badge over Run, Host, Port, Save log
 *   Traceroute   the actions Start / Stop, Target, Save log as a row of large cells, over the chain: one row per hop (node,
 *                number, name, latency bar, ms, loss tag); Enter or a tap on the selected hop opens its details in place
 *   HTTP check   Run, then the result right under it (the status badge, the final URL, the waterfall, the certificate badge
 *                and its rows, Remote IP, HTTP version, Size, Redirects), then URL, Method, Ignore certificate errors, Save log
 *   Public IP    asks by itself on entry: Public IPv4 / IPv6, Country, Cloudflare data centre, Local address, the carrier-grade
 *                NAT note, then Run again, Whois of this address, Save log
 *   Whois        Query, Run, Save log, then the summary rows, then the raw answer as compact rows
 *   Choice       a Roller of Choice rows (the target, the type, the resolver, the port)
 *   Typed        a TextField (a host, a name, a resolver address, a port number)
 *
 * Esc / Left / the header's Esc go one level up (the tool in use is stopped first); at Home they end the app
 * (cdk::request_quit). Leaving the app stops every worker: nothing runs in the background.
 */
#pragma once

#include "backend.hpp"
#include "chain.hpp"
#include "charts.hpp"
#include "rich_list.hpp"
#include "settings.hpp"
#include "view_model.hpp"
#include "wifi_link.hpp"

#include "async_run.hpp"

#include "cp0_ui_list.h"
#include "cp0_ui_roller.h"
#include "cp0_ui_widgets.hpp"

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace nt {

class ToolboxApp {
public:
    explicit ToolboxApp(lv_obj_t *screen);
    ~ToolboxApp();
    ToolboxApp(const ToolboxApp &) = delete;
    ToolboxApp &operator=(const ToolboxApp &) = delete;

private:
    enum class View { Home, Interfaces, Diagnosis, Ping, Dns, Port, Trace, Http, PublicIp, Whois, Choice, Typed };
    struct Frame {
        View view = View::Home;
        int select = 0;
    };
    /* the Choice page: a pick-one list with a check on the current value */
    struct Choice {
        std::string title;
        std::vector<std::string> labels;
        int current = -1;
        std::function<void(int)> chosen; /* after the page is gone */
    };
    /* the Typed page: one text field */
    struct Typed {
        std::string title, label, text, placeholder;
        cp0::ui::TextType type = cp0::ui::TextType::Text;
        std::size_t max_bytes = 253;
        std::function<std::string(const std::string &)> check; /* "" ok, else the reason (shown under the field) */
        std::function<void(const std::string &)> done;         /* after the page is gone */
    };
    /* the row ids of the lists */
    enum RowId {
        kHomeDiag = 1, kHomePing, kHomeDns, kHomePort, kHomeIfaces, kHomeTrace, kHomeHttp, kHomePublicIp, kHomeWhois,
        kRun = 10, kRunAgain, kSaveLog, kTarget, kSaveTarget, kName, kType, kResolver, kCompare, kHost, kPortRow, kUrl,
        kMethod, kInsecure, kWhoisOf, kQuery,
        kResult = 100,
        kHop = 200
    };

    /* the column and the sizes the pages are built from (the layout service's tokens and the CAPTION font) */
    struct Geo {
        int x = 0, w = 0;    /* the column */
        int s = 0;           /* SPACE_S */
        int inset = 0;       /* SPACE_L: where a list's labels start */
        int lh = 0;          /* a line of CAPTION text */
        int top = 0, bottom = 0; /* the body, less the top margin */
        int row = 0, crow = 0;   /* ROW and LIST_ROW tokens: the comfortable and the compact list row */
    };

    static void posted_cb(void *data);
    static void tick_cb(lv_timer_t *timer);
    void post(std::function<void()> fn);
    void focus(lv_obj_t *obj);
    void tick();

    /* the frame: pages, the chrome, saving (toolbox_app.cpp) */
    void back();
    void open(View view);
    void show(View view, int select);
    void teardown();
    void finish_sub(const std::function<void()> &action);
    void show_title();
    Geo geo() const;
    int build_band();
    void show_band(const ui::Band &dflt);
    void note(const ui::Band &b);
    void build_list(cp0_ui_list_density_t density, int y, int h, bool read_only, std::vector<cp0::ui::ListRow> rows,
                    int select);
    std::vector<cp0::ui::ListRow> to_rows(const std::vector<ui::RowDef> &defs) const;
    void activate(int index, std::intptr_t id);
    void build_home(int select);
    void refresh_home(bool force);
    void open_choice(Choice choice);
    void build_choice();
    void open_typed(Typed typed);
    void build_typed();
    void set_typed_status(const std::string &text);
    void save_result();
    void save_settings_now();
    void stop_tools();
    std::string system_resolver() const;
    std::string gateway() const;
    std::string selftest();

    /* the tool pages (tool_pages.cpp) */
    void build_interfaces();
    void refresh_interfaces();
    void request_links();
    void apply_links();
    void build_diagnosis();
    void poll_diagnosis(bool force);
    void activate_diagnosis(std::intptr_t id);
    std::vector<cp0::ui::ListRow> diag_rows() const;
    void build_ping(int select);
    void poll_ping(bool force);
    void activate_ping(std::intptr_t id);
    std::vector<cp0::ui::ListRow> ping_rows() const;
    std::string ping_target_text() const;
    void start_ping();
    void open_ping_target();
    void build_dns(int select);
    void poll_dns(bool force);
    void activate_dns(std::intptr_t id);
    std::vector<cp0::ui::ListRow> dns_list() const;
    std::string dns_resolver_label() const;
    void start_dns(bool compare);
    void open_dns_type();
    void open_dns_resolver();
    void build_port(int select);
    void poll_port(bool force);
    void activate_port(std::intptr_t id);
    std::vector<cp0::ui::ListRow> port_list() const;
    void start_port();
    void open_port_choice();

    /* the 0.2 pages (tool_pages_02.cpp) */
    void build_trace();
    void poll_trace(bool force);
    void activate_trace(std::intptr_t id);
    void start_trace();
    void open_trace_target();
    std::string trace_target_text() const;
    void build_http(int select);
    void poll_http(bool force);
    void activate_http(std::intptr_t id);
    std::vector<ui::RichRow> http_list();
    void start_http();
    void build_publicip();
    void poll_publicip(bool force);
    void activate_publicip(std::intptr_t id);
    std::vector<ui::RichRow> publicip_list() const;
    std::string local_address() const;
    void build_whois(int select);
    void poll_whois(bool force);
    void activate_whois(std::intptr_t id);
    std::vector<ui::RichRow> whois_list() const;
    void start_whois();
    void build_rich(int y, int h);
    void focus_group(const std::vector<lv_obj_t *> &objs);
    ui::RichRow rich_row(const ui::RowDef &def, int id = 0) const;

    cp0_ui_roller_page_t layout_{};
    ui::Backend backend_;
    Settings settings_;
    std::string config_path_;

    std::unique_ptr<cp0::ui::Page> page_;
    std::unique_ptr<cp0::ui::CompactList> list_;
    std::unique_ptr<ui::RichList> rich_;
    std::unique_ptr<ui::ActionBar> bar_;
    std::unique_ptr<cp0::ui::Roller> roller_;
    std::unique_ptr<cp0::ui::TextField> field_;
    std::unique_ptr<ui::HealthLadder> ladder_;
    std::unique_ptr<ui::PingPanel> ping_panel_;
    std::unique_ptr<ui::ResultBadge> badge_;
    std::unique_ptr<ui::NoticeBox> notice_;
    lv_obj_t *band_ = nullptr;
    lv_obj_t *message_head_ = nullptr;
    lv_obj_t *message_body_ = nullptr;
    lv_obj_t *typed_status_ = nullptr;
    lv_timer_t *timer_ = nullptr;
    std::vector<std::function<void()>> posted_;
    bool post_armed_ = false;

    std::vector<Frame> stack_;
    Choice choice_;
    Typed typed_;
    std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);

    /* live state of the pages */
    NetState net_;
    std::map<std::string, IwLink> links_;
    cdk::async_run::JobId iw_job_ = 0;
    std::int64_t next_state_ms_ = 0;
    std::uint64_t shown_seq_ = 0;
    bool fresh_ = false;             /* the runner's snapshot belongs to what the page shows */
    bool ping_ok_ = true;            /* the ping socket opens */
    std::string ping_why_;
    std::string ping_target_;        /* "" = the gateway */
    std::string dns_name_;
    std::string dns_type_ = "A";
    std::string dns_resolver_;       /* "" = the system's */
    std::string port_host_;
    std::string port_preset_;
    int port_ = 443;
    bool msg_cut_ = false;           /* demonstrations: the diagnosis message did not fit its block */
    bool band_check_ = false;        /* demonstrations: a status line of a 0.2 page must fit (never an ellipsis) */
    bool layout_cut_ = false;        /* demonstrations: a 0.2 page's text or chain did not fit */
    ui::ChainStyle chain_style_;
    std::vector<std::shared_ptr<ui::HopRowView>> hop_views_;
    int hop_open_ = -1;              /* the hop shown in detail, -1 none */
    std::string trace_target_;       /* "" = the gateway */
    std::string trace_typed_;        /* the host typed last: the "last used" entry of the target list */
    std::string http_url_;
    bool http_head_ = false;
    bool http_insecure_ = false;
    std::shared_ptr<ui::BadgeBlock> http_badge_;
    std::shared_ptr<ui::WaterfallBlock> http_water_;
    std::shared_ptr<ui::CertBlock> http_cert_;
    std::uint64_t http_blocks_seq_ = 0;
    std::string whois_query_;
    bool whois_autorun_ = false;     /* Whois of this address: the page runs the query at once */
    std::string saved_note_;
    ui::Tone saved_tone_ = ui::Tone::Normal;
    std::int64_t saved_until_ = 0;
};

} // namespace nt
