/*
 * SPDX-License-Identifier: MIT
 *
 * A small JSON value, parser and writer for the bridge's JSON lines (docs/dev/reticulum-bridge.md, section 3). No LVGL,
 * no exceptions. Written for this app from RFC 8259; bounded by the input and by a nesting limit.
 */
#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace ret {
namespace json {

class Value {
public:
    enum class Kind { Null, Bool, Number, String, Array, Object };

    Value() = default;
    static Value boolean(bool b);
    static Value number(double n);
    static Value string(std::string s);
    static Value array();
    static Value object();

    Kind kind() const { return kind_; }
    bool is_null() const { return kind_ == Kind::Null; }
    bool is_bool() const { return kind_ == Kind::Bool; }
    bool is_number() const { return kind_ == Kind::Number; }
    bool is_string() const { return kind_ == Kind::String; }
    bool is_array() const { return kind_ == Kind::Array; }
    bool is_object() const { return kind_ == Kind::Object; }

    /* Reads with defaults: another kind than asked gives the default. */
    bool as_bool(bool def = false) const { return kind_ == Kind::Bool ? bool_ : def; }
    double as_number(double def = 0) const { return kind_ == Kind::Number ? num_ : def; }
    std::int64_t as_int(std::int64_t def = 0) const { return kind_ == Kind::Number ? static_cast<std::int64_t>(num_) : def; }
    const std::string &as_string() const; /* "" when not a string */
    std::string as_string(const std::string &def) const { return kind_ == Kind::String ? str_ : def; }

    /* Object members and array items; a missing key or index gives the shared null value. */
    const Value &operator[](const std::string &key) const;
    const Value &operator[](std::size_t index) const;
    bool has(const std::string &key) const;
    std::size_t size() const;
    const std::vector<Value> &items() const { return arr_; }
    const std::vector<std::pair<std::string, Value>> &members() const { return obj_; }

    /* Builders (object: a repeated key is replaced). */
    Value &set(const std::string &key, Value v);
    Value &push(Value v);

private:
    Kind kind_ = Kind::Null;
    bool bool_ = false;
    double num_ = 0;
    std::string str_;
    std::vector<Value> arr_;
    std::vector<std::pair<std::string, Value>> obj_;
};

/* True with the value; false on any syntax error, trailing text or nesting over 32 levels. */
bool parse(const std::string &text, Value &out);
/* Compact JSON on one line (no newline); strings escaped, control characters as \u00XX; non-ASCII kept as UTF-8. */
std::string dump(const Value &v);
/* A number as the bridge and the config spell it: integers without a fraction, else shortest %.15g. */
std::string number_text(double n);

} // namespace json
} // namespace ret
