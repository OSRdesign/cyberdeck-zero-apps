/*
 * SPDX-License-Identifier: MIT
 */
#include "fsutil.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

namespace rssreader {

bool read_file(const std::string &path, std::string &out, std::size_t max_bytes)
{
    out.clear();
    std::FILE *f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    char buf[16384];
    std::size_t n;
    bool ok = true;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) {
        if (out.size() + n > max_bytes) { ok = false; break; }
        out.append(buf, n);
    }
    if (std::ferror(f)) ok = false;
    std::fclose(f);
    if (!ok) out.clear();
    return ok;
}

bool write_file_atomic(const std::string &path, const std::string &data)
{
    const std::string tmp = path + ".tmp";
    std::FILE *f = std::fopen(tmp.c_str(), "wb");
    if (!f) return false;
    bool ok = data.empty() || std::fwrite(data.data(), 1, data.size(), f) == data.size();
    ok = (std::fflush(f) == 0) && ok;
    ok = (::fsync(::fileno(f)) == 0) && ok;
    ok = (std::fclose(f) == 0) && ok;
    if (ok && std::rename(tmp.c_str(), path.c_str()) != 0) ok = false;
    if (!ok) ::unlink(tmp.c_str());
    return ok;
}

bool is_directory(const std::string &path)
{
    struct stat st;
    return ::stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

bool path_exists(const std::string &path)
{
    struct stat st;
    return ::lstat(path.c_str(), &st) == 0;
}

bool make_dirs(const std::string &path)
{
    if (path.empty()) return false;
    if (is_directory(path)) return true;
    std::string cur;
    std::size_t i = 0;
    while (i <= path.size()) {
        std::size_t j = path.find('/', i);
        if (j == std::string::npos) j = path.size();
        cur = path.substr(0, j);
        if (!cur.empty() && !is_directory(cur)) {
            if (::mkdir(cur.c_str(), 0755) != 0 && errno != EEXIST) return false;
        }
        i = j + 1;
    }
    return is_directory(path);
}

std::int64_t file_size(const std::string &path)
{
    struct stat st;
    if (::stat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) return -1;
    return static_cast<std::int64_t>(st.st_size);
}

std::vector<std::string> list_dir(const std::string &path)
{
    std::vector<std::string> v;
    DIR *d = ::opendir(path.c_str());
    if (!d) return v;
    while (struct dirent *e = ::readdir(d)) {
        std::string n = e->d_name;
        if (n != "." && n != "..") v.push_back(n);
    }
    ::closedir(d);
    std::sort(v.begin(), v.end());
    return v;
}

bool remove_tree(const std::string &path)
{
    struct stat st;
    if (::lstat(path.c_str(), &st) != 0) return true;
    if (!S_ISDIR(st.st_mode)) return ::unlink(path.c_str()) == 0;
    bool ok = true;
    for (const std::string &n : list_dir(path)) ok = remove_tree(path + "/" + n) && ok;
    return (::rmdir(path.c_str()) == 0) && ok;
}

std::uint64_t tree_size(const std::string &path)
{
    struct stat st;
    if (::lstat(path.c_str(), &st) != 0) return 0;
    if (S_ISREG(st.st_mode)) return static_cast<std::uint64_t>(st.st_size);
    if (!S_ISDIR(st.st_mode)) return 0;
    std::uint64_t total = 0;
    for (const std::string &n : list_dir(path)) total += tree_size(path + "/" + n);
    return total;
}

bool rename_path(const std::string &from, const std::string &to) { return std::rename(from.c_str(), to.c_str()) == 0; }

} // namespace rssreader
