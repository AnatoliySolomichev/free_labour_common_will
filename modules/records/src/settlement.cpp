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
    // word in the same branch cannot be earlier than the first.
    if (!deal.until || replies[*first].reply.timestamp > *deal.until) return out;
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

double bill_credit(const Transfer& src, const std::array<uint8_t, 32>& issuer) noexcept {
    double units = 0.0;
    for (const auto& o : src.origins)
        if (o.issuer == issuer) units += o.units;
    return units;
}

bool is_reversal(const Redemption& rd, const std::array<uint8_t, 32>& author) noexcept {
    return rd.transfer.chain == author;
}

BillState check_bill(const std::array<uint8_t, 32>& holder, uint32_t purse, double credited,
                     std::optional<uint64_t> arrived, std::vector<BillSpend> spends) {
    BillState st;
    st.credited = credited;
    std::sort(spends.begin(), spends.end(), [](const BillSpend& a, const BillSpend& b) {
        return a.index != b.index ? a.index < b.index : a.hash < b.hash;
    });
    // The same block can reach a reader twice (a merge and a fetch).
    spends.erase(std::unique(spends.begin(), spends.end(),
                             [](const BillSpend& a, const BillSpend& b) { return a.hash == b.hash; }),
                 spends.end());

    std::optional<std::array<uint8_t, 32>> last;   // the previous spend on the thread
    bool broken = false;
    for (std::size_t i = 0; i < spends.size(); ++i) {
        const BillSpend& s = spends[i];
        if (broken) {
            st.unbacked.insert(s.hash);
            continue;
        }
        const char* fault = nullptr;
        if (s.node != purse) {
            fault = "вексель потрачен не из того кошелька, куда пришёл";
        } else if (arrived && s.index <= *arrived) {
            fault = "вексель потрачен раньше, чем принят";
        } else if (!last) {
            if (s.prev) fault = "первая трата векселя называет предыдущую, которой нет";
        } else if (!s.prev || s.prev->chain != holder || s.prev->hash != *last) {
            fault = "нить векселя порвана: трата называет не предыдущую трату";
        }
        if (!fault && s.units > st.remaining() + 1e-9)
            fault = "трата больше остатка векселя";
        if (fault) {
            broken   = true;
            st.fault = fault;
            st.unbacked.insert(s.hash);
            continue;
        }
        st.backed.insert(s.hash);
        last = s.hash;
        // An offer refused, or left unanswered past its term by the time the
        // holder spent the bill again, held nothing: its hours are back. Nor does
        // a spend that moves nothing at all.
        bool dead = !s.stands;
        if (!dead && !s.move) {
            if (s.answer == Answer::refused)
                dead = true;
            else if (s.answer == Answer::none && i + 1 < spends.size() &&
                     spends[i + 1].timestamp > s.until)
                dead = true;
        }
        if (!dead) st.consumed += s.units;
    }
    return st;
}

} // namespace records
