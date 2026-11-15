/*
 * SPDX-License-Identifier: MIT
 *
 * VHS: photos on disk (brief 7 step 4, 8, 9.5). save_photo() makes the photo file (jpeg/exif.hpp: the camera's JPEG,
 * an EXIF APP1, the Annex K tables when missing), takes the next number of the shared counter and writes
 * photo_<NNN>_<when>.jpg atomically (temp, fsync, rename, fsync of the folder: Cassette's atomic_file). The folder is
 * made with mode 0700 when missing. list_photos() is the photo review's list (newest first); the full scene index is
 * M4. Runs on the scenes worker, never on the LVGL thread. LVGL-free.
 */
#pragma once

#include "jpeg/exif.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace vhs {

class FileOps;

/* "~/VHS" -> "$HOME/VHS"; other paths unchanged. */
std::string expand_home(const std::string &folder);
/* mkdir -p, then mode 0700 for a folder this made (brief 8). 0 or -errno. */
int ensure_folder(FileOps &ops, const std::string &folder);

struct PhotoSave {
    bool ok = false;
    std::string path, name, error;
    PhotoFacts facts;
    std::size_t bytes = 0;
};

/* f: Make, Model and Software (width / height come from the frame); datetime is set here when synchronized. */
PhotoSave save_photo(FileOps &ops, const std::string &folder, const std::uint8_t *frame, std::size_t n, ExifFields f,
                     std::int64_t wall_s, bool synchronized, bool utc = false);

struct PhotoEntry {
    std::string name, path;
    std::uint64_t size = 0;
    std::int64_t mtime = 0;
    int number = 0;
};

/* The .jpg / .jpeg files of the folder (foreign names too), newest first: modification time, then number, then name. */
std::vector<PhotoEntry> list_photos(FileOps &ops, const std::string &folder);

/* "1920x1080, 98 KB, Nov 14 2026 18:47" (canvas PhotoReview): the size from the JPEG, bytes in KB / MB, the local
 * modification time (utc: UTC). */
std::string photo_meta(int w, int h, std::uint64_t bytes, std::int64_t mtime, bool utc = false);

} // namespace vhs
