# Cassette 0.3.1: release notes

Cassette 0.3.1 changes **one file in the package**: `/usr/share/doc/cassette/copyright`. There is no functional change;
the program, its settings and its behaviour are those of [0.3.0](RELEASE-0.3.0.md).

## What is new

The `copyright` file now names every third-party library Cassette uses:

- the system libraries the binaries are linked against (FreeType, libinput, libxkbcommon, libstdc++, libgcc), with their
  licences;
- the libraries it opens at run time and does not ship: libasound (alsa-lib, LGPL-2.1-or-later), libFLAC, libopus and
  libopusenc (BSD-3-Clause) and libpulse (LGPL-2.1-or-later), with their copyright holders and the package that provides
  each.

It says that none of them is part of the package, that they are loaded from the system when present, and that the
package's `Depends` makes apt install them. The font notices (Caveat SemiBold compiled in; Montserrat and JetBrains Mono
read from the launcher, SIL OFL 1.1) are unchanged. `depends` is unchanged.

0.3.0 and older packages named only libasound in that file.
