#!/usr/bin/env python3
"""Check that the screen requirements of every registry entry match the app's packaged .desktop.

    python3 tools/check_registry.py            # exit 0 when all pass, 1 otherwise

For every entry of registry.json (arm64) and registry-amd64.json (amd64) whose share_code is an apps/<id>/app.json:
  - "min_size" equals the X-MinSize of the app's .desktop (root/ with root.<arch>/ over it), else the app.json
    "min_size", else the field is absent;
  - "portrait_min_size" equals the .desktop X-Portrait, else absent;
  - a value present in the registry is a well-formed "WxH" (the launcher treats a malformed one as no restriction).
After editing a .desktop or an app.json "min_size", run tools/make_registry.py (with --no-build to keep the tested
packages) so the registry follows. The values are what the launcher computes: see tools/screen_info.py.
"""
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
sys.path.insert(0, HERE)
import screen_info  # noqa: E402

REGISTRY_FILES = {"registry.json": "arm64", "registry-amd64.json": "amd64"}
FIELDS = ("min_size", "portrait_min_size")


def check_repo(repo=REPO):
    """Every mismatch between the registries and the .desktop files, as "where: message" lines."""
    problems = []
    apps_dir = os.path.join(repo, "apps")
    by_code = {}
    for name in sorted(os.listdir(apps_dir)):
        path = os.path.join(apps_dir, name, "app.json")
        if os.path.isfile(path):
            with open(path, encoding="utf-8") as handle:
                by_code[json.load(handle).get("share_code")] = (name, path)
    for registry_file, arch in REGISTRY_FILES.items():
        path = os.path.join(repo, registry_file)
        if not os.path.isfile(path):
            continue
        with open(path, encoding="utf-8") as handle:
            registry = json.load(handle)
        for entry in registry.get("apps", []):
            code = entry.get("share_code", "?")
            where = "%s: %s" % (registry_file, code)
            for field in FIELDS:
                if field in entry and screen_info.parse_size(entry[field]) != entry[field]:
                    problems.append("%s: %s %r is not a normalised WxH" % (where, field, entry[field]))
            if code not in by_code:
                continue
            name, manifest = by_code[code]
            with open(manifest, encoding="utf-8") as handle:
                meta = json.load(handle)
            expected = screen_info.screen_fields(os.path.join(apps_dir, name), meta, arch)
            for field in FIELDS:
                if entry.get(field) != expected.get(field):
                    problems.append("%s: %s is %r but apps/%s gives %r (run tools/make_registry.py --no-build)"
                                    % (where, field, entry.get(field), name, expected.get(field)))
    return problems


def main():
    problems = check_repo()
    for line in problems:
        print(line)
    if problems:
        print("registry: %d problem(s)" % len(problems), file=sys.stderr)
        return 1
    print("registry: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
