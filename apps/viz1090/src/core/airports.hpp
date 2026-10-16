/*
 * SPDX-License-Identifier: MIT
 *
 * viz1090 intro: the airport list of the world map file (/opt/viz1090/world-map.bin, written by build/make_world.py):
 *
 *   "VIZWORLD", uint32 chunks, points, airports, runways, places     (28 bytes, little endian)
 *   chunks x 24 bytes, points x 8 bytes                               (the coastlines: skipped)
 *   airports x { int32 lat, lon (degrees x 10000); char code[8] }    (the IATA code, NUL padded)
 *
 * Only the airport records are read (about 4600, 75 KB). Pure C++, no LVGL: unit-tested on a small synthetic file.
 */
#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace viz {

struct Airport {
    std::string code;
    double lat = 0, lon = 0;
};

/* $VIZ_WORLD (tests), else /opt/viz1090/world-map.bin */
std::string world_map_path();

class AirportList {
public:
    /* false (and an empty list) when the file is missing or not a world map */
    bool load(const std::string &path);
    bool loaded() const { return loaded_; }
    std::size_t size() const { return airports_.size(); }
    /* the airport with this code (upper case), NULL when there is none */
    const Airport *find(const std::string &code) const;

private:
    std::vector<Airport> airports_; /* sorted by code */
    bool loaded_ = false;
};

} // namespace viz
