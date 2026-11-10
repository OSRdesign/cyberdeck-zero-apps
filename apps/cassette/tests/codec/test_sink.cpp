/*
 * SPDX-License-Identifier: MIT
 *
 * Cassette codec tests: the byte sinks (positional writes, short writes, failures), the sample conversions and the tag
 * names.
 */
#include "check.hpp"

#include "byte_sink.hpp"
#include "decoder.hpp" /* the phase C seam: it must compile */
#include "encoder.hpp"
#include "fileops.hpp"
#include "siggen.hpp"

#include <cerrno>
#include <cstdlib>
#include <fcntl.h>
#include <filesystem>
#include <string>
#include <unistd.h>
#include <vector>

using namespace cassette;

namespace {

/* posix calls, with pwrite limited to `chunk` bytes (short writes) and an optional failure after `fail_after` bytes */
class ShortOps final : public FileOps {
public:
    FileOps &b = posix_file_ops();
    size_t chunk = 7;
    uint64_t written = 0, fail_after = 0;
    int eintr_once = 1;
    int open(const std::string &p, int f, int m) override { return b.open(p, f, m); }
    int close(int fd) override { return b.close(fd); }
    ssize_t write(int fd, const void *buf, size_t n) override { return b.write(fd, buf, n); }
    ssize_t pwrite(int fd, const void *buf, size_t n, uint64_t off) override {
        if (eintr_once > 0) {
            --eintr_once;
            return -EINTR;
        }
        if (fail_after && written >= fail_after) return -ENOSPC;
        ssize_t w = b.pwrite(fd, buf, n < chunk ? n : chunk, off);
        if (w > 0) written += static_cast<uint64_t>(w);
        return w;
    }
    ssize_t pread(int fd, void *buf, size_t n, uint64_t off) override { return b.pread(fd, buf, n, off); }
    int fdatasync(int fd) override { return b.fdatasync(fd); }
    int fsync(int fd) override { return b.fsync(fd); }
    int ftruncate(int fd, uint64_t len) override { return b.ftruncate(fd, len); }
    int fstat(int fd, StatInfo &st) override { return b.fstat(fd, st); }
    int stat(const std::string &p, StatInfo &st) override { return b.stat(p, st); }
    int rename(const std::string &a, const std::string &c) override { return b.rename(a, c); }
    int unlink(const std::string &p) override { return b.unlink(p); }
    int mkdir(const std::string &p) override { return b.mkdir(p); }
    int fsync_dir(const std::string &p) override { return b.fsync_dir(p); }
    int list_dir(const std::string &p, std::vector<std::string> &n) override { return b.list_dir(p, n); }
    int free_space(const std::string &p, uint64_t &a) override { return b.free_space(p, a); }
};

std::string temp_file() {
    std::string t = (std::filesystem::temp_directory_path() / "cassette-codec-XXXXXX").string();
    std::vector<char> b(t.begin(), t.end());
    b.push_back('\0');
    int fd = mkstemp(b.data());
    if (fd >= 0) ::close(fd);
    return b.data();
}

void test_file_sink() {
    ShortOps ops;
    const std::string path = temp_file();
    int fd = ops.open(path, O_RDWR | O_TRUNC, 0644);
    CHECK(fd >= 0);
    {
        FileByteSink s(ops, fd);
        const std::string a = "fLaC-header-0123456789", c = "XYZW";
        CHECK_EQ(s.write(a.data(), a.size()), 0); /* 7-byte pwrites and one EINTR */
        CHECK_EQ(s.tell(), uint64_t(a.size()));
        CHECK_EQ(s.size(), uint64_t(a.size()));
        CHECK(s.writes() >= 4);
        CHECK_EQ(s.seek(4), 0); /* rewrite in the middle: the end stays */
        CHECK_EQ(s.write(c.data(), c.size()), 0);
        CHECK_EQ(s.tell(), uint64_t(8));
        CHECK_EQ(s.size(), uint64_t(a.size()));
        StatInfo st;
        CHECK_EQ(ops.fstat(fd, st), 0);
        CHECK_EQ(st.size, uint64_t(a.size()));
        char back[64] = {0};
        CHECK_EQ(ops.pread(fd, back, a.size(), 0), ssize_t(a.size()));
        CHECK_EQ(std::string(back, a.size()), std::string("fLaCXYZWder-0123456789"));
        /* a failing card */
        ops.fail_after = ops.written;
        CHECK_EQ(s.seek(s.size()), 0);
        CHECK_EQ(s.write("more", 4), -ENOSPC);
    }
    ops.close(fd);
    ops.unlink(path);
}

void test_memory_sink() {
    MemoryByteSink m;
    CHECK_EQ(m.write("abcdef", 6), 0);
    CHECK_EQ(m.seek(2), 0);
    CHECK_EQ(m.write("ZZ", 2), 0);
    CHECK_EQ(std::string(m.data.begin(), m.data.end()), std::string("abZZef"));
    CHECK_EQ(m.size(), uint64_t(6));
    CHECK_EQ(m.seeks(), uint64_t(1));
    m.fail_at = 10;
    CHECK_EQ(m.seek(6), 0);
    CHECK_EQ(m.write("1234", 4), 0);
    CHECK_EQ(m.write("5", 1), -ENOSPC);
    CHECK_EQ(m.size(), uint64_t(10));
}

void test_conversions() {
    /* 16384, -16384, -32768, 32767 and 4194304, -4194304, -8388608, 8388607, little endian */
    const sig::Bytes b16 = {0x00, 0x40, 0x00, 0xC0, 0x00, 0x80, 0xFF, 0x7F};
    const sig::Bytes b24 = {0x00, 0x00, 0x40, 0x00, 0x00, 0xC0, 0x00, 0x00, 0x80, 0xFF, 0xFF, 0x7F};
    int32_t i32[4];
    pcm_to_int32(b16.data(), 16, i32, 4);
    CHECK_EQ(i32[0], 16384);
    CHECK_EQ(i32[1], -16384);
    CHECK_EQ(i32[2], -32768);
    CHECK_EQ(i32[3], 32767);
    pcm_to_int32(b24.data(), 24, i32, 4);
    CHECK_EQ(i32[0], 4194304);
    CHECK_EQ(i32[1], -4194304);
    CHECK_EQ(i32[2], -8388608);
    float fl[4];
    pcm_to_float(b24.data(), 24, fl, 4);
    CHECK(fl[0] > 0.4999f && fl[0] < 0.5001f);
    CHECK_EQ(fl[2], -1.0f);
    pcm_to_float(b16.data(), 16, fl, 4);
    CHECK(fl[1] < -0.4999f && fl[1] > -0.5001f);
    int16_t s16[4];
    pcm_to_int16(b24.data(), 24, s16, 4);
    CHECK_EQ(s16[0], int16_t(16384));
    CHECK_EQ(s16[2], int16_t(-32768));
    /* unaligned input: a 24-bit buffer read from an odd address */
    sig::Bytes odd(1 + 12);
    for (size_t i = 0; i < 12; ++i) odd[1 + i] = b24[i];
    pcm_to_int32(odd.data() + 1, 24, i32, 4);
    CHECK_EQ(i32[1], -4194304);
    pcm_to_int32(b24.data(), 24, i32, 4);
    CHECK_EQ(i32[3], 8388607);
}

void test_tag_names() {
    CHECK(valid_tag_name("TITLE"));
    CHECK(valid_tag_name("REPLAYGAIN_TRACK_GAIN"));
    CHECK(valid_tag_name("Take Notes"));
    CHECK(!valid_tag_name(""));
    CHECK(!valid_tag_name("A=B"));
    CHECK(!valid_tag_name("caf\xC3\xA9")); /* names are ASCII */
    CHECK(!valid_tag_name("~TILDE"));     /* 0x7E is outside 0x20..0x7D */
    CHECK(!valid_tag_name(std::string(65, 'A')));
}

void test_stats() {
    EncoderStats s;
    s.frames_in = 100;
    s.frames_out = 30;
    CHECK_EQ(s.pending_frames(), uint64_t(70));
    s.frames_out = 120;
    CHECK_EQ(s.pending_frames(), uint64_t(0));
    DecodedInfo di; /* the decoder seam compiles and has sane defaults */
    CHECK(!di.frames_known);
}

}  // namespace

void test_sink() {
    test_file_sink();
    test_memory_sink();
    test_conversions();
    test_tag_names();
    test_stats();
}
