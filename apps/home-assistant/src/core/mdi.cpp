/*
 * SPDX-License-Identifier: MIT
 *
 * Home Assistant app core: Material Design Icons names (see mdi.hpp).
 */
#include "mdi.hpp"

#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <mutex>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

namespace ha {

namespace {

constexpr std::size_t kHead = 24;

std::uint32_t u32(const unsigned char *p)
{
    return static_cast<std::uint32_t>(p[0]) | static_cast<std::uint32_t>(p[1]) << 8 |
           static_cast<std::uint32_t>(p[2]) << 16 | static_cast<std::uint32_t>(p[3]) << 24;
}

std::mutex g_mutex;

} // namespace

MdiNames::~MdiNames()
{
    if (mapped_ && data_) ::munmap(const_cast<unsigned char *>(data_), len_);
}

bool MdiNames::open(const std::string &path)
{
    if (data_) return loaded();
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return false;
    struct stat st;
    if (::fstat(fd, &st) != 0 || st.st_size < static_cast<off_t>(kHead) || st.st_size > (16 << 20)) {
        ::close(fd);
        return false;
    }
    void *p = ::mmap(nullptr, static_cast<std::size_t>(st.st_size), PROT_READ, MAP_PRIVATE, fd, 0);
    ::close(fd);
    if (p == MAP_FAILED) return false;
    const auto *d = static_cast<const unsigned char *>(p);
    const std::size_t len = static_cast<std::size_t>(st.st_size);
    const std::size_t count = u32(d + 4);
    const std::size_t blob = kHead + count * 4;
    bool ok = std::memcmp(d, "MDN1", 4) == 0 && count > 0 && count < (1u << 20) && blob < len;
    /* every offset inside the blob, each entry's name and codepoint inside the file */
    for (std::size_t i = 0; ok && i < count; ++i) {
        const std::size_t at = blob + u32(d + kHead + i * 4);
        const void *nul = at < len ? std::memchr(d + at, 0, len - at) : nullptr;
        ok = nul && static_cast<const unsigned char *>(nul) + 4 <= d + len;
    }
    if (!ok) {
        ::munmap(p, len);
        return false;
    }
    data_ = d;
    len_ = len;
    count_ = count;
    mapped_ = true;
    return true;
}

std::string MdiNames::version() const
{
    if (!data_) return std::string();
    const char *v = reinterpret_cast<const char *>(data_ + 8);
    return std::string(v, strnlen(v, 16));
}

std::uint32_t MdiNames::lookup(const std::string &name) const
{
    if (!data_ || name.empty()) return 0;
    std::size_t start = 0;
    const std::size_t colon = name.find(':');
    if (colon != std::string::npos) {
        if (name.compare(0, colon, "mdi") != 0) return 0;
        start = colon + 1;
    }
    const char *key = name.c_str() + start;
    const std::size_t blob = kHead + count_ * 4;
    std::size_t lo = 0, hi = count_;
    while (lo < hi) {
        const std::size_t mid = lo + (hi - lo) / 2;
        const unsigned char *entry = data_ + blob + u32(data_ + kHead + mid * 4);
        const int c = std::strcmp(reinterpret_cast<const char *>(entry), key);
        if (c == 0) {
            const unsigned char *cp = entry + std::strlen(reinterpret_cast<const char *>(entry)) + 1;
            return static_cast<std::uint32_t>(cp[0]) | static_cast<std::uint32_t>(cp[1]) << 8 |
                   static_cast<std::uint32_t>(cp[2]) << 16;
        }
        if (c < 0) lo = mid + 1;
        else hi = mid;
    }
    return 0;
}

static MdiNames &table()
{
    static MdiNames t;
    return t;
}

void set_mdi_path(const std::string &path)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    std::string p = path;
    if (const char *env = std::getenv("HA_MDI_NAMES"))
        if (*env) p = env;
    if (!table().loaded() && !p.empty()) table().open(p);
}

const MdiNames &mdi()
{
    return table();
}

std::string utf8_of(std::uint32_t cp)
{
    std::string s;
    if (cp == 0 || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return s;
    if (cp < 0x80) {
        s += static_cast<char>(cp);
    } else if (cp < 0x800) {
        s += static_cast<char>(0xC0 | (cp >> 6));
        s += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        s += static_cast<char>(0xE0 | (cp >> 12));
        s += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        s += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        s += static_cast<char>(0xF0 | (cp >> 18));
        s += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        s += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        s += static_cast<char>(0x80 | (cp & 0x3F));
    }
    return s;
}

} // namespace ha
