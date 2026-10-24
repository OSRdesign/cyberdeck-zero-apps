// Report 033 (Mesh Hop CLI), phase P5: the CLI page without LVGL (ui/cli_logic.*): the output and the password rule, the history,
// the typed line's bytes, inserting a command, the endings, the Help list; the copy of Serial Console's TermBuffer (core/term.*)
// on what meshcore-cli writes. Included by test_ui.cpp. MESHHOP_CLI_PY (run_tests.sh): the bundled meshcore_cli.py, whose prompt
// texts and shortcuts the page relies on.

#include "cli_logic.hpp"

#include <fstream>
#include <sstream>

static void test_cli_term_copy()
{
    using namespace meshzero::term;
    TermBuffer b(3, 10);
    b.feed("ab\x1b[31mc\x1b[0m\r\nline two is long\r\n\x07");          // colours dropped, a long line cut at 10, a bell counted
    CHECK(b.line(b.first()) == "abc" && b.line(b.first() + 1) == "line two i" && b.line(b.first() + 2) == "s long");
    CHECK(b.bells() == 1 && b.line(b.end() - 1).empty());
    b.feed("x\r\ny\r\nz\r\n");                                          // only the newest 3 finished lines are kept
    CHECK(b.first() > 0 && b.line(b.first()) == "x");
    b.feed("abc\bX");                                                   // BS moves back, the next text overwrites
    CHECK(b.line(b.end() - 1) == "abX");
    ViewPos top;
    const std::vector<std::string> v = view_bottom(b, 4, 2, &top);
    CHECK(v.size() == 2 && v[0] == "z" && v[1] == "abX");
    CHECK(scroll_view(b, 4, 2, &top, -1) == false && scroll_view(b, 4, 2, &top, 5) == true);
    CHECK(wrap_line("abcdefgh", 3) == std::vector<std::string>({"abc", "def", "gh"}) && utf8_count("\xC3\xA9t\xC3\xA9") == 3);
}

static void test_cli_screen()
{
    CliScreen s;
    // meshcore-cli's start and its prompt in dumb mode (-c off: no colour), then a command and its echo
    s.feed("INFO:meshcore:Connected to SimNode running on a v1.15.0 (11) fw.\r\nInteractive mode, most commands from terminal chat should work.\r\n");
    CHECK(!s.at_prompt() && !s.password_prompt());
    s.feed("SimNode> ");
    CHECK(s.at_prompt() && !s.password_prompt() && s.current_line() == "SimNode> ");
    s.sent_line(false);
    s.feed("i");
    s.feed("nfos\r\n{\r\n    \"name\": \"SimNode\"\r\n}\r\nSimNode> ");
    CHECK(s.text().find("SimNode> infos\n{\n    \"name\": \"SimNode\"\n}") != std::string::npos);
    // to a repeater, login: the password prompt, the typed password never shown (its echo dropped up to the line end)
    s.feed("to Hilltop repeater\r\nSimNode/Hilltop repeater|0> login\r\nPassword for Hilltop");
    CHECK(!s.password_prompt());                                      // not the whole prompt yet
    s.feed(" repeater: ");
    CHECK(s.password_prompt() && !s.at_prompt() && s.current_line() == "Password for Hilltop repeater: ");
    s.sent_line(true);
    CHECK(s.masking() && !s.password_prompt());
    s.feed("s");
    s.feed("ecret");
    s.feed("\r\nLogin success\r\nSimNode/Hilltop repeater|0> ");
    CHECK(!s.masking() && s.masked_bytes() == 6);
    CHECK(s.text().find("secret") == std::string::npos && s.text().find("Password for Hilltop repeater: \nLogin success") != std::string::npos);
    CHECK(s.at_prompt());
    // the other prompt (login <name> $): "Password: "
    s.feed("login Weather $\r\nPassword: ");
    CHECK(s.password_prompt());
    s.sent_line(true);
    s.feed("pw\r\n");
    CHECK(s.text().find("pw\n") == std::string::npos && !s.masking());
    // never hidden for long: after kCliMaskMax bytes without a line end the output shows again
    s.feed("Password: ");
    CHECK(s.password_prompt());
    s.sent_line(true);
    s.feed(std::string(kCliMaskMax + 5, 'x'));
    CHECK(!s.masking() && s.current_line() == "Password: xxxxx");
    // a line of Mesh Hop's own goes on a line of its own
    s.note("-- meshcore-cli ended");
    CHECK(s.text().find("Password: xxxxx\n-- meshcore-cli ended\n") != std::string::npos && s.current_line().empty());
    s.clear();
    CHECK(s.text().empty() && !s.at_prompt());
    // 0.7.2: Open in CLI's own `to <64 hex>` line: its echo is replaced by the short text, the rest shows as it comes
    s.feed("SimNode> ");
    s.sent_line_shown("to Hilltop repeater <909192");
    CHECK(s.masking());
    const std::string key64 = "909192939495969798999a9b9c9d9e9fa0a1a2a3a4a5a6a7a8a9aaabacadaeaf";
    s.feed("to " + key64.substr(0, 20));
    s.feed(key64.substr(20) + "\r\nSimNode/Hilltop repeater|0> ");
    CHECK(!s.masking() && s.text().find(key64) == std::string::npos && s.text().find("SimNode> to Hilltop repeater <909192\nSimNode/Hilltop repeater|0> ") != std::string::npos);
    s.sent_line(false);                                                // what the user types shows as it is
    s.feed("ver\r\n");
    CHECK(s.text().find("|0> ver\n") != std::string::npos);
    s.clear();
    // the prompt texts
    CHECK(is_password_prompt("Password for Weather room: ") && is_password_prompt("Password: "));
    CHECK(!is_password_prompt("Password for : ") && !is_password_prompt("SimNode> ") && !is_password_prompt("Password for x:"));
}

static void test_cli_history()
{
    CliHistory h(3);
    std::string out;
    CHECK(!h.up("draft", out) && !h.down(out));
    h.add("infos");
    h.add("ver");
    h.add("ver");                                                      // the same line twice: kept once
    h.add("");                                                         // an empty line: nothing
    CHECK(h.size() == 2 && h.at_end());
    CHECK(h.up("typ", out) && out == "ver");
    CHECK(h.up("ignored", out) && out == "infos");
    CHECK(!h.up("", out) && out == "infos");                           // the oldest: no further
    CHECK(h.down(out) && out == "ver");
    CHECK(h.down(out) && out == "typ" && h.at_end());                  // back to the draft
    CHECK(!h.down(out));
    h.add("a");
    h.add("b");                                                        // the cap: the oldest goes
    CHECK(h.size() == 3 && h.up("", out) && out == "b" && h.up("", out) && out == "a" && h.up("", out) && out == "ver" && !h.up("", out));
    h.reset();
    CHECK(h.at_end() && h.up("", out) && out == "b");
}

static void test_cli_lines_and_texts()
{
    CHECK(cli_line_bytes("infos") == "infos\n");
    CHECK(cli_line_bytes("") == "\n");
    CHECK(cli_line_bytes("to a\rb\x1b[A\tc\x7f") == "to ab[Ac\n");               // controls out: a pasted CR or ESC never acts
    CHECK(cli_line_bytes("h\xC3\xA9") == "h\xC3\xA9\n");                       // UTF-8 kept
    CHECK(std::string(kCliInterrupt) == "\x03");
    size_t at = 0;
    CHECK(cli_insert("", 0, "to ", &at) == "to " && at == 3);
    CHECK(cli_insert("abc", 3, "rs", &at) == "abc rs" && at == 6);
    CHECK(cli_insert("to ", 3, "login", &at) == "to login" && at == 8);
    CHECK(cli_insert("ab", 1, "x", &at) == "a xb" && at == 3);
    CHECK(cli_insert("ab", 99, "x", &at) == "ab x" && at == 4);
    CHECK(cli_exit_text(0, 0, true) == "meshcore-cli ended");
    CHECK(cli_exit_text(127, 0, false).rfind("python3 is missing: install it", 0) == 0);
    CHECK(cli_exit_text(127, 0, true) == "meshcore-cli could not start (exit 127)");
    CHECK(cli_exit_text(3, 0, true).find("does not match Mesh Hop's wrapper (mccli.py, exit 3)") != std::string::npos);
    CHECK(cli_exit_text(1, 0, true) == "meshcore-cli ended (exit 1)");
    CHECK(cli_exit_text(-1, 9, true) == "meshcore-cli was stopped (signal 9)");
    CHECK(cli_relay_end_text("the link to the node was lost") == "The link to the node was lost");
    CHECK(cli_relay_end_text("the console did not connect") == "meshcore-cli did not connect to Mesh Hop in time");
    CHECK(cli_relay_end_text("closed by Mesh Hop") == "Closed by Mesh Hop" && cli_relay_end_text("") == "The session ended");
    CHECK(std::string(cli_phase_name(CliPhase::Running)) == "running" && std::string(cli_phase_name(CliPhase::Ended)) == "ended");
}

/* The Help list: its shape, and every name and shortcut it gives against the bundled meshcore-cli (when run_tests.sh points at it). */
static void test_cli_help()
{
    const std::vector<HelpItem> &items = cli_help_items();
    int sections = 0, inserts = 0, warnings = 0, usage = 0;
    for (const HelpItem &h : items) {
        if (h.kind == HelpKind::Section) ++sections;
        if (h.kind == HelpKind::Insert) {
            ++inserts;
            CHECK(!h.insert.empty() && !h.text.empty());
        }
        if (h.kind == HelpKind::Warning) ++warnings;
        if (h.kind == HelpKind::Usage) ++usage;
    }
    CHECK(sections == 5 && inserts >= 25 && warnings == 7 && usage == 1 && items.front().kind == HelpKind::Section);
    auto has = [&](const std::string &label) {
        for (const HelpItem &h : items)
            if (h.label == label) return true;
        return false;
    };
    for (const char *w : {"set prv.key", "poweroff, reboot, clkreboot", "start ota", "set radio, tempradio", "password", "setperm", "No acknowledgement"}) CHECK(has(w));
    for (const char *c : {"to <name>", "/<command>", "?<command>", "login", "infos", "ver", "clock", "clock sync", "advert", "floodadv", "contacts",
                          "node_discover <type>", "logout", "rs", "rn", "rt", "trace", "board", "get name", "get radio"})
        CHECK(has(c));
    // 0.7.0: How it works says where to log in (the Repeater page of Contacts' Manage), in its first section
    bool manage = false;
    for (size_t i = 0; i < items.size() && (i == 0 || items[i].kind != HelpKind::Section); ++i)
        manage = manage || (items[i].label == "Manage" && items[i].text == "Use Manage on the Contacts page to log in: it also works here." &&
                            items[i].kind == HelpKind::Info);
    CHECK(manage);
    const char *py = std::getenv("MESHHOP_CLI_PY");
    std::ifstream f(py ? py : "");
    if (!f) {
        std::printf("cli help: MESHHOP_CLI_PY not set: the bundled meshcore_cli.py is not checked\n");
        return;
    }
    std::stringstream ss;
    ss << f.rdbuf();
    const std::string src = ss.str();
    // the two password prompts the masking relies on (a pin bump that changes them fails here)
    CHECK(src.find("PromptSession(f\"Password for {contact['adv_name']}: \", is_password=True)") != std::string::npos);
    CHECK(src.find("PromptSession(\"Password: \", is_password=True)") != std::string::npos);
    CHECK(src.find("VERSION = \"v1.6.6\"") != std::string::npos);
    // the commands and the shortcuts the list names, as meshcore-cli matches them
    for (const char *q : {"\"infos\"", "\"i\"", "\"ver\"", "\"v\"", "\"clock\"", "\"st\"", "\"advert\"", "\"a\"", "\"floodadv\"", "\"contacts\"",
                          "\"lc\"", "\"msg\"", "\"m\"", "\"chan\"", "\"ch\"", "\"node_discover\"", "\"nd\"", "\"reboot\"", "\"login\"", "\"logout\"",
                          "\"cmd\"", "\"rs\"", "\"rn\"", "\"rt\"", "\"trace\"", "\"to\"", "\"get\"", "\"set\"", "\"clkreboot\"", "\"start ota\"",
                          "\"password\"", "\"setperm\"", "\"forget_password\"", "\"fp\"", "\"req_status\"", "\"req_neighbours\"", "\"req_telemetry\""})
        if (src.find(q) == std::string::npos) {
            CHECK(!"a command of the Help list is not in the bundled meshcore-cli");
            std::printf("  missing: %s\n", q);
        }
    CHECK(src.find("line.startswith(\"get acl\")") != std::string::npos);     // get acl becomes req_acl (the Cable only row says so)
    CHECK(src.find("dest == \"/\"") != std::string::npos);                  // to / goes back to the own node
}

static void test_cli_ui()
{
    test_cli_term_copy();
    test_cli_screen();
    test_cli_history();
    test_cli_lines_and_texts();
    test_cli_help();
}
