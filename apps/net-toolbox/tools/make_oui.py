#!/usr/bin/env python3
"""Builds root/usr/share/APPLaunch/share/net-toolbox-oui.tsv (MAC prefix -> vendor) from the IEEE OUI registry.

Net Toolbox's Local devices tool shows the vendor of each MAC address from this table. It is a modified, abridged
derivative of the public IEEE OUI listing: the 24-bit prefixes are kept and the company names are shortened to fit the
small screen (corporate suffixes dropped, at most 30 characters). See root/usr/share/doc/net-toolbox/copyright.

    python3 tools/make_oui.py            # downloads https://standards-oui.ieee.org/oui/oui.csv
    python3 tools/make_oui.py oui.csv    # or use a local copy
"""
import csv, io, os, re, sys, urllib.request

URL = "https://standards-oui.ieee.org/oui/oui.csv"
OUT = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                   "root", "usr", "share", "APPLaunch", "share", "net-toolbox-oui.tsv")


def short(name):
    name = re.sub(r"\s+", " ", name).strip()
    # drop corporate suffixes to keep names short on a small screen
    name = re.sub(r"[,.]?\s+(Co\.?,? ?Ltd\.?|Corporation|Corp\.?|Incorporated|Inc\.?|GmbH|LLC|Ltd\.?|Limited|S\.?A\.?|B\.?V\.?|AG|PLC|Pty|Company)\b.*$", "", name, flags=re.I)
    return name[:30].strip(" ,.")


if len(sys.argv) > 1:
    text = open(sys.argv[1], encoding="utf-8", errors="replace").read()
else:
    request = urllib.request.Request(URL, headers={"User-Agent": "Mozilla/5.0 (cyberdeck-zero-net-toolbox)"})
    text = urllib.request.urlopen(request, timeout=60).read().decode("utf-8", errors="replace")

rows = {}
for row in csv.reader(io.StringIO(text)):
    if len(row) >= 3 and re.fullmatch(r"[0-9A-Fa-f]{6}", row[1]):
        rows[row[1].upper()] = short(row[2])
with open(OUT, "w", encoding="utf-8", newline="\n") as out:
    for prefix in sorted(rows):
        out.write(f"{prefix}\t{rows[prefix]}\n")
print(len(rows), "prefixes written to", OUT)
