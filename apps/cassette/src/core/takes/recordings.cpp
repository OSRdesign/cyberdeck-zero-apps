/*
 * SPDX-License-Identifier: MIT
 */
#include "recordings.hpp"

#include "../journal.hpp"
#include "../wav.hpp"
#include "atomic_file.hpp"

#include <algorithm>
#include <cerrno>
#include <fcntl.h>

namespace cassette {
namespace {

bool ends_with(const std::string &s, const std::string &tail) {
    return s.size() >= tail.size() && s.compare(s.size() - tail.size(), tail.size(), tail) == 0;
}

/* A fixed-size bext text field: up to the first NUL, trailing spaces dropped. */
std::string field(const unsigned char *p, size_t n) {
    size_t k = 0;
    while (k < n && p[k]) k++;
    std::string s(reinterpret_cast<const char *>(p), k);
    while (!s.empty() && s.back() == ' ') s.pop_back();
    return s;
}

struct JournalRef {
    std::string file;
    JournalEntry e;
};

std::vector<JournalRef> read_journals(FileOps &ops, const std::string &dir) {
    std::vector<JournalRef> v;
    std::vector<std::string> names;
    const std::string jd = journal_dir(dir);
    if (ops.list_dir(jd, names) != 0) return v;
    std::sort(names.begin(), names.end());
    for (const std::string &n : names) {
        if (!ends_with(n, ".json")) continue;
        JournalRef r;
        r.file = jd + "/" + n;
        if (journal_read(ops, r.file, r.e) == 0) v.push_back(std::move(r));
    }
    return v;
}

bool journal_names(const JournalEntry &e, const std::string &name) {
    if (base_name(e.path) == name) return true;
    for (const std::string &p : e.parts)
        if (base_name(p) == name) return true;
    return false;
}

const JournalRef *journal_of(const std::vector<JournalRef> &js, const std::string &name) {
    for (const JournalRef &j : js)
        if (journal_names(j.e, name)) return &j;
    return nullptr;
}

void fill_header(FileOps &ops, TakeInfo &t) {
    HeaderInfo h = parse_header(ops, t.path);
    if (!h.ok || h.block_align == 0) {
        t.damaged = true;
        t.error = h.error.empty() ? "unreadable header" : h.error;
        return;
    }
    t.fmt = h.fmt;
    uint64_t bytes = h.suspicious() ? h.actual_data : std::min(h.declared_data, h.actual_data);
    t.frames = bytes / h.block_align;
    t.seconds = h.fmt.rate ? static_cast<double>(t.frames) / h.fmt.rate : 0;
    if (h.has_bext) {
        unsigned char b[346] = {};
        int fd = ops.open(t.path, O_RDONLY, 0);
        if (fd >= 0) {
            if (ops.pread(fd, b, sizeof b, h.bext_offset) == static_cast<ssize_t>(sizeof b)) {
                t.has_bext = true;
                t.origination_date = field(b + 320, 10);
                t.origination_time = field(b + 330, 8);
                uint64_t tr = 0;
                for (int k = 7; k >= 0; k--) tr = (tr << 8) | b[338 + k];
                t.time_reference = tr;
            }
            ops.close(fd);
        }
    }
}

TakeInfo read_one(FileOps &ops, const std::string &dir, const std::string &name, const std::vector<JournalRef> &js) {
    TakeInfo t;
    t.name = name;
    t.path = dir + "/" + name;
    t.named = parse_take_name(name, t.tn);
    t.label = t.named ? take_label(t.tn) : file_stem(name);
    StatInfo st;
    if (ops.stat(t.path, st) == 0) {
        t.size = st.size;
        t.mtime = st.mtime;
    }
    fill_header(ops, t);
    if (const JournalRef *j = journal_of(js, name)) {
        t.journal = j->file;
        t.repaired = j->e.state == "repaired";
    }
    return t;
}

bool newer_first(const TakeInfo &a, const TakeInfo &b) {
    if (a.named != b.named) return a.named;
    if (a.named) {
        if (a.tn.take != b.tn.take) return a.tn.take > b.tn.take;
        if (a.tn.part != b.tn.part) return a.tn.part < b.tn.part;
        if (a.tn.edit != b.tn.edit) return a.tn.edit < b.tn.edit;
    } else if (a.mtime != b.mtime) {
        return a.mtime > b.mtime;
    }
    return a.name < b.name;
}

}  // namespace

std::string expand_home(const std::string &path, const std::string &home) {
    if (path == "~") return home;
    if (path.size() >= 2 && path[0] == '~' && path[1] == '/') return home + path.substr(1);
    return path;
}

bool is_take_file_name(const std::string &name) {
    if (name.empty() || name[0] == '.') return false;
    if (!has_wav_extension(name) || name.size() <= 4) return false;
    if (name.find(".tmp") != std::string::npos) return false; /* "x.wav.tmp", "x.tmp.wav": unfinished writes */
    return true;
}

TakeInfo read_take_info(FileOps &ops, const std::string &dir, const std::string &name) {
    return read_one(ops, dir, name, read_journals(ops, dir));
}

int list_takes(FileOps &ops, const std::string &dir, std::vector<TakeInfo> &out) {
    out.clear();
    std::vector<std::string> names;
    int r = ops.list_dir(dir, names);
    if (r < 0) return r;
    const std::vector<JournalRef> js = read_journals(ops, dir);
    for (const std::string &n : names)
        if (is_take_file_name(n)) out.push_back(read_one(ops, dir, n, js));
    std::stable_sort(out.begin(), out.end(), newer_first);
    return 0;
}

uint64_t bytes_per_hour(const AudioFormat &fmt) {
    const uint64_t bytes = (static_cast<uint64_t>(fmt.bits) + 7) / 8;
    return static_cast<uint64_t>(fmt.rate) * fmt.channels * bytes * 3600u;
}

double hours_left(uint64_t free_bytes, const AudioFormat &fmt) {
    const uint64_t bph = bytes_per_hour(fmt);
    return bph ? static_cast<double>(free_bytes) / static_cast<double>(bph) : 0.0;
}

int folder_space(FileOps &ops, const std::string &dir, const AudioFormat &fmt, SpaceInfo &out) {
    uint64_t avail = 0;
    int r = ops.free_space(dir, avail);
    if (r < 0) return r;
    out.free_bytes = avail;
    out.hours_left = hours_left(avail, fmt);
    return 0;
}

std::string peaks_name(const std::string &take_file_name) { return file_stem(take_file_name) + ".peaks"; }

std::string find_journal_for(FileOps &ops, const std::string &dir, const std::string &name) {
    const std::vector<JournalRef> js = read_journals(ops, dir);
    const JournalRef *j = journal_of(js, base_name(name));
    return j ? j->file : std::string();
}

int delete_take(FileOps &ops, const std::string &dir, const std::string &take_file_name) {
    const std::string name = base_name(take_file_name);
    if (!is_take_file_name(name)) return -EINVAL;
    int r = ops.unlink(dir + "/" + name);
    if (r < 0) return r;
    ops.unlink(dir + "/" + peaks_name(name));
    const std::vector<JournalRef> js = read_journals(ops, dir);
    for (const JournalRef &j : js) {
        if (!journal_names(j.e, name)) continue;
        JournalEntry e = j.e;
        std::vector<std::string> rest;
        for (const std::string &p : e.parts)
            if (base_name(p) != name) rest.push_back(p);
        if (rest.empty()) {
            ops.unlink(j.file);
        } else {
            e.parts = rest;
            e.path = rest[0];
            journal_write(ops, dir, e); /* best effort: the audio file is already gone */
        }
    }
    ops.fsync_dir(dir);
    return 0;
}

int rename_take(FileOps &ops, const std::string &dir, const std::string &take_file_name, const std::string &new_prefix,
                std::string &new_name) {
    const std::string name = base_name(take_file_name);
    TakeName t;
    if (!valid_prefix(new_prefix) || !parse_take_name(name, t)) return -EINVAL;
    t.prefix = new_prefix;
    const std::string target = format_take_name(t);
    if (target == name) {
        new_name = name;
        return 0;
    }
    if (path_exists(ops, dir + "/" + target)) return -EEXIST;
    if (!path_exists(ops, dir + "/" + name)) return -ENOENT;
    int r = ops.rename(dir + "/" + name, dir + "/" + target);
    if (r < 0) return r;
    new_name = target;
    if (path_exists(ops, dir + "/" + peaks_name(name)))
        ops.rename(dir + "/" + peaks_name(name), dir + "/" + peaks_name(target));
    ops.fsync_dir(dir);
    const std::vector<JournalRef> js = read_journals(ops, dir);
    for (const JournalRef &j : js) {
        if (!journal_names(j.e, name)) continue;
        JournalEntry e = j.e;
        auto swap = [&](std::string &p) {
            if (base_name(p) == name) p = (p.find('/') == std::string::npos ? target : dir_name(p) + "/" + target);
        };
        swap(e.path);
        for (std::string &p : e.parts) swap(p);
        /* e.take (the journal's file name) stays: take numbers never repeat, and the journal is found by path. */
        r = journal_write(ops, dir, e);
        if (r < 0) return r;
    }
    return 0;
}

}  // namespace cassette
