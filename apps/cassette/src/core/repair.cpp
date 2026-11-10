/*
 * SPDX-License-Identifier: MIT
 */
#include "repair.hpp"

#include "codec/stream_scan.hpp"
#include "journal.hpp"
#include "wav.hpp"

#include <algorithm>
#include <cctype>
#include <fcntl.h>
#include <set>

namespace cassette {
namespace {

std::string base_of(const std::string &p) {
    size_t s = p.rfind('/');
    return s == std::string::npos ? p : p.substr(s + 1);
}

std::string lower_suffix(const std::string &n, size_t len) {
    std::string e = n.substr(n.size() - len);
    for (char &c : e) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return e;
}

bool has_wav_suffix(const std::string &n) {
    if (n.size() < 5) return false;
    return lower_suffix(n, 4) == ".wav";
}

/* ".flac" / ".opus" (either case): the compressed takes */
bool has_stream_suffix(const std::string &n) {
    if (n.size() < 6) return false;
    const std::string e = lower_suffix(n, 5);
    return e == ".flac" || e == ".opus";
}

/* "x.wav" -> "x", "x.flac" -> "x" */
std::string stem_of(const std::string &n) { return n.substr(0, n.rfind('.')); }

FileRepair repair_stream(FileOps &ops, const std::string &path, bool force, Container k) {
    const StreamFix x = k == Container::Flac ? fix_flac(ops, path, force) : fix_ogg_opus(ops, path, force);
    FileRepair r;
    r.ok = x.ok;
    r.error = x.error;
    r.changed = x.changed;
    r.fmt = x.fmt;
    r.frames = x.frames;
    r.data_bytes = 0;
    r.truncated_bytes = static_cast<uint32_t>(std::min<uint64_t>(x.truncated_bytes, 0xFFFFFFFFull));
    return r;
}

}  // namespace

FileRepair repair_file(FileOps &ops, const std::string &path, bool force) {
    if (has_stream_suffix(path)) { /* FLAC or Ogg Opus: by the file's own magic, the extension as a fallback */
        const Container k = container_of(ops, path);
        if (k == Container::Flac || k == Container::OggOpus) return repair_stream(ops, path, force, k);
        FileRepair r;
        r.error = "not a FLAC or Ogg Opus file";
        return r;
    }
    FileRepair r;
    HeaderInfo h = parse_header(ops, path);
    if (!h.ok) {
        r.error = h.error;
        return r;
    }
    r.fmt = h.fmt;
    r.rf64 = h.rf64;
    const uint64_t ba = h.block_align;
    if (!force && !h.suspicious()) {
        r.ok = true;
        r.data_bytes = h.actual_data;
        r.frames = h.actual_data / ba;
        return r;
    }
    const uint32_t partial = static_cast<uint32_t>(h.actual_data % ba);
    const uint64_t data = h.actual_data - partial;
    r.data_bytes = data;
    r.frames = data / ba;
    r.truncated_bytes = partial;
    if (h.consistent() && partial == 0) {
        r.ok = true;
        return r;
    }
    PatchSet ps = size_patches(h.layout(), data);
    if (ps.clamped && partial == 0 && h.raw_data == 0xFFFFFFFFull && h.raw_riff == 0xFFFFFFFFull) {
        r.ok = true; /* already clamped by an earlier run: nothing more can be said */
        r.clamped = true;
        return r;
    }
    int fd = ops.open(path, O_WRONLY, 0);
    if (fd < 0) {
        r.error = "cannot open for repair (" + std::to_string(-fd) + ")";
        return r;
    }
    int rc = 0;
    if (partial) rc = ops.ftruncate(fd, h.file_size - partial); /* first: a crash here leaves a repairable file */
    for (const Patch &p : ps.patches) {
        if (rc < 0) break;
        ssize_t w = ops.pwrite(fd, p.bytes.data(), p.bytes.size(), p.offset);
        rc = w < 0 ? static_cast<int>(w) : (static_cast<size_t>(w) == p.bytes.size() ? 0 : -5);
    }
    if (rc == 0) rc = ops.fdatasync(fd);
    ops.close(fd);
    if (rc < 0) {
        r.error = "write failed (" + std::to_string(-rc) + ")";
        return r;
    }
    r.ok = true;
    r.changed = true;
    r.rf64 = ps.rf64;
    r.clamped = ps.clamped;
    return r;
}

RepairReport repair_all(FileOps &ops, const std::string &dir) {
    RepairReport rep;
    std::set<std::string> claimed; /* basenames of every file a journal knows */
    const std::string jd = journal_dir(dir);
    std::vector<std::string> names;
    ops.list_dir(jd, names); /* a missing .journal is fine */
    std::sort(names.begin(), names.end());
    for (const std::string &n : names) {
        if (n.size() < 6 || n.substr(n.size() - 5) != ".json") continue;
        JournalEntry je;
        if (journal_read(ops, jd + "/" + n, je) < 0) {
            rep.errors.push_back(".journal/" + n + ": unreadable journal");
            continue;
        }
        for (const std::string &p : je.parts) claimed.insert(base_of(p));
        if (je.state != "open") continue;

        RepairedTake t;
        t.take = je.take;
        t.path = je.path;
        t.had_journal = true;
        int64_t mtime = 0;
        uint64_t frames = 0;
        bool any = false;
        for (const std::string &p : je.parts) {
            StatInfo st;
            if (ops.stat(p, st) < 0) {
                t.note += (t.note.empty() ? "" : "; ") + base_of(p) + " is missing";
                continue;
            }
            mtime = std::max(mtime, st.mtime);
            FileRepair fr = repair_file(ops, p, true);
            if (!fr.ok) {
                rep.errors.push_back(base_of(p) + ": " + fr.error);
                t.note += (t.note.empty() ? "" : "; ") + base_of(p) + " could not be repaired";
                continue;
            }
            any = true;
            frames += fr.frames;
            t.truncated_bytes += fr.truncated_bytes;
            if (fr.clamped) t.note += (t.note.empty() ? "" : "; ") + base_of(p) + " is over 4 GiB without RF64 room";
        }
        if (!any) {
            je.state = "abandoned";
            je.reason = "no audio file found";
            if (journal_write(ops, dir, je) < 0) rep.errors.push_back(je.take + ": journal not updated");
            rep.errors.push_back(je.take + ": no audio file to repair");
            continue;
        }
        t.frames = frames;
        const double rate = je.fmt.rate ? je.fmt.rate : 48000;
        t.seconds = static_cast<double>(frames) / rate;
        if (je.started_wall > 0) {
            const int64_t end = std::max<int64_t>(je.last_sync_wall, mtime);
            const double elapsed = static_cast<double>(end - je.started_wall);
            const double have = static_cast<double>(frames + je.paused_frames) / rate;
            t.missing_seconds = std::max(0.0, elapsed - have);
        }
        je.state = "repaired";
        je.frames = frames;
        je.reason.clear();
        if (journal_write(ops, dir, je) < 0) rep.errors.push_back(je.take + ": journal not updated");
        rep.repaired.push_back(t);
    }

    std::vector<std::string> files;
    if (ops.list_dir(dir, files) == 0) {
        std::sort(files.begin(), files.end());
        for (const std::string &f : files) {
            if (!(has_wav_suffix(f) || has_stream_suffix(f)) || claimed.count(f)) continue;
            const std::string path = dir + "/" + f;
            FileRepair fr = repair_file(ops, path, false);
            if (!fr.ok) {
                rep.errors.push_back(f + ": " + fr.error);
                continue;
            }
            if (!fr.changed) continue;
            RepairedTake t;
            t.take = stem_of(f);
            t.path = path;
            t.frames = fr.frames;
            t.seconds = static_cast<double>(fr.frames) / (fr.fmt.rate ? fr.fmt.rate : 48000);
            t.truncated_bytes = fr.truncated_bytes;
            if (fr.clamped) t.note = "over 4 GiB without RF64 room";
            rep.repaired.push_back(t);
        }
    }
    return rep;
}

}  // namespace cassette
