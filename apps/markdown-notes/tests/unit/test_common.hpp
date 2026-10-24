// Shared pieces of the Markdown Notes host tests: the CHECK macros, temp folders, file helpers. See run_tests.sh.
#pragma once

#include "fsutil.hpp"

#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

#ifndef APP_DIR
#define APP_DIR "../.."
#endif

extern int g_fail, g_checks;

#define CHECK(...)                                                                  \
    do {                                                                            \
        ++g_checks;                                                                 \
        if (!(__VA_ARGS__)) {                                                       \
            ++g_fail;                                                               \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #__VA_ARGS__);      \
        }                                                                           \
    } while (0)

#define CHECK_EQ(a, b)                                                              \
    do {                                                                            \
        ++g_checks;                                                                 \
        auto va_ = (a);                                                             \
        auto vb_ = (b);                                                             \
        if (!(va_ == vb_)) {                                                        \
            ++g_fail;                                                               \
            std::printf("FAIL %s:%d  %s == %s\n", __FILE__, __LINE__, #a, #b);      \
            std::printf("     got: [%s]\n", notes_test::show(va_).c_str());         \
            std::printf("    want: [%s]\n", notes_test::show(vb_).c_str());         \
        }                                                                           \
    } while (0)

namespace notes_test {

inline std::string show(const std::string &s) { return s; }
inline std::string show(const char *s) { return s ? s : "(null)"; }
inline std::string show(long long v) { return std::to_string(v); }
inline std::string show(unsigned long long v) { return std::to_string(v); }
inline std::string show(long v) { return std::to_string(v); }
inline std::string show(unsigned long v) { return std::to_string(v); }
inline std::string show(int v) { return std::to_string(v); }
inline std::string show(unsigned v) { return std::to_string(v); }
inline std::string show(bool v) { return v ? "true" : "false"; }
inline std::string show(char v) { return std::string(1, v); }

/* A fresh empty temp folder, removed by the destructor. */
struct TempDir {
    std::string path;
    TempDir()
    {
        const char *t = std::getenv("TMPDIR");
        std::string tmpl = std::string(t && *t ? t : "/tmp") + "/mdnotes-test-XXXXXX";
        std::vector<char> b(tmpl.begin(), tmpl.end());
        b.push_back('\0');
        if (::mkdtemp(b.data())) path = b.data();
    }
    ~TempDir()
    {
        if (!path.empty()) mdnotes::remove_tree(path);
    }
    std::string operator/(const std::string &rel) const { return path + "/" + rel; }
    TempDir(const TempDir &) = delete;
    TempDir &operator=(const TempDir &) = delete;
};

/* Writes bytes as they are (folders made), for fixtures. */
inline bool put(const std::string &path, const std::string &bytes)
{
    mdnotes::make_dirs(mdnotes::dir_name(path));
    std::FILE *f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    const bool ok = std::fwrite(bytes.data(), 1, bytes.size(), f) == bytes.size();
    return std::fclose(f) == 0 && ok;
}

inline std::string get(const std::string &path)
{
    std::string s;
    mdnotes::read_file(path, s, 64u * 1024 * 1024);
    return s;
}

/* Sets a file's mtime (seconds) so that walk orders are deterministic. */
inline void set_mtime(const std::string &path, long seconds)
{
    struct timespec ts[2];
    ts[0].tv_sec = seconds;
    ts[0].tv_nsec = 0;
    ts[1] = ts[0];
    ::utimensat(AT_FDCWD, path.c_str(), ts, 0);
}

} // namespace notes_test
