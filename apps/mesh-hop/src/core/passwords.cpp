/*
 * SPDX-License-Identifier: MIT
 */

#include "passwords.hpp"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace meshzero {

namespace {

bool is_dir(const std::string &p)
{
    struct stat st;
    return ::stat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

/* mkdir -p with `mode` for the folders it creates. */
bool make_dirs(const std::string &dir, mode_t mode)
{
    std::string cur;
    for (size_t i = 0; i <= dir.size(); ++i) {
        if (i == dir.size() || dir[i] == '/') {
            if (!cur.empty() && ::mkdir(cur.c_str(), mode) != 0 && errno != EEXIST) return false;
        }
        if (i < dir.size()) cur.push_back(dir[i]);
    }
    return is_dir(dir);
}

bool is_space(char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f';
}

/* Python's str.strip() for the ASCII white space a password line can carry. */
std::string strip(const std::string &s)
{
    size_t a = 0, b = s.size();
    while (a < b && is_space(s[a])) ++a;
    while (b > a && is_space(s[b - 1])) --b;
    return s.substr(a, b - a);
}

/* Overwrites a buffer that held a password before it is released. */
void wipe(std::vector<char> &v)
{
    volatile char *p = v.data();
    for (size_t i = 0; i < v.size(); ++i) p[i] = 0;
}

} // namespace

PasswordStore::PasswordStore(std::string data_dir) : data_dir_(std::move(data_dir))
{
    while (data_dir_.size() > 1 && data_dir_.back() == '/') data_dir_.pop_back();
}

std::string PasswordStore::dir() const
{
    return data_dir_ + "/cli/.config/meshcore";
}

std::string PasswordStore::normalize_key(const std::string &key_hex)
{
    if (key_hex.size() != 64) return "";
    std::string k = key_hex;
    for (char &c : k) {
        if (c >= 'A' && c <= 'F') c = static_cast<char>(c - 'A' + 'a');
        else if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return "";
    }
    return k;
}

std::string PasswordStore::path(const std::string &key_hex) const
{
    const std::string k = normalize_key(key_hex);
    return k.empty() ? "" : dir() + "/" + k + ".pass";
}

std::optional<std::string> PasswordStore::read(const std::string &key_hex) const
{
    const std::string p = path(key_hex);
    if (p.empty()) return std::nullopt;
    const int fd = ::open(p.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return std::nullopt;
    std::vector<char> buf(512, 0);                 // a password is at most 15 bytes: the first line is all that is read
    size_t got = 0;
    while (got < buf.size()) {
        const ssize_t n = ::read(fd, buf.data() + got, buf.size() - got);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        got += static_cast<size_t>(n);
    }
    ::close(fd);
    size_t end = 0;
    while (end < got && buf[end] != '\n') ++end;
    std::string line = strip(std::string(buf.data(), end));
    wipe(buf);
    if (line.empty() || line.find('\0') != std::string::npos) return std::nullopt;
    return line;
}

PasswordStore::SaveResult PasswordStore::save(const std::string &key_hex, const std::string &password)
{
    const std::string p = path(key_hex);
    if (p.empty()) return SaveResult::BadKey;
    if (password.empty()) return SaveResult::Blank;
    if (password.find_first_of(std::string("\n\r\0", 3)) != std::string::npos || strip(password) != password) return SaveResult::NotStorable;

    // the folders: <data> as the Store makes it, then cli/, .config/, meshcore/ private to the user (as mccli makes them)
    if (!make_dirs(data_dir_, 0755)) return SaveResult::IoError;
    for (const std::string &d : {data_dir_ + "/cli", data_dir_ + "/cli/.config", dir()}) {
        if (::mkdir(d.c_str(), 0700) != 0 && errno != EEXIST) return SaveResult::IoError;
        if (!is_dir(d) || ::chmod(d.c_str(), 0700) != 0) return SaveResult::IoError;
    }

    std::string tmpl = p + ".XXXXXX";
    std::vector<char> name(tmpl.begin(), tmpl.end());
    name.push_back('\0');
    const int fd = ::mkstemp(name.data());          // created 0600, in the same folder: the rename below is atomic
    if (fd < 0) return SaveResult::IoError;
    bool ok = ::fchmod(fd, 0600) == 0;
    size_t done = 0;
    while (ok && done < password.size()) {
        const ssize_t n = ::write(fd, password.data() + done, password.size() - done);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) { ok = false; break; }
        done += static_cast<size_t>(n);
    }
    if (ok && ::fsync(fd) != 0) ok = false;
    if (::close(fd) != 0) ok = false;
    if (ok && ::rename(name.data(), p.c_str()) != 0) ok = false;
    if (!ok) {
        ::unlink(name.data());
        return SaveResult::IoError;
    }
    return SaveResult::Saved;
}

bool PasswordStore::forget(const std::string &key_hex)
{
    const std::string p = path(key_hex);
    if (p.empty()) return false;
    return ::unlink(p.c_str()) == 0 || errno == ENOENT;
}

} // namespace meshzero
