#!/usr/bin/env python3
"""Cuts a window around a position out of world-map.bin and writes the four files viz1090 reads
(mapdata.bin, mapnames, airportdata.bin, airportnames). Pure Python 3, no dependencies, under a second.

    python3 extract_map.py --world world-map.bin --lat 48.85 --lon 2.35 [--radius 500] --out DIR

--radius is in nautical miles (default 500). See make_world.py for the file format.
"""
import argparse, math, os, struct, sys


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--world", required=True)
    parser.add_argument("--lat", type=float, required=True)
    parser.add_argument("--lon", type=float, required=True)
    parser.add_argument("--radius", type=float, default=500.0, help="nautical miles")
    parser.add_argument("--out", required=True)
    args = parser.parse_args()

    half_lat = args.radius / 60.0
    half_lon = min(180.0, half_lat / max(0.1, math.cos(math.radians(args.lat))))
    lat_lo, lat_hi = int((args.lat - half_lat) * 10000), int((args.lat + half_lat) * 10000)
    lon_lo, lon_hi = int((args.lon - half_lon) * 10000), int((args.lon + half_lon) * 10000)

    with open(args.world, "rb") as handle:
        data = handle.read()
    if data[:8] != b"VIZWORLD":
        sys.exit("not a world-map.bin file")
    chunks, points, airports, runways, places = struct.unpack_from("<5I", data, 8)
    offset = 28
    table = offset
    offset += chunks * 24
    point_base = offset
    offset += points * 8
    airport_base = offset
    offset += airports * 16
    runway_base = offset
    offset += runways * 16
    place_base = offset

    def inside(lat, lon):
        return lat_lo <= lat <= lat_hi and lon_lo <= lon <= lon_hi

    values = []
    lines = 0
    for lat_min, lat_max, lon_min, lon_max, first, count in struct.iter_unpack("<4i2I", data[table:table + chunks * 24]):
        if lat_max < lat_lo or lat_min > lat_hi or lon_max < lon_lo or lon_min > lon_hi:
            continue
        pts = struct.unpack_from("<%di" % (count * 2), data, point_base + first * 8)
        for i in range(count):
            values += [pts[2 * i + 1] / 10000.0, pts[2 * i] / 10000.0]      # lon, lat
        values += [0.0, 0.0]                                                # end of line
        lines += 1
    os.makedirs(args.out, exist_ok=True)
    map_points = len(values) // 2
    with open(os.path.join(args.out, "mapdata.bin"), "wb") as out:
        out.write(struct.pack("<%df" % len(values), *values))

    names = 0
    with open(os.path.join(args.out, "airportnames"), "w", encoding="utf-8", newline="\n") as out:
        for lat, lon, code in struct.iter_unpack("<2i8s", data[airport_base:airport_base + airports * 16]):
            if inside(lat, lon):
                out.write("%s %s %s\n" % (lon / 10000.0, lat / 10000.0, code.split(b"\0")[0].decode("ascii", "replace")))
                names += 1

    values = []
    count_runways = 0
    for lat1, lon1, lat2, lon2 in struct.iter_unpack("<4i", data[runway_base:runway_base + runways * 16]):
        if inside(lat1, lon1) or inside(lat2, lon2):
            values += [lon1 / 10000.0, lat1 / 10000.0, lon2 / 10000.0, lat2 / 10000.0, 0.0, 0.0]
            count_runways += 1
    with open(os.path.join(args.out, "airportdata.bin"), "wb") as out:
        out.write(struct.pack("<%df" % len(values), *values))

    towns = 0
    with open(os.path.join(args.out, "mapnames"), "w", encoding="utf-8", newline="\n") as out:
        for lat, lon, population, name in struct.iter_unpack("<2iI32s", data[place_base:place_base + places * 44]):
            if inside(lat, lon):
                out.write("%s %s %s\n" % (lon / 10000.0, lat / 10000.0, name.split(b"\0")[0].decode("utf-8", "replace")))
                towns += 1
    print("map window: %d lines, %d points, %d airports, %d runways, %d towns" % (
        lines, map_points, names, count_runways, towns))


if __name__ == "__main__":
    main()
