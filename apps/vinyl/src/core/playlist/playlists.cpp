/*
 * SPDX-License-Identifier: MIT
 *
 * Vinyl playlists: the files (see playlists.hpp).
 */
#include "playlists.hpp"

#include "library/browse.hpp"

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <fstream>
#include <sstream>

namespace vinyl {

namespace {

bool lower_eq(const std::string &a, const std::string &b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) return false;
    return true;
}

bool ends_with_ci(const std::string &s, const char *suffix) {
    const size_t n = std::char_traits<char>::length(suffix);
    return s.size() >= n && lower_eq(s.substr(s.size() - n), suffix);
}

/* the next code point of a valid UTF-8 text at i (advances i); false for a bad sequence */
bool next_cp(const std::string &s, size_t &i, uint32_t &cp) {
    const unsigned char c = static_cast<unsigned char>(s[i]);
    size_t n = 0;
    if (c < 0x80) {
        cp = c;
        ++i;
        return true;
    } else if ((c & 0xE0) == 0xC0) {
        n = 1;
        cp = c & 0x1F;
    } else if ((c & 0xF0) == 0xE0) {
        n = 2;
        cp = c & 0x0F;
    } else if ((c & 0xF8) == 0xF0) {
        n = 3;
        cp = c & 0x07;
    } else {
        return false;
    }
    if (i + n >= s.size()) return false;
    for (size_t k = 1; k <= n; ++k) {
        const unsigned char d = static_cast<unsigned char>(s[i + k]);
        if ((d & 0xC0) != 0x80) return false;
        cp = (cp << 6) | (d & 0x3F);
    }
    i += n + 1;
    return true;
}

bool allowed_cp(uint32_t cp) {
    if (cp < 0x80) return std::isalnum(static_cast<int>(cp)) || cp == ' ' || cp == '-' || cp == '_';
    return cp >= 0xA1 && cp != 0x2028 && cp != 0x2029 && cp != 0xFEFF && !(cp >= 0xD800 && cp < 0xE000) && cp <= 0x10FFFF;
}

std::string read_whole(const std::string &path, bool *ok) {
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        if (ok) *ok = false;
        return std::string();
    }
    std::ostringstream ss;
    ss << f.rdbuf();
    if (ok) *ok = true;
    return ss.str();
}

}  // namespace

const char *pl_result_text(PlResult r) {
    switch (r) {
    case PlResult::Ok: return "ok";
    case PlResult::Taken: return "that name is taken";
    case PlResult::NotFound: return "no such playlist";
    case PlResult::Invalid: return "not a valid name";
    case PlResult::Io: return "the file could not be written";
    case PlResult::Range: return "out of range";
    }
    return "?";
}

std::string playlist_stem(const std::string &path) {
    const size_t slash = path.rfind('/');
    std::string n = slash == std::string::npos ? path : path.substr(slash + 1);
    const size_t dot = n.rfind('.');
    if (dot != std::string::npos && dot > 0 && (ends_with_ci(n, ".m3u8") || ends_with_ci(n, ".m3u"))) n.erase(dot);
    return n;
}

bool is_playlist_file_name(const std::string &name) { return ends_with_ci(name, ".m3u8") || ends_with_ci(name, ".m3u"); }

PlaylistStore::PlaylistStore(std::string dir, std::vector<std::string> roots, std::vector<DriveBase> bases)
    : dir_(std::move(dir)), roots_(std::move(roots)), bases_(std::move(bases)) {
    while (dir_.size() > 1 && dir_.back() == '/') dir_.pop_back();
}

bool PlaylistStore::same_drive(const std::string &a, const std::string &b) const {
    return drive_dir_of(a, bases_) == drive_dir_of(b, bases_);
}

/* ------------------------------------------------------------------------------------------------ names */

int PlaylistStore::name_length(const std::string &name) {
    int n = 0;
    size_t i = 0;
    uint32_t cp;
    while (i < name.size()) {
        if (!next_cp(name, i, cp)) {
            ++i;
        }
        ++n;
    }
    return n;
}

std::string PlaylistStore::trim_name(const std::string &name) {
    size_t a = 0, b = name.size();
    while (a < b && name[a] == ' ') ++a;
    while (b > a && name[b - 1] == ' ') --b;
    return name.substr(a, b - a);
}

NameProblem PlaylistStore::name_problem(const std::string &raw) {
    const std::string name = trim_name(raw);
    if (name.empty()) return NameProblem::Empty;
    size_t i = 0;
    uint32_t cp;
    int n = 0;
    while (i < name.size()) {
        if (!next_cp(name, i, cp) || !allowed_cp(cp)) return NameProblem::BadChar;
        ++n;
    }
    return n > kMaxName ? NameProblem::TooLong : NameProblem::None;
}

std::string PlaylistStore::clean_name(const std::string &raw) {
    std::string out;
    size_t i = 0;
    uint32_t cp;
    int n = 0;
    bool space = true; /* no leading space, runs of spaces one */
    while (i < raw.size()) {
        const size_t start = i;
        bool ok = next_cp(raw, i, cp);
        if (!ok) {
            ++i;
            cp = ' ';
        }
        const bool keep = ok && allowed_cp(cp) && cp != ' ';
        if (!keep) {
            if (!space) {
                out += ' ';
                ++n;
                space = true;
            }
        } else {
            out += raw.substr(start, i - start);
            ++n;
            space = false;
        }
        if (n >= kMaxName) break;
    }
    out = trim_name(out);
    if (out.empty()) return "Playlist";
    return out;
}

std::string PlaylistStore::free_name(const std::string &base_in) const {
    const std::string base = clean_name(base_in);
    if (!exists(base)) return base;
    for (int k = 2; k < 1000; ++k) {
        const std::string suffix = " " + std::to_string(k);
        std::string b = base;
        while (name_length(b) + static_cast<int>(suffix.size()) > kMaxName && !b.empty()) {
            /* drop the last code point */
            size_t cut = b.size() - 1;
            while (cut > 0 && (static_cast<unsigned char>(b[cut]) & 0xC0) == 0x80) --cut;
            b.erase(cut);
        }
        b = trim_name(b) + suffix;
        if (!exists(b)) return b;
    }
    return base + " 1000";
}

std::string PlaylistStore::path_of(const std::string &name) const { return dir_ + "/" + name + ".m3u8"; }

std::string PlaylistStore::find(const std::string &name) const {
    DIR *d = ::opendir(dir_.c_str());
    if (!d) return std::string();
    std::string found;
    while (const dirent *e = ::readdir(d)) {
        const std::string n = e->d_name;
        if (n.empty() || n[0] == '.' || !ends_with_ci(n, ".m3u8")) continue;
        const std::string stem = n.substr(0, n.size() - 5);
        if (lower_eq(stem, name)) {
            found = stem;
            break;
        }
    }
    ::closedir(d);
    return found;
}

std::vector<std::string> PlaylistStore::names() {
    std::vector<std::string> out;
    DIR *d = ::opendir(dir_.c_str());
    if (!d) return out;
    while (const dirent *e = ::readdir(d)) {
        const std::string n = e->d_name;
        if (n.size() > 5 && n[0] == '.' && ends_with_ci(n, ".tmp")) { /* an interrupted write: the old file is whole, this is the leftover */
            ::unlink((dir_ + "/" + n).c_str());
            continue;
        }
        if (n.empty() || n[0] == '.' || !ends_with_ci(n, ".m3u8")) continue;
        struct stat st;
        if (::stat((dir_ + "/" + n).c_str(), &st) != 0 || !S_ISREG(st.st_mode)) continue;
        out.push_back(n.substr(0, n.size() - 5));
    }
    ::closedir(d);
    std::sort(out.begin(), out.end(), [](const std::string &a, const std::string &b) { return natural_less(a, b); });
    return out;
}

/* ------------------------------------------------------------------------------------------------ files */

m3u::ParseOptions PlaylistStore::parse_options(const std::string &file_dir) const {
    m3u::ParseOptions o;
    o.base_dir = file_dir;
    o.roots = roots_;
    return o;
}

std::string PlaylistStore::text_for(const std::vector<m3u::Entry> &entries, const std::string &file_dir) const {
    m3u::WriteOptions w;
    w.playlist_dir = file_dir;
    w.same_drive = [this](const std::string &a, const std::string &b) { return same_drive(a, b); };
    return m3u::write(entries, w);
}

bool PlaylistStore::ensure_dir() const {
    struct stat st;
    if (::stat(dir_.c_str(), &st) == 0) return S_ISDIR(st.st_mode);
    for (size_t i = 1; i <= dir_.size(); ++i)
        if (i == dir_.size() || dir_[i] == '/') {
            const std::string part = dir_.substr(0, i);
            if (::mkdir(part.c_str(), 0755) != 0 && errno != EEXIST) return false;
        }
    return ::stat(dir_.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

PlResult PlaylistStore::write_file_atomic(const std::string &path, const std::string &bytes) const {
    const size_t slash = path.rfind('/');
    const std::string dir = slash == std::string::npos ? "." : path.substr(0, slash);
    const std::string name = slash == std::string::npos ? path : path.substr(slash + 1);
    const std::string tmp = dir + "/." + name + ".tmp";
    const int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) return PlResult::Io;
    size_t off = 0;
    bool ok = true;
    while (off < bytes.size()) {
        const ssize_t w = ::write(fd, bytes.data() + off, bytes.size() - off);
        if (w < 0) {
            if (errno == EINTR) continue;
            ok = false;
            break;
        }
        off += static_cast<size_t>(w);
    }
    if (ok && ::fsync(fd) != 0 && errno != EINVAL && errno != EROFS) ok = false;
    ::close(fd);
    if (!ok) {
        ::unlink(tmp.c_str());
        return PlResult::Io;
    }
    if (test_hook && !test_hook("written")) return PlResult::Io; /* as a power cut: the temporary file stays, the old one is whole */
    if (::rename(tmp.c_str(), path.c_str()) != 0) {
        ::unlink(tmp.c_str());
        return PlResult::Io;
    }
    const int dfd = ::open(dir.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (dfd >= 0) {
        ::fsync(dfd);
        ::close(dfd);
    }
    return PlResult::Ok;
}

PlResult PlaylistStore::read(const std::string &name, std::vector<m3u::Entry> &out) const {
    out.clear();
    const std::string real = find(name);
    if (real.empty()) return PlResult::NotFound;
    bool ok = false;
    const std::string bytes = read_whole(path_of(real), &ok);
    if (!ok) return PlResult::Io;
    out = m3u::parse(bytes, parse_options(dir_));
    return PlResult::Ok;
}

PlResult PlaylistStore::write(const std::string &name, const std::vector<m3u::Entry> &entries) {
    const std::string real = find(trim_name(name));
    if (real.empty() && name_problem(name) != NameProblem::None) return PlResult::Invalid; /* a file dropped in by hand keeps its name */
    if (!ensure_dir()) return PlResult::Io;
    return write_file_atomic(path_of(real.empty() ? trim_name(name) : real), text_for(entries, dir_));
}

PlResult PlaylistStore::create(const std::string &name, const std::vector<m3u::Entry> &entries) {
    if (name_problem(name) != NameProblem::None) return PlResult::Invalid;
    if (exists(trim_name(name))) return PlResult::Taken;
    return write(name, entries);
}

PlResult PlaylistStore::append(const std::string &name, const std::vector<m3u::Entry> &add) {
    std::vector<m3u::Entry> e;
    const PlResult r = read(name, e);
    if (r != PlResult::Ok) return r;
    e.insert(e.end(), add.begin(), add.end());
    return write(find(name), e);
}

PlResult PlaylistStore::move(const std::string &name, int index, int delta, int *new_index) {
    std::vector<m3u::Entry> e;
    const PlResult r = read(name, e);
    if (r != PlResult::Ok) return r;
    const int to = index + delta;
    if (index < 0 || index >= static_cast<int>(e.size()) || to < 0 || to >= static_cast<int>(e.size())) return PlResult::Range;
    const m3u::Entry x = e[static_cast<size_t>(index)];
    e.erase(e.begin() + index);
    e.insert(e.begin() + to, x);
    if (new_index) *new_index = to;
    return write(find(name), e);
}

PlResult PlaylistStore::remove_at(const std::string &name, int index) {
    std::vector<m3u::Entry> e;
    const PlResult r = read(name, e);
    if (r != PlResult::Ok) return r;
    if (index < 0 || index >= static_cast<int>(e.size())) return PlResult::Range;
    e.erase(e.begin() + index);
    return write(find(name), e);
}

PlResult PlaylistStore::rename(const std::string &from, const std::string &to) {
    const std::string real = find(from);
    if (real.empty()) return PlResult::NotFound;
    if (name_problem(to) != NameProblem::None) return PlResult::Invalid;
    const std::string target = trim_name(to);
    const std::string other = find(target);
    if (!other.empty() && !lower_eq(other, real)) return PlResult::Taken; /* the same playlist under a new case is allowed */
    if (other == target && real == target) return PlResult::Ok;
    if (::rename(path_of(real).c_str(), path_of(target).c_str()) != 0) return PlResult::Io;
    return PlResult::Ok;
}

PlResult PlaylistStore::remove(const std::string &name) {
    const std::string real = find(name);
    if (real.empty()) return PlResult::NotFound;
    return ::unlink(path_of(real).c_str()) == 0 ? PlResult::Ok : PlResult::Io;
}

PlResult PlaylistStore::import_file(const std::string &src_path, const std::string &name) {
    if (name_problem(name) != NameProblem::None) return PlResult::Invalid;
    if (exists(trim_name(name))) return PlResult::Taken;
    bool ok = false;
    const std::string bytes = read_whole(src_path, &ok);
    if (!ok) return PlResult::NotFound;
    const std::vector<m3u::Entry> entries = m3u::parse(bytes, parse_options(m3u::dir_of(src_path)));
    return write(name, entries);
}

PlResult PlaylistStore::export_to(const std::string &name, const std::string &dest_dir_in, std::string *out_path) {
    std::vector<m3u::Entry> e;
    const PlResult r = read(name, e);
    if (r != PlResult::Ok) return r;
    std::string dest = dest_dir_in;
    while (dest.size() > 1 && dest.back() == '/') dest.pop_back();
    struct stat st;
    if (::stat(dest.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) return PlResult::Io;
    const std::string path = dest + "/" + find(name) + ".m3u8";
    const PlResult w = write_file_atomic(path, text_for(e, dest));
    if (w == PlResult::Ok && out_path) *out_path = path;
    return w;
}

}  // namespace vinyl
