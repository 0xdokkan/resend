#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace netsim::json {

// A small, strict JSON value for resend's file formats. Numbers are whole
// (64-bit signed): the formats store times in nanoseconds and positions in
// canvas units, so no value depends on floating-point printing. Object
// members keep their order, which makes the output stable and diffable.
class Value {
public:
    using Array = std::vector<Value>;
    using Member = std::pair<std::string, Value>;
    using Object = std::vector<Member>;

    Value() = default;  // null
    Value(std::nullptr_t) {}
    Value(bool value) : data_(value) {}
    Value(int value) : data_(std::int64_t{value}) {}
    Value(std::int64_t value) : data_(value) {}
    Value(std::uint32_t value) : data_(std::int64_t{value}) {}
    Value(const char* value) : data_(std::string(value)) {}
    Value(std::string value) : data_(std::move(value)) {}
    Value(Array value) : data_(std::move(value)) {}
    Value(Object value) : data_(std::move(value)) {}
    static Value object() { return Value(Object{}); }
    static Value array() { return Value(Array{}); }

    bool is_null() const { return std::holds_alternative<std::nullptr_t>(data_); }
    const bool* boolean() const { return std::get_if<bool>(&data_); }
    const std::int64_t* integer() const { return std::get_if<std::int64_t>(&data_); }
    const std::string* string() const { return std::get_if<std::string>(&data_); }
    const Array* array_items() const { return std::get_if<Array>(&data_); }
    const Object* members() const { return std::get_if<Object>(&data_); }
    // The member named `key` of an object, or null if absent.
    const Value* find(std::string_view key) const;

    // Building: appends a member to an object, or an item to an array.
    Value& set(std::string key, Value value);
    Value& push(Value value);

    bool operator==(const Value&) const = default;

private:
    std::variant<std::nullptr_t, bool, std::int64_t, std::string, Array, Object> data_;
};

struct Error {
    std::size_t line = 0, column = 0;  // 1-based; column counts bytes
    std::string message;
};

// Rejects anything outside RFC 8259, plus fractions and exponents, duplicate
// keys, invalid UTF-8 and nesting deeper than 64 levels.
std::optional<Value> parse(std::string_view text, Error& error);
// Two-space indentation and a final newline.
std::string write(const Value& value);

} // namespace netsim::json
