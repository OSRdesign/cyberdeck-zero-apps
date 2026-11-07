/*
 * SPDX-License-Identifier: MIT
 *
 * Deck Atlas: an archive record's extra fields (see record.hpp).
 */
#include "record.hpp"

#include <cerrno>
#include <cstdlib>
#include <sstream>

namespace atlas {

ArchiveRecord parse_archive_record(const std::string &text)
{
    ArchiveRecord r;
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const std::size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string key = line.substr(0, eq), value = line.substr(eq + 1);
        if (key == "tiles") {
            char *end = nullptr;
            errno = 0;
            const unsigned long long v = std::strtoull(value.c_str(), &end, 10);
            if (errno == 0 && end != value.c_str() && *end == '\0' && value[0] != '-') r.tiles = v;
        } else if (key == "imported") {
            r.imported = value;
        } else if (key == "source_path") {
            r.source_path = value;
        }
    }
    return r;
}

} // namespace atlas
