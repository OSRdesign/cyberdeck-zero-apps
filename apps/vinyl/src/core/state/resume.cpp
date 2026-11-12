/*
 * SPDX-License-Identifier: MIT
 *
 * Vinyl state (see resume.hpp).
 */
#include "resume.hpp"

#include "../util/atomic_file.hpp"
#include "../util/json.hpp"

#include <algorithm>
#include <cstdlib>

namespace vinyl {

std::string resume_path() {
    const char *xdg = std::getenv("XDG_CONFIG_HOME");
    std::string base;
    if (xdg && *xdg) base = xdg;
    else {
        const char *home = std::getenv("HOME");
        base = std::string(home ? home : ".") + "/.config";
    }
    return base + "/vinyl/state.json";
}

std::string resume_to_json(const ResumeState &s) {
    json::Value root = json::Value::object();
    root.set("v", json::Value::number(1));
    json::Value q = json::Value::object();
    json::Value items;
    items.type = json::Value::Arr;
    const size_t n = std::min(s.items.size(), kMaxResumeItems);
    for (size_t i = 0; i < n; ++i) items.arr.push_back(json::Value::string(s.items[i]));
    json::Value order;
    order.type = json::Value::Arr;
    if (s.order.size() == s.items.size() && s.items.size() <= kMaxResumeItems)
        for (int o : s.order) order.arr.push_back(json::Value::number(o));
    q.set("items", items);
    q.set("order", order);
    q.set("current", json::Value::number(s.current));
    q.set("shuffle", json::Value::boolean(s.shuffle));
    q.set("repeat", json::Value::string(repeat_name(s.repeat)));
    q.set("source", json::Value::string(s.source));
    root.set("queue", q);
    root.set("position_ms", json::Value::number(static_cast<double>(s.position_ms)));
    root.set("output", json::Value::string(s.output));
    root.set("browse", json::Value::string(s.browse));
    root.set("app_volume", json::Value::number(s.app_volume));
    return json::write(root);
}

bool resume_from_json(const std::string &text, ResumeState &out) {
    out = ResumeState();
    json::Value root;
    if (!json::parse(text, root) || !root.is_obj()) return false;
    const json::Value *v = root.get("v");
    if (!v || v->type != json::Value::Num || v->n != 1) return false;
    if (const json::Value *q = root.get("queue"); q && q->is_obj()) {
        if (const json::Value *it = q->get("items"); it && it->type == json::Value::Arr)
            for (const json::Value &e : it->arr)
                if (e.type == json::Value::Str && out.items.size() < kMaxResumeItems) out.items.push_back(e.s);
        if (const json::Value *o = q->get("order"); o && o->type == json::Value::Arr)
            for (const json::Value &e : o->arr)
                if (e.type == json::Value::Num) out.order.push_back(static_cast<int>(e.n));
        if (const json::Value *c = q->get("current"); c && c->type == json::Value::Num) out.current = static_cast<int>(c->n);
        if (const json::Value *sh = q->get("shuffle"); sh && sh->type == json::Value::Bool) out.shuffle = sh->b;
        if (const json::Value *r = q->get("repeat"); r && r->type == json::Value::Str) out.repeat = repeat_from(r->s);
        if (const json::Value *s = q->get("source"); s && s->type == json::Value::Str) out.source = s->s;
    }
    if (const json::Value *p = root.get("position_ms"); p && p->type == json::Value::Num && p->n >= 0)
        out.position_ms = static_cast<uint64_t>(p->n);
    if (const json::Value *o = root.get("output"); o && o->type == json::Value::Str && !o->s.empty()) out.output = o->s;
    if (const json::Value *b = root.get("browse"); b && b->type == json::Value::Str) out.browse = b->s;
    if (const json::Value *a = root.get("app_volume"); a && a->type == json::Value::Num)
        out.app_volume = std::max(0, std::min(100, static_cast<int>(a->n)));
    if (out.current >= static_cast<int>(out.items.size())) out.current = -1;
    return true;
}

int save_resume(const std::string &path, const ResumeState &s) {
    FileOps &ops = posix_file_ops();
    const int r = make_dirs(ops, dir_name(path));
    if (r < 0) return r;
    return atomic_write_file(ops, path, resume_to_json(s));
}

bool load_resume(const std::string &path, ResumeState &out) {
    std::string text;
    if (read_small_file(posix_file_ops(), path, text, 4u << 20) != 0) {
        out = ResumeState();
        return false;
    }
    return resume_from_json(text, out);
}

void resume_from_queue(const Queue &q, ResumeState &s) {
    s.items = q.items();
    s.order = q.order();
    s.current = q.current();
    s.shuffle = q.shuffle();
    s.repeat = q.repeat();
    s.source = q.source();
}

void queue_from_resume(const ResumeState &s, Queue &q) {
    q.restore(s.items, s.order, s.current, s.shuffle, s.repeat, s.source);
}

}  // namespace vinyl
