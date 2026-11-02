/*
 * SPDX-License-Identifier: MIT
 *
 * Home Assistant app core: the client state machine (see engine.hpp).
 */
#include "engine.hpp"

#include "format.hpp"
#include "mdi.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <time.h>

namespace ha {

namespace {

constexpr std::int64_t kTokenMargin = 60 * 1000;        /* refresh before connecting when less is left */
constexpr std::int64_t kTokenEarly = 120 * 1000;        /* refresh while connected this long before the end */
/* both at most a quarter of the token's life (Home Assistant's 30 minutes: 60 s and 120 s; a short test token keeps
 * working without a refresh storm) */
constexpr std::int64_t kAuthTimeout = 10 * 1000;
constexpr std::int64_t kPingAfter = 30 * 1000;          /* quiet this long: ping */
constexpr std::int64_t kPongTimeout = 15 * 1000;        /* no answer: the link is dead */
constexpr std::int64_t kFrozenGap = 20 * 1000;          /* ticks this far apart: the process was stopped (hidden) */
constexpr std::int64_t kPendingFor = 5 * 1000;          /* "..." at most this long */
constexpr std::int64_t kDoneFor = 1500;                 /* "Done" after a scene or script */
constexpr std::int64_t kPairHintAfter = 180 * 1000;     /* nothing back from the phone */
constexpr std::size_t kMaxSubscribed = 1000;
constexpr int kOfflineAfter = 3;                        /* failed attempts before the line says Offline */
constexpr std::int64_t kGraphRefresh = 5 * 60 * 1000;  /* a sensor card's history asked again after this */
constexpr std::int64_t kExtrasEvery = 10 * 1000;        /* how often the ticks look at the graphs' age */
constexpr std::size_t kMaxDashboards = 20;              /* listed dashboards read at most */

/* the zone's offset east of UTC at that time (the forecast's day names) */
long utc_offset(std::int64_t wall)
{
    const std::time_t t = static_cast<std::time_t>(wall);
    struct tm tm {};
    if (!localtime_r(&t, &tm)) return 0;
    return tm.tm_gmtoff;
}

std::string graph_key(const DashCard &c)
{
    return c.entity + "|" + std::to_string(c.hours) + "|" + std::to_string(c.detail);
}

std::string domain_word(const std::string &entity)
{
    const std::size_t dot = entity.find('.');
    return dot == std::string::npos ? entity : entity.substr(0, dot);
}

} // namespace

std::int64_t backoff_ms(int failures)
{
    if (failures <= 1) return 2000;
    if (failures >= 6) return 60000;
    return 2000LL << (failures - 1); /* 4, 8, 16, 32 s */
}

Engine::Engine(Settings settings, Credentials credentials, std::vector<std::string> favourites)
    : settings_(std::move(settings)), creds_(std::move(credentials)), favourites_(std::move(favourites))
{
}

Server Engine::server() const
{
    Server s;
    parse_server(settings_.server, s);
    return s;
}

std::string Engine::base() const
{
    return creds_.valid() ? creds_.server : server().base_url();
}

void Engine::say(const std::string &text, bool error)
{
    message_ = text;
    message_error_ = error;
    ++message_seq_;
    changed();
}

void Engine::take_token(const std::string &access, const json::Value &answer, std::int64_t now)
{
    const json::Value *exp = answer.get("expires_in");
    std::int64_t life = (exp ? exp->as_int(1800) : 1800) * 1000;
    if (life < 10000) life = 10000;
    access_token_ = access;
    token_obtained_ = now;
    token_expires_ = now + life;
    token_margin_ = std::min<std::int64_t>(kTokenMargin, life / 4);
    token_early_ = std::min<std::int64_t>(kTokenEarly, life / 4);
}

void Engine::start(std::int64_t now)
{
    last_tick_ = now;
    if (creds_.valid()) begin_connect(now);
    else begin_pairing(now);
}

/* --------------------------------------------------------------------------------------------- pairing */

void Engine::begin_pairing(std::int64_t now, const std::string &notice, Tone tone)
{
    (void)now;
    reset_session();
    qr_url_.clear();
    pair_client_id_.clear();
    pair_state_.clear();
    paired_now_ = false;
    pair_hint_ = false;
    Server s = server();
    if (!s.valid()) {
        st_ = St::PairCheckWait;
        retry_at_ = 0;
        pair_status_ = notice.empty() ? "Set the server address of Home Assistant" : notice;
        pair_tone_ = notice.empty() ? Tone::Warn : tone;
        changed();
        return;
    }
    st_ = St::PairCheck;
    pair_status_ = notice.empty() ? "Looking for Home Assistant..." : notice;
    pair_tone_ = notice.empty() ? Tone::Normal : tone;
    pair_notice_ = !notice.empty();
    Action a;
    a.kind = Action::Kind::HttpGet;
    a.url = s.base_url() + "/auth/providers";
    a.tag = HttpTag::CheckServer;
    push(a);
    changed();
}

void Engine::on_listen(bool ok, const std::string &lan_ip, int port, const std::string &ha_ip,
                       const std::string &state, const std::string &why, std::int64_t now)
{
    if (st_ != St::PairListen) return;
    if (!ok || lan_ip.empty() || port <= 0 || state.size() < 16) {
        st_ = St::PairCheckWait;
        retry_at_ = now + backoff_ms(++pair_failures_);
        pair_status_ = "The deck cannot open its pairing address: " + (why.empty() ? std::string("no network") : why);
        pair_tone_ = Tone::Error;
        changed();
        return;
    }
    if (!is_private_ip(lan_ip)) {
        st_ = St::PairCheckWait;
        retry_at_ = now + backoff_ms(++pair_failures_);
        pair_status_ = "The deck's address " + lan_ip + " is not on a home network: Home Assistant would refuse it";
        pair_tone_ = Tone::Error;
        changed();
        return;
    }
    const std::string host = lan_ip.find(':') != std::string::npos ? "[" + lan_ip + "]" : lan_ip;
    pair_client_id_ = "http://" + host + ":" + std::to_string(port) + "/";
    pair_state_ = state;
    const Server s = server();
    const std::string qr_base = (settings_.qr_use_ip && !ha_ip.empty()) ? s.base_url_with_host(ha_ip) : s.base_url();
    qr_url_ = qr_base + "/auth/authorize?" +
              form_encode({{"client_id", pair_client_id_},
                           {"redirect_uri", pair_client_id_ + "callback"},
                           {"state", pair_state_}});
    st_ = St::PairWait;
    pair_failures_ = 0;
    pair_since_ = now;
    pair_hint_ = false;
    /* keep a notice that explains why we are here (signed out, refused) until the phone answers */
    if (!pair_notice_) {
        pair_status_ = "Waiting for the phone...";
        pair_tone_ = Tone::Normal;
    }
    changed();
}

void Engine::on_callback(const std::string &path, const std::map<std::string, std::string> &query, std::int64_t now)
{
    if (st_ != St::PairWait) return;
    if (path != "/callback") return; /* the phone opened the deck's address by itself: nothing to do */
    auto err = query.find("error");
    if (err != query.end()) {
        pair_status_ = "Home Assistant refused the sign-in (" + err->second + "). Scan the code again.";
        pair_tone_ = Tone::Error;
        changed();
        return;
    }
    auto st = query.find("state");
    if (st == query.end() || !constant_time_equal(st->second, pair_state_)) return; /* not from this code */
    auto code = query.find("code");
    if (code == query.end() || code->second.empty()) return;
    st_ = St::PairExchange;
    pair_status_ = "Signing in...";
    pair_tone_ = Tone::Normal;
    Action a;
    a.kind = Action::Kind::HttpPost;
    a.url = server().base_url() + "/auth/token";
    a.body = form_encode({{"grant_type", "authorization_code"}, {"code", code->second}, {"client_id", pair_client_id_}});
    a.tag = HttpTag::Exchange;
    push(a);
    (void)now;
    changed();
}

/* --------------------------------------------------------------------------------------------- connecting */

void Engine::reset_session()
{
    pending_req_.clear();
    registry_wait_ = 0;
    registry_ready_ = false;
    sub_id_ = 0;
    sub_set_.clear();
    pending_until_.clear();
    awaiting_pong_ = false;
    /* 0.3.0: the link's subscriptions and requests die with it */
    forecast_sub_.clear();
    forecast_of_.clear();
    graph_at_.clear();
    sub_loaded_ = false;
    dash_wait_ = 0;
    dash_list_done_ = sysdata_done_ = false;
}

void Engine::begin_connect(std::int64_t now)
{
    if (!creds_.valid()) {
        begin_pairing(now);
        return;
    }
    if (!access_token_.empty() && now < token_expires_ - token_margin_) {
        open_link(now);
        return;
    }
    st_ = St::Refresh;
    refresh_in_flight_ = true;
    Action a;
    a.kind = Action::Kind::HttpPost;
    a.url = creds_.server + "/auth/token";
    a.body = form_encode({{"grant_type", "refresh_token"},
                          {"refresh_token", creds_.refresh_token},
                          {"client_id", creds_.client_id}});
    a.tag = HttpTag::Refresh;
    push(a);
    changed();
}

void Engine::open_link(std::int64_t now)
{
    reset_session();
    Server s;
    std::string url;
    /* the credentials' base URL is what we signed in with: connect to the same place */
    std::string b = creds_.server;
    if (parse_server(b, s)) url = s.websocket_url();
    if (url.empty()) {
        fail_connect(now, "bad server address");
        return;
    }
    st_ = St::Open;
    Action a;
    a.kind = Action::Kind::WsOpen;
    a.url = url;
    push(a);
    changed();
}

void Engine::fail_connect(std::int64_t now, const std::string &why)
{
    push(Action{Action::Kind::WsClose, "", "", "", HttpTag::CheckServer});
    reset_session();
    ++failures_;
    st_ = St::Backoff;
    retry_at_ = now + backoff_ms(failures_);
    last_error_ = why;
    changed();
}

void Engine::on_ws_open(bool ok, const std::string &why, std::int64_t now)
{
    if (st_ != St::Open) return;
    if (!ok) {
        fail_connect(now, why.empty() ? "cannot connect" : why);
        return;
    }
    st_ = St::Auth;
    auth_deadline_ = now + kAuthTimeout;
    last_rx_ = now;
}

void Engine::on_ws_closed(const std::string &why, std::int64_t now)
{
    if (st_ == St::Open || st_ == St::Auth || st_ == St::Online) fail_connect(now, why.empty() ? "link closed" : why);
}

void Engine::signed_out(std::int64_t now, const std::string &notice, Tone tone, bool revoke)
{
    push(Action{Action::Kind::WsClose, "", "", "", HttpTag::CheckServer});
    if (revoke && creds_.valid()) {
        /* not a login: /auth/revoke never counts toward a ban, and always answers 200 */
        Action a;
        a.kind = Action::Kind::HttpPost;
        a.url = creds_.server + "/auth/revoke";
        a.body = form_encode({{"token", creds_.refresh_token}});
        a.tag = HttpTag::RevokeQuiet;
        push(a);
    }
    creds_ = Credentials();
    access_token_.clear();
    token_expires_ = token_obtained_ = 0;
    refresh_in_flight_ = false;
    auth_retry_used_ = false;
    failures_ = 0;
    ever_connected_ = false;
    signing_out_ = false;
    user_.clear();
    ha_version_.clear();
    model_.clear_registry();
    model_.clear_states();
    registry_ever_ = false;
    dashes_.clear();
    dash_choices_.clear();
    dash_key_.clear();
    dash_names_.clear();
    dash_ids_.clear();
    forecasts_.clear();
    graphs_.clear();
    user_id_.clear();
    last_update_ = 0;
    push(Action{Action::Kind::DeleteCredentials, "", "", "", HttpTag::CheckServer});
    begin_pairing(now, notice, tone);
}

/* --------------------------------------------------------------------------------------------- HTTP answers */

void Engine::on_http(HttpTag tag, int status, const std::string &body, const std::string &error, std::int64_t now)
{
    json::Value j;
    if (!body.empty()) json::parse(body, j);
    auto field = [&j](const char *k) {
        const json::Value *v = j.get(k);
        return v ? v->as_string() : std::string();
    };
    switch (tag) {
    case HttpTag::CheckServer:
        if (st_ != St::PairCheck) return;
        if (status == 200) {
            st_ = St::PairListen;
            Action a;
            a.kind = Action::Kind::ListenStart;
            a.host = server().host;
            push(a);
        } else {
            st_ = St::PairCheckWait;
            retry_at_ = now + backoff_ms(++pair_failures_);
            qr_url_.clear();
            if (status == 0)
                pair_status_ = "Cannot reach Home Assistant at " + server().display() + ". Check the server address.";
            else
                pair_status_ = server().display() + " answered HTTP " + std::to_string(status) +
                               ": is it Home Assistant?";
            pair_tone_ = Tone::Error;
            pair_notice_ = false;
        }
        changed();
        return;
    case HttpTag::Exchange: {
        if (st_ != St::PairExchange) return;
        const std::string access = field("access_token"), refresh = field("refresh_token");
        if (status == 200 && !access.empty() && !refresh.empty()) {
            creds_.server = server().base_url();
            creds_.client_id = pair_client_id_;
            creds_.refresh_token = refresh;
            push(Action{Action::Kind::SaveCredentials, "", "", "", HttpTag::CheckServer});
            push(Action{Action::Kind::ListenStop, "", "", "", HttpTag::CheckServer});
            take_token(access, j, now);
            auth_retry_used_ = false;
            failures_ = 0;
            pair_status_ = "Connecting...";
            pair_tone_ = Tone::Normal;
            open_link(now);
            return;
        }
        st_ = St::PairWait;
        if (status == 0) {
            pair_status_ = "Cannot reach Home Assistant to finish the sign-in (" + error + "). Scan the code again.";
        } else {
            std::string d = field("error_description");
            if (d.empty()) d = field("error");
            if (d.empty()) d = "HTTP " + std::to_string(status);
            pair_status_ = "Home Assistant refused the sign-in (" + d + "). Scan the code again.";
        }
        pair_tone_ = Tone::Error;
        changed();
        return;
    }
    case HttpTag::Refresh: {
        refresh_in_flight_ = false;
        if (!creds_.valid()) return;
        const std::string access = field("access_token");
        if (status == 200 && !access.empty()) {
            take_token(access, j, now);
            if (st_ == St::Refresh) open_link(now);
            changed();
            return;
        }
        if (status >= 400 && status < 500) {
            /* the refresh token is no longer valid (revoked in Home Assistant, user removed or not allowed from
             * here): never retried, it would count as a failed login */
            signed_out(now, "Home Assistant no longer accepts this deck's sign-in. Scan the code to sign in again.",
                       Tone::Error, false);
            return;
        }
        if (st_ == St::Refresh) {
            fail_connect(now, status == 0 ? (error.empty() ? std::string("cannot reach Home Assistant") : error)
                                          : "Home Assistant answered HTTP " + std::to_string(status));
        } else {
            refresh_retry_at_ = now + 60 * 1000;
        }
        return;
    }
    case HttpTag::Revoke:
        if (!signing_out_) return;
        if (status > 0) {
            signed_out(now, "Signed out. Scan the code to sign in again.", Tone::Normal, false);
        } else {
            signing_out_ = false;
            say("Sign out failed: cannot reach Home Assistant. Try again when connected.", true);
        }
        return;
    case HttpTag::RevokeQuiet: return;
    }
}

/* --------------------------------------------------------------------------------------------- WebSocket messages */

int Engine::send(const std::string &type, const std::string &fields, Req req, const std::string &entity,
                 const std::string &service)
{
    const int id = next_id_++;
    std::string m = "{\"id\":" + std::to_string(id) + ",\"type\":" + json::quote(type);
    if (!fields.empty()) m += "," + fields;
    m += "}";
    Action a;
    a.kind = Action::Kind::WsSend;
    a.body = m;
    push(a);
    Pending p;
    p.req = req;
    p.entity = entity;
    p.service = service;
    pending_req_[id] = p;
    return id;
}

void Engine::on_ws_message(const std::string &text, std::int64_t now, std::int64_t wall)
{
    last_rx_ = now;
    awaiting_pong_ = false;
    wall_ = wall;
    msg_bytes_ = text.size();
    json::Value msg;
    if (!json::parse(text, msg)) return;
    if (msg.is_array()) {
        for (const json::Value &m : msg.items) handle(m, now, wall);
    } else {
        handle(msg, now, wall);
    }
}

void Engine::handle(const json::Value &msg, std::int64_t now, std::int64_t wall)
{
    const json::Value *t = msg.get("type");
    if (!t) return;
    const std::string &type = t->text;
    if (type == "auth_required") {
        if (st_ != St::Auth) return;
        if (const json::Value *v = msg.get("ha_version")) ha_version_ = v->as_string();
        Action a;
        a.kind = Action::Kind::WsSend;
        a.body = "{\"type\":\"auth\",\"access_token\":" + json::quote(access_token_) + "}";
        push(a);
        return;
    }
    if (type == "auth_ok") {
        if (st_ != St::Auth) return;
        st_ = St::Online;
        failures_ = 0;
        auth_retry_used_ = false;
        ever_connected_ = true;
        last_error_.clear();
        if (const json::Value *v = msg.get("ha_version")) ha_version_ = v->as_string();
        send("supported_features", "\"features\":{\"coalesce_messages\":1}", Req::Features);
        send("get_config", "", Req::Config);
        send("auth/current_user", "", Req::User);
        send("config/area_registry/list", "", Req::Areas);
        send("config/device_registry/list", "", Req::Devices);
        send("config/entity_registry/list_for_display", "", Req::Entities);
        registry_wait_ = 3;
        changed();
        return;
    }
    if (type == "auth_invalid") {
        if (st_ != St::Auth) return;
        if (!auth_retry_used_ && now - token_obtained_ > token_margin_) {
            /* probably an access token that ran out while the app was stopped: one new token, one more try */
            auth_retry_used_ = true;
            access_token_.clear();
            push(Action{Action::Kind::WsClose, "", "", "", HttpTag::CheckServer});
            reset_session();
            begin_connect(now);
            return;
        }
        signed_out(now, "Home Assistant refused this deck's sign-in. Scan the code to sign in again.", Tone::Error,
                   true);
        return;
    }
    if (st_ != St::Online) return;
    const json::Value *idv = msg.get("id");
    const int id = idv ? static_cast<int>(idv->as_int(0)) : 0;
    if (type == "pong") {
        pending_req_.erase(id);
        return;
    }
    if (type == "result") {
        auto it = pending_req_.find(id);
        if (it == pending_req_.end()) return;
        const Pending p = it->second;
        pending_req_.erase(it);
        on_result(msg, p, now);
        return;
    }
    if (type == "event") {
        const json::Value *ev = msg.get("event");
        if (!ev || id == 0) return;
        auto f = forecast_of_.find(id);
        if (f != forecast_of_.end()) { /* weather/subscribe_forecast (0.3.0) */
            std::vector<ForecastDay> days;
            if (parse_forecast(*ev, utc_offset(wall), days)) {
                forecasts_[f->second] = std::move(days);
                changed();
            }
            return;
        }
        if (id != sub_id_) return; /* an old subscription */
        if (ev->get("a")) sub_loaded_ = true;
        std::set<std::string> ids;
        if (model_.apply_event(*ev, &ids)) {
            bool favourite = false, dash_name = false;
            for (const std::string &e : ids) {
                pending_until_.erase(e);
                favourite = favourite || std::find(favourites_.begin(), favourites_.end(), e) != favourites_.end();
                if (dash_ids_.count(e))
                    if (const State *s = model_.state(e))
                        if (!s->friendly_name.empty() && dash_names_[e] != s->friendly_name) {
                            dash_names_[e] = s->friendly_name;
                            dash_name = true;
                        }
            }
            if (favourite) update_name_cache();
            if (dash_name) save_dashboard();
            last_update_ = wall;
            changed();
        } else if (ev->get("a")) {
            changed(); /* an empty snapshot: "Not available" can show */
        }
        return;
    }
}

void Engine::on_result(const json::Value &msg, const Pending &p, std::int64_t now)
{
    const json::Value *ok = msg.get("success");
    const bool success = ok && ok->as_bool();
    const json::Value *result = msg.get("result");
    static const json::Value kNull;
    const json::Value &r = result ? *result : kNull;
    switch (p.req) {
    case Req::Config:
        if (success)
            if (const json::Value *us = r.get("unit_system"))
                if (const json::Value *t = us->get("temperature")) {
                    temp_unit_ = clean_unit(t->as_string("C"));
                    temp_unit_raw_ = t->as_string(temp_unit_raw_);
                }
        return;
    case Req::IconsComponent:
    case Req::IconsEntity: {
        /* a refusal (an older server) keeps what we had: the app's own table covers every phase-1 domain */
        if (!success) return;
        bool ok = false;
        if (p.req == Req::IconsComponent) {
            ok = icons_.set_component(r);
        } else {
            std::set<std::string> keep;
            for (const RegEntry &e : model_.resolved_entries())
                if (!e.platform.empty() && !e.translation_key.empty())
                    keep.insert(e.platform + "|" + domain_word(e.id) + "|" + e.translation_key);
            ok = icons_.set_entity(r, keep);
        }
        if (ok) {
            save_layout();
            changed();
        }
        return;
    }
    case Req::User:
        if (success) {
            if (const json::Value *n = r.get("name")) user_ = n->as_string();
            if (const json::Value *i = r.get("id")) user_id_ = i->as_string();
        }
        if (!pair_client_id_.empty() || st_ == St::Online) {
            if (!pair_client_id_.empty()) {
                paired_now_ = true;
                pair_status_ = "Connected as " + (user_.empty() ? std::string("your user") : user_);
                pair_tone_ = Tone::Normal;
                pair_client_id_.clear();
            }
        }
        changed();
        return;
    case Req::Areas:
    case Req::Devices:
    case Req::Entities:
        if (success) {
            if (p.req == Req::Areas) model_.set_areas(r);
            else if (p.req == Req::Devices) model_.set_devices(r);
            else model_.set_entities(r);
        }
        if (--registry_wait_ <= 0) {
            registry_ready_ = true;
            registry_ever_ = true;
            update_subscription();
            update_name_cache();
            send_icon_requests();
            save_layout();
            send_dashboard_requests();
            update_dash_extras(now);
        }
        changed();
        return;
    case Req::Call:
        if (success) {
            if (p.service == "turn_on" && (domain_word(p.entity) == "scene" || domain_word(p.entity) == "script"))
                done_until_[p.entity] = now + kDoneFor;
        } else {
            pending_until_.erase(p.entity);
            std::string why = "error";
            if (const json::Value *e = msg.get("error"))
                if (const json::Value *m = e->get("message")) why = m->as_string(why);
            say("Could not " + p.service + " " + model_.display_name(p.entity) + ": " + why, true);
        }
        changed();
        return;
    case Req::DashList: {
        dash_list_done_ = true;
        if (success && r.is_array())
            for (const json::Value &d : r.items) {
                if (dash_choices_.size() > kMaxDashboards) break;
                const json::Value *u = d.get("url_path");
                const std::string url = u ? u->as_string() : std::string();
                const json::Value *t = d.get("title");
                const std::string title = t ? t->as_string() : std::string();
                if (url.empty()) continue;
                if (url == kDashDefault) { /* the default itself, listed: its title */
                    if (!title.empty()) choice(kDashDefault)->title = title;
                    continue;
                }
                if (choice(url)) continue;
                DashChoice c;
                c.key = url;
                c.title = title;
                dash_choices_.push_back(c);
                send("lovelace/config", "\"url_path\":" + json::quote(url), Req::DashConfig, url);
                ++dash_wait_;
            }
        apply_dashboards(false, now);
        return;
    }
    case Req::SysData:
        sysdata_done_ = true;
        default_panel_.clear();
        if (success)
            if (const json::Value *v = r.get("value"))
                if (const json::Value *d = v->get("default_panel")) default_panel_ = d->as_string();
        apply_dashboards(false, now);
        return;
    case Req::DashConfig:
        on_dash_config(p, success, msg, r);
        apply_dashboards(false, now);
        return;
    case Req::Forecast:
        if (!success) { /* the entity has no daily forecast: the current weather only */
            auto it = forecast_sub_.find(p.entity);
            if (it != forecast_sub_.end()) {
                forecast_of_.erase(it->second);
                forecast_sub_.erase(it);
            }
            forecasts_.erase(p.entity);
            changed();
        }
        return;
    case Req::History:
        if (success) graphs_[p.service] = downsample(parse_history(r, p.entity), static_cast<double>(p.t0),
                                                     static_cast<double>(p.t1), p.points);
        else graphs_[p.service].clear();
        changed();
        return;
    default: return;
    }
}

/* The friendly name of the live state first, then the name cached from an earlier state (the registry's own name can
 * differ: "Kitchen" plus the device's name), then the registry's. */
std::string Engine::name_of(const std::string &id) const
{
    if (const State *s = model_.state(id))
        if (!s->friendly_name.empty()) return s->friendly_name;
    auto it = names_.find(id);
    if (it != names_.end() && !it->second.empty()) return it->second;
    if (!model_.entry(id)) { /* not a phase-1 entity of the registry: the name a dashboard row saw last */
        auto d = dash_names_.find(id);
        if (d != dash_names_.end() && !d->second.empty()) return d->second;
    }
    return model_.display_name(id);
}

/* The favourites' names, as Home Assistant gives them now, kept in the cache file when one changed. Only favourites:
 * the file stays as small as the list. */
void Engine::update_name_cache()
{
    std::map<std::string, std::string> next;
    for (const std::string &id : favourites_) {
        /* a registry name never replaces a cached one: only a state's friendly name does (no flip at each connect) */
        const std::string name = name_of(id);
        if (model_.known(id) || names_.count(id)) next[id] = name;
    }
    if (next == names_) return;
    names_ = std::move(next);
    push(Action{Action::Kind::SaveNames, "", "", "", HttpTag::CheckServer});
}

/* The icon rules of Home Assistant (study 053 section 6.6), both open to any user: the phase-1 domains' rules, then the
 * translation-key icons of the integrations whose entities the registry holds with a key. */
void Engine::send_icon_requests()
{
    std::string domains = "[";
    for (std::size_t i = 0; i < icon_domains().size(); ++i) domains += (i ? "," : "") + json::quote(icon_domains()[i]);
    send("frontend/get_icons", "\"category\":\"entity_component\",\"integration\":" + domains + "]", Req::IconsComponent);
    std::set<std::string> platforms;
    for (const RegEntry &e : model_.resolved_entries())
        if (!e.platform.empty() && !e.translation_key.empty() && platforms.size() < 200) platforms.insert(e.platform);
    if (platforms.empty()) return;
    std::string list = "[";
    bool first = true;
    for (const std::string &p : platforms) {
        list += (first ? "" : ",") + json::quote(p);
        first = false;
    }
    send("frontend/get_icons", "\"category\":\"entity\",\"integration\":" + list + "]", Req::IconsEntity);
}

void Engine::set_cached_layout(std::vector<Area> areas, std::vector<RegEntry> entries, IconRules icons)
{
    if (entries.empty() && areas.empty()) return;
    model_.restore(std::move(areas), std::move(entries));
    icons_ = std::move(icons);
    registry_ever_ = model_.registry_size() > 0;
    layout_saved_ = layout_text();
}

std::string Engine::layout_text() const
{
    if (model_.registry_size() == 0 && model_.area_count() == 0) return std::string();
    std::string s = "{\n  \"version\": 1,\n  \"areas\": [";
    bool first = true;
    for (const Area &a : model_.areas()) {
        s += std::string(first ? "\n    " : ",\n    ") + "{\"id\": " + json::quote(a.id) + ", \"name\": " + json::quote(a.name) + "}";
        first = false;
    }
    s += "\n  ],\n  \"entities\": [";
    first = true;
    for (const RegEntry &e : model_.resolved_entries()) {
        s += std::string(first ? "\n    " : ",\n    ") + "{\"id\": " + json::quote(e.id) + ", \"name\": " + json::quote(e.name);
        if (!e.area.empty()) s += ", \"area\": " + json::quote(e.area);
        if (e.precision >= 0) s += ", \"dp\": " + std::to_string(e.precision);
        if (!e.icon.empty()) s += ", \"ic\": " + json::quote(e.icon);
        if (!e.platform.empty()) s += ", \"pl\": " + json::quote(e.platform);
        if (!e.translation_key.empty()) s += ", \"tk\": " + json::quote(e.translation_key);
        s += "}";
        first = false;
        if (s.size() > kMaxLayoutBytes) return std::string();
    }
    s += "\n  ],\n  \"icons\": " + icons_.to_json() + "\n}\n";
    return s.size() > kMaxLayoutBytes ? std::string() : s;
}

void Engine::save_layout()
{
    const std::string text = layout_text();
    if (text.empty() || text == layout_saved_) return;
    layout_saved_ = text;
    push(Action{Action::Kind::SaveLayout, "", "", "", HttpTag::CheckServer});
}

std::set<std::string> Engine::wanted_set() const
{
    std::set<std::string> s(favourites_.begin(), favourites_.end());
    if (view_room_)
        for (const std::string &id : model_.entities_in(view_room_id_)) {
            if (s.size() >= kMaxSubscribed) break;
            s.insert(id);
        }
    if (!view_entity_.empty()) s.insert(view_entity_);
    if (const DashView *v = shown_view())
        for (const std::string &id : view_entities(*v)) {
            if (s.size() >= kMaxSubscribed) break;
            s.insert(id);
        }
    return s;
}

void Engine::update_subscription()
{
    if (st_ != St::Online || !registry_ready_) return;
    std::set<std::string> want = wanted_set();
    if (want == sub_set_ && (sub_id_ != 0 || want.empty())) return;
    if (sub_id_) send("unsubscribe_events", "\"subscription\":" + std::to_string(sub_id_), Req::Unsubscribe);
    sub_id_ = 0;
    if (!want.empty()) {
        std::string ids = "\"entity_ids\":[";
        bool first = true;
        for (const std::string &e : want) {
            if (!first) ids += ",";
            ids += json::quote(e);
            first = false;
        }
        ids += "]";
        sub_id_ = send("subscribe_entities", ids, Req::Subscribe);
    }
    sub_loaded_ = false;
    sub_set_ = want;
    model_.keep_only(want);
}

void Engine::call_service(const std::string &entity, const std::string &domain, const std::string &service,
                          const std::string &data, bool pending, std::int64_t now)
{
    if (st_ != St::Online) {
        say("Not connected: try again when the line says Connected", true);
        return;
    }
    std::string f = "\"domain\":" + json::quote(domain) + ",\"service\":" + json::quote(service);
    if (!data.empty()) f += ",\"service_data\":" + data;
    f += ",\"target\":{\"entity_id\":" + json::quote(entity) + "}";
    send("call_service", f, Req::Call, entity, service);
    if (pending) pending_until_[entity] = now + kPendingFor;
    changed();
}

/* One service for several entities of a domain (an entities card's header toggle): one call, a target list. */
void Engine::call_group(const std::string &domain, const std::string &service, const std::vector<std::string> &ids,
                        std::int64_t now)
{
    if (ids.empty()) return;
    if (st_ != St::Online) {
        say("Not connected: try again when the line says Connected", true);
        return;
    }
    std::string list = "[";
    for (std::size_t i = 0; i < ids.size(); ++i) list += (i ? "," : "") + json::quote(ids[i]);
    send("call_service",
         "\"domain\":" + json::quote(domain) + ",\"service\":" + json::quote(service) + ",\"target\":{\"entity_id\":" +
             list + "]}",
         Req::Call, ids.front(), service);
    for (const std::string &id : ids) pending_until_[id] = now + kPendingFor;
    changed();
}

/* --------------------------------------------------------------------------------------------- commands */

void Engine::on_command(const Command &c, std::int64_t now)
{
    switch (c.kind) {
    case Command::Kind::SetView:
        if (view_room_ == c.flag && view_room_id_ == c.a && view_entity_ == c.b && view_dash_ == c.value) return;
        view_room_ = c.flag;
        view_room_id_ = c.flag ? c.a : std::string();
        view_entity_ = c.b;
        view_dash_ = c.value;
        update_subscription();
        update_dash_extras(now);
        changed();
        return;
    case Command::Kind::ToggleFavourite: {
        if (c.a.empty()) return;
        auto it = std::find(favourites_.begin(), favourites_.end(), c.a);
        if (it == favourites_.end()) favourites_.push_back(c.a);
        else favourites_.erase(it);
        push(Action{Action::Kind::SaveFavourites, "", "", "", HttpTag::CheckServer});
        update_subscription();
        update_name_cache();
        changed();
        return;
    }
    case Command::Kind::Toggle: {
        const Domain d = domain_of(c.a);
        if (d == Domain::Light) {
            const State *s = model_.state(c.a);
            const bool on = s && s->state == "on";
            call_service(c.a, "light", on ? "turn_off" : "turn_on", "", true, now);
        } else if (d == Domain::Switch || d == Domain::InputBoolean || domain_word(c.a) == "fan") {
            call_service(c.a, domain_word(c.a), "toggle", "", true, now);
        }
        return;
    }
    case Command::Kind::Run: {
        const Domain d = domain_of(c.a);
        if (d == Domain::Scene || d == Domain::Script) call_service(c.a, domain_word(c.a), "turn_on", "", false, now);
        return;
    }
    case Command::Kind::SetBrightness:
        if (domain_of(c.a) != Domain::Light) return;
        if (c.value <= 0) call_service(c.a, "light", "turn_off", "", true, now);
        else
            call_service(c.a, "light", "turn_on",
                         "{\"brightness_pct\":" + std::to_string(std::min(100, c.value)) + "}", true, now);
        return;
    case Command::Kind::Service: {
        const Domain d = domain_of(c.a);
        const bool lock_ok = d == Domain::Lock && (c.b == "lock" || c.b == "unlock");
        const bool cover_ok = d == Domain::Cover && (c.b == "open_cover" || c.b == "close_cover");
        if (lock_ok || cover_ok) call_service(c.a, domain_word(c.a), c.b, "", true, now);
        return;
    }
    case Command::Kind::SignOut: {
        if (!creds_.valid() || signing_out_) return;
        signing_out_ = true;
        Action a;
        a.kind = Action::Kind::HttpPost;
        a.url = creds_.server + "/auth/revoke";
        a.body = form_encode({{"token", creds_.refresh_token}});
        a.tag = HttpTag::Revoke;
        push(a);
        changed();
        return;
    }
    case Command::Kind::SetServer: {
        Server s;
        std::string why;
        if (!parse_server(c.a, s, &why)) {
            say(why, true);
            return;
        }
        settings_.server = s.display();
        push(Action{Action::Kind::SaveSettings, "", "", "", HttpTag::CheckServer});
        if (!creds_.valid()) {
            push(Action{Action::Kind::ListenStop, "", "", "", HttpTag::CheckServer});
            pair_failures_ = 0;
            begin_pairing(now);
        }
        changed();
        return;
    }
    case Command::Kind::SetDashboard: {
        const std::string want = c.a == kDashOff || choice(c.a) ? c.a : std::string(kDashAuto);
        if (want == settings_.dashboard) return;
        settings_.dashboard = want;
        push(Action{Action::Kind::SaveSettings, "", "", "", HttpTag::CheckServer});
        apply_dashboards(true, now);
        changed();
        return;
    }
    case Command::Kind::SetGroup: {
        /* the header toggle: per domain one turn_on / turn_off with the list (light, switch, fan, input_boolean) */
        std::map<std::string, std::vector<std::string>> by_domain;
        for (const std::string &id : c.ids)
            if (toggle_domain(id)) by_domain[domain_word(id)].push_back(id);
        for (const auto &kv : by_domain) call_group(kv.first, c.flag ? "turn_on" : "turn_off", kv.second, now);
        return;
    }
    }
}

/* --------------------------------------------------------------------------------------------- time */

void Engine::on_tick(std::int64_t now, std::int64_t wall)
{
    wall_ = wall;
    if (st_ == St::Online && view_dash_ >= 0 && now >= extras_at_) {
        extras_at_ = now + kExtrasEvery;
        update_dash_extras(now); /* a sensor card's history again after 5 minutes */
    }
    const bool frozen = last_tick_ && now - last_tick_ > kFrozenGap;
    last_tick_ = now;
    if (frozen && (st_ == St::Online || st_ == St::Auth)) {
        /* the process was stopped (the launcher hid the app): Home Assistant has dropped the link by now (no pongs) */
        fail_connect(now, "the app was paused");
        --failures_;
        retry_at_ = now;
    }
    switch (st_) {
    case St::Backoff:
        if (now >= retry_at_) begin_connect(now);
        break;
    case St::PairCheckWait:
        if (retry_at_ && now >= retry_at_) begin_pairing(now);
        break;
    case St::Auth:
        if (now > auth_deadline_) fail_connect(now, "no answer to the sign-in");
        break;
    case St::Online:
        if (awaiting_pong_ && now - ping_at_ > kPongTimeout) {
            fail_connect(now, "no answer from Home Assistant");
            break;
        }
        if (!awaiting_pong_ && now - last_rx_ > kPingAfter) {
            send("ping", "", Req::Ping);
            awaiting_pong_ = true;
            ping_at_ = now;
        }
        if (!refresh_in_flight_ && token_expires_ && now > token_expires_ - token_early_ && now >= refresh_retry_at_) {
            refresh_in_flight_ = true;
            Action a;
            a.kind = Action::Kind::HttpPost;
            a.url = creds_.server + "/auth/token";
            a.body = form_encode({{"grant_type", "refresh_token"},
                                  {"refresh_token", creds_.refresh_token},
                                  {"client_id", creds_.client_id}});
            a.tag = HttpTag::Refresh;
            push(a);
        }
        break;
    case St::PairWait:
        if (!pair_hint_ && now - pair_since_ > kPairHintAfter) {
            pair_hint_ = true;
            pair_status_ = "Nothing came back from the phone yet. If it could not open the page, use Home "
                           "Assistant's IP address as the server address.";
            pair_tone_ = Tone::Warn;
            changed();
        }
        break;
    default: break;
    }
    for (auto it = pending_until_.begin(); it != pending_until_.end();) {
        if (now >= it->second) {
            it = pending_until_.erase(it);
            changed();
        } else {
            ++it;
        }
    }
    for (auto it = done_until_.begin(); it != done_until_.end();) {
        if (now >= it->second) {
            it = done_until_.erase(it);
            changed();
        } else {
            ++it;
        }
    }
}

std::int64_t Engine::next_deadline(std::int64_t now) const
{
    std::int64_t next = now + 1000;
    auto earlier = [&next](std::int64_t t) {
        if (t > 0 && t < next) next = t;
    };
    if (st_ == St::Backoff || st_ == St::PairCheckWait) earlier(retry_at_);
    if (st_ == St::Auth) earlier(auth_deadline_);
    for (const auto &p : pending_until_) earlier(p.second);
    for (const auto &d : done_until_) earlier(d.second);
    return std::max(now, next);
}

std::vector<Action> Engine::take_actions()
{
    std::vector<Action> out;
    out.swap(actions_);
    return out;
}

/* --------------------------------------------------------------------------------------------- the stored dashboard */

/* Study 053 P2: every listed dashboard and the default one are read (lovelace/dashboards/list, lovelace/config, any
 * user), with the system's default panel (frontend/get_system_data core); the setting picks one once all answered. */
void Engine::send_dashboard_requests()
{
    std::vector<DashChoice> fresh;
    DashChoice def;
    def.key = kDashDefault;
    fresh.push_back(def);
    dash_choices_ = std::move(fresh);
    dash_list_done_ = sysdata_done_ = false;
    send("lovelace/dashboards/list", "", Req::DashList);
    send("frontend/get_system_data", "\"key\":\"core\"", Req::SysData);
    send("lovelace/config", "", Req::DashConfig, kDashDefault);
    dash_wait_ = 1;
}

DashChoice *Engine::choice(const std::string &key)
{
    for (DashChoice &c : dash_choices_)
        if (c.key == key) return &c;
    return nullptr;
}

void Engine::on_dash_config(const Pending &p, bool success, const json::Value &msg, const json::Value &r)
{
    if (dash_wait_ > 0) --dash_wait_;
    DashChoice *c = choice(p.entity);
    if (!c) return;
    c->loaded = true;
    c->supported = 0;
    c->generated = c->missing = c->too_large = false;
    if (!success) {
        c->missing = true; /* config_not_found (generated by the frontend), or refused */
        (void)msg;
    } else if (msg_bytes_ > kMaxDashBytes) {
        c->too_large = true;
    } else {
        Dashboard d;
        const Import res = import_lovelace(r, d);
        if (res == Import::Generated) c->generated = true;
        else if (res == Import::Invalid) c->missing = true;
        else c->supported = supported_cards(d);
        if (c->title.empty()) c->title = d.title;
        if (c->supported > 0) dashes_[c->key] = std::move(d);
    }
    if (c->title.empty()) c->title = c->key == kDashDefault ? "Overview" : c->key;
    if (c->supported <= 0) dashes_.erase(c->key);
}

/* The dashboard to show again (after the answers, or a new setting). Unforced, it waits for every answer, so that the
 * tabs never flip while they arrive. */
void Engine::apply_dashboards(bool force, std::int64_t now)
{
    if (!force && (!dash_list_done_ || !sysdata_done_ || dash_wait_ > 0)) return;
    std::string key = resolve_dashboard(settings_.dashboard, dash_choices_, default_panel_);
    if (!key.empty() && !dashes_.count(key)) key.clear();
    const std::string want = wanted_dashboard(settings_.dashboard, dash_choices_, default_panel_);
    if (!want.empty() && !dash_said_large_)
        if (DashChoice *c = choice(want))
            if (c->too_large) {
                dash_said_large_ = true;
                say("The dashboard \"" + c->title + "\" is larger than 1 MB: the deck shows the tiles instead", true);
            }
    dash_key_ = key;
    refresh_dash_ids();
    update_subscription();
    update_dash_extras(now);
    save_dashboard();
    changed();
}

void Engine::refresh_dash_ids()
{
    dash_ids_.clear();
    if (const Dashboard *d = shown_dashboard())
        for (const DashView &v : d->views)
            for (const std::string &id : view_entities(v)) dash_ids_.insert(id);
    for (const std::string &id : dash_ids_)
        if (const State *s = model_.state(id))
            if (!s->friendly_name.empty()) dash_names_[id] = s->friendly_name;
    for (auto it = dash_names_.begin(); it != dash_names_.end();) {
        if (!dash_ids_.count(it->first)) it = dash_names_.erase(it);
        else ++it;
    }
}

const Dashboard *Engine::shown_dashboard() const
{
    if (dash_key_.empty()) return nullptr;
    auto it = dashes_.find(dash_key_);
    return it == dashes_.end() ? nullptr : &it->second;
}

const DashView *Engine::shown_view() const
{
    const Dashboard *d = shown_dashboard();
    if (!d || view_dash_ < 0 || view_dash_ >= static_cast<int>(d->views.size())) return nullptr;
    const DashView &v = d->views[static_cast<std::size_t>(view_dash_)];
    return view_visible(v, user_id_) ? &v : nullptr;
}

/* The forecasts of the view shown (subscribed while it shows) and its sensor cards' history (asked when it shows,
 * again after 5 minutes). */
void Engine::update_dash_extras(std::int64_t now)
{
    if (st_ != St::Online || !registry_ready_) return;
    const DashView *v = shown_view();
    const std::vector<std::string> want = v ? view_forecasts(*v) : std::vector<std::string>();
    for (auto it = forecast_sub_.begin(); it != forecast_sub_.end();) {
        if (std::find(want.begin(), want.end(), it->first) == want.end()) {
            send("unsubscribe_events", "\"subscription\":" + std::to_string(it->second), Req::Unsubscribe);
            forecast_of_.erase(it->second);
            it = forecast_sub_.erase(it);
        } else {
            ++it;
        }
    }
    for (const std::string &e : want) {
        if (forecast_sub_.count(e)) continue;
        const int id = send("weather/subscribe_forecast",
                            "\"entity_id\":" + json::quote(e) + ",\"forecast_type\":\"daily\"", Req::Forecast, e);
        forecast_sub_[e] = id;
        forecast_of_[id] = e;
    }
    if (!v || wall_ <= 0) return;
    for (const DashCard *c : view_cards(*v)) {
        if (c->kind != DashCard::Kind::Sensor || !c->graph || c->entity.empty()) continue;
        const std::string key = graph_key(*c);
        auto at = graph_at_.find(key);
        if (at != graph_at_.end() && now - at->second < kGraphRefresh) continue;
        graph_at_[key] = now;
        const std::int64_t t1 = wall_, t0 = wall_ - static_cast<std::int64_t>(c->hours) * 3600;
        const int id = send("history/history_during_period",
                            "\"start_time\":" + json::quote(iso_utc(t0)) + ",\"end_time\":" + json::quote(iso_utc(t1)) +
                                ",\"entity_ids\":[" + json::quote(c->entity) +
                                "],\"minimal_response\":true,\"no_attributes\":true,\"significant_changes_only\":false",
                            Req::History, c->entity, key);
        Pending &p = pending_req_[id];
        p.t0 = t0;
        p.t1 = t1;
        p.points = graph_points(c->hours, c->detail);
    }
}

void Engine::save_dashboard()
{
    const std::string text = dashboard_text();
    if (text.empty() || text == dash_saved_) return;
    dash_saved_ = text;
    push(Action{Action::Kind::SaveDashboard, "", "", "", HttpTag::CheckServer});
}

std::string Engine::dashboard_text() const
{
    std::string s = "{\n  \"version\": 1,\n  \"key\": " + json::quote(dash_key_) + ",\n  \"default_panel\": " +
                    json::quote(default_panel_) + ",\n  \"user_id\": " + json::quote(user_id_) + ",\n  \"choices\": [";
    bool first = true;
    for (const DashChoice &c : dash_choices_) {
        if (!c.loaded) continue;
        s += std::string(first ? "\n    " : ",\n    ") + "{\"key\": " + json::quote(c.key) + ", \"title\": " +
             json::quote(c.title) + ", \"supported\": " + std::to_string(c.supported) + "}";
        first = false;
    }
    s += "\n  ],\n  \"names\": {";
    first = true;
    for (const auto &kv : dash_names_) {
        s += std::string(first ? "\n    " : ",\n    ") + json::quote(kv.first) + ": " + json::quote(kv.second);
        first = false;
    }
    s += "\n  }";
    if (const Dashboard *d = shown_dashboard()) s += ",\n  \"config\": " + lovelace_text(*d);
    s += "\n}\n";
    return s.size() > kMaxDashBytes ? std::string() : s;
}

bool Engine::set_cached_dashboard(const std::string &text)
{
    json::Value v;
    if (text.empty() || text.size() > kMaxDashBytes || !json::parse(text, v) || !v.is_object()) return false;
    const json::Value *ver = v.get("version");
    if (!ver || ver->as_int(0) != 1) return false;
    std::vector<DashChoice> choices;
    if (const json::Value *list = v.get("choices"))
        for (const json::Value &c : list->items) {
            DashChoice d;
            if (const json::Value *k = c.get("key")) d.key = k->as_string();
            if (const json::Value *t = c.get("title")) d.title = t->as_string();
            if (const json::Value *n = c.get("supported")) d.supported = static_cast<int>(n->as_int(0));
            d.loaded = true;
            if (!d.key.empty() && choices.size() <= kMaxDashboards) choices.push_back(std::move(d));
        }
    const std::string key = v.get("key") ? v.get("key")->as_string() : std::string();
    if (const json::Value *d = v.get("default_panel")) default_panel_ = d->as_string();
    if (const json::Value *u = v.get("user_id")) user_id_ = u->as_string();
    dash_names_.clear();
    if (const json::Value *names = v.get("names"))
        for (const auto &m : names->members)
            if (m.first.find('.') != std::string::npos && dash_names_.size() < 5000) dash_names_[m.first] = m.second.as_string();
    dash_choices_ = std::move(choices);
    dashes_.clear();
    if (const json::Value *config = v.get("config")) {
        Dashboard d;
        if (!key.empty() && import_lovelace(*config, d) == Import::Ok && supported_cards(d) > 0) dashes_[key] = std::move(d);
    }
    dash_key_ = resolve_dashboard(settings_.dashboard, dash_choices_, default_panel_);
    if (!dashes_.count(dash_key_)) dash_key_.clear();
    std::map<std::string, std::string> names = dash_names_;
    refresh_dash_ids();
    for (const auto &kv : names) /* nothing is subscribed yet: keep the names as they were */
        if (dash_ids_.count(kv.first)) dash_names_[kv.first] = kv.second;
    dash_saved_ = text;
    changed();
    return true;
}

namespace {

/* the glyph names of the domains Home Assistant's rules are not asked for (rows of other domains) */
std::string other_icon(const std::string &id, const State *s)
{
    const std::string d = id.substr(0, id.find('.'));
    const std::string st = s ? s->state : std::string();
    if (d == "weather") return weather_icon(st);
    if (d == "person" || d == "device_tracker") return st == "not_home" ? "mdi:account-arrow-right" : "mdi:account";
    if (d == "fan") return st == "off" ? "mdi:fan-off" : "mdi:fan";
    if (d == "sun") return st == "below_horizon" ? "mdi:weather-night" : "mdi:white-balance-sunny";
    if (d == "automation") return st == "off" ? "mdi:robot-off" : "mdi:robot";
    if (d == "media_player") return st == "off" ? "mdi:cast-off" : "mdi:cast";
    if (d == "input_number" || d == "number") return "mdi:ray-vertex";
    if (d == "input_select" || d == "select") return "mdi:format-list-bulleted";
    if (d == "button" || d == "input_button") return "mdi:gesture-tap-button";
    if (d == "update") return "mdi:package-up";
    if (d == "zone") return "mdi:map-marker-radius";
    return "mdi:bookmark";
}

std::string degrees(double t)
{
    if (!std::isfinite(t)) return "";
    char buf[24];
    std::snprintf(buf, sizeof buf, "%.0f\xC2\xB0", t);
    return buf;
}

} // namespace

EntityRow Engine::dash_entity(const std::string &id, const std::string &name, const std::string &icon,
                              std::int64_t now) const
{
    EntityRow r = row(id, now);
    if (!name.empty()) r.name = name;
    const MdiNames &names = mdi();
    if (!names.loaded()) return r;
    const State *s = model_.state(id);
    std::uint32_t cp = icon.empty() ? 0 : names.lookup(icon);
    if (!cp && !supported(r.domain) && !model_.entry(id)) {
        /* no rule of Home Assistant asked for this domain: the state's icon attribute, else the app's own */
        if (s && !s->icon.empty()) cp = names.lookup(s->icon);
        if (!cp) cp = names.lookup(other_icon(id, s));
    }
    if (cp) r.glyph = cp;
    if (s && !r.unavailable && id.compare(0, 4, "fan.") == 0 && s->state == "on") { /* a fan on: its state colour */
        r.active = true;
        r.glyph_rgb = rgb::kCyan;
        r.circle_rgb = mix_rgb(rgb::kCyan, rgb::kTile, rgb::kActivePct);
    }
    return r;
}

DashRowView Engine::dash_row(const DashRow &row, std::int64_t now) const
{
    DashRowView v;
    v.kind = row.kind;
    switch (row.kind) {
    case DashRow::Kind::Section: v.label = row.name; break;
    case DashRow::Kind::Divider: break;
    case DashRow::Kind::Other: v.label = row.name.empty() ? row.type : row.name; break;
    case DashRow::Kind::Entity: {
        v.e = dash_entity(row.entity, row.name, row.icon, now);
        v.toggle = toggle_domain(row.entity);
        v.tap = row.tap;
        const State *s = model_.state(row.entity);
        const RegEntry *re = model_.entry(row.entity);
        v.has_state = s != nullptr;
        if (v.e.pending) v.word = "...";
        else if (v.e.done) v.word = "Done";
        else if (s) v.word = entity_word(row.entity, s, re ? re->precision : -1, temp_unit_raw_);
        else if (st_ == St::Online && sub_loaded_ && sub_set_.count(row.entity)) {
            /* subscribed, and Home Assistant has no state for it: removed or disabled */
            v.available = false;
            v.word = "Not available";
            v.e.unavailable = true;
            v.e.glyph_rgb = rgb::kDisabled;
        } else {
            v.word = v.e.tile_value; /* "...", or "Offline" */
        }
        break;
    }
    }
    return v;
}

std::vector<DashCardView> Engine::dash_cards(const DashView &view, std::int64_t now) const
{
    std::vector<DashCardView> out;
    for (const DashCard *c : view_cards(view)) {
        DashCardView cv;
        cv.kind = c->kind;
        cv.type = c->type;
        cv.title = c->title;
        switch (c->kind) {
        case DashCard::Kind::Entities: {
            for (const DashRow &r : c->rows) cv.rows.push_back(dash_row(r, now));
            if (c->header_toggle)
                for (const DashRowView &r : cv.rows) {
                    if (r.kind != DashRow::Kind::Entity || !r.toggle || !r.available || r.e.unavailable ||
                        !model_.state(r.e.id) ||
                        std::find(cv.toggle_ids.begin(), cv.toggle_ids.end(), r.e.id) != cv.toggle_ids.end())
                        continue;
                    cv.toggle_ids.push_back(r.e.id);
                    cv.header_on = cv.header_on || r.e.on;
                    cv.header_pending = cv.header_pending || r.e.pending;
                }
            cv.header_toggle = !cv.toggle_ids.empty();
            break;
        }
        case DashCard::Kind::Sensor: {
            cv.e = dash_entity(c->entity, c->title, c->icon, now);
            const State *s = model_.state(c->entity);
            const RegEntry *re = model_.entry(c->entity);
            if (s && !unavailable(*s)) {
                double num = 0;
                char *end = nullptr;
                num = std::strtod(s->state.c_str(), &end);
                cv.value = end && *end == '\0' && std::isfinite(num) ? format_number(s->state, re ? re->precision : -1)
                                                                     : entity_word(c->entity, s, -1, temp_unit_raw_);
                cv.unit = c->unit.empty() ? tile_unit(s->unit) : c->unit;
            } else {
                cv.value = s ? entity_word(c->entity, s, -1, temp_unit_raw_) : cv.e.tile_value;
            }
            cv.graph = c->graph;
            auto g = graphs_.find(graph_key(*c));
            if (c->graph && g != graphs_.end()) {
                bool any = false;
                for (double x : g->second) any = any || std::isfinite(x);
                if (any) cv.spark = scale_points(g->second);
            }
            break;
        }
        case DashCard::Kind::Weather: {
            cv.e = dash_entity(c->entity, c->title, "", now);
            const State *s = model_.state(c->entity);
            cv.forecast = c->forecast;
            if (c->current) {
                if (s) {
                    cv.condition = weather_word(s->state);
                    const std::string unit = s->temperature_unit.empty() ? temp_unit_raw_ : s->temperature_unit;
                    const std::string t = card_temperature(s->target_temperature, "");
                    cv.value = t;
                    cv.unit = t.empty() ? std::string() : tile_unit(unit);
                } else {
                    cv.condition = cv.e.tile_value;
                }
                const MdiNames &names = mdi();
                cv.big_glyph = s && names.loaded() ? names.lookup(weather_icon(s->state)) : 0;
                if (!cv.big_glyph) cv.big_glyph = cv.e.glyph;
            }
            auto f = forecasts_.find(c->entity);
            if (c->forecast && f != forecasts_.end()) {
                const MdiNames &names = mdi();
                for (const ForecastDay &d : f->second) {
                    ForecastCol col;
                    col.day = d.day;
                    col.glyph = names.loaded() ? names.lookup(weather_icon(d.condition)) : 0;
                    col.high = degrees(d.high);
                    col.low = degrees(d.low);
                    cv.days.push_back(std::move(col));
                }
            }
            break;
        }
        case DashCard::Kind::Unsupported: break;
        }
        out.push_back(std::move(cv));
    }
    return out;
}

/* --------------------------------------------------------------------------------------------- the view */

EntityRow Engine::row(const std::string &id, std::int64_t now) const
{
    EntityRow r;
    r.id = id;
    r.domain = domain_of(id);
    r.action = row_action(r.domain);
    r.tile_action = tile_action(r.domain);
    r.name = name_of(id);
    const RegEntry *e = model_.entry(id);
    const State *s = model_.state(id);
    if (s) {
        r.value = state_label(r.domain, *s, e ? e->precision : -1, temp_unit_);
        r.tile_value = tile_word(r.domain, *s, e ? e->precision : -1, temp_unit_raw_);
        r.muted = unavailable(*s) && r.domain != Domain::Scene;
        r.running = r.domain == Domain::Script && s->state == "on";
        r.dimmable = r.domain == Domain::Light && s->dimmable;
        r.brightness_pct = brightness_pct(*s);
        r.on = is_on(r.domain, *s);
        r.confirm = r.domain == Domain::Lock || (r.domain == Domain::Cover && confirm_cover(*s));
    } else {
        r.muted = true;
        r.tile_value = failures_ >= kOfflineAfter && st_ != St::Online ? "Offline" : "...";
        r.confirm = r.domain == Domain::Lock || r.domain == Domain::Cover; /* unknown class: ask */
    }
    const TileLook look = tile_look(r.domain, s);
    r.active = look.active;
    r.unavailable = look.unavailable;
    r.glyph_rgb = look.glyph;
    r.circle_rgb = look.circle;
    const MdiNames &names = mdi();
    if (names.loaded()) {
        const std::string icon =
            resolve_icon(id, e, s, icons_, [&names](const std::string &n) { return names.lookup(n) != 0; });
        r.glyph = names.lookup(icon);
    }
    auto p = pending_until_.find(id);
    if (p != pending_until_.end() && now < p->second) {
        r.pending = true;
        r.value = "...";
        r.tile_value = "...";
        r.muted = false;
    }
    auto d = done_until_.find(id);
    if (d != done_until_.end() && now < d->second) {
        r.done = true;
        r.value = "Done";
        r.tile_value = "Done";
        r.muted = false;
    }
    if (st_ != St::Online) r.muted = true; /* the last value, greyed */
    return r;
}

View Engine::view(std::int64_t now, std::int64_t wall) const
{
    View v;
    v.signed_in = creds_.valid();
    const Server s = server();
    v.server_display = s.valid() ? s.display() : settings_.server;
    v.server_host = s.valid() ? s.host : settings_.server;
    v.server_url = creds_.valid() ? creds_.server : (s.valid() ? s.base_url() : std::string());
    v.last_update = last_update_;
    v.user = user_;
    v.ha_version = ha_version_;
    v.registry_loaded = registry_ever_;
    if (!v.signed_in) v.conn = Conn::SignedOut;
    else if (st_ == St::Online) v.conn = Conn::Connected;
    else if (failures_ >= kOfflineAfter) v.conn = Conn::Offline;
    else if (!ever_connected_) v.conn = Conn::Connecting;
    else v.conn = Conn::Reconnecting;

    v.qr_url = qr_url_;
    v.pair_status = pair_status_;
    v.pair_tone = pair_tone_;
    v.paired_now = paired_now_;

    for (const std::string &id : favourites_) v.favourites.push_back(row(id, now));
    const std::vector<RoomInfo> rooms = model_.rooms();
    for (const RoomInfo &r : rooms) v.rooms.push_back(RoomRow{r.id, r.name, r.count});
    /* the stored dashboard (0.3.0): its views' tabs first, the cards of the view shown */
    v.dash_setting = settings_.dashboard;
    for (const DashChoice &c : offered_dashboards(dash_choices_)) v.dash_choices.emplace_back(c.key, c.title);
    const std::string auto_key = resolve_dashboard(kDashAuto, dash_choices_, default_panel_);
    for (const DashChoice &c : dash_choices_)
        if (c.key == auto_key) v.dash_auto_title = c.title;
    std::vector<Tab> view_tabs;
    if (const Dashboard *d = shown_dashboard()) {
        const MdiNames &names = mdi();
        for (std::size_t i = 0; i < d->views.size(); ++i) {
            const DashView &dv = d->views[i];
            if (!view_visible(dv, user_id_)) continue;
            DashTab t;
            t.view = static_cast<int>(i);
            t.title = dv.title;
            t.glyph = names.loaded() ? names.lookup(dv.icon) : 0;
            v.dash_tabs.push_back(t);
            Tab tab;
            tab.kind = Tab::Kind::View;
            tab.view = t.view;
            tab.title = t.title.empty() && !t.glyph ? "View " + std::to_string(i + 1) : t.title;
            tab.glyph = t.glyph;
            view_tabs.push_back(std::move(tab));
        }
        v.dash = !v.dash_tabs.empty();
        v.dash_key = dash_key_;
        for (const DashChoice &c : dash_choices_)
            if (c.key == dash_key_) v.dash_title = c.title;
        if (const DashView *sv = shown_view()) {
            v.dash_view = view_dash_;
            v.dash_panel = sv->type == "panel";
            v.dash_cards = dash_cards(*sv, now);
        }
    }
    v.tabs = make_tabs(rooms, view_tabs);
    v.room_shown = view_room_;
    if (view_room_) {
        v.room_id = view_room_id_;
        v.room_name = model_.area_name(view_room_id_);
        for (const std::string &id : model_.entities_in(view_room_id_)) v.room.push_back(row(id, now));
    }
    v.entity_id = view_entity_;
    if (!view_entity_.empty()) {
        Detail &d = v.detail;
        const EntityRow r = row(view_entity_, now);
        d.tile = r;
        d.known = model_.known(view_entity_);
        d.id = view_entity_;
        d.name = r.name;
        d.domain = r.domain;
        d.action = r.action;
        d.value = r.value;
        d.muted = r.muted;
        d.pending = r.pending;
        const std::string area = model_.area_of(view_entity_);
        d.room = model_.entry(view_entity_) ? model_.area_name(area) : std::string();
        d.favourite = std::find(favourites_.begin(), favourites_.end(), view_entity_) != favourites_.end();
        if (const State *st = model_.state(view_entity_)) {
            d.info = detail_rows(d.domain, *st, temp_unit_raw_, d.room, wall); /* the page writes "21.4 °C" */
            d.dimmable = d.domain == Domain::Light && st->dimmable;
            d.brightness_pct = brightness_pct(*st);
            d.on = is_on(d.domain, *st);
            d.confirm = d.domain == Domain::Lock || (d.domain == Domain::Cover && confirm_cover(*st));
            const std::string changed = changed_text(st->last_changed, wall);
            d.meta = d.room;
            if (!changed.empty()) d.meta += (d.meta.empty() ? "Changed " : " - changed ") + changed;
        } else {
            if (!d.room.empty()) d.info.emplace_back("Room", d.room);
            d.confirm = d.domain == Domain::Lock || d.domain == Domain::Cover; /* unknown class: ask */
            d.meta = d.room;
        }
    }
    v.message = message_;
    v.message_error = message_error_;
    v.message_seq = message_seq_;
    v.signing_out = signing_out_;
    return v;
}

} // namespace ha
