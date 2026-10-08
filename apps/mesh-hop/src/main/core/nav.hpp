/*
 * SPDX-License-Identifier: MIT
 *
 * Esc navigation rule (user decision): a short press of Esc is always "Back" and never quits the app. The app is left
 * only by holding Esc for 3 seconds, which the launcher itself detects (cp0_esc_exit_policy) and answers by ending the
 * app: nothing here reimplements it. At a top-level screen a short Esc only shows a hint.
 */

#pragma once

namespace meshzero {

enum class Screen { Home, Chats, Contacts, Settings, Chat, Detail, Edit };

struct EscResult {
    Screen next;              // the screen to show (the same one when nothing changes)
    bool cancel_editor;       // Esc in an editor: cancel it (the editor then returns to its own caller)
    bool show_exit_hint;      // top level: show "Hold Esc 3 s to exit"
};

inline bool is_top_level(Screen s)
{
    return s == Screen::Home || s == Screen::Chats || s == Screen::Contacts || s == Screen::Settings;
}

/* tab: the top-level screen the user came from (Chat and Detail go back to the list they were opened from). */
inline EscResult esc_pressed(Screen current, Screen tab)
{
    if (current == Screen::Edit) return {Screen::Edit, true, false};
    if (current == Screen::Chat || current == Screen::Detail)
        return {tab == Screen::Contacts ? Screen::Contacts : Screen::Chats, false, false};
    return {current, false, true};
}

inline const char *kExitHint = "Hold Esc 3 s to exit";

} // namespace meshzero
