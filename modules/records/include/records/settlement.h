#pragma once

// When a deal happens and what pays it (economy.md §4.4, records.md §11.1 v6,
// records.md §11.13) — the rules every reader applies the same way, client and
// aggregator alike, so that they never disagree about whose hours moved.
//
// A deal the buyer signs is an OFFER. Hours move only when the seller accepts
// it with a DealReply written in the purse the deal pays into, by the deal's
// own term `until`; a refusal, or silence past the term, and nothing happened.
// So a payment never hangs: at most until the term both sides agreed to.
//
// Paper someone else issued is spent from a BILL — the block it came to the
// holder in (a deal that paid him, or his own move between branches). Every
// spend names its bill and the holder's previous spend of it, so the hours of
// one bill form a thread in the holder's purse, and every hour traces back hop
// by hop to the self-issue it was born in. A spend the bill cannot cover, or
// one that breaks the thread, is not paper at all: the deal it pays is
// unbacked and moves nothing.

#include "records/types.h"

#include <array>
#include <cstdint>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace records {

// ── The seller's answer ──────────────────────────────────────────────────────

enum class Answer { none, accepted, refused };

// One DealReply as a reader found it: the record and where its block lies.
struct ObservedReply {
    DealReply               reply;
    std::array<uint8_t, 32> author{};   // the chain that wrote the block
    uint32_t                node  = 0;  // the branch it lies in
    uint64_t                index = 0;  // its position in that branch
};

struct DealAnswer {
    Answer                     answer = Answer::none;
    std::optional<std::size_t> reply;   // which of the given replies counted
};

// What the seller answered to `deal` (whose block is `deal_ref`): the first
// reply in the purse the deal pays into — the seller's chain, branch
// `deal.to_node` — given by the deal's term. A reply by anyone else, from
// another branch, about another deal, or past the term does not count.
DealAnswer answer_of(const Transfer& deal, const Ref& deal_ref,
                     const std::vector<ObservedReply>& replies);

// A deal's standing at time `now`: answered, or still waiting, or dead of
// silence. Expired and refused alike: the deal did not happen.
enum class DealStatus { pending, accepted, refused, expired };
DealStatus deal_status(const Transfer& deal, Answer answer, int64_t now) noexcept;
const char* deal_status_name(DealStatus s) noexcept;

// A buyback on the pre-signed return (records.md §9.8 v5) needs no answer when
// it pays only with paper the holder agreed to in advance by signing that
// return: his own — it comes home and annihilates his debt — and the
// addressee's self-issue: he knew whom he signed the return to. Any other paper
// waits for his answer like any deal; refusing it blocks nothing, because the
// addressee can always pay with own paper.
bool pre_accepted_buyback(const Transfer& deal) noexcept;

// ── Bills (векселя) ──────────────────────────────────────────────────────────

// How many hours of `issuer` the block `src` put into its receiver's hands.
double bill_credit(const Transfer& src, const std::array<uint8_t, 32>& issuer) noexcept;

// A Redemption naming the author's OWN deal is a reversal: that deal was
// refused or let lapse, so the paper it self-issued was never born (records.md
// §11.5 v2). It closes the emission link; it repays nothing.
bool is_reversal(const Redemption& rd, const std::array<uint8_t, 32>& author) noexcept;

// One spend of a bill as a reader found it.
struct BillSpend {
    std::array<uint8_t, 32> hash{};       // the spending block (a deal or a move)
    uint32_t                node  = 0;    // its branch
    uint64_t                index = 0;    // its position in that branch
    int64_t                 timestamp = 0;// the transfer's own
    double                  units = 0;    // the portion it took from the bill
    std::optional<Ref>      prev;         // the previous spend, as written
    // How the spend ended. A move always stands. A deal stands unless refused,
    // or unanswered with its term before the holder's next spend of the bill —
    // he paid again after the term, the earlier offer was dead. An offer still
    // waiting holds its hours: they are not the holder's to spend twice.
    bool                    move   = false;
    Answer                  answer = Answer::none;
    int64_t                 until  = 0;
    // Whether the spend could hold hours at all, as far as the reader knows: a
    // deal that completes nothing (not addressed) or one whose other paper is no
    // paper moves nothing, so it holds nothing either.
    bool                    stands = true;
};

struct BillState {
    double                            credited = 0;
    double                            consumed = 0;   // gone for good, and held by live offers
    std::set<std::array<uint8_t, 32>> backed;         // spends the bill covers
    std::set<std::array<uint8_t, 32>> unbacked;       // from the first break on
    std::string                       fault;          // why the first break is one
    double remaining() const noexcept { return credited - consumed; }
};

// One bill: `credited` hours that came to `holder`'s branch `purse`; when they
// came by a deal, the holder's acceptance lies at position `arrived` of that
// branch and nothing before it may spend them (a move: available at once).
// Spends go in branch order; each must lie in the purse, after the arrival,
// name the spend before it (the first names none), and fit what is left. From
// the first one that does not, every spend is unbacked: the thread is broken
// there — a hidden spend, a lie, or hours the holder never had. Whether a spend
// fits depends only on the spends before it, so a reader may pass a prefix.
BillState check_bill(const std::array<uint8_t, 32>& holder, uint32_t purse, double credited,
                     std::optional<uint64_t> arrived, std::vector<BillSpend> spends);

// The same rule, spend by spend, for a reader who learns how each spend ended
// only as it goes (the aggregator: whether a deal stands depends on other
// bills). Feed the spends in branch order: judge() says whether the next one
// is backed — on what came before it alone; settle() then records how it ended,
// i.e. whether it holds its hours (BillSpend::stands, the answer, the term
// against the holder's next spend of the bill). check_bill is exactly this loop.
class BillRun {
public:
    BillRun(const std::array<uint8_t, 32>& holder, uint32_t purse, double credited,
            std::optional<uint64_t> arrived);
    bool judge(const BillSpend& s);
    void settle(const BillSpend& s, std::optional<int64_t> next_timestamp);
    const BillState& state() const noexcept { return st_; }

private:
    std::array<uint8_t, 32>                holder_;
    uint32_t                               purse_;
    std::optional<uint64_t>                arrived_;
    std::optional<std::array<uint8_t, 32>> last_;   // the previous spend on the thread
    bool                                   broken_ = false;
    BillState                              st_;
};

} // namespace records
