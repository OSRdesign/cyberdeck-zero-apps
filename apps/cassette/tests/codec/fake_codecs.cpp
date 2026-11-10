/* SPDX-License-Identifier: MIT */
#include "fake_codecs.hpp"

#include <cstdarg>
#include <cstdlib>
#include <cstring>
#include <deque>

using namespace cassette;

namespace fake {

/* ---------------------------------------------------------------- FLAC */

namespace {

struct FakeMeta {
    flac::MetadataHead head; /* first, as in FLAC__StreamMetadata */
    std::vector<std::string> comments;
};

struct FakeFlac {
    flac::WriteCb w = nullptr;
    flac::SeekCb s = nullptr;
    flac::TellCb t = nullptr;
    void *client = nullptr;
    bool initialized = false;
    int state = flac::kStateUninitialized;
    std::vector<flac::Metadata *> meta;
    uint64_t pending = 0, total = 0;
};

FlacState g_flac;

FakeFlac *ff(const flac::Encoder *e) { return reinterpret_cast<FakeFlac *>(const_cast<flac::Encoder *>(e)); }
flac::Encoder *fe(FakeFlac *f) { return reinterpret_cast<flac::Encoder *>(f); }

bool emit(FakeFlac *f, const std::vector<unsigned char> &b, uint32_t samples) {
    if (f->w(fe(f), b.data(), b.size(), samples, 0, f->client) != flac::kWriteOk) {
        f->state = flac::kStateClientError;
        return false;
    }
    return true;
}

bool emit_frame(FakeFlac *f, uint64_t n) {
    std::vector<unsigned char> b = {'F', 'R', 'A', 'M'};
    for (int i = 0; i < 4; ++i) b.push_back(static_cast<unsigned char>((n >> (8 * i)) & 0xFF));
    b.resize(8 + n * g_flac.channels, 0x5A); /* one byte a sample */
    if (!emit(f, b, static_cast<uint32_t>(n))) return false;
    g_flac.frames_emitted += n;
    return true;
}

bool flac_finish_impl(FakeFlac *f) {
    if (!f->initialized) return true;
    f->initialized = false;
    g_flac.finishes++;
    if (f->state != flac::kStateOk) return false;
    if (f->pending && !emit_frame(f, f->pending)) return false;
    f->pending = 0;
    /* rewrite STREAMINFO: in the fake, the first 8 bytes of its body hold the total (offset 4 + 4) */
    if (f->s(fe(f), 8, f->client) != flac::kSeekOk) {
        f->state = flac::kStateClientError;
        return false;
    }
    std::vector<unsigned char> t;
    for (int i = 0; i < 8; ++i) t.push_back(static_cast<unsigned char>((f->total >> (8 * i)) & 0xFF));
    return emit(f, t, 0);
}

flac::Encoder *flac_new() {
    if (g_flac.fail_new) return nullptr;
    g_flac.news++;
    return fe(new FakeFlac());
}
void flac_delete(flac::Encoder *e) {
    FakeFlac *f = ff(e);
    if (f->initialized) { /* libFLAC finishes an unfinished stream on delete */
        if (!flac_finish_impl(f)) g_flac.wrote_after_delete_refused = true;
    }
    g_flac.deletes++;
    delete f;
}
flac::Bool set_verify(flac::Encoder *e, flac::Bool v) {
    if (ff(e)->initialized) return 0;
    g_flac.verify = v;
    return 1;
}
flac::Bool set_subset(flac::Encoder *e, flac::Bool v) {
    if (ff(e)->initialized) return 0;
    g_flac.subset = v;
    return 1;
}
flac::Bool set_channels(flac::Encoder *e, uint32_t v) {
    if (ff(e)->initialized) return 0;
    g_flac.channels = v;
    return 1;
}
flac::Bool set_bits(flac::Encoder *e, uint32_t v) {
    if (ff(e)->initialized) return 0;
    g_flac.bits = v;
    return 1;
}
flac::Bool set_rate(flac::Encoder *e, uint32_t v) {
    if (ff(e)->initialized) return 0;
    g_flac.rate = v;
    return 1;
}
flac::Bool set_level(flac::Encoder *e, uint32_t v) {
    if (ff(e)->initialized) return 0;
    g_flac.level = v;
    return 1;
}
flac::Bool set_total(flac::Encoder *e, uint64_t) { return ff(e)->initialized ? 0 : 1; }
flac::Bool set_metadata(flac::Encoder *e, flac::Metadata **m, uint32_t n) {
    FakeFlac *f = ff(e);
    if (f->initialized) return 0;
    f->meta.assign(m, m + n);
    g_flac.metadata_blocks = n;
    return 1;
}
int init_stream(flac::Encoder *e, flac::WriteCb w, flac::SeekCb s, flac::TellCb t, flac::MetadataCb, void *client) {
    FakeFlac *f = ff(e);
    g_flac.inits++;
    if (f->initialized) return flac::kInitAlreadyInitialized;
    if (g_flac.refuse_init) return g_flac.refuse_init;
    if (!w) return flac::kInitInvalidCallbacks;
    f->w = w;
    f->s = s;
    f->t = t;
    f->client = client;
    f->state = flac::kStateOk;
    if (!emit(f, {'f', 'L', 'a', 'C'}, 0)) return flac::kInitEncoderError;
    std::vector<unsigned char> si = {0, 0, 0, 34};
    si.resize(4 + 34, 0);
    if (!emit(f, si, 0)) return flac::kInitEncoderError;
    for (size_t i = 0; i < f->meta.size(); ++i) {
        FakeMeta *m = reinterpret_cast<FakeMeta *>(f->meta[i]);
        std::vector<unsigned char> body;
        if (m->head.type == flac::kMetaVorbisComment) {
            g_flac.comments = m->comments;
            for (const std::string &c : m->comments) {
                body.insert(body.end(), c.begin(), c.end());
                body.push_back('\n');
            }
        } else if (m->head.type == flac::kMetaPadding) {
            g_flac.padding = m->head.length;
            body.assign(m->head.length, 0);
        }
        const uint32_t len = static_cast<uint32_t>(body.size());
        std::vector<unsigned char> b = {static_cast<unsigned char>(m->head.type | (i + 1 == f->meta.size() ? 0x80 : 0)),
                                        static_cast<unsigned char>(len >> 16), static_cast<unsigned char>(len >> 8),
                                        static_cast<unsigned char>(len)};
        b.insert(b.end(), body.begin(), body.end());
        if (!emit(f, b, 0)) return flac::kInitEncoderError;
    }
    f->initialized = true;
    return flac::kInitOk;
}
flac::Bool process_interleaved(flac::Encoder *e, const int32_t *buf, uint32_t n) {
    FakeFlac *f = ff(e);
    const int call = g_flac.process_calls++;
    if (!f->initialized || f->state != flac::kStateOk) return 0;
    if (call == g_flac.fail_process_at) {
        f->state = flac::kStateFramingError;
        return 0;
    }
    g_flac.samples.insert(g_flac.samples.end(), buf, buf + static_cast<size_t>(n) * g_flac.channels);
    f->pending += n;
    f->total += n;
    while (f->pending >= g_flac.blocksize) {
        if (!emit_frame(f, g_flac.blocksize)) return 0;
        f->pending -= g_flac.blocksize;
    }
    return 1;
}
flac::Bool flac_finish(flac::Encoder *e) { return flac_finish_impl(ff(e)) ? 1 : 0; }
int get_state(const flac::Encoder *e) { return ff(e)->state; }
flac::Metadata *meta_new(int type) {
    FakeMeta *m = new FakeMeta();
    m->head.type = type;
    m->head.is_last = 0;
    m->head.length = 0;
    g_flac.meta_alive++;
    return reinterpret_cast<flac::Metadata *>(m);
}
void meta_delete(flac::Metadata *m) {
    g_flac.meta_alive--;
    delete reinterpret_cast<FakeMeta *>(m);
}
flac::Bool vc_from_pair(flac::VorbisCommentEntry *e, const char *name, const char *value) {
    const std::string s = std::string(name) + "=" + value;
    e->entry = static_cast<uint8_t *>(std::malloc(s.size() + 1));
    std::memcpy(e->entry, s.c_str(), s.size() + 1);
    e->length = static_cast<uint32_t>(s.size());
    return 1;
}
flac::Bool vc_append(flac::Metadata *m, flac::VorbisCommentEntry e, flac::Bool copy) {
    reinterpret_cast<FakeMeta *>(m)->comments.emplace_back(reinterpret_cast<const char *>(e.entry), e.length);
    if (!copy) std::free(e.entry);
    return 1;
}

}  // namespace

FlacState &flac_state() { return g_flac; }

FlacLib make_flac_lib() {
    g_flac = FlacState();
    FlacLib l;
    l.name = "fake";
    l.version = "fake";
    l.encoder_new = flac_new;
    l.encoder_delete = flac_delete;
    l.set_verify = set_verify;
    l.set_streamable_subset = set_subset;
    l.set_channels = set_channels;
    l.set_bits_per_sample = set_bits;
    l.set_sample_rate = set_rate;
    l.set_compression_level = set_level;
    l.set_total_samples_estimate = set_total;
    l.set_metadata = set_metadata;
    l.init_stream = init_stream;
    l.process_interleaved = process_interleaved;
    l.finish = flac_finish;
    l.get_state = get_state;
    l.metadata_new = meta_new;
    l.metadata_delete = meta_delete;
    l.vc_entry_from_pair = vc_from_pair;
    l.vc_append_comment = vc_append;
    return l;
}

/* ---------------------------------------------------------------- Opus */

namespace {

struct FakeComments {
    std::vector<std::string> list;
};

struct FakeOpe {
    int32_t rate = 48000;
    int ch = 1;
    int decision = 96000, muxing = 48000;
    uint64_t in_frames = 0, enc48 = 0, page_start48 = 0;
    int page_packets = 0;
    size_t page_bytes = 0;
    uint32_t seq = 0;
    bool header_done = false;
    std::deque<std::vector<unsigned char>> queue;
    std::vector<unsigned char> current;
};

OpusState g_opus;

FakeOpe *fo(ope::Enc *e) { return reinterpret_cast<FakeOpe *>(e); }

void le(std::vector<unsigned char> &b, uint64_t v, int n) {
    for (int i = 0; i < n; ++i) b.push_back(static_cast<unsigned char>((v >> (8 * i)) & 0xFF));
}

void make_page(FakeOpe *o, unsigned char type, uint64_t granule, const std::vector<unsigned char> &body) {
    std::vector<unsigned char> p = {'O', 'g', 'g', 'S', 0, type};
    le(p, granule, 8);
    le(p, 1234, 4);
    le(p, o->seq++, 4);
    le(p, 0, 4); /* CRC: not computed by the fake */
    p.push_back(1);
    p.push_back(static_cast<unsigned char>(body.size() > 255 ? 255 : body.size()));
    p.insert(p.end(), body.begin(), body.end());
    o->queue.push_back(p);
    g_opus.pages_made++;
}

void close_page(FakeOpe *o) {
    if (o->page_packets == 0) return;
    make_page(o, 0, o->enc48 + g_opus.preskip, std::vector<unsigned char>(o->page_bytes, 0xAB));
    o->page_start48 = o->enc48;
    o->page_packets = 0;
    o->page_bytes = 0;
}

void packet(FakeOpe *o) {
    const int bitrate = g_opus.ctl.count(ope::kSetBitrate) ? g_opus.ctl[ope::kSetBitrate] : 32000;
    o->page_packets++;
    o->page_bytes += static_cast<size_t>(bitrate / 8 / 50);
    o->enc48 += 960;
    if (o->enc48 - o->page_start48 >= static_cast<uint64_t>(o->muxing)) close_page(o);
}

void header(FakeOpe *o) {
    if (o->header_done) return;
    o->header_done = true;
    g_opus.header_flushes++;
    std::vector<unsigned char> head = {'O', 'p', 'u', 's', 'H', 'e', 'a', 'd', 1, static_cast<unsigned char>(o->ch)};
    le(head, g_opus.preskip, 2);
    le(head, static_cast<uint64_t>(o->rate), 4);
    le(head, 0, 2);
    head.push_back(0);
    make_page(o, 0x02, 0, head);
    std::vector<unsigned char> tags = {'O', 'p', 'u', 's', 'T', 'a', 'g', 's'};
    for (const std::string &c : g_opus.comments) {
        tags.insert(tags.end(), c.begin(), c.end());
        tags.push_back('\n');
    }
    make_page(o, 0, 0, tags);
}

void encode(FakeOpe *o) {
    const uint64_t in48 = o->in_frames * 48000u / static_cast<uint64_t>(o->rate);
    while (in48 > o->enc48 && in48 - o->enc48 > static_cast<uint64_t>(960 + o->decision)) packet(o);
}

ope::Comments *c_create() { return reinterpret_cast<ope::Comments *>(new FakeComments()); }
int c_add(ope::Comments *c, const char *tag, const char *val) {
    if (std::strchr(tag, '=')) return ope::kBadArg;
    reinterpret_cast<FakeComments *>(c)->list.push_back(std::string(tag) + "=" + val);
    return ope::kOk;
}
void c_destroy(ope::Comments *c) { delete reinterpret_cast<FakeComments *>(c); }

ope::Enc *create_pull(ope::Comments *c, int32_t rate, int channels, int family, int *error) {
    if (rate < 8000 || rate > 96000 || channels < 1 || channels > 2 || family != 0) {
        if (error) *error = ope::kBadArg;
        return nullptr;
    }
    g_opus.creates++;
    g_opus.rate = rate;
    g_opus.channels = channels;
    g_opus.family = family;
    g_opus.comments = reinterpret_cast<FakeComments *>(c)->list;
    FakeOpe *o = new FakeOpe();
    o->rate = rate;
    o->ch = channels;
    if (error) *error = ope::kOk;
    return reinterpret_cast<ope::Enc *>(o);
}
int e_write(ope::Enc *e, const int16_t *pcm, int n) {
    if (g_opus.writes++ == g_opus.fail_write_at) return ope::kInternalError;
    FakeOpe *o = fo(e);
    header(o);
    g_opus.s16.insert(g_opus.s16.end(), pcm, pcm + static_cast<size_t>(n) * o->ch);
    o->in_frames += static_cast<uint64_t>(n);
    encode(o);
    return ope::kOk;
}
int e_write_float(ope::Enc *e, const float *pcm, int n) {
    if (g_opus.writes++ == g_opus.fail_write_at) return ope::kInternalError;
    FakeOpe *o = fo(e);
    header(o);
    g_opus.f32.insert(g_opus.f32.end(), pcm, pcm + static_cast<size_t>(n) * o->ch);
    o->in_frames += static_cast<uint64_t>(n);
    encode(o);
    return ope::kOk;
}
int get_page(ope::Enc *e, unsigned char **page, int32_t *len, int flush) {
    FakeOpe *o = fo(e);
    if (flush) close_page(o);
    if (o->queue.empty()) return 0;
    o->current = o->queue.front();
    o->queue.pop_front();
    *page = o->current.data();
    *len = static_cast<int32_t>(o->current.size());
    return 1;
}
int drain(ope::Enc *e) {
    FakeOpe *o = fo(e);
    g_opus.drains++;
    header(o);
    const uint64_t total48 = (o->in_frames * 48000u + static_cast<uint64_t>(o->rate) - 1) / static_cast<uint64_t>(o->rate);
    while (o->enc48 < total48) packet(o);
    o->enc48 = total48;
    make_page(o, 0x04, total48 + g_opus.preskip, std::vector<unsigned char>(o->page_bytes + 1, 0xAB));
    o->page_packets = 0;
    o->page_bytes = 0;
    return ope::kOk;
}
void destroy(ope::Enc *e) {
    g_opus.destroys++;
    delete fo(e);
}
int flush_header(ope::Enc *e) {
    FakeOpe *o = fo(e);
    if (o->header_done) return ope::kTooLate;
    header(o);
    return ope::kOk;
}
int ctl(ope::Enc *e, int request, ...) {
    va_list ap;
    va_start(ap, request);
    const int32_t v = va_arg(ap, int32_t);
    va_end(ap);
    if (request == g_opus.fail_ctl_request) return -1; /* OPUS_BAD_ARG */
    switch (request) {
    case ope::kSetApplication: case ope::kSetBitrate: case ope::kSetVbr: case ope::kSetComplexity:
    case ope::kSetForceChannels: case ope::kSetSignal: case ope::kSetExpertFrameDuration: case ope::kSetSerialno:
    case ope::kSetCommentPadding: break;
    case ope::kSetDecisionDelay: fo(e)->decision = v; break;
    case ope::kSetMuxingDelay: fo(e)->muxing = v; break;
    default: return ope::kUnimplemented;
    }
    g_opus.ctl[request] = v;
    return ope::kOk;
}
const char *str_error(int) { return "fake"; }

}  // namespace

OpusState &opus_state() { return g_opus; }

OpusEncLib make_opus_lib() {
    g_opus = OpusState();
    OpusEncLib l;
    l.name = "fake";
    l.version = "fake";
    l.comments_create = c_create;
    l.comments_add = c_add;
    l.comments_destroy = c_destroy;
    l.encoder_create_pull = create_pull;
    l.encoder_write = e_write;
    l.encoder_write_float = e_write_float;
    l.encoder_get_page = get_page;
    l.encoder_drain = drain;
    l.encoder_destroy = destroy;
    l.encoder_flush_header = flush_header;
    l.encoder_ctl = ctl;
    l.strerror = str_error;
    return l;
}

}  // namespace fake
