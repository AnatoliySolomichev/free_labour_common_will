#include "aggregator/economy_view.h"

#include <records/codec.h>
#include <records/types.h>

#include <algorithm>
#include <set>

namespace aggregator {

namespace {

// Ref-keyed maps: 32-byte block hash of the referenced record.
using RefHash = std::array<uint8_t, 32>;

struct PledgeState {
    UserId  pledger{};
    RefHash target{};
    double  units   = 0;
    double  settled = 0;
    bool    revoked = false;
    std::optional<int64_t> expires;
};

} // namespace

EconomyView EconomyView::build(const AggregatorStorage& storage, int64_t now) {
    EconomyView view;

    std::map<RefHash, std::string>  concept_texts;
    std::map<RefHash, std::size_t>  copies;
    std::map<RefHash, int64_t>      reactions;
    std::map<RefHash, PledgeState>  pledges;       // pledge block hash → state
    std::map<RefHash, double>       settlements;   // reason hash → paid total
    // Blocks are scanned in hash order, so a revoke may precede its pledge —
    // collect them and apply after the scan.
    std::vector<std::pair<UserId, RefHash>> revokes;

    // Single pass over every known block. Transfers/pledges count only when
    // the record's author field matches the chain that signed the block —
    // the same spoof guard the CLI wallet applies.
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

        if (const auto* c = std::get_if<records::Concept>(&rec)) {
            concept_texts[bh.bytes] = c->text;
        } else if (const auto* cp = std::get_if<records::Copy>(&rec)) {
            ++copies[cp->source.hash];
        } else if (const auto* rx = std::get_if<records::Reaction>(&rec)) {
            reactions[rx->target.hash] += rx->value;
        } else if (const auto* p = std::get_if<records::Pledge>(&rec)) {
            PledgeState st;
            st.pledger = owner;
            st.target  = p->target.hash;
            st.units   = p->units;
            st.expires = p->expires;
            pledges[bh.bytes] = st;
        } else if (const auto* pr = std::get_if<records::PledgeRevoke>(&rec)) {
            if (pr->pledge.chain == owner.bytes)
                revokes.emplace_back(owner, pr->pledge.hash);
        } else if (const auto* t = std::get_if<records::Transfer>(&rec)) {
            if (t->from != owner.bytes) continue;   // spoofed sender
            double total = 0;
            for (const auto& o : t->origins) {
                total += o.units;
                if (o.issuer == t->from) view.chains_[owner].issued += o.units;
                if (o.issuer == t->to) {
                    UserId to{};
                    to.bytes = t->to;
                    view.chains_[to].redeemed += o.units;
                }
            }
            view.chains_[owner].spent += total;
            UserId to{};
            to.bytes = t->to;
            view.chains_[to].received += total;
            if (t->reason) settlements[t->reason->hash] += total;
        } else if (const auto* a = std::get_if<records::Acceptance>(&rec)) {
            UserId worker{};
            worker.bytes = a->work.chain;
            ++view.chains_[worker].works_accepted;
            view.chains_[worker].labor_appraised += a->labor_units;
        }
    }

    for (const auto& [owner, pledge_hash] : revokes) {
        auto it = pledges.find(pledge_hash);
        if (it != pledges.end() && it->second.pledger == owner)
            it->second.revoked = true;
    }

    // Resolve pledge statuses and fold them into ideas and chains.
    std::map<RefHash, IdeaFunding>       ideas;
    std::map<RefHash, std::set<UserId>>  pledger_sets;
    // A pledge's settlement = transfers whose reason points at the pledge.
    for (auto& [hash, st] : pledges) {
        st.settled = 0;
        auto s = settlements.find(hash);
        if (s != settlements.end()) st.settled = s->second;

        auto& idea = ideas[st.target];
        idea.idea_hash.bytes = st.target;
        auto& chain = view.chains_[st.pledger];
        pledger_sets[st.target].insert(st.pledger);

        const double settled = std::min(st.settled, st.units);
        idea.pledged_settled += settled;
        if (st.settled + 1e-9 >= st.units) ++chain.pledges_settled;
        else if (st.revoked)               ++chain.pledges_revoked;
        else if (st.expires && *st.expires < now) ++chain.pledges_expired;
        else {
            idea.pledged_active += st.units - settled;
            ++chain.pledges_active;
        }
    }
    for (const auto& [target, count] : copies) {
        auto& idea = ideas[target];
        idea.idea_hash.bytes = target;
        idea.copies = count;
    }
    for (const auto& [target, sum] : reactions) {
        auto& idea = ideas[target];
        idea.idea_hash.bytes = target;
        idea.reaction_sum = sum;
    }

    for (auto& [target, idea] : ideas) {
        auto text = concept_texts.find(target);
        if (text != concept_texts.end()) idea.text = text->second;
        auto ps = pledger_sets.find(target);
        if (ps != pledger_sets.end()) idea.pledgers = ps->second.size();
        view.ideas_.push_back(std::move(idea));
    }
    std::sort(view.ideas_.begin(), view.ideas_.end(),
              [](const IdeaFunding& a, const IdeaFunding& b) {
                  return a.pledged_active > b.pledged_active;
              });
    return view;
}

std::optional<ChainEconomy> EconomyView::chain(const UserId& uid) const {
    auto it = chains_.find(uid);
    if (it == chains_.end()) return std::nullopt;
    return it->second;
}

} // namespace aggregator
