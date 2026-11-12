# `app.json`: every field

`apps/<id>/app.json` is the one file you write by hand to describe an app. `tools/make_registry.py` reads it, builds the
package from it and copies most of it into the registries (`registry.json`, `registry-amd64.json`), which Settings > Apps
and the Store read. Never edit a registry by hand: change `app.json` and regenerate
([publishing](PUBLISHING.md)). Overview of the files of an app: [README](../README.md#host-your-own-apps).

```json
{
  "share_code": "myapp",
  "package": "myapp",
  "version": "0.1.0",
  "title": "My App",
  "summary": "One line shown in the list (at most 80 characters).",
  "description": "What the app does, its main features and what it needs.",
  "categories": ["Tools"],
  "author": {"github": "your-github-name", "display_name": "Your Name"},
  "license": "MIT",
  "source_repo": "https://github.com/your-github-name/myapp",
  "depends": "",
  "permissions": {"network": true},
  "arch": ["arm64", "amd64"]
}
```

`share_code`, `package`, `version`, `title` and `summary` are required: `make_registry.py` stops without them.

## Fields

| Field | Req. | Rule |
|---|---|---|
| `share_code` | yes | short unique id, lowercase letters and digits (and `-` as in `mesh-hop`); the app's identity: **keep it the same across versions**. The registry `uuid` is derived from it. By convention the same as `package` and the app folder name; the `.desktop` file is `<share_code>.desktop` |
| `package` | yes | Debian package name (lowercase letters, digits, `+ - .`) |
| `version` | yes | **raise it for every release**: the deck compares versions Debian-style (`dpkg --compare-versions`), and re-uploading a package with the same version is not seen as an update |
| `title` | yes | the name shown in the list |
| `summary`, `description` | summary yes | the **descriptor**, the only text the launcher shows (Settings > Apps): a summary of one line, at most **80 characters**, then a description of what the app is, its main features and what it needs (keyboard, USB dongle, network, a minimum screen when the app declares one), nothing more. Summary + newline + description: at most **500 characters** (characters, not bytes); the launcher cuts a longer text at 500 with dots. A missing `description` counts as the summary. The long documentation goes in the app's `README.md` in this repository. `python3 tools/check_descriptors.py` checks every app and both registries, and `make_registry.py` refuses an app that breaks the rule |
| `categories` | no | list of names for the catalogue; default `["Tools"]`. `make_registry.py` validates nothing: the Store's own list is the authority. In use here: Tools, Games, Network, Radio, Home, Productivity, Internet |
| `author` | no | `{"github": ..., "display_name": ...}`; default: the repository owner |
| `license` | no | SPDX expression of **everything the package ships** (`MIT`, `MIT AND Apache-2.0`, `MIT AND BSD-3-Clause AND GPL-3.0-or-later`); default `MIT`. System libraries the app only uses are not part of it: they are credited in the app's README and copyright file ([third-party libraries](../README.md#third-party-and-run-time-libraries)) |
| `source_repo` | no | URL shown as the source and used as the package's homepage; default: this repository |
| `depends` | no | the Debian `Depends:` line, see [below](#depends-and-recommends) |
| `recommends` | no | the Debian `Recommends:` line: packages a feature uses when they are there |
| `permissions` | no | what the app uses, shown to the user, so be honest. Keys (all default `false`): `network`, `additional_hardware`, `camera`, `microphone`, `imu`, `background_service`, `external_display`. Set the ones you need to `true`: `{"network": true}` |
| `arch` | no | the architectures the app is packaged for, default `["arm64"]` (the decks); `["arm64", "amd64"]` also builds an amd64 package from `root.amd64/` and lists the app in `registry-amd64.json` ([amd64](../README.md#amd64-x86-64-pcs)) |
| `draft` | no | `true` keeps an unfinished app out of the packages and the registries (`make_registry.py` skips it). To build a local `.deb` of a draft without publishing, run `python3 tools/make_registry.py --only <id> --out <folder outside packages/>` |
| `min_size` | no | `"WxH"`, the app's landscape minimum screen, only for an app whose UI cannot fit a smaller one (there is no global minimum; without it the app is listed everywhere), used **only when the `.desktop` has no `X-MinSize`** (the `.desktop` wins). Normally leave it out and declare `X-MinSize` (and `X-Portrait`) in the `.desktop`; an old package without it needs a rebuild to carry it, this field does not |
| `published_at`, `maintainer` | no | rarely used: `published_at` overrides the date the registry shows (otherwise the date of the first publish is kept); `maintainer` is the package's `Maintainer:` (default `<owner> <noreply@users.noreply.github.com>`) |

## `depends` and `recommends`

Both are Debian relationship fields, written as one string:

* comma separated: `"libfreetype6, libinput10, libxkbcommon0"`;
* a version bound in parentheses: `python3 (>= 3.10)`;
* alternatives with `|`, for a library whose package name differs between Debian releases:
  `libasound2t64 | libasound2`, `libflac14 | libflac12t64 | libflac12` (Cassette);
* check that every name exists on Debian 13 (arm64, and amd64 for a PC app).

What belongs in `depends`: what the binary **links**, what it **opens at run time with `dlopen`** (the Store must install
it, or the feature fails at run time: Cassette lists `libopus0, libopusenc0, libflac...` and `libpulse0` for that
reason, Deck Atlas `libsqlite3-0` and `libcurl4t64`), and the **tools it runs** (`curl`, `picocom`, `network-manager`).
A native app's usual start is `libfreetype6, libinput10, libxkbcommon0`.

`recommends` is for an **optional** feature: apt installs the package by default but the app must work without it
(Mesh Hop and mmWave Radar recommend `bluez`: the Bluetooth page needs it, the rest of the app does not). Rule of
thumb: if the app is useless or crashes without the library, `depends`; if only one optional feature is missing,
`recommends`. Every library opened at run time must also be named in the app's `copyright` file and README credits
(see the [third-party table](../README.md#third-party-and-run-time-libraries)).

`tools/build_deb.py` adds `libc6 (>= 2.38)` to every package (the binaries are built for Debian 13): do not write it,
and never lower it. A clause with `|` is passed through untouched.

## Files next to `app.json`

| Path | Rule |
|---|---|
| `apps/<id>/root/...` | the files of the package, laid out as on the device ([README](../README.md#4-lay-out-the-files)) |
| `apps/<id>/root.amd64/...` | the amd64 files that replace those of `root/` (the binary, `build-info`) |
| `apps/<id>/DEBIAN/` | optional maintainer scripts (`preinst`, `postinst`, `prerm`, `postrm`) |
| `apps/<id>/icon.png` | optional, a 100x100 square transparent PNG ([ICON-PALETTE.md](ICON-PALETTE.md)); listed in the registry as `icon` and `assets.icon` only when the file exists |
| `apps/<id>/screenshots/*.png` | optional, PNG files of any real size (480x320, 320x480, 640x480 and 800x480 are all in use; no count is enforced); the registry lists **every** PNG in **sorted file-name order**, and the Store shows them in that order, so prefix the names `01-`, `02-`... |

The registry entry also gets `min_size` and `portrait_min_size` from the `.desktop`, `download.url`, `md5`, `sha256`,
`size` of the package and `review.status: approved`: what they mean is in [PUBLISHING.md](PUBLISHING.md#what-the-registry-entry-holds).
