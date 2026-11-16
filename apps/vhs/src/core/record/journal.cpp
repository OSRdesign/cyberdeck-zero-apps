/*
 * SPDX-License-Identifier: MIT
 *
 * VHS: the recording journal and the repair (see journal.hpp).
 */
#include "journal.hpp"

#include "mkv_writer.hpp"
#include "scenes/atomic_file.hpp"
#include "scenes/fileops.hpp"
#include "scenes/json.hpp"

#include <cstdio>

namespace vhs {

std::string journal_path(const std::string &folder, const std::string &stem)
{
    return folder + "/.journal/" + stem + ".json";
}

std::string journal_to_json(const Journal &j)
{
    json::Value v = json::Value::object();
    v.set("state", json::Value::string(j.state));
    v.set("file", json::Value::string(j.file));
    json::Value parts;
    parts.type = json::Value::Arr;
    for (const std::string &p : j.parts) parts.arr.push_back(json::Value::string(p));
    v.set("parts", parts);
    v.set("started", json::Value::number(static_cast<double>(j.started)));
    if (j.repaired) v.set("repaired", json::Value::boolean(true));
    return json::write(v);
}

bool journal_from_json(const std::string &text, Journal &out)
{
    json::Value v;
    if (!json::parse(text, v) || !v.is_obj()) return false;
    Journal j;
    if (const json::Value *s = v.get("state"); s && s->type == json::Value::Str) j.state = s->s;
    if (const json::Value *f = v.get("file"); f && f->type == json::Value::Str) j.file = f->s;
    if (const json::Value *p = v.get("parts"); p && p->type == json::Value::Arr)
        for (const json::Value &e : p->arr)
            if (e.type == json::Value::Str) j.parts.push_back(e.s);
    if (const json::Value *t = v.get("started"); t && t->type == json::Value::Num) j.started = static_cast<std::int64_t>(t->n);
    if (const json::Value *r = v.get("repaired"); r && r->type == json::Value::Bool) j.repaired = r->b;
    if (j.parts.empty() && !j.file.empty()) j.parts.push_back(j.file);
    out = j;
    return true;
}

int journal_write(FileOps &ops, const std::string &folder, const std::string &stem, const Journal &j)
{
    int r = make_dirs(ops, folder + "/.journal");
    if (r != 0) return r;
    return atomic_write_file(ops, journal_path(folder, stem), journal_to_json(j));
}

bool journal_read(FileOps &ops, const std::string &path, Journal &out)
{
    std::string text;
    if (read_small_file(ops, path, text, 1u << 16) != 0) return false;
    return journal_from_json(text, out);
}

RepairItem repair_part(FileOps &ops, const AvLibs &av, const std::string &folder, const std::string &part)
{
    RepairItem it;
    it.part = part;
    const std::string path = folder + "/" + part;
    if (!path_exists(ops, path)) {
        it.damaged = true;
        it.error = "missing";
        return it;
    }
    const std::size_t dot = part.find_last_of('.');
    const std::string tmp = folder + "/." + (dot == std::string::npos ? part : part.substr(0, dot)) + ".repair.mkv";
    ops.unlink(tmp);
    const RemuxResult r = remux_mkv(av, path, tmp);
    /* plausible: at least one video packet and a duration between 0 and 48 h */
    if (!r.ok || r.duration_ms <= 0 || r.duration_ms > 48LL * 3600 * 1000) {
        ops.unlink(tmp);
        it.damaged = true;
        it.error = r.error.empty() ? "implausible duration" : r.error;
        return it;
    }
    int fd = ops.open(tmp, 0 /* O_RDONLY */, 0);
    if (fd >= 0) {
        ops.fsync(fd);
        ops.close(fd);
    }
    if (ops.rename(tmp, path) != 0) {
        ops.unlink(tmp);
        it.damaged = true;
        it.error = "rename failed";
        return it;
    }
    ops.fsync_dir(folder);
    it.repaired = true;
    it.duration_ms = r.duration_ms;
    return it;
}

std::vector<RepairItem> repair_open_journals(FileOps &ops, const AvLibs &av, const std::string &folder)
{
    std::vector<RepairItem> out;
    std::vector<std::string> names;
    if (ops.list_dir(folder + "/.journal", names) != 0) return out;
    for (const std::string &n : names) {
        if (n.size() < 6 || n.compare(n.size() - 5, 5, ".json") != 0) continue;
        const std::string stem = n.substr(0, n.size() - 5);
        Journal j;
        if (!journal_read(ops, folder + "/.journal/" + n, j) || j.state != "open") continue;
        bool any = false, all_bad = true;
        for (const std::string &p : j.parts) {
            RepairItem it = repair_part(ops, av, folder, p);
            it.stem = stem;
            std::fprintf(stderr, "[vhs] repair: %s: %s%s%s\n", p.c_str(), it.repaired ? "repaired" : "damaged",
                         it.error.empty() ? "" : ": ", it.error.c_str());
            any = any || it.repaired;
            all_bad = all_bad && !it.repaired;
            out.push_back(it);
        }
        j.state = (all_bad && !j.parts.empty()) ? "damaged" : "closed";
        j.repaired = any;
        journal_write(ops, folder, stem, j);
    }
    return out;
}

} // namespace vhs
