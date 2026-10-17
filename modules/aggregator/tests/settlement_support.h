#pragma once
// Test support (economy.md §4.4, records.md §11.13): a deal is an offer until
// its seller accepts it by its term, and paper someone else issued is spent from
// bills. Fixtures write deals the way they always have; each fixture's add()
// passes records through here, and this completes them the way the clients do:
//
//   stamp()  — before the record is encoded: a deal gets a term and the
//              seller's purse (the fixtures' one branch); a portion of someone
//              else's paper written without a bill is taken from the payer's
//              bills of that issuer, oldest first, each naming the spend before.
//              Paper the payer does not hold gets a bill that does not exist —
//              exactly what a forger would have to write;
//   placed() — after the block is added: the payer's bills shrink, and the
//              seller's acceptance comes back to be written in his branch —
//              unless the test holds answers back to write them itself; the
//              seller's new bills are booked.
//
// A record written with its own term, purse or bills is left as written — that
// is how a test breaks the rules on purpose.

#include <records/types.h>

#include <array>
#include <cstdint>
#include <deque>
#include <map>
#include <optional>
#include <utility>
#include <variant>
#include <vector>

class Settlement {
public:
    static constexpr uint32_t kLeaf = 0x7FFF'FFFFu;   // the fixtures' one branch per chain
    // Far past any date the fixtures write: an answer is never later than its
    // branch, and the fixtures date works by the calendar but deals near zero.
    static constexpr int64_t  kTerm = 100LL * 365 * 86'400;

    // Hold the sellers' answers back: deals stay offers until a test answers.
    void hold_answers(bool hold = true) { hold_ = hold; }

    records::Record stamp(const std::array<uint8_t, 32>& author,
                          const records::Record& rec) const {
        records::Record out = rec;
        auto* t = std::get_if<records::Transfer>(&out);
        if (!t) return out;
        if (t->accept) {
            if (!t->until) t->until = t->timestamp + kTerm;
            if (t->to_node == 0) t->to_node = kLeaf;
        }
        std::vector<records::OriginQty> portions;
        for (const auto& o : t->origins) {
            if (o.issuer == t->from || o.src) {
                portions.push_back(o);
                continue;
            }
            double need = o.units;
            const auto mine = bills_.find(author);
            if (mine != bills_.end())
                if (const auto it = mine->second.find(o.issuer); it != mine->second.end())
                    for (const auto& b : it->second) {
                        if (need <= 1e-9) break;
                        if (b.remaining <= 1e-9) continue;
                        const double take = std::min(need, b.remaining);
                        records::OriginQty q{o.issuer, take, b.src, std::nullopt};
                        if (b.last) q.prev = records::Ref{author, *b.last};
                        portions.push_back(q);
                        need -= take;
                    }
            if (need > 1e-9) {
                // Paper the payer never had: the only bill he can name is one
                // that does not exist.
                records::Ref none{};
                none.chain = author;
                none.hash.fill(0xF0);
                none.hash[0] = static_cast<uint8_t>(++forged_);
                portions.push_back({o.issuer, need, none, std::nullopt});
            }
        }
        t->origins = std::move(portions);
        return out;
    }

    // The seller's answer to write, if any: (seller, DealReply).
    std::optional<std::pair<std::array<uint8_t, 32>, records::Record>>
    placed(const std::array<uint8_t, 32>& author, const records::Record& rec,
           const std::array<uint8_t, 32>& block_hash) {
        const auto* t = std::get_if<records::Transfer>(&rec);
        if (!t || t->from != author) return std::nullopt;
        for (const auto& o : t->origins) {
            if (!o.src) continue;
            auto& list = bills_[author][o.issuer];
            for (auto& b : list)
                if (b.src == *o.src) {
                    b.remaining -= o.units;
                    b.last = block_hash;
                }
        }
        if (!t->accept) return std::nullopt;
        // The seller holds what he was paid with — all but his own paper, which
        // comes home and dies.
        const records::Ref deal{author, block_hash};
        for (const auto& o : t->origins)
            if (o.issuer != t->to)
                bills_[t->to][o.issuer].push_back(Bill{deal, o.units, std::nullopt});
        if (hold_) return std::nullopt;
        records::DealReply r{};
        r.deal      = deal;
        r.accepted  = true;
        r.timestamp = t->timestamp;
        return std::make_pair(t->to, records::Record{r});
    }

private:
    struct Bill {
        records::Ref                           src;
        double                                 remaining = 0;
        std::optional<std::array<uint8_t, 32>> last;   // the holder's last spend of it
    };
    std::map<std::array<uint8_t, 32>, std::map<std::array<uint8_t, 32>, std::deque<Bill>>> bills_;
    bool            hold_   = false;
    mutable uint8_t forged_ = 0;
};
