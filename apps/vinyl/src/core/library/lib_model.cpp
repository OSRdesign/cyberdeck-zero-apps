/*
 * SPDX-License-Identifier: MIT
 *
 * Vinyl library pages, the data side (see lib_model.hpp).
 */
#include "lib_model.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <ctime>

namespace vinyl {
namespace libm {

namespace {

const char *const kMonths[12] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};

bool decode(const std::string &s, size_t &i, uint32_t &cp) {
    const unsigned char c = static_cast<unsigned char>(s[i]);
    int n = 0;
    if (c < 0x80) {
        cp = c;
        n = 1;
    } else if ((c & 0xE0) == 0xC0) {
        cp = c & 0x1F;
        n = 2;
    } else if ((c & 0xF0) == 0xE0) {
        cp = c & 0x0F;
        n = 3;
    } else if ((c & 0xF8) == 0xF0) {
        cp = c & 0x07;
        n = 4;
    } else {
        return false;
    }
    if (i + static_cast<size_t>(n) > s.size()) return false;
    for (int k = 1; k < n; ++k) {
        const unsigned char d = static_cast<unsigned char>(s[i + static_cast<size_t>(k)]);
        if ((d & 0xC0) != 0x80) return false;
        cp = (cp << 6) | (d & 0x3F);
    }
    i += static_cast<size_t>(n);
    return true;
}

void encode(std::string &out, uint32_t cp) {
    if (cp < 0x80) {
        out += static_cast<char>(cp);
    } else if (cp < 0x800) {
        out += static_cast<char>(0xC0 | (cp >> 6));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += static_cast<char>(0xE0 | (cp >> 12));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (cp >> 18));
        out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
}

uint32_t upper_cp(uint32_t c) {
    if (c >= 'a' && c <= 'z') return c - 0x20;
    if (c < 0xB5) return c;
    if (c >= 0xE0 && c <= 0xFE && c != 0xF7) return c - 0x20;
    if (c == 0xFF) return 0x178;
    if ((c >= 0x100 && c <= 0x137) || (c >= 0x14A && c <= 0x177)) return (c & 1) ? c - 1 : c;
    if ((c >= 0x139 && c <= 0x148) || (c >= 0x179 && c <= 0x17E)) return (c & 1) ? c : c - 1;
    if (c >= 0x3B1 && c <= 0x3C9 && c != 0x3C2) return c - 0x20;
    if (c >= 0x430 && c <= 0x44F) return c - 0x20;
    if (c >= 0x450 && c <= 0x45F) return c - 0x50;
    return c;
}

Row item(Row::Type type, int64_t id, int index, const std::string &text, const std::string &value, Row::Mark mark) {
    Row r;
    r.type = type;
    r.id = id;
    r.index = index;
    r.text = text;
    r.value = value;
    r.mark = mark;
    return r;
}

Row heading(const std::string &t) {
    Row r;
    r.kind = Row::Kind::Heading;
    r.mark = Row::Mark::None;
    r.text = t;
    return r;
}

std::string base_name(const std::string &p) {
    std::string s = p;
    while (s.size() > 1 && s.back() == '/') s.pop_back();
    const size_t slash = s.rfind('/');
    return slash == std::string::npos ? s : s.substr(slash + 1);
}

}  // namespace

std::string count_text(int64_t n) {
    const bool neg = n < 0;
    std::string d = std::to_string(neg ? -n : n), out;
    for (size_t i = 0; i < d.size(); ++i) {
        if (i && (d.size() - i) % 3 == 0) out += ',';
        out += d[i];
    }
    return neg ? "-" + out : out;
}

std::string plural(int64_t n, const char *one, const char *many) {
    return n == 1 ? "1 " + std::string(one) : count_text(n) + " " + many;
}

std::string upper_text(const std::string &s) {
    std::string out;
    out.reserve(s.size());
    size_t i = 0;
    while (i < s.size()) {
        const size_t at = i;
        uint32_t cp = 0;
        if (!decode(s, i, cp)) {
            out += s[at];
            i = at + 1;
            continue;
        }
        encode(out, upper_cp(cp));
    }
    return out;
}

std::string duration_text(uint64_t ms) {
    if (ms == 0) return "";
    const uint64_t s = ms / 1000;
    char b[32];
    if (s >= 3600) std::snprintf(b, sizeof b, "%u:%02u:%02u", unsigned(s / 3600), unsigned((s / 60) % 60), unsigned(s % 60));
    else std::snprintf(b, sizeof b, "%02u:%02u", unsigned(s / 60), unsigned(s % 60));
    return b;
}

std::string length_text(uint64_t ms) {
    const uint64_t s = ms / 1000;
    char b[32];
    if (s >= 3600) std::snprintf(b, sizeof b, "%u:%02u:%02u", unsigned(s / 3600), unsigned((s / 60) % 60), unsigned(s % 60));
    else std::snprintf(b, sizeof b, "%u:%02u", unsigned(s / 60), unsigned(s % 60));
    return b;
}

static bool broken_down(int64_t epoch, bool utc, std::tm &tm) {
    const time_t t = static_cast<time_t>(epoch);
    return (utc ? gmtime_r(&t, &tm) : localtime_r(&t, &tm)) != nullptr;
}

std::string when_text(int64_t epoch, bool utc) {
    std::tm tm{};
    if (!broken_down(epoch, utc, tm)) return "";
    char b[48];
    std::snprintf(b, sizeof b, "%d %s %02d:%02d", tm.tm_mday, kMonths[tm.tm_mon % 12], tm.tm_hour, tm.tm_min);
    return b;
}

std::string when_comma_text(int64_t epoch, bool utc) {
    std::tm tm{};
    if (!broken_down(epoch, utc, tm)) return "";
    char b[48];
    std::snprintf(b, sizeof b, "%d %s, %02d:%02d", tm.tm_mday, kMonths[tm.tm_mon % 12], tm.tm_hour, tm.tm_min);
    return b;
}

std::string home_short(const std::string &path, const std::string &home) {
    if (home.size() > 1 && path.compare(0, home.size(), home) == 0 && (path.size() == home.size() || path[home.size()] == '/'))
        return "~" + path.substr(home.size());
    return path;
}

std::string artist_name(const std::string &n) { return n.empty() ? "Unknown artist" : n; }
std::string album_name(const std::string &n) { return n.empty() ? "Unknown album" : n; }
std::string genre_name(const std::string &n) { return n.empty() ? "No genre" : n; }

/* ------------------------------------------------------------------------------------------------ rows */

std::vector<Row> hub_rows(const LibraryCounts &c, bool counts_known) {
    auto n = [&](int v) { return counts_known ? count_text(v) : std::string(); };
    std::vector<Row> rows;
    auto add = [&](const char *key, const char *text, const std::string &value) {
        Row r = item(Row::Type::Hub, 0, static_cast<int>(rows.size()), text, value, Row::Mark::Chevron);
        r.key = key;
        r.mono_value = true;
        rows.push_back(r);
    };
    add("folders", "Folders", "");
    add("playlists", "Playlists", "");
    add("artists", "Artists", n(c.artists));
    add("albums", "Albums", n(c.albums));
    add("songs", "Songs", n(c.tracks));
    add("genres", "Genres", n(c.genres));
    return rows;
}

std::string hub_heading(int tracks, bool scanning) {
    return "LIBRARY, " + count_text(tracks) + (scanning ? " TRACKS SO FAR" : tracks == 1 ? " TRACK" : " TRACKS");
}

std::vector<Row> artist_rows(const std::vector<ArtistRow> &a, int first_index) {
    std::vector<Row> rows;
    for (size_t i = 0; i < a.size(); ++i) {
        Row r = item(Row::Type::Artist, a[i].id, first_index + static_cast<int>(i), artist_name(a[i].name),
                     plural(a[i].albums, "album", "albums"), Row::Mark::Chevron);
        rows.push_back(r);
    }
    return rows;
}

std::vector<Row> album_rows(const std::vector<AlbumRow> &a, bool show_artist, int first_index) {
    std::vector<Row> rows;
    for (size_t i = 0; i < a.size(); ++i) {
        std::string v;
        if (show_artist) v = a[i].artist.empty() ? std::string() : a[i].artist;
        else if (a[i].year > 0) v = std::to_string(a[i].year);
        Row r = item(Row::Type::Album, a[i].id, first_index + static_cast<int>(i), album_name(a[i].title), v, Row::Mark::Chevron);
        r.mono_value = !show_artist;
        rows.push_back(r);
    }
    return rows;
}

std::vector<Row> album_track_rows(const std::vector<TrackRow> &t, const std::string &playing_path) {
    std::vector<Row> rows;
    int prev_disc = -1;
    for (size_t i = 0; i < t.size(); ++i) {
        if (i > 0 && t[i].disc != prev_disc && t[i].disc > 1) rows.push_back(heading("DISC " + std::to_string(t[i].disc)));
        prev_disc = t[i].disc;
        std::string text = t[i].track > 0 ? std::to_string(t[i].track) + "  " + t[i].title : t[i].title;
        Row r = item(Row::Type::Track, t[i].id, static_cast<int>(i), text, duration_text(t[i].duration_ms), Row::Mark::Dot);
        r.mono_value = true;
        r.playing = !playing_path.empty() && t[i].path == playing_path;
        rows.push_back(r);
    }
    return rows;
}

std::vector<Row> song_rows(const std::vector<TrackRow> &t, const std::string &playing_path, int first_index) {
    std::vector<Row> rows;
    for (size_t i = 0; i < t.size(); ++i) {
        Row r = item(Row::Type::Track, t[i].id, first_index + static_cast<int>(i), t[i].title, t[i].artist, Row::Mark::Dot);
        r.playing = !playing_path.empty() && t[i].path == playing_path;
        rows.push_back(r);
    }
    return rows;
}

std::vector<Row> genre_rows(const std::vector<GenreRow> &g, int first_index) {
    std::vector<Row> rows;
    for (size_t i = 0; i < g.size(); ++i)
        rows.push_back(item(Row::Type::Genre, g[i].id, first_index + static_cast<int>(i), genre_name(g[i].name),
                            plural(g[i].tracks, "track", "tracks"), Row::Mark::Chevron));
    return rows;
}

std::vector<Row> search_rows(const SearchResults &r, const std::string &playing_path) {
    std::vector<Row> rows;
    if (!r.songs.empty()) {
        rows.push_back(heading("SONGS"));
        for (size_t i = 0; i < r.songs.size(); ++i) {
            Row x = item(Row::Type::Track, r.songs[i].id, static_cast<int>(i), r.songs[i].title, r.songs[i].artist, Row::Mark::Dot);
            x.playing = !playing_path.empty() && r.songs[i].path == playing_path;
            rows.push_back(x);
        }
    }
    if (!r.albums.empty()) {
        rows.push_back(heading("ALBUMS"));
        for (size_t i = 0; i < r.albums.size(); ++i)
            rows.push_back(item(Row::Type::Album, r.albums[i].id, static_cast<int>(i), album_name(r.albums[i].title),
                                r.albums[i].artist, Row::Mark::Chevron));
    }
    if (!r.artists.empty()) {
        rows.push_back(heading("ARTISTS"));
        for (size_t i = 0; i < r.artists.size(); ++i)
            rows.push_back(item(Row::Type::Artist, r.artists[i].id, static_cast<int>(i), artist_name(r.artists[i].name),
                                plural(r.artists[i].albums, "album", "albums"), Row::Mark::Chevron));
    }
    if (!r.genres.empty()) {
        rows.push_back(heading("GENRES"));
        for (size_t i = 0; i < r.genres.size(); ++i)
            rows.push_back(item(Row::Type::Genre, r.genres[i].id, static_cast<int>(i), genre_name(r.genres[i].name), "",
                                Row::Mark::Chevron));
    }
    return rows;
}

std::string artists_heading(int n) { return "ARTISTS, " + count_text(n); }
std::string albums_heading(int n) { return "ALBUMS, " + count_text(n); }
std::string songs_heading(int n) { return "SONGS, " + count_text(n); }
std::string genres_heading(int n) { return "GENRES, " + count_text(n); }

std::string artist_heading(const std::string &name, int albums, int tracks) {
    return upper_text(artist_name(name)) + ", " + upper_text(plural(albums, "album", "albums")) + ", " +
           upper_text(plural(tracks, "track", "tracks"));
}

std::string album_heading(const std::string &title, int year, int tracks) {
    std::string h = upper_text(album_name(title));
    if (year > 0) h += ", " + std::to_string(year);
    return h + ", " + upper_text(plural(tracks, "track", "tracks"));
}

std::string genre_heading(const std::string &name, int albums) {
    return upper_text(genre_name(name)) + ", " + upper_text(plural(albums, "album", "albums"));
}

int first_row(const std::vector<Row> &rows) {
    for (size_t i = 0; i < rows.size(); ++i)
        if (rows[i].kind == Row::Kind::Item) return static_cast<int>(i);
    return -1;
}

int step_row(const std::vector<Row> &rows, int from, int d) {
    int i = from + d;
    while (i >= 0 && i < static_cast<int>(rows.size())) {
        if (rows[static_cast<size_t>(i)].kind == Row::Kind::Item) return i;
        i += d;
    }
    return -1;
}

/* ------------------------------------------------------------------------------------------------ paging */

void Pager::reset(int total) {
    total_ = std::max(0, total);
    loaded_ = 0;
    pending_ = false;
}

bool Pager::want_more(int selected) {
    if (pending_ || loaded_ >= total_) return false;
    if (selected < loaded_ - kAhead) return false;
    pending_ = true;
    return true;
}

void Pager::appended(int n) {
    loaded_ = std::min(total_, loaded_ + std::max(0, n));
    pending_ = false;
    if (n <= 0) total_ = loaded_; /* the library has no more (rows went away under a scan): stop asking */
}

/* ------------------------------------------------------------------------------------------------ notes */

ScanNote scan_note(const ScanProgress &p, int prior_tracks) {
    ScanNote n;
    if (p.state != ScanProgress::State::Scanning && p.state != ScanProgress::State::Paused) return n;
    n.shown = true;
    const int64_t files = static_cast<int64_t>(p.files);
    if (prior_tracks > 0) n.pct = static_cast<int>(std::min<int64_t>(99, files * 100 / std::max<int64_t>(prior_tracks, files + 1)));
    if (p.state == ScanProgress::State::Paused) {
        n.hot = true;
        n.text = "Paused: the board is hot. Resumes when it cools.";
        return n;
    }
    if (prior_tracks > files) n.text = "Scanning: " + count_text(files) + " of about " + count_text(prior_tracks) + " files";
    else n.text = "Scanning: " + count_text(files) + (files == 1 ? " file" : " files");
    return n;
}

std::string scan_heading(const ScanProgress &p, int prior_tracks) {
    const int64_t files = static_cast<int64_t>(p.files);
    if (p.state == ScanProgress::State::Paused) return "PAUSED, THE BOARD IS HOT";
    if (prior_tracks > files) return "SCANNING, " + count_text(files) + " OF " + count_text(prior_tracks);
    return "SCANNING, " + count_text(files) + (files == 1 ? " FILE" : " FILES");
}

std::string last_scan_text(const LibraryStats &s, bool utc) {
    if (s.last_scan <= 0) return "Not scanned yet";
    return "Last scan " + when_comma_text(s.last_scan, utc) + ": " + plural(s.counts.tracks, "track", "tracks") + ", " +
           plural(s.counts.albums, "album", "albums");
}

bool is_drive_path(const std::string &path) {
    for (const char *p : {"/media/", "/run/media/", "/mnt/"})
        if (path.compare(0, std::strlen(p), p) == 0) return true;
    return false;
}

int online_roots(const std::vector<RootInfo> &roots) {
    int n = 0;
    for (const RootInfo &r : roots) n += r.online;
    return n;
}

std::vector<Row> folder_rows(const std::vector<RootInfo> &roots, const std::string &home, const std::vector<DriveInfo> &drives) {
    std::vector<Row> rows;
    for (size_t i = 0; i < roots.size(); ++i) {
        const RootInfo &r = roots[i];
        if (!r.online) continue; /* gone is gone: no row (the rows stay in the database, hidden, for the day it is back) */
        std::string text = home_short(r.path, home);
        std::string value = plural(r.tracks, "track", "tracks"), key;
        if (is_drive_path(r.path)) text = (r.label.empty() ? base_name(r.path) : r.label) + " (USB drive)";
        for (const DriveInfo &d : drives)
            if (d.mounted && d.mount_dir == r.path) {
                text = d.name() + " (USB drive)";
                key = (d.by_vinyl ? "vdrive:" : "sdrive:") + d.dev;
                if (d.by_vinyl) value = "mounted by Vinyl";
            }
        Row row = item(Row::Type::None, r.id, static_cast<int>(i), text, value, Row::Mark::None);
        row.key = key;
        rows.push_back(row);
    }
    for (const DriveInfo &d : drives) { /* mounted, its root not made yet (a moment after a mount): the row is there already */
        if (!d.mounted) continue;
        const std::string key = (d.by_vinyl ? "vdrive:" : "sdrive:") + d.dev;
        if (std::any_of(rows.begin(), rows.end(), [&](const Row &r) { return r.key == key; })) continue;
        Row row = item(Row::Type::None, 0, -1, d.name() + " (USB drive)", d.by_vinyl ? "mounted by Vinyl" : size_text(d.size) + ", mounted", Row::Mark::None);
        row.key = key;
        rows.push_back(row);
    }
    for (const DriveInfo &d : drives) {
        if (d.mounted) continue;
        Row row = item(Row::Type::None, 0, -1, d.name() + " (USB drive)", "not mounted", Row::Mark::Dot);
        row.key = "ndrive:" + d.dev;
        row.amber = true; /* it can be mounted from here (owner, 2026-11-14): not dimmed, not inert */
        rows.push_back(row);
    }
    return rows;
}

std::vector<Row> settings_rows(int folders, const std::string &last_scan, bool scanning) {
    std::vector<Row> rows;
    Row f = item(Row::Type::None, 0, 0, "Music folders", std::to_string(folders), Row::Mark::Chevron);
    f.key = "folders";
    rows.push_back(f);
    Row r = item(Row::Type::None, 0, 1, "Rescan now", "", Row::Mark::Dot);
    r.key = "rescan";
    r.dimmed = scanning; /* a scan is running: nothing to start */
    rows.push_back(r);
    Row l = item(Row::Type::None, 0, 2, last_scan, "", Row::Mark::None); /* information: no marker (D-45) */
    l.key = "last";
    l.dimmed = true;
    rows.push_back(l);
    return rows;
}

/* ------------------------------------------------------------------------------------------------ track info */

std::string format_text(const TrackDetails &d) {
    std::string codec = d.row.codec;
    for (char &c : codec) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    if (codec.empty()) codec = "Unknown format";
    std::string s = codec;
    if (d.rate) {
        char b[24];
        if (d.rate % 1000 == 0) std::snprintf(b, sizeof b, "%u kHz", d.rate / 1000);
        else std::snprintf(b, sizeof b, "%.1f kHz", d.rate / 1000.0);
        s += std::string(", ") + b;
    }
    if (d.bits) s += ", " + std::to_string(d.bits) + " bit";
    else if (d.kbps) s += ", " + std::to_string(d.kbps) + " kbps";
    return s;
}

std::string replaygain_text(const ReplayGainTags &rg) {
    char b[48];
    std::string s;
    if (rg.has_track) {
        std::snprintf(b, sizeof b, "track %+.1f dB", static_cast<double>(rg.track_db));
        s = b;
    }
    if (rg.has_album) {
        std::snprintf(b, sizeof b, "album %+.1f dB", static_cast<double>(rg.album_db));
        s += (s.empty() ? "" : ", ") + std::string(b);
    }
    return s.empty() ? "none" : s;
}

std::string played_text(int plays, int64_t last, bool utc) {
    if (plays <= 0) return "Never";
    std::string s = plural(plays, "time", "times");
    if (last > 0) s += ", last " + when_text(last, utc);
    return s;
}

std::vector<std::pair<std::string, std::string>> track_info(const TrackDetails &d, const std::string &home, bool utc) {
    auto dash = [](const std::string &s) { return s.empty() ? std::string("-") : s; };
    std::vector<std::pair<std::string, std::string>> v;
    v.emplace_back("Title", dash(d.row.title));
    v.emplace_back("Artist", artist_name(d.row.artist));
    v.emplace_back("Album", album_name(d.row.album));
    std::string yg = d.year > 0 ? std::to_string(d.year) : std::string();
    if (!d.genre.empty()) yg += (yg.empty() ? "" : ", ") + d.genre;
    v.emplace_back("Year, genre", dash(yg));
    std::string td;
    if (d.row.track > 0) td = std::to_string(d.row.track);
    if (d.row.disc > 0) td += (td.empty() ? "" : ", ") + std::string("disc ") + std::to_string(d.row.disc);
    v.emplace_back("Track, disc", dash(td));
    v.emplace_back("Length", d.row.duration_ms ? length_text(d.row.duration_ms) : std::string("-"));
    v.emplace_back("Format", format_text(d));
    v.emplace_back("ReplayGain", replaygain_text(d.rg));
    v.emplace_back("Played", played_text(d.plays, d.last_played, utc));
    v.emplace_back("File", home_short(d.row.path, home));
    return v;
}

/* ------------------------------------------------------------------------------------------------ sheets */

static const char *const kHoldHint = "Hold a row for 1.5 s, or press Ctrl+O, to open this sheet.";

Sheet track_sheet(const std::string &title, const std::string &artist) {
    Sheet s;
    s.kind = Sheet::Kind::Track;
    s.title = upper_text(artist.empty() ? title : title + ", " + artist);
    s.rows = {{"play", "Play", "Enter", true},
              {"next", "Play next", "N", true},
              {"add", "Add to queue", "A", true},
              {"playlist", "Add to playlist", "P", false}, /* P2c: drawn dimmed, does nothing */
              {"info", "Track info", "I", true}};
    s.hint = kHoldHint;
    return s;
}

Sheet album_sheet(const std::string &title, int tracks) {
    Sheet s;
    s.kind = Sheet::Kind::Album;
    s.title = upper_text(album_name(title)) + ", " + upper_text(plural(tracks, "track", "tracks"));
    s.rows = {{"play", "Play album", "Enter", true},
              {"next", "Play next", "N", true},
              {"add", "Add to queue", "A", true},
              {"playlist", "Add to playlist", "P", false}};
    s.hint = kHoldHint;
    return s;
}

Sheet artist_sheet(const std::string &name, int albums) {
    Sheet s;
    s.kind = Sheet::Kind::Artist;
    s.title = upper_text(artist_name(name)) + ", " + upper_text(plural(albums, "album", "albums"));
    s.rows = {{"play", "Play all", "Enter", true},
              {"next", "Play next", "N", true},
              {"add", "Add to queue", "A", true},
              {"playlist", "Add to playlist", "P", false}};
    s.hint = kHoldHint;
    return s;
}

std::string sheet_action_for_key(uint32_t evdev) {
    switch (evdev) {
    case kKeyN: return "next";
    case kKeyA: return "add";
    case kKeyP: return "playlist";
    case kKeyI: return "info";
    default: return "";
    }
}

int sheet_index(const Sheet &s, const std::string &id) {
    for (size_t i = 0; i < s.rows.size(); ++i)
        if (s.rows[i].id == id) return static_cast<int>(i);
    return -1;
}

bool Nav::pop() {
    if (stack_.size() <= 1) return false;
    stack_.pop_back();
    return true;
}

}  // namespace libm
}  // namespace vinyl
