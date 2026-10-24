/*
 * SPDX-License-Identifier: MIT
 */
#include "note_file.hpp"

#include "dates.hpp"
#include "fsutil.hpp"
#include "utf8.hpp"

#include <cerrno>

namespace mdnotes {

namespace {

DiskStamp stamp_of(const std::string &path, const std::string &bytes)
{
    DiskStamp s;
    const FileStat fs = stat_path(path);
    s.exists = fs.exists;
    s.mtime_ns = fs.mtime_ns;
    s.size = fs.exists ? fs.size : -1;
    s.hash = content_hash(bytes);
    return s;
}

} // namespace

std::uint64_t content_hash(const std::string &bytes)
{
    std::uint64_t h = 1469598103934665603ULL;
    for (unsigned char c : bytes) {
        h ^= c;
        h *= 1099511628211ULL;
    }
    return h;
}

LoadedNote load_note(const std::string &path)
{
    LoadedNote n;
    std::string raw;
    int err = 0;
    switch (read_file(path, raw, kReadCap, &err)) {
    case ReadStatus::Ok: break;
    case ReadStatus::Missing: n.status = LoadStatus::Missing; return n;
    case ReadStatus::TooBig: n.status = LoadStatus::TooBig; return n;
    case ReadStatus::Error: n.status = LoadStatus::Error; n.err = err; return n;
    }
    n.stamp = stamp_of(path, raw);
    n.body = decode_text(raw);
    if (!utf8_valid(raw)) n.status = LoadStatus::ReadOnlyNotUtf8;
    else if (raw.size() > kEditCap) n.status = LoadStatus::ReadOnlyLarge;
    else n.status = LoadStatus::Ok;
    return n;
}

DiskState check_disk(const std::string &path, DiskStamp &stamp)
{
    const FileStat fs = stat_path(path);
    if (!fs.exists || !fs.is_reg) return DiskState::Gone;
    if (!stamp.exists) return DiskState::Changed;   /* it appeared where we had nothing */
    if (fs.size != stamp.size) return DiskState::Changed;
    std::string raw;   /* same size: the bytes decide (at most kEditCap for an editable note, from the page cache) */
    if (read_file(path, raw, kReadCap + 1) != ReadStatus::Ok) return DiskState::Changed;
    if (content_hash(raw) != stamp.hash) return DiskState::Changed;
    stamp.mtime_ns = fs.mtime_ns;   /* touched, same bytes */
    return DiskState::Same;
}

SaveResult save_note(const std::string &path, const std::string &body, const TextFormat &fmt, DiskStamp &stamp, bool force)
{
    SaveResult r;
    const DiskState state = check_disk(path, stamp);
    if (state == DiskState::Changed && !force) {
        r.status = SaveStatus::Conflict;
        return r;
    }
    r.recreated = state == DiskState::Gone;
    const std::string raw = encode_text(body, fmt);
    if (r.recreated) {
        int err = 0;
        const std::string dir = dir_name(path);
        if (!dir.empty() && !make_dirs(dir, &err)) {
            r.err = err;
            return r;
        }
    }
    int err = 0;
    if (!write_file_atomic(path, raw, &err)) {
        r.err = err;
        return r;
    }
    stamp = stamp_of(path, raw);
    r.status = SaveStatus::Ok;
    return r;
}

std::string conflict_copy_path(const std::string &path, std::time_t now)
{
    const std::string dir = dir_name(path);
    std::string name = base_name(path);
    if (name.size() > 3 && equal_ci(name.substr(name.size() - 3), ".md")) name.resize(name.size() - 3);
    const std::string stem = name + " (deck " + conflict_stamp(now) + ")";
    for (int i = 1; i < 1000; ++i) {
        const std::string candidate = path_join(dir, (i == 1 ? stem : stem + " " + std::to_string(i)) + ".md");
        if (!stat_path(candidate, false).exists) return candidate;
    }
    return path_join(dir, stem + " " + std::to_string(static_cast<long long>(now)) + ".md");
}

bool create_note(const std::string &path, const std::string &body, const TextFormat &fmt, DiskStamp *stamp, int *err)
{
    const std::string dir = dir_name(path);
    if (!dir.empty() && !make_dirs(dir, err)) return false;
    const std::string raw = encode_text(body, fmt);
    if (!create_file_exclusive(path, raw, err)) return false;
    if (stamp) *stamp = stamp_of(path, raw);
    return true;
}

} // namespace mdnotes
