#include "records/social.h"

#include <algorithm>
#include <set>
#include <utility>
#include <vector>

namespace records {

namespace {

constexpr std::size_t kMaxNamespace = 16;

bool is_lower_alnum(char c) noexcept { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'); }
bool is_digit(char c) noexcept { return c >= '0' && c <= '9'; }
bool is_alpha(char c) noexcept { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }

// Пробел, управляющий знак или DEL: в ключе и ссылке их не бывает.
bool is_blank(char c) noexcept {
    const auto u = static_cast<unsigned char>(c);
    return u <= 0x20 || u == 0x7f;
}

int hex_value(char c) noexcept {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

char to_lower(char c) noexcept { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; }
char to_upper(char c) noexcept { return (c >= 'a' && c <= 'z') ? static_cast<char>(c - 'a' + 'A') : c; }

std::string lowered(std::string_view s) {
    std::string out(s);
    std::transform(out.begin(), out.end(), out.begin(), to_lower);
    return out;
}

bool starts_with_ci(std::string_view s, std::string_view prefix) noexcept {
    if (s.size() < prefix.size()) return false;
    for (std::size_t i = 0; i < prefix.size(); ++i)
        if (to_lower(s[i]) != prefix[i]) return false;
    return true;
}

bool all_digits(std::string_view s) noexcept {
    return !s.empty() && std::all_of(s.begin(), s.end(), is_digit);
}

std::string_view trim(std::string_view s) noexcept {
    while (!s.empty() && is_blank(s.front())) s.remove_prefix(1);
    while (!s.empty() && is_blank(s.back()))  s.remove_suffix(1);
    return s;
}

// Номер без ведущих нулей: «042» и «42» — один пост.
std::string strip_zeros(std::string_view digits) {
    const auto first = digits.find_first_not_of('0');
    return first == std::string_view::npos ? std::string("0") : std::string(digits.substr(first));
}

// Части между разделителями, пустые — тоже.
std::vector<std::string_view> split(std::string_view s, char sep) {
    std::vector<std::string_view> out;
    std::size_t pos = 0;
    for (auto at = s.find(sep); at != std::string_view::npos; at = s.find(sep, pos)) {
        out.push_back(s.substr(pos, at - pos));
        pos = at + 1;
    }
    out.push_back(s.substr(pos));
    return out;
}

// Непустые сегменты пути.
std::vector<std::string> segments(std::string_view path) {
    std::vector<std::string> out;
    for (const auto seg : split(path, '/'))
        if (!seg.empty()) out.emplace_back(seg);
    return out;
}

// Пространство ключа: [a-z0-9]{1,16} перед «:», не схема ссылки (social.md §3.1).
std::optional<std::string_view> key_namespace(std::string_view s) noexcept {
    const auto colon = s.find(':');
    if (colon == std::string_view::npos || colon == 0 || colon > kMaxNamespace)
        return std::nullopt;
    const auto ns = s.substr(0, colon);
    if (!std::all_of(ns.begin(), ns.end(), is_lower_alnum)) return std::nullopt;
    if (ns == "http" || ns == "https") return std::nullopt;
    if (s.substr(colon + 1).substr(0, 2) == "//") return std::nullopt;
    return ns;
}

// ── YouTube (social.md §3.2) ─────────────────────────────────────────────────

bool is_youtube_id(std::string_view id) noexcept {
    return id.size() == 11 && std::all_of(id.begin(), id.end(), [](char c) {
        return is_alpha(c) || is_digit(c) || c == '_' || c == '-';
    });
}

// ── Telegram (social.md §3.3) ────────────────────────────────────────────────

bool is_telegram_name(std::string_view name) noexcept {
    return !name.empty() && name.size() <= 64 &&
           std::all_of(name.begin(), name.end(),
                       [](char c) { return is_alpha(c) || is_digit(c) || c == '_'; });
}

// Служебные пути t.me — не каналы.
const std::set<std::string>& telegram_reserved() {
    static const std::set<std::string> names = {
        "addemoji", "addlist", "addstickers", "addtheme", "bg", "boost", "c",
        "confirmphone", "contact", "invoice", "iv", "joinchat", "login", "proxy",
        "setlanguage", "share", "socks",
    };
    return names;
}

std::optional<std::string> telegram_key(std::vector<std::string> segs) {
    if (!segs.empty() && segs.front() == "s") segs.erase(segs.begin());   // веб-превью
    if (segs.size() == 3 && segs[0] == "c" && all_digits(segs[1]) && all_digits(segs[2]))
        return "tg:c/" + strip_zeros(segs[1]) + "/" + strip_zeros(segs[2]);
    if (segs.empty() || segs.size() > 2 || !is_telegram_name(segs[0])) return std::nullopt;
    const auto name = lowered(segs[0]);
    if (telegram_reserved().count(name)) return std::nullopt;
    if (segs.size() == 1) return "tg:" + name;
    if (!all_digits(segs[1])) return std::nullopt;
    return "tg:" + name + "/" + strip_zeros(segs[1]);
}

// ── Любая другая ссылка (social.md §3.4) ─────────────────────────────────────

bool is_unreserved(char c) noexcept {
    return is_alpha(c) || is_digit(c) || c == '-' || c == '.' || c == '_' || c == '~';
}

// %xx: hex заглавными, незарезервированные знаки раскрыты (RFC 3986 §6.2.2).
std::string norm_percent(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size() && hex_value(s[i + 1]) >= 0 &&
            hex_value(s[i + 2]) >= 0) {
            const char c = static_cast<char>(hex_value(s[i + 1]) * 16 + hex_value(s[i + 2]));
            if (is_unreserved(c)) {
                out += c;
            } else {
                out += '%';
                out += to_upper(s[i + 1]);
                out += to_upper(s[i + 2]);
            }
            i += 2;
        } else {
            out += s[i];
        }
    }
    return out;
}

bool is_tracking(const std::string& name) {
    static const std::set<std::string> names = {
        "_ga", "_gl", "dclid", "fbclid", "gbraid", "gclid", "igshid", "mc_cid",
        "mc_eid", "msclkid", "wbraid", "yclid", "ysclid",
    };
    const auto lname = lowered(name);
    return lname.rfind("utm_", 0) == 0 || names.count(lname) > 0;
}

// Метки шеринга YouTube — у страниц, которые не ролик (канал, плейлист).
const std::set<std::string>& youtube_share_params() {
    static const std::set<std::string> names = {"feature", "pp", "si"};
    return names;
}

std::string norm_query(std::string_view query, const std::set<std::string>& also_drop) {
    std::vector<std::pair<std::string, std::string>> params;   // имя, параметр целиком
    for (const auto raw : split(query, '&')) {
        if (raw.empty()) continue;
        const auto param = norm_percent(raw);
        const auto name  = param.substr(0, param.find('='));
        if (is_tracking(name) || also_drop.count(name)) continue;
        params.emplace_back(name, param);
    }
    std::stable_sort(params.begin(), params.end(),
                     [](const auto& a, const auto& b) { return a.first < b.first; });
    std::string out;
    for (const auto& [name, param] : params) {
        if (!out.empty()) out += '&';
        out += param;
    }
    return out;
}

std::optional<std::string> query_param(std::string_view query, std::string_view name) {
    for (const auto param : split(query, '&')) {
        const auto eq = param.find('=');
        if (param.substr(0, eq) == name)
            return std::string(eq == std::string_view::npos ? std::string_view{} : param.substr(eq + 1));
    }
    return std::nullopt;
}

// ── Товары (social.md §3.6) ───────────────────────────────────────────────────

// GS1: последняя цифра — контрольная; веса 3, 1, 3, … справа налево без неё.
bool gtin_check_ok(std::string_view d) noexcept {
    int sum = 0;
    for (std::size_t i = 0; i + 1 < d.size(); ++i)
        sum += (d[d.size() - 2 - i] - '0') * (i % 2 == 0 ? 3 : 1);
    return (10 - sum % 10) % 10 == d.back() - '0';
}

// Штрихкод: EAN-8, UPC-A, EAN-13, GTIN-14 — к 14 цифрам, с проверкой контрольной.
std::optional<std::string> gtin_key(std::string_view digits) {
    const auto n = digits.size();
    if (!all_digits(digits) || (n != 8 && n != 12 && n != 13 && n != 14) || !gtin_check_ok(digits))
        return std::nullopt;
    return "gtin:" + std::string(14 - n, '0') + std::string(digits);
}

// Название модели: латиница и кириллица — строчными, «ё» — «е», всё, что не буква
// и не цифра, — один «-» между словами; другие письменности — как есть.
std::optional<std::string> model_key(std::string_view name) {
    std::string out;
    bool        gap = false;
    const auto  put = [&](const std::string& piece) {
        if (gap && !out.empty()) out += '-';
        gap = false;
        out += piece;
    };
    for (std::size_t i = 0; i < name.size(); ++i) {
        const auto c = static_cast<unsigned char>(name[i]);
        if (c < 0x80) {
            if (is_alpha(static_cast<char>(c)) || is_digit(static_cast<char>(c)))
                put(std::string(1, to_lower(static_cast<char>(c))));
            else
                gap = true;
            continue;
        }
        const std::size_t len = c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : c >= 0xC0 ? 2 : 1;
        if (len == 1 || i + len > name.size()) return std::nullopt;   // не UTF-8
        std::string ch(name.substr(i, len));
        // Знаки, а не буквы: U+0080–U+00BF («», ©, неразрывный пробел) и общая
        // пунктуация U+2000–U+207F (тире, кавычки, многоточие) — граница слов.
        const auto c2 = static_cast<unsigned char>(ch[1]);
        if ((len == 2 && c == 0xC2) || (len == 3 && c == 0xE2 && (c2 == 0x80 || c2 == 0x81))) {
            gap = true;
            i += len - 1;
            continue;
        }
        if (len == 2 && c == 0xD0) {
            if (c2 >= 0x90 && c2 <= 0x9F) {                 // А–П → а–п
                ch[1] = static_cast<char>(c2 + 0x20);
            } else if (c2 >= 0xA0 && c2 <= 0xAF) {          // Р–Я → р–я
                ch = {static_cast<char>(0xD1), static_cast<char>(c2 - 0x20)};
            } else if (c2 == 0x81) {                        // Ё → е
                ch = {static_cast<char>(0xD0), static_cast<char>(0xB5)};
            }
        } else if (len == 2 && c == 0xD1 && c2 == 0x91) {
            ch = {static_cast<char>(0xD0), static_cast<char>(0xB5)};   // ё → е
        }
        put(ch);
        i += len - 1;
    }
    if (out.empty()) return std::nullopt;
    return "model:" + out;
}

bool is_asin(std::string_view s) noexcept {
    return s.size() == 10 &&
           std::all_of(s.begin(), s.end(), [](char c) { return is_alpha(c) || is_digit(c); });
}

std::string uppered(std::string_view s) {
    std::string out(s);
    std::transform(out.begin(), out.end(), out.begin(), to_upper);
    return out;
}

// Цифры после последнего «-»: «powerbank-xiaomi-123456» → «123456».
std::string_view tail_digits(std::string_view seg) {
    const auto dash = seg.rfind('-');
    return dash == std::string_view::npos ? seg : seg.substr(dash + 1);
}

bool ends_with(std::string_view s, std::string_view tail) noexcept {
    return s.size() >= tail.size() && s.substr(s.size() - tail.size()) == tail;
}

// Ссылка магазина → ключ товара; nullopt — не страница товара этого магазина.
std::optional<std::string> shop_key(const std::string& host, const std::vector<std::string>& segs) {
    const auto first = [&](std::string_view name) { return !segs.empty() && segs[0] == name; };
    if (host == "ozon.ru" || host == "ozon.kz" || host == "ozon.by" || host == "ozon.com") {
        if (first("product") && segs.size() >= 2 && all_digits(tail_digits(segs[1])))
            return "ozon:" + std::string(tail_digits(segs[1]));
        return std::nullopt;
    }
    if (host == "wildberries.ru" || host == "wildberries.by" || host == "wildberries.kz" || host == "wb.ru") {
        if (first("catalog") && segs.size() >= 2 && all_digits(segs[1])) return "wb:" + segs[1];
        return std::nullopt;
    }
    if (host == "market.yandex.ru" || host == "market.yandex.kz" || host == "market.yandex.by") {
        if (!segs.empty() && (segs[0] == "product" || segs[0].rfind("product--", 0) == 0) &&
            segs.size() >= 2 && all_digits(segs[1]))
            return "ymarket:" + segs[1];
        if (first("card") && segs.size() >= 3 && all_digits(segs[2])) return "ymarket:card/" + segs[2];
        return std::nullopt;
    }
    const std::string bare = host.rfind("smile.", 0) == 0 ? host.substr(6) : host;
    if (bare == "amazon.com" || bare.rfind("amazon.", 0) == 0) {
        for (std::size_t i = 0; i + 1 < segs.size(); ++i)
            if (segs[i] == "dp" && is_asin(segs[i + 1])) return "amazon:" + uppered(segs[i + 1]);
        for (std::size_t i = 0; i + 2 < segs.size(); ++i)
            if (segs[i] == "gp" && (segs[i + 1] == "product" || segs[i + 1] == "aw") ) {
                const auto& id = segs[i + 1] == "aw" && i + 3 < segs.size() ? segs[i + 3] : segs[i + 2];
                if (is_asin(id)) return "amazon:" + uppered(id);
            }
        return std::nullopt;
    }
    if (host == "aliexpress.com" || host == "aliexpress.ru" || host == "aliexpress.us" ||
        ends_with(host, ".aliexpress.com") || ends_with(host, ".aliexpress.ru")) {
        if (first("item") && segs.size() >= 2) {
            const std::string_view id = ends_with(segs[1], ".html")
                ? std::string_view(segs[1]).substr(0, segs[1].size() - 5) : std::string_view(segs[1]);
            if (all_digits(id)) return "ali:" + std::string(id);
        }
        return std::nullopt;
    }
    return std::nullopt;
}

// Готовый ключ магазина — по форме его номера.
std::optional<std::string> shop_key_form(std::string_view ns, std::string_view value) {
    if ((ns == "ozon" || ns == "wb" || ns == "ali") && all_digits(value))
        return std::string(ns) + ":" + std::string(value);
    if (ns == "ymarket" && (all_digits(value) || (value.rfind("card/", 0) == 0 && all_digits(value.substr(5)))))
        return "ymarket:" + std::string(value);
    if (ns == "amazon" && is_asin(value)) return "amazon:" + uppered(value);
    return std::nullopt;
}

struct LinkParts {
    std::string host;    // строчными, без завершающей точки
    std::string port;    // пусто — порт по умолчанию
    std::string path;
    std::string query;
};

std::optional<LinkParts> split_link(std::string_view s) {
    if (std::any_of(s.begin(), s.end(), is_blank)) return std::nullopt;
    const auto auth_end = s.find_first_of("/?#");
    const auto scheme   = s.find("://");
    if (starts_with_ci(s, "https://"))      s.remove_prefix(8);
    else if (starts_with_ci(s, "http://"))  s.remove_prefix(7);
    else if (scheme != std::string_view::npos && scheme < auth_end)
        return std::nullopt;                                     // другая схема: не страница
    else if (s.substr(0, 2) == "//")        s.remove_prefix(2);  // без схемы, но с «//»

    const auto end  = s.find_first_of("/?#");
    auto       auth = s.substr(0, end);
    auto       rest = end == std::string_view::npos ? std::string_view{} : s.substr(end);
    if (const auto at = auth.rfind('@'); at != std::string_view::npos) auth.remove_prefix(at + 1);

    LinkParts p;
    const auto bracket = auth.rfind(']');                        // [IPv6]:порт
    const auto colon   = auth.rfind(':');
    if (colon != std::string_view::npos && (bracket == std::string_view::npos || colon > bracket)) {
        p.port = std::string(auth.substr(colon + 1));
        auth   = auth.substr(0, colon);
        if (!p.port.empty() && !all_digits(p.port)) return std::nullopt;
        if (!p.port.empty()) p.port = strip_zeros(p.port);
        if (p.port == "80" || p.port == "443") p.port.clear();
    }
    p.host = lowered(auth);
    while (!p.host.empty() && p.host.back() == '.') p.host.pop_back();
    if (p.host.empty()) return std::nullopt;

    if (const auto hash = rest.find('#'); hash != std::string_view::npos) rest = rest.substr(0, hash);
    const auto q = rest.find('?');
    p.path  = std::string(rest.substr(0, q));
    p.query = q == std::string_view::npos ? std::string{} : std::string(rest.substr(q + 1));
    return p;
}

std::string url_key(const std::string& host, const std::string& port, std::string_view path,
                    std::string_view query, const std::set<std::string>& also_drop) {
    std::string key = "url:" + host;
    if (!port.empty()) key += ":" + port;
    auto p = norm_percent(path);
    while (!p.empty() && p.back() == '/') p.pop_back();
    key += p;
    const auto q = norm_query(query, also_drop);
    if (!q.empty()) key += "?" + q;
    return key;
}

std::optional<std::string> link_key(std::string_view link) {
    const auto p = split_link(link);
    if (!p) return std::nullopt;
    const std::string host = p->host.rfind("www.", 0) == 0 ? p->host.substr(4) : p->host;
    const auto        segs = segments(p->path);

    if (host == "youtu.be") {
        if (!segs.empty() && is_youtube_id(segs[0])) return "yt:" + segs[0];
        return url_key(host, p->port, p->path, p->query, youtube_share_params());
    }
    if (host == "youtube.com" || host == "m.youtube.com" || host == "music.youtube.com" ||
        host == "youtube-nocookie.com") {
        if (!segs.empty() && segs[0] == "watch")
            if (const auto v = query_param(p->query, "v"); v && is_youtube_id(*v)) return "yt:" + *v;
        if (segs.size() >= 2 && is_youtube_id(segs[1]) &&
            (segs[0] == "shorts" || segs[0] == "embed" || segs[0] == "live" || segs[0] == "v"))
            return "yt:" + segs[1];
        // Канал с телефона и с компьютера — один предмет (social.md §3.2).
        return url_key("youtube.com", {}, p->path, p->query, youtube_share_params());
    }
    if (host == "t.me" || host == "telegram.me") {
        if (auto k = telegram_key(segs)) return k;
        return url_key("t.me", {}, p->path, p->query, {});
    }
    if (auto k = shop_key(host, segs)) return k;
    return url_key(host, p->port, p->path, p->query, {});
}

std::string to_hex(const uint8_t* data, std::size_t len) {
    static const char* digits = "0123456789abcdef";
    std::string s;
    s.reserve(len * 2);
    for (std::size_t i = 0; i < len; ++i) {
        s += digits[data[i] >> 4];
        s += digits[data[i] & 0x0f];
    }
    return s;
}

bool from_hex32(std::string_view s, std::array<uint8_t, 32>& out) noexcept {
    if (s.size() != 64) return false;
    for (std::size_t i = 0; i < 32; ++i) {
        const int hi = hex_value(s[2 * i]), lo = hex_value(s[2 * i + 1]);
        if (hi < 0 || lo < 0) return false;
        out[i] = static_cast<uint8_t>(hi * 16 + lo);
    }
    return true;
}

// Объект по хешу содержимого (social.md §3.5): b2: и 64 hex BLAKE2b-256, строчными.
std::optional<std::string> content_key(std::string_view hex) {
    if (hex.size() != 64 || !std::all_of(hex.begin(), hex.end(), [](char c) { return hex_value(c) >= 0; }))
        return std::nullopt;
    return "b2:" + lowered(hex);
}

} // namespace

bool is_external_key(std::string_view key) noexcept {
    const auto ns = key_namespace(key);
    if (!ns) return false;
    const auto value = key.substr(ns->size() + 1);
    return !value.empty() && std::none_of(value.begin(), value.end(), is_blank);
}

std::optional<std::string> external_key(std::string_view link_or_key) {
    const auto in = trim(link_or_key);
    if (in.empty()) return std::nullopt;
    if (all_digits(in)) return gtin_key(in);   // голый штрихкод
    if (const auto ns = key_namespace(in)) {
        const auto value = in.substr(ns->size() + 1);
        if (*ns == "gtin")  return gtin_key(value);
        if (*ns == "model") return model_key(value);
        if (*ns == "ozon" || *ns == "wb" || *ns == "ymarket" || *ns == "amazon" || *ns == "ali")
            return shop_key_form(*ns, value);
        if (*ns == "yt")
            return is_youtube_id(value) ? std::optional<std::string>("yt:" + std::string(value))
                                        : std::nullopt;
        if (*ns == "tg")  return telegram_key(segments(value));
        if (*ns == "b2")  return content_key(value);
        if (*ns == "url") return link_key(value);
        return is_external_key(in) ? std::optional<std::string>(std::string(in)) : std::nullopt;
    }
    return link_key(in);
}

std::string subject_id(const Subject& s) {
    if (const auto* r = std::get_if<Ref>(&s))
        return to_hex(r->chain.data(), 32) + "/" + to_hex(r->hash.data(), 32);
    if (const auto* k = std::get_if<std::string>(&s)) return *k;
    const auto& c = std::get<ChainId>(s);
    return to_hex(c.data(), 32);
}

std::optional<Subject> parse_subject(std::string_view s) {
    if (s.find(':') != std::string_view::npos) {
        if (!is_external_key(s)) return std::nullopt;
        return Subject{std::string(s)};
    }
    if (s.size() == 129 && s[64] == '/') {
        Ref r{};
        if (!from_hex32(s.substr(0, 64), r.chain) || !from_hex32(s.substr(65), r.hash))
            return std::nullopt;
        return Subject{r};
    }
    ChainId c{};
    if (!from_hex32(s, c)) return std::nullopt;
    return Subject{c};
}

} // namespace records
