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
    // Per result; the same block seen twice is one deal.
    std::map<Hash32, std::map<Hash32, const DealClaim*>> by_work;
    for (const auto& c : claims) by_work[c.work].emplace(c.deal, &c);

    DealRecognition out;
    for (const auto& [work, deals] : by_work) {
        std::vector<const DealClaim*> addressed;
        std::set<Hash32>              payers;
        for (const auto& [hash, c] : deals) {
            if (!c->addressed) { out.unaddressed.insert(hash); continue; }
            addressed.push_back(c);
            payers.insert(c->payer);
        }
        if (addressed.empty()) continue;
        // A work outside its worker's thread, or on hours another work of the
        // chain also claims (records.md §9.4 v4): the payment stands — the seller
        // addressed it — but the deal does not count; its hours may be sold twice.
        if (!addressed.front()->sound) {
            out.unsound_works.insert(work);
            for (const auto* c : addressed) out.unsound.insert(c->deal);
            continue;
        }
        if (payers.size() > 1) {        // the seller gave one result to two buyers
            out.oversold.insert(work);
            for (const auto* c : addressed) out.contested.insert(c->deal);
            continue;
        }
        // One buyer: the first of their deals counts, by their own timestamp and
        // then by hash, so every reader picks the same one.
        const auto* first = *std::min_element(
            addressed.begin(), addressed.end(),
            [](const DealClaim* x, const DealClaim* y) {
                return x->timestamp != y->timestamp ? x->timestamp < y->timestamp
                                                    : x->deal < y->deal;
            });
        for (const auto* c : addressed)
            (c == first ? out.recognized : out.repeated).insert(c->deal);
    }
    return out;
}

bool settled_exactly(double paid, double payable) noexcept {
    return paid > 0.0 && std::abs(paid - payable) <= 1e-6;
}

} // namespace records
