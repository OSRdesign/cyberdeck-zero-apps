/*
 * SPDX-License-Identifier: MIT
 *
 * RSS Reader core: a minimal JSON value, reader and writer (no dependency) used by the catalog and the store.
 *
 * The reader is tolerant: a UTF-8 BOM, comments and trailing commas are accepted; it refuses
 * (returns false) on anything it cannot make sense of, nesting deeper than 64 levels and documents over 32 MB. Objects
 * keep their member order. Numbers are doubles (exact for the integers the app stores: epochs, byte counts).
 */
#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace rssreader {

struct Json {
    enum class Type { Null, Bool, Number, String, Array, Object };
    Type type = Type::Null;
    bool b = false;
    double num = 0;
    std::string str;
    std::vector<Json> arr;
    std::vector<std::pair<std::string, Json>> obj;

    static Json null() { return Json(); }
    static Json boolean(bool v) { Json j; j.type = Type::Bool; j.b = v; return j; }
    static Json number(double v) { Json j; j.type = Type::Number; j.num = v; return j; }
    static Json string(std::string v) { Json j; j.type = Type::String; j.str = std::move(v); return j; }
    static Json array() { Json j; j.type = Type::Array; return j; }
    static Json object() { Json j; j.type = Type::Object; return j; }

    bool is_object() const { return type == Type::Object; }
    bool is_array() const { return type == Type::Array; }
    bool is_string() const { return type == Type::String; }
    bool is_number() const { return type == Type::Number; }

    /* The member `key` of an object (the first one), or nullptr. */
    const Json *get(const std::string &key) const;
    /* Typed reads with a default for a missing or mistyped member. */
    std::string get_string(const std::string &key, const std::string &def = "") const;
    std::int64_t get_int(const std::string &key, std::int64_t def = 0) const;
    double get_double(const std::string &key, double def = 0) const;
    bool get_bool(const std::string &key, bool def = false) const;

    /* Builders: set replaces an existing member, push appends. */
    Json &set(const std::string &key, Json value);
    Json &push(Json value);
};

bool json_parse(const std::string &text, Json &out);
std::string json_dump(const Json &value, bool pretty = false);
std::string json_quote(const std::string &s);

} // namespace rssreader
