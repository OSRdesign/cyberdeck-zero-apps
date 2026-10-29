/*
 * SPDX-License-Identifier: MIT
 *
 * Klondike core: the small JSON reader and the string quoting the saved game and the statistics use (no LVGL;
 * tests/test_store.cpp).
 *
 *   parse()   one JSON value (object, array, string, number, true / false / null) with nothing but white space after
 *             it; at most 32 levels and 4 MB; never throws. Numbers keep their literal: as_int() reads integers exactly
 *             (64 bit), so a deal number or a bank survives the round trip. Strings are decoded (\" \\ \/ \b \f \n \r
 *             \t \uXXXX with surrogate pairs) into UTF-8.
 *   quote()   a JSON string literal of UTF-8 text (control characters escaped).
 * Writing is done by hand with quote() and std::to_string (the files are small and flat).
 */
#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace klondike {
namespace json {

struct Value {
    enum class Kind { Null, Bool, Number, String, Array, Object };
    Kind kind = Kind::Null;
    bool boolean = false;
    std::string text;                                        /* String: decoded; Number: the literal */
    std::vector<Value> items;                                /* Array */
    std::vector<std::pair<std::string, Value>> members;      /* Object, in file order */

    const Value *get(const std::string &key) const;          /* Object member or null */
    bool is_int() const;                                     /* a Number written as an integer that fits 64 bits */
    std::int64_t as_int(std::int64_t def = 0) const;
    bool as_bool(bool def = false) const;
    std::string as_string(const std::string &def = "") const;
};

bool parse(const std::string &text, Value &out, std::string *why = nullptr);
std::string quote(const std::string &s);

} // namespace json
} // namespace klondike
