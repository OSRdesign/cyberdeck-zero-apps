/*
 * SPDX-License-Identifier: MIT
 *
 * The app's debug log: one text file (<data folder>/mesh-hop.log), a timestamp and a line per event. Bounded: when the file
 * passes max_bytes it is renamed to mesh-hop.log.1 (the previous .1 is dropped) and a new one starts. The Controller reads it on
 * the deck to confirm what the board answered (for example the response code of a channel send). No message text is logged.
 */

#pragma once

#include <cstddef>
#include <string>

namespace meshzero {

class FileLog {
public:
    explicit FileLog(std::string path, size_t max_bytes = 64 * 1024);
    /* Appends "YYYY-MM-DD HH:MM:SS text". Never throws, never blocks long; failures are ignored. */
    void line(const std::string &text);
    const std::string &path() const { return path_; }
    size_t written() const { return written_; }

private:
    void rotate_if_needed();

    std::string path_;
    size_t max_bytes_;
    size_t size_ = 0;
    bool sized_ = false;
    size_t written_ = 0;
};

} // namespace meshzero
