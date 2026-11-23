/*
 * SPDX-License-Identifier: MIT
 *
 * Unit tests of the Reticulum app's core/ (no LVGL, no screen): the JSON reader, the link to the bridge (against a
 * small in-process server on a temporary Unix socket), the plain-language model, and the package files. Built and run
 * by ../../_common/build_app.sh host (make -C _common/headless).
 */
#include "json.hpp"
#include "link.hpp"
#include "model.hpp"

#include <atomic>
#include <cctype>
#include <fcntl.h>
#include <vector>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <dirent.h>
#include <fstream>
#include <functional>
#include <sstream>
#include <string>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <thread>
#include <unistd.h>

#ifndef CDK_APP_DIR
#define CDK_APP_DIR "."
#endif

static int g_pass = 0, g_fail = 0;
#define CHECK(c)                                                                                                       \
    do {                                                                                                               \
        if (c) ++g_pass;                                                                                               \
        else {                                                                                                         \
            ++g_fail;                                                                                                  \
            std::fprintf(stderr, "%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #c);                                 \
        }                                                                                                              \
    } while (0)

using ret::json::Value;

static std::string read_file(const std::string &path)
{
    std::ifstream in(path);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

static Value J(const std::string &text)
{
    Value v;
    if (!ret::json::parse(text, v)) std::fprintf(stderr, "bad test json: %s\n", text.c_str());
    return v;
}

/* ------------------------------------------------------------------ json */

static void test_json()
{
    Value v = J(R"({"a":1,"b":[true,null,"x\n\u00e9\ud83d\ude00"],"c":{"d":-2.5e1}})");
    CHECK(v.is_object());
    CHECK(v["a"].as_int() == 1);
    CHECK(v["b"].size() == 3);
    CHECK(v["b"][0].as_bool());
    CHECK(v["b"][1].is_null());
    CHECK(v["b"][2].as_string() == "x\n\xC3\xA9\xF0\x9F\x98\x80");
    CHECK(v["c"]["d"].as_number() == -25.0);
    CHECK(v["missing"]["deeper"].is_null());
    CHECK(v["b"][9].is_null());
    CHECK(v["a"].as_string().empty());
    CHECK(v["b"].as_int(7) == 7);

    Value out = Value::object();
    out.set("name", Value::string("a\"b\\c\t\x01"));
    out.set("n", Value::number(868));
    out.set("f", Value::number(0.5));
    out.set("t", Value::boolean(true));
    out.set("z", Value());
    Value arr = Value::array();
    arr.push(Value::number(1)).push(Value::string("é"));
    out.set("l", arr);
    const std::string text = ret::json::dump(out);
    CHECK(text == "{\"name\":\"a\\\"b\\\\c\\t\\u0001\",\"n\":868,\"f\":0.5,\"t\":true,\"z\":null,\"l\":[1,\"\xC3\xA9\"]}");
    Value back;
    CHECK(ret::json::parse(text, back));
    CHECK(ret::json::dump(back) == text);
    out.set("n", Value::number(1)); /* a repeated key is replaced */
    CHECK(out["n"].as_int() == 1 && out.size() == 6);

    Value bad;
    CHECK(!ret::json::parse("", bad));
    CHECK(!ret::json::parse("{", bad));
    CHECK(!ret::json::parse("{\"a\":}", bad));
    CHECK(!ret::json::parse("[1,2", bad));
    CHECK(!ret::json::parse("[1] x", bad));
    CHECK(!ret::json::parse("\"\\ud83d\"", bad));
    CHECK(!ret::json::parse("01", bad));
    CHECK(!ret::json::parse("\"a\nb\"", bad));
    CHECK(ret::json::parse(" [ ] ", bad) && bad.is_array());
    std::string deep(40, '[');
    deep += std::string(40, ']');
    CHECK(!ret::json::parse(deep, bad));
    CHECK(ret::json::number_text(868000000) == "868000000");
    CHECK(ret::json::number_text(2.5) == "2.5");
}

/* ------------------------------------------------------------------ link */

class FakeServer {
public:
    using Handler = std::function<std::string(const std::string &line)>; /* the reply lines to send ("" = none) */
    explicit FakeServer(Handler h) : handler_(std::move(h))
    {
        char tmpl[] = "/tmp/ret-test-XXXXXX";
        dir_ = mkdtemp(tmpl);
        path_ = dir_ + "/bridge.sock";
        fd_ = socket(AF_UNIX, SOCK_STREAM, 0);
        sockaddr_un a{};
        a.sun_family = AF_UNIX;
        std::snprintf(a.sun_path, sizeof a.sun_path, "%s", path_.c_str());
        bind(fd_, reinterpret_cast<sockaddr *>(&a), sizeof a);
        listen(fd_, 4);
        thread_ = std::thread([this] { run(); });
    }
    ~FakeServer()
    {
        stop_ = true;
        if (fd_ >= 0) shutdown(fd_, SHUT_RDWR);
        if (client_ >= 0) shutdown(client_, SHUT_RDWR);
        if (thread_.joinable()) thread_.join();
        if (fd_ >= 0) close(fd_);
        unlink(path_.c_str());
        rmdir(dir_.c_str());
    }
    const std::string &path() const { return path_; }
    std::vector<std::string> lines()
    {
        return seen_;
    }
    void push(const std::string &line) /* an event line while idle */
    {
        const std::string l = line + "\n";
        if (client_ >= 0) (void)!write(client_, l.data(), l.size());
    }
    void close_client() { close_next_ = true; }
    std::atomic<int> connects{0};

private:
    void run()
    {
        while (!stop_) {
            client_ = accept(fd_, nullptr, nullptr);
            if (client_ < 0) return;
            ++connects;
            std::string buf;
            char tmp[1024];
            while (!stop_) {
                const ssize_t n = read(client_, tmp, sizeof tmp);
                if (n <= 0) break;
                buf.append(tmp, static_cast<std::size_t>(n));
                std::size_t nl;
                while ((nl = buf.find('\n')) != std::string::npos) {
                    const std::string line = buf.substr(0, nl);
                    buf.erase(0, nl + 1);
                    seen_.push_back(line);
                    if (close_next_) { /* hang up on this request without answering it */
                        close_next_ = false;
                        goto done;
                    }
                    const std::string reply = handler_(line);
                    if (!reply.empty()) (void)!write(client_, reply.data(), reply.size());
                }
            }
        done:
            close(client_);
            client_ = -1;
        }
    }
    Handler handler_;
    std::string dir_, path_;
    int fd_ = -1;
    std::atomic<int> client_{-1};
    std::atomic<bool> stop_{false};
    std::atomic<bool> close_next_{false};
    std::thread thread_;
    std::vector<std::string> seen_;
};

static std::string id_of(const std::string &line)
{
    return J(line)["id"].is_number() ? ret::json::number_text(J(line)["id"].as_number()) : "null";
}

static bool wait_for(ret::Link &link, const std::function<bool()> &done, int ms = 6000)
{
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (std::chrono::steady_clock::now() < end) {
        link.poll();
        if (done()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    link.poll();
    return done();
}

static void test_link_pure()
{
    CHECK(ret::is_transmit_enabling("announce", Value::object()));
    CHECK(ret::is_transmit_enabling("lxmf.send", Value::object()));
    CHECK(ret::is_transmit_enabling("iface.outgoing", J(R"({"outgoing":true})")));
    CHECK(!ret::is_transmit_enabling("iface.outgoing", J(R"({"outgoing":false})")));
    CHECK(!ret::is_transmit_enabling("iface.outgoing", Value::object()));
    CHECK(!ret::is_transmit_enabling("iface.enable", J(R"({"enabled":true})")));
    CHECK(!ret::is_transmit_enabling("iface.add", Value::object()));
    CHECK(!ret::is_transmit_enabling("identity.name", J(R"({"name":"x"})")));

    ret::Response r;
    CHECK(ret::parse_reply(R"({"id":7,"ok":true,"result":{"a":1}})", 7, r));
    CHECK(r.ok && r.transport && r.result["a"].as_int() == 1);
    CHECK(!ret::parse_reply(R"({"id":8,"ok":true,"result":{}})", 7, r));
    CHECK(!ret::parse_reply(R"({"event":"announce","data":{}})", 7, r));
    CHECK(!ret::parse_reply("garbage", 7, r));
    CHECK(ret::parse_reply(R"({"id":7,"ok":false,"error":{"code":"busy","message":"m","field":"port"}})", 7, r));
    CHECK(!r.ok && r.code == "busy" && r.message == "m" && r.field == "port");
}

static void test_link_server()
{
    FakeServer server([](const std::string &line) -> std::string {
        const Value v = J(line);
        const std::string id = id_of(line);
        const std::string verb = v["verb"].as_string();
        if (verb == "announces.subscribe")
            return "{\"id\":" + id + ",\"ok\":true,\"result\":{\"subscribed\":true,\"recent\":[{\"destination\":\"ab12\",\"time\":5,\"name\":\"Kim\",\"hops\":2}]}}\n";
        if (verb == "hello") return "{\"event\":\"announce\",\"data\":{\"destination\":\"cd34\"}}\n{\"id\":" + id + ",\"ok\":true,\"result\":{\"protocol\":1}}\n";
        if (verb == "fail") return "{\"id\":" + id + ",\"ok\":false,\"error\":{\"code\":\"invalid\",\"message\":\"bad\",\"field\":\"x\"}}\n";
        if (verb == "silent") return "";
        if (verb == "iface.outgoing") return "{\"id\":" + id + ",\"ok\":true,\"result\":{}}\n";
        return "{\"id\":" + id + ",\"ok\":true,\"result\":{\"echo\":\"" + verb + "\"}}\n";
    });
    ret::Link link(server.path());
    link.set_subscribe(true);

    int got = 0;
    ret::Response hello;
    link.request("hello", Value::object(), 2, [&](const ret::Response &r) {
        hello = r;
        ++got;
    });
    CHECK(wait_for(link, [&] { return got == 1; }));
    CHECK(hello.ok && hello.result["protocol"].as_int() == 1);
    CHECK(link.connected());
    /* the subscribe step ran first (read-only), its recent list and the event before the reply came as events */
    const auto events = link.take_events();
    CHECK(events.size() == 2);
    CHECK(events.size() >= 1 && events[0]["data"]["destination"].as_string() == "ab12");
    CHECK(server.lines().size() >= 2 && J(server.lines()[0])["verb"].as_string() == "announces.subscribe");
    CHECK(link.take_events().empty());

    /* an error reply carries code, message and field */
    ret::Response fail;
    link.request("fail", Value::object(), 2, [&](const ret::Response &r) {
        fail = r;
        ++got;
    });
    CHECK(wait_for(link, [&] { return got == 2; }));
    CHECK(!fail.ok && fail.transport && fail.code == "invalid" && fail.field == "x" && fail.message == "bad");

    /* requests are answered in order, every callback once */
    std::vector<std::string> order;
    for (const char *verb : {"a", "b", "c"})
        link.request(verb, Value::object(), 2, [&order](const ret::Response &r) { order.push_back(r.result["echo"].as_string()); });
    CHECK(wait_for(link, [&] { return order.size() == 3; }));
    CHECK(order == std::vector<std::string>({"a", "b", "c"}));
    CHECK(link.pending() == 0);

    /* an event that arrives while idle is kept */
    server.push(R"({"event":"announce","data":{"destination":"ef56","name":"Lee"}})");
    std::vector<Value> late;
    CHECK(wait_for(link, [&] {
        for (const Value &e : link.take_events()) late.push_back(e);
        return !late.empty();
    }));
    CHECK(late[0]["data"]["name"].as_string() == "Lee");

    /* the transmit guard: refused here, never sent, unless the user acted */
    const std::size_t before = server.lines().size();
    ret::Response blocked;
    link.request("announce", Value::object(), 2, [&](const ret::Response &r) {
        blocked = r;
        ++got;
    });
    Value on = Value::object();
    on.set("name", Value::string("x"));
    on.set("outgoing", Value::boolean(true));
    link.request("iface.outgoing", on, 2, [&](const ret::Response &r) {
        blocked = r;
        ++got;
    });
    CHECK(wait_for(link, [&] { return got == 4; }));
    CHECK(blocked.code == "blocked" && !blocked.ok);
    CHECK(link.blocked() == 2);
    CHECK(server.lines().size() == before);
    ret::Response allowed;
    link.request("iface.outgoing", on, 2, [&](const ret::Response &r) {
        allowed = r;
        ++got;
    }, /*user_action=*/true);
    CHECK(wait_for(link, [&] { return got == 5; }));
    CHECK(allowed.ok);
    CHECK(server.lines().size() == before + 1);
    Value off = on;
    off.set("outgoing", Value::boolean(false));
    link.request("iface.outgoing", off, 2, [&](const ret::Response &r) {
        allowed = r;
        ++got;
    });
    CHECK(wait_for(link, [&] { return got == 6; }));
    CHECK(allowed.ok && link.blocked() == 2); /* listen-only needs no consent */

    /* a bridge that goes quiet: no_answer after the bridge's timeout plus the link's slack, then it reconnects */
    ret::Response silent;
    link.request("silent", Value::object(), 0.5, [&](const ret::Response &r) {
        silent = r;
        ++got;
    });
    CHECK(wait_for(link, [&] { return got == 7; }, 8000));
    CHECK(!silent.transport && silent.code == "no_answer");
    ret::Response again;
    link.request("again", Value::object(), 2, [&](const ret::Response &r) {
        again = r;
        ++got;
    });
    CHECK(wait_for(link, [&] { return got == 8; }));
    CHECK(again.ok && server.connects >= 2);

    /* a bridge that hangs up: lost */
    server.close_client();
    ret::Response lost;
    link.request("bye", Value::object(), 2, [&](const ret::Response &r) {
        lost = r;
        ++got;
    });
    CHECK(wait_for(link, [&] { return got == 9; }, 8000));
    CHECK(!lost.ok && !lost.transport && lost.code == "lost");
    /* and the next request reconnects by itself */
    ret::Response next;
    link.request("hello", Value::object(), 2, [&](const ret::Response &r) {
        next = r;
        ++got;
    });
    CHECK(wait_for(link, [&] { return got == 10; }));
    CHECK(next.ok && server.connects >= 3);
}

static void test_link_no_service()
{
    ret::Link missing("/tmp/ret-test-does-not-exist/bridge.sock");
    int got = 0;
    ret::Response r;
    missing.request("hello", Value::object(), 2, [&](const ret::Response &x) {
        r = x;
        ++got;
    });
    CHECK(wait_for(missing, [&] { return got == 1; }));
    CHECK(!r.ok && !r.transport && r.code == "no_service");
    CHECK(!ret::plain_error(r).empty());

    ret::Link empty{std::string()};
    empty.request("hello", Value::object(), 2, [&](const ret::Response &x) {
        r = x;
        ++got;
    });
    CHECK(wait_for(empty, [&] { return got == 2; }));
    CHECK(r.code == "no_service");

    /* destroying a link with work queued is clean and runs no callback */
    int late = 0;
    {
        ret::Link gone("/tmp/ret-test-does-not-exist/bridge.sock");
        gone.request("hello", Value::object(), 2, [&](const ret::Response &) { ++late; });
    }
    CHECK(late == 0);
}

/* ------------------------------------------------------------------ model */

static const char *kTypes = R"({"types":[
 {"type":"AutoInterface","supported":true,"port_lock":false,"options":[{"name":"group_id","kind":"str","default":null}]},
 {"type":"TCPClientInterface","supported":true,"options":[{"name":"target_host","kind":"str","required":true,"default":null},
   {"name":"target_port","kind":"int","min":1,"max":65535,"required":true,"default":null}]},
 {"type":"RNodeInterface","supported":true,"port_lock":true,"options":[
   {"name":"port","kind":"str","required":true,"device":true,"default":null},
   {"name":"frequency","kind":"int","min":137000000,"max":3000000000,"unit":"Hz","required":true,"default":null},
   {"name":"spreadingfactor","kind":"int","min":5,"max":12,"required":true,"default":null},
   {"name":"flow_control","kind":"bool","default":false}]},
 {"type":"SerialInterface","supported":true,"options":[]},
 {"type":"RNodeMultiInterface","supported":false,"reason":"its sub-interfaces are nested sections","options":[]}],
 "common":[{"name":"mode","kind":"str","choices":["full","gateway"],"default":"full"},
   {"name":"passphrase","kind":"str","secret":true,"default":null},
   {"name":"announce_cap","kind":"float","min":1,"max":100,"unit":"%","default":2}],
 "switches":{"enabled":false,"outgoing":false}})";

static void test_model_parse()
{
    ret::Catalog cat;
    CHECK(ret::parse_catalog(J(kTypes), cat));
    CHECK(cat.loaded && cat.types.size() == 5 && cat.common.size() == 3);
    const ret::TypeDef *rn = cat.find("RNodeInterface");
    CHECK(rn && rn->port_lock && rn->options.size() == 4);
    CHECK(rn && rn->options[0].device && rn->options[0].required);
    CHECK(rn && rn->options[1].has_min && rn->options[1].min == 137000000 && rn->options[1].unit == "Hz");
    CHECK(rn && !rn->options[1].has_default);
    CHECK(rn && rn->options[3].kind == ret::OptKind::Bool && rn->options[3].has_default && rn->options[3].def == "no");
    CHECK(cat.find("RNodeMultiInterface") && !cat.find("RNodeMultiInterface")->supported);
    CHECK(cat.common[0].choices.size() == 2 && cat.common[1].secret);
    CHECK(!cat.find("Nope"));
    ret::Catalog none;
    CHECK(!ret::parse_catalog(J("{}"), none) && !none.loaded);

    std::vector<ret::Iface> ifs;
    CHECK(ret::parse_ifaces(J(R"({"interfaces":[
      {"name":"Default Interface","type":"AutoInterface","enabled":false,"outgoing":false,"online":false,"supported":true,"options":{},"missing":[],"stats":null,"error":null},
      {"name":"LoRa","type":"RNodeInterface","enabled":true,"outgoing":false,"online":true,"supported":true,
       "options":{"port":"/dev/ttyACM0","frequency":"868000000","flow_control":true},"secrets_set":["passphrase"],
       "missing":["spreadingfactor"],"lock":"held","stats":{"channel_load_short":3.2,"noise_floor":-112},"error":null}]})"), ifs));
    CHECK(ifs.size() == 2);
    CHECK(ifs[1].value("frequency") == "868000000");
    bool present = true;
    CHECK(ifs[1].value("spreadingfactor", &present).empty() && !present);
    CHECK(ifs[1].value("flow_control", &present) == "yes" && present);
    CHECK(ifs[1].secret_set("passphrase") && !ifs[1].secret_set("x"));
    CHECK(ifs[1].has_channel && ifs[1].channel_load == 3.2 && ifs[1].has_noise && ifs[1].noise == -112);
    CHECK(!ifs[0].has_channel);
    CHECK(ifs[1].missing.size() == 1 && ifs[1].lock == "held");
    CHECK(!ret::parse_ifaces(J("{}"), ifs));

    ret::Deck d;
    CHECK(ret::parse_deck(J(R"({"lxmf_address":"329a1f002b4c7d9e5566778899aabc09","display_name":null,"interfaces_total":2,
        "interfaces_up":["LoRa"],"uptime":65})"), d));
    CHECK(d.known && d.address.size() == 32 && d.display_name.empty() && d.total == 2 && d.up == 1 && d.uptime == 65);

    ret::Heard h;
    CHECK(ret::parse_heard(J(R"({"event":"announce","data":{"time":100.5,"destination":"aa","name":"Kim","kind":"lxmf","hops":1,"interface":"LoRa"}})"), h));
    CHECK(h.destination == "aa" && h.name == "Kim" && h.hops == 1 && h.time == 100.5 && h.iface == "LoRa");
    CHECK(ret::parse_heard(J(R"({"destination":"bb","name":null,"hops":null,"time":9})"), h) && h.hops == -1 && h.name.empty());
    CHECK(!ret::parse_heard(J(R"({"event":"stopping"})"), h));

    ret::HeardList list;
    ret::Heard a;
    a.destination = "aa";
    a.time = 10;
    ret::Heard b;
    b.destination = "bb";
    b.time = 20;
    list.add(a);
    list.add(b);
    CHECK(list.items().size() == 2 && list.items()[0].destination == "bb");
    a.time = 30;
    a.name = "Kim";
    list.add(a); /* later announce of the same destination replaces the earlier */
    CHECK(list.items().size() == 2 && list.items()[0].destination == "aa" && list.items()[0].name == "Kim");
    CHECK(list.count_within(40, 15) == 1 && list.count_within(40, 3600) == 2);
    for (int i = 0; i < 100; ++i) {
        ret::Heard x;
        x.destination = "d" + std::to_string(i);
        x.time = i;
        list.add(x);
    }
    CHECK(list.items().size() == 64);
}

static void test_model_words()
{
    ret::Catalog cat;
    ret::parse_catalog(J(kTypes), cat);
    ret::Iface i;
    i.name = "LoRa";
    i.type = "RNodeInterface";
    CHECK(ret::conn_state(i).word == "off" && ret::conn_state(i).tone == ret::Tone::Muted);
    i.enabled = true;
    CHECK(ret::conn_state(i).word == "on, not connected" && ret::conn_state(i).tone == ret::Tone::Warn);
    i.online = true;
    CHECK(ret::conn_state(i).word == "on, listen only" && ret::conn_state(i).tone == ret::Tone::Ok);
    i.outgoing = true;
    CHECK(ret::conn_state(i).word == "on, sending" && ret::conn_state(i).tone == ret::Tone::Warn);
    i.error = "port busy";
    CHECK(ret::conn_state(i).word == "problem" && ret::conn_state(i).tone == ret::Tone::Bad);
    i.enabled = false;
    CHECK(ret::conn_state(i).word == "off"); /* off wins over an old problem */
    i.supported = false;
    CHECK(ret::conn_state(i).word == "set by hand");

    std::vector<ret::Iface> all;
    CHECK(ret::connections_summary(all) == "all off");
    ret::Iface a, b;
    a.enabled = true;
    all = {a, b};
    CHECK(ret::connections_summary(all) == "1 on" && ret::count_on(all) == 1);
    CHECK(ret::heard_summary(0) == "nobody heard yet" && ret::heard_summary(3) == "3 heard");

    ret::Iface def;
    def.name = "Default Interface";
    def.type = "AutoInterface";
    CHECK(ret::conn_title(def) == "Wi-Fi LAN");
    CHECK(ret::conn_title(i) == "LoRa");
    CHECK(ret::suggest_name("RNodeInterface", {}) == "LoRa radio");
    CHECK(ret::suggest_name("AutoInterface", {def}) == "Wi-Fi LAN 2");
    ret::Iface lora;
    lora.name = "LoRa radio";
    ret::Iface lora2;
    lora2.name = "LoRa radio 2";
    CHECK(ret::suggest_name("RNodeInterface", {lora, lora2}) == "LoRa radio 3");
    CHECK(ret::suggest_name("AutoInterface", {}).find('/') == std::string::npos);

    CHECK(ret::kinds_main().size() == 3);
    CHECK(ret::kinds_main()[0]->title == "Wi-Fi / LAN" && ret::kinds_main()[1]->title == "Internet" &&
          ret::kinds_main()[2]->title == "LoRa radio");
    const auto more = ret::kinds_more(cat);
    CHECK(more.size() == 1 && more[0]->type == "SerialInterface"); /* only supported types the bridge lists */
    CHECK(ret::kind_info("RNodeInterface").section == "Radio: you choose" && ret::kind_info("RNodeInterface").note == "Check your local rules");
    CHECK(ret::kind_info("AutoInterface").note.empty() && ret::kind_info("TCPClientInterface").section == "Settings");
    CHECK(ret::kind_info("MyOwnInterface").title == "MyOwn");

    /* option rows: radio fields start empty and read "not set" in amber, no value is ever invented */
    const ret::TypeDef *rn = cat.find("RNodeInterface");
    for (const char *name : {"frequency", "spreadingfactor", "port"}) {
        for (const ret::OptionDef &o : rn->options)
            if (o.name == name) {
                const ret::OptText t = ret::option_text(o, nullptr, false);
                CHECK(t.text == "not set" && t.tone == ret::Tone::Warn);
            }
    }
    const std::string f = "868000000";
    CHECK(ret::option_text(rn->options[1], &f, false).text == "868 MHz");
    const std::string sf = "7";
    CHECK(ret::option_text(rn->options[2], &sf, false).text == "7");
    const std::string yes = "True";
    CHECK(ret::option_text(rn->options[3], &yes, false).text == "yes");
    CHECK(ret::option_text(rn->options[3], nullptr, false).text == "default (no)");
    const std::string port = "/dev/serial/by-id/usb-X-if00";
    CHECK(ret::option_text(rn->options[0], &port, false).text == "usb-X-if00");
    CHECK(ret::option_text(cat.common[1], nullptr, false).text == "not set" && ret::option_text(cat.common[1], nullptr, false).tone == ret::Tone::Muted);
    CHECK(ret::option_text(cat.common[1], nullptr, true).text == "set");
    CHECK(ret::option_text(cat.common[2], nullptr, false).text == "default (2 %)");
    CHECK(ret::option_edit_text(cat.common[1], &port).empty()); /* a secret is never put back into a field */
    CHECK(ret::format_hz(125000) == "125 kHz" && ret::format_hz(433920000) == "433.92 MHz" && ret::format_hz(50) == "50 Hz");
    CHECK(ret::bool_text("Yes") == "yes" && ret::bool_text("false") == "no" && ret::bool_text("x").empty());
    CHECK(ret::option_hint(rn->options[2]) == "A whole number from 5 to 12.");
    CHECK(ret::option_hint(cat.common[2]).find("(%)") != std::string::npos);
    CHECK(ret::option_label("spreadingfactor") == "Spreading factor" && ret::option_label("some_new_thing") == "Some new thing");

    /* the bridge's errors, in plain words */
    ret::Response r;
    r.transport = true;
    r.code = "invalid";
    r.field = "frequency";
    r.message = "frequency: at least 137000000";
    CHECK(ret::plain_error(r) == "Frequency: at least 137000000");
    r.code = "busy";
    r.field.clear();
    r.message = "/dev/ttyACM0 is used by mesh-hop. Close that program first.";
    CHECK(ret::plain_error(r) == r.message);
    r.code = "incomplete";
    r.error = J(R"({"code":"incomplete","missing":["frequency","spreadingfactor"]})");
    CHECK(ret::plain_error(r) == "Not ready to switch on. Still to set: Frequency, Spreading factor.");
    r.code = "not_found";
    r.message = "no interface named 'X'";
    CHECK(ret::plain_error(r) == "no connection named 'X'");
    r.code = "weird";
    r.message.clear();
    CHECK(ret::plain_error(r) == "Something went wrong (weird).");
    r.transport = false;
    r.code = "no_service";
    CHECK(ret::plain_error(r) == "The service is not running.");
    r.code = "no_answer";
    CHECK(ret::plain_error(r).find("slow") != std::string::npos);
    CHECK(ret::plainify("Interfaces and an interface") == "Connections and a connection");

    CHECK(ret::short_address("329a1f002b4c7d9e5566778899aabc09") == "329a...bc09");
    CHECK(ret::short_address("abc") == "abc");
    CHECK(ret::grouped_address("329a1f002b4c7d9e5566778899aabc09") == "329a1f00 2b4c7d9e 55667788 99aabc09");
    CHECK(ret::ago_text(5) == "just now" && ret::ago_text(120) == "2 min ago" && ret::ago_text(7200) == "2 h ago" &&
          ret::ago_text(86400 * 3) == "3 days ago" && ret::ago_text(-4) == "just now");
    CHECK(ret::trim_text("  a b \n") == "a b" && ret::trim_text("   ").empty());
}

static void test_model_misc()
{
    ret::Nav nav;
    CHECK(nav.current() == ret::View::Home && nav.depth() == 0);
    CHECK(nav.back() == ret::Back::Quit);
    nav.open(ret::View::Connections);
    nav.open(ret::View::Connection);
    CHECK(nav.current() == ret::View::Connection && nav.has(ret::View::Connections) && nav.depth() == 2);
    CHECK(nav.back() == ret::Back::Closed && nav.current() == ret::View::Connections);
    CHECK(nav.back() == ret::Back::Closed && nav.back() == ret::Back::Quit);
    nav.open(ret::View::Settings);
    nav.reset();
    CHECK(nav.current() == ret::View::Home);

    CHECK(ret::run_dir(nullptr, "/run/user/1000", "640x480") == "/run/user/1000/reticulum");
    CHECK(ret::run_dir("/tmp/x/{canvas}", "/run/user/1000", "640x480") == "/tmp/x/640x480");
    CHECK(ret::run_dir("", "", "").empty() && ret::run_dir(nullptr, nullptr, nullptr).empty());
    CHECK(ret::systemctl_argv(nullptr, {"start", "reticulum"}) ==
          std::vector<std::string>({"systemctl", "--user", "start", "reticulum"}));
    CHECK(ret::systemctl_argv("/t/fake", {"stop"})[0] == "/t/fake");
    CHECK(ret::unit_is_enabled("enabled\n") && !ret::unit_is_enabled("disabled\n") && !ret::unit_is_enabled("") &&
          !ret::unit_is_enabled("static"));

    /* serial ports: by-id entries first (with the device they point to), then the plain ones nothing points to */
    char tmpl[] = "/tmp/ret-dev-XXXXXX";
    const std::string root = mkdtemp(tmpl);
    mkdir((root + "/serial").c_str(), 0755);
    mkdir((root + "/serial/by-id").c_str(), 0755);
    for (const char *n : {"ttyACM0", "ttyACM1", "ttyUSB0", "tty0", "null"}) close(open((root + "/" + n).c_str(), O_CREAT | O_WRONLY, 0644));
    CHECK(symlink((root + "/ttyACM0").c_str(), (root + "/serial/by-id/usb-Test_RNode_1-if00").c_str()) == 0);
    const auto ports = ret::list_ports(root);
    CHECK(ports.size() == 3);
    CHECK(ports.size() == 3 && ports[0].path == root + "/serial/by-id/usb-Test_RNode_1-if00" &&
          ports[0].label == "Test_RNode_1-if00 (ttyACM0)");
    CHECK(ports.size() == 3 && ports[1].label == "ttyACM1" && ports[2].label == "ttyUSB0");
    CHECK(ret::list_ports(root + "/nothing").empty());
    unlink((root + "/serial/by-id/usb-Test_RNode_1-if00").c_str());
    for (const char *n : {"ttyACM0", "ttyACM1", "ttyUSB0", "tty0", "null"}) unlink((root + "/" + n).c_str());
    rmdir((root + "/serial/by-id").c_str());
    rmdir((root + "/serial").c_str());
    rmdir(root.c_str());
}

/* Every value the user can see or pick goes through value_label: the bridge's own list of choices (options.py, read here)
 * must all have words of their own in the table, never the raw term or its humanised fallback. */
static void test_value_words()
{
    CHECK(ret::value_label("mode", "full") == "Normal");
    CHECK(ret::value_label("mode", "access_point") == "Access point");
    CHECK(ret::value_label("parity", "odd") == "Odd");
    CHECK(ret::value_label("mode", "some_new_mode") == "Some new mode"); /* an unknown value is humanised, never raw */
    CHECK(ret::value_label("nothing", "x_y") == "X y");
    CHECK(ret::plain_systemctl("Failed to start reticulum.service: Unit reticulum.service is masked.") ==
          "The service is switched off by the system.");
    CHECK(ret::plain_systemctl("Unit reticulum.service not found.") == "The service is not installed.");
    CHECK(ret::plain_systemctl("Failed to enable unit: Access denied") == "The system did not allow it.");
    CHECK(ret::plain_systemctl("something odd") == "something odd");

    ret::Catalog cat;
    ret::parse_catalog(J(kTypes), cat);
    const std::string full = "full", ap = "access_point";
    CHECK(ret::option_text(cat.common[0], &full, false).text == "Normal");
    CHECK(ret::option_text(cat.common[0], &ap, false).text == "Access point");
    CHECK(ret::option_text(cat.common[0], nullptr, false).text == "default (Normal)");
    for (const char *name : {"target_host", "network_name", "id_callsign", "port"}) {
        ret::OptionDef d;
        d.name = name;
        d.kind = ret::OptKind::Str;
        CHECK(ret::option_hint(d) != "Text.");
    }

    const std::string src = read_file(std::string(CDK_APP_DIR) + "/bridge/reticulum_bridge/options.py");
    int checked = 0;
    for (std::size_t at = src.find("_o(\""); at != std::string::npos; at = src.find("_o(\"", at + 1)) {
        const std::size_t name_end = src.find('"', at + 4);
        const std::size_t next = src.find("_o(\"", at + 1);
        const std::size_t ch = src.find("choices=[", at);
        if (name_end == std::string::npos || ch == std::string::npos || (next != std::string::npos && ch > next)) continue;
        const std::string name = src.substr(at + 4, name_end - at - 4);
        const std::size_t close = src.find(']', ch);
        const std::string list = src.substr(ch + 9, close - ch - 9);
        for (std::size_t q = list.find('"'); q != std::string::npos; q = list.find('"', list.find('"', q + 1) + 1)) {
            const std::size_t e = list.find('"', q + 1);
            if (e == std::string::npos) break;
            const std::string value = list.substr(q + 1, e - q - 1);
            const std::string label = ret::value_label(name, value);
            ++checked;
            CHECK(label.find('_') == std::string::npos);
            CHECK(label != value); /* a table entry, or at least a capital letter */
            if (label == value) std::fprintf(stderr, "option %s value %s is shown raw\n", name.c_str(), value.c_str());
        }
    }
    CHECK(checked >= 14); /* mode 6, discovery_scope 5, multicast 2, parity 3 */
}

/* ------------------------------------------------------------------ the sources and the package */

static std::vector<std::string> list_files(const std::string &dir, const std::string &suffix)
{
    std::vector<std::string> out;
    if (DIR *d = opendir(dir.c_str())) {
        while (const dirent *e = readdir(d)) {
            const std::string n = e->d_name;
            if (n.size() > suffix.size() && n.compare(n.size() - suffix.size(), suffix.size(), suffix) == 0) out.push_back(dir + "/" + n);
        }
        closedir(d);
    }
    return out;
}

static std::string lower(std::string s)
{
    for (char &c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

/* The words of the network's own vocabulary must not reach a screen: no quoted string of the UI sources (or of model.cpp,
 * which words the states and options) holds one, unless the line is marked `vocab-ok` (a protocol word, never shown). */
static void test_vocabulary()
{
    const std::string app = CDK_APP_DIR;
    std::vector<std::string> files = list_files(app + "/src/ui", ".cpp");
    files.push_back(app + "/src/core/model.cpp");
    const char *banned[] = {"interface", "announce", "destination", "lxmf", "transport", "rns", "propagation", "identity hash"};
    int scanned = 0;
    for (const std::string &file : files) {
        std::istringstream in(read_file(file));
        std::string line;
        int n = 0;
        while (std::getline(in, line)) {
            ++n;
            ++scanned;
            if (line.find("vocab-ok") != std::string::npos) continue;
            std::size_t pos = 0;
            while ((pos = line.find('"', pos)) != std::string::npos) {
                std::string lit;
                std::size_t i = pos + 1;
                for (; i < line.size() && line[i] != '"'; ++i) {
                    if (line[i] == '\\' && i + 1 < line.size()) ++i;
                    lit += line[i];
                }
                pos = i + 1;
                const std::string l = lower(lit);
                for (const char *w : banned) {
                    for (std::size_t at = l.find(w); at != std::string::npos; at = l.find(w, at + 1)) {
                        const bool left_ok = at == 0 || !std::isalpha(static_cast<unsigned char>(l[at - 1]));
                        if (left_ok) {
                            ++g_fail;
                            std::fprintf(stderr, "%s:%d: the word \"%s\" in a string of the UI: %s\n", file.c_str(), n, w, lit.c_str());
                        }
                    }
                }
            }
        }
    }
    CHECK(scanned > 500);
}

/* The only request the UI sends that lets the deck transmit is the confirmed "Allow sending": no `announce` request, no
 * message send, and `user_action` set to true in exactly one place. */
static void test_transmit_sources()
{
    const std::string app = CDK_APP_DIR;
    int user_action_true = 0;
    for (const std::string &file : list_files(app + "/src/ui", ".cpp")) {
        const std::string text = read_file(file);
        CHECK(text.find("request(\"announce\"") == std::string::npos);
        CHECK(text.find("request(\"lxmf.send\"") == std::string::npos);
        for (std::size_t at = text.find("user_action=*/true"); at != std::string::npos; at = text.find("user_action=*/true", at + 1)) ++user_action_true;
        /* every request of iface.outgoing that is not the confirmed one carries false */
        std::size_t at = 0;
        while ((at = text.find("\"iface.outgoing\"", at)) != std::string::npos) at += 5;
    }
    CHECK(user_action_true == 1);
    const std::string edit = read_file(app + "/src/ui/ret_edit.cpp");
    const std::size_t allow = edit.find("void App::allow_sending()");
    const std::size_t next = edit.find("void App::confirm_remove()");
    CHECK(allow != std::string::npos && next != std::string::npos && edit.substr(allow, next - allow).find("user_action=*/true") != std::string::npos);
    /* allow_sending() is reached only from the dialog's primary button */
    int callers = 0;
    for (const std::string &file : list_files(app + "/src/ui", ".cpp")) {
        const std::string text = read_file(file);
        for (std::size_t at = text.find("allow_sending()"); at != std::string::npos; at = text.find("allow_sending()", at + 1)) ++callers;
    }
    CHECK(callers == 2); /* its definition and the dialog's callback */
    CHECK(edit.find("confirm(\"Allow sending?\"") != std::string::npos);
    /* a new connection is written off and listen-only by the bridge; the app sends no `outgoing` or `enabled` with iface.add */
    const std::size_t save = edit.find("void App::save_draft()");
    const std::size_t form_end = edit.find("/* ------------------------------------------------------------------ editing one option */");
    CHECK(save != std::string::npos && form_end != std::string::npos);
    const std::string body = edit.substr(save, form_end - save);
    CHECK(body.find("\"enabled\"") == std::string::npos && body.find("\"outgoing\"") == std::string::npos);
}

static void test_package_files()
{
    const std::string app = CDK_APP_DIR;
    const std::string desktop = read_file(app + "/root/usr/share/APPLaunch/applications/reticulum.desktop");
    CHECK(!desktop.empty());
    CHECK(desktop.find("\nX-Fullscreen=true") != std::string::npos);
    CHECK(desktop.find("\nX-MinSize=") == std::string::npos); /* AppSpec has no minimum either (app_spec.cpp) */
    CHECK(desktop.find("\nX-Portrait=480x640\n") != std::string::npos); /* equal to AppSpec portrait_w x portrait_h */
    CHECK(desktop.find("\nExec=/usr/share/APPLaunch/bin/reticulum\n") != std::string::npos);
    CHECK(desktop.find("\nIcon=share/images/reticulum.png") != std::string::npos);
    CHECK(desktop.find("\nX-Background=freeze") != std::string::npos);
    const std::string spec = read_file(app + "/src/ui/app_spec.cpp");
    CHECK(spec.find("min_w") == std::string::npos);
    CHECK(spec.find("s.portrait_w = 480;") != std::string::npos && spec.find("s.portrait_h = 640;") != std::string::npos);

    const std::string json = read_file(app + "/app.json");
    Value v = J(json);
    CHECK(v["share_code"].as_string() == "reticulum" && v["package"].as_string() == "reticulum");
    CHECK(v["draft"].as_bool(false)); /* a draft until the board tests */
    CHECK(v["license"].as_string() == "MIT AND BSD-3-Clause AND LicenseRef-Reticulum");
    CHECK(v["title"].as_string() == "Reticulum");
    CHECK(v["summary"].as_string().size() <= 80);
    CHECK(v["summary"].as_string().size() + v["description"].as_string().size() <= 500);
    CHECK(v["arch"].size() == 2);
    CHECK(v["permissions"]["background_service"].as_bool(false));
    const std::string depends = v["depends"].as_string();
    for (const char *d : {"python3", "python3-cryptography", "python3-serial", "libfreetype6", "libinput10", "libxkbcommon0"})
        CHECK(depends.find(d) != std::string::npos);

    struct stat st;
    CHECK(stat((app + "/icon.png").c_str(), &st) == 0);
    CHECK(stat((app + "/root/usr/share/APPLaunch/share/images/reticulum.png").c_str(), &st) == 0);
    CHECK(stat((app + "/root/usr/lib/systemd/user/reticulum.service").c_str(), &st) == 0);
    CHECK(stat((app + "/root/usr/share/doc/reticulum/copyright").c_str(), &st) == 0);
    const std::string unit = read_file(app + "/root/usr/lib/systemd/user/reticulum.service");
    CHECK(unit.find("ExecStart=/usr/bin/python3 -m reticulum_bridge") != std::string::npos);
    CHECK(unit.find("\n[Install]") != std::string::npos && unit.find("WantedBy=default.target") != std::string::npos);
}

int main()
{
    test_json();
    test_link_pure();
    test_link_no_service();
    test_link_server();
    test_model_parse();
    test_model_words();
    test_model_misc();
    test_value_words();
    test_vocabulary();
    test_transmit_sources();
    test_package_files();
    std::printf("reticulum core: %d checks passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
