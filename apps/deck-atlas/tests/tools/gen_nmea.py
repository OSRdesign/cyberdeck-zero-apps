#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Deck Atlas: writes the SYNTHETIC NMEA feeds of the GPS scenes (pure standard library, deterministic).

Nothing here is a recording: every position is computed from one invented point in the middle of the synthetic
fixture tiles' area (zoom-15 tile 16590.62 / 11268.55, see gen_fixture_tiles.py), every sentence carries a valid
checksum. The feeds are played by the app's demonstration GPS port (src/core/gps_demo.cpp): one sentence per line,
"#wait N" pauses the feed N ms.

    python3 gen_nmea.py <out folder>      (build.sh passes tests/gps-feeds; the files are never packaged or committed)

Files: searching (receiver talks, no fix), fix2d, fix3d (nine satellites), moving (19 km/h, course 70), follow (a fix,
a pause, then a walk east: the map follows), stale (one epoch, then silence), noise (garbage before valid sentences),
weak3d (a 3D fix with four satellites used: weak geometry), sky (a multi-constellation 3D fix as a GN receiver sends it:
GPS and SBAS satellites under $GP, GLONASS under $GL, one $GNGSA per system with the NMEA 4.1 system id, multi-message
GSV groups, one satellite in view but not tracked), skylost (the same sky, then silence). The satellite geometry (PRNs,
elevations, azimuths, SNRs) is invented: a fixed table, not a recording and not a real almanac.
"""
import math
import os
import sys

DATE = "150126"                  # an invented date (2026-01-15), ddmmyy
START = (10, 15, 30)             # hh, mm, ss


def tile_to_latlon(x, y, z=15):
    n = 2.0 ** z
    lon = x / n * 360.0 - 180.0
    lat = math.degrees(math.atan(math.sinh(math.pi * (1 - 2 * y / n))))
    return lat, lon


LAT0, LON0 = tile_to_latlon(16590.62, 11268.55)


def frame(body):
    c = 0
    for ch in body:
        c ^= ord(ch)
    return "$%s*%02X" % (body, c)


def dm(value, is_lat):
    hemi = ("N" if value >= 0 else "S") if is_lat else ("E" if value >= 0 else "W")
    a = abs(value)
    d = int(a)
    m = (a - d) * 60.0
    return ("%02d%08.5f" if is_lat else "%03d%08.5f") % (d, m), hemi


def hms(t):
    s = START[0] * 3600 + START[1] * 60 + START[2] + t
    return "%02d%02d%02d.00" % ((s // 3600) % 24, (s // 60) % 60, s % 60)


def gsv(n_view, talker="GP"):
    prn = list(range(2, 2 + min(n_view, 4)))
    f = ",".join("%02d,%02d,%03d,%02d" % (p, 20 + 7 * i, 40 * i + 30, 28 + 3 * i) for i, p in enumerate(prn))
    return frame("%sGSV,1,1,%02d,%s" % (talker, n_view, f))


def epoch(t, lat, lon, kind, sats_used, in_view, hdop, speed_kn, course, alt):
    """The sentences of one second. kind: 0 no fix, 2 2D, 3 3D."""
    out = []
    if kind == 0:
        out.append(frame("GNRMC,%s,V,,,,,,,%s,,,N,V" % (hms(t), DATE)))
        out.append(frame("GNGGA,%s,,,,,0,00,99.99,,,,,," % hms(t)))
        out.append(frame("GNGSA,A,1,,,,,,,,,,,,,99.99,99.99,99.99,1"))
        out.append(gsv(in_view))
        return out
    la, ha = dm(lat, True)
    lo, ho = dm(lon, False)
    crs = "%.1f" % course if speed_kn >= 0.5 else ""
    out.append(frame("GNRMC,%s,A,%s,%s,%s,%s,%.2f,%s,%s,,,A,V" % (hms(t), la, ha, lo, ho, speed_kn, crs, DATE)))
    altf = ("%.1f" % alt) if kind == 3 else ""
    out.append(frame("GNGGA,%s,%s,%s,%s,%s,1,%02d,%.2f,%s,M,47.3,M,," % (hms(t), la, ha, lo, ho, sats_used, hdop, altf)))
    prn = ",".join("%02d" % p for p in range(2, 2 + min(sats_used, 12)))
    pad = "," * (12 - min(sats_used, 12))
    out.append(frame("GNGSA,A,%d,%s%s,%.2f,%.2f,%.2f,1" % (kind, prn, pad, hdop * 1.3, hdop, hdop * 1.1)))
    out.append(gsv(in_view))
    return out


# invented sky: (prn, elevation, azimuth, snr or None = in view, not tracked)
SKY_GPS = [(2, 62, 37, 41), (5, 48, 78, 38), (7, 35, 119, 36), (13, 22, 160, 29), (15, 15, 201, 24), (20, 71, 242, 44),
           (24, 10, 283, None), (29, 40, 324, 33), (31, 55, 5, 40), (33, 30, 150, 27), (135, 25, 190, 25)]
SKY_GLO = [(66, 58, 20, 39), (67, 44, 71, 35), (74, 30, 122, 31), (81, 18, 173, 26), (82, 66, 224, 42), (88, 12, 275, 21)]
USED_GPS = [2, 5, 7, 13, 15, 20, 29, 31, 33]   # eight GPS and one SBAS (PRN 33), listed in the GPS $GNGSA
USED_GLO = [66, 67, 74, 82]


def gsv_group(talker, sats):
    """The GSV messages of one talker: four satellites per sentence, total / number counted."""
    out = []
    n = len(sats)
    total = (n + 3) // 4
    for k in range(total):
        blocks = []
        for prn, el, az, snr in sats[4 * k:4 * k + 4]:
            blocks.append("%02d,%02d,%03d,%s" % (prn, el, az, "" if snr is None else "%02d" % snr))
        out.append(frame("%sGSV,%d,%d,%02d,%s" % (talker, total, k + 1, n, ",".join(blocks))))
    return out


def gsa_line(mode, used, pdop, hdop, vdop, system_id):
    prn = ",".join("%02d" % p for p in used[:12])
    pad = "," * (12 - min(len(used), 12))
    return frame("GNGSA,A,%d,%s%s,%.2f,%.2f,%.2f,%d" % (mode, prn, pad, pdop, hdop, vdop, system_id))


def sky_epoch(t, lat, lon, hdop=0.9, alt=41.5):
    """One second of a good 3D fix in the multi-constellation form of a GN receiver."""
    la, ha = dm(lat, True)
    lo, ho = dm(lon, False)
    out = [frame("GNRMC,%s,A,%s,%s,%s,%s,0.10,,%s,,,A,V" % (hms(t), la, ha, lo, ho, DATE)),
           frame("GNGGA,%s,%s,%s,%s,%s,1,%02d,%.2f,%.1f,M,47.3,M,," % (hms(t), la, ha, lo, ho, len(USED_GPS) + len(USED_GLO), hdop, alt)),
           gsa_line(3, USED_GPS, hdop * 1.5, hdop, hdop * 1.2, 1),
           gsa_line(3, USED_GLO, hdop * 1.5, hdop, hdop * 1.2, 2)]
    out += gsv_group("GP", SKY_GPS)
    out += gsv_group("GL", SKY_GLO)
    return out


def feed(epochs):
    lines = ["# synthetic NMEA feed (tests/tools/gen_nmea.py): invented position, valid checksums"]
    for i, e in enumerate(epochs):
        if i:
            lines.append("#wait 1000")
        lines.extend(e)
    return "\n".join(lines) + "\n"


def offset(lat, lon, north_m, east_m):
    return lat + north_m / 111320.0, lon + east_m / (111320.0 * math.cos(math.radians(lat)))


def main(out_dir):
    os.makedirs(out_dir, exist_ok=True)
    files = {}
    files["searching"] = feed([epoch(t, 0, 0, 0, 0, 5 + (t % 2), 99.99, 0, 0, 0) for t in range(4)])
    files["fix2d"] = feed([epoch(t, LAT0, LON0, 2, 4, 6, 4.0, 0.0, 0, 0) for t in range(4)])
    files["fix3d"] = feed([epoch(t, LAT0, LON0, 3, 9, 12, 2.4, 0.1, 0, 41.5) for t in range(4)])
    files["moving"] = feed([epoch(t, *offset(LAT0, LON0, 0, 5.2 * t), 3, 9, 12, 1.1, 10.1, 70.0, 41.5) for t in range(4)])
    # a fix, then (after the scene turned follow on) a walk east, 1 s apart, 12 m a step: the map follows
    walk = [epoch(t, LAT0, LON0, 3, 8, 11, 1.2, 0.0, 0, 41.0) for t in range(2)]
    for k in range(1, 40):
        la, lo = offset(LAT0, LON0, 0, 12.0 * k)
        walk.append(epoch(1 + k, la, lo, 3, 8, 11, 1.2, 23.3, 90.0, 41.0))
    files["follow"] = feed(walk)
    files["weak3d"] = feed([epoch(t, LAT0, LON0, 3, 4, 8, 1.8, 0.1, 0, 41.5) for t in range(4)])
    files["sky"] = feed([sky_epoch(t, LAT0, LON0) for t in range(4)])
    files["skylost"] = feed([sky_epoch(0, LAT0, LON0)])
    files["stale"] = feed([epoch(0, LAT0, LON0, 3, 9, 12, 1.1, 0.0, 0, 41.5)] + [])
    # line noise and a binary burst before the sentences, a broken checksum between them
    noisy = ["# garbage, then valid sentences (the reader must drop the garbage)", "\x7f\x01\x02garbage$GPGGA,broken*00"]
    noisy += epoch(0, LAT0, LON0, 3, 9, 12, 1.1, 0.0, 0, 41.5)
    files["noise"] = "\n".join(noisy) + "\n"
    for name, text in files.items():
        with open(os.path.join(out_dir, name + ".nmea"), "w", newline="\n") as f:
            f.write(text)


if __name__ == "__main__":
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    main(sys.argv[1])
