#include "records/draft.h"

#include <algorithm>
#include <utility>

namespace records {

namespace {

// ── Minimal JSON reader (hand-written — no extra dependency) ──────────────────
//
// The draft schema is small and fixed, so a full JSON library would be a large
// dependency bought for very little. Strings carry UTF-8 through verbatim and
// decode \uXXXX escapes (including surrogate pairs) — an AI writing Cyrillic
// commonly emits them.

struct JsonValue;
using JsonArray  = std::vector<JsonValue>;
using JsonObject = std::vector<std::pair<std::string, JsonValue>>;

struct JsonValue {
    enum class Type { Null, Bool, Number, String, Array, Object };

    Type        type = Type::Null;
    bool        boolean = false;
    double      number  = 0;
    std::string string;
    JsonArray   array;
    JsonObject  object;
};

class JsonReader {
public:
    explicit JsonReader(const std::string& text) : text_(text) {}

    JsonValue read() {
        skip_ws();
        JsonValue v = read_value(0);
        skip_ws();
        if (pos_ != text_.size()) fail("посторонние символы после JSON");
        return v;
    }

private:
    static constexpr int MAX_DEPTH = 16;

    const std::string& text_;
    std::size_t        pos_ = 0;

    [[noreturn]] void fail(const std::string& msg) const {
        throw DraftError("черновик, байт " + std::to_string(pos_) + ": " + msg);
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

    JsonValue read_value(int depth) {
        if (depth > MAX_DEPTH) fail("слишком глубокая вложенность");
        switch (peek()) {
            case '{': return read_object(depth);
            case '[': return read_array(depth);
            case '"': {
                JsonValue v;
                v.type   = JsonValue::Type::String;
                v.string = read_string();
                return v;
            }
            case 't': {
                if (!literal("true")) fail("некорректный литерал");
                JsonValue v;
                v.type = JsonValue::Type::Bool;
                v.boolean = true;
                return v;
            }
            case 'f': {
                if (!literal("false")) fail("некорректный литерал");
                JsonValue v;
                v.type = JsonValue::Type::Bool;
                return v;
            }
            case 'n': {
                if (!literal("null")) fail("некорректный литерал");
                return JsonValue{};
            }
            default: return read_number();
        }
    }

    JsonValue read_object(int depth) {
        expect('{');
        JsonValue v;
        v.type = JsonValue::Type::Object;
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

    JsonValue read_array(int depth) {
        expect('[');
        JsonValue v;
        v.type = JsonValue::Type::Array;
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

    JsonValue read_number() {
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
        JsonValue v;
        v.type = JsonValue::Type::Number;
        try {
            v.number = std::stod(text_.substr(start, pos_ - start));
        } catch (const std::exception&) {
            fail("некорректное число");
        }
        return v;
    }

    // Appends one Unicode code point as UTF-8.
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

// ── Schema helpers ────────────────────────────────────────────────────────────

const JsonValue* find(const JsonObject& obj, const std::string& key) {
    for (const auto& [k, v] : obj)
        if (k == key) return &v;
    return nullptr;
}

std::string require_string(const JsonObject& obj, const std::string& key,
                           const std::string& where) {
    const JsonValue* v = find(obj, key);
    if (!v) throw DraftError(where + ": нет обязательного поля \"" + key + "\"");
    if (v->type != JsonValue::Type::String)
        throw DraftError(where + ": поле \"" + key + "\" должно быть строкой");
    if (v->string.empty())
        throw DraftError(where + ": поле \"" + key + "\" пустое");
    return v->string;
}

std::vector<std::string> string_array(const JsonObject& obj, const std::string& key,
                                      const std::string& where, bool required) {
    const JsonValue* v = find(obj, key);
    if (!v) {
        if (required)
            throw DraftError(where + ": нет обязательного поля \"" + key + "\"");
        return {};
    }
    if (v->type != JsonValue::Type::Array)
        throw DraftError(where + ": поле \"" + key + "\" должно быть массивом");
    std::vector<std::string> out;
    for (const auto& item : v->array) {
        if (item.type != JsonValue::Type::String)
            throw DraftError(where + ": в \"" + key + "\" ожидались строки");
        out.push_back(item.string);
    }
    if (required && out.empty())
        throw DraftError(where + ": массив \"" + key + "\" пуст");
    return out;
}

std::array<uint8_t, 32> parse_hex32(const std::string& hex, const std::string& where) {
    if (hex.size() != 64)
        throw DraftError(where + ": ожидалось 64 hex-символа, а не " +
                         std::to_string(hex.size()));
    std::array<uint8_t, 32> out{};
    for (std::size_t i = 0; i < 32; ++i) {
        auto nibble = [&](char c) -> int {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
            throw DraftError(where + ": не-hex символ в ссылке");
        };
        out[i] = static_cast<uint8_t>((nibble(hex[2 * i]) << 4) | nibble(hex[2 * i + 1]));
    }
    return out;
}

// Ref is written "<chain_64hex>/<block_hash_64hex>" (records.md §4).
Ref parse_ref(const std::string& text, const std::string& where) {
    const auto slash = text.find('/');
    if (slash == std::string::npos)
        throw DraftError(where + ": ссылка должна иметь вид <цепь_64hex>/<хеш_64hex>");
    Ref r{};
    r.chain = parse_hex32(text.substr(0, slash), where);
    r.hash  = parse_hex32(text.substr(slash + 1), where);
    return r;
}

// The whitelist (ИР-002). Naming the refused type explicitly beats a generic
// "unknown type": the person is told why, not just that.
[[noreturn]] void refuse_type(const std::string& t, const std::string& where) {
    static const char* VALUE_BEARING[] = {
        "transfer", "pledge", "pledge-revoke", "redemption", "acceptance",
        "work", "work-record", "specialty", "grade", "worker", "fraud-claim",
    };
    for (const char* v : VALUE_BEARING) {
        if (t == v)
            throw DraftError(
                where + ": тип \"" + t + "\" в черновике запрещён. Ценностные и "
                "необратимые записи подписываются по одной, осознанно, и никогда "
                "пакетом (ИР-002) — чтобы перевод не мог спрятаться среди фактов "
                "профиля");
    }
    throw DraftError(where + ": неизвестный тип \"" + t +
                     "\". Разрешены: concept, concept-link, composite");
}

Record record_from(const JsonObject& obj, const std::string& where) {
    const std::string t = require_string(obj, "t", where);

    if (t == "concept") {
        Concept c;
        c.text = require_string(obj, "text", where);
        c.tags = string_array(obj, "tags", where, /*required=*/false);
        return c;
    }
    if (t == "concept-link") {
        ConceptLink cl;
        cl.from = parse_ref(require_string(obj, "from", where), where + ", from");
        cl.to   = parse_ref(require_string(obj, "to",   where), where + ", to");
        cl.kind = require_string(obj, "kind", where);
        return cl;
    }
    if (t == "composite") {
        Composite comp;
        comp.title = require_string(obj, "title", where);
        for (const auto& part : string_array(obj, "parts", where, /*required=*/true))
            comp.parts.push_back(parse_ref(part, where + ", parts"));
        return comp;
    }
    refuse_type(t, where);
}

std::string to_hex(const std::array<uint8_t, 32>& bytes, std::size_t nibbles) {
    static const char* DIGITS = "0123456789abcdef";
    std::string out;
    for (std::size_t i = 0; i < nibbles / 2 && i < bytes.size(); ++i) {
        out += DIGITS[bytes[i] >> 4];
        out += DIGITS[bytes[i] & 0x0F];
    }
    return out;
}

std::string short_ref(const Ref& r) {
    return to_hex(r.chain, 8) + "…/" + to_hex(r.hash, 8) + "…";
}

} // namespace

// ── Public API ────────────────────────────────────────────────────────────────

Draft parse_draft(const std::string& json) {
    JsonReader      reader(json);
    const JsonValue root = reader.read();
    if (root.type != JsonValue::Type::Object)
        throw DraftError("черновик: на верхнем уровне ожидался объект JSON");

    Draft draft;

    if (const JsonValue* leaf = find(root.object, "leaf")) {
        if (leaf->type != JsonValue::Type::Number)
            throw DraftError("черновик: \"leaf\" должен быть числом");
        if (leaf->number < 0 || leaf->number > 4294967294.0)
            throw DraftError("черновик: \"leaf\" вне диапазона 0..0xFFFFFFFE");
        draft.leaf = static_cast<uint32_t>(leaf->number);
    }

    const JsonValue* recs = find(root.object, "records");
    if (!recs || recs->type != JsonValue::Type::Array)
        throw DraftError("черновик: нужен массив \"records\"");
    if (recs->array.empty())
        throw DraftError("черновик: \"records\" пуст — нечего подписывать");
    if (recs->array.size() > MAX_DRAFT_RECORDS)
        throw DraftError("черновик: записей " + std::to_string(recs->array.size()) +
                         ", максимум " + std::to_string(MAX_DRAFT_RECORDS) +
                         " — пакет должен оставаться читаемым целиком");

    for (std::size_t i = 0; i < recs->array.size(); ++i) {
        const JsonValue& item = recs->array[i];
        const std::string where = "запись #" + std::to_string(i + 1);
        if (item.type != JsonValue::Type::Object)
            throw DraftError(where + ": ожидался объект");
        draft.records.push_back(record_from(item.object, where));
    }
    return draft;
}

std::string render_record(const Record& rec) {
    if (const auto* c = std::get_if<Concept>(&rec)) {
        std::string out = "Concept  \"" + c->text + "\"";
        if (!c->tags.empty()) {
            out += "\n         теги:";
            for (const auto& tag : c->tags) out += " " + tag;
        }
        return out;
    }
    if (const auto* cl = std::get_if<ConceptLink>(&rec)) {
        return "ConceptLink  \"" + cl->kind + "\"\n         "
             + short_ref(cl->from) + "  →  " + short_ref(cl->to);
    }
    if (const auto* comp = std::get_if<Composite>(&rec)) {
        std::string out = "Composite  \"" + comp->title + "\"";
        for (const auto& part : comp->parts) out += "\n         часть: " + short_ref(part);
        return out;
    }
    return "<запись типа, недопустимого в черновике>";
}

} // namespace records
