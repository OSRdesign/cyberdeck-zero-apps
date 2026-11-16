/*
 * SPDX-License-Identifier: MIT
 *
 * VHS: the scenes list and the file actions (see index.hpp).
 */
#include "index.hpp"

#include "atomic_file.hpp"
#include "fileops.hpp"
#include "json.hpp"
#include "locks.hpp"
#include "record/av_libs.hpp"
#include "record/disk_guard.hpp"
#include "record/journal.hpp"
#include "settings/settings.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <ctime>

namespace vhs {
namespace {

std::string lower_ext(const std::string &name)
{
    const std::size_t dot = name.find_last_of('.');
    if (dot == std::string::npos) return "";
    std::string e = name.substr(dot + 1);
    for (char &c : e) c = static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
    return e;
}

bool movie_ext(const std::string &e) { return e == "mkv" || e == "mp4" || e == "mov"; }
bool photo_ext(const std::string &e) { return e == "jpg" || e == "jpeg"; }

std::string stem_of(const std::string &name)
{
    const std::size_t dot = name.find_last_of('.');
    return dot == std::string::npos ? name : name.substr(0, dot);
}

/* the journals: part name -> (repaired, damaged) */
std::map<std::string, std::pair<bool, bool>> journal_marks(FileOps &ops, const std::string &folder)
{
    std::map<std::string, std::pair<bool, bool>> m;
    std::vector<std::string> names;
    if (ops.list_dir(folder + "/.journal", names) != 0) return m;
    for (const std::string &n : names) {
        if (n.size() < 6 || n.compare(n.size() - 5, 5, ".json") != 0) continue;
        Journal j;
        if (!journal_read(ops, folder + "/.journal/" + n, j)) continue;
        for (const std::string &p : j.parts) m[p] = std::make_pair(j.repaired, j.state == "damaged");
    }
    return m;
}

} // namespace

std::vector<SceneEntry> list_scenes(FileOps &ops, const std::string &folder)
{
    std::vector<SceneEntry> out;
    std::vector<std::string> names;
    if (ops.list_dir(folder, names) != 0) return out;
    const std::set<std::string> locks = read_locks(ops, folder);
    const auto marks = journal_marks(ops, folder);
    for (const std::string &n : names) {
        if (n.empty() || n[0] == '.') continue;
        const std::string e = lower_ext(n);
        if (!movie_ext(e) && !photo_ext(e)) continue;
        SceneEntry s;
        s.name = n;
        s.path = folder + "/" + n;
        StatInfo st;
        if (ops.stat(s.path, st) != 0) continue;
        s.size = st.size;
        s.mtime = st.mtime;
        s.kind = photo_ext(e) ? SceneKind::Photo : SceneKind::Movie;
        s.parsed = parse_scene_name(n, s.sn);
        s.locked = locks.count(n) != 0;
        const auto it = marks.find(n);
        if (it != marks.end()) {
            s.repaired = it->second.first;
            s.damaged = it->second.second;
        }
        out.push_back(std::move(s));
    }
    std::sort(out.begin(), out.end(), [](const SceneEntry &a, const SceneEntry &b) {
        if (a.mtime != b.mtime) return a.mtime > b.mtime;
        const int na = a.parsed ? a.sn.number : 0, nb = b.parsed ? b.sn.number : 0;
        if (na != nb) return na > nb;
        const int pa = a.parsed ? a.sn.part : 1, pb = b.parsed ? b.sn.part : 1;
        if (pa != pb) return pa > pb;
        return a.name > b.name;
    });
    return out;
}

void count_scenes(FileOps &ops, const std::string &folder, int &movies, int &photos)
{
    movies = photos = 0;
    std::vector<std::string> names;
    if (ops.list_dir(folder, names) != 0) return;
    for (const std::string &n : names) {
        if (n.empty() || n[0] == '.') continue;
        const std::string e = lower_ext(n);
        if (movie_ext(e)) ++movies;
        else if (photo_ext(e)) ++photos;
    }
}

std::string duration_text(std::int64_t ms)
{
    if (ms < 0) return "--:--";
    const long long s = (ms + 500) / 1000;
    char buf[32];
    if (s >= 3600) std::snprintf(buf, sizeof buf, "%lld:%02lld:%02lld", s / 3600, (s / 60) % 60, s % 60);
    else std::snprintf(buf, sizeof buf, "%02lld:%02lld", s / 60, s % 60);
    return buf;
}

std::string video_size_label(int w, int h)
{
    const int lo = std::min(w, h), hi = std::max(w, h);
    if (lo > 0 && hi * 9 == lo * 16 && (lo == 360 || lo == 480 || lo == 720 || lo == 1080 || lo == 2160)) return std::to_string(lo) + "p";
    return std::to_string(w) + "x" + std::to_string(h);
}

std::string scene_date_text(std::int64_t mtime, bool utc)
{
    const std::time_t t = static_cast<std::time_t>(mtime);
    struct tm tmv;
    char buf[32];
    if (!(utc ? gmtime_r(&t, &tmv) : localtime_r(&t, &tmv))) return "";
    if (std::strftime(buf, sizeof buf, "%b %e %H:%M", &tmv) == 0) return "";
    std::string s = buf;
    const std::size_t dbl = s.find("  ");
    if (dbl != std::string::npos) s.erase(dbl, 1);
    return s;
}

std::string scene_label(const SceneEntry &e, bool short_form, const std::string &movie_prefix)
{
    if (!e.parsed) return stem_of(e.name); /* a foreign file: its name */
    const SceneName &n = e.sn;
    const std::string part = n.part > 1 ? " p" + std::to_string(n.part) : std::string();
    const bool renamed = n.prefix != movie_prefix && n.prefix != kPhotoPrefix && n.prefix != "scene";
    if (renamed) return n.prefix + part; /* brief 12.4 item 4: the new prefix alone */
    if (short_form) {
        char num[16];
        std::snprintf(num, sizeof num, "%03d", n.number);
        return n.prefix + "_" + num + part;
    }
    SceneName base = n;
    base.part = 1;
    return stem_of(format_scene_name(base)) + part;
}

std::string scene_meta_text(const SceneEntry &e, bool short_form, bool utc)
{
    std::string s;
    if (e.kind == SceneKind::Photo) {
        if (e.meta.ok && e.meta.w > 0) s = std::to_string(e.meta.w) + "x" + std::to_string(e.meta.h) + " - ";
        s += bytes_text(e.size);
    } else {
        s = (e.meta.ok ? duration_text(e.meta.duration_ms) : std::string("--:--")) + " - " + bytes_text(e.size);
        if (e.meta.ok && e.meta.w > 0) {
            const std::string size = video_size_label(e.meta.w, e.meta.h);
            s += " - " + (short_form ? size : e.meta.codec + " " + size);
        }
    }
    if (e.repaired) s += " - repaired";
    if (e.damaged) s += " - damaged";
    if (short_form) {
        /* the portrait rows: the day at the end ("Nov 14") */
        std::string d = scene_date_text(e.mtime, utc);
        const std::size_t sp = d.rfind(' ');
        if (sp != std::string::npos) d = d.substr(0, sp);
        if (!d.empty()) s += " - " + d;
    }
    return s;
}

bool jpeg_size(const std::uint8_t *d, std::size_t n, int &w, int &h)
{
    if (n < 4 || d[0] != 0xFF || d[1] != 0xD8) return false;
    std::size_t i = 2;
    while (i + 4 <= n) {
        if (d[i] != 0xFF) return false;
        const std::uint8_t m = d[i + 1];
        if (m == 0xFF) {
            ++i;
            continue;
        }
        if (m == 0xD8 || (m >= 0xD0 && m <= 0xD7) || m == 0x01) {
            i += 2;
            continue;
        }
        const std::size_t len = (static_cast<std::size_t>(d[i + 2]) << 8) | d[i + 3];
        if (len < 2) return false;
        const bool sof = m >= 0xC0 && m <= 0xCF && m != 0xC4 && m != 0xC8 && m != 0xCC;
        if (sof && i + 9 <= n) {
            h = (d[i + 5] << 8) | d[i + 6];
            w = (d[i + 7] << 8) | d[i + 8];
            return w > 0 && h > 0;
        }
        if (m == 0xDA || m == 0xD9) return false;
        i += 2 + len;
    }
    return false;
}

SceneMeta probe_photo(FileOps &ops, const std::string &path)
{
    SceneMeta m;
    const int fd = ops.open(path, 0 /* O_RDONLY */, 0);
    if (fd < 0) return m;
    std::vector<std::uint8_t> head(256 * 1024);
    const ssize_t got = ops.pread(fd, head.data(), head.size(), 0);
    ops.close(fd);
    if (got <= 0) return m;
    m.codec = "JPEG";
    m.ok = jpeg_size(head.data(), static_cast<std::size_t>(got), m.w, m.h);
    return m;
}

std::string codec_label(int id, int lavf_major)
{
    switch (id) {
    case av::kCodecMjpeg: return "MJPEG";
    case av::kCodecH264: return "H.264";
    case av::kCodecHevc: return "HEVC";
    case av::kCodecMpeg4: return "MPEG-4";
    case av::kCodecVp8: return "VP8";
    case av::kCodecVp9: return "VP9";
    default: break;
    }
    if (id == av::offsets_for(lavf_major).codec_av1) return "AV1";
    return "video";
}

SceneMeta probe_movie(const AvLibs &av, const std::string &path)
{
    SceneMeta m;
    av::FormatContext *ic = nullptr;
    if (av.open_input(&ic, path.c_str(), nullptr, nullptr) < 0 || !ic) return m;
    const std::int64_t dur = av.at<std::int64_t>(ic, av.off.fmt_duration);
    if (dur != av::kNoPts && dur > 0) m.duration_ms = dur / 1000; /* AV_TIME_BASE: us */
    const unsigned n = av.fmt_nb_streams(ic);
    for (unsigned i = 0; i < n; ++i) {
        av::CodecParameters *p = av.stream_par(av.fmt_stream(ic, i));
        if (av.at<int>(p, av.off.par_codec_type) != av::kMediaVideo) continue;
        m.codec = codec_label(av.at<int>(p, av.off.par_codec_id), av.lavf_major);
        m.w = av.at<int>(p, av.off.par_width);
        m.h = av.at<int>(p, av.off.par_height);
        break;
    }
    av.close_input(&ic);
    m.ok = true;
    return m;
}

/* ------------------------------------------------------------------------------------------------ the cache */

void MetaCache::load(FileOps &ops, const std::string &path)
{
    path_ = path;
    items_.clear();
    dirty_ = false;
    std::string text;
    if (read_small_file(ops, path, text, 8u << 20) != 0) return;
    json::Value v;
    if (!json::parse(text, v) || !v.is_obj()) return;
    const json::Value *e = v.get("entries");
    if (!e || !e->is_obj()) return;
    for (const auto &kv : e->obj) {
        const json::Value &o = kv.second;
        if (!o.is_obj()) continue;
        Item it;
        auto num = [&o](const char *k, double d) {
            const json::Value *x = o.get(k);
            return x && x->type == json::Value::Num ? x->n : d;
        };
        it.size = static_cast<std::uint64_t>(num("size", 0));
        it.mtime = static_cast<std::int64_t>(num("mtime", 0));
        it.meta.duration_ms = static_cast<std::int64_t>(num("duration_ms", -1));
        it.meta.w = static_cast<int>(num("w", 0));
        it.meta.h = static_cast<int>(num("h", 0));
        if (const json::Value *c = o.get("codec"); c && c->type == json::Value::Str) it.meta.codec = c->s;
        it.meta.ok = true;
        items_[kv.first] = it;
    }
}

bool MetaCache::get(const SceneEntry &e, SceneMeta &out) const
{
    const auto it = items_.find(e.name);
    if (it == items_.end() || it->second.size != e.size || it->second.mtime != e.mtime) return false;
    out = it->second.meta;
    return true;
}

void MetaCache::put(const SceneEntry &e, const SceneMeta &m)
{
    if (!m.ok) return;
    Item it;
    it.size = e.size;
    it.mtime = e.mtime;
    it.meta = m;
    items_[e.name] = it;
    dirty_ = true;
}

void MetaCache::erase(const std::string &name)
{
    if (items_.erase(name)) dirty_ = true;
}

int MetaCache::save(FileOps &ops)
{
    if (!dirty_ || path_.empty()) return 1;
    json::Value root = json::Value::object();
    json::Value entries = json::Value::object();
    for (const auto &kv : items_) {
        json::Value o = json::Value::object();
        o.set("size", json::Value::number(static_cast<double>(kv.second.size)));
        o.set("mtime", json::Value::number(static_cast<double>(kv.second.mtime)));
        o.set("duration_ms", json::Value::number(static_cast<double>(kv.second.meta.duration_ms)));
        o.set("codec", json::Value::string(kv.second.meta.codec));
        o.set("w", json::Value::number(kv.second.meta.w));
        o.set("h", json::Value::number(kv.second.meta.h));
        entries.set(kv.first, o);
    }
    root.set("entries", entries);
    const std::size_t slash = path_.find_last_of('/');
    if (slash != std::string::npos) make_dirs(ops, path_.substr(0, slash));
    const int r = atomic_write_file(ops, path_, json::write(root));
    if (r == 0) dirty_ = false;
    return r;
}

/* ------------------------------------------------------------------------------------------------ the actions */

namespace {

/* the journal that lists `name`: its stem and contents */
bool journal_of(FileOps &ops, const std::string &folder, const std::string &name, std::string &stem, Journal &j)
{
    std::vector<std::string> names;
    if (ops.list_dir(folder + "/.journal", names) != 0) return false;
    for (const std::string &n : names) {
        if (n.size() < 6 || n.compare(n.size() - 5, 5, ".json") != 0) continue;
        Journal x;
        if (!journal_read(ops, folder + "/.journal/" + n, x)) continue;
        if (std::find(x.parts.begin(), x.parts.end(), name) == x.parts.end()) continue;
        stem = n.substr(0, n.size() - 5);
        j = x;
        return true;
    }
    return false;
}

} // namespace

int rename_scene(FileOps &ops, const std::string &folder, const std::string &name, const std::string &new_prefix,
                 std::string &new_name, std::string &why)
{
    SceneName n;
    if (!parse_scene_name(name, n)) {
        why = "this file is not a VHS scene and cannot be renamed";
        return -EINVAL;
    }
    if (n.kind() == SceneKind::Photo) { /* brief 8: photos are always "photo" (the grammar) */
        why = "photos keep the name photo";
        return -EINVAL;
    }
    if (new_prefix.empty()) {
        why = "the name is empty";
        return -EINVAL;
    }
    if (new_prefix.size() > 24) {
        why = "up to 24 characters";
        return -EINVAL;
    }
    if (!valid_prefix(new_prefix)) {
        why = "use letters, digits, space, - and _";
        return -EINVAL;
    }
    n.prefix = new_prefix;
    const std::string target = format_scene_name(n);
    if (target == name) {
        new_name = name;
        return 0;
    }
    if (path_exists(ops, folder + "/" + target)) {
        why = "that name is taken";
        return -EEXIST;
    }
    int r = ops.rename(folder + "/" + name, folder + "/" + target);
    if (r < 0) {
        why = std::strerror(-r);
        return r;
    }
    ops.fsync_dir(folder);
    new_name = target;
    std::set<std::string> locks = read_locks(ops, folder);
    if (locks.erase(name)) {
        locks.insert(target);
        write_locks(ops, folder, locks);
    }
    std::string stem;
    Journal j;
    if (journal_of(ops, folder, name, stem, j)) {
        for (std::string &p : j.parts)
            if (p == name) p = target;
        if (j.file == name) j.file = target;
        journal_write(ops, folder, stem, j);
    }
    return 0;
}

int delete_scene(FileOps &ops, const std::string &folder, const std::string &name, std::string &why)
{
    if (read_locks(ops, folder).count(name)) {
        why = "the file is locked";
        return -EPERM;
    }
    const int r = ops.unlink(folder + "/" + name);
    if (r < 0 && r != -ENOENT) {
        why = std::strerror(-r);
        return r;
    }
    ops.fsync_dir(folder);
    set_locked(ops, folder, name, false);
    std::string stem;
    Journal j;
    if (journal_of(ops, folder, name, stem, j)) {
        j.parts.erase(std::remove(j.parts.begin(), j.parts.end(), name), j.parts.end());
        if (j.parts.empty()) ops.unlink(journal_path(folder, stem));
        else {
            if (j.file == name) j.file = j.parts.front();
            journal_write(ops, folder, stem, j);
        }
    }
    return 0;
}

int open_journal_count(FileOps &ops, const std::string &folder)
{
    int n = 0;
    std::vector<std::string> names;
    if (ops.list_dir(folder + "/.journal", names) != 0) return 0;
    for (const std::string &f : names) {
        if (f.size() < 6 || f.compare(f.size() - 5, 5, ".json") != 0) continue;
        Journal j;
        if (journal_read(ops, folder + "/.journal/" + f, j) && j.state == "open") ++n;
    }
    return n;
}

} // namespace vhs
