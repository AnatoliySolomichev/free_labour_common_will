#include "records/credit.h"

#include <algorithm>
#include <cmath>
#include <set>

namespace records {

CreditHistory credit_history(std::vector<ObservedLink> links) {
    CreditHistory h{};
    if (links.empty()) return h;

    std::sort(links.begin(), links.end(),
              [](const ObservedLink& a, const ObservedLink& b) {
                  if (a.link.seq != b.link.seq) return a.link.seq < b.link.seq;
                  return a.block_hash < b.block_hash;
              });
    // The same block can arrive via merge and via fetch — not equivocation.
    links.erase(std::unique(links.begin(), links.end(),
                            [](const ObservedLink& a, const ObservedLink& b) {
                                return a.link.seq == b.link.seq &&
                                       a.block_hash == b.block_hash;
                            }),
                links.end());

    // Own deals a reversal names: their self-issue was never born.
    std::set<std::array<uint8_t, 32>> reversed;
    for (const auto& o : links)
        if (o.is_reversal) reversed.insert(o.reverses);

    double   peak     = 0;
    double   phantom  = 0;   // declared, but reversed further down the thread
    uint64_t prev_seq = 0;
    uint64_t issues   = 0;
    bool     first    = true;
    for (const auto& o : links) {
        if (!first && o.link.seq == prev_seq) {
            // One seq in two different blocks: the thread forked.
            if (h.equivocated_seqs.empty() ||
                h.equivocated_seqs.back() != o.link.seq)
                h.equivocated_seqs.push_back(o.link.seq);
            continue;  // digest the first-seen block of the forked link
        }
        first    = false;
        prev_seq = o.link.seq;
        ++h.links_seen;

        // The thread declares debt as a non-positive debt_after (economy.md §4.3).
        const double outstanding = -o.link.debt_after;
        h.outstanding = outstanding;

        if (o.is_reversal) {
            // The declared level steps back by the units it once added.
            h.reversed += o.units;
            ++h.reversals;
            phantom -= o.units;
            continue;
        }
        if (!o.is_redemption && reversed.count(o.block_hash)) {
            // An offer the seller refused or let lapse: it declared a debt that
            // never existed. Until its reversal every declared level carries it,
            // so the debt curve takes it back out — or a refused offer would pass
            // for a debt repaid (records.md §11.6).
            phantom += o.units;
            continue;
        }
        if (o.is_redemption) {
            h.redeemed += o.units;
            if (!h.last_redemption_ts || o.timestamp > *h.last_redemption_ts)
                h.last_redemption_ts = o.timestamp;
        } else {
            h.issued += o.units;
            ++issues;
            if (!h.last_selfissue_ts || o.timestamp > *h.last_selfissue_ts)
                h.last_selfissue_ts = o.timestamp;
        }

        const double real = outstanding - phantom;
        peak         = std::max(peak, real);
        h.max_repaid = std::max(h.max_repaid, peak - real);
        h.max_debt   = std::max(h.max_debt, real);
    }

    h.links_expected = prev_seq + 1;
    h.gaps           = h.links_seen < h.links_expected;

    if (!h.gaps) {
        h.growth_without_redemption =
            h.issued > 1e-9 && h.redeemed < 1e-9 && issues >= 2;
        h.thread_inconsistent =
            std::abs(h.outstanding - (h.issued - h.redeemed)) > 1e-6;
    }
    return h;
}

} // namespace records
