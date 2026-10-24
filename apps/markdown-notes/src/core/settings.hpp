/*
 * SPDX-License-Identifier: MIT
 *
 * Markdown Notes core: the settings (report 037 sections 2.4 and 4, Settings). One small key=value file,
 * ~/.config/markdown-notes/settings.conf ($XDG_CONFIG_HOME when set):
 *
 *   vault=~/Notes          the vault folder ("~/" is the home folder; a relative path is taken from the home folder)
 *   text_size=medium       small | medium | large (the preview and the editor, as RSS Reader's)
 *   open_in=preview        preview | editor: how an existing note opens (new notes and Today always open in the editor)
 *
 * $NOTES_VAULT, when set and not empty, wins over vault= (tests, power users). Unknown keys and malformed lines are
 * ignored and dropped on the next save; a missing or unreadable file gives the defaults. The vault folder is created
 * on first use (open_vault()).
 */
#pragma once

#include <string>

namespace mdnotes {

constexpr const char *kAppId = "markdown-notes";
constexpr const char *kDefaultVault = "~/Notes";

enum class TextSize { Small, Medium, Large };
enum class OpenIn { Preview, Editor };

struct Settings {
    std::string vault = kDefaultVault;   /* as written in the file */
    TextSize text_size = TextSize::Medium;
    OpenIn open_in = OpenIn::Preview;
};

const char *text_size_key(TextSize s);   /* "small" ... */
const char *open_in_key(OpenIn o);       /* "preview" | "editor" */

/* $XDG_CONFIG_HOME/markdown-notes, else <home>/.config/markdown-notes. */
std::string config_dir(const std::string &home);
std::string settings_path(const std::string &home);

Settings parse_settings(const std::string &text);
std::string format_settings(const Settings &s);
Settings load_settings(const std::string &path);
/* The folder created when missing, the file written atomically. */
bool save_settings(const std::string &path, const Settings &s, int *err = nullptr);

/* "~" and "~/x" from home; a relative path from base; trailing slashes removed. */
std::string expand_path(const std::string &path, const std::string &home, const std::string &base);
/* The vault folder in force: $NOTES_VAULT (env_vault, relative to the current folder) when not empty, else the
 * settings' (relative to home). */
std::string resolve_vault(const Settings &s, const char *env_vault, const std::string &home);

enum class VaultState { Ready, Created, NotAFolder, Unusable };
struct VaultOpen {
    VaultState state = VaultState::Unusable;
    std::string path;
    int err = 0;
};
/* The folder exists (Ready) or was made now with its parents (Created); a file in its place (NotAFolder); it cannot be
 * made or read (Unusable, err). */
VaultOpen open_vault(const std::string &path);

} // namespace mdnotes
