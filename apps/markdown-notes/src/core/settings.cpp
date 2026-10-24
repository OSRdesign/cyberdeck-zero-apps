/*
 * SPDX-License-Identifier: MIT
 */
#include "settings.hpp"

#include "fsutil.hpp"

#include <cerrno>
#include <cstdlib>
#include <unistd.h>

namespace mdnotes {

namespace {

std::string trim(const std::string &s)
{
    std::size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r')) ++a;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r')) --b;
    return s.substr(a, b - a);
}

} // namespace

const char *text_size_key(TextSize s)
{
    switch (s) {
    case TextSize::Small: return "small";
    case TextSize::Large: return "large";
    case TextSize::Medium: break;
    }
    return "medium";
}

const char *open_in_key(OpenIn o) { return o == OpenIn::Editor ? "editor" : "preview"; }

std::string config_dir(const std::string &home)
{
    if (const char *x = std::getenv("XDG_CONFIG_HOME"); x && *x == '/') return path_join(x, kAppId);
    return path_join(path_join(home, ".config"), kAppId);
}

std::string settings_path(const std::string &home) { return path_join(config_dir(home), "settings.conf"); }

Settings parse_settings(const std::string &text)
{
    Settings s;
    std::size_t i = 0;
    while (i <= text.size()) {
        std::size_t j = text.find('\n', i);
        if (j == std::string::npos) j = text.size();
        const std::string line = trim(text.substr(i, j - i));
        i = j + 1;
        if (line.empty() || line[0] == '#') continue;
        const std::size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string key = trim(line.substr(0, eq)), value = trim(line.substr(eq + 1));
        if (key == "vault") {
            if (!value.empty()) s.vault = value;
        } else if (key == "text_size") {
            if (value == "small") s.text_size = TextSize::Small;
            else if (value == "large") s.text_size = TextSize::Large;
            else if (value == "medium") s.text_size = TextSize::Medium;
        } else if (key == "open_in") {
            if (value == "editor") s.open_in = OpenIn::Editor;
            else if (value == "preview") s.open_in = OpenIn::Preview;
        }
    }
    return s;
}

std::string format_settings(const Settings &s)
{
    std::string out = "# Markdown Notes settings (NOTES_VAULT in the environment wins over vault=)\n";
    out += "vault=" + s.vault + "\n";
    out += std::string("text_size=") + text_size_key(s.text_size) + "\n";
    out += std::string("open_in=") + open_in_key(s.open_in) + "\n";
    return out;
}

Settings load_settings(const std::string &path)
{
    std::string text;
    if (read_file(path, text, 64 * 1024) != ReadStatus::Ok) return Settings();
    return parse_settings(text);
}

bool save_settings(const std::string &path, const Settings &s, int *err)
{
    if (!make_dirs(dir_name(path), err)) return false;
    return write_file_atomic(path, format_settings(s), err);
}

std::string expand_path(const std::string &path, const std::string &home, const std::string &base)
{
    std::string p = trim(path);
    if (p == "~") p = home;
    else if (p.compare(0, 2, "~/") == 0) p = path_join(home, p.substr(2));
    else if (!p.empty() && p[0] != '/') p = path_join(base, p);
    while (p.size() > 1 && p.back() == '/') p.pop_back();
    return p;
}

std::string resolve_vault(const Settings &s, const char *env_vault, const std::string &home)
{
    if (env_vault && *env_vault) return expand_path(env_vault, home, current_dir());
    const std::string v = s.vault.empty() ? std::string(kDefaultVault) : s.vault;
    return expand_path(v, home, home);
}

VaultOpen open_vault(const std::string &path)
{
    VaultOpen v;
    v.path = path;
    const FileStat fs = stat_path(path);
    if (fs.exists && !fs.is_dir) {
        v.state = VaultState::NotAFolder;
        v.err = ENOTDIR;
        return v;
    }
    if (!fs.exists) {
        int err = 0;
        if (!make_dirs(path, &err)) {
            v.state = VaultState::Unusable;
            v.err = err;
            return v;
        }
        v.state = VaultState::Created;
        return v;
    }
    if (::access(path.c_str(), R_OK | W_OK | X_OK) != 0) {
        v.state = VaultState::Unusable;
        v.err = errno;
        return v;
    }
    v.state = VaultState::Ready;
    return v;
}

} // namespace mdnotes
