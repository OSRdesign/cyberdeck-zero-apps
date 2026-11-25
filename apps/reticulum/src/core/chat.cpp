/*
 * SPDX-License-Identifier: MIT
 *
 * Contacts and messages in plain words (see chat.hpp).
 */
#include "chat.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <ctime>

namespace ret {

namespace {

std::string iface_words(const std::string &name)
{
    if (name == "Default Interface") return "Wi-Fi LAN";  // vocab-ok: the bridge's own word, never shown
    return name;
}

struct tm local_tm(double t)
{
    const std::time_t tt = static_cast<std::time_t>(t);
    struct tm out {};
    localtime_r(&tt, &out);
    return out;
}

} // namespace

/* ------------------------------------------------------------------ limits */

bool parse_limits(const json::Value &r, Limits &out)
{
    if (!r.is_object() || !r["single_packet_bytes"].is_number()) return false;
    Limits l;
    l.single = static_cast<int>(r["single_packet_bytes"].as_int(287));
    l.link = static_cast<int>(r["link_packet_bytes"].as_int(319));
    l.max_text = static_cast<int>(r["max_text_bytes"].as_int(8192));
    if (l.single < 1) l.single = 287;
    if (l.max_text < l.single) l.max_text = 8192;
    l.loaded = true;
    out = l;
    return true;
}

/* ------------------------------------------------------------------ contacts */

std::string Contact::display() const
{
    if (!name.empty()) return name;
    if (!heard_name.empty()) return heard_name;
    return "Anonymous";
}

bool parse_contacts(const json::Value &result, std::vector<Contact> &out)
{
    if (!result.is_object() || !result["contacts"].is_array()) return false;
    std::vector<Contact> list;
    for (const json::Value &c : result["contacts"].items()) {
        if (!c["address"].is_string()) continue;
        Contact k;
        k.address = c["address"].as_string();
        k.name = c["name"].as_string();
        k.heard_name = c["heard_name"].as_string();
        k.added = c["added"].as_number(0);
        k.last_heard = c["last_heard"].as_number(0);
        k.unread = static_cast<int>(c["unread"].as_int(0));
        list.push_back(std::move(k));
    }
    out = std::move(list);
    return true;
}

State contact_state(const Contact &c, double now)
{
    State s;
    if (c.unread > 0) {
        s.word = unread_text(c.unread);
        s.tone = Tone::Warn;
    } else if (c.last_heard > 0) {
        const std::string ago = ago_text(std::max(0.0, now - c.last_heard));
        s.word = ago == "just now" ? "heard just now" : "heard " + ago;
        s.tone = Tone::Ok;
    } else {
        s.word = "not heard yet";
        s.tone = Tone::Muted;
    }
    return s;
}

std::string contact_heard_line(const Contact &c, const std::string &iface, double now)
{
    if (c.last_heard <= 0) return "not heard yet";
    const std::string ago = ago_text(std::max(0.0, now - c.last_heard));
    std::string s = ago == "just now" ? "heard just now" : "heard " + ago;
    if (!iface.empty()) s += " on " + iface_words(iface);
    return s;
}

/* ------------------------------------------------------------------ the heard list */

bool parse_person(const json::Value &d, Person &out)
{
    if (!d.is_object() || !d["address"].is_string()) return false;
    Person p;
    p.address = d["address"].as_string();
    p.name = d["name"].as_string();
    p.iface = d["interface"].as_string();  // vocab-ok: the bridge's own word, never shown
    p.time = d["time"].as_number(0);
    p.hops = d["hops"].is_number() ? static_cast<int>(d["hops"].as_int(-1)) : -1;
    p.contact = d["contact"].as_bool(false);
    out = std::move(p);
    return true;
}

bool parse_people(const json::Value &result, std::vector<Person> &out)
{
    if (!result.is_object() || !result["heard"].is_array()) return false;
    std::vector<Person> list;
    for (const json::Value &h : result["heard"].items()) {
        Person p;
        if (parse_person(h, p)) list.push_back(std::move(p));
    }
    out = std::move(list);
    return true;
}

void merge_person(std::vector<Person> &list, const Person &p)
{
    bool found = false;
    for (Person &e : list)
        if (e.address == p.address) {
            Person n = p;
            if (n.name.empty()) n.name = e.name; /* a later announce without a name keeps the earlier one */
            e = n;
            found = true;
            break;
        }
    if (!found) list.push_back(p);
    std::stable_sort(list.begin(), list.end(), [](const Person &a, const Person &b) { return a.time > b.time; });
    if (list.size() > 500) list.resize(500);
}

std::string person_line(const Person &p)
{
    if (p.contact) return "Already in your contacts";
    const std::string on = p.iface.empty() ? std::string() : "heard on " + iface_words(p.iface);
    if (!p.name.empty()) return on.empty() ? std::string("Heard") : "Heard on " + iface_words(p.iface);
    return on.empty() ? short_address(p.address) : short_address(p.address) + ", " + on;
}

/* ------------------------------------------------------------------ messages */

bool parse_message(const json::Value &m, Message &out)
{
    if (!m.is_object() || !m["id"].is_number()) return false;
    Message k;
    k.id = m["id"].as_int(0);
    k.address = m["address"].as_string();
    k.dir = m["dir"].as_string();
    k.text = m["text"].as_string();
    k.state = m["state"].as_string();
    k.detail = m["detail"].as_string();
    k.time = m["time"].as_number(0);
    k.unread = m["unread"].as_bool(false);
    k.bytes = static_cast<int>(m["bytes"].as_int(static_cast<std::int64_t>(k.text.size())));
    out = std::move(k);
    return true;
}

bool parse_history(const json::Value &result, std::vector<Message> &out, bool *has_more)
{
    if (!result.is_object() || !result["messages"].is_array()) return false;
    std::vector<Message> list;
    for (const json::Value &m : result["messages"].items()) {
        Message k;
        if (parse_message(m, k)) list.push_back(std::move(k));
    }
    out = std::move(list);
    if (has_more) *has_more = result["has_more"].as_bool(false);
    return true;
}

bool merge_message(std::vector<Message> &list, const Message &m)
{
    for (Message &e : list)
        if (e.id == m.id) {
            if (e.state == m.state && e.detail == m.detail && e.text == m.text && e.unread == m.unread) return false;
            e = m;
            return true;
        }
    list.push_back(m);
    std::stable_sort(list.begin(), list.end(), [](const Message &a, const Message &b) { return a.id < b.id; });
    return true;
}

bool apply_state(std::vector<Message> &list, std::int64_t id, const std::string &state, const std::string &detail)
{
    for (Message &e : list)
        if (e.id == id) {
            e.state = state;
            e.detail = state == "failed" ? detail : std::string();
            return true;
        }
    return false;
}

MsgStatus message_status(const Message &m)
{
    MsgStatus s;
    if (!m.out()) return s;
    if (m.state == "sending") {
        s.text = "Sending...";
        s.tone = Tone::Muted;
    } else if (m.state == "sent") {
        s.text = "Sent, not confirmed by contact";
        s.tone = Tone::Muted;
    } else if (m.state == "delivered") {
        s.text = "Delivered " + clock_text(m.time);
        s.tone = Tone::Ok;
    } else if (m.state == "failed") {
        s.text = failure_text(m.detail);
        s.tone = Tone::Bad;
        s.retry = true;
    }
    return s;
}

std::string failure_text(const std::string &detail)
{
    if (detail == "no_transmit") return "Failed, no connection is allowed to send";
    if (detail == "no_path") return "Failed, no way to reach the contact yet";
    return "Failed, contact may be offline";
}

std::string failure_brief(const std::string &detail)
{
    if (detail == "no_transmit") return "Failed, sending was off";
    return failure_text(detail);
}

bool parse_conversations(const json::Value &result, std::vector<Conversation> &out, int *unread_total)
{
    if (!result.is_object() || !result["conversations"].is_array()) return false;
    std::vector<Conversation> list;
    for (const json::Value &c : result["conversations"].items()) {
        if (!c["address"].is_string()) continue;
        Conversation k;
        k.address = c["address"].as_string();
        k.name = c["name"].as_string();
        k.preview = c["preview"].as_string();
        k.last_dir = c["last_dir"].as_string();
        k.last_state = c["last_state"].as_string();
        k.last_detail = c["last_detail"].as_string();
        k.contact = c["contact"].as_bool(false);
        k.count = static_cast<int>(c["count"].as_int(0));
        k.unread = static_cast<int>(c["unread"].as_int(0));
        k.last_time = c["last_time"].as_number(0);
        k.last_id = c["last_id"].as_int(0);
        list.push_back(std::move(k));
    }
    out = std::move(list);
    if (unread_total) *unread_total = static_cast<int>(result["unread_total"].as_int(0));
    return true;
}

ConvLine conversation_line(const Conversation &c)
{
    ConvLine l;
    if (c.last_dir == "out" && c.last_state == "failed") {
        l.text = failure_brief(c.last_detail);
        l.tone = Tone::Bad;
        return l;
    }
    l.text = (c.last_dir == "out" ? "You: " : "") + c.preview;
    l.tone = c.unread > 0 ? Tone::Normal : Tone::Muted;
    return l;
}

/* ------------------------------------------------------------------ text */

int text_bytes(const std::string &utf8)
{
    return static_cast<int>(utf8.size());
}

std::string clip_utf8(const std::string &text, std::size_t max_bytes)
{
    if (text.size() <= max_bytes) return text;
    std::size_t n = max_bytes;
    while (n > 0 && (static_cast<unsigned char>(text[n]) & 0xC0) == 0x80) --n; /* never inside a character */
    return text.substr(0, n);
}

Counter counter_for(int bytes, const Limits &limits)
{
    Counter c;
    c.empty = bytes <= 0;
    c.too_long = bytes > limits.max_text;
    const int left = limits.single - bytes;
    if (left >= 0) {
        c.text = std::to_string(left) + " left";
        c.level = left <= 30 ? Level::Near : Level::Normal;
    } else {
        c.text = std::to_string(-left) + " over";
        c.level = Level::Over;
        c.slow = true;
    }
    return c;
}

/* ------------------------------------------------------------------ addresses */

std::string normalize_address(const std::string &raw, bool *valid)
{
    std::string out;
    for (const char ch : raw) {
        const unsigned char c = static_cast<unsigned char>(ch);
        if (std::isspace(c) || c == '<' || c == '>') continue;
        out += static_cast<char>(std::tolower(c));
    }
    if (valid) {
        bool ok = out.size() == 32;
        for (const char c : out)
            if (!std::isxdigit(static_cast<unsigned char>(c))) ok = false;
        *valid = ok;
    }
    return out;
}

int address_digits(const std::string &raw)
{
    int n = 0;
    for (const char c : raw)
        if (std::isxdigit(static_cast<unsigned char>(c))) ++n;
    return n;
}

std::string spaced_address(const std::string &hex)
{
    std::string out;
    for (std::size_t i = 0; i < hex.size(); i += 4) {
        if (!out.empty()) out += ' ';
        out += hex.substr(i, 4);
    }
    return out;
}

/* ------------------------------------------------------------------ time */

std::string clock_text(double t)
{
    const struct tm tm = local_tm(t);
    char buf[16];
    std::snprintf(buf, sizeof buf, "%02d:%02d", tm.tm_hour, tm.tm_min);
    return buf;
}

std::string list_time(double now, double t)
{
    const struct tm a = local_tm(now), b = local_tm(t);
    if (a.tm_year == b.tm_year && a.tm_yday == b.tm_yday) return clock_text(t);
    static const char *days[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
    static const char *months[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    if (now >= t && now - t < 6 * 86400.0) return days[b.tm_wday % 7];
    char buf[24];
    std::snprintf(buf, sizeof buf, "%d %s", b.tm_mday, months[b.tm_mon % 12]);
    return buf;
}

/* ------------------------------------------------------------------ Home and Settings words */

std::string unread_text(int unread)
{
    return unread > 0 ? std::to_string(unread) + " new" : std::string();
}

std::string contacts_summary(int count)
{
    return count > 0 ? std::to_string(count) : "none yet";
}

std::string heard_hour_summary(int heard_count)
{
    if (heard_count <= 0) return "nobody heard yet";
    return "heard " + std::to_string(heard_count) + " this hour";
}

std::string connections_value(const std::vector<Iface> &ifaces)
{
    const int n = count_on(ifaces);
    if (n == 0) return "all off";
    if (n == 1)
        for (const Iface &i : ifaces)
            if (i.enabled) return i.outgoing ? "sending" : "listen only"; /* the name is on the Connections page */
    return std::to_string(n) + " on";
}

namespace {
bool may_send(const Iface &i)
{
    return i.supported && i.enabled && i.online && i.outgoing && i.error.empty();
}
} // namespace

bool nothing_may_send(const std::vector<Iface> &ifaces)
{
    for (const Iface &i : ifaces)
        if (may_send(i)) return false;
    return true;
}

std::string sending_names(const std::vector<Iface> &ifaces)
{
    std::string s;
    for (const Iface &i : ifaces)
        if (may_send(i)) {
            if (!s.empty()) s += ", ";
            s += conn_title(i);
        }
    return s;
}

} // namespace ret
