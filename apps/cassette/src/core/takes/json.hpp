/*
 * SPDX-License-Identifier: MIT
 *
 * Cassette takes: a minimal JSON value, reader and writer for the settings file (journal.cpp keeps its own private
 * reader). Objects keep their key order so unknown keys survive a read-modify-write. Numbers are doubles. Strings are
 * UTF-8; \uXXXX escapes (with surrogate pairs) are decoded. Depth is limited. LVGL-free.
 */
#pragma once

#include <string>
#include <utility>
#include <vector>

namespace cassette {
namespace json {

struct Value {
    enum Type { Null, Bool, Num, Str, Arr, Obj } type = Null;
    bool b = false;
    double n = 0;
    std::string s;
    std::vector<Value> arr;
    std::vector<std::pair<std::string, Value>> obj; /* in file order; a later duplicate key replaces the earlier */

    static Value boolean(bool v);
    static Value number(double v);
    static Value string(const std::string &v);
    static Value object();

    bool is_obj() const { return type == Obj; }
    const Value *get(const std::string &key) const; /* nullptr when absent or not an object */
    Value *get(const std::string &key);
    /* Sets (or appends) key in an object; turns a non-object into an empty object first. */
    Value &set(const std::string &key, Value v);
    bool erase(const std::string &key);
};

/* Parses one JSON document (whitespace around it allowed). False on any syntax error; out is then Null. */
bool parse(const std::string &text, Value &out);
/* Pretty-printed with two-space indent and a final newline. Non-finite numbers are written as null. */
std::string write(const Value &v);

}  // namespace json
}  // namespace cassette
