# stb_image (vendored)

`stb_image.h`: the single-header image loader of Sean Barrett and the stb contributors, **v2.30**, unmodified.

| | |
|---|---|
| Upstream | https://github.com/nothings/stb |
| Commit | `013ac3beddff3dbffafd5177e7972067cd2b5083` (2024-05-31, "stb_image: fix gcc bounds-check warning (believed erroneous)", the last change to `stb_image.h` at the time of vendoring; the header says v2.30) |
| Fetched from | `https://raw.githubusercontent.com/nothings/stb/013ac3beddff3dbffafd5177e7972067cd2b5083/stb_image.h` |
| SHA-256 | `594c2fe35d49488b4382dbfaec8f98366defca819d916ac95becf3e75f4200b3` (283,010 bytes) |
| Licence | public domain (Unlicense) **or** MIT, at your choice: the text is at the end of the header |

How RSS Reader uses it: `src/ui/image_codecs.c` is the only file that includes it (`STB_IMAGE_IMPLEMENTATION`), with
`STBI_ONLY_JPEG | PNG | GIF | BMP`, `STBI_NO_STDIO`, `STBI_NO_LINEAR`, `STBI_NO_HDR`, `STBI_NO_SIMD`,
`STBI_MAX_DIMENSIONS 16384` and a counting allocator (`STBI_MALLOC`, `STBI_REALLOC_SIZED`, `STBI_FREE`) with a hard memory
ceiling. See `docs/IMAGES.md`. `build_app.sh` copies this folder next to `src/` for the cross build (the header is included
as `../../third_party/stb/stb_image.h`).

To update: fetch the new commit's `stb_image.h`, replace the file, update this table and `docs/IMAGES.md`, run
`tests/unit/run_tests.sh` and the sanitizer run (`SANITIZE=1`, `RSS_FUZZ_N=6000`). Do not edit the header: fixes go around it
in `image_codecs.c` (as the `wrapv` pragma does).
