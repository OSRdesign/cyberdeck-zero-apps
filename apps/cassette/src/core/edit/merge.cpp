/*
 * SPDX-License-Identifier: MIT
 */
#include "merge.hpp"

#include "../codec/byte_sink.hpp"
#include "../codec/decoder.hpp"
#include "../codec/flac_encoder.hpp"
#include "../codec/ogg_opus.hpp"
#include "../codec/stream_scan.hpp"
#include "edit_util.hpp"
#include "peaks.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <set>

namespace cassette {
namespace {

using namespace editutil;

const char *kind_name(EditKind k) {
    switch (k) {
    case EditKind::Flac: return "FLAC";
    case EditKind::OggOpus: return "Opus";
    case EditKind::Wav: break;
    }
    return "WAV";
}

const char *kind_ext(EditKind k) {
    switch (k) {
    case EditKind::Flac: return ".flac";
    case EditKind::OggOpus: return ".opus";
    case EditKind::Wav: break;
    }
    return ".wav";
}

bool ends_with(const std::string &s, const std::string &t) {
    return s.size() >= t.size() && s.compare(s.size() - t.size(), t.size(), t) == 0;
}

std::string errtxt(const char *what, int err) { return std::string(what) + " failed (errno " + std::to_string(-err) + ")"; }

bool codec_err(int err) { return err <= kCodecMissing && err >= kCodecBadStream; }

MergePlan plan_fail(MergePlan &p, MergeError e, const std::string &msg) {
    p.ok = false;
    p.error = e;
    p.message = msg;
    return p;
}

MergeResult result_fail(MergeResult &r, MergeError e, const std::string &msg) {
    r.ok = false;
    r.error = e;
    r.message = msg;
    return r;
}

/* The last packet's end and the first packet kept of an Ogg Opus take (see merge.hpp: a later take leaves out the packets
 * that lie wholly inside its pre-skip). */
struct OpusEnds {
    uint64_t drop = 0;
    int64_t first_start48 = 0; /* the start of the first packet kept */
    int64_t last_end48 = 0;    /* the end of the last packet (beyond end48 by the end trimming) */
};

int opus_ends(oggopus::OggOpusFile &f, bool later, OpusEnds &e) {
    if (!f.has_audio()) return kCodecBadStream;
    int r = f.rewind();
    if (r < 0) return r;
    oggopus::Packet p;
    const int64_t first_sample = f.start48() + static_cast<int64_t>(f.head().preskip);
    e = OpusEnds();
    for (;;) {
        r = f.next(p);
        if (r < 0) return r;
        if (r == 0) return kCodecBadStream; /* no packet reaches the first sample */
        if (later && p.end48 <= first_sample) {
            e.drop++;
            continue;
        }
        e.first_start48 = p.start48;
        break;
    }
    /* the tail: the packets from shortly before the end */
    r = f.seek(f.end48() - static_cast<int64_t>(2 * oggopus::kMaxPacket48));
    if (r < 0) return r;
    bool any = false;
    for (;;) {
        r = f.next(p);
        if (r < 0) return r;
        if (r == 0) break;
        e.last_end48 = p.end48;
        any = true;
    }
    if (!any || e.last_end48 < f.end48() || e.last_end48 <= e.first_start48) return kCodecBadStream;
    return f.rewind();
}

}  // namespace

std::string merge_format_label(EditKind kind, uint32_t rate) {
    std::string s = kind_name(kind);
    if (rate) {
        char b[32];
        std::snprintf(b, sizeof b, " %g kHz", rate / 1000.0);
        s += b;
    }
    return s;
}

std::string merge_take_reason(const MergeFacts &t) {
    if (!t.named) return "Not a Cassette take";
    if (!t.readable) return "Damaged take";
    return "";
}

std::string merge_join_reason(const MergeFacts &first, const MergeFacts &other) {
    const std::string r = merge_take_reason(other);
    if (!r.empty()) return r;
    if (other.kind != first.kind) return "Different format: " + merge_format_label(other.kind, other.rate);
    if (other.rate != first.rate) return "Different rate";
    if (other.channels != first.channels) return "Different channels";
    if (other.kind != EditKind::OggOpus && other.bits != first.bits) return "Different bit depth";
    return "";
}

MergeFacts merge_facts_of(const EditTakeInfo &t, bool named) {
    MergeFacts f;
    f.named = named;
    f.readable = t.ok;
    f.kind = t.kind;
    f.rate = t.fmt.rate;
    f.channels = t.fmt.channels;
    f.bits = t.fmt.bits;
    return f;
}

MergePlan plan_merge(const MergeRequest &req, const MergeOptions &opt) {
    FileOps &ops = opt.ops ? *opt.ops : posix_file_ops();
    MergePlan p;
    if (req.sources.size() < 2) return plan_fail(p, MergeError::BadSources, "Merging needs two takes");
    std::set<std::string> seen;
    p.dir = dir_of(req.sources.front());
    for (const std::string &s : req.sources) {
        if (!seen.insert(s).second) return plan_fail(p, MergeError::BadSources, "a take is chosen twice: " + base_of(s));
        if (dir_of(s) != p.dir) return plan_fail(p, MergeError::BadSources, "the takes are not in one folder");
    }
    for (const std::string &s : req.sources) {
        EditTakeInfo t = inspect_take(ops, s);
        if (!t.ok) return plan_fail(p, MergeError::BadSources, base_of(s) + ": " + t.error);
        p.takes.push_back(t);
    }
    const MergeFacts first = merge_facts_of(p.takes.front(), true);
    for (size_t i = 1; i < p.takes.size(); ++i) {
        const std::string why = merge_join_reason(first, merge_facts_of(p.takes[i], true));
        if (!why.empty()) return plan_fail(p, MergeError::Incompatible, base_of(req.sources[i]) + ": " + why);
    }
    p.kind = p.takes.front().kind;
    p.fmt = p.takes.front().fmt;
    /* the name: a plain file name of the takes' kind that does not exist */
    const std::string &n = req.output_name;
    if (n.empty() || n.find('/') != std::string::npos || n[0] == '.' || !ends_with(n, kind_ext(p.kind)))
        return plan_fail(p, MergeError::BadName, "bad name for the new take: " + n);
    StatInfo st;
    if (ops.stat(p.dir + "/" + n, st) == 0) return plan_fail(p, MergeError::BadName, "a file named " + n + " exists");
    for (const EditTakeInfo &t : p.takes) {
        p.source_frames += t.frames;
        p.source_bytes += t.size;
    }
    p.frames = p.source_frames;
    if (p.kind == EditKind::Wav) {
        p.estimate_bytes = kDataOffset + p.source_frames * p.takes.front().block_align;
    } else if (p.kind == EditKind::Flac) {
        p.estimate_bytes = p.source_bytes; /* the same audio at the same settings: about the same size */
    } else {
        /* Opus: the packets kept, the joins (merge.hpp) */
        int64_t kept48 = 0;
        uint64_t headers = 0;
        int16_t gain = 0;
        for (size_t i = 0; i < p.takes.size(); ++i) {
            oggopus::OggOpusFile f(ops);
            std::string why;
            int r = f.open(req.sources[i], &why);
            if (r < 0) return plan_fail(p, MergeError::BadSources, base_of(req.sources[i]) + ": " + (why.empty() ? decode_error_text(r, req.sources[i]) : why));
            if (i == 0) gain = f.head().gain_q8;
            else if (f.head().gain_q8 != gain)
                return plan_fail(p, MergeError::Incompatible, base_of(req.sources[i]) + ": a different Opus output gain");
            OpusEnds e;
            r = opus_ends(f, i > 0, e);
            if (r < 0) return plan_fail(p, MergeError::BadSources, base_of(req.sources[i]) + ": the take is damaged or empty");
            p.opus_drop.push_back(e.drop);
            p.opus_lead48.push_back(f.start48() + static_cast<int64_t>(f.head().preskip) - e.first_start48);
            p.opus_tail48.push_back(e.last_end48 - f.end48());
            kept48 += e.last_end48 - e.first_start48;
            headers += f.audio_offset();
        }
        oggopus::Timeline tl;
        tl.start48 = 0;
        tl.preskip = static_cast<uint32_t>(p.opus_lead48.front()); /* the first take keeps its pre-skip, all of it */
        tl.k = p.takes.front().opus_k;
        const int64_t end48 = kept48 - p.opus_tail48.back();
        p.frames = tl.length(end48);
        p.seam_frames = p.frames > p.source_frames ? p.frames - p.source_frames : 0;
        p.estimate_bytes = p.source_bytes > headers ? p.source_bytes - headers + p.takes.front().data_offset : p.source_bytes;
    }
    p.need_bytes = p.source_bytes + (p.source_bytes * (opt.space_percent > 100 ? opt.space_percent - 100 : 0) + 99) / 100;
    uint64_t avail = 0;
    const int r = ops.free_space(p.dir, avail);
    if (r < 0) return plan_fail(p, MergeError::Io, errtxt("checking free space", r));
    if (avail < p.need_bytes) return plan_fail(p, MergeError::NoSpace, "Not enough free space");
    p.ok = true;
    return p;
}

namespace {

class MergeJob {
public:
    MergeJob(const MergeRequest &r, const MergeOptions &o) : req(r), opt(o), ops(o.ops ? *o.ops : posix_file_ops()) {}
    MergeResult run();

private:
    const MergeRequest &req;
    const MergeOptions &opt;
    FileOps &ops;
    MergePlan plan;
    MergeResult res;
    std::string tmp;
    uint64_t done = 0;

    bool cancelled() const { return opt.cancel && opt.cancel->load(); }
    void tick(uint64_t frames) {
        done = std::min(plan.frames, done + frames);
        if (opt.progress) opt.progress(done, plan.frames);
    }
    MergeResult abort_with(MergeError e, const std::string &msg) {
        if (!tmp.empty()) ops.unlink(tmp);
        tmp.clear();
        return result_fail(res, e, msg);
    }
    Tags tags() const;
    int write_wav(ByteSink &sink);
    int write_flac(ByteSink &sink);
    int write_opus(ByteSink &sink);
    std::string verify(const std::string &path) const;
};

Tags MergeJob::tags() const {
    if (!req.tags.empty()) return req.tags;
    return plan.takes.front().tags;
}

int MergeJob::write_wav(ByteSink &sink) {
    const uint32_t ba = plan.takes.front().block_align;
    const std::vector<unsigned char> hdr = build_header(plan.fmt, req.bwf, plan.frames * ba);
    int r = sink.write(hdr.data(), hdr.size());
    if (r < 0) return r;
    const size_t block = std::max<size_t>(ba, opt.block_bytes / ba * ba);
    std::vector<unsigned char> buf(block);
    for (size_t i = 0; i < plan.takes.size(); ++i) {
        const EditTakeInfo &t = plan.takes[i];
        const int fd = ops.open(req.sources[i], O_RDONLY, 0);
        if (fd < 0) return fd;
        FdGuard in(ops, fd);
        const uint64_t bytes = t.frames * ba;
        uint64_t off = 0;
        while (off < bytes) {
            if (cancelled()) return -ECANCELED;
            const size_t n = static_cast<size_t>(std::min<uint64_t>(buf.size(), bytes - off));
            const int64_t got = pread_full(ops, fd, buf.data(), n, t.data_offset + off);
            if (got < 0) return static_cast<int>(got);
            if (static_cast<size_t>(got) != n) return -ESTALE; /* the take is shorter than it was */
            r = sink.write(buf.data(), n);
            if (r < 0) return r;
            off += n;
            tick(n / ba);
        }
    }
    return 0;
}

int MergeJob::write_flac(ByteSink &sink) {
    DecoderLibs real;
    if (!opt.decoders) real = real_decoder_libs();
    const DecoderLibs &libs = opt.decoders ? *opt.decoders : real;
    std::unique_ptr<Encoder> enc;
    if (opt.make_flac_encoder) {
        enc = opt.make_flac_encoder(plan.fmt);
    } else {
        FlacConfig fc;
        fc.fmt = plan.fmt;
        fc.level = 5;
        enc.reset(new FlacEncoder(real_flac(), fc));
    }
    if (!enc) return kCodecMissing;
    int r = enc->start(sink, tags());
    if (r < 0) return r;
    const size_t align = static_cast<size_t>(plan.fmt.channels) * (plan.fmt.bits / 8u);
    const size_t chunk = std::max<size_t>(1024, opt.block_bytes / std::max<size_t>(align, 1));
    std::vector<unsigned char> buf(chunk * align);
    for (size_t i = 0; i < plan.takes.size(); ++i) {
        DecodedInfo di;
        int err = 0;
        std::unique_ptr<Decoder> dec = make_decoder(ops, req.sources[i], libs, di, &err);
        if (!dec) return err < 0 ? err : kCodecBadStream;
        if (di.total_frames != plan.takes[i].frames || di.output.rate != plan.fmt.rate ||
            di.output.channels != plan.fmt.channels || di.output.bits != plan.fmt.bits)
            return -ESTALE;
        uint64_t left = di.total_frames;
        while (left > 0) {
            if (cancelled()) return -ECANCELED;
            const size_t n = static_cast<size_t>(std::min<uint64_t>(chunk, left));
            const long got = dec->read(buf.data(), n);
            if (got < 0) return static_cast<int>(got);
            if (got == 0) return -ESTALE; /* the take is shorter than it said */
            r = enc->write(buf.data(), static_cast<size_t>(got));
            if (r < 0) return r;
            left -= static_cast<uint64_t>(got);
            tick(static_cast<uint64_t>(got));
        }
    }
    r = enc->finish();
    if (r < 0) return r;
    if (enc->stats().frames_in != plan.frames) return -ESTALE;
    return 0;
}

int MergeJob::write_opus(ByteSink &sink) {
    oggopus::OggOpusFile first(ops);
    int r = first.open(req.sources.front());
    if (r < 0) return r;
    const uint32_t serial = first.serial() ^ 0x4D455247u; /* "MERG": any number other than the first take's */
    uint32_t seq = 0;
    auto out = [&](const std::vector<unsigned char> &page) { return sink.write(page.data(), page.size()); };
    oggopus::Head h = first.head(); /* the first take's pre-skip, channels, input rate and gain */
    r = out(oggopus::build_page(0x02, 0, serial, seq++, {oggopus::build_head(h)}));
    if (r < 0) return r;
    std::vector<unsigned char> tb = oggopus::build_tags(first.vendor(), tags());
    if (tb.size() > 255u * 255u - 1u) tb = oggopus::build_tags(first.vendor(), Tags()); /* a comment block this large: dropped */
    r = out(oggopus::build_page(0, 0, serial, seq++, {tb}));
    if (r < 0) return r;
    first.close();

    const unsigned k = plan.takes.front().opus_k ? plan.takes.front().opus_k : 1;
    const uint32_t preskip = static_cast<uint32_t>(plan.opus_lead48.front());
    std::vector<std::vector<unsigned char>> page;
    size_t segs = 0;
    int64_t pos48 = 0, page_start = 0;
    uint64_t done48 = 0;
    auto shown = [&](int64_t at48) {
        const int64_t f = (at48 - static_cast<int64_t>(preskip)) / static_cast<int64_t>(k);
        return f > 0 ? static_cast<uint64_t>(f) : 0;
    };
    for (size_t i = 0; i < plan.takes.size(); ++i) {
        oggopus::OggOpusFile f(ops);
        r = f.open(req.sources[i]);
        if (r < 0) return r;
        r = f.rewind();
        if (r < 0) return r;
        const int64_t want48 = (f.end48() + plan.opus_tail48[i]) -
                               (f.start48() + static_cast<int64_t>(f.head().preskip) - plan.opus_lead48[i]);
        int64_t got48 = 0;
        uint64_t skipped = 0;
        oggopus::Packet q;
        for (;;) {
            if (cancelled()) return -ECANCELED;
            r = f.next(q);
            if (r < 0) return r;
            if (r == 0) break;
            if (skipped < plan.opus_drop[i]) {
                skipped++;
                continue;
            }
            const uint32_t dur = static_cast<uint32_t>(q.end48 - q.start48);
            const size_t need = q.data.size() / 255 + 1;
            if (!page.empty() && (segs + need > 255 || pos48 - page_start >= 48000)) {
                r = out(oggopus::build_page(0, static_cast<uint64_t>(pos48), serial, seq++, page));
                if (r < 0) return r;
                page.clear();
                segs = 0;
                page_start = pos48;
            }
            page.push_back(std::move(q.data));
            segs += need;
            pos48 += dur;
            got48 += dur;
            const uint64_t now = std::min<uint64_t>(shown(pos48), plan.frames);
            if (now > done48) {
                tick(now - done48);
                done48 = now;
            }
        }
        if (got48 != want48) return kCodecBadStream; /* a packet is missing (a damaged page): never a take with a hole */
    }
    if (page.empty()) return kCodecBadStream;
    const int64_t end48 = pos48 - plan.opus_tail48.back();
    return out(oggopus::build_page(0x04, static_cast<uint64_t>(end48), serial, seq++, page));
}

/* "" when the written file is what the plan says: the format and the length. */
std::string MergeJob::verify(const std::string &path) const {
    if (plan.kind == EditKind::Wav) {
        const HeaderInfo h = parse_header(ops, path);
        if (!h.ok) return "the new take cannot be read back: " + h.error;
        if (h.fmt.rate != plan.fmt.rate || h.fmt.channels != plan.fmt.channels || h.fmt.bits != plan.fmt.bits)
            return "the new take has another format";
        if (!h.consistent() || h.declared_data != plan.frames * h.block_align || h.actual_data != h.declared_data)
            return "the new take does not hold the sum of the takes";
        return "";
    }
    if (plan.kind == EditKind::Flac) {
        const StreamInfo si = read_stream_info(ops, path);
        if (!si.ok) return "the new take cannot be read back: " + si.error;
        if (!si.frames_known || si.frames != plan.frames) return "the new take does not hold the sum of the takes";
        if (si.fmt.rate != plan.fmt.rate || si.fmt.channels != plan.fmt.channels) return "the new take has another format";
        return "";
    }
    oggopus::OggOpusFile f(ops);
    std::string why;
    const int r = f.open(path, &why);
    if (r < 0) return "the new take cannot be read back: " + (why.empty() ? decode_error_text(r, path) : why);
    oggopus::Timeline tl;
    tl.start48 = f.start48();
    tl.preskip = f.head().preskip;
    tl.k = plan.takes.front().opus_k ? plan.takes.front().opus_k : 1;
    if (!f.eos() || f.head().channels != plan.fmt.channels || tl.length(f.end48()) != plan.frames)
        return "the new take does not hold the sum of the takes";
    return "";
}

MergeResult MergeJob::run() {
    plan = plan_merge(req, opt);
    if (!plan.ok) return result_fail(res, plan.error, plan.message);
    if (opt.progress) opt.progress(0, plan.frames);
    if (cancelled()) return result_fail(res, MergeError::Cancelled, "cancelled");
    tmp = tmp_name(plan.dir, "merge-");
    const int fd = ops.open(tmp, O_WRONLY | O_CREAT | O_EXCL, 0644);
    if (fd < 0) {
        tmp.clear();
        return result_fail(res, fd == -ENOSPC ? MergeError::NoSpace : MergeError::Io, errtxt("creating the new take", fd));
    }
    int r = 0;
    {
        FdGuard g(ops, fd);
        FileByteSink sink(ops, fd, 0);
        r = plan.kind == EditKind::Wav ? write_wav(sink) : plan.kind == EditKind::Flac ? write_flac(sink) : write_opus(sink);
        if (r == 0) r = ops.fdatasync(fd);
        if (r == 0) r = ops.close(g.release());
    }
    if (r == -ECANCELED || (r == 0 && cancelled())) return abort_with(MergeError::Cancelled, "cancelled");
    if (r == -ESTALE) return abort_with(MergeError::SourceChanged, "a take changed while it was being merged");
    if (codec_err(r)) return abort_with(MergeError::Io, "cannot write the new take: " + decode_error_text(r, req.sources.front()));
    if (r < 0) {
        const bool full = r == -ENOSPC || r == -EDQUOT;
        return abort_with(full ? MergeError::NoSpace : MergeError::Io, full ? "the disk is full" : errtxt("writing the new take", r));
    }
    /* read back: the format and the whole length, before the takes are trusted to it */
    const std::string bad = verify(tmp);
    if (!bad.empty()) return abort_with(MergeError::Verify, bad);
    /* the takes must be what we read */
    for (size_t i = 0; i < plan.takes.size(); ++i) {
        StatInfo st;
        const int s = ops.stat(req.sources[i], st);
        if (s < 0 || st.size != plan.takes[i].size || st.mtime != plan.takes[i].mtime)
            return abort_with(MergeError::SourceChanged, "a take changed while it was being merged");
    }
    if (cancelled()) return abort_with(MergeError::Cancelled, "cancelled");
    const std::string final_path = plan.dir + "/" + req.output_name;
    StatInfo st;
    if (ops.stat(final_path, st) == 0) return abort_with(MergeError::BadName, "a file named " + req.output_name + " exists");
    r = ops.rename(tmp, final_path);
    if (r < 0) return abort_with(MergeError::Io, errtxt("rename", r));
    tmp.clear();
    r = ops.fsync_dir(plan.dir);
    if (r < 0) {
        ops.unlink(final_path); /* not durable: as if it had not happened (the takes are untouched) */
        return result_fail(res, MergeError::Io, errtxt("syncing the folder", r));
    }
    res.ok = true;
    res.error = MergeError::None;
    res.output = final_path;
    res.frames = plan.frames;
    res.seam_frames = plan.seam_frames;
    if (opt.on_committed) opt.on_committed(final_path);
    if (req.delete_originals) {
        int failed = 0;
        for (const std::string &s : req.sources) {
            int d = 0;
            if (opt.delete_original) {
                d = opt.delete_original(s);
            } else {
                d = ops.unlink(s);
                if (d == 0) ops.unlink(PeakPyramid::cache_path(s));
            }
            if (d < 0) failed++;
            else res.deleted.push_back(s);
        }
        const int f = ops.fsync_dir(plan.dir);
        if (failed) res.warning = std::to_string(failed) + (failed == 1 ? " original could not be deleted" : " originals could not be deleted");
        else if (f < 0) res.warning = errtxt("syncing the folder", f);
    }
    return res;
}

}  // namespace

MergeResult merge_takes(const MergeRequest &req, const MergeOptions &opt) {
    MergeJob j(req, opt);
    return j.run();
}

}  // namespace cassette
