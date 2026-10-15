/*
 * SPDX-License-Identifier: MIT
 *
 * Net Toolbox: whois, the app's own TCP port 43 client (no LVGL; tests/test_whois.cpp runs it against fake whois
 * servers on loopback and parses answers recorded from the registries' documented formats).
 *
 * The query (a domain, an IPv4 or an IPv6 address) goes first to whois.iana.org, whose "refer:" (or "whois:") line
 * names the server in charge: a TLD's registry for a domain, the regional registry for an address (ARIN, RIPE NCC,
 * APNIC, LACNIC, AFRINIC). That server's answer may name another one ("Registrar WHOIS Server:" of a registry,
 * "ReferralServer: whois://..." of ARIN), which is asked in turn: at most kMaxReferrals referrals, never a server
 * twice for the same object, kMaxAnswerBytes per answer, kTotalMs for the whole run. whois.arin.net gets
 * "n + <address>" (the network, in full), every other server the object as typed. When a registry finds no domain
 * "www.example.com" it is asked once more for the parent ("example.com"), at most twice.
 *
 * The result keeps the raw text (cleaned: control characters dropped, bytes that are not UTF-8 shown as '?', lines cut
 * at kMaxLineBytes, kMaxLines lines in all) and a short summary of the most specific answer (IANA's own answer only when
 * no other came). A field the answers do not have stays empty: nothing is guessed.
 */
#pragma once

#include "dns_client.hpp"
#include "worker.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace nt {

constexpr int kWhoisMaxReferrals = 3;
constexpr std::size_t kWhoisMaxAnswerBytes = 64 * 1024;
constexpr std::size_t kWhoisMaxLines = 400;
constexpr std::size_t kWhoisMaxLineBytes = 300;
constexpr int kWhoisTotalMs = 10000;
constexpr const char *kWhoisIana = "whois.iana.org";

/* A domain, an IPv4 or an IPv6 address: trimmed, a domain in lower case without its trailing dot. */
bool parse_whois_query(const std::string &input, std::string &object, bool &is_ip, std::string *why = nullptr);

/* What to send to that server for that object: "n + 8.8.8.8" to whois.arin.net, the object itself elsewhere. */
std::string whois_query_text(const std::string &server, const std::string &object, bool is_ip);

/* The next server named by an answer ("" when none): refer:, whois:, ReferralServer: whois://..., Registrar WHOIS
 * Server:, Whois Server:. Not the current server, never rwhois:// or a web address; a valid host name. */
std::string whois_referral(const std::vector<std::string> &lines, const std::string &current_server);

/* The answer's lines, cleaned (see the header); stops after max_lines (*truncated). */
std::vector<std::string> whois_clean_lines(const std::string &raw, std::size_t max_lines, bool *truncated = nullptr);

/* "No match for", "NOT FOUND", "No Data Found", "No entries found", ... */
bool whois_not_found(const std::vector<std::string> &lines);

struct WhoisSummary {
    /* a domain */
    std::string registrar, created, updated, expires;
    std::vector<std::string> status;        /* EPP codes: "clientTransferProhibited", ... */
    std::vector<std::string> name_servers;  /* lower case */
    /* an address */
    std::string netname, organisation, country, cidr, as_number, abuse;
    bool empty() const;
};

/* One answer's fields (pure). Domain fields take their first value; network fields their last (ARIN lists the
 * wider network first, the narrower one last). */
WhoisSummary parse_whois_summary(const std::vector<std::string> &lines, bool is_ip);

struct WhoisAnswer {
    std::string server;
    std::string query;           /* what was sent */
    bool ok = false;             /* an answer came (possibly cut by the deadline) */
    std::string error;           /* plain words when nothing came */
    bool truncated = false;      /* more than kWhoisMaxAnswerBytes came */
    std::size_t bytes = 0;
    std::vector<std::string> lines;     /* cleaned; the result's line budget may cut them */
    WhoisSummary summary;               /* of the whole answer, before any cut */
};

struct WhoisResult {
    std::string object;
    bool is_ip = false;
    std::vector<WhoisAnswer> answers;   /* in the order asked */
    WhoisSummary summary;
    bool lines_truncated = false;       /* kWhoisMaxLines reached */
    std::string note;                   /* "referral loop stopped", "referral limit reached", "time limit reached" */
    std::size_t line_count() const;
};

/* The summary of a whole run (pure): the most specific answer first, IANA only when nothing else answered. */
WhoisSummary whois_summary_of(const WhoisResult &r);

struct WhoisRequest {
    std::string query;                  /* as typed */
    ResolveOptions resolve;             /* the DNS server for the whois servers' names (and the hosts file) */
    std::uint16_t port = 43;            /* tests: a loopback port */
    int total_ms = kWhoisTotalMs;
    std::string first_server = kWhoisIana;
};

/* One question to one server (a worker thread): connect, send "query\r\n", read until it closes, kWhoisMaxAnswerBytes
 * or the deadline. */
WhoisAnswer whois_ask(const std::string &server, const std::string &query, const WhoisRequest &req,
                      std::int64_t deadline_ms, const CancelToken &cancel);

/* The whole run (a worker thread); publish after each answer. */
WhoisResult run_whois(const WhoisRequest &req, const CancelToken &cancel,
                      const std::function<void(const WhoisResult &, const std::string &asking)> &publish = nullptr);

struct WhoisSnapshot {
    enum class State { Idle, Running, Done, Failed, Cancelled };
    State state = State::Idle;
    std::string input;
    std::string asking;                 /* Running: the server being asked */
    std::string error;                  /* Failed: a bad query */
    WhoisResult result;
    bool running() const { return state == State::Running; }
};

class WhoisSession {
public:
    bool start(const WhoisRequest &req); /* LVGL thread; false (a Failed snapshot) for a bad query */
    void stop();
    bool running() const { return worker_.running(); }
    std::uint64_t seq() const { return channel_.seq(); }
    std::shared_ptr<const WhoisSnapshot> latest() const { return channel_.latest(); }
    bool wait(int timeout_ms) const { return worker_.wait(timeout_ms); } /* tests */

private:
    Channel<WhoisSnapshot> channel_;
    Worker worker_;
};

} // namespace nt
