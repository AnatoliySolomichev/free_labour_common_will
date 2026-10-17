#include "records/settlement.h"

#include <algorithm>

namespace records {

DealAnswer answer_of(const Transfer& deal, const Ref& deal_ref,
                     const std::vector<ObservedReply>& replies) {
    DealAnswer out;
    // Only the purse being paid answers, and its first word is final: a branch
    // is a line, so «first» needs no clock.
    std::optional<std::size_t> first;
    for (std::size_t i = 0; i < replies.size(); ++i) {
        const auto& r = replies[i];
        if (!(r.reply.deal == deal_ref)) continue;
        if (r.author != deal.to || r.node != deal.to_node) continue;
        if (!first || r.index < replies[*first].index) first = i;
    }
    if (!first) return out;
    // Past the term the offer is dead: a late word answers nothing — and a later
    // word in the same branch cannot be earlier than the first. Its time is its
    // own date or the latest date of the branch before it, whichever is later.
    const auto& r = replies[*first];
    if (!deal.until || std::max(r.reply.timestamp, r.since) > *deal.until) return out;
    out.answer = replies[*first].reply.accepted ? Answer::accepted : Answer::refused;
    out.reply  = first;
    return out;
}

DealStatus deal_status(const Transfer& deal, Answer answer, int64_t now) noexcept {
    if (answer == Answer::accepted) return DealStatus::accepted;
    if (answer == Answer::refused)  return DealStatus::refused;
    if (deal.until && now > *deal.until) return DealStatus::expired;
    return DealStatus::pending;
}

const char* deal_status_name(DealStatus s) noexcept {
    switch (s) {
        case DealStatus::pending:  return "pending";
        case DealStatus::accepted: return "accepted";
        case DealStatus::refused:  return "refused";
        case DealStatus::expired:  return "expired";
    }
    return "pending";
}

bool pre_accepted_buyback(const Transfer& deal) noexcept {
    if (deal.origins.empty()) return false;
    for (const auto& o : deal.origins)
        if (o.issuer != deal.to && o.issuer != deal.from) return false;
    return true;
}

bool pre_accepted_buyback(const Transfer& deal, const Ref& purchase_ref,
                          const Transfer& purchase) noexcept {
    const bool deposit = purchase.accept && purchase.accept->buyback &&
                         purchase.accept->buyback->deposit;
    if (!deposit) return pre_accepted_buyback(deal);
    if (deal.origins.empty()) return false;
    // The part of the payment that was the seller's own paper came home and died
    // with his debt — no bill holds it: it goes back as his own paper again, no
    // more than it was.
    double own = 0.0;
    for (const auto& o : deal.origins) {
        if (o.issuer == deal.to || (o.src && *o.src == purchase_ref)) continue;
        if (o.issuer != deal.from || o.src) return false;
        own += o.units;
    }
    return own <= bill_credit(purchase, deal.from) + 1e-9;
}

bool deposit_frozen(const Transfer& purchase, int64_t spend_timestamp,
                    bool bought_back) noexcept {
    if (!purchase.accept || !purchase.accept->buyback || !purchase.accept->buyback->deposit)
        return false;
    return !bought_back && spend_timestamp <= purchase.accept->buyback->until;
}

double bill_credit(const Transfer& src, const std::array<uint8_t, 32>& issuer) noexcept {
    double units = 0.0;
    for (const auto& o : src.origins)
        if (o.issuer == issuer) units += o.units;
    return units;
}

bool is_reversal(const Redemption& rd, const std::array<uint8_t, 32>& author) noexcept {
    return rd.transfer.chain == author;
}

BillRun::BillRun(const std::array<uint8_t, 32>& holder, uint32_t purse, double credited,
                 std::optional<uint64_t> arrived)
    : holder_(holder), purse_(purse), arrived_(arrived) {
    st_.credited = credited;
}

bool BillRun::judge(const BillSpend& s) {
    if (broken_) {
        st_.unbacked.insert(s.hash);
        return false;
    }
    const char* fault = nullptr;
    if (s.frozen) {
        fault = "залог заморожен до срока возврата: первым его тратит выкуп";
    } else if (s.node != purse_) {
        fault = "вексель потрачен не из того кошелька, куда пришёл";
    } else if (arrived_ && s.index <= *arrived_) {
        fault = "вексель потрачен раньше, чем принят";
    } else if (!last_) {
        if (s.prev) fault = "первая трата векселя называет предыдущую, которой нет";
    } else if (!s.prev || s.prev->chain != holder_ || s.prev->hash != *last_) {
        fault = "нить векселя порвана: трата называет не предыдущую трату";
    }
    if (!fault && s.units > st_.remaining() + 1e-9)
        fault = "трата больше остатка векселя";
    if (fault) {
        broken_   = true;
        st_.fault = fault;
        st_.unbacked.insert(s.hash);
        return false;
    }
    st_.backed.insert(s.hash);
    last_ = s.hash;
    return true;
}

void BillRun::settle(const BillSpend& s, std::optional<int64_t> next_timestamp) {
    if (!st_.backed.count(s.hash)) return;   // a spend past the break holds nothing
    // An offer refused, or left unanswered past its term by the time the holder
    // spent the bill again, held nothing: its hours are back. Nor does a spend
    // that moves nothing at all.
    bool dead = !s.stands;
    if (!dead && !s.move) {
        if (s.answer == Answer::refused)
            dead = true;
        else if (s.answer == Answer::none && next_timestamp && *next_timestamp > s.until)
            dead = true;
    }
    if (!dead) st_.consumed += s.units;
}

BillState check_bill(const std::array<uint8_t, 32>& holder, uint32_t purse, double credited,
                     std::optional<uint64_t> arrived, std::vector<BillSpend> spends) {
    std::sort(spends.begin(), spends.end(), [](const BillSpend& a, const BillSpend& b) {
        return a.index != b.index ? a.index < b.index : a.hash < b.hash;
    });
    // The same block can reach a reader twice (a merge and a fetch).
    spends.erase(std::unique(spends.begin(), spends.end(),
                             [](const BillSpend& a, const BillSpend& b) { return a.hash == b.hash; }),
                 spends.end());
    BillRun run(holder, purse, credited, arrived);
    for (std::size_t i = 0; i < spends.size(); ++i) {
        if (!run.judge(spends[i])) continue;
        run.settle(spends[i], i + 1 < spends.size()
                                  ? std::optional<int64_t>(spends[i + 1].timestamp)
                                  : std::nullopt);
    }
    return run.state();
}

} // namespace records
