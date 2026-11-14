/*
 * SPDX-License-Identifier: MIT
 *
 * Vinyl tests: core/playlist/m3u (P2c, report 060 2.4): Windows paths with backslashes and drive letters, Latin-1 bytes, a BOM,
 * CRLF, relative and absolute paths, missing files kept, URL lines skipped, #EXTINF, the writer and the round trip.
 */
#include "check.hpp"

#include "playlist/m3u.hpp"

#include <set>

using namespace vinyl;

namespace {

m3u::ParseOptions opts(const std::string &base, const std::set<std::string> &files, std::vector<std::string> roots = {}) {
    m3u::ParseOptions o;
    o.base_dir = base;
    o.roots = std::move(roots);
    o.exists = [files](const std::string &p) { return files.count(p) != 0; };
    return o;
}

}  // namespace

void test_m3u() {
    /* paths, normalizing and relative */
    CHECK_EQ(m3u::normalize("/a/b/../c/./d//e"), std::string("/a/c/d/e"));
    CHECK_EQ(m3u::normalize("/a/.."), std::string("/"));
    CHECK_EQ(m3u::normalize("/.."), std::string("/"));
    CHECK_EQ(m3u::relative_to("/home/pi/Music/Playlists", "/home/pi/Music/A/x.mp3"), std::string("../A/x.mp3"));
    CHECK_EQ(m3u::relative_to("/media/u/S", "/media/u/S/Music/x.mp3"), std::string("Music/x.mp3"));
    CHECK_EQ(m3u::relative_to("/a", "/b/x.mp3"), std::string("")); /* nothing shared: absolute */
    CHECK_EQ(m3u::dir_of("/a/b/c.m3u8"), std::string("/a/b"));
    CHECK_EQ(m3u::dir_of("/c.m3u8"), std::string("/"));

    /* UTF-8, BOM, CRLF, a lone CR, #EXTINF, comments, blank lines */
    {
        const std::set<std::string> f = {"/m/a.mp3", "/m/sub/b.flac", "/abs/c.ogg"};
        const std::string text = std::string("\xEF\xBB\xBF") + "#EXTM3U\r\n#EXTINF:123,Artist - First\r\na.mp3\r\n\r\n# a comment\r\n"
                                 "#EXTINF:-1,Second\rsub/b.flac\r/abs/c.ogg\n";
        const auto e = m3u::parse(text, opts("/m", f));
        CHECK_EQ(e.size(), static_cast<size_t>(3));
        if (e.size() == 3) {
            CHECK_EQ(e[0].path, std::string("/m/a.mp3"));
            CHECK_EQ(e[0].seconds, 123);
            CHECK_EQ(e[0].text, std::string("Artist - First"));
            CHECK_EQ(e[1].path, std::string("/m/sub/b.flac"));
            CHECK_EQ(e[1].seconds, -1);
            CHECK_EQ(e[1].text, std::string("Second"));
            CHECK_EQ(e[2].path, std::string("/abs/c.ogg"));
            CHECK_EQ(e[2].seconds, -1);
            CHECK(e[2].text.empty());
            CHECK(e[0].resolved && e[1].resolved && e[2].resolved);
        }
        /* a line with a CR LF at the very end and none: both read */
        CHECK_EQ(m3u::parse("a.mp3", opts("/m", f)).size(), static_cast<size_t>(1));
        CHECK_EQ(m3u::parse("", opts("/m", f)).size(), static_cast<size_t>(0));
    }

    /* relative paths are made lexical: "../" climbs, "./" goes */
    {
        const std::set<std::string> f = {"/m/Music/x.mp3", "/m/y.mp3"};
        const auto e = m3u::parse("./Music/x.mp3\nMusic/../y.mp3\n", opts("/m", f));
        CHECK_EQ(e.size(), static_cast<size_t>(2));
        if (e.size() == 2) {
            CHECK_EQ(e[0].path, std::string("/m/Music/x.mp3"));
            CHECK_EQ(e[1].path, std::string("/m/y.mp3"));
        }
    }

    /* Windows: backslashes, a drive letter resolved against the playlist's own folder, then the roots; the missing one is kept as
     * it was read, not resolved */
    {
        const std::set<std::string> f = {"/media/u/S/Artist/One.mp3", "/home/pi/Music/Band/Two.mp3", "/media/u/S/rel/three.mp3"};
        const std::string text = "D:\\Music\\Artist\\One.mp3\r\nC:\\Users\\me\\Music\\Band\\Two.mp3\r\nrel\\three.mp3\r\nD:\\Music\\Gone.mp3\r\n";
        const auto e = m3u::parse(text, opts("/media/u/S", f, {"/home/pi/Music"}));
        CHECK_EQ(e.size(), static_cast<size_t>(4));
        if (e.size() == 4) {
            CHECK_EQ(e[0].path, std::string("/media/u/S/Artist/One.mp3")); /* "Music" dropped under the playlist's folder */
            CHECK(e[0].resolved);
            CHECK_EQ(e[1].path, std::string("/home/pi/Music/Band/Two.mp3")); /* found under a known root */
            CHECK(e[1].resolved);
            CHECK_EQ(e[2].path, std::string("/media/u/S/rel/three.mp3"));
            CHECK_EQ(e[3].path, std::string("D:\\Music\\Gone.mp3"));
            CHECK(!e[3].resolved);
        }
    }

    /* file:// URLs (percent escapes, a Windows drive), http(s) and others skipped, and the #EXTINF of a skipped line not carried */
    {
        const std::set<std::string> f = {"/m/a b.mp3", "/m/next.mp3", "/media/u/S/Music/z.mp3"};
        const std::string text = "file:///m/a%20b.mp3\r\n#EXTINF:5,Stream\r\nhttp://radio.example/live\r\nhttps://x/y.mp3\r\nrtsp://h/p\r\n"
                                 "next.mp3\r\nfile:///D:/Music/z.mp3\r\n";
        const auto e = m3u::parse(text, opts("/media/u/S", f, {"/m"}));
        CHECK_EQ(e.size(), static_cast<size_t>(3));
        if (e.size() == 3) {
            CHECK_EQ(e[0].path, std::string("/m/a b.mp3"));
            CHECK_EQ(e[1].path, std::string("/media/u/S/next.mp3")); /* relative to the playlist: not there, kept */
            CHECK(e[1].text.empty());                               /* the skipped stream's #EXTINF did not leak */
            CHECK_EQ(e[2].path, std::string("/media/u/S/Music/z.mp3"));
        }
    }

    /* Latin-1 (Windows-1252) bytes: accents, and the file named by the same bytes in UTF-8 on the disk */
    {
        const std::string name_utf8 = "Caf\xC3\xA9 del Mar \xE2\x80\x93 \xC3\x89t\xC3\xA9.mp3"; /* Café del Mar – Été.mp3 (en dash) */
        const std::set<std::string> f = {"/m/" + name_utf8};
        const std::string latin = "#EXTINF:60,Caf\xE9 del Mar\r\nCaf\xE9 del Mar \x96 \xC9t\xE9.mp3\r\n";
        CHECK(!m3u::valid_utf8(latin));
        bool was = false;
        const std::string dec = m3u::decode_text(latin, &was);
        CHECK(was);
        CHECK(m3u::valid_utf8(dec));
        const auto e = m3u::parse(latin, opts("/m", f));
        CHECK_EQ(e.size(), static_cast<size_t>(1));
        if (e.size() == 1) {
            CHECK_EQ(e[0].path, "/m/" + name_utf8);
            CHECK_EQ(e[0].text, std::string("Caf\xC3\xA9 del Mar"));
            CHECK_EQ(e[0].seconds, 60);
        }
        /* a real UTF-8 text is not taken for Latin-1 */
        CHECK(m3u::valid_utf8("Caf\xC3\xA9"));
        was = true;
        CHECK_EQ(m3u::decode_text("Caf\xC3\xA9", &was), std::string("Caf\xC3\xA9"));
        CHECK(!was);
        /* a Latin-1 file whose disk name kept the Latin-1 bytes is found by them */
        const std::set<std::string> f2 = {std::string("/m/Caf\xE9.mp3")};
        const auto e2 = m3u::parse("Caf\xE9.mp3\n", opts("/m", f2));
        CHECK_EQ(e2.size(), static_cast<size_t>(1));
        if (e2.size() == 1) CHECK_EQ(e2[0].path, std::string("/m/Caf\xE9.mp3"));
        /* an invalid sequence is Latin-1, an overlong one too; UTF-16 with a BOM is read */
        CHECK(!m3u::valid_utf8("\xC0\x80"));
        CHECK(!m3u::valid_utf8("abc\xE2\x82"));
        const std::string u16 = std::string("\xFF\xFE") + std::string("a\0.\0m\0p\0" "3\0", 10);
        CHECK_EQ(m3u::decode_text(u16), std::string("a.mp3"));
    }

    /* missing entries are KEPT (the best guess), never dropped; the cap holds */
    {
        const auto e = m3u::parse("/gone/a.mp3\nb.mp3\n", opts("/m", {}));
        CHECK_EQ(e.size(), static_cast<size_t>(2));
        if (e.size() == 2) {
            CHECK_EQ(e[0].path, std::string("/gone/a.mp3"));
            CHECK(e[0].resolved);
            CHECK_EQ(e[1].path, std::string("/m/b.mp3"));
        }
        m3u::ParseOptions o = opts("/m", {});
        o.max_entries = 3;
        CHECK_EQ(m3u::parse("a\nb\nc\nd\ne\n", o).size(), static_cast<size_t>(3));
    }
    /* the default existence test is the real one: nothing at /nonexistent-vinyl is there, but the entry stays */
    {
        m3u::ParseOptions o;
        o.base_dir = "/nonexistent-vinyl";
        const auto e = m3u::parse("x.mp3\n", o);
        CHECK_EQ(e.size(), static_cast<size_t>(1));
    }

    /* the writer: UTF-8, #EXTM3U, #EXTINF only where there is something to say, relative on the same drive, absolute across,
     * an unresolved entry as it was read */
    {
        std::vector<m3u::Entry> in;
        m3u::Entry a;
        a.path = "/home/pi/Music/A/One.mp3";
        a.text = "Artist - One";
        a.seconds = 201;
        m3u::Entry b;
        b.path = "/media/u/S/Two.mp3";
        m3u::Entry c;
        c.path = "D:\\Music\\Gone.mp3";
        c.resolved = false;
        m3u::Entry d;
        d.path = "/home/pi/Elsewhere/Three \xC3\xA9.mp3";
        d.seconds = 7;
        in = {a, b, c, d};
        m3u::WriteOptions w;
        w.playlist_dir = "/home/pi/Music/Playlists";
        w.same_drive = [](const std::string &x, const std::string &y) {
            return (x.compare(0, 7, "/media/") == 0) == (y.compare(0, 7, "/media/") == 0);
        };
        const std::string text = m3u::write(in, w);
        CHECK_EQ(text,
                 std::string("#EXTM3U\n#EXTINF:201,Artist - One\n../A/One.mp3\n/media/u/S/Two.mp3\nD:\\Music\\Gone.mp3\n"
                             "#EXTINF:7,\n../../Elsewhere/Three \xC3\xA9.mp3\n"));
        CHECK(text.find('\r') == std::string::npos);
        CHECK(m3u::valid_utf8(text));

        /* a round trip: what was written reads back to the same entries (missing ones too) */
        std::set<std::string> files = {a.path, b.path, d.path};
        m3u::ParseOptions o = opts("/home/pi/Music/Playlists", files);
        const auto back = m3u::parse(text, o);
        CHECK_EQ(back.size(), static_cast<size_t>(4));
        if (back.size() == 4) {
            CHECK(back[0] == a);
            CHECK(back[1].path == b.path && back[1].text.empty() && back[1].seconds == -1);
            CHECK(back[2] == c);
            CHECK(back[3].path == d.path && back[3].seconds == 7);
        }
        /* the same entries written beside a USB drive's playlist: its own tracks relative, the others absolute */
        m3u::WriteOptions w2 = w;
        w2.playlist_dir = "/media/u/S";
        const std::string t2 = m3u::write(in, w2);
        CHECK(t2.find("\nTwo.mp3\n") != std::string::npos);
        CHECK(t2.find("\n/home/pi/Music/A/One.mp3\n") != std::string::npos);
        /* no same_drive function: everything relative where a path is shared */
        m3u::WriteOptions w3;
        w3.playlist_dir = "/home/pi/Music/Playlists";
        CHECK(m3u::write({a}, w3).find("\n../A/One.mp3\n") != std::string::npos);
        /* a name with a new line cannot break the file */
        m3u::Entry nl;
        nl.path = "/m/a\nb.mp3";
        nl.text = "x\ny";
        CHECK_EQ(m3u::write({nl}, w3), std::string("#EXTM3U\n#EXTINF:-1,x y\n/m/a b.mp3\n"));
    }
    CHECK_EQ(m3u::total_seconds({}), static_cast<int64_t>(-1));
    {
        m3u::Entry x, y;
        x.seconds = 10;
        y.seconds = 5;
        CHECK_EQ(m3u::total_seconds({x, y}), static_cast<int64_t>(15));
    }
}
