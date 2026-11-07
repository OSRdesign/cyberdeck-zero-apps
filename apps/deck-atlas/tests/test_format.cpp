/*
 * SPDX-License-Identifier: MIT
 *
 * Deck Atlas tests: the texts of the pages (sizes, counts, zooms, the Settings values, coordinates, shortening).
 */
#include "check.hpp"
#include "format.hpp"
#include "record.hpp"

using namespace atlas;

void test_format()
{
    CHECK_EQ(bytes_label(0), std::string("0 B"));
    CHECK_EQ(bytes_label(820), std::string("820 B"));
    CHECK_EQ(bytes_label(12 * 1024 + 300), std::string("12 KB"));
    CHECK_EQ(bytes_label(229169), std::string("224 KB"));
    CHECK_EQ(bytes_label(1048575), std::string("1.0 MB"));
    CHECK_EQ(bytes_label(3565158), std::string("3.4 MB"));
    CHECK_EQ(bytes_label(289406976ull), std::string("276 MB"));
    CHECK_EQ(bytes_label(512ull << 20), std::string("512 MB"));
    CHECK_EQ(bytes_label(1288490188ull), std::string("1.2 GB"));

    CHECK_EQ(count_label(0), std::string("0"));
    CHECK_EQ(count_label(999), std::string("999"));
    CHECK_EQ(count_label(10326), std::string("10,326"));
    CHECK_EQ(count_label(1234567), std::string("1,234,567"));

    CHECK_EQ(zoom_label(1, 17), std::string("z1-17"));
    CHECK_EQ(zoom_label(14, 14), std::string("z14"));
    CHECK_EQ(zoom_label(17, 1), std::string("z1-17"));
    CHECK_EQ(zoom_label(-1, 5), std::string(""));

    CHECK_EQ(std::string(units_label(Units::Km)), std::string("km"));
    CHECK_EQ(std::string(units_label(Units::Mi)), std::string("mi"));
    CHECK_EQ(scale_label(0.0f, 1.5f), std::string("Auto (1.5x)"));
    CHECK_EQ(scale_label(0.0f, 2.0f), std::string("Auto (2x)"));
    CHECK_EQ(scale_label(0.0f, 0.0f), std::string("Auto"));
    CHECK_EQ(scale_label(1.0f, 2.0f), std::string("1x"));
    CHECK_EQ(scale_label(2.0f, 2.0f), std::string("2x"));
    CHECK_EQ(cache_label(0, 512ull << 20), std::string("Auto (512 MB)"));
    CHECK_EQ(cache_label(0, 0), std::string("Auto"));
    CHECK_EQ(cache_label(128, 0), std::string("128 MB"));
    CHECK_EQ(cache_label(1024, 0), std::string("1 GB"));
    CHECK_EQ(std::string(on_off(true)), std::string("On"));

    CHECK_EQ(latlon_label(48.91591, 2.27412), std::string("48.916 N, 2.274 E"));
    CHECK_EQ(latlon_label(-33.8688, -70.6693), std::string("33.869 S, 70.669 W"));
    CHECK_EQ(bounds_label(2.1, 48.8, 2.45, 48.99), std::string("48.80-48.99 N, 2.10-2.45 E"));
    CHECK_EQ(bounds_label(-0.5, -1.2, 1.2, 0.5), std::string("1.20 S-0.50 N, 0.50 W-1.20 E"));
    CHECK_EQ(bounds_label(-71.0, -34.0, -70.5, -33.5), std::string("33.50-34.00 S, 70.50-71.00 W"));

    CHECK_EQ(ellipsize_middle("short", 10), std::string("short"));
    CHECK_EQ(ellipsize_middle("/media/deck/MAPS/europe/paris.mbtiles", 20), std::string("/media/d...s.mbtiles"));
    CHECK_EQ(utf8_chars(ellipsize_middle("/media/deck/MAPS/europe/paris.mbtiles", 20)), 20);
    CHECK_EQ(utf8_chars("caf\xc3\xa9"), 4);
    CHECK_EQ(utf8_chars(ellipsize_middle("\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9", 5)), 5);
    CHECK_EQ(ellipsize_middle("abcdef", 2), std::string(".."));

    CHECK_EQ(tile_rate_label(812.4), std::string("812 tiles/s"));
    CHECK_EQ(tile_rate_label(5070.6), std::string("5,071 tiles/s"));
    CHECK_EQ(tile_rate_label(2.25), std::string("2.2 tiles/s"));
    CHECK_EQ(tile_rate_label(0), std::string(""));
    CHECK_EQ(byte_rate_label(35.0 * 1024 * 1024), std::string("35 MB/s"));
    CHECK_EQ(eta_label(12.2), std::string("about 12 s"));
    CHECK_EQ(eta_label(0.2), std::string("about 1 s"));
    CHECK_EQ(eta_label(185), std::string("about 3 min"));
    CHECK_EQ(eta_label(3900), std::string("about 1 h 5 min"));
    CHECK_EQ(eta_label(-1), std::string(""));
    CHECK_EQ(date_label("2026-11-07T10:20:30Z"), std::string("2026-11-07"));
    CHECK_EQ(date_label("yesterday"), std::string("yesterday"));

    /* the archive record's extra fields (the engine's key=value file) */
    const ArchiveRecord r = parse_archive_record("v=1\nid=tiles\ntitle=tiles\nkind=mbtiles\nmode=converted\n"
                                                 "source_path=/home/deck/Downloads/tiles\ntiles=10326\n"
                                                 "imported=2026-11-07T10:20:30Z\nfuture=1\n");
    CHECK_EQ(r.tiles, static_cast<std::uint64_t>(10326));
    CHECK_EQ(r.imported, std::string("2026-11-07T10:20:30Z"));
    CHECK_EQ(r.source_path, std::string("/home/deck/Downloads/tiles"));
    CHECK_EQ(parse_archive_record("tiles=-3\n").tiles, static_cast<std::uint64_t>(0));
    CHECK_EQ(parse_archive_record("tiles=12x\n").tiles, static_cast<std::uint64_t>(0));
    CHECK_EQ(parse_archive_record("").tiles, static_cast<std::uint64_t>(0));
}
