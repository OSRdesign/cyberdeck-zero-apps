/*
 * SPDX-License-Identifier: MIT
 *
 * Deck Atlas tests: the file browser model: the start places (home, sticks and cards), a folder's rows (folders, the
 * archives, hidden and other files left out, the cap), the tile-folder test (MapTileDl's layout, a TMS folder, a
 * folder of photos), paths, and the real POSIX lister on a temporary folder (folder bytes, free space).
 */
#include "check.hpp"
#include "browser.hpp"

#include <fcntl.h>
#include <map>
#include <sys/stat.h>
#include <unistd.h>

using namespace atlas;

namespace {

class FakeLister : public Lister {
public:
    std::map<std::string, std::vector<DirEntry>> dirs;
    int calls = 0;
    void dir(const std::string &path, std::vector<DirEntry> es) { dirs[path] = std::move(es); }
    bool list(const std::string &path, std::vector<DirEntry> &out, std::string *why) override
    {
        ++calls;
        auto it = dirs.find(path);
        if (it == dirs.end()) {
            if (why) *why = "No such file or directory";
            return false;
        }
        out = it->second;
        return true;
    }
};

DirEntry D(const char *n) { return {n, true, 0}; }
DirEntry F(const char *n, std::uint64_t size = 100) { return {n, false, size}; }

void touch(const std::string &path, std::size_t bytes)
{
    const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return;
    std::string data(bytes, 'x');
    if (::write(fd, data.data(), data.size()) < 0) {}
    ::close(fd);
}

} // namespace

void test_browser()
{
    /* names */
    CHECK(is_archive_name("paris.mbtiles"));
    CHECK(is_archive_name("Europe.PMTiles"));
    CHECK(!is_archive_name(".mbtiles"));
    CHECK(!is_archive_name("paris.mbtiles.part"));
    CHECK(!is_archive_name("notes.txt"));
    CHECK(is_number("0"));
    CHECK(is_number("16590"));
    CHECK(!is_number(""));
    CHECK(!is_number("1a"));
    CHECK(!is_number("1234567890"));
    CHECK(is_tile_file_name("11268.png"));
    CHECK(is_tile_file_name("11268.JPG"));
    CHECK(is_tile_file_name("11268"));
    CHECK(is_tile_file_name("11268.png.tile"));
    CHECK(is_tile_file_name("5.pbf"));
    CHECK(!is_tile_file_name("inventory.json"));
    CHECK(!is_tile_file_name("12.txt"));
    CHECK(!is_tile_file_name(".png"));

    /* paths */
    CHECK_EQ(path_join("/media/deck", "MAPS"), std::string("/media/deck/MAPS"));
    CHECK_EQ(path_join("/", "mnt"), std::string("/mnt"));
    CHECK_EQ(path_parent("/media/deck/MAPS"), std::string("/media/deck"));
    CHECK_EQ(path_parent("/media/"), std::string("/"));
    CHECK_EQ(path_parent("/"), std::string("/"));
    CHECK_EQ(path_name("/home/deck/Downloads/tiles/"), std::string("tiles"));
    CHECK_EQ(path_display("/home/deck/Maps", "/home/deck"), std::string("~/Maps"));
    CHECK_EQ(path_display("/home/deck", "/home/deck"), std::string("~"));
    CHECK_EQ(path_display("/home/deckard/x", "/home/deck"), std::string("/home/deckard/x"));

    /* a MapTileDl folder: zoom folders, column folders, tile files, and its inventory.json */
    FakeLister fs;
    fs.dir("/home/deck", {D("Downloads"), D(".cache"), F("notes.txt"), F("paris.mbtiles", 12u << 20)});
    fs.dir("/home/deck/Downloads", {D("tiles"), D("photos"), F("europe.pmtiles", 300u << 20), F(".hidden.mbtiles")});
    fs.dir("/home/deck/Downloads/tiles", {F("inventory.json"), D("1"), D("14"), D("15"), D("17")});
    fs.dir("/home/deck/Downloads/tiles/1", {D("0"), D("1")});
    fs.dir("/home/deck/Downloads/tiles/1/0", {F("0.png"), F("1.png")});
    fs.dir("/home/deck/Downloads/photos", {D("2024"), F("a.jpg")});
    fs.dir("/home/deck/Downloads/photos/2024", {D("05")});
    fs.dir("/home/deck/Downloads/photos/2024/05", {F("IMG_0001.jpg")});
    CHECK(looks_like_tile_folder(fs, "/home/deck/Downloads/tiles"));
    CHECK(!looks_like_tile_folder(fs, "/home/deck/Downloads/photos"));
    CHECK(!looks_like_tile_folder(fs, "/home/deck/Downloads"));
    CHECK(!looks_like_tile_folder(fs, "/nowhere"));

    std::string err;
    std::vector<BrowserRow> rows = browser_rows(fs, "/home/deck/Downloads/tiles", false, &err);
    CHECK(err.empty());
    CHECK_EQ(rows.size(), static_cast<std::size_t>(2));
    CHECK(rows[0].kind == RowKind::ImportFolder);
    CHECK_EQ(rows[0].value, std::string("z1-17"));
    CHECK_EQ(rows[0].path, std::string("/home/deck/Downloads/tiles"));
    CHECK(rows[1].kind == RowKind::Up);
    CHECK_EQ(rows[1].path, std::string("/home/deck/Downloads"));

    rows = browser_rows(fs, "/home/deck/Downloads", false, &err);
    CHECK_EQ(rows.size(), static_cast<std::size_t>(4)); /* up, photos, tiles, europe.pmtiles; no hidden file */
    CHECK(rows[0].kind == RowKind::Up);
    CHECK(rows[1].kind == RowKind::Folder);
    CHECK_EQ(rows[1].label, std::string("photos"));
    CHECK_EQ(rows[2].label, std::string("tiles"));
    CHECK(rows[3].kind == RowKind::Archive);
    CHECK_EQ(rows[3].label, std::string("europe.pmtiles"));
    CHECK_EQ(rows[3].value, std::string("300 MB"));

    rows = browser_rows(fs, "/home/deck", true, &err);
    CHECK(rows[0].kind == RowKind::Up);
    CHECK_EQ(rows[0].label, std::string("Back to the start"));
    CHECK_EQ(rows[0].path, std::string(""));
    CHECK_EQ(rows.size(), static_cast<std::size_t>(3)); /* back, Downloads, paris.mbtiles; no .cache, no notes.txt */
    CHECK_EQ(rows[2].value, std::string("12 MB"));

    rows = browser_rows(fs, "/home/deck/Downloads/photos/2024/05", false, &err);
    CHECK_EQ(rows.size(), static_cast<std::size_t>(2));
    CHECK(rows[1].kind == RowKind::Note);
    CHECK_EQ(rows[1].label, std::string("No folders or map files here"));

    rows = browser_rows(fs, "/gone", false, &err);
    CHECK_EQ(err, std::string("Cannot open this folder: No such file or directory"));
    CHECK_EQ(rows.size(), static_cast<std::size_t>(1));
    CHECK(rows[0].kind == RowKind::Up);

    /* a big folder: the first kMaxEntries rows and a note */
    std::vector<DirEntry> many;
    for (int i = 0; i < kMaxEntries + 5; ++i) many.push_back(D(("d" + std::to_string(10000 + i)).c_str()));
    fs.dir("/big", many);
    rows = browser_rows(fs, "/big", false, &err);
    CHECK_EQ(rows.size(), static_cast<std::size_t>(kMaxEntries + 2));
    CHECK(rows.back().kind == RowKind::Note);
    CHECK_EQ(rows.back().label, std::string("5 more not shown"));

    /* the tile test lists only a handful of folders, even for a big folder */
    FakeLister big;
    std::vector<DirEntry> zooms, cols;
    for (int z = 0; z < 18; ++z) zooms.push_back(D(std::to_string(z).c_str()));
    for (int x = 0; x < 500; ++x) cols.push_back(D(std::to_string(x).c_str()));
    big.dir("/t", zooms);
    for (int z = 0; z < 18; ++z) big.dir("/t/" + std::to_string(z), cols);
    big.dir("/t/0/0", {F("0.png")});
    CHECK(looks_like_tile_folder(big, "/t"));
    CHECK(big.calls <= 4);

    /* the start places: home, then sticks and cards under /media/<user>, /media, /run/media/<user>, /mnt */
    FakeLister roots;
    roots.dir("/media/deck", {D("MAPS"), D(".trash")});
    roots.dir("/media", {D("deck"), D("CARD")});
    roots.dir("/mnt", {D("usb")});
    std::vector<BrowserRow> r = browser_roots(roots, "/home/deck", "deck");
    CHECK_EQ(r.size(), static_cast<std::size_t>(4));
    CHECK_EQ(r[0].label, std::string("Home folder"));
    CHECK_EQ(r[0].path, std::string("/home/deck"));
    CHECK_EQ(r[1].path, std::string("/media/deck/MAPS"));
    CHECK_EQ(r[1].value, std::string("USB stick or card"));
    CHECK_EQ(r[2].path, std::string("/media/CARD"));
    CHECK_EQ(r[3].path, std::string("/mnt/usb"));
    CHECK(r[0].kind == RowKind::Root);
    CHECK_EQ(browser_roots(roots, "", "").size(), static_cast<std::size_t>(3));

    /* the real lister on a temporary tile folder */
    const std::string tmp = test_temp_dir();
    CHECK(!tmp.empty());
    const std::string t = tmp + "/tiles";
    ::mkdir(t.c_str(), 0755);
    ::mkdir((t + "/15").c_str(), 0755);
    ::mkdir((t + "/15/16590").c_str(), 0755);
    ::mkdir((t + "/15/16591").c_str(), 0755);
    touch(t + "/15/16590/11268.png", 1000);
    touch(t + "/15/16590/11269.png", 2000);
    touch(t + "/15/16591/11268.png", 3000);
    touch(t + "/inventory.json", 500);
    touch(tmp + "/a.mbtiles", 4096);
    touch(tmp + "/b.txt", 10);
    PosixLister px;
    CHECK(looks_like_tile_folder(px, t));
    CHECK(!looks_like_tile_folder(px, tmp));
    std::uint64_t files = 0;
    CHECK_EQ(folder_bytes(t, &files, nullptr), static_cast<std::uint64_t>(6000));
    CHECK_EQ(files, static_cast<std::uint64_t>(3));
    rows = browser_rows(px, tmp, true, &err);
    CHECK_EQ(rows.size(), static_cast<std::size_t>(3)); /* back, tiles, a.mbtiles */
    CHECK_EQ(rows[2].label, std::string("a.mbtiles"));
    CHECK_EQ(rows[2].value, std::string("4 KB"));
    CHECK_EQ(file_size(tmp + "/a.mbtiles"), static_cast<std::uint64_t>(4096));
    CHECK_EQ(file_size(t), static_cast<std::uint64_t>(0));
    CHECK(free_bytes(tmp) > 0);
    test_remove_tree(tmp);
}
