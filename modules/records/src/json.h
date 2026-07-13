#pragma once

// ── Minimal JSON reader (internal to the records module) ──────────────────────
//
// Hand-written, no extra dependency — the schemas it serves (drafts §8.8,
// catalogs §8.7) are small and fixed, so a full JSON library would be a large
// dependency bought for very little. Strings carry UTF-8 through verbatim and
// decode \uXXXX escapes including surrogate pairs: a model writing Cyrillic
// commonly emits them.
//
// Not a public header: callers translate JsonError into their own error type.

#include <cstdint>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace records::json {

class JsonError : public std::runtime_error {
public:
    explicit JsonError(const std::string& msg) : std::runtime_error(msg) {}
};

struct Value;
using Array  = std::vector<Value>;
using Object = std::vector<std::pair<std::string, Value>>;

struct Value {
    enum class Type { Null, Bool, Number, String, Array, Object };

    Type        type    = Type::Null;
    bool        boolean = false;
    double      number  = 0;
    std::string string;
    Array       array;
    Object      object;

    bool is_object() const noexcept { return type == Type::Object; }
    bool is_array()  const noexcept { return type == Type::Array;  }
    bool is_string() const noexcept { return type == Type::String; }
    bool is_number() const noexcept { return type == Type::Number; }
};

// Member of an object, or nullptr when absent.
inline const Value* find(const Object& obj, const std::string& key) {
    for (const auto& [k, v] : obj)
        if (k == key) return &v;
    return nullptr;
}

class Reader {
public:
    explicit Reader(const std::string& text) : text_(text) {}

    // Throws JsonError.
    Value read() {
        skip_ws();
        Value v = read_value(0);
        skip_ws();
        if (pos_ != text_.size()) fail("посторонние символы после JSON");
        return v;
    }

private:
    static constexpr int MAX_DEPTH = 16;

    const std::string& text_;
    std::size_t        pos_ = 0;

    [[noreturn]] void fail(const std::string& msg) const {
        throw JsonError("байт " + std::to_string(pos_) + ": " + msg);
    }

    void skip_ws() {
        while (pos_ < text_.size()) {
            const char c = text_[pos_];
            if (c != ' ' && c != '\t' && c != '\n' && c != '\r') break;
            ++pos_;
        }
    }

    char peek() const { return pos_ < text_.size() ? text_[pos_] : '\0'; }

    void expect(char c) {
        if (peek() != c) fail(std::string("ожидался '") + c + "'");
        ++pos_;
    }

    bool literal(const char* word) {
        const std::size_t n = std::char_traits<char>::length(word);
        if (text_.compare(pos_, n, word) != 0) return false;
        pos_ += n;
        return true;
    }

    Value read_value(int depth) {
        if (depth > MAX_DEPTH) fail("слишком глубокая вложенность");
        switch (peek()) {
            case '{': return read_object(depth);
            case '[': return read_array(depth);
            case '"': {
                Value v;
                v.type   = Value::Type::String;
                v.string = read_string();
                return v;
            }
            case 't': {
                if (!literal("true")) fail("некорректный литерал");
                Value v;
                v.type    = Value::Type::Bool;
                v.boolean = true;
                return v;
            }
            case 'f': {
                if (!literal("false")) fail("некорректный литерал");
                Value v;
                v.type = Value::Type::Bool;
                return v;
            }
            case 'n': {
                if (!literal("null")) fail("некорректный литерал");
                return Value{};
            }
            default: return read_number();
        }
    }

    Value read_object(int depth) {
        expect('{');
        Value v;
        v.type = Value::Type::Object;
        skip_ws();
        if (peek() == '}') { ++pos_; return v; }
        for (;;) {
            skip_ws();
            std::string key = read_string();
            skip_ws();
            expect(':');
            skip_ws();
            v.object.emplace_back(std::move(key), read_value(depth + 1));
            skip_ws();
            if (peek() == ',') { ++pos_; continue; }
            expect('}');
            return v;
        }
    }

    Value read_array(int depth) {
        expect('[');
        Value v;
        v.type = Value::Type::Array;
        skip_ws();
        if (peek() == ']') { ++pos_; return v; }
        for (;;) {
            skip_ws();
            v.array.push_back(read_value(depth + 1));
            skip_ws();
            if (peek() == ',') { ++pos_; continue; }
            expect(']');
            return v;
        }
    }

    Value read_number() {
        const std::size_t start = pos_;
        if (peek() == '-' || peek() == '+') ++pos_;
        bool any = false;
        while (pos_ < text_.size()) {
            const char c = text_[pos_];
            const bool part = (c >= '0' && c <= '9') || c == '.' || c == 'e' ||
                              c == 'E' || c == '-' || c == '+';
            if (!part) break;
            any = true;
            ++pos_;
        }
        if (!any) fail("ожидалось значение");
        Value v;
        v.type = Value::Type::Number;
        try {
            v.number = std::stod(text_.substr(start, pos_ - start));
        } catch (const std::exception&) {
            fail("некорректное число");
        }
        return v;
    }

    static void append_utf8(std::string& out, uint32_t cp) {
        if (cp < 0x80) {
            out += static_cast<char>(cp);
        } else if (cp < 0x800) {
            out += static_cast<char>(0xC0 | (cp >> 6));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            out += static_cast<char>(0xE0 | (cp >> 12));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else {
            out += static_cast<char>(0xF0 | (cp >> 18));
            out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        }
    }

    uint32_t read_hex4() {
        if (pos_ + 4 > text_.size()) fail("оборванный \\u-эскейп");
        uint32_t cp = 0;
        for (int i = 0; i < 4; ++i) {
            const char c = text_[pos_++];
            int digit;
            if      (c >= '0' && c <= '9') digit = c - '0';
            else if (c >= 'a' && c <= 'f') digit = c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') digit = c - 'A' + 10;
            else fail("не-hex в \\u-эскейпе");
            cp = (cp << 4) | static_cast<uint32_t>(digit);
        }
        return cp;
    }

    std::string read_string() {
        expect('"');
        std::string out;
        for (;;) {
            if (pos_ >= text_.size()) fail("незакрытая строка");
            const char c = text_[pos_++];
            if (c == '"') return out;
            if (c != '\\') { out += c; continue; }   // UTF-8 passes through
            if (pos_ >= text_.size()) fail("оборванный эскейп");
            const char e = text_[pos_++];
            switch (e) {
                case '"':  out += '"';  break;
                case '\\': out += '\\'; break;
                case '/':  out += '/';  break;
                case 'b':  out += '\b'; break;
                case 'f':  out += '\f'; break;
                case 'n':  out += '\n'; break;
                case 'r':  out += '\r'; break;
                case 't':  out += '\t'; break;
                case 'u': {
                    uint32_t cp = read_hex4();
                    if (cp >= 0xD800 && cp <= 0xDBFF) {          // high surrogate
                        if (pos_ + 1 < text_.size() && text_[pos_] == '\\' &&
                            text_[pos_ + 1] == 'u') {
                            pos_ += 2;
                            const uint32_t lo = read_hex4();
                            if (lo >= 0xDC00 && lo <= 0xDFFF)
                                cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                            else
                                fail("некорректная суррогатная пара");
                        } else {
                            fail("одинокий высокий суррогат");
                        }
                    }
                    append_utf8(out, cp);
                    break;
                }
                default: fail("неизвестный эскейп");
            }
        }
    }
};

} // namespace records::json
