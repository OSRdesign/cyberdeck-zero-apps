# md4c (vendored)

MD4C, a Markdown parser in C, by Martin Mitáš: https://github.com/mity/md4c

- Version: **v0.6.0**, tag `v0.6.0`, commit `7fc1815a5eeba2af7d6120a76202bf59f3b6e6e4` (2026-09-17).
- Files: `src/md4c.c` and `src/md4c.h` of that tag, unchanged, and its `LICENSE.md`. Fetched with
  `gh api repos/mity/md4c/contents/<file>?ref=v0.6.0` on 2026-10-24.
- Licence: MIT (`LICENSE.md`; the same text in each source file's header). Shipped in the package as
  `/usr/share/doc/markdown-notes/LICENSE.md4c` and credited in its `copyright` file and the app's README.
- Not vendored: `entity.c` (the HTML entity table, 74 KB): `src/core/markdown.cpp` decodes numeric references and
  about forty common names itself (report 037 section 2.3).

How it is built: `build_app.sh` copies `third_party/` next to the app's sources without compiling it, so
`src/core/md4c_impl.c` includes `md4c.c` (one translation unit, md4c's own warnings silenced there). The unit tests
compile the same wrapper. To update: replace the three files with those of a newer tag, change the version above and in
the copyright file and the README, run `build.sh test`.
