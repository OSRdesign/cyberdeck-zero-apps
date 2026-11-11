/*
 * SPDX-License-Identifier: MIT
 *
 * Crash test: a forked child records a synthetic signal through the real writer (real POSIX, real clock, real thread)
 * and is killed with SIGKILL at a different moment for each seed of a fixed list. The parent repairs and checks
 * the file against the generated signal. A second pass emulates the loss of the unsynced tail (a power cut) by cutting
 * a copy of the file back to the last synced length plus a random part of one sync interval.
 */
#include "check.hpp"
#include "helpers.hpp"
#include "repair.hpp"
#include "ring.hpp"
#include "writer.hpp"

#include <atomic>
#include <csignal>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

using namespace cassette;
using namespace tst;

namespace {

constexpr uint32_t kSyncMs = 150;
constexpr uint64_t kRate = 48000;

struct Shared {
    std::atomic<uint64_t> produced; /* frames accepted by the ring */
    std::atomic<uint64_t> synced;   /* frames the writer reported durable (set after fdatasync returned) */
    std::atomic<uint64_t> dropped;
};

[[noreturn]] void child(const std::string &dir, Shared *sh) {
    AudioFormat f;
    Ring ring(f, 2.0);
    WriterConfig cfg;
    cfg.fmt = f;
    cfg.take = "crash";
    cfg.recordings_dir = dir;
    const std::string p = dir + "/crash.wav";
    cfg.namer = [p](int) { return p; };
    cfg.min_free_bytes = 0;
    cfg.sync_interval_ms = kSyncMs;
    cfg.flush_ms = 50;
    cfg.block_bytes = 32768;
    cfg.on_sync = [sh](uint64_t frames) { sh->synced.store(frames); };
    Writer w(ring, posix_file_ops(), system_clock(), cfg);
    if (w.start() != 0) _exit(2);
    w.start_thread();
    const uint64_t t0 = system_clock().mono_ms();
    uint64_t made = 0;
    for (;;) {
        const uint64_t target = (system_clock().mono_ms() - t0) * kRate / 1000;
        while (made < target) {
            size_t n = static_cast<size_t>(std::min<uint64_t>(target - made, 960));
            std::vector<int16_t> b = sig_block(made, n);
            size_t acc = ring.write(b.data(), n); /* the stream is the accepted frames, in order */
            made += acc;
            sh->produced.store(made);
            sh->dropped.store(ring.frames_dropped());
            if (acc < n) break;
        }
        system_clock().sleep_ms(4);
        if (system_clock().mono_ms() - t0 > 20000) _exit(0); /* never reached when the parent kills */
    }
}

uint64_t xorshift(uint64_t &s) {
    s ^= s << 13;
    s ^= s >> 7;
    s ^= s << 17;
    return s;
}

}  // namespace

void test_crash() {
    const uint64_t seeds[] = {1, 2, 3, 5, 8, 13, 21, 34, 55, 89, 144, 233, 377, 610, 987, 1597, 2584, 4181, 6765, 10946, 17711, 28657, 46368, 75025};
    int kills = 0, lost_more_than_interval = 0, power_cases = 0;
    uint64_t worst_missing = 0;
    Shared *sh = static_cast<Shared *>(mmap(nullptr, sizeof(Shared), PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0));
    CHECK(sh != MAP_FAILED);
    if (sh == MAP_FAILED) return;
    for (uint64_t seed : seeds) {
        TempDir d;
        sh->produced = 0;
        sh->synced = 0;
        sh->dropped = 0;
        uint64_t rng = seed * 0x9E3779B97F4A7C15ull + 1;
        const uint32_t delay_ms = 250 + static_cast<uint32_t>(xorshift(rng) % 1100);
        fflush(nullptr);
        pid_t pid = fork();
        if (pid == 0) child(d.path, sh);
        CHECK(pid > 0);
        if (pid <= 0) continue;
        system_clock().sleep_ms(delay_ms);
        kill(pid, SIGKILL);
        int status = 0;
        waitpid(pid, &status, 0);
        CHECK(WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL);
        kills++;
        const uint64_t produced = sh->produced.load(), synced = sh->synced.load();
        CHECK_EQ(sh->dropped.load(), static_cast<uint64_t>(0));
        CHECK(produced > 0);

        const std::string file = d.file("crash.wav");
        /* the pre-repair state, kept for the power-cut emulation */
        std::vector<unsigned char> raw = read_file(file);
        std::vector<unsigned char> rawJournal = read_file(journal_file(d.path, "crash"));
        CHECK(raw.size() >= kDataOffset);
        HeaderInfo pre = parse_header(posix_file_ops(), file);
        CHECK(pre.ok); /* the header written at start is always valid */

        RepairReport rep = repair_all(posix_file_ops(), d.path);
        CHECK(rep.errors.empty());
        CHECK_EQ(rep.repaired.size(), static_cast<size_t>(1));
        HeaderInfo h = parse_header(posix_file_ops(), file);
        CHECK(h.ok && h.consistent() && !h.suspicious());
        StatInfo st;
        posix_file_ops().stat(file, st);
        CHECK_EQ(h.declared_data, st.size - kDataOffset); /* data size equals the file length */
        CHECK_EQ((st.size - kDataOffset) % 2, static_cast<uint64_t>(0));
        const uint64_t frames = h.declared_data / 2;
        CHECK(frames >= synced); /* what was synced before the kill is in the file */
        CHECK(frames <= produced);
        CHECK(audio_is_sig(file, kDataOffset, 0, frames)); /* every recovered frame is the generated signal */
        const uint64_t missing = produced - frames;
        worst_missing = std::max(worst_missing, missing);
        if (missing > kRate * kSyncMs / 1000) lost_more_than_interval++;
        CHECK(missing <= kRate * kSyncMs / 1000); /* at most one sync interval of audio is lost */

        /* idempotent */
        std::vector<unsigned char> once = read_file(file);
        RepairReport rep2 = repair_all(posix_file_ops(), d.path);
        CHECK(rep2.repaired.empty() && rep2.errors.empty());
        CHECK(read_file(file) == once);

        /* power-cut emulation: the unsynced tail is gone, down to a random byte (maybe mid-frame) */
        TempDir p;
        std::filesystem::create_directories(p.path + "/.journal");
        uint64_t keep = kDataOffset + synced * 2 + xorshift(rng) % (kRate * kSyncMs / 1000 * 2 + 1);
        keep = std::min<uint64_t>(keep, raw.size());
        std::vector<unsigned char> cut(raw.begin(), raw.begin() + static_cast<std::ptrdiff_t>(keep));
        write_file(p.file("crash.wav"), cut);
        {
            JournalEntry pj;
            CHECK(journal_from_json(std::string(rawJournal.begin(), rawJournal.end()), pj));
            pj.path = p.file("crash.wav"); /* the journal names the file by path: point it at the copy */
            pj.parts = {pj.path};
            CHECK_EQ(journal_write(posix_file_ops(), p.path, pj), 0);
        }
        RepairReport rp = repair_all(posix_file_ops(), p.path);
        CHECK(rp.errors.empty());
        CHECK_EQ(rp.repaired.size(), static_cast<size_t>(1));
        HeaderInfo ph = parse_header(posix_file_ops(), p.file("crash.wav"));
        CHECK(ph.ok && ph.consistent());
        const uint64_t pframes = ph.declared_data / 2;
        CHECK_EQ(pframes, (keep - kDataOffset) / 2);
        CHECK(pframes >= synced);
        CHECK(audio_is_sig(p.file("crash.wav"), kDataOffset, 0, pframes));
        CHECK(produced - pframes <= kRate * kSyncMs / 1000 * 3); /* synced + up to one more interval kept */
        RepairReport rp2 = repair_all(posix_file_ops(), p.path);
        CHECK(rp2.repaired.empty());
        power_cases++;
    }
    std::printf("crash test: %d SIGKILLs, %d power-cut emulations, worst unrecovered tail %llu frames (%.0f ms), "
                "%d kills lost more than one sync interval (%u ms)\n",
                kills, power_cases, static_cast<unsigned long long>(worst_missing),
                static_cast<double>(worst_missing) * 1000.0 / kRate, lost_more_than_interval, kSyncMs);
    CHECK_EQ(lost_more_than_interval, 0);
    munmap(sh, sizeof(Shared));
}
