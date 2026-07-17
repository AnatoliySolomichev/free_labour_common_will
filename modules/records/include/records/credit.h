#pragma once
// Layer-1 credit history (ИР-010, records.md §11.6): what a chain's emission
// thread (economy.md §4.3) tells about it before trusting any third party.
// Pure computation: the caller extracts observed links from whatever blocks
// of the subject's chain it holds locally. A partial view is legitimate and
// is reported as such (`gaps`) — never presented as the full picture.

#include "records/types.h"

#include <cstdint>
#include <optional>
#include <vector>

namespace records {

// One emission-thread link as seen in a block of the subject's chain:
// a self-issuing Transfer ("−", hours borrowed) or a Redemption ("+").
struct ObservedLink {
    EmissionLink            link;
    double                  units = 0;      // hours self-issued or redeemed
    bool                    is_redemption = false;
    int64_t                 timestamp = 0;  // record's claimed timestamp
    std::array<uint8_t, 32> block_hash{};   // carrier block (equivocation evidence)
};

struct CreditHistory {
    // Sums over the seen part of the thread.
    double issued   = 0;
    double redeemed = 0;

    // Declared debt at the newest seen link (positive = hours owed).
    double outstanding = 0;

    double max_debt   = 0;  // deepest declared debt ever seen
    double max_repaid = 0;  // largest debt drawdown ever repaid («доверили — вернул»)

    uint64_t links_seen     = 0;  // unique seq values observed
    uint64_t links_expected = 0;  // newest seq + 1
    bool     gaps           = false;  // partial local view — honesty first

    // Set only on a complete view: with gaps these would slander an honest
    // chain whose redemptions simply have not reached us yet.
    bool growth_without_redemption = false;  // ≥2 issues, zero redemptions
    bool thread_inconsistent       = false;  // declared debt ≠ issued − redeemed

    std::optional<int64_t> last_selfissue_ts;
    std::optional<int64_t> last_redemption_ts;

    // seq values carried by two different blocks — objective equivocation
    // proof (economy.md §4.3), no further trust or context needed.
    std::vector<uint64_t> equivocated_seqs;
};

// Digest observed links (any order; the same block seen twice is tolerated).
CreditHistory credit_history(std::vector<ObservedLink> links);

} // namespace records
