/*
 * SPDX-License-Identifier: MIT
 *
 * The template app's logic, without LVGL (unit-tested on the PC by tests/test_core.cpp): page navigation, the name
 * rule of the text field, the state of the background command shown by the Busy row, the device info rows.
 * A real app keeps its logic the same way: core/ holds what can be tested without a screen, ui/ only draws it.
 */
#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace tpl {

/* The smallest screen the template supports: X-MinSize of its .desktop file (tests/test_core.cpp compares them). */
constexpr int kMinW = 640;
constexpr int kMinH = 480;

/* ------------------------------------------------------------------ navigation */

enum class View { Root, Info, Form };
enum class Back { Quit, Popped };

class Nav {
public:
    View current() const { return stack_.back(); }
    int depth() const { return static_cast<int>(stack_.size()); }
    void open(View view) { stack_.push_back(view); }
    /* Esc / Left / the header's Esc: one level up, or the app ends at the root (Controller decision 2026-10-13). */
    Back back();
    /* The header title path: the app title, then the open page. */
    std::vector<std::string> title_path(const std::string &app_title) const;

private:
    std::vector<View> stack_{View::Root};
};

const char *view_title(View view);

/* ------------------------------------------------------------------ the text field */

constexpr std::size_t kNameMaxChars = 24;

/* Spaces at both ends removed. */
std::string trim(const std::string &text);
/* Code points in a UTF-8 string (a malformed byte counts as one). */
std::size_t utf8_length(const std::string &text);
/* "" when `name` (already trimmed) is acceptable, else the message for the page's status line. */
std::string name_error(const std::string &name);

/* ------------------------------------------------------------------ the background command (Busy row) */

enum class Phase { Idle, Running, Done, Failed, TimedOut, Stopped };

class RunState {
public:
    void begin();
    void line(const std::string &text);
    /* from async_run::Result: started, exit code (-1 none), signal (0 none), timed out, cancelled */
    void finish(bool started, int exit_code, int term_signal, bool timed_out, bool cancelled);

    Phase phase() const { return phase_; }
    bool busy() const { return phase_ == Phase::Running; }
    std::uint64_t lines() const { return lines_; }
    /* The row: while running a Busy row labelled busy_label(); otherwise a Nav row "label()" with value(). */
    std::string busy_label() const;
    std::string label() const { return "Run a command"; }
    std::string value() const;
    bool failed() const { return phase_ == Phase::Failed || phase_ == Phase::TimedOut; }

private:
    Phase phase_ = Phase::Idle;
    std::uint64_t lines_ = 0;
    std::string last_;
    int exit_code_ = -1;
};

/* The demonstration command: five lines, 0.4 s apart (sh is on every board). */
std::vector<std::string> demo_command();
constexpr int kDemoTimeoutMs = 10000;

/* ------------------------------------------------------------------ device info */

struct Facts {
    int w = 0, h = 0;
    std::string cls;              /* "standard", "wide", ... */
    int ppmm_x100 = 0;
    int row = 0, target = 0, header = 0;
    bool contract = false;        /* started by a contract-1 launcher */
    std::string mode;             /* "fullscreen", "compat", "" */
    bool input_file = false;      /* input.state found */
    unsigned keyboards = 0;
    std::string vkb;              /* "auto", "off", "force" */
    bool osk_allowed = false;
    std::string version, launcher_ref;
};

/* "11.30 px/mm" */
std::string format_ppmm(int ppmm_x100);
/* The rows of the Device info table: key, value. */
std::vector<std::pair<std::string, std::string>> info_items(const Facts &facts);
/* The root's Screen row value: "640x480 standard" */
std::string screen_summary(const Facts &facts);

} // namespace tpl
