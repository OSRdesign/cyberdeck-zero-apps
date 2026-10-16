/*
 * SPDX-License-Identifier: MIT
 */

#include "log.hpp"

#include <cstdio>
#include <ctime>
#include <sys/stat.h>

namespace meshzero {

FileLog::FileLog(std::string path, size_t max_bytes) : path_(std::move(path)), max_bytes_(max_bytes) {}

void FileLog::rotate_if_needed()
{
    if (!sized_) {
        struct stat st;
        size_ = ::stat(path_.c_str(), &st) == 0 ? static_cast<size_t>(st.st_size) : 0;
        sized_ = true;
    }
    if (size_ < max_bytes_) return;
    const std::string old = path_ + ".1";
    std::remove(old.c_str());
    std::rename(path_.c_str(), old.c_str());
    size_ = 0;
}

void FileLog::line(const std::string &text)
{
    rotate_if_needed();
    FILE *f = std::fopen(path_.c_str(), "ab");
    if (!f) return;
    const std::time_t now = std::time(nullptr);
    struct tm tmv;
    localtime_r(&now, &tmv);
    char stamp[32];
    std::strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S ", &tmv);
    std::string out = std::string(stamp) + text + "\n";
    const size_t n = std::fwrite(out.data(), 1, out.size(), f);
    std::fclose(f);
    size_ += n;
    written_ += n;
}

} // namespace meshzero
