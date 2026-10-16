/*
 * SPDX-License-Identifier: MIT
 *
 * viz1090 intro: the airport list (see airports.hpp). The format is the one load_airports() of the 0.1 intro read.
 */
#include "airports.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>

namespace viz {

namespace {

constexpr std::uint32_t kMaxAirports = 200000; /* a sanity cap: the real file has about 4600 */

std::uint32_t u32le(const unsigned char *p)
{
    return static_cast<std::uint32_t>(p[0]) | static_cast<std::uint32_t>(p[1]) << 8 |
           static_cast<std::uint32_t>(p[2]) << 16 | static_cast<std::uint32_t>(p[3]) << 24;
}

std::int32_t i32le(const unsigned char *p)
{
    return static_cast<std::int32_t>(u32le(p));
}

} // namespace

std::string world_map_path()
{
    const char *env = std::getenv("VIZ_WORLD");
    return env && env[0] ? std::string(env) : std::string("/opt/viz1090/world-map.bin");
}

bool AirportList::load(const std::string &path)
{
    airports_.clear();
    loaded_ = false;
    std::ifstream file(path, std::ios::binary);
    unsigned char header[28];
    if (!file || !file.read(reinterpret_cast<char *>(header), sizeof(header)) ||
        std::memcmp(header, "VIZWORLD", 8) != 0)
        return false;
    const std::uint64_t chunks = u32le(header + 8), points = u32le(header + 12);
    const std::uint32_t count = u32le(header + 16);
    if (count > kMaxAirports) return false;
    file.seekg(static_cast<std::streamoff>(28 + chunks * 24 + points * 8));
    if (!file) return false;
    std::vector<unsigned char> records(static_cast<std::size_t>(count) * 16);
    if (count > 0 && !file.read(reinterpret_cast<char *>(records.data()), static_cast<std::streamsize>(records.size())))
        return false;
    airports_.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        const unsigned char *r = records.data() + static_cast<std::size_t>(i) * 16;
        Airport a;
        a.lat = i32le(r) / 10000.0;
        a.lon = i32le(r + 4) / 10000.0;
        a.code.assign(reinterpret_cast<const char *>(r + 8), strnlen(reinterpret_cast<const char *>(r + 8), 8));
        if (a.code.empty() || a.lat < -90 || a.lat > 90 || a.lon < -180 || a.lon > 180) continue;
        airports_.push_back(a);
    }
    std::stable_sort(airports_.begin(), airports_.end(),
                     [](const Airport &x, const Airport &y) { return x.code < y.code; });
    loaded_ = true;
    return true;
}

const Airport *AirportList::find(const std::string &code) const
{
    if (code.empty()) return nullptr;
    const auto it = std::lower_bound(airports_.begin(), airports_.end(), code,
                                     [](const Airport &a, const std::string &c) { return a.code < c; });
    return it != airports_.end() && it->code == code ? &*it : nullptr;
}

} // namespace viz
