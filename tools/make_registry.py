#!/usr/bin/env python3
"""Build the packages and registry.json of this repository.

    python3 tools/make_registry.py                       # build every app, write registry.json
    python3 tools/make_registry.py --owner me --repo my-apps

For every apps/<id>/app.json it
  1. builds packages/<package>_<version>_arm64.deb from apps/<id>/root/ (tools/build_deb.py),
  2. adds an entry to registry.json with the download URL, md5, sha256 and size of that package.

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

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
NAMESPACE = uuid.UUID("6f1c2f0e-6a52-4a54-9d3b-2a3e5c7f9b10")   # fixed: the same share_code always gives the same uuid


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


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--owner")
    parser.add_argument("--repo")
    parser.add_argument("--branch", default="main")
    parser.add_argument("--no-build", action="store_true", help="reuse the .deb files already in packages/")
    parser.add_argument("--registry-name", help="registry_id / display name (default: the repository name)")
    args = parser.parse_args()

    owner, repo = detect_remote()
    owner, repo = args.owner or owner, args.repo or repo
    if not owner or not repo:
        sys.exit("cannot tell the GitHub owner/repo: pass --owner and --repo (or set the git remote 'origin')")
    base = "https://raw.githubusercontent.com/%s/%s/%s" % (owner, repo, args.branch)

    apps = []
    apps_dir = os.path.join(REPO, "apps")
    for name in sorted(os.listdir(apps_dir)):
        manifest_path = os.path.join(apps_dir, name, "app.json")
        if not os.path.isfile(manifest_path):
            continue
        with open(manifest_path, encoding="utf-8") as handle:
            meta = json.load(handle)
        for key in ("share_code", "package", "version", "title", "summary"):
            if not meta.get(key):
                sys.exit("%s: missing '%s'" % (manifest_path, key))
        package = "%s_%s_arm64.deb" % (meta["package"], meta["version"])
        package_path = os.path.join(REPO, "packages", package)
        scripts = os.path.join(apps_dir, name, "DEBIAN")
        if not args.no_build:
            subprocess.check_call([
                sys.executable, os.path.join(HERE, "build_deb.py"),
                "--root", os.path.join(apps_dir, name, "root"),
                "--package", meta["package"], "--version", meta["version"],
                "--description", meta["summary"],
                "--maintainer", meta.get("maintainer", "%s <noreply@users.noreply.github.com>" % owner),
                "--homepage", meta.get("source_repo", ""),
                "--depends", meta.get("depends", ""),
                "--out", os.path.join(REPO, "packages")] + (["--scripts", scripts] if os.path.isdir(scripts) else []))
        if not os.path.isfile(package_path):
            sys.exit("package not found: " + package_path)
        md5, sha256, size = digest(package_path)

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
        apps.append({
            "uuid": str(uuid.uuid5(NAMESPACE, meta["share_code"])),
            "share_code": meta["share_code"],
            **text,
            "locales": {"en": text},
            "i18n": {"en": text},
            "categories": meta.get("categories", ["Tools"]),
            "author": meta.get("author", {"github": owner, "display_name": owner}),
            "version": meta["version"],
            "published_at": meta.get("published_at", now),
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
        })

    registry = {
        "schema_version": 2,
        "generated_at": datetime.datetime.now(datetime.timezone.utc).replace(microsecond=0).isoformat(),
        "registry_id": args.registry_name or repo,
        "device_targets": ["CardputerZero"],
        "i18n": {"default_locale": "en", "fallback_locale": "en", "supported_locales": ["en"]},
        "apps": apps,
    }
    with open(os.path.join(REPO, "registry.json"), "w", encoding="utf-8", newline="\n") as handle:
        json.dump(registry, handle, indent=2, ensure_ascii=False)
        handle.write("\n")
    print("registry.json: %d app(s). Registry URL to add on the device:\n  %s/registry.json" % (len(apps), base))


if __name__ == "__main__":
    main()
