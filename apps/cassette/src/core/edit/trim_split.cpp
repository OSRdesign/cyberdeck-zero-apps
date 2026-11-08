/*
 * SPDX-License-Identifier: MIT
 */
#include "trim_split.hpp"

#include "edit_util.hpp"
#include "peaks.hpp"

#include <algorithm>
#include <cstring>
#include <fcntl.h>
#include <set>

namespace cassette {
namespace {

using namespace editutil;

std::string cstr(const unsigned char *p, size_t max) {
    size_t n = 0;
    while (n < max && p[n]) n++;
    return std::string(reinterpret_cast<const char *>(p), n);
}

std::string xml_escape(const std::string &s) {
    std::string o;
    for (char c : s) {
        switch (c) {
        case '&': o += "&amp;"; break;
        case '<': o += "&lt;"; break;
        case '>': o += "&gt;"; break;
        case '"': o += "&quot;"; break;
        default: o += c;
        }
    }
    return o;
}

/* Reads the source's bext fields from the first bytes of the file. */
/* false when the file cannot be read (a source without bext is fine: true). */
bool read_bext(FileOps &ops, const std::string &path, const HeaderInfo &h, BwfInfo &out) {
    if (!h.has_bext) return true;
    int fd = ops.open(path, O_RDONLY, 0);
    if (fd < 0) return false;
    FdGuard g(ops, fd);
    std::vector<unsigned char> b(static_cast<size_t>(std::min<uint64_t>(h.data_offset, 65536)));
    if (pread_full(ops, fd, b.data(), b.size(), 0) != static_cast<int64_t>(b.size())) return false;
    if (h.bext_offset < 8 || h.bext_offset + 346 > b.size()) return true;
    const uint32_t sz = get32(&b[static_cast<size_t>(h.bext_offset) - 4]);
    if (sz < 346 || h.bext_offset + sz > b.size()) return true;
    const unsigned char *p = &b[static_cast<size_t>(h.bext_offset)];
    out.description = cstr(p, 256);
    out.originator = cstr(p + 256, 32);
    out.originator_reference = cstr(p + 288, 32);
    out.origination_date = cstr(p + 320, 10);
    out.origination_time = cstr(p + 330, 8);
    out.time_reference = static_cast<uint64_t>(get32(p + 338)) | static_cast<uint64_t>(get32(p + 342)) << 32;
    if (sz > 602) out.coding_history = cstr(p + 602, sz - 602);
    out.ixml.clear();
    return true;
}

std::string trim_history(std::string h, size_t max) {
    while (h.size() > max) {
        size_t nl = h.find('\n');
        if (nl == std::string::npos) return h.substr(h.size() - max);
        h.erase(0, nl + 1);
    }
    return h;
}

EditResult fail(EditResult &r, EditError e, const std::string &msg) {
    r.ok = false;
    r.error = e;
    r.message = msg;
    return r;
}

std::string errtxt(const char *what, int err) { return std::string(what) + " failed (errno " + std::to_string(-err) + ")"; }

}  // namespace

TakeInfo inspect_take(FileOps &ops, const std::string &path) {
    TakeInfo t;
    HeaderInfo h = parse_header(ops, path);
    if (!h.ok) {
        t.error = "cannot read the take: " + h.error;
        return t;
    }
    if (h.oversized_riff || h.oversized_data) {
        t.error = "the take is damaged (its header declares more audio than the file holds); repair it first";
        return t;
    }
    uint64_t avail = h.actual_data;
    if (!h.suspicious() && h.declared_data < avail) avail = h.declared_data;
    t.fmt = h.fmt;
    t.block_align = h.block_align;
    t.frames = avail / h.block_align;
    t.data_offset = h.data_offset;
    t.size = h.file_size;
    StatInfo st;
    if (ops.stat(path, st) == 0) t.mtime = st.mtime;
    t.bwf.originator.clear();
    if (!read_bext(ops, path, h, t.bwf)) {
        t.error = "cannot read the take's metadata";
        return t;
    }
    if (t.bwf.originator.empty()) t.bwf.originator = "Cassette";
    t.ok = true;
    return t;
}

std::vector<unsigned char> build_part_header(const TakeInfo &take, const std::string &source_name, uint64_t start_frame,
                                             uint64_t frames, int part, int parts) {
    BwfInfo info = take.bwf;
    info.time_reference = take.bwf.time_reference + start_frame;
    std::string d = "Cut from " + source_name + ", frames " + std::to_string(start_frame) + "-" +
                    std::to_string(start_frame + frames) + " (part " + std::to_string(part) + " of " +
                    std::to_string(parts) + ")";
    if (!take.bwf.description.empty()) d += " | " + take.bwf.description;
    if (d.size() > 256) d.resize(256);
    info.description = d;
    std::string line = coding_history_line(take.fmt);
    const size_t at = line.find("T=Cassette");
    if (at != std::string::npos) line.replace(at, 10, "T=Cassette edit");
    std::string hist = take.bwf.coding_history; /* empty for a source without bext: only the edit line */
    if (!hist.empty() && hist.back() != '\n') hist += "\r\n";
    info.coding_history = trim_history(hist + line, 255);

    const uint64_t tr = info.time_reference;
    info.ixml = "<?xml version=\"1.0\" encoding=\"UTF-8\"?><BWFXML><IXML_VERSION>3.01</IXML_VERSION><NOTE>" +
                xml_escape(d) + "</NOTE><SPEED><FILE_SAMPLE_RATE>" + std::to_string(take.fmt.rate) +
                "</FILE_SAMPLE_RATE><AUDIO_BIT_DEPTH>" + std::to_string(take.fmt.bits) +
                "</AUDIO_BIT_DEPTH><TIMESTAMP_SAMPLE_RATE>" + std::to_string(take.fmt.rate) +
                "</TIMESTAMP_SAMPLE_RATE><TIMESTAMP_SAMPLES_SINCE_MIDNIGHT_HI>" + std::to_string(tr >> 32) +
                "</TIMESTAMP_SAMPLES_SINCE_MIDNIGHT_HI><TIMESTAMP_SAMPLES_SINCE_MIDNIGHT_LO>" +
                std::to_string(tr & 0xFFFFFFFFull) +
                "</TIMESTAMP_SAMPLES_SINCE_MIDNIGHT_LO></SPEED><HISTORY><ORIGINAL_FILENAME>" + xml_escape(source_name) +
                "</ORIGINAL_FILENAME></HISTORY></BWFXML>";
    return build_header(take.fmt, info, frames * take.block_align);
}

int cleanup_temporaries(FileOps &ops, const std::string &dir) {
    std::vector<std::string> names;
    int r = ops.list_dir(dir, names);
    if (r < 0) return r;
    int n = 0;
    for (const std::string &nm : names)
        if (nm.compare(0, std::strlen(kTmpPrefix), kTmpPrefix) == 0 && ops.unlink(dir + "/" + nm) == 0) n++;
    return n;
}
int cleanup_temporaries(const std::string &dir) { return cleanup_temporaries(posix_file_ops(), dir); }

namespace {

struct Part {
    uint64_t start = 0, frames = 0;
    std::string tmp, final_path;
    bool renamed = false;
};

class Job {
public:
    Job(const EditRequest &r, const EditOptions &o) : req(r), opt(o), ops(o.ops ? *o.ops : posix_file_ops()) {}
    EditResult run();

private:
    const EditRequest &req;
    const EditOptions &opt;
    FileOps &ops;
    EditResult res;
    TakeInfo take;
    std::vector<Part> parts;
    std::string dir, base;
    uint64_t total_bytes = 0, done_bytes = 0;

    bool cancelled() const { return opt.cancel && opt.cancel->load(); }
    EditResult abort_with(EditError e, const std::string &msg) {
        discard();
        return fail(res, e, msg);
    }
    void discard() { /* temporaries and any final already renamed (never the original) */
        for (Part &p : parts) {
            if (!p.tmp.empty() && !p.renamed) ops.unlink(p.tmp);
            if (p.renamed && p.final_path != req.source_path) ops.unlink(p.final_path);
        }
    }
    EditResult validate(bool &single_over);
    int copy_part(Part &p);
    EditResult commit(bool single_over);
};

EditResult Job::validate(bool &single_over) {
    take = inspect_take(ops, req.source_path);
    if (!take.ok) return fail(res, EditError::BadSource, take.error);
    if (req.in_frame >= req.out_frame) return fail(res, EditError::BadPositions, "IN must be before OUT");
    if (req.out_frame > take.frames) return fail(res, EditError::BadPositions, "OUT is past the end of the take");
    uint64_t prev = req.in_frame;
    for (uint64_t s : req.splits) {
        if (s <= prev) return fail(res, EditError::BadPositions, "split points must be increasing and after IN");
        if (s >= req.out_frame) return fail(res, EditError::BadPositions, "a split point must be before OUT");
        prev = s;
    }
    dir = dir_of(req.source_path);
    base = base_of(req.source_path);
    std::vector<uint64_t> cuts;
    cuts.push_back(req.in_frame);
    cuts.insert(cuts.end(), req.splits.begin(), req.splits.end());
    cuts.push_back(req.out_frame);
    for (size_t i = 0; i + 1 < cuts.size(); i++) {
        Part p;
        p.start = cuts[i];
        p.frames = cuts[i + 1] - cuts[i];
        parts.push_back(p);
    }
    single_over = req.mode == SaveMode::ReplaceOriginal && parts.size() == 1;
    if (single_over) {
        parts[0].final_path = req.source_path;
    } else {
        if (!opt.names) return fail(res, EditError::BadNames, "no name provider");
        std::vector<std::string> names = opt.names(base, static_cast<int>(parts.size()));
        if (names.size() != parts.size()) return fail(res, EditError::BadNames, "the name provider gave the wrong number of names");
        std::set<std::string> seen;
        for (size_t i = 0; i < names.size(); i++) {
            const std::string &n = names[i];
            if (n.empty() || n.find('/') != std::string::npos || n == base || n.compare(0, 5, kTmpPrefix) == 0 ||
                !seen.insert(n).second)
                return fail(res, EditError::BadNames, "bad result name: " + n);
            StatInfo st;
            if (ops.stat(dir + "/" + n, st) == 0) return fail(res, EditError::BadNames, "a file named " + n + " exists");
            parts[i].final_path = dir + "/" + n;
        }
    }
    for (const Part &p : parts) total_bytes += p.frames * take.block_align;
    uint64_t need = opt.free_margin;
    for (const Part &p : parts) need += kDataOffset + p.frames * take.block_align;
    uint64_t avail = 0;
    int r = ops.free_space(dir, avail);
    if (r < 0) return fail(res, EditError::Io, errtxt("checking free space", r));
    if (avail < need)
        return fail(res, EditError::NoSpace,
                    "not enough free space: " + std::to_string(need) + " bytes needed, " + std::to_string(avail) + " free");
    res.ok = true;
    return res;
}

/* Writes one result into its temporary file. 0, or -errno (-ECANCELED on cancel, -ESTALE when the source changed). */
int Job::copy_part(Part &p) {
    p.tmp = tmp_name(dir);
    int fd = ops.open(p.tmp, O_WRONLY | O_CREAT | O_EXCL, 0644);
    if (fd < 0) {
        p.tmp.clear();
        return fd;
    }
    FdGuard out(ops, fd);
    int sfd = ops.open(req.source_path, O_RDONLY, 0);
    if (sfd < 0) return sfd;
    FdGuard in(ops, sfd);

    const std::vector<unsigned char> hdr = build_part_header(take, base, p.start, p.frames,
                                                             static_cast<int>(&p - &parts[0]) + 1, static_cast<int>(parts.size()));
    int r = write_all(ops, fd, hdr.data(), hdr.size());
    if (r < 0) return r;
    const uint64_t bytes = p.frames * take.block_align;
    const uint64_t src0 = take.data_offset + p.start * take.block_align;
    std::vector<unsigned char> buf(std::max<size_t>(opt.block_bytes, 1));
    uint64_t off = 0;
    while (off < bytes) {
        if (cancelled()) return -ECANCELED;
        const size_t n = static_cast<size_t>(std::min<uint64_t>(buf.size(), bytes - off));
        int64_t got = pread_full(ops, sfd, buf.data(), n, src0 + off);
        if (got < 0) return static_cast<int>(got);
        if (static_cast<size_t>(got) != n) return -EIO; /* the take is shorter than it was */
        r = write_all(ops, fd, buf.data(), n);
        if (r < 0) return r;
        off += n;
        done_bytes += n;
        if (opt.progress) opt.progress(done_bytes, total_bytes);
    }
    r = ops.fdatasync(fd);
    if (r < 0) return r;
    r = ops.close(out.release());
    return r < 0 ? r : 0;
}

EditResult Job::commit(bool single_over) {
    if (cancelled()) return abort_with(EditError::Cancelled, "cancelled");
    /* the source must be what we copied from */
    StatInfo st;
    int r = ops.stat(req.source_path, st);
    if (r < 0 || st.size != take.size || st.mtime != take.mtime)
        return abort_with(EditError::SourceChanged, "the take changed while it was being copied");

    for (Part &p : parts) {
        r = ops.rename(p.tmp, p.final_path);
        if (r < 0) return abort_with(EditError::Io, errtxt("rename", r));
        p.renamed = true;
        res.outputs.push_back(p.final_path);
    }
    r = ops.fsync_dir(dir);
    if (r < 0) {
        if (single_over) { /* the original was replaced atomically: nothing to roll back */
            res.warning = errtxt("syncing the folder", r);
        } else {
            res.outputs.clear();
            return abort_with(EditError::Io, errtxt("syncing the folder", r));
        }
    }
    res.ok = true;
    res.error = EditError::None;
    if (req.mode == SaveMode::ReplaceOriginal) {
        if (single_over) {
            res.original_removed = true; /* its name now holds the result */
        } else {
            r = ops.unlink(req.source_path);
            if (r < 0) {
                res.warning = "the results are saved but the original could not be removed (" + errtxt("unlink", r) + ")";
            } else {
                res.original_removed = true;
            }
        }
        if (res.original_removed) {
            ops.unlink(PeakPyramid::cache_path(req.source_path));
            if (opt.on_original_deleted) opt.on_original_deleted(req.source_path);
            const int d = ops.fsync_dir(dir);
            if (d < 0 && res.warning.empty()) res.warning = errtxt("syncing the folder", d);
        }
    }
    return res;
}

EditResult Job::run() {
    bool single_over = false;
    validate(single_over);
    if (!res.ok) return res;
    res.ok = false;
    if (opt.progress) opt.progress(0, total_bytes);
    for (Part &p : parts) {
        if (cancelled()) return abort_with(EditError::Cancelled, "cancelled");
        int r = copy_part(p);
        if (r == -ECANCELED) return abort_with(EditError::Cancelled, "cancelled");
        if (r < 0) {
            EditError e = (r == -ENOSPC || r == -EDQUOT) ? EditError::NoSpace : EditError::Io;
            return abort_with(e, e == EditError::NoSpace ? "the disk is full" : errtxt("writing the result", r));
        }
    }
    return commit(single_over);
}

}  // namespace

EditResult trim_split(const EditRequest &req, const EditOptions &opt) {
    Job j(req, opt);
    return j.run();
}

}  // namespace cassette
