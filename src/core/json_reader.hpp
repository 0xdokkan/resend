#pragma once

#include "core/json.hpp"
#include "core/text.hpp"

#include <cstdint>
#include <initializer_list>
#include <string>
#include <string_view>

namespace netsim::json {

// Reads members with the path of each one, so errors say where they are.
class Reader {
public:
    std::string error;

    bool fail(const std::string& path, const std::string& message) {
        if (error.empty()) error = path + ": " + message;
        return false;
    }
    // Every member of `object` must be one of `known`.
    bool only(const Value& object, const std::string& path, std::initializer_list<std::string_view> known) {
        for (const auto& [key, value] : *object.members()) {
            bool listed = false;
            for (const auto name : known) listed = listed || name == key;
            if (!listed) return fail(path, text::tr("unknown member \"{0}\"", {key}));
        }
        return true;
    }
    const Value* object(const Value& value, const std::string& path) {
        if (!value.members()) {
            fail(path, text::tr("expected an object"));
            return nullptr;
        }
        return &value;
    }
    const Value* member(const Value& object, std::string_view key, const std::string& path) {
        const auto* value = object.find(key);
        if (!value) fail(path, text::tr("missing member \"{0}\"", {key}));
        return value;
    }
    bool text(const Value& object, std::string_view key, const std::string& path, std::string& out, bool required = true) {
        const auto* value = object.find(key);
        if (!value) return !required || fail(path, text::tr("missing member \"{0}\"", {key}));
        if (!value->string()) return fail(path + "." + std::string(key), text::tr("expected a string"));
        out = *value->string();
        return true;
    }
    bool flag(const Value& object, std::string_view key, const std::string& path, bool& out) {
        const auto* value = object.find(key);
        if (!value) return true;  // optional, keeps the default
        if (!value->boolean()) return fail(path + "." + std::string(key), text::tr("expected true or false"));
        out = *value->boolean();
        return true;
    }
    bool number(const Value& object, std::string_view key, const std::string& path, std::int64_t low, std::int64_t high,
                std::int64_t& out, bool required = true) {
        const auto* value = object.find(key);
        if (!value) return !required || fail(path, text::tr("missing member \"{0}\"", {key}));
        if (!value->integer() || *value->integer() < low || *value->integer() > high)
            return fail(path + "." + std::string(key), text::tr("expected a whole number from {0} to {1}", {std::to_string(low), std::to_string(high)}));
        out = *value->integer();
        return true;
    }
};

} // namespace netsim::json
