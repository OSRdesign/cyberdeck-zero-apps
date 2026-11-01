#!/usr/bin/env python3
"""Build the packages and registries (registry.json, registry-amd64.json) of this repository.

    python3 tools/make_registry.py                       # build every app, write both registries
    python3 tools/make_registry.py --owner me --repo my-apps
    python3 tools/make_registry.py --arch amd64          # only the amd64 packages and registry-amd64.json

For every apps/<id>/app.json and every architecture the app lists in "arch" (default ["arm64"]) it
  1. builds packages/<package>_<version>_<arch>.deb from apps/<id>/root/ with apps/<id>/root.<arch>/ laid over it
     (a file of root.<arch>/ replaces the one of root/ at the same path: the binary built for that architecture;
     tools/build_deb.py),
  2. adds an entry to that architecture's registry with the download URL, md5, sha256 and size of that package:
     registry.json for arm64 (the decks), registry-amd64.json for amd64 (PCs), the same schema. An app that does not
     list an architecture is not in its registry.

    "arch": ["arm64", "amd64"]        in app.json: root/ holds the arm64 binary, root.amd64/ the amd64 one

Each entry also carries the screen requirements the launcher reads from the app's packaged .desktop, so it can hide
the apps a board's screen cannot run (tools/screen_info.py has the exact semantics):
    "min_size": "WxH"           from X-MinSize (landscape minimum); absent when the .desktop has none (fits everywhere);
                                an app.json "min_size" fills it when the .desktop has none
    "portrait_min_size": "WxH"  from X-Portrait (the smallest canvas on a portrait launcher), only when declared

A plain run is idempotent: an entry whose package (version and sha256) did not change keeps its published_at and
updated_at, each registry file keeps its device_targets, and generated_at moves only when the content changed.

Every app it reads must keep the descriptor rule of tools/check_descriptors.py (summary <= 80 characters, summary +
newline + description <= 500 characters), or it stops.

An app whose app.json has  "draft": true  is skipped (no .deb, no registry entry). To build a local .deb of an
app without publishing it (a draft included), use --only <id> --out <folder> [--arch amd64]: it writes just that .deb
into the folder and touches neither packages/ nor the registries.

    python3 tools/make_registry.py --only meshzero --out C:/somewhere/outside/packages
    python3 tools/make_registry.py --only serial-console --arch amd64 --out C:/somewhere/outside/packages

URLs point to raw.githubusercontent.com/<owner>/<repo>/<branch>/..., so nothing but a public GitHub
repository is needed. owner/repo are taken from the git remote "origin" unless given.
"""
import argparse
import datetime
import hashlib
import json
import os
import re
import subprocess
import sys
import uuid

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import check_descriptors  # noqa: E402  (tools/check_descriptors.py: the 500-character descriptor rule)
import screen_info  # noqa: E402  (tools/screen_info.py: X-MinSize / X-Portrait of the .desktop)

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
NAMESPACE = uuid.UUID("6f1c2f0e-6a52-4a54-9d3b-2a3e5c7f9b10")   # fixed: the same share_code always gives the same uuid
ARCHES = ("arm64", "amd64")                                      # what an app may list in app.json "arch"
REGISTRY_FILES = {"arm64": "registry.json", "amd64": "registry-amd64.json"}


def detect_remote():
    try:
        url = subprocess.check_output(["git", "-C", REPO, "remote", "get-url", "origin"], text=True,
                                      stderr=subprocess.DEVNULL).strip()
    except Exception:
        return None, None
    match = re.search(r"github\.com[:/]([^/]+)/([^/.]+?)(?:\.git)?/?$", url)
    return (match.group(1), match.group(2)) if match else (None, None)


def digest(path):
    md5, sha = hashlib.md5(), hashlib.sha256()
    with open(path, "rb") as handle:
        for chunk in iter(lambda: handle.read(1 << 20), b""):
            md5.update(chunk)
            sha.update(chunk)
    return md5.hexdigest(), sha.hexdigest(), os.path.getsize(path)


def app_arches(meta, manifest_path):
    """The architectures an app is packaged for: app.json "arch", default ["arm64"]."""
    arches = meta.get("arch", ["arm64"])
    if not isinstance(arches, list) or not arches or any(arch not in ARCHES for arch in arches):
        sys.exit("%s: 'arch' must be a list of %s" % (manifest_path, ", ".join(ARCHES)))
    return arches


def build_package(name, meta, arch, out_dir, owner):
    """tools/build_deb.py: apps/<name>/root/ with root.<arch>/ over it, into out_dir."""
    app_dir = os.path.join(REPO, "apps", name)
    overlay = os.path.join(app_dir, "root." + arch)
    scripts = os.path.join(app_dir, "DEBIAN")
    subprocess.check_call([
        sys.executable, os.path.join(HERE, "build_deb.py"),
        "--root", os.path.join(app_dir, "root"),
        "--arch", arch,
        "--package", meta["package"], "--version", meta["version"],
        "--description", meta["summary"],
        "--maintainer", meta.get("maintainer", "%s <noreply@users.noreply.github.com>" % owner),
        "--homepage", meta.get("source_repo", ""),
        "--depends", meta.get("depends", ""),
        "--recommends", meta.get("recommends", ""),
        "--out", out_dir]
        + (["--overlay", overlay] if os.path.isdir(overlay) else [])
        + (["--scripts", scripts] if os.path.isdir(scripts) else []))


def registry_entry(name, meta, base, package, package_path, owner, repo, arch="arm64", previous=None):
    md5, sha256, size = digest(package_path)
    apps_dir = os.path.join(REPO, "apps")
    now = datetime.datetime.now(datetime.timezone.utc).replace(microsecond=0).isoformat()
    asset_base = "%s/apps/%s" % (base, name)
    icon = asset_base + "/icon.png" if os.path.isfile(os.path.join(apps_dir, name, "icon.png")) else ""
    shots_dir = os.path.join(apps_dir, name, "screenshots")
    shots = ["%s/screenshots/%s" % (asset_base, f) for f in sorted(os.listdir(shots_dir))
             if f.lower().endswith(".png")] if os.path.isdir(shots_dir) else []
    text = {"title": meta["title"], "summary": meta["summary"], "description": meta.get("description", meta["summary"])}
    permissions = {"camera": False, "microphone": False, "imu": False, "network": False,
                   "additional_hardware": False, "background_service": False, "external_display": False}
    permissions.update(meta.get("permissions", {}))
    entry = {
        "uuid": str(uuid.uuid5(NAMESPACE, meta["share_code"])),
        "share_code": meta["share_code"],
        **text,
        "locales": {"en": text},
        "i18n": {"en": text},
        "categories": meta.get("categories", ["Tools"]),
        "author": meta.get("author", {"github": owner, "display_name": owner}),
        "version": meta["version"],
        "published_at": meta.get("published_at") or (previous or {}).get("published_at") or now,
        "updated_at": now,
        "review": {"status": "approved"},      # the Store only installs approved entries; you review your own
        "license": meta.get("license", "MIT"),
        "source_repo": meta.get("source_repo", "https://github.com/%s/%s" % (owner, repo)),
        "download": {"type": "deb", "package": meta["package"], "url": "%s/packages/%s" % (base, package),
                     "md5": md5, "sha256": sha256, "size": size},
        "depends": meta.get("depends", ""),
        "permissions": permissions,
        "assets": {"icon": icon, "screenshots": shots},
        "icon": icon,
        "screenshots": shots,
        **screen_info.screen_fields(os.path.join(apps_dir, name), meta, arch),
    }
    if previous and previous.get("version") == entry["version"] \
            and (previous.get("download") or {}).get("sha256") == sha256:
        # the package did not change: keep its dates, a plain run must not rewrite them
        for key in ("published_at", "updated_at"):
            if previous.get(key):
                entry[key] = previous[key]
    return entry


def load_registry(path):
    """The registry file as a dict, or {} when missing or unreadable."""
    try:
        with open(path, encoding="utf-8") as handle:
            data = json.load(handle)
        return data if isinstance(data, dict) else {}
    except (OSError, ValueError):
        return {}


def build_registry(old, apps, registry_id, now):
    """The registry document. The old file's device_targets are kept; generated_at stays the old one unless the
    content (everything but generated_at) changed."""
    registry = {
        "schema_version": 2,
        "generated_at": now,
        "registry_id": registry_id,
        # informative only: read by neither the Store nor the launcher
        "device_targets": old.get("device_targets") or ["CyberdeckZero"],
        "i18n": {"default_locale": "en", "fallback_locale": "en", "supported_locales": ["en"]},
        "apps": apps,
    }
    if old.get("generated_at") and {k: v for k, v in old.items() if k != "generated_at"} \
            == {k: v for k, v in registry.items() if k != "generated_at"}:
        registry["generated_at"] = old["generated_at"]
    return registry


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--owner")
    parser.add_argument("--repo")
    parser.add_argument("--branch", default="main")
    parser.add_argument("--no-build", action="store_true", help="reuse the .deb files already in packages/")
    parser.add_argument("--registry-name", help="registry_id / display name (default: the repository name)")
    parser.add_argument("--arch", choices=ARCHES, help="only this architecture's packages and registry (default: "
                        "every architecture); with --only: the package's architecture (default arm64)")
    parser.add_argument("--only", help="build only the app <id> (needs --out); a draft app is allowed")
    parser.add_argument("--out", help="with --only: folder for the .deb; no registry is written")
    args = parser.parse_args()
    if bool(args.only) != bool(args.out):
        sys.exit("--only and --out go together")

    owner, repo = detect_remote()
    owner, repo = args.owner or owner, args.repo or repo
    if not owner or not repo:
        sys.exit("cannot tell the GitHub owner/repo: pass --owner and --repo (or set the git remote 'origin')")
    base = "https://raw.githubusercontent.com/%s/%s/%s" % (owner, repo, args.branch)

    manifests = []
    apps_dir = os.path.join(REPO, "apps")
    for name in sorted(os.listdir(apps_dir)):
        manifest_path = os.path.join(apps_dir, name, "app.json")
        if not os.path.isfile(manifest_path):
            continue
        with open(manifest_path, encoding="utf-8") as handle:
            meta = json.load(handle)
        if args.only:
            if name != args.only:
                continue
        elif meta.get("draft"):
            print("skipping draft app: " + name)
            continue
        for key in ("share_code", "package", "version", "title", "summary"):
            if not meta.get(key):
                sys.exit("%s: missing '%s'" % (manifest_path, key))
        errors = check_descriptors.manifest_errors(meta)
        if errors:
            sys.exit("%s: %s (summary <= %d characters, summary + description <= %d; the long text goes in the "
                     "README)" % (manifest_path, "; ".join(errors), check_descriptors.SUMMARY_MAX,
                                  check_descriptors.DESCRIPTOR_MAX))
        manifests.append((name, meta, app_arches(meta, manifest_path)))

    if args.only:
        if not manifests:
            sys.exit("no app %s (apps/%s/app.json)" % (args.only, args.only))
        name, meta, arches = manifests[0]
        arch = args.arch or "arm64"
        if arch not in arches:
            sys.exit("apps/%s/app.json: 'arch' does not list %s" % (name, arch))
        package_path = os.path.join(args.out, "%s_%s_%s.deb" % (meta["package"], meta["version"], arch))
        os.makedirs(args.out, exist_ok=True)
        if not args.no_build:
            build_package(name, meta, arch, args.out, owner)
        if not os.path.isfile(package_path):
            sys.exit("package not found: " + package_path)
        md5, _, size = digest(package_path)
        print("%s  md5 %s  size %d" % (package_path, md5, size))
        return

    for arch in [args.arch] if args.arch else ARCHES:
        registry_file = REGISTRY_FILES[arch]
        old = load_registry(os.path.join(REPO, registry_file))
        old_apps = {a.get("share_code"): a for a in old.get("apps", []) if isinstance(a, dict)}
        apps = []
        for name, meta, arches in manifests:
            if arch not in arches:
                continue
            package = "%s_%s_%s.deb" % (meta["package"], meta["version"], arch)
            package_path = os.path.join(REPO, "packages", package)
            if not args.no_build:
                build_package(name, meta, arch, os.path.join(REPO, "packages"), owner)
            if not os.path.isfile(package_path):
                sys.exit("package not found: " + package_path)
            apps.append(registry_entry(name, meta, base, package, package_path, owner, repo, arch,
                                     old_apps.get(meta["share_code"])))

        now = datetime.datetime.now(datetime.timezone.utc).replace(microsecond=0).isoformat()
        registry = build_registry(old, apps, args.registry_name or repo, now)
        with open(os.path.join(REPO, registry_file), "w", encoding="utf-8", newline="\n") as handle:
            json.dump(registry, handle, indent=2, ensure_ascii=False)
            handle.write("\n")
        print("%s: %d app(s) (%s). Registry URL to add on the device:\n  %s/%s"
              % (registry_file, len(apps), arch, base, registry_file))


if __name__ == "__main__":
    main()
