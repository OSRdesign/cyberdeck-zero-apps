/*
 * SPDX-License-Identifier: MIT
 *
 * Contacts and messages (phase P2): what the app knows of the bridge's contact, heard and message verbs
 * (docs/dev/reticulum-bridge.md section 10) and how it says it in plain words. No LVGL, nothing here sends anything
 * (tests/test_core.cpp).
 *
 * Words: the screens say "address", "connection", "contact" and "heard recently"; a contact without a name is shown as
 * "Anonymous" with the short form of its address. Message states are the owner's list: Sending..., Delivered HH:MM,
 * Sent, not confirmed by contact, Failed, contact may be offline (manual Retry). There is no read state.
 */
#pragma once

#include "json.hpp"
#include "model.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace ret {

/* ------------------------------------------------------------------ the bridge's limits */

struct Limits {
    int single = 287;      /* bytes of text that fit one packet (messages.limits single_packet_bytes) */
    int link = 319;        /* what a link packet holds (not used by the screens) */
    int max_text = 8192;   /* the bridge's own cap; a longer text is refused */
    bool loaded = false;
};
bool parse_limits(const json::Value &result, Limits &out);

/* ------------------------------------------------------------------ contacts */

struct Contact {
    std::string address;     /* 32 hex digits */
    std::string name;        /* "" = none */
    std::string heard_name;  /* the name the contact announced, "" = none */
    double added = 0;
    double last_heard = 0;   /* 0 = never heard */
    int unread = 0;
    /* The name to show: the contact's own, else the heard one, else "Anonymous". */
    std::string display() const;
    bool anonymous() const { return name.empty() && heard_name.empty(); }
};
bool parse_contacts(const json::Value &result, std::vector<Contact> &out);
/* A contact row's value: "2 new" (warn), "heard 5 min ago" (ok), "not heard yet" (muted). */
State contact_state(const Contact &c, double now);
/* The line under a contact page's title: "heard 5 min ago on LoRa radio" / "not heard yet" (iface may be empty). */
std::string contact_heard_line(const Contact &c, const std::string &iface, double now);

/* ------------------------------------------------------------------ the heard list (heard.list) */

struct Person {
    std::string address, name, iface;
    double time = 0;
    int hops = -1;
    bool contact = false;
    std::string display() const { return name.empty() ? "Anonymous" : name; }
};
bool parse_people(const json::Value &result, std::vector<Person> &out);
bool parse_person(const json::Value &data, Person &out); /* one `heard` event's data */
/* Replaces or inserts one entry, newest first, at most 500. */
void merge_person(std::vector<Person> &list, const Person &p);
/* The second line of a heard row: "Heard on LoRa radio" / "7be1...04aa, heard on Wi-Fi" / "Already in your contacts". */
std::string person_line(const Person &p);

/* ------------------------------------------------------------------ messages */

struct Message {
    std::int64_t id = 0;
    std::string address, dir, text, state, detail; /* dir: "in" / "out"; state: sending sent delivered failed received */
    double time = 0;
    bool unread = false;
    int bytes = 0;
    bool out() const { return dir == "out"; }
};
bool parse_message(const json::Value &m, Message &out);
bool parse_history(const json::Value &result, std::vector<Message> &out, bool *has_more);
/* Puts a message in the list by id (a newer state replaces the old one); false when it is already the same. */
bool merge_message(std::vector<Message> &list, const Message &m);
/* A `message_state` event; false when the message is not in the list. */
bool apply_state(std::vector<Message> &list, std::int64_t id, const std::string &state, const std::string &detail);

struct MsgStatus {
    std::string text; /* "" for a message that was received */
    Tone tone = Tone::Muted;
    bool retry = false; /* the Retry button belongs to it */
};
MsgStatus message_status(const Message &m);
/* Why a message of yours failed, in the words both the conversation and the Messages list use (one place, so they
 * cannot disagree): detail is the service's: "no_transmit", "no_path" or anything else. */
std::string failure_text(const std::string &detail);
/* The same reason for a list row, which has room for about 25 characters at 640 px: only no_transmit differs (the full
 * words, "...no connection is allowed to send", are cut by the dots there). */
std::string failure_brief(const std::string &detail);

struct Conversation {
    std::string address, name, preview, last_dir, last_state, last_detail;
    bool contact = false;
    int count = 0, unread = 0;
    double last_time = 0;
    std::int64_t last_id = 0;
    std::string display() const { return name.empty() ? "Anonymous" : name; }
};
bool parse_conversations(const json::Value &result, std::vector<Conversation> &out, int *unread_total);
/* The second line of a Messages row: the last text ("You: ..." for one you sent) or, for a failed one, the failure. */
struct ConvLine {
    std::string text;
    Tone tone = Tone::Normal;
};
ConvLine conversation_line(const Conversation &c);

/* ------------------------------------------------------------------ text, bytes and the counter */

/* The bytes of a text as the bridge counts them (UTF-8). */
int text_bytes(const std::string &utf8);
/* The longest prefix of text (whole characters) that fits `max_bytes`. */
std::string clip_utf8(const std::string &text, std::size_t max_bytes);

enum class Level { Normal, Near, Over };
struct Counter {
    std::string text;        /* "287 left", "12 left", "23 over" */
    Level level = Level::Normal;
    bool slow = false;       /* past one packet: the amber warning shows (Send stays enabled) */
    bool too_long = false;   /* past the bridge's cap: Send is refused */
    bool empty = true;       /* nothing to send */
};
/* The counter of the compose row: `bytes` of text against the limits. Near = 30 bytes or fewer left. */
Counter counter_for(int bytes, const Limits &limits);
constexpr const char *kSlowWarning = "Long messages are slow over radio";

/* ------------------------------------------------------------------ addresses */

/* The address as typed or pasted: blanks, tabs, line breaks and angle brackets dropped, letters in lower case.
 * `valid` is true for exactly 32 hexadecimal digits. */
std::string normalize_address(const std::string &raw, bool *valid = nullptr);
/* How many hexadecimal digits the text holds so far (for "17 of 32 characters so far"). */
int address_digits(const std::string &raw);
/* "329a 4f1c ..." in groups of four, as the My address page shows it. */
std::string spaced_address(const std::string &hex);

/* ------------------------------------------------------------------ time */

/* "12:34" in local time. */
std::string clock_text(double t);
/* The time of a conversation row: the clock for today, a weekday name within the last six days, else "12 Nov". */
std::string list_time(double now, double t);

/* ------------------------------------------------------------------ Home and Settings words */

std::string unread_text(int unread);                     /* "2 new" or "" */
std::string contacts_summary(int count);                 /* "3" or "none yet" */
std::string heard_hour_summary(int heard_count);         /* "heard 4 this hour" / "nobody heard yet" */
/* The Connections row's value on Home: "listen only" or "sending" for one connection that is on, "N on", "all off". */
std::string connections_value(const std::vector<Iface> &ifaces);
/* No connection may send now (none is on, online and allowed to send): what the blocked pages test. */
bool nothing_may_send(const std::vector<Iface> &ifaces);
/* The names of the connections that may send, joined with ", " (for the announce confirmation). */
std::string sending_names(const std::vector<Iface> &ifaces);

} // namespace ret
