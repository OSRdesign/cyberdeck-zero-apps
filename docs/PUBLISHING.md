# Release a version

How a new app, or a new version of an app, gets from the source tree to the decks. The deck reads two catalogues from the
`main` branch of this repository, `registry.json` (arm64, the decks) and `registry-amd64.json` (x86-64 PCs); a release
is a commit that puts the package and its registry entry on `main`. Writing the app: the
[app guidelines](APP-GUIDELINES.md); the fields of `app.json`: [APP-JSON.md](APP-JSON.md); the scripts used below:
[`tools/README.md`](../tools/README.md).

## 1. Before you start

* `version` raised in `apps/<id>/app.json` (the deck compares versions; the same number is not an update) and `draft`
  removed.
* The descriptor fits (summary at most 80 characters, summary + newline + description at most 500):
  `python3 tools/check_descriptors.py`.
* The app's `README.md` is current: version line, Changes, keys, requirements, and the **credits** (every library it
  links or opens at run time).
* The `usr/share/doc/<id>/copyright` file inside the package names every library the app opens at run time, as well as
  what it ships ([third-party libraries](../README.md#third-party-and-run-time-libraries)). It is inside the `.deb`, so
  a fix needs a new version.
* `depends` lists what the app links and opens at run time ([APP-JSON.md](APP-JSON.md#depends-and-recommends)).
* The row for the app in the table of the root [README](../README.md#apps) (name, version, packages, one paragraph).
* The [checklist](APP-GUIDELINES.md#13-final-checklist) is ticked, the board tests have been run on the package you are
  about to publish.

## 2. Build the packages

Build with the app's own script on the pinned toolchain, one binary per architecture:

```
sh apps/<id>/build.sh cross                  # arm64: apps/<id>/root/.../bin/<id>
sh apps/<id>/build.sh cross --arch amd64     # amd64: apps/<id>/root.amd64/.../bin/<id> (only for "arch": [.., "amd64"])
```

(The exact command and the toolchain are in the app's README, "Build and test"; the native apps share
[`apps/_common/build_app.sh`](../apps/_common/README.md).) Then build the `.deb` files from the checkout with
**Windows Python** (WSL on `/mnt/c` gives mode 755 to every file; Windows Python gives 644 and 755 for the binaries):

```
python tools/make_registry.py --only <id> --out <folder outside packages/>               # arm64
python tools/make_registry.py --only <id> --arch amd64 --out <folder outside packages/>  # amd64
```

`--only` writes just that package into the folder and touches neither `packages/` nor the registries. Test **that**
`.deb` on the boards. Rebuilding gives a different file every time (a build is not byte-identical), so what you
publish is the file you tested: copy it to `packages/<package>_<version>_<arch>.deb`, one per architecture.

## 3. Write the registry entry

```
python3 tools/make_registry.py --no-build
```

`--no-build` reuses the `.deb` files already in `packages/` instead of rebuilding them. For every non-draft app and
every architecture the script rebuilds the entry from `app.json` and the package, but an entry whose version and sha256
are unchanged keeps its `published_at` and `updated_at`, and a registry file keeps its `device_targets`; `generated_at`
moves only when something else in the file changed. So the run changes only the entry of the app you released (its
version, `download` md5, sha256, size and url, `updated_at`, and the text, screenshots and screen sizes that come from
its files) and `generated_at`, in each of the two registries. **Read the diff**: `git diff --stat registry.json
registry-amd64.json` should show one entry per file. A plain run, without `--no-build`, builds every package again, and
each new file has a new sha256, so every entry changes: do not use it for a release.

Then run the checks:

```
python3 tools/check_registry.py            # screen sizes of the registries equal the .desktop files
python3 tools/check_descriptors.py         # descriptor rule, apps and registries
python3 tools/test_registry_tools.py       # self test of the tools
sh tools/branding/check-branding.sh        # no brand names outside the allowed places
```

## 4. The pull request

A release is one pull request that holds:

| What | Where |
|---|---|
| the sources of the version | `apps/<id>/...` |
| the packages, one per architecture | `packages/<package>_<version>_arm64.deb` and, for an `arch: [arm64, amd64]` app, `_amd64.deb` |
| both registries | `registry.json` and `registry-amd64.json` (an arm64-only app: `registry.json` alone) |
| the table row of the root README | `README.md` |
| the app's README and release notes | `apps/<id>/README.md`, `apps/<id>/docs/` |

Cassette 0.3.0 went out like that (#146). The sources can also come first in a pull request of their own and the
packages and registries in a second one, as for Cassette 0.1.0 (#140, then #141); the rule is that the registries on
`main` never name a package that is not on `main`.

What must **not** change: the entries of other apps and the `device_targets` of the registries (the diff in step 3).

**Old packages stay.** Never delete an older `.deb` from `packages/`: the registries of older commits, and decks that
installed it, refer to its `raw.githubusercontent.com` address, and the history of what was published is the folder.
Only the latest version of an app is in the registry. There is no pruning policy; the folder holds the packages of
unpublished apps as well (LAN Scan, Wi-Fi Survey, earlier Mesh Hop versions).

Merge when the checks above pass and the diff shows what you expect. Nothing else is needed: no Pages, no release.
Packages above 50 MB are the exception ([README](../README.md#6-publish)).

## What the registry entry holds

Written by `make_registry.py`, never by hand:

| Field | Meaning |
|---|---|
| `download.url` | `https://raw.githubusercontent.com/<owner>/<repo>/main/packages/<file>`: the repository must be public and the branch `main` (owner and repo come from the git remote `origin`) |
| `download.md5` | what the deck checks after the download (protects against a corrupted file, not a malicious one) |
| `download.sha256`, `download.size` | the same package, for the Store and for the checks |
| `published_at`, `updated_at` | first publish, and the last time the package changed |
| `review.status` | always `approved`: the Store installs only approved entries, and you are the reviewer of your own repository |
| `min_size`, `portrait_min_size` | the app's own screen requirements, from `X-MinSize` and `X-Portrait` of the packaged `.desktop`, only when declared (the launcher hides an app that does not fit; an app without them is listed everywhere) |
| `icon`, `screenshots` | the raw URLs of `apps/<id>/icon.png` and of every `apps/<id>/screenshots/*.png`, in sorted file-name order |
| `depends`, `permissions`, `categories`, `license` ... | copied from `app.json` ([APP-JSON.md](APP-JSON.md)) |

## Draft, published, unpublished

* **Draft**: `"draft": true`. No package, no registry entry. `--only <id> --out ...` still builds a local `.deb`.
* **Published**: not a draft, the package is in `packages/`, the entry is in the registry (both, for amd64 apps).
* **Unpublish**: set `"draft": true` and run `make_registry.py --no-build` again: the entry disappears from the
  registries and the `.deb` files stay in `packages/`. Net Toolbox replaced LAN Scan and Wi-Fi Survey that way
  (2026-11-02); the deck keeps the installed app, it is just no longer offered.

## After the merge

* On a deck: **Settings > Apps > Sources > Sync my sources** (GitHub's cache can delay the new registry by a few
  minutes), the app shows the update, and installing it keeps the tile where it was on the home grid as long as the
  `.desktop` file name did not change.
* Check the version on the app's page and start the app once.
