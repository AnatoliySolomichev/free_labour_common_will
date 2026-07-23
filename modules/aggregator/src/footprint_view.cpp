#include "aggregator/footprint_view.h"

#include <records/codec.h>
#include <records/types.h>

#include <algorithm>
#include <set>

namespace aggregator {

namespace {

// Net holdings of one issuer's paper: holder chain → units still held.
using Holdings = std::map<UserId, double>;

} // namespace

FootprintView FootprintView::build(const AggregatorStorage& storage) {
    FootprintView view;

    // issuer → (holder → units). Self-issue creates paper in the receiver's
    // hands, endorsement moves it, redemption annihilates it (§11.1).
    std::map<UserId, Holdings> paper;
    // Undirected exchange graph: chain → (counterparty → volume).
    std::map<UserId, std::map<UserId, double>> adj;
    // Layer-2 tallies keyed by the chain they describe.
    std::map<UserId, std::set<UserId>> redeemer_sets, acceptor_sets;
    std::map<UserId, double>           redeemed_to_others, labor_outward;

    for (const Hash& bh : storage.all_block_hashes()) {
        const auto block = storage.get_block_by_hash(bh);
        if (!block || block->type != BlockType::DATA) continue;
        const UserId owner = block->address.user_id;

        records::Record rec;
        try {
            rec = records::Codec::decode(block->payload.data(),
                                         block->payload.size());
        } catch (const records::CodecError&) {
            continue;
        }

        if (const auto* t = std::get_if<records::Transfer>(&rec)) {
            if (t->from != owner.bytes) continue;      // spoofed sender
            UserId from{}, to{};
            from.bytes = t->from;
            to.bytes   = t->to;
            if (from == to) continue;                  // self-move: no signal

            double total = 0;
            for (const auto& o : t->origins) {
                total += o.units;
                UserId issuer{};
                issuer.bytes = o.issuer;

                if (o.issuer == t->to) {
                    // Redemption: the payer returns the receiver's own paper,
                    // and it dies. Someone worked to earn it back — the dearest
                    // signal a chain can leave (§12.10).
                    paper[issuer][from] -= o.units;
                    redeemer_sets[to].insert(from);
                    redeemed_to_others[to] += o.units;
                } else {
                    // Self-issue (issuer == from) or endorsement: the receiver
                    // ends up holding issuer's paper; an endorser gives it up.
                    if (o.issuer != t->from) paper[issuer][from] -= o.units;
                    paper[issuer][to] += o.units;
                }
            }
            adj[from][to] += total;
            adj[to][from] += total;
        } else if (const auto* a = std::get_if<records::Acceptance>(&rec)) {
            if (a->receiver != owner.bytes) continue;  // spoofed receiver
            UserId worker{};
            worker.bytes = a->work.chain;
            if (worker == owner) continue;             // self-deal: no signal
            acceptor_sets[worker].insert(owner);
            labor_outward[worker] += a->labor_units;
        }
    }

    // Every chain that left any trace gets a footprint.
    std::set<UserId> subjects;
    for (const auto& [issuer, holders] : paper) {
        subjects.insert(issuer);
        for (const auto& [holder, units] : holders) {
            (void)units;
            subjects.insert(holder);
        }
    }
    for (const auto& [chain, peers] : adj) { (void)peers; subjects.insert(chain); }
    for (const auto& [chain, s] : acceptor_sets) { (void)s; subjects.insert(chain); }

    for (const UserId& subject : subjects) {
        ChainFootprint fp{};

        if (const auto it = paper.find(subject); it != paper.end())
            for (const auto& [holder, units] : it->second) {
                if (units <= 1e-9 || holder == subject) continue;
                fp.holders.push_back({holder, units});
                fp.paper_outstanding += units;
            }
        std::sort(fp.holders.begin(), fp.holders.end(),
                  [](const PaperHolder& a, const PaperHolder& b) {
                      return a.units != b.units ? a.units > b.units
                                                : a.chain < b.chain;
                  });

        if (const auto it = redeemer_sets.find(subject); it != redeemer_sets.end())
            fp.redeemers = it->second.size();
        if (const auto it = redeemed_to_others.find(subject);
            it != redeemed_to_others.end())
            fp.redeemed_to_others = it->second;
        if (const auto it = acceptor_sets.find(subject); it != acceptor_sets.end())
            fp.acceptors = it->second.size();
        if (const auto it = labor_outward.find(subject); it != labor_outward.end())
            fp.labor_outward = it->second;

        // Layer 3: the subject's circle is everything within TWO exchange hops
        // — the same reachability the trust footprint uses elsewhere (ИР-010:
        // "1 шаг — прямой контрагент, 2 шага — контрагент контрагента"). One
        // hop is too tight: a farm arranged in a ring would show half its own
        // trade as "outward" simply because the far side sits 2 hops away.
        std::set<UserId> circle{subject};
        if (const auto it = adj.find(subject); it != adj.end()) {
            fp.counterparties = it->second.size();
            for (const auto& [peer, vol] : it->second) { (void)vol; circle.insert(peer); }
        }
        for (const UserId& direct : std::set<UserId>(circle)) {
            const auto it = adj.find(direct);
            if (it == adj.end()) continue;
            for (const auto& [peer, vol] : it->second) { (void)vol; circle.insert(peer); }
        }
        double inside_twice = 0, crossing = 0;
        for (const UserId& member : circle) {
            const auto it = adj.find(member);
            if (it == adj.end()) continue;
            for (const auto& [peer, vol] : it->second) {
                if (circle.count(peer)) inside_twice += vol;   // seen from both ends
                else                    crossing     += vol;
            }
        }
        fp.internal_turnover = inside_twice / 2.0;
        fp.boundary_turnover = crossing;

        view.chains_[subject] = std::move(fp);
    }
    return view;
}

std::optional<ChainFootprint> FootprintView::chain(const UserId& uid) const {
    auto it = chains_.find(uid);
    if (it == chains_.end()) return std::nullopt;
    return it->second;
}

} // namespace aggregator
