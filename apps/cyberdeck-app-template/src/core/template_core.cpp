/*
 * SPDX-License-Identifier: MIT
 *
 * The template app's logic (see template_core.hpp).
 */
#include "template_core.hpp"

#include <cstdio>

namespace tpl {

Back Nav::back()
{
    if (stack_.size() <= 1) return Back::Quit;
    stack_.pop_back();
    return Back::Popped;
}

std::vector<std::string> Nav::title_path(const std::string &app_title) const
{
    std::vector<std::string> path{app_title};
    for (std::size_t i = 1; i < stack_.size(); ++i) path.push_back(view_title(stack_[i]));
    return path;
}

const char *view_title(View view)
{
    switch (view) {
    case View::Root: return "";
    case View::Info: return "Device info";
    case View::Form: return "Name";
    }
    return "";
}

std::string trim(const std::string &text)
{
    const std::size_t first = text.find_first_not_of(" \t");
    if (first == std::string::npos) return std::string();
    const std::size_t last = text.find_last_not_of(" \t");
    return text.substr(first, last - first + 1);
}

std::size_t utf8_length(const std::string &text)
{
    std::size_t count = 0;
    for (std::size_t i = 0; i < text.size();) {
        const unsigned char c = static_cast<unsigned char>(text[i]);
        std::size_t n = 1;
        if (c >= 0xF0 && c < 0xF8) n = 4;
        else if (c >= 0xE0) n = 3;
        else if (c >= 0xC0) n = 2;
        if (i + n > text.size()) n = 1;
        for (std::size_t k = 1; k < n; ++k)
            if ((static_cast<unsigned char>(text[i + k]) & 0xC0) != 0x80) n = 1;
        i += n;
        ++count;
    }
    return count;
}

std::string name_error(const std::string &name)
{
    if (name.empty()) return "Type a name first";
    for (const char ch : name) {
        const unsigned char c = static_cast<unsigned char>(ch);
        if (c < 0x20 || c == 0x7F) return "A name cannot hold tabs or control keys";
    }
    if (utf8_length(name) > kNameMaxChars) return "At most " + std::to_string(kNameMaxChars) + " characters";
    return std::string();
}

void RunState::begin()
{
    phase_ = Phase::Running;
    lines_ = 0;
    last_.clear();
    exit_code_ = -1;
}

void RunState::line(const std::string &text)
{
    ++lines_;
    if (!trim(text).empty()) last_ = trim(text);
}

void RunState::finish(bool started, int exit_code, int term_signal, bool timed_out, bool cancelled)
{
    exit_code_ = exit_code;
    if (!started) phase_ = Phase::Failed;
    else if (cancelled) phase_ = Phase::Stopped;
    else if (timed_out) phase_ = Phase::TimedOut;
    else if (term_signal != 0 || exit_code != 0) phase_ = Phase::Failed;
    else phase_ = Phase::Done;
}

std::string RunState::busy_label() const
{
    return last_.empty() ? std::string("Starting...") : last_;
}

std::string RunState::value() const
{
    switch (phase_) {
    case Phase::Idle: return std::string();
    case Phase::Running: return busy_label();
    case Phase::Done: return "Done, " + std::to_string(lines_) + (lines_ == 1 ? " line" : " lines");
    case Phase::Failed: return exit_code_ > 0 ? "Failed (exit " + std::to_string(exit_code_) + ")" : "Failed";
    case Phase::TimedOut: return "Timed out";
    case Phase::Stopped: return "Stopped";
    }
    return std::string();
}

std::vector<std::string> demo_command()
{
    return {"sh", "-c", "for i in 1 2 3 4 5; do echo \"step $i of 5\"; sleep 0.4; done"};
}

std::string format_ppmm(int ppmm_x100)
{
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%d.%02d px/mm", ppmm_x100 / 100, ppmm_x100 % 100);
    return buf;
}

std::string screen_summary(const Facts &f)
{
    return std::to_string(f.w) + "x" + std::to_string(f.h) + (f.cls.empty() ? "" : " " + f.cls);
}

std::vector<std::pair<std::string, std::string>> info_items(const Facts &f)
{
    std::vector<std::pair<std::string, std::string>> items;
    items.emplace_back("Screen", screen_summary(f));
    items.emplace_back("Density", format_ppmm(f.ppmm_x100));
    items.emplace_back("Row / target", std::to_string(f.row) + " / " + std::to_string(f.target) + " px");
    items.emplace_back("Header", std::to_string(f.header) + " px");
    items.emplace_back("Launcher", f.contract ? "contract 1, " + (f.mode.empty() ? std::string("?") : f.mode)
                                              : std::string("no contract"));
    if (f.input_file) {
        items.emplace_back("Keyboards", std::to_string(f.keyboards));
        items.emplace_back("Keyboard rule", f.vkb.empty() ? std::string("auto") : f.vkb);
    } else {
        items.emplace_back("Keyboards", "no input.state");
    }
    items.emplace_back("On-screen keyboard", f.osk_allowed ? "allowed" : "not now");
    items.emplace_back("Version", f.version);
    items.emplace_back("Launcher build", f.launcher_ref);
    return items;
}

} // namespace tpl
