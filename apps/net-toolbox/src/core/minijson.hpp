/*
 * SPDX-License-Identifier: MIT
 *
 * Net Toolbox: a small JSON reader for curl's `-w '%{json}'` report (no LVGL; tests/test_httpcheck.cpp).
 *
 * One object of scalar members (strings, numbers, true / false / null); nested objects and arrays are checked and
 * skipped (16 levels at most). Strings are decoded (\n, \t, \", \\, \/, \uXXXX with surrogate pairs, into UTF-8).
 * A raw line break inside a string is dropped instead of failing: an output read line by line in pieces (async_run
 * cuts lines longer than its max_line) can still be read. Never throws; bounded by the input.
 */
#pragma once

#include <map>
#include <string>

namespace nt {
namespace json {

struct Value {
    enum class Kind { Null, Bool, Number, String, Nested };
    Kind kind = Kind::Null;
    std::string text;   /* String: the decoded text; Number: the literal; Bool: "true" / "false" */
    double number = 0;  /* Number */
};

using Object = std::map<std::string, Value>;

/* The object that starts at text[pos] ('{'). True with its members and *end = the byte after its '}'. */
bool parse_object(const std::string &text, std::size_t pos, Object &out, std::size_t *end = nullptr);

/* Helpers on a parsed object: the default when the member is missing or of another kind. */
std::string get_string(const Object &o, const std::string &key, const std::string &def = "");
double get_number(const Object &o, const std::string &key, double def = 0);
bool has(const Object &o, const std::string &key);

} // namespace json
} // namespace nt
