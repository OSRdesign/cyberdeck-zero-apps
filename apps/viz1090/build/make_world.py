#!/usr/bin/env python3
"""Builds world-map.bin: the whole world's map data in one compact file, from open data.

viz1090 cannot load the whole world on a Raspberry Pi Zero 2W, so the deck cuts a window around your position
out of this file (extract_map.py) and gives viz1090 only that. Run this once on a PC:

    python3 make_world.py [--out world-map.bin] [--tolerance 0.004]

Sources (public domain / open data): Natural Earth 10m (states and provinces, populated places) through
github.com/martynafford/natural-earth-geojson, and OurAirports (airports with an IATA code, runways).

File format (little endian):
    "VIZWORLD"
    uint32 chunks, points, airports, runways, places
    chunks   x { int32 lat_min, lat_max, lon_min, lon_max; uint32 first_point; uint32 count }      (24 bytes)
    points   x { int32 lat, lon }                                                                  (1e-4 degrees)
    airports x { int32 lat, lon; char code[8] }
    runways  x { int32 lat1, lon1, lat2, lon2 }
    places   x { int32 lat, lon; uint32 population; char name[32] }
"""
import argparse, csv, json, os, struct, sys, urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
NE = "https://raw.githubusercontent.com/martynafford/natural-earth-geojson/master/10m/cultural/"
URLS = {
    "states": NE + "ne_10m_admin_1_states_provinces.json",
    "places": NE + "ne_10m_populated_places.json",
    "airports": "https://davidmegginson.github.io/ourairports-data/airports.csv",
    "runways": "https://davidmegginson.github.io/ourairports-data/runways.csv",
}
CHUNK = 64


def fetch(url, cache):
    os.makedirs(cache, exist_ok=True)
    path = os.path.join(cache, url.rsplit("/", 1)[1])
    if not os.path.exists(path):
        print("downloading", url, flush=True)
        request = urllib.request.Request(url, headers={"User-Agent": "cyberdeck-zero-launcher map builder"})
        with urllib.request.urlopen(request, timeout=300) as response, open(path, "wb") as out:
            out.write(response.read())
    return path


def f32(value):
    return struct.unpack("<f", struct.pack("<f", value))[0]


def simplify(points, tolerance):
    if len(points) < 4 or tolerance <= 0:
        return points
    keep = [False] * len(points)
    keep[0] = keep[-1] = True
    stack = [(0, len(points) - 1)]
    while stack:
        first, last = stack.pop()
        ax, ay = points[first]
        bx, by = points[last]
        dx, dy = bx - ax, by - ay
        norm = dx * dx + dy * dy
        best, index = 0.0, -1
        for i in range(first + 1, last):
            px, py = points[i]
            if norm == 0:
                d = (px - ax) ** 2 + (py - ay) ** 2
            else:
                t = max(0.0, min(1.0, ((px - ax) * dx + (py - ay) * dy) / norm))
                d = (px - ax - t * dx) ** 2 + (py - ay - t * dy) ** 2
            if d > best:
                best, index = d, i
        if index >= 0 and best > tolerance * tolerance:
            keep[index] = True
            stack.append((first, index))
            stack.append((index, last))
    return [p for p, k in zip(points, keep) if k]


def dedupe(points):
    out = []
    for lon, lat in points:
        point = (f32(lon), f32(lat))
        if not out or abs(point[0] - out[-1][0]) > 1e-5 or abs(point[1] - out[-1][1]) > 1e-5:
            out.append(point)
    return out


def rings(geometry):
    kind = geometry["type"]
    if kind == "Polygon":
        yield from geometry["coordinates"]
    elif kind == "MultiPolygon":
        for polygon in geometry["coordinates"]:
            yield from polygon
    elif kind == "LineString":
        yield geometry["coordinates"]
    elif kind == "MultiLineString":
        yield from geometry["coordinates"]


def e4(value):
    return int(round(value * 10000))


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--out", default=os.path.join(HERE, "world-map.bin"))
    parser.add_argument("--tolerance", type=float, default=0.004)
    parser.add_argument("--minpop", type=int, default=100000)
    parser.add_argument("--cache", default=os.path.join(HERE, ".cache"))
    args = parser.parse_args()

    chunks, points = [], []
    with open(fetch(URLS["states"], args.cache), encoding="utf-8") as handle:
        data = json.load(handle)
    for feature in data["features"]:
        if not feature.get("geometry"):
            continue
        for ring in rings(feature["geometry"]):
            line = dedupe(simplify([(p[0], p[1]) for p in ring], args.tolerance))
            for start in range(0, max(0, len(line) - 1), CHUNK - 1):
                part = line[start:start + CHUNK]
                if len(part) < 2:
                    continue
                lats = [e4(p[1]) for p in part]
                lons = [e4(p[0]) for p in part]
                chunks.append((min(lats), max(lats), min(lons), max(lons), len(points), len(part)))
                points.extend(zip(lats, lons))
    del data
    print("chunks %d, points %d" % (len(chunks), len(points)), flush=True)

    airports, wanted = [], {}
    with open(fetch(URLS["airports"], args.cache), encoding="utf-8", newline="") as handle:
        for row in csv.DictReader(handle):
            if row["type"] not in ("large_airport", "medium_airport") or not row["iata_code"]:
                continue
            try:
                lat, lon = float(row["latitude_deg"]), float(row["longitude_deg"])
            except ValueError:
                continue
            wanted[row["ident"]] = True
            airports.append((e4(lat), e4(lon), row["iata_code"][:7]))
    runways = []
    with open(fetch(URLS["runways"], args.cache), encoding="utf-8", newline="") as handle:
        for row in csv.DictReader(handle):
            if row["airport_ident"] not in wanted or row["closed"] == "1":
                continue
            try:
                ends = dedupe([(float(row["le_longitude_deg"]), float(row["le_latitude_deg"])),
                               (float(row["he_longitude_deg"]), float(row["he_latitude_deg"]))])
            except ValueError:
                continue
            if len(ends) == 2:
                runways.append((e4(ends[0][1]), e4(ends[0][0]), e4(ends[1][1]), e4(ends[1][0])))
    places = []
    with open(fetch(URLS["places"], args.cache), encoding="utf-8") as handle:
        for feature in json.load(handle)["features"]:
            props = feature["properties"]
            population = props.get("POP_MIN") or 0
            if population > args.minpop:
                lon, lat = feature["geometry"]["coordinates"][:2]
                places.append((e4(lat), e4(lon), int(population), props["NAME"].encode("utf-8")[:31]))

    with open(args.out, "wb") as out:
        out.write(b"VIZWORLD")
        out.write(struct.pack("<5I", len(chunks), len(points), len(airports), len(runways), len(places)))
        for c in chunks:
            out.write(struct.pack("<4i2I", *c))
        for lat, lon in points:
            out.write(struct.pack("<2i", lat, lon))
        for lat, lon, code in airports:
            out.write(struct.pack("<2i8s", lat, lon, code.encode("ascii", "replace")))
        for r in runways:
            out.write(struct.pack("<4i", *r))
        for lat, lon, population, name in places:
            out.write(struct.pack("<2iI32s", lat, lon, population, name))
    print("%s: %d chunks, %d points, %d airports, %d runways, %d places, %.1f MB" % (
        args.out, len(chunks), len(points), len(airports), len(runways), len(places), os.path.getsize(args.out) / 1e6))


if __name__ == "__main__":
    main()
