#include "records/catalog.h"

#include "json.h"

namespace records {

namespace {

using json::Object;
using json::Value;

std::string require_string(const Object& obj, const std::string& key,
                           const std::string& where) {
    const Value* v = json::find(obj, key);
    if (!v || !v->is_string() || v->string.empty())
        throw CatalogError(where + ": нужно непустое строковое поле \"" + key + "\"");
    return v->string;
}

std::vector<std::string> optional_strings(const Object& obj, const std::string& key,
                                          const std::string& where) {
    const Value* v = json::find(obj, key);
    if (!v) return {};
    if (!v->is_array())
        throw CatalogError(where + ": поле \"" + key + "\" должно быть массивом");
    std::vector<std::string> out;
    for (const auto& item : v->array) {
        if (!item.is_string())
            throw CatalogError(where + ": в \"" + key + "\" ожидались строки");
        out.push_back(item.string);
    }
    return out;
}

Catalog catalog_from(const Value& root, const std::string& where) {
    if (!root.is_object())
        throw CatalogError(where + ": ожидался объект");

    Catalog cat;
    cat.name    = require_string(root.object, "catalog", where);
    cat.version = require_string(root.object, "version", where);

    const Value* entries = json::find(root.object, "entries");
    if (!entries || !entries->is_array())
        throw CatalogError(where + ": нужен массив \"entries\"");

    for (std::size_t i = 0; i < entries->array.size(); ++i) {
        const Value&      item = entries->array[i];
        const std::string ew   = where + ", запись #" + std::to_string(i + 1);
        if (!item.is_object()) throw CatalogError(ew + ": ожидался объект");

        CatalogEntry e;
        e.slug      = require_string(item.object, "slug", ew);
        e.ru        = require_string(item.object, "ru",   ew);
        if (const Value* g = json::find(item.object, "group"); g && g->is_string())
            e.group = g->string;
        e.aliases   = optional_strings(item.object, "aliases",   ew);
        e.closed_by = optional_strings(item.object, "closed_by", ew);
        cat.entries.push_back(std::move(e));
    }
    return cat;
}

} // namespace

const CatalogEntry* Catalog::find(const std::string& slug) const noexcept {
    for (const auto& e : entries)
        if (e.slug == slug) return &e;
    return nullptr;
}

Catalog parse_catalog(const std::string& json_text) {
    Value root;
    try {
        root = json::Reader(json_text).read();
    } catch (const json::JsonError& e) {
        throw CatalogError(std::string("каталог, ") + e.what());
    }
    return catalog_from(root, "каталог");
}

std::vector<Catalog> parse_catalog_bundle(const std::string& json_text) {
    Value root;
    try {
        root = json::Reader(json_text).read();
    } catch (const json::JsonError& e) {
        throw CatalogError(std::string("каталоги, ") + e.what());
    }
    if (!root.is_object())
        throw CatalogError("каталоги: на верхнем уровне ожидался объект");

    std::vector<Catalog> out;
    for (const auto& [name, value] : root.object)
        out.push_back(catalog_from(value, "каталог \"" + name + "\""));
    if (out.empty())
        throw CatalogError("каталоги: пусто");
    return out;
}

std::set<std::string> all_slugs(const std::vector<Catalog>& catalogs) {
    std::set<std::string> out;
    for (const auto& cat : catalogs)
        for (const auto& e : cat.entries) out.insert(e.slug);
    return out;
}

std::vector<const CatalogEntry*> search(const std::vector<Catalog>& catalogs,
                                        const std::string&          query) {
    std::vector<const CatalogEntry*> out;
    for (const auto& cat : catalogs) {
        for (const auto& e : cat.entries) {
            if (query.empty()) { out.push_back(&e); continue; }
            bool hit = e.slug.find(query) != std::string::npos ||
                       e.ru.find(query)   != std::string::npos;
            for (const auto& a : e.aliases)
                hit = hit || a.find(query) != std::string::npos;
            if (hit) out.push_back(&e);
        }
    }
    return out;
}

} // namespace records
