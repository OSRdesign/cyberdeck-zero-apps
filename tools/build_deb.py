#!/usr/bin/env python3
"""Build a Debian package (.deb) from a directory tree, in pure Python.

No dpkg, ar or tar needed, so it runs on Windows, macOS and Linux.

    python3 tools/build_deb.py --root apps/myapp/root --package myapp --version 0.1.0 \
        --description "What my app does" --maintainer "Me <me@example.com>" --out packages

The tree under --root is installed as is: put files where they must end up on the device, e.g.
    root/usr/share/APPLaunch/bin/M5CardputerZero-myapp           (the executable, aarch64)
    root/usr/share/APPLaunch/applications/myapp.desktop          (the launcher entry)
    root/usr/share/APPLaunch/share/images/myapp.png              (the icon)
"""
import argparse
import gzip
import hashlib
import io
import os
import stat
import sys
import tarfile
import time


def collect(root):
    """Yield (relative path with forward slashes, absolute path, is_dir)."""
    entries = []
    for base, dirs, files in os.walk(root):
        dirs.sort()
        rel_base = os.path.relpath(base, root).replace(os.sep, "/")
        if rel_base != ".":
            entries.append((rel_base, base, True))
        for name in sorted(files):
            path = os.path.join(base, name)
            rel = (name if rel_base == "." else rel_base + "/" + name)
            entries.append((rel, path, False))
    return entries


def file_mode(rel, path):
    """Executables: anything under a bin/ directory, *.sh, or with the exec bit set on POSIX."""
    if "/bin/" in "/" + rel or rel.endswith(".sh"):
        return 0o755
    try:
        if os.stat(path).st_mode & stat.S_IXUSR:
            return 0o755
    except OSError:
        pass
    return 0o644


def tar_gz(members, mtime):
    """members: list of (name, data or None for a directory, mode). Returns gzip bytes."""
    raw = io.BytesIO()
    with tarfile.open(fileobj=raw, mode="w", format=tarfile.GNU_FORMAT) as tar:
        for name, data, mode in members:
            info = tarfile.TarInfo(name)
            info.uid = info.gid = 0
            info.uname = info.gname = "root"
            info.mtime = mtime
            info.mode = mode
            if data is None:
                info.type = tarfile.DIRTYPE
                tar.addfile(info)
            else:
                info.size = len(data)
                tar.addfile(info, io.BytesIO(data))
    out = io.BytesIO()
    with gzip.GzipFile(fileobj=out, mode="wb", mtime=0, compresslevel=9) as gz:
        gz.write(raw.getvalue())
    return out.getvalue()


def ar_member(name, data, mtime):
    header = "%-16s%-12d%-6d%-6d%-8s%-10d`\n" % (name + "/" if name != "debian-binary" else name, mtime, 0, 0, "100644", len(data))
    blob = header.encode("ascii") + data
    if len(data) % 2:
        blob += b"\n"
    return blob


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--root", required=True, help="directory tree to install")
    parser.add_argument("--package", required=True, help="package name (lowercase letters, digits, + - .)")
    parser.add_argument("--version", required=True)
    parser.add_argument("--arch", default="arm64")
    parser.add_argument("--description", required=True, help="one-line description")
    parser.add_argument("--maintainer", default="Unknown <unknown@example.com>")
    parser.add_argument("--homepage", default="")
    parser.add_argument("--depends", default="", help="Debian Depends: field, e.g. 'curl, libfreetype6'")
    parser.add_argument("--section", default="APPLaunch")
    parser.add_argument("--out", default=".", help="output directory")
    args = parser.parse_args()

    if not os.path.isdir(args.root):
        sys.exit("root directory not found: " + args.root)
    entries = collect(args.root)
    if not any(not is_dir for _, _, is_dir in entries):
        sys.exit("the root directory is empty")
    mtime = int(os.environ.get("SOURCE_DATE_EPOCH", time.time()))

    data_members = [("./", None, 0o755)]
    md5_lines = []
    installed_size = 0
    for rel, path, is_dir in entries:
        if is_dir:
            data_members.append(("./" + rel + "/", None, 0o755))
            continue
        with open(path, "rb") as handle:
            data = handle.read()
        data_members.append(("./" + rel, data, file_mode(rel, path)))
        md5_lines.append("%s  %s\n" % (hashlib.md5(data).hexdigest(), rel))
        installed_size += len(data)

    control = [
        "Package: " + args.package,
        "Version: " + args.version,
        "Architecture: " + args.arch,
        "Maintainer: " + args.maintainer,
        "Installed-Size: %d" % ((installed_size + 1023) // 1024),
        "Section: " + args.section,
        "Priority: optional",
    ]
    if args.depends:
        control.append("Depends: " + args.depends)
    if args.homepage:
        control.append("Homepage: " + args.homepage)
    control.append("Description: " + args.description)
    control_members = [
        ("./", None, 0o755),
        ("./control", ("\n".join(control) + "\n").encode("utf-8"), 0o644),
        ("./md5sums", "".join(md5_lines).encode("utf-8"), 0o644),
    ]

    deb = b"!<arch>\n"
    deb += ar_member("debian-binary", b"2.0\n", mtime)
    deb += ar_member("control.tar.gz", tar_gz(control_members, mtime), mtime)
    deb += ar_member("data.tar.gz", tar_gz(data_members, mtime), mtime)

    os.makedirs(args.out, exist_ok=True)
    target = os.path.join(args.out, "%s_%s_%s.deb" % (args.package, args.version, args.arch))
    with open(target, "wb") as handle:
        handle.write(deb)
    print("%s (%d bytes, %d files)" % (target, len(deb), len(md5_lines)))


if __name__ == "__main__":
    main()
