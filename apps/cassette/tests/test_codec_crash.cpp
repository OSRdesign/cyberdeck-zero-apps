/*
 * SPDX-License-Identifier: MIT
 *
 * Crash test for compressed takes: as test_crash.cpp, a forked child records the synthetic signal through the real
 * writer (real POSIX, real clock, real thread) with the FLAC or the Ogg Opus test encoder and is killed with SIGKILL at a
 * different moment for each seed; the parent repairs from the journal and checks the stream. A second pass emulates a
 * power cut: a copy cut back to the length of the last sync plus a random part of what followed.
 */
#include "check.hpp"
#include "fixture_codecs.hpp"
#include "helpers.hpp"
#include "repair.hpp"
#include "ring.hpp"
#include "writer.hpp"

#include <atomic>
#include <csignal>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

using namespace cassette;
using namespace tst;

namespace {

constexpr uint32_t kSyncMs = 150;
constexpr uint64_t kRate = 48000;

struct Shared {
    std::atomic<uint64_t> produced;
    std::atomic<uint64_t> synced;       /* frames the writer reported durable */
    std::atomic<uint64_t> synced_bytes; /* the file length at that sync */
    std::atomic<uint64_t> dropped;
};

[[noreturn]] void child(const std::string &dir, bool opus, Shared *sh) {
    AudioFormat f;
    Ring ring(f, 2.0);
    WriterConfig cfg;
    cfg.fmt = f;
    cfg.take = "crash";
    cfg.recordings_dir = dir;
    const std::string p = dir + (opus ? "/crash.opus" : "/crash.flac");
    cfg.namer = [p](int) { return p; };
    cfg.min_free_bytes = 0;
    cfg.sync_interval_ms = kSyncMs;
    cfg.flush_ms = 50;
    cfg.block_bytes = 32768;
    cfg.codec = opus ? "opus" : "flac";
    cfg.make_encoder = [f, opus](int) -> std::unique_ptr<Encoder> {
        if (opus) return std::unique_ptr<Encoder>(new fx::PlainOggOpus(f, 160, 50));
        return std::unique_ptr<Encoder>(new fx::VerbatimFlac(f, 4096));
    };
    cfg.on_sync = [sh, p](uint64_t frames) {
        struct stat st;
        if (::stat(p.c_str(), &st) == 0) sh->synced_bytes.store(static_cast<uint64_t>(st.st_size));
        sh->synced.store(frames);
    };
    Writer w(ring, posix_file_ops(), system_clock(), cfg);
    if (w.start() != 0) _exit(2);
    w.start_thread();
    const uint64_t t0 = system_clock().mono_ms();
    uint64_t made = 0;
    for (;;) {
        const uint64_t target = (system_clock().mono_ms() - t0) * kRate / 1000;
        while (made < target) {
            const size_t n = static_cast<size_t>(std::min<uint64_t>(target - made, 960));
            const std::vector<unsigned char> b = fx::pcm(f, made, n);
            const size_t acc = ring.write(b.data(), n);
            made += acc;
            sh->produced.store(made);
            sh->dropped.store(ring.frames_dropped());
            if (acc < n) break;
        }
        system_clock().sleep_ms(4);
        if (system_clock().mono_ms() - t0 > 20000) _exit(0);
    }
}

uint64_t xorshift(uint64_t &s) {
    s ^= s << 13;
    s ^= s >> 7;
    s ^= s << 17;
    return s;
}

StreamScan scan(bool opus, const std::string &p) {
    return opus ? scan_ogg_opus(posix_file_ops(), p) : scan_flac(posix_file_ops(), p);
}

bool audio_ok(bool opus, const std::string &p, uint64_t frames) {
    if (opus) return true; /* the Opus fixture's packets carry no samples: the duration is the check */
    const AudioFormat f;
    return fx::decode_verbatim_flac(read_file(p)) == fx::samples_of(f, fx::pcm(f, 0, frames));
}

void run(bool opus) {
    const uint64_t seeds[] = {3, 7, 11, 19, 42, 77, 123, 999, 4242, 31337};
    const char *ext = opus ? ".opus" : ".flac";
    int kills = 0, power = 0, over = 0;
    uint64_t worst = 0;
    /* what may be missing at most: a sync interval (an Opus page is only closed by a sync), the writer's flush time
     * (50 ms) and the producer's step (4 ms), plus the encoder's unit that was still open (a FLAC block, a packet) */
    const uint64_t allowance = kRate * (kSyncMs + 50 + 10) / 1000 + (opus ? 960 : 4096);
    Shared *sh = static_cast<Shared *>(mmap(nullptr, sizeof(Shared), PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0));
    CHECK(sh != MAP_FAILED);
    if (sh == MAP_FAILED) return;
    for (uint64_t seed : seeds) {
        TempDir d;
        sh->produced = 0;
        sh->synced = 0;
        sh->synced_bytes = 0;
        sh->dropped = 0;
        uint64_t rng = seed * 0x9E3779B97F4A7C15ull + 1;
        const uint32_t delay_ms = 250 + static_cast<uint32_t>(xorshift(rng) % 1100);
        fflush(nullptr);
        const pid_t pid = fork();
        if (pid == 0) child(d.path, opus, sh);
        CHECK(pid > 0);
        if (pid <= 0) continue;
        system_clock().sleep_ms(delay_ms);
        kill(pid, SIGKILL);
        int status = 0;
        waitpid(pid, &status, 0);
        CHECK(WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL);
        kills++;
        const uint64_t produced = sh->produced.load(), synced = sh->synced.load(), synced_bytes = sh->synced_bytes.load();
        CHECK_EQ(sh->dropped.load(), uint64_t(0));
        const std::string file = d.file(std::string("crash") + ext);
        const std::vector<unsigned char> raw = read_file(file);
        const std::vector<unsigned char> raw_journal = read_file(journal_file(d.path, "crash"));
        CHECK(scan(opus, file).ok); /* the stream headers written at start are always there */

        const RepairReport rep = repair_all(posix_file_ops(), d.path);
        CHECK(rep.errors.empty());
        CHECK_EQ(rep.repaired.size(), size_t(1));
        const StreamScan s = scan(opus, file);
        CHECK(s.ok && s.valid_end == s.file_size);
        CHECK(s.frames >= synced);
        CHECK(s.frames <= produced);
        if (!opus) CHECK_EQ(s.stored_total, s.frames);
        if (opus) CHECK(s.eos || s.header_only);
        CHECK(audio_ok(opus, file, s.frames));
        const uint64_t missing = produced - s.frames;
        worst = std::max(worst, missing);
        if (missing > allowance) over++;
        const std::vector<unsigned char> once = read_file(file);
        const RepairReport rep2 = repair_all(posix_file_ops(), d.path);
        CHECK(rep2.repaired.empty() && rep2.errors.empty());
        CHECK(read_file(file) == once);

        /* power cut: the unsynced tail is gone, down to a random byte */
        TempDir p;
        uint64_t keep = std::max<uint64_t>(synced_bytes, 1) + xorshift(rng) % 20000;
        keep = std::min<uint64_t>(keep, raw.size());
        std::vector<unsigned char> cut(raw.begin(), raw.begin() + static_cast<std::ptrdiff_t>(keep));
        const std::string pf = p.file(std::string("crash") + ext);
        write_file(pf, cut);
        JournalEntry pj;
        CHECK(journal_from_json(std::string(raw_journal.begin(), raw_journal.end()), pj));
        pj.path = pf;
        pj.parts = {pf};
        CHECK_EQ(journal_write(posix_file_ops(), p.path, pj), 0);
        const RepairReport rp = repair_all(posix_file_ops(), p.path);
        CHECK(rp.errors.empty());
        CHECK_EQ(rp.repaired.size(), size_t(1));
        const StreamScan ps = scan(opus, pf);
        CHECK(ps.ok && ps.valid_end == ps.file_size);
        CHECK(ps.frames >= synced || synced_bytes == 0);
        CHECK(audio_ok(opus, pf, ps.frames));
        power++;
    }
    std::printf("codec crash test %s: %d SIGKILLs, %d power-cut emulations, worst unrecovered tail %.0f ms, %d over "
                "the allowance (%u ms sync + 60 ms + one %s)\n",
                ext, kills, power, static_cast<double>(worst) * 1000.0 / kRate, over, kSyncMs, opus ? "packet" : "block");
    CHECK_EQ(over, 0);
    munmap(sh, sizeof(Shared));
}

}  // namespace

void test_codec_crash() {
    run(false);
    run(true);
}
