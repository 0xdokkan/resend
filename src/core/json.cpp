#include "core/json.hpp"

#include "core/text.hpp"

#include <algorithm>
#include <limits>

namespace netsim::json {
using text::tr;

const Value* Value::find(std::string_view key) const {
    const auto* object = members();
    if (!object) return nullptr;
    const auto it = std::find_if(object->begin(), object->end(), [&](const Member& m) { return m.first == key; });
    return it == object->end() ? nullptr : &it->second;
}

Value& Value::set(std::string key, Value value) {
    if (!std::holds_alternative<Object>(data_)) data_ = Object{};
    auto& object = std::get<Object>(data_);
    object.emplace_back(std::move(key), std::move(value));
    return object.back().second;
}

Value& Value::push(Value value) {
    if (!std::holds_alternative<Array>(data_)) data_ = Array{};
    auto& array = std::get<Array>(data_);
    array.push_back(std::move(value));
    return array.back();
}

namespace {
constexpr int max_depth = 64;

void append_utf8(std::string& out, std::uint32_t code) {
    if (code < 0x80) {
        out += static_cast<char>(code);
    } else if (code < 0x800) {
        out += static_cast<char>(0xc0 | (code >> 6));
        out += static_cast<char>(0x80 | (code & 0x3f));
    } else if (code < 0x10000) {
        out += static_cast<char>(0xe0 | (code >> 12));
        out += static_cast<char>(0x80 | ((code >> 6) & 0x3f));
        out += static_cast<char>(0x80 | (code & 0x3f));
    } else {
        out += static_cast<char>(0xf0 | (code >> 18));
        out += static_cast<char>(0x80 | ((code >> 12) & 0x3f));
        out += static_cast<char>(0x80 | ((code >> 6) & 0x3f));
        out += static_cast<char>(0x80 | (code & 0x3f));
    }
}

class Parser {
public:
    Parser(std::string_view text, Error& error) : text_(text), error_(error) {}

    std::optional<Value> document() {
        skip_space();
        auto value = parse_value(0);
        if (!value) return std::nullopt;
        skip_space();
        if (pos_ != text_.size()) return fail(tr("unexpected text after the end of the document"));
        return value;
    }

private:
    std::nullopt_t fail(std::string message) {
        if (error_.message.empty()) {
            error_.line = 1;
            error_.column = 1;
            for (std::size_t i = 0; i < pos_ && i < text_.size(); ++i) {
                if (text_[i] == '\n') {
                    ++error_.line;
                    error_.column = 1;
                } else {
                    ++error_.column;
                }
            }
            error_.message = std::move(message);
        }
        return std::nullopt;
    }
    bool at_end() const { return pos_ >= text_.size(); }
    char peek() const { return at_end() ? '\0' : text_[pos_]; }
    void skip_space() {
        while (!at_end() && (peek() == ' ' || peek() == '\t' || peek() == '\n' || peek() == '\r')) ++pos_;
    }
    bool literal(std::string_view word) {
        if (text_.substr(pos_, word.size()) != word) return false;
        pos_ += word.size();
        return true;
    }

    std::optional<Value> parse_value(int depth) {
        if (depth >= max_depth) return fail(tr("nesting is too deep"));
        switch (peek()) {
        case '{': return parse_object(depth);
        case '[': return parse_array(depth);
        case '"': {
            auto text = parse_string();
            if (!text) return std::nullopt;
            return Value(std::move(*text));
        }
        case 't': if (literal("true")) return Value(true); break;
        case 'f': if (literal("false")) return Value(false); break;
        case 'n': if (literal("null")) return Value(); break;
        default:
            if (peek() == '-' || (peek() >= '0' && peek() <= '9')) return parse_number();
            break;
        }
        return fail(at_end() ? tr("unexpected end of the document") : tr("expected a value"));
    }

    std::optional<Value> parse_object(int depth) {
        ++pos_;  // {
        Value result = Value::object();
        skip_space();
        if (peek() == '}') {
            ++pos_;
            return result;
        }
        while (true) {
            skip_space();
            if (peek() != '"') return fail(tr("expected a member name in double quotes"));
            auto key = parse_string();
            if (!key) return std::nullopt;
            if (result.find(*key)) return fail(tr("duplicate member \"{0}\"", {*key}));
            skip_space();
            if (peek() != ':') return fail(tr("expected ':' after the member name"));
            ++pos_;
            skip_space();
            auto value = parse_value(depth + 1);
            if (!value) return std::nullopt;
            result.set(std::move(*key), std::move(*value));
            skip_space();
            if (peek() == ',') { ++pos_; continue; }
            if (peek() == '}') { ++pos_; return result; }
            return fail(tr("expected ',' or '}'"));
        }
    }

    std::optional<Value> parse_array(int depth) {
        ++pos_;  // [
        Value result = Value::array();
        skip_space();
        if (peek() == ']') {
            ++pos_;
            return result;
        }
        while (true) {
            skip_space();
            auto value = parse_value(depth + 1);
            if (!value) return std::nullopt;
            result.push(std::move(*value));
            skip_space();
            if (peek() == ',') { ++pos_; continue; }
            if (peek() == ']') { ++pos_; return result; }
            return fail(tr("expected ',' or ']'"));
        }
    }

    std::optional<Value> parse_number() {
        const std::size_t start = pos_;
        const bool negative = peek() == '-';
        if (negative) ++pos_;
        if (peek() == '0' && pos_ + 1 < text_.size() && text_[pos_ + 1] >= '0' && text_[pos_ + 1] <= '9')
            return fail(tr("numbers cannot have leading zeros"));
        if (!(peek() >= '0' && peek() <= '9')) return fail(tr("expected a digit"));
        // Accumulate as a negative number, which has the larger range.
        std::int64_t value = 0;
        constexpr auto lowest = std::numeric_limits<std::int64_t>::min();
        while (peek() >= '0' && peek() <= '9') {
            const int digit = peek() - '0';
            if (value < (lowest + digit) / 10) {
                pos_ = start;
                return fail(tr("number out of range"));
            }
            value = value * 10 - digit;
            ++pos_;
        }
        if (peek() == '.' || peek() == 'e' || peek() == 'E') return fail(tr("only whole numbers are allowed"));
        if (!negative) {
            if (value == lowest) {
                pos_ = start;
                return fail(tr("number out of range"));
            }
            value = -value;
        }
        return Value(value);
    }

    std::optional<std::uint32_t> hex4() {
        if (pos_ + 4 > text_.size()) return std::nullopt;
        std::uint32_t code = 0;
        for (int i = 0; i < 4; ++i) {
            const char c = text_[pos_++];
            code <<= 4;
            if (c >= '0' && c <= '9') code |= static_cast<std::uint32_t>(c - '0');
            else if (c >= 'a' && c <= 'f') code |= static_cast<std::uint32_t>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') code |= static_cast<std::uint32_t>(c - 'A' + 10);
            else return std::nullopt;
        }
        return code;
    }

    // Copies one UTF-8 sequence, rejecting overlong forms and surrogates.
    bool copy_utf8(std::string& out) {
        const auto lead = static_cast<unsigned char>(text_[pos_]);
        int extra = 0;
        std::uint32_t code = 0, minimum = 0;
        if (lead >= 0xc2 && lead <= 0xdf) { extra = 1; code = lead & 0x1fU; minimum = 0x80; }
        else if (lead >= 0xe0 && lead <= 0xef) { extra = 2; code = lead & 0x0fU; minimum = 0x800; }
        else if (lead >= 0xf0 && lead <= 0xf4) { extra = 3; code = lead & 0x07U; minimum = 0x10000; }
        else return false;
        if (pos_ + static_cast<std::size_t>(extra) >= text_.size()) return false;
        for (int i = 1; i <= extra; ++i) {
            const auto next = static_cast<unsigned char>(text_[pos_ + static_cast<std::size_t>(i)]);
            if ((next & 0xc0) != 0x80) return false;
            code = (code << 6) | (next & 0x3fU);
        }
        if (code < minimum || code > 0x10ffff || (code >= 0xd800 && code <= 0xdfff)) return false;
        out.append(text_.substr(pos_, static_cast<std::size_t>(extra) + 1));
        pos_ += static_cast<std::size_t>(extra) + 1;
        return true;
    }

    std::optional<std::string> parse_string() {
        ++pos_;  // "
        std::string out;
        while (true) {
            if (at_end()) return fail(tr("unterminated string"));
            const auto c = static_cast<unsigned char>(peek());
            if (c == '"') {
                ++pos_;
                return out;
            }
            if (c < 0x20) return fail(tr("control characters must be escaped in strings"));
            if (c >= 0x80) {
                if (!copy_utf8(out)) return fail(tr("invalid UTF-8 in string"));
                continue;
            }
            if (c != '\\') {
                out += static_cast<char>(c);
                ++pos_;
                continue;
            }
            ++pos_;
            const char escape = peek();
            ++pos_;
            switch (escape) {
            case '"': out += '"'; break;
            case '\\': out += '\\'; break;
            case '/': out += '/'; break;
            case 'b': out += '\b'; break;
            case 'f': out += '\f'; break;
            case 'n': out += '\n'; break;
            case 'r': out += '\r'; break;
            case 't': out += '\t'; break;
            case 'u': {
                auto code = hex4();
                if (!code) return fail(tr("invalid \\u escape"));
                if (*code >= 0xdc00 && *code <= 0xdfff) return fail(tr("unpaired surrogate in \\u escape"));
                if (*code >= 0xd800 && *code <= 0xdbff) {
                    if (!literal("\\u")) return fail(tr("unpaired surrogate in \\u escape"));
                    const auto low = hex4();
                    if (!low || *low < 0xdc00 || *low > 0xdfff) return fail(tr("unpaired surrogate in \\u escape"));
                    code = 0x10000 + ((*code - 0xd800) << 10) + (*low - 0xdc00);
                }
                append_utf8(out, *code);
                break;
            }
            default:
                --pos_;
                return fail(tr("invalid escape in string"));
            }
        }
    }

    std::string_view text_;
    Error& error_;
    std::size_t pos_ = 0;
};

void write_string(std::string& out, const std::string& text) {
    constexpr char hex[] = "0123456789abcdef";
    out += '"';
    for (const char c : text) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        case '\b': out += "\\b"; break;
        case '\f': out += "\\f"; break;
        default:
            if (static_cast<unsigned char>(c) < 0x20) {
                out += "\\u00";
                out += hex[(c >> 4) & 0xf];
                out += hex[c & 0xf];
            } else {
                out += c;
            }
        }
    }
    out += '"';
}

void write_value(std::string& out, const Value& value, int indent) {
    const auto newline = [&](int level) {
        out += '\n';
        out.append(static_cast<std::size_t>(level) * 2, ' ');
    };
    if (value.is_null()) out += "null";
    else if (const auto* flag = value.boolean()) out += *flag ? "true" : "false";
    else if (const auto* number = value.integer()) out += std::to_string(*number);
    else if (const auto* text = value.string()) write_string(out, *text);
    else if (const auto* items = value.array_items()) {
        if (items->empty()) {
            out += "[]";
            return;
        }
        out += '[';
        for (std::size_t i = 0; i < items->size(); ++i) {
            if (i) out += ',';
            newline(indent + 1);
            write_value(out, (*items)[i], indent + 1);
        }
        newline(indent);
        out += ']';
    } else if (const auto* members = value.members()) {
        if (members->empty()) {
            out += "{}";
            return;
        }
        out += '{';
        for (std::size_t i = 0; i < members->size(); ++i) {
            if (i) out += ',';
            newline(indent + 1);
            write_string(out, (*members)[i].first);
            out += ": ";
            write_value(out, (*members)[i].second, indent + 1);
        }
        newline(indent);
        out += '}';
    }
}
} // namespace

std::optional<Value> parse(std::string_view text, Error& error) {
    error = {};
    return Parser(text, error).document();
}

std::string write(const Value& value) {
    std::string out;
    write_value(out, value, 0);
    out += '\n';
    return out;
}

} // namespace netsim::json
