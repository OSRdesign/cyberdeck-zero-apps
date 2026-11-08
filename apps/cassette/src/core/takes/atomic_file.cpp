/*
 * SPDX-License-Identifier: MIT
 */
#include "atomic_file.hpp"

#include <cerrno>
#include <fcntl.h>

namespace cassette {

std::string dir_name(const std::string &path) {
    const size_t k = path.find_last_of('/');
    if (k == std::string::npos) return ".";
    if (k == 0) return "/";
    return path.substr(0, k);
}

std::string base_name(const std::string &path) {
    const size_t k = path.find_last_of('/');
    return k == std::string::npos ? path : path.substr(k + 1);
}

bool path_exists(FileOps &ops, const std::string &path) {
    StatInfo st;
    return ops.stat(path, st) == 0;
}

int make_dirs(FileOps &ops, const std::string &path) {
    if (path.empty()) return -EINVAL;
    size_t pos = path[0] == '/' ? 1 : 0;
    while (pos <= path.size()) {
        size_t next = path.find('/', pos);
        if (next == std::string::npos) next = path.size();
        if (next > pos) {
            int r = ops.mkdir(path.substr(0, next));
            if (r < 0) return r;
        }
        pos = next + 1;
    }
    return 0;
}

int atomic_write_file(FileOps &ops, const std::string &path, const std::string &body) {
    const std::string tmp = path + ".tmp";
    int fd = ops.open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return fd;
    int r = write_all(ops, fd, body.data(), body.size());
    if (r == 0) r = ops.fsync(fd);
    int c = ops.close(fd);
    if (r == 0) r = c;
    if (r == 0) r = ops.rename(tmp, path);
    if (r < 0) {
        ops.unlink(tmp);
        return r;
    }
    return ops.fsync_dir(dir_name(path));
}

int read_small_file(FileOps &ops, const std::string &path, std::string &out, size_t max_bytes) {
    out.clear();
    StatInfo st;
    int r = ops.stat(path, st);
    if (r < 0) return r;
    if (st.size > max_bytes) return -EFBIG;
    int fd = ops.open(path, O_RDONLY, 0);
    if (fd < 0) return fd;
    out.resize(static_cast<size_t>(st.size));
    size_t got = 0;
    while (got < out.size()) {
        ssize_t k = ops.pread(fd, &out[got], out.size() - got, got);
        if (k < 0) {
            ops.close(fd);
            out.clear();
            return static_cast<int>(k);
        }
        if (k == 0) break;
        got += static_cast<size_t>(k);
    }
    ops.close(fd);
    out.resize(got);
    return 0;
}

}  // namespace cassette
