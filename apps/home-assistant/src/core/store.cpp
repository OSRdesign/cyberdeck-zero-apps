/*
 * SPDX-License-Identifier: MIT
 *
 * Home Assistant app core: the files the app keeps (see store.hpp).
 */
#include "store.hpp"

#include "json.hpp"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace ha {
namespace store {

std::string config_dir()
{
    if (const char *d = std::getenv("HA_CONFIG_DIR"))
        if (*d) return d;
    std::string base;
    if (const char *x = std::getenv("XDG_CONFIG_HOME"))
        if (*x == '/') base = x;
    if (base.empty()) {
        const char *home = std::getenv("HOME");
        base = std::string(home && *home ? home : "/tmp") + "/.config";
    }
    return base + "/home-assistant";
}

bool make_dirs(const std::string &dir, unsigned mode)
{
    if (dir.empty()) return false;
    std::string cur;
    std::size_t pos = 0;
    while (pos != std::string::npos) {
        pos = dir.find('/', pos + 1);
        cur = dir.substr(0, pos);
        if (cur.empty()) continue;
        if (::mkdir(cur.c_str(), pos == std::string::npos ? mode : 0755) != 0 && errno != EEXIST) return false;
    }
    ::chmod(dir.c_str(), mode);
    return true;
}

bool read_file(const std::string &path, std::string &out, std::size_t max_bytes)
{
    out.clear();
    FILE *f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    char buf[4096];
    std::size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) {
        out.append(buf, n);
        if (out.size() > max_bytes) {
            std::fclose(f);
            out.clear();
            return false;
        }
    }
    std::fclose(f);
    return true;
}

bool write_atomic(const std::string &path, const std::string &text, unsigned mode)
{
    const std::string tmp = path + ".tmp";
    ::unlink(tmp.c_str());
    const int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, mode);
    if (fd < 0) return false;
    ::fchmod(fd, mode); /* the umask must not widen or narrow it */
    std::size_t off = 0;
    while (off < text.size()) {
        const ssize_t w = ::write(fd, text.data() + off, text.size() - off);
        if (w < 0) {
            if (errno == EINTR) continue;
            ::close(fd);
            ::unlink(tmp.c_str());
            return false;
        }
        off += static_cast<std::size_t>(w);
    }
    ::fsync(fd);
    if (::close(fd) != 0 || ::rename(tmp.c_str(), path.c_str()) != 0) {
        ::unlink(tmp.c_str());
        return false;
    }
    return true;
}

static bool load_json(const std::string &path, json::Value &v)
{
    std::string text;
    return read_file(path, text) && json::parse(text, v) && v.is_object();
}

Settings load_settings(const std::string &dir)
{
    Settings s;
    json::Value v;
    if (!load_json(dir + "/settings.json", v)) return s;
    if (const json::Value *x = v.get("server"))
        if (!x->as_string().empty()) s.server = x->as_string();
    if (const json::Value *x = v.get("qr_use_ip")) s.qr_use_ip = x->as_bool();
    return s;
}

bool save_settings(const std::string &dir, const Settings &s)
{
    if (!make_dirs(dir, 0700)) return false;
    const std::string text = std::string("{\n  \"server\": ") + json::quote(s.server) +
                             ",\n  \"qr_use_ip\": " + (s.qr_use_ip ? "true" : "false") + "\n}\n";
    return write_atomic(dir + "/settings.json", text, 0644);
}

std::vector<std::string> load_favourites(const std::string &dir)
{
    std::vector<std::string> out;
    json::Value v;
    if (!load_json(dir + "/favourites.json", v)) return out;
    if (const json::Value *list = v.get("entities"))
        for (const json::Value &e : list->items) {
            const std::string id = e.as_string();
            if (!id.empty() && id.find('.') != std::string::npos && out.size() < 500) out.push_back(id);
        }
    return out;
}

bool save_favourites(const std::string &dir, const std::vector<std::string> &ids)
{
    if (!make_dirs(dir, 0700)) return false;
    std::string text = "{\n  \"entities\": [";
    for (std::size_t i = 0; i < ids.size(); ++i) text += (i ? ", " : "") + json::quote(ids[i]);
    text += "]\n}\n";
    return write_atomic(dir + "/favourites.json", text, 0644);
}

std::map<std::string, std::string> load_names(const std::string &dir)
{
    std::map<std::string, std::string> out;
    json::Value v;
    if (!load_json(dir + "/names.json", v)) return out;
    if (const json::Value *names = v.get("names"))
        for (const auto &m : names->members) {
            const std::string name = m.second.as_string();
            if (m.first.find('.') != std::string::npos && !name.empty() && out.size() < 500) out[m.first] = name;
        }
    return out;
}

bool save_names(const std::string &dir, const std::map<std::string, std::string> &names)
{
    if (!make_dirs(dir, 0700)) return false;
    std::string text = "{\n  \"names\": {";
    bool first = true;
    for (const auto &kv : names) {
        text += std::string(first ? "\n    " : ",\n    ") + json::quote(kv.first) + ": " + json::quote(kv.second);
        first = false;
    }
    text += first ? "}\n}\n" : "\n  }\n}\n";
    return write_atomic(dir + "/names.json", text, 0644);
}

Credentials load_credentials(const std::string &dir)
{
    Credentials c;
    json::Value v;
    if (!load_json(dir + "/credentials.json", v)) return c;
    if (const json::Value *x = v.get("server")) c.server = x->as_string();
    if (const json::Value *x = v.get("client_id")) c.client_id = x->as_string();
    if (const json::Value *x = v.get("refresh_token")) c.refresh_token = x->as_string();
    return c;
}

bool save_credentials(const std::string &dir, const Credentials &c)
{
    if (!make_dirs(dir, 0700)) return false;
    const std::string text = "{\n  \"server\": " + json::quote(c.server) + ",\n  \"client_id\": " +
                             json::quote(c.client_id) + ",\n  \"refresh_token\": " + json::quote(c.refresh_token) +
                             "\n}\n";
    return write_atomic(dir + "/credentials.json", text, 0600);
}

bool delete_credentials(const std::string &dir)
{
    const std::string path = dir + "/credentials.json";
    return ::unlink(path.c_str()) == 0 || errno == ENOENT;
}

} // namespace store
} // namespace ha
