#pragma once

#include "types.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace records {

// ── Draft: prepared by a scribe, signed by the owner (ИР-005 п.5) ─────────────
//
// A key-free authoring format. A helper — an accountant, a scribe, an AI — turns
// a conversation into a draft; the draft carries NO authority whatsoever. It
// becomes real only when the owner appends each record to their own branch with
// their own key: one block, one signature (blockchain.md §5). So a draft is safe
// to send over any channel and safe to generate with a commercial model.
//
// Only fully reversible, value-free record types are accepted — the whitelist is
// the «градиент необратимости» of ИР-002 made concrete. A mistaken Concept is
// retired by a follow-up record; a mistaken Transfer moves value irreversibly.
// Value-bearing records (Transfer, Pledge, Acceptance, Redemption) and key
// operations are refused outright, so a Transfer can never hide among thirty
// profile facts. That refusal — not the reader's vigilance — is what makes it
// safe to confirm a batch in one deliberate reading.

class DraftError : public std::runtime_error {
public:
    explicit DraftError(const std::string& msg) : std::runtime_error(msg) {}
};

// A batch a human can actually read before signing. Past this, review degrades
// into clicking "yes" — and confirmation fatigue is itself an attack surface.
inline constexpr std::size_t MAX_DRAFT_RECORDS = 20;

struct Draft {
    std::optional<uint32_t> leaf;     // target branch; the CLI default when absent
    std::vector<Record>     records;  // 1..MAX_DRAFT_RECORDS, whitelisted types only
};

// Parse and validate a draft:
//
// {
//   "leaf": 2147483647,                        // optional
//   "records": [
//     {"t":"concept",      "text":"Электромонтаж, 8 лет",
//                          "tags":["kind:skill","cat:prof.electrician"]},
//     {"t":"concept-link", "from":"<64hex>/<64hex>", "to":"<64hex>/<64hex>",
//                          "kind":"закрыто"},
//     {"t":"composite",    "title":"Профиль", "parts":["<64hex>/<64hex>"]}
//   ]
// }
//
// Throws DraftError: malformed JSON, unknown or non-whitelisted "t", a missing or
// ill-typed field, a bad Ref, an empty draft, more than MAX_DRAFT_RECORDS entries.
Draft parse_draft(const std::string& json);

// What the owner reads before signing. Never truncates — hiding part of a record
// is precisely the risk being guarded against.
std::string render_record(const Record& rec);

} // namespace records
