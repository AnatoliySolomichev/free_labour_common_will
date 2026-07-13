#include "records/draft.h"

#include "json.h"

#include <algorithm>
#include <array>

namespace records {

namespace {

using json::Object;
using json::Value;

std::string require_string(const Object& obj, const std::string& key,
                           const std::string& where) {
    const Value* v = json::find(obj, key);
    if (!v) throw DraftError(where + ": нет обязательного поля \"" + key + "\"");
    if (!v->is_string())
        throw DraftError(where + ": поле \"" + key + "\" должно быть строкой");
    if (v->string.empty())
        throw DraftError(where + ": поле \"" + key + "\" пустое");
    return v->string;
}

std::vector<std::string> string_array(const Object& obj, const std::string& key,
                                      const std::string& where, bool required) {
    const Value* v = json::find(obj, key);
    if (!v) {
        if (required)
            throw DraftError(where + ": нет обязательного поля \"" + key + "\"");
        return {};
    }
    if (!v->is_array())
        throw DraftError(where + ": поле \"" + key + "\" должно быть массивом");
    std::vector<std::string> out;
    for (const auto& item : v->array) {
        if (!item.is_string())
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
// "unknown type": the person is told why, not merely that.
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

Record record_from(const Object& obj, const std::string& where) {
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

Draft parse_draft(const std::string& text) {
    Value root;
    try {
        root = json::Reader(text).read();
    } catch (const json::JsonError& e) {
        throw DraftError(std::string("черновик, ") + e.what());
    }
    if (!root.is_object())
        throw DraftError("черновик: на верхнем уровне ожидался объект JSON");

    Draft draft;

    if (const Value* leaf = json::find(root.object, "leaf")) {
        if (!leaf->is_number())
            throw DraftError("черновик: \"leaf\" должен быть числом");
        if (leaf->number < 0 || leaf->number > 4294967294.0)
            throw DraftError("черновик: \"leaf\" вне диапазона 0..0xFFFFFFFE");
        draft.leaf = static_cast<uint32_t>(leaf->number);
    }

    const Value* recs = json::find(root.object, "records");
    if (!recs || !recs->is_array())
        throw DraftError("черновик: нужен массив \"records\"");
    if (recs->array.empty())
        throw DraftError("черновик: \"records\" пуст — нечего подписывать");
    if (recs->array.size() > MAX_DRAFT_RECORDS)
        throw DraftError("черновик: записей " + std::to_string(recs->array.size()) +
                         ", максимум " + std::to_string(MAX_DRAFT_RECORDS) +
                         " — пакет должен оставаться читаемым целиком");

    for (std::size_t i = 0; i < recs->array.size(); ++i) {
        const Value&      item  = recs->array[i];
        const std::string where = "запись #" + std::to_string(i + 1);
        if (!item.is_object()) throw DraftError(where + ": ожидался объект");
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
