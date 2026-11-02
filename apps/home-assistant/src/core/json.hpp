/*
 * SPDX-License-Identifier: MIT
 *
 * Home Assistant app core: the small JSON reader and the string quoting of the WebSocket messages and the files (no
 * LVGL; tests/test_json.cpp). Adapted from Mine Field's reader (same repository, MIT).
 *
 *   parse()   one JSON value (object, array, string, number, true / false / null) with nothing but white space after
 *             it; at most 32 levels and kMaxText bytes; never throws. Numbers keep their literal: as_int() reads
 *             integers exactly, as_double() any number. Strings are decoded (\" \\ \/ \b \f \n \r \t \uXXXX with
 *             surrogate pairs) into UTF-8. A DOM per message: the app never asks Home Assistant for everything at once
 *             (favourites and the room shown, registry lists without attributes), so a message stays small.
 *   quote()   a JSON string literal of UTF-8 text (control characters escaped).
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace ha {
namespace json {

constexpr std::size_t kMaxText = 4u * 1024 * 1024; /* the largest message or file the app parses */

struct Value {
    enum class Kind { Null, Bool, Number, String, Array, Object };
    Kind kind = Kind::Null;
    bool boolean = false;
    std::string text;                                   /* String: decoded; Number: the literal */
    std::vector<Value> items;                           /* Array */
    std::vector<std::pair<std::string, Value>> members; /* Object, in message order */

    const Value *get(const std::string &key) const; /* Object member or null */
    bool is_object() const { return kind == Kind::Object; }
    bool is_array() const { return kind == Kind::Array; }
    bool is_string() const { return kind == Kind::String; }
    bool is_number() const { return kind == Kind::Number; }
    bool is_null() const { return kind == Kind::Null; }
    bool is_int() const; /* a Number written as an integer that fits 64 bits */
    std::int64_t as_int(std::int64_t def = 0) const;
    double as_double(double def = 0) const; /* any Number */
    bool as_bool(bool def = false) const;
    std::string as_string(const std::string &def = "") const;
};

bool parse(const std::string &text, Value &out, std::string *why = nullptr);
std::string quote(const std::string &s);
/* A number as JSON text: integers without a fraction, others with up to 6 significant decimals. */
std::string number(double v);

} // namespace json
} // namespace ha
