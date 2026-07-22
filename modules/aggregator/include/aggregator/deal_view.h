#pragma once

#include "profile_view.h"

#include <optional>
#include <string>
#include <vector>

namespace aggregator {

// ── Deal view (records.md §8.6 «словарь сделки»; ИР-006 шаг 2) ────────────────
//
// A deal is NOT a rigid pipeline. It is a need with a tail of records pointing
// at it: "берусь" links (takers), pledges, "исполняет" links (work done under
// it), acceptances of that work, transfers paying those acceptances, and the
// owner's closing link. Every link is optional — a direct job with no posted
// need, work done for stock, a need closed by a neighbour's favour are all
// legal states, not errors. The stage is derived from whichever links exist.
//
// The whole vocabulary is ordinary ConceptLinks — zero new protocol types.
//
// Spoof guards mirror the rest of the aggregator: a link binds only records
// its author owns ("берусь"/"исполняет" — your own skill/work; "закрыто" —
// your own need), an Acceptance counts only from its receiver, a Transfer
// only from its sender. Advisory and re-checkable, like every derived view.

enum class DealStage {
    Open,      // потребность висит, никто не взялся
    Taken,     // есть «берусь» — ждёт найма
    Hired,     // есть активное обещание — ждёт работы
    Worked,    // работа сделана — ждёт приёмки
    Accepted,  // принято — ждёт оплаты
    Paid,      // оплачено сполна — ждёт закрытия хозяином
    Closed,    // снято хозяином
};

struct DealTaker {
    UserId chain{};       // who volunteered
    Hash   skill_hash{};  // their skill fact ("берусь" from-side)
};

struct DealPledge {
    UserId                pledger{};
    Hash                  pledge_hash{};
    double                units = 0;
    std::optional<UserId> executor;
    double                settled = 0;   // via Transfer.settles (v4)
    bool                  revoked = false;

    bool active() const noexcept { return !revoked && settled + 1e-9 < units; }
};

// One WorkRecord attached to the deal by its worker's "исполняет" link,
// plus whatever acceptance/payment reached it.
struct DealWork {
    UserId      worker{};
    Hash        work_hash{};
    std::string action;
    double      hours = 0;

    bool        accepted = false;
    UserId      acceptor{};          // acceptance owner (the customer)
    Hash        acceptance_hash{};
    double      labor_units = 0;     // appraisal of the live labor
    double      carried = 0;         // carried cost of tools/materials (§9.5 v2)
    double      paid = 0;            // transfers whose reason names the acceptance
};

struct Deal {
    // Anchor: a posted need — or, when need_hash is nullopt, a direct deal
    // anchored by its acceptance (case 2: nobody posted a need).
    UserId              seeker{};
    std::optional<Hash> need_hash;
    std::string         slug, text, urgency;
    bool                closed = false;

    std::vector<DealTaker>  takers;
    std::vector<DealPledge> pledges;
    std::vector<DealWork>   works;

    DealStage stage() const noexcept;
    double    appraised() const noexcept;   // Σ (labor + carried) of accepted works
    double    paid() const noexcept;
};

class DealView {
public:
    static DealView build(const AggregatorStorage& storage);

    // Need-anchored deals first (posted order per chain), then direct ones.
    const std::vector<Deal>& deals() const noexcept { return deals_; }

    // Deals a chain takes part in — as seeker, taker or worker.
    std::vector<const Deal*> for_chain(const UserId& uid) const;

    static const char* stage_name(DealStage stage) noexcept;

private:
    std::vector<Deal> deals_;
};

} // namespace aggregator
