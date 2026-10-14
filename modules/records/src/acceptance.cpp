#include "records/acceptance.h"

#include <algorithm>
#include <cmath>
#include <map>

namespace records {

bool addressed_to(const Transfer& deal,
                  const std::optional<std::array<uint8_t, 32>>& customer,
                  const Release* release) noexcept {
    if (!deal.accept) return false;
    const Acceptance& a = *deal.accept;
    if (a.release)
        return release != nullptr
            && release->work == a.work
            && release->to == deal.from
            && deal.timestamp < release->expires;
    return customer && *customer == deal.from;
}

DealRecognition recognize_deals(const std::vector<DealClaim>& claims) {
    using Hash32 = std::array<uint8_t, 32>;
    // Per work; the same block seen twice is one deal.
    std::map<Hash32, std::map<Hash32, const DealClaim*>> by_work;
    for (const auto& c : claims) by_work[c.work].emplace(c.deal, &c);

    DealRecognition out;
    for (const auto& [work, deals] : by_work) {
        // What each addressed deal bought: the whole result, or a portion.
        std::map<Hash32, std::vector<const DealClaim*>> by_result;
        bool whole = false, portions = false;
        for (const auto& [hash, c] : deals) {
            if (!c->addressed) { out.unaddressed.insert(hash); continue; }
            if (!c->settled)   { out.unsettled.insert(hash);   continue; }
            (c->portion ? portions : whole) = true;
            by_result[c->portion ? *c->portion : work].push_back(c);
        }
        if (by_result.empty()) continue;

        // Sold whole and in portions at once: the same hours given twice
        // (records.md §9.8 v2) — by the seller, who signed both.
        if (whole && portions) {
            out.oversold.insert(work);
            for (const auto& [result, cs] : by_result)
                for (const auto* c : cs) out.contested.insert(c->deal);
            continue;
        }

        for (const auto& [result, cs] : by_result) {
            // A work outside its worker's thread or on hours another work of the
            // chain claims, a portion off its lot (records.md §9.4 v4, records.md §9.8 v2):
            // the payment stands — the seller addressed it — but the deal does
            // not count; its hours may be sold twice.
            if (!cs.front()->sound) {
                out.unsound_results.insert(result);
                for (const auto* c : cs) out.unsound.insert(c->deal);
                continue;
            }
            std::set<Hash32> payers;
            for (const auto* c : cs) payers.insert(c->payer);
            if (payers.size() > 1) {    // the seller gave one result to two buyers
                out.oversold.insert(result);
                for (const auto* c : cs) out.contested.insert(c->deal);
                continue;
            }
            // One buyer: the first of their deals counts, by their own timestamp
            // and then by hash, so every reader picks the same one.
            const auto* first = *std::min_element(
                cs.begin(), cs.end(), [](const DealClaim* x, const DealClaim* y) {
                    return x->timestamp != y->timestamp ? x->timestamp < y->timestamp
                                                        : x->deal < y->deal;
                });
            for (const auto* c : cs)
                (c != first ? out.repeated : c->resale ? out.resold : out.recognized)
                    .insert(c->deal);
        }
    }
    return out;
}

bool settled_exactly(double paid, double payable) noexcept {
    return paid > 0.0 && std::abs(paid - payable) <= 1e-6;
}

} // namespace records
