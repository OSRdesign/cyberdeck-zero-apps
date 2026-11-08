/*
 * SPDX-License-Identifier: MIT
 *
 * Cassette edit tests: trim and split (byte-exact against a reference sliced from the source samples, names, headers, bext,
 * RF64 boundary with mocked sizes, refusals, cancel, disk full, replace mode, ordering of the file operations).
 */
#include "check.hpp"
#include "helpers.hpp"

#include <atomic>
#include <unistd.h>

using namespace tst;

namespace {

const char *kDir = "/rec";
const char *kSrc = "take_006_20261108-1105.wav";

struct Env {
    MemOps m;
    FakeNames nm;
    SrcSpec spec;
    Bytes src;
    std::string path = std::string(kDir) + "/" + kSrc;
    std::vector<uint64_t> progress;
    uint64_t progress_total = 0;
    std::vector<std::string> deleted;

    explicit Env(const SrcSpec &s = SrcSpec()) : spec(s) {
        src = make_source(spec);
        m.put(path, src);
    }
    EditOptions opts() {
        EditOptions o;
        o.ops = &m;
        o.names = [this](const std::string &s, int c) { return nm(s, c); };
        o.on_original_deleted = [this](const std::string &p) { deleted.push_back(p); };
        o.progress = [this](uint64_t d, uint64_t t) {
            progress.push_back(d);
            progress_total = t;
        };
        o.block_bytes = 4096;
        return o;
    }
    EditResult run(uint64_t in, uint64_t out, std::vector<uint64_t> splits = {}, SaveMode mode = SaveMode::NewFiles) {
        EditRequest r;
        r.source_path = path;
        r.in_frame = in;
        r.out_frame = out;
        r.splits = std::move(splits);
        r.mode = mode;
        return trim_split(r, opts());
    }
    size_t data_off() const { return spec.kind == SrcSpec::Arecord44 ? 44 : kDataOffset; }
    /* The audio bytes of [start, start+frames) sliced straight from the source file. */
    Bytes slice(uint64_t start, uint64_t frames) const {
        const size_t ba = block_align(spec.fmt);
        const size_t o = data_off() + static_cast<size_t>(start) * ba;
        return Bytes(src.begin() + static_cast<std::ptrdiff_t>(o), src.begin() + static_cast<std::ptrdiff_t>(o + frames * ba));
    }
    bool unchanged() const { return m.get(path) == src; }
    std::vector<std::string> tmp_files() const {
        std::vector<std::string> v;
        for (const std::string &n : m.names(kDir))
            if (n.compare(0, 5, ".tmp-") == 0) v.push_back(n);
        return v;
    }
};

uint64_t bext_tref(const Bytes &f, const HeaderInfo &h) {
    const unsigned char *p = &f[static_cast<size_t>(h.bext_offset)];
    return static_cast<uint64_t>(p[338]) | static_cast<uint64_t>(p[339]) << 8 | static_cast<uint64_t>(p[340]) << 16 |
           static_cast<uint64_t>(p[341]) << 24 | static_cast<uint64_t>(p[342]) << 32 | static_cast<uint64_t>(p[343]) << 40 |
           static_cast<uint64_t>(p[344]) << 48 | static_cast<uint64_t>(p[345]) << 56;
}
std::string field(const Bytes &f, size_t off, size_t max) {
    size_t n = 0;
    while (n < max && f[off + n]) n++;
    return std::string(reinterpret_cast<const char *>(&f[off]), n);
}

/* Checks one result file against the reference: header, bext, iXML and every audio byte. */
void verify_part(Env &e, const std::string &name, uint64_t start, uint64_t frames, int part, int parts) {
    const std::string p = std::string(kDir) + "/" + name;
    CHECK(e.m.exists(p));
    const Bytes f = e.m.get(p);
    HeaderInfo h = parse_header(e.m, p); /* headers parse back */
    CHECK(h.ok);
    CHECK(h.consistent());
    CHECK(!h.suspicious());
    CHECK_EQ(h.fmt.rate, e.spec.fmt.rate);
    CHECK_EQ(h.fmt.channels, e.spec.fmt.channels);
    CHECK_EQ(h.fmt.bits, e.spec.fmt.bits);
    CHECK_EQ(h.data_offset, static_cast<uint64_t>(kDataOffset));
    const size_t ba = block_align(e.spec.fmt);
    CHECK_EQ(h.actual_data, frames * ba);
    CHECK_EQ(h.declared_data, frames * ba);
    CHECK(h.has_bext);
    CHECK(h.has_ixml);
    const Bytes want = e.slice(start, frames);
    CHECK_EQ(f.size(), kDataOffset + want.size());
    CHECK(std::equal(want.begin(), want.end(), f.begin() + kDataOffset)); /* byte exact */
    if (frames > 0) { /* the first and the last frame, the edges */
        CHECK(std::equal(want.begin(), want.begin() + static_cast<std::ptrdiff_t>(ba), f.begin() + kDataOffset));
        CHECK(std::equal(want.end() - static_cast<std::ptrdiff_t>(ba), want.end(), f.end() - static_cast<std::ptrdiff_t>(ba)));
    }
    /* bext: the time reference moved with the cut; the description names the source and the cut */
    const uint64_t src_tref = e.spec.kind == SrcSpec::Arecord44 ? 0 : e.spec.tref;
    CHECK_EQ(bext_tref(f, h), src_tref + start);
    const std::string desc = field(f, static_cast<size_t>(h.bext_offset), 256);
    CHECK(desc.find(kSrc) != std::string::npos);
    CHECK(desc.find("part " + std::to_string(part) + " of " + std::to_string(parts)) != std::string::npos);
    CHECK(desc.find("frames " + std::to_string(start) + "-" + std::to_string(start + frames)) != std::string::npos);
    CHECK_EQ(field(f, static_cast<size_t>(h.bext_offset) + 256, 32), "Cassette");
    if (e.spec.kind != SrcSpec::Arecord44) {
        CHECK_EQ(field(f, static_cast<size_t>(h.bext_offset) + 320, 10), "2026-11-08");
        CHECK_EQ(field(f, static_cast<size_t>(h.bext_offset) + 330, 8), "11:05:00");
        const std::string hist = field(f, static_cast<size_t>(h.bext_offset) + 602, 256);
        CHECK(hist.find("T=Cassette\r\n") != std::string::npos);      /* the source's own line stays */
        CHECK(hist.find("T=Cassette edit\r\n") != std::string::npos); /* and the edit is added */
    } else {
        CHECK(field(f, static_cast<size_t>(h.bext_offset) + 602, 256).find("T=Cassette edit") != std::string::npos);
    }
    /* iXML: well-formed padding, the new time reference, the original name */
    const std::string ix = field(f, static_cast<size_t>(h.ixml_offset), static_cast<size_t>(h.ixml_size));
    CHECK(ix.find("<BWFXML>") != std::string::npos && ix.find("</BWFXML>") != std::string::npos);
    CHECK(ix.find("<TIMESTAMP_SAMPLES_SINCE_MIDNIGHT_LO>" + std::to_string((src_tref + start) & 0xFFFFFFFFull)) != std::string::npos);
    CHECK(ix.find(std::string("<ORIGINAL_FILENAME>") + kSrc) != std::string::npos);
    CHECK_EQ(h.ixml_size, 5120u - h.ixml_offset - 8);
}

bool monotonic(const std::vector<uint64_t> &v) {
    for (size_t i = 1; i < v.size(); i++)
        if (v[i] < v[i - 1]) return false;
    return true;
}

void t_trim_alone() {
    Env e;
    EditResult r = e.run(1000, 9000);
    CHECK(r.ok);
    CHECK_EQ(r.outputs.size(), 1u);
    CHECK_EQ(r.outputs[0], std::string(kDir) + "/take_006_20261108-1105-1.wav");
    verify_part(e, "take_006_20261108-1105-1.wav", 1000, 8000, 1, 1);
    CHECK(e.unchanged()); /* the original stays */
    CHECK(e.tmp_files().empty());
    CHECK(!r.original_removed);
    CHECK_EQ(e.nm.calls.size(), 1u);
    CHECK_EQ(e.nm.calls[0], std::string(kSrc)); /* the provider sees the file name, not the path */
    CHECK(monotonic(e.progress));
    CHECK(!e.progress.empty() && e.progress.front() == 0 && e.progress.back() == e.progress_total);
    CHECK_EQ(e.progress_total, 8000u * 2);
    CHECK(e.deleted.empty());
}

void t_edges() {
    { /* whole take, first frame, last frame */
        Env e;
        CHECK(e.run(0, 12000).ok);
        verify_part(e, "take_006_20261108-1105-1.wav", 0, 12000, 1, 1);
    }
    { /* one frame only */
        Env e;
        CHECK(e.run(5, 6).ok);
        verify_part(e, "take_006_20261108-1105-1.wav", 5, 1, 1, 1);
    }
    { /* the last frame */
        Env e;
        CHECK(e.run(11999, 12000).ok);
        verify_part(e, "take_006_20261108-1105-1.wav", 11999, 1, 1, 1);
    }
}

void t_split_two_points() {
    Env e;
    EditResult r = e.run(500, 11000, {3000, 3001});
    CHECK(r.ok);
    CHECK_EQ(r.outputs.size(), 3u);
    const char *n[3] = {"take_006_20261108-1105-1.wav", "take_006_20261108-1105-2.wav", "take_006_20261108-1105-3.wav"};
    for (int i = 0; i < 3; i++) CHECK_EQ(r.outputs[static_cast<size_t>(i)], std::string(kDir) + "/" + n[i]);
    verify_part(e, n[0], 500, 2500, 1, 3);
    verify_part(e, n[1], 3000, 1, 2, 3); /* a part of exactly one frame */
    verify_part(e, n[2], 3001, 7999, 3, 3);
    CHECK(e.unchanged());
    CHECK(e.tmp_files().empty());
    CHECK_EQ(e.m.names(kDir).size(), 4u);
    CHECK(monotonic(e.progress));
    CHECK_EQ(e.progress.back(), e.progress_total);
    CHECK_EQ(e.progress_total, 10500u * 2);
    CHECK_EQ(e.nm.calls.size(), 1u);
    /* the numbers are never reused: a second edit gets -4 and -5 even when -2 was deleted meanwhile */
    e.m.files.erase(std::string(kDir) + "/" + n[1]);
    EditResult r2 = e.run(0, 100, {50});
    CHECK(r2.ok);
    CHECK_EQ(r2.outputs[0], std::string(kDir) + "/take_006_20261108-1105-4.wav");
    CHECK_EQ(r2.outputs[1], std::string(kDir) + "/take_006_20261108-1105-5.wav");
}

void t_formats() {
    struct F {
        uint16_t ch, bits;
    } fs[] = {{1, 16}, {2, 16}, {1, 24}, {2, 24}, {1, 32}, {2, 32}};
    for (const F &f : fs) {
        SrcSpec s;
        s.fmt.channels = f.ch;
        s.fmt.bits = f.bits;
        s.fmt.rate = f.ch == 1 ? 48000 : 44100;
        s.frames = 5000;
        Env e(s);
        EditResult r = e.run(100, 4900, {1234, 4000});
        CHECK(r.ok);
        CHECK_EQ(r.outputs.size(), 3u);
        verify_part(e, "take_006_20261108-1105-1.wav", 100, 1134, 1, 3);
        verify_part(e, "take_006_20261108-1105-2.wav", 1234, 2766, 2, 3);
        verify_part(e, "take_006_20261108-1105-3.wav", 4000, 900, 3, 3);
    }
}

void t_placeholder_sources() {
    for (SrcSpec::Kind k : {SrcSpec::Placeholder, SrcSpec::Arecord44}) {
        SrcSpec s;
        s.kind = k;
        s.frames = 7000;
        Env e(s);
        HeaderInfo h = parse_header(e.m, e.path);
        CHECK(h.ok && h.suspicious()); /* the source really is unrepaired */
        TakeInfo t = inspect_take(e.m, e.path);
        CHECK(t.ok);
        CHECK_EQ(t.frames, 7000u); /* the real file length counts */
        EditResult r = e.run(10, 6990, {3000});
        CHECK(r.ok);
        CHECK_EQ(r.outputs.size(), 2u);
        verify_part(e, "take_006_20261108-1105-1.wav", 10, 2990, 1, 2);
        verify_part(e, "take_006_20261108-1105-2.wav", 3000, 3990, 2, 2);
        CHECK(e.unchanged());
    }
    { /* OUT may not pass the real end */
        SrcSpec s;
        s.kind = SrcSpec::Placeholder;
        s.frames = 100;
        Env e(s);
        EditResult r = e.run(0, 101);
        CHECK(!r.ok && r.error == EditError::BadPositions);
    }
}

void t_time_reference() {
    SrcSpec s;
    s.tref = 0xFFFFFF00ull; /* crosses 32 bits */
    s.frames = 2000;
    Env e(s);
    CHECK(e.run(0, 2000, {1000}).ok);
    const Bytes f2 = e.m.get(std::string(kDir) + "/take_006_20261108-1105-2.wav");
    HeaderInfo h = parse_header(f2.data(), 65536, f2.size());
    CHECK_EQ(bext_tref(f2, h), 0xFFFFFF00ull + 1000);
    CHECK_EQ(bext_tref(f2, h) >> 32, 1u);
    const std::string ix = field(f2, static_cast<size_t>(h.ixml_offset), static_cast<size_t>(h.ixml_size));
    CHECK(ix.find("<TIMESTAMP_SAMPLES_SINCE_MIDNIGHT_HI>1<") != std::string::npos);
    CHECK(ix.find("<TIMESTAMP_SAMPLES_SINCE_MIDNIGHT_LO>" + std::to_string((0xFFFFFF00ull + 1000) & 0xFFFFFFFFull)) !=
          std::string::npos);
}

void t_refusals() {
    struct Case {
        const char *what;
        uint64_t in, out;
        std::vector<uint64_t> splits;
    } cases[] = {
        {"in == out", 100, 100, {}},
        {"in > out", 200, 100, {}},
        {"out past the end", 0, 12001, {}},
        {"in past the end", 12001, 12002, {}},
        {"split == in", 100, 5000, {100}},
        {"split == out", 100, 5000, {5000}},
        {"split before in", 100, 5000, {50}},
        {"split past out", 100, 5000, {6000}},
        {"splits equal", 100, 5000, {2000, 2000}},
        {"splits unsorted", 100, 5000, {3000, 2000}},
        {"a part shorter than a frame", 100, 5000, {2000, 2000, 2001}},
        {"zero frames", 0, 0, {}},
    };
    for (const Case &c : cases) {
        Env e;
        EditResult r = e.run(c.in, c.out, c.splits);
        if (r.ok || r.error != EditError::BadPositions) std::fprintf(stderr, "case: %s\n", c.what);
        CHECK(!r.ok);
        CHECK(r.error == EditError::BadPositions);
        CHECK(!r.message.empty());
        CHECK(r.outputs.empty());
        CHECK(e.unchanged());
        CHECK_EQ(e.m.names(kDir).size(), 1u);
        for (const std::string &l : e.m.log) CHECK(l.compare(0, 5, "write") != 0 && l.compare(0, 6, "rename") != 0);
    }
}

void t_damaged_sources() {
    { /* a truncated take: the header declares more audio than the file holds */
        Env e;
        Bytes b = e.src;
        b.resize(b.size() - 4000);
        e.m.put(e.path, b);
        EditResult r = e.run(0, 100);
        CHECK(!r.ok && r.error == EditError::BadSource);
        CHECK(r.message.find("repair") != std::string::npos);
        CHECK_EQ(e.m.names(kDir).size(), 1u);
    }
    { /* not a WAV */
        Env e;
        e.m.put(e.path, Bytes(10000, 0x55));
        EditResult r = e.run(0, 100);
        CHECK(!r.ok && r.error == EditError::BadSource);
    }
    { /* too short for a header */
        Env e;
        e.m.put(e.path, Bytes(7, 1));
        CHECK(e.run(0, 1).error == EditError::BadSource);
    }
    { /* missing */
        Env e;
        e.m.files.clear();
        EditResult r = e.run(0, 100);
        CHECK(!r.ok && r.error == EditError::BadSource);
    }
    { /* not PCM */
        Env e;
        Bytes b = e.src;
        b[48 + 8] = 3; /* fmt tag: float */
        e.m.put(e.path, b);
        CHECK(e.run(0, 100).error == EditError::BadSource);
    }
    { /* a header of zero audio */
        SrcSpec s;
        s.frames = 0;
        Env e(s);
        CHECK(e.run(0, 1).error == EditError::BadPositions);
    }
}

void t_names() {
    { /* the wrong count, a name that exists, bad names: nothing is written */
        Env e;
        EditOptions o = e.opts();
        EditRequest r;
        r.source_path = e.path;
        r.in_frame = 0;
        r.out_frame = 1000;
        r.splits = {500};
        o.names = [](const std::string &, int) { return std::vector<std::string>{"a.wav"}; };
        CHECK(trim_split(r, o).error == EditError::BadNames);
        o.names = [](const std::string &, int) { return std::vector<std::string>{"a.wav", "a.wav"}; };
        CHECK(trim_split(r, o).error == EditError::BadNames);
        o.names = [](const std::string &s, int) { return std::vector<std::string>{s, "b.wav"}; };
        CHECK(trim_split(r, o).error == EditError::BadNames);
        o.names = [](const std::string &, int) { return std::vector<std::string>{"../x.wav", "b.wav"}; };
        CHECK(trim_split(r, o).error == EditError::BadNames);
        o.names = [](const std::string &, int) { return std::vector<std::string>{".tmp-x", "b.wav"}; };
        CHECK(trim_split(r, o).error == EditError::BadNames);
        o.names = {};
        CHECK(trim_split(r, o).error == EditError::BadNames);
        e.m.put(std::string(kDir) + "/b.wav", Bytes(3, 1));
        o.names = [](const std::string &, int) { return std::vector<std::string>{"a.wav", "b.wav"}; };
        CHECK(trim_split(r, o).error == EditError::BadNames);
        CHECK(e.unchanged());
        CHECK_EQ(e.m.names(kDir).size(), 2u);
        CHECK(e.m.events("write").empty());
    }
    { /* an existing result is never overwritten: the fake gives -2 when -1 is taken (the provider's job) */
        Env e;
        e.nm.used.insert(1);
        EditResult r = e.run(0, 100);
        CHECK(r.ok);
        CHECK_EQ(r.outputs[0], std::string(kDir) + "/take_006_20261108-1105-2.wav");
    }
}

void t_replace_split() {
    Env e;
    e.m.put(e.path + ".peaks", Bytes(100, 7));
    const std::string orig_path = e.path;
    EditResult r = e.run(0, 12000, {4000, 8000}, SaveMode::ReplaceOriginal);
    CHECK(r.ok);
    CHECK(r.original_removed);
    CHECK_EQ(r.outputs.size(), 3u);
    CHECK(!e.m.exists(orig_path));
    CHECK(!e.m.exists(orig_path + ".peaks"));
    CHECK_EQ(e.deleted.size(), 1u);
    CHECK_EQ(e.deleted.empty() ? std::string() : e.deleted[0], orig_path);
    verify_part(e, "take_006_20261108-1105-1.wav", 0, 4000, 1, 3);
    verify_part(e, "take_006_20261108-1105-2.wav", 4000, 4000, 2, 3);
    verify_part(e, "take_006_20261108-1105-3.wav", 8000, 4000, 3, 3);
    CHECK(e.tmp_files().empty());
    /* the order: every result renamed and the folder synced, and only then the original goes */
    size_t last_rename = 0, unlink_orig = 0, sync_between = 0;
    for (size_t i = 0; i < e.m.log.size(); i++) {
        const std::string &l = e.m.log[i];
        if (l.compare(0, 6, "rename") == 0) last_rename = i;
        if (l == "unlink " + orig_path) unlink_orig = i;
    }
    for (size_t i = last_rename; i < unlink_orig; i++)
        if (e.m.log[i].compare(0, 9, "fsync_dir") == 0) sync_between++;
    CHECK(unlink_orig > last_rename);
    CHECK(sync_between >= 1);
}

void t_replace_trim_alone() {
    Env e;
    e.m.put(e.path + ".peaks", Bytes(100, 7));
    EditResult r = e.run(2000, 3000, {}, SaveMode::ReplaceOriginal);
    CHECK(r.ok);
    CHECK(r.original_removed);
    CHECK_EQ(r.outputs.size(), 1u);
    CHECK_EQ(r.outputs[0], e.path); /* the original's exact name */
    CHECK(e.nm.calls.empty());      /* no new number is used */
    verify_part(e, kSrc, 2000, 1000, 1, 1);
    CHECK_EQ(e.m.names(kDir).size(), 1u); /* nothing else: no peaks cache, no temporary */
    CHECK_EQ(e.deleted.size(), 1u);
    /* one rename over the original and no unlink of it: there is no moment without the file */
    for (const std::string &l : e.m.log) CHECK(l != "unlink " + e.path);
    int renames = 0;
    for (const std::string &l : e.m.log) renames += l.compare(0, 6, "rename") == 0;
    CHECK_EQ(renames, 1);
}

void t_ordering() {
    Env e;
    CHECK(e.run(0, 9000, {3000, 6000}, SaveMode::NewFiles).ok);
    /* every result is fdatasync'ed before it is renamed, and the folder is synced after the last rename */
    std::set<std::string> synced;
    size_t renames = 0, last_rename = 0, last_sync_dir = 0;
    for (size_t i = 0; i < e.m.log.size(); i++) {
        const std::string &l = e.m.log[i];
        if (l.compare(0, 10, "fdatasync ") == 0) synced.insert(l.substr(10));
        if (l.compare(0, 7, "rename ") == 0) {
            const std::string from = l.substr(7, l.find(' ', 7) - 7);
            CHECK(synced.count(from) == 1);
            renames++;
            last_rename = i;
        }
        if (l.compare(0, 9, "fsync_dir") == 0) last_sync_dir = i;
    }
    CHECK_EQ(renames, 3u);
    CHECK(last_sync_dir > last_rename);
    /* no rename happens before the last result is complete: all temporaries are synced first */
    size_t first_rename = e.m.log.size(), last_fdatasync = 0;
    for (size_t i = 0; i < e.m.log.size(); i++) {
        if (e.m.log[i].compare(0, 7, "rename ") == 0) first_rename = std::min(first_rename, i);
        if (e.m.log[i].compare(0, 10, "fdatasync ") == 0) last_fdatasync = i;
    }
    CHECK(last_fdatasync < first_rename);
}

void t_cancel() {
    for (int when = 0; when < 3; when++) {
        Env e;
        std::atomic<bool> cancel(false);
        EditOptions o = e.opts();
        if (when == 0) cancel = true; /* before the start */
        uint64_t calls = 0;
        o.progress = [&](uint64_t d, uint64_t t) {
            ++calls;
            e.progress.push_back(d);
            if (when == 1 && d > t / 3) cancel = true;  /* in the first part */
            if (when == 2 && d > t * 3 / 4) cancel = true; /* in the last part */
        };
        o.cancel = &cancel;
        EditRequest r;
        r.source_path = e.path;
        r.in_frame = 0;
        r.out_frame = 12000;
        r.splits = {4000, 8000};
        r.mode = SaveMode::ReplaceOriginal;
        EditResult res = trim_split(r, o);
        CHECK(!res.ok);
        CHECK(res.error == EditError::Cancelled);
        CHECK(e.unchanged());                      /* the original is untouched */
        CHECK(e.tmp_files().empty());              /* the temporaries are gone */
        CHECK_EQ(e.m.names(kDir).size(), 1u);      /* and no result was left under a final name */
        CHECK(e.deleted.empty());
        CHECK(!res.original_removed);
        CHECK(monotonic(e.progress));
    }
    { /* the cancel flag is honoured at every block of a single part too, and the list of events shows no rename */
        Env e;
        std::atomic<bool> cancel(false);
        EditOptions o = e.opts();
        o.progress = [&](uint64_t d, uint64_t) { if (d >= 4096) cancel = true; };
        o.cancel = &cancel;
        EditRequest r;
        r.source_path = e.path;
        r.out_frame = 12000;
        CHECK(trim_split(r, o).error == EditError::Cancelled);
        CHECK(e.m.events("rename").empty());
    }
}

void t_disk_full() {
    { /* the check before starting: nothing is written */
        Env e;
        e.m.free_bytes = 5000;
        EditResult r = e.run(0, 12000, {6000});
        CHECK(!r.ok && r.error == EditError::NoSpace);
        CHECK(r.message.find("free space") != std::string::npos);
        CHECK(e.unchanged());
        CHECK_EQ(e.m.names(kDir).size(), 1u);
        CHECK(e.m.events("write").empty());
    }
    { /* exactly enough (results + margin) passes; one byte less fails */
        Env e;
        const uint64_t need = 2ull * kDataOffset + 12000ull * 2 + 64 * 1024;
        e.m.free_bytes = need - 1;
        CHECK(e.run(0, 12000, {6000}).error == EditError::NoSpace);
        e.nm.used.clear();
        e.m.free_bytes = need;
        CHECK(e.run(0, 12000, {6000}).ok);
    }
    { /* the disk fills up while writing: the temporaries go, the original stays, the reason says so */
        for (int at = 1; at < 40; at += 3) {
            Env e;
            e.m.fail_errno = ENOSPC;
            e.m.fail_at = -1;
            /* count steps of a clean run first */
            Env probe;
            CHECK(probe.run(0, 12000, {6000}).ok);
            if (at > probe.m.step) break;
            e.m.fail_at = at;
            EditResult r = e.run(0, 12000, {6000});
            CHECK(!r.ok);
            CHECK(r.error == EditError::NoSpace || r.error == EditError::Io || r.error == EditError::BadSource);
            CHECK(!r.message.empty());
            CHECK(e.unchanged());
            cleanup_temporaries(e.m, kDir);
            CHECK(e.tmp_files().empty());
        }
    }
    { /* ENOSPC on a write is reported as the disk being full */
        Env e;
        Env probe;
        probe.run(0, 12000, {6000});
        int first_write_step = 0, s = 0;
        for (const std::string &l : probe.m.log) {
            s++;
            if (l.compare(0, 5, "write") == 0 && !first_write_step) first_write_step = s;
        }
        CHECK(first_write_step > 0);
        e.m.fail_errno = ENOSPC;
        e.m.fail_at = first_write_step + 2;
        EditResult r = e.run(0, 12000, {6000});
        CHECK(!r.ok && r.error == EditError::NoSpace);
        CHECK(r.message.find("full") != std::string::npos);
        CHECK(e.m.names(kDir).size() == 1);
    }
}

void t_source_changed() {
    Env e;
    EditOptions o = e.opts();
    bool touched = false;
    o.progress = [&](uint64_t d, uint64_t) {
        if (d > 0 && !touched) {
            touched = true;
            e.m.files[e.path]->mtime += 5; /* someone wrote to the take */
        }
    };
    EditRequest r;
    r.source_path = e.path;
    r.out_frame = 12000;
    EditResult res = trim_split(r, o);
    CHECK(!res.ok && res.error == EditError::SourceChanged);
    CHECK_EQ(e.m.names(kDir).size(), 1u);
}

void t_rename_failure_rolls_back() {
    Env e;
    Env probe;
    CHECK(probe.run(0, 12000, {4000, 8000}).ok);
    int third_rename = 0, s = 0, n = 0;
    for (const std::string &l : probe.m.log) {
        s++;
        if (l.compare(0, 7, "rename ") == 0 && ++n == 3) third_rename = s;
    }
    CHECK(third_rename > 0);
    e.m.fail_at = third_rename;
    EditResult r = e.run(0, 12000, {4000, 8000}, SaveMode::ReplaceOriginal);
    CHECK(!r.ok && r.error == EditError::Io);
    CHECK(e.unchanged()); /* replace never removed the original */
    CHECK_EQ(e.m.names(kDir).size(), 1u); /* the two results already renamed were taken back */
    CHECK(e.deleted.empty());
}

void t_unlink_original_fails() {
    Env e;
    Env probe;
    CHECK(probe.run(0, 12000, {6000}, SaveMode::ReplaceOriginal).ok);
    int s = 0, at = 0;
    for (const std::string &l : probe.m.log) {
        s++;
        if (l == "unlink " + probe.path) at = s;
    }
    e.m.fail_at = at;
    EditResult r = e.run(0, 12000, {6000}, SaveMode::ReplaceOriginal);
    CHECK(r.ok); /* the results exist: the save worked */
    CHECK(!r.original_removed);
    CHECK(!r.warning.empty());
    CHECK(e.unchanged());
    CHECK(e.deleted.empty());
    CHECK_EQ(e.m.names(kDir).size(), 3u);
}

void t_rf64_header_boundary() {
    TakeInfo t;
    t.ok = true;
    t.fmt.rate = 48000;
    t.fmt.channels = 1;
    t.fmt.bits = 16;
    t.block_align = 2;
    t.frames = 1ull << 33;
    t.data_offset = kDataOffset;
    t.bwf.time_reference = 10;
    const uint64_t plain_bytes = 0xFFFFFFFEull - (kDataOffset - 8); /* the largest data size a plain RIFF can state */
    CHECK(!needs_rf64(kDataOffset, plain_bytes));
    CHECK(needs_rf64(kDataOffset, plain_bytes + 2));
    for (uint64_t frames : std::vector<uint64_t>{plain_bytes / 2 - 1, plain_bytes / 2, plain_bytes / 2 + 1, 3000000000ull}) {
        const bool rf = needs_rf64(kDataOffset, frames * 2);
        std::vector<unsigned char> h = build_part_header(t, "x.wav", 12345, frames, 1, 1);
        CHECK_EQ(h.size(), static_cast<size_t>(kDataOffset));
        HeaderInfo p = parse_header(h.data(), h.size(), kDataOffset + frames * 2); /* a file of that size */
        CHECK(p.ok);
        CHECK_EQ(p.rf64, rf);
        CHECK_EQ(p.has_ds64, rf);
        CHECK_EQ(p.declared_data, frames * 2);
        CHECK_EQ(p.actual_data, frames * 2);
        CHECK(p.consistent());
        CHECK(p.has_bext && p.has_ixml);
        CHECK_EQ(bext_tref(h, p), 10u + 12345u);
        CHECK_EQ(std::memcmp(h.data(), rf ? "RF64" : "RIFF", 4), 0);
    }
}

/* The whole engine on a MOCKED source of 4.5 GiB (generated on read) and discarded results: nothing big is stored. */
void t_rf64_engine_mocked() {
    MemOps m;
    m.discard_new = true;
    m.log_io = false;
    AudioFormat fmt;
    const uint64_t frames = 2415919104ull; /* 4.5 GiB of mono S16 */
    BwfInfo info;
    info.time_reference = 1000;
    info.origination_date = "2026-11-08";
    info.origination_time = "11:05:00";
    const Bytes head = build_header(fmt, info, frames * 2, true);
    const std::string path = std::string(kDir) + "/big.wav";
    m.put_virtual(path, kDataOffset + frames * 2, [head](uint64_t off, unsigned char *b, size_t n) {
        std::memset(b, 0, n);
        if (off < head.size()) std::memcpy(b, &head[static_cast<size_t>(off)], std::min<size_t>(n, head.size() - static_cast<size_t>(off)));
    });
    HeaderInfo sh = parse_header(m, path);
    CHECK(sh.ok && sh.rf64 && sh.consistent());
    FakeNames nm;
    EditOptions o;
    o.ops = &m;
    o.names = [&](const std::string &s, int c) { return nm(s, c); };
    std::vector<uint64_t> prog;
    o.progress = [&](uint64_t d, uint64_t) { prog.push_back(d); };
    EditRequest r;
    r.source_path = path;
    r.in_frame = 1000;
    r.out_frame = frames - 1000; /* 4.5 GiB minus 4000 bytes: needs RF64 */
    EditResult res = trim_split(r, o);
    CHECK(res.ok);
    CHECK_EQ(res.outputs.size(), 1u);
    if (res.ok) {
        const std::string out = res.outputs[0];
        StatInfo st;
        CHECK_EQ(m.stat(out, st), 0);
        const uint64_t data = (frames - 2000) * 2;
        CHECK_EQ(st.size, kDataOffset + data);
        const Bytes h = m.get(out);
        HeaderInfo p = parse_header(h.data(), h.size(), st.size);
        CHECK(p.ok && p.rf64 && p.has_ds64 && p.consistent());
        CHECK_EQ(p.declared_data, data);
        CHECK_EQ(bext_tref(h, p), 2000u); /* 1000 + the 1000 frames cut off */
    }
    CHECK(monotonic(prog));
    CHECK(!prog.empty() && prog.back() == (frames - 2000) * 2);
    CHECK(m.names(kDir).size() == 2);
    /* the same source cut into two parts of about 2.25 GiB: both plain RIFF */
    m.files.erase(std::string(kDir) + "/big-1.wav");
    nm.used.clear();
    EditRequest r2 = r;
    r2.splits = {frames / 2};
    EditResult res2 = trim_split(r2, o);
    CHECK(res2.ok);
    for (const std::string &out : res2.outputs) {
        StatInfo st;
        m.stat(out, st);
        const Bytes h = m.get(out);
        HeaderInfo p = parse_header(h.data(), h.size(), st.size);
        CHECK(p.ok && !p.rf64 && p.consistent());
    }
}

void t_cleanup_temporaries() {
    MemOps m;
    m.put("/d/.tmp-abc", Bytes(5, 1));
    m.put("/d/.tmp-pk123", Bytes(5, 1));
    m.put("/d/take.wav", Bytes(5, 1));
    m.put("/d/take.wav.peaks", Bytes(5, 1));
    m.put("/other/.tmp-keep", Bytes(5, 1));
    CHECK_EQ(cleanup_temporaries(m, "/d"), 2);
    CHECK(m.exists("/d/take.wav") && m.exists("/d/take.wav.peaks") && m.exists("/other/.tmp-keep"));
    CHECK(!m.exists("/d/.tmp-abc"));
    CHECK_EQ(cleanup_temporaries(m, "/d"), 0);
}

/* The real file system: a trim and a split of a generated take in a real folder, byte compared. */
void t_real_fs() {
    char tmpl[] = "/tmp/cassette-edit-XXXXXX";
    const char *d = mkdtemp(tmpl);
    CHECK(d != nullptr);
    if (!d) return;
    const std::string dir = d;
    SrcSpec s;
    s.fmt.channels = 2;
    s.frames = 100000;
    const Bytes src = make_source(s);
    const std::string path = dir + "/take_006_20261108-1105.wav";
    {
        std::ofstream f(path, std::ios::binary);
        f.write(reinterpret_cast<const char *>(src.data()), static_cast<std::streamsize>(src.size()));
    }
    FakeNames nm;
    EditOptions o;
    o.names = [&](const std::string &n, int c) { return nm(n, c); };
    EditRequest r;
    r.source_path = path;
    r.in_frame = 1234;
    r.out_frame = 98765;
    r.splits = {40000, 70000};
    EditResult res = trim_split(r, o);
    CHECK(res.ok);
    CHECK_EQ(res.outputs.size(), 3u);
    const uint64_t starts[3] = {1234, 40000, 70000}, ends[3] = {40000, 70000, 98765};
    for (size_t i = 0; i < res.outputs.size(); i++) {
        std::ifstream f(res.outputs[i], std::ios::binary);
        Bytes b((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        const Bytes want = make_audio(s.fmt, starts[i], ends[i] - starts[i]);
        CHECK_EQ(b.size(), kDataOffset + want.size());
        CHECK(b.size() > kDataOffset && std::equal(want.begin(), want.end(), b.begin() + kDataOffset));
        HeaderInfo h = parse_header(posix_file_ops(), res.outputs[i]);
        CHECK(h.ok && h.consistent());
    }
    std::vector<std::string> left;
    posix_file_ops().list_dir(dir, left);
    CHECK_EQ(left.size(), 4u);
    for (const std::string &n : left) CHECK(n.compare(0, 5, ".tmp-") != 0);
    /* replace + trim alone on the real file system */
    EditRequest r2;
    r2.source_path = path;
    r2.in_frame = 10;
    r2.out_frame = 20010;
    r2.mode = SaveMode::ReplaceOriginal;
    EditResult res2 = trim_split(r2, o);
    CHECK(res2.ok);
    HeaderInfo h2 = parse_header(posix_file_ops(), path);
    CHECK(h2.ok && h2.consistent());
    CHECK_EQ(h2.actual_data, 20000u * 4);
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

}  // namespace

void test_trim() {
    t_trim_alone();
    t_edges();
    t_split_two_points();
    t_formats();
    t_placeholder_sources();
    t_time_reference();
    t_refusals();
    t_damaged_sources();
    t_names();
    t_replace_split();
    t_replace_trim_alone();
    t_ordering();
    t_cancel();
    t_disk_full();
    t_source_changed();
    t_rename_failure_rolls_back();
    t_unlink_original_fails();
    t_rf64_header_boundary();
    t_rf64_engine_mocked();
    t_cleanup_temporaries();
    t_real_fs();
}
