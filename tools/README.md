# tools

The scripts that build, describe and check the packages and registries of this repository. Everything here is pure
Python 3 with the standard library (nothing to install), except `icons/make_icons.py` (Pillow) and the shell script in
`branding/` (a POSIX `sh` and `git`). Run them from the repository root. None runs in CI: they are run by hand, before
a pull request ([PUBLISHING.md](../docs/PUBLISHING.md)).

| Script | Purpose | Writes files? |
|---|---|---|
| [`make_registry.py`](#make_registrypy) | builds the packages and writes `registry.json` and `registry-amd64.json` | yes |
| [`build_deb.py`](#build_debpy) | builds one `.deb` from a directory tree (used by `make_registry.py`) | yes |
| [`check_registry.py`](#check_registrypy) | the registries' screen sizes equal the packaged `.desktop` files | no |
| [`check_descriptors.py`](#check_descriptorspy) | the summary and description rule, for the apps and the registries | no |
| [`screen_info.py`](#screen_infopy) | module: how `X-MinSize` / `X-Portrait` become registry fields | no |
| [`test_registry_tools.py`](#test_registry_toolspy) | self test of the tools above | no (temporary folder) |
| [`branding/check-branding.sh`](#brandingcheck-brandingsh) | no brand names outside the allowed places | no |
| [`icons/make_icons.py`](#iconsmake_iconspy) | generates the tile icons and the review sheet | yes (`icons/out/`) |

## make_registry.py

Reads every `apps/<id>/app.json`, builds `packages/<package>_<version>_<arch>.deb` for each architecture the app lists
(`root/` with `root.<arch>/` laid over it), and writes the registries with the download URL, md5, sha256 and size.
Owner and repository come from the git remote `origin` (`--owner`, `--repo`, `--branch` override them).

```
python3 tools/make_registry.py                    # every app, every architecture: builds and writes both registries
python3 tools/make_registry.py --no-build         # reuse the .deb files already in packages/ (releases: use this)
python3 tools/make_registry.py --arch amd64       # only the amd64 packages and registry-amd64.json
python tools/make_registry.py --only cassette --out C:/tmp/pkg                 # one local .deb, no registry
python tools/make_registry.py --only cassette --arch amd64 --out C:/tmp/pkg    # the same for amd64
```

* A draft app (`"draft": true`) is skipped, unless named with `--only`.
* `--only <id>` needs `--out <folder>`; it writes only that package and touches neither `packages/` nor the registries.
* An entry whose version and sha256 did not change keeps its `published_at` and `updated_at`; each registry keeps its
  `device_targets`; `generated_at` moves only when the content changed. With `--no-build` a run therefore changes only
  the entry of the app that has a new package. Without it every package is rebuilt (not byte-identical) and every
  entry changes: use it only on a fork, or to build everything.
* It stops on an app that breaks the descriptor rule, misses a required field or a package file.
* Use Windows Python when you build packages on Windows: files get mode 644 and the binaries 755.

## build_deb.py

Packs a directory tree into a `.deb` (`ar` of `debian-binary`, `control.tar.gz`, `data.tar.gz`, written in Python, so no
`dpkg-deb` is needed). `make_registry.py` calls it; call it yourself only for a one-off package.

```
python3 tools/build_deb.py --root apps/myapp/root --package myapp --version 0.1.0 \
    --description "One line" --depends "curl" --out /tmp/pkg
```

Options: `--overlay <tree>` (repeatable, its files replace the root's at the same path), `--arch` (default `arm64`),
`--depends`, `--recommends`, `--maintainer`, `--homepage`, `--section`, `--scripts <dir>` (maintainer scripts).

* **libc6 floor.** It adds `libc6 (>= 2.38)` to `Depends` (a weaker bound is raised, a stronger one kept), so that `dpkg`
  refuses the package on a system too old for the binaries.
* **Wrong architecture.** It refuses a package that holds an ELF file for another architecture than `--arch` (an arm64
  binary left in `root/` of an amd64 package).
* **Modes.** Files under a `bin/` directory, `*.sh`, ELF files and `#!` scripts get 755, all others 644, whatever the
  file system says (so a checkout on Windows gives the same package as one on Linux).
* Alternatives in `--depends` (`a | b`) pass through unchanged.

## check_registry.py

```
python3 tools/check_registry.py          # exit 0 when everything matches, 1 with a list of mismatches
```

For each entry of both registries it compares `min_size` and `portrait_min_size` with the `X-MinSize` and `X-Portrait`
of the packaged `.desktop` (the `app.json` `min_size` when the `.desktop` has none) and checks that a value is a
well-formed `WxH`. After you change a `.desktop` or `min_size`, run `make_registry.py --no-build` and this check again.

## check_descriptors.py

```
python3 tools/check_descriptors.py       # exit 0 when everything passes, 1 otherwise
```

Checks the descriptor rule (summary one line and at most 80 characters, summary + newline + description at most 500
characters) of every `apps/<id>/app.json`, drafts and the template included, and of every registry entry, including its
`locales` and `i18n` copies.

## screen_info.py

A module, not a command. It holds the launcher's reading of `X-MinSize` and `X-Portrait` (what is accepted as a size, how
it is normalised to `WxH`) so that `make_registry.py` and `check_registry.py` compute the same registry fields. Read its
header when a size you wrote seems to be ignored (a malformed size counts as absent).

## test_registry_tools.py

```
python3 tools/test_registry_tools.py     # unittest; exit 0 when all pass
```

Tests `screen_info`, `make_registry` and `check_registry` on a throw-away repository in a temporary folder: no network,
no build. Run it after you change any script here.

## branding/check-branding.sh

```
sh tools/branding/check-branding.sh [--allow FILE] [REPO_ROOT]
```

<!-- branding-allow-begin: describes what the check looks for -->
Fails (exit 1, one `file:line: text` per hit) when a tracked or untracked text file names M5Stack or CardputerZero
outside the allowed places: the approved credit phrases, names inherited from upstream that stay (`APPLaunch`, `cp0_`,
`~/.config/cardputerzero`), blocks between `branding-allow-begin` and `branding-allow-end` markers in a Markdown comment,
and the paths of the allow-list [`branding-allow.txt`](branding/branding-allow.txt) (one git pathspec glob per line, a
`#` comment gives the reason).
<!-- branding-allow-end -->

`sh tools/branding/check-branding.sh -h` lists the allowed tokens. Add an allow-list
entry only for a real false positive, with its reason in the comment.

## icons/make_icons.py

```
python tools/icons/make_icons.py [--launcher DIR] [--out DIR]
```

Needs Pillow (`pip install Pillow`). Draws every app tile icon at 800x800 and reduces it to the canonical 100x100
transparent PNG in `icons/out/<id>.png`, plus a review sheet (`sheet.png`: 64 px, 32 px and greyscale, next to the
launcher's built-in tiles when `--launcher` points at the launcher checkout). The rules are in
[ICON-PALETTE.md](../docs/ICON-PALETTE.md).
