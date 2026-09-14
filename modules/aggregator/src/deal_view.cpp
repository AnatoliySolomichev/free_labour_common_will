#include "aggregator/deal_view.h"
#include "aggregator/recognition.h"

#include <records/codec.h>
#include <records/types.h>

#include <algorithm>
#include <map>
#include <set>

namespace aggregator {

namespace {

// The deal vocabulary (records.md §8.6) — ordinary ConceptLink kinds.
constexpr const char* TAKE_KIND    = "берусь";     // my skill    → your need
constexpr const char* PERFORM_KIND = "исполняет";  // my work     → your need
constexpr const char* CLOSE_KIND   = "закрыто";    // my accept.  → my need

using RefHash = std::array<uint8_t, 32>;
using NeedKey = std::pair<UserId, RefHash>;

struct WorkInfo {
    UserId                owner{};
    std::string           action;
    double                hours = 0;
    std::optional<UserId> customer;   // records.md §9.4 v3
};

// A deal paying for a work: the Transfer that carries its acceptance
// (records.md §9.5, ИР-023) — appraisal and payment in one record.
struct AcceptInfo {
    UserId  owner{};      // the customer: accepted and paid (sender, spoof-guarded)
    RefHash work{};       // accepted WorkRecord's block hash
    double  labor_units = 0;
    double  carried     = 0;  // carried cost of tools/materials (records.md §9.5 v2)
    double  paid        = 0;  // = labor_units + carried, by construction
};

} // namespace

double Deal::appraised() const noexcept {
    // Payable = live labor + carried cost (records.md §9.5 v2) — what the deal
    // record itself pays.
    double total = 0;
    for (const auto& w : works)
        if (w.accepted) total += w.labor_units + w.carried;
    return total;
}

double Deal::paid() const noexcept {
    double total = 0;
    for (const auto& w : works) total += w.paid;
    return total;
}

DealStage Deal::stage() const noexcept {
    if (closed) return DealStage::Closed;
    // Accepted is paid: the acceptance rides inside its payment (records.md §9.5).
    for (const auto& w : works)
        if (w.accepted) return DealStage::Paid;
    if (!works.empty()) return DealStage::Worked;
    for (const auto& p : pledges)
        if (p.active()) return DealStage::Hired;
    if (!takers.empty()) return DealStage::Taken;
    return DealStage::Open;
}

const char* DealView::stage_name(DealStage stage) noexcept {
    switch (stage) {
        case DealStage::Open:     return "открыта";
        case DealStage::Taken:    return "есть желающие";
        case DealStage::Hired:    return "нанят";
        case DealStage::Worked:   return "работа сделана";
        case DealStage::Paid:     return "оплачено";
        case DealStage::Closed:   return "закрыта";
    }
    return "?";
}

DealView DealView::build(const AggregatorStorage& storage) {
    DealView view;

    // Pass 1: profiles give the needs and their closed flags (records.md §8.6 rules,
    // including the acceptance-based close — its to.chain == owner holds).
    const auto profiles = ProfileView::build(storage);
    // Which deals happened (records.md §9.5, ИР-023): a deal the seller did not
    // address is void — it neither accepts, pays nor settles a pledge.
    const auto deals = read_deals(storage);

    // Pass 2: everything that points at a need, plus lookup tables.
    std::map<NeedKey, std::vector<DealTaker>> takers;
    std::map<NeedKey, std::vector<RefHash>>   performs;      // need → work hashes
    std::map<RefHash, WorkInfo>               work_infos;    // work hash → info
    std::map<RefHash, AcceptInfo>             accept_infos;  // deal hash → info
    std::map<RefHash, std::size_t>            unaddressed_of_work;   // work hash → void deals
    std::map<RefHash, double>                 settlements;   // pledge hash → settled
    struct RawPledge {
        UserId pledger{}; RefHash hash{}; NeedKey target;
        double units = 0; std::optional<UserId> executor; bool revoked = false;
    };
    std::vector<RawPledge>                    raw_pledges;
    std::vector<std::pair<UserId, RefHash>>   revokes;

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

        if (const auto* link = std::get_if<records::ConceptLink>(&rec)) {
            UserId to_chain{};
            to_chain.bytes = link->to.chain;
            const NeedKey need{to_chain, link->to.hash};
            if (link->kind == TAKE_KIND && link->from.chain == owner.bytes) {
                // you volunteer with YOUR OWN skill
                takers[need].push_back(DealTaker{owner, Hash{link->from.hash}});
            } else if (link->kind == PERFORM_KIND &&
                       link->from.chain == owner.bytes) {
                // you attach YOUR OWN work — nobody pins someone else's labour
                // onto a need
                performs[need].push_back(link->from.hash);
            }
            // CLOSE_KIND is already folded into ProfileView's closed flag.
        } else if (const auto* w = std::get_if<records::WorkRecord>(&rec)) {
            WorkInfo wi{owner, w->action, w->hours, std::nullopt};
            if (w->customer) {
                UserId c{};
                c.bytes = *w->customer;
                wi.customer = c;
            }
            work_infos[bh.bytes] = std::move(wi);
        } else if (const auto* p = std::get_if<records::Pledge>(&rec)) {
            UserId target_chain{};
            target_chain.bytes = p->target.chain;
            RawPledge rp;
            rp.pledger = owner;
            rp.hash    = bh.bytes;
            rp.target  = NeedKey{target_chain, p->target.hash};
            rp.units   = p->units;
            if (p->executor) {
                UserId ex{};
                ex.bytes = *p->executor;
                rp.executor = ex;
            }
            raw_pledges.push_back(std::move(rp));
        } else if (const auto* pr = std::get_if<records::PledgeRevoke>(&rec)) {
            if (pr->pledge.chain == owner.bytes)
                revokes.emplace_back(owner, pr->pledge.hash);
        } else if (const auto* t = std::get_if<records::Transfer>(&rec)) {
            if (t->from != owner.bytes) continue;       // spoofed sender
            if (t->accept && !deals.moves_hours(bh.bytes)) {
                ++unaddressed_of_work[t->accept->work.hash];   // void: counted, never shown
                continue;
            }
            double total = 0;
            for (const auto& o : t->origins) total += o.units;
            if (t->accept)
                accept_infos[bh.bytes] =
                    AcceptInfo{owner, t->accept->work.hash, t->accept->labor_units,
                               t->accept->carried_units ? *t->accept->carried_units : 0.0,
                               total};
            if (t->settles) settlements[t->settles->hash] += total;
        }
    }

    for (const auto& [owner, pledge_hash] : revokes)
        for (auto& rp : raw_pledges)
            if (rp.hash == pledge_hash && rp.pledger == owner) rp.revoked = true;

    // Deals indexed by the work they pay for. One result — one counted deal
    // (records.md §9.5, ИР-023): the recognized deal is shown; a result the
    // seller gave to two buyers shows the first by hash. The others are counted,
    // not silently overwritten away.
    std::map<RefHash, RefHash>     acceptance_of_work;    // work hash → deal hash
    std::map<RefHash, std::size_t> acceptances_of_work;
    for (const auto& [ah, info] : accept_infos) {
        ++acceptances_of_work[info.work];
        if (deals.counts(ah) || !acceptance_of_work.count(info.work))
            acceptance_of_work[info.work] = ah;
    }

    auto attach_work = [&](Deal& deal, const RefHash& wh) {
        DealWork dw;
        dw.work_hash = Hash{wh};
        if (const auto wi = work_infos.find(wh); wi != work_infos.end()) {
            dw.worker   = wi->second.owner;
            dw.action   = wi->second.action;
            dw.hours    = wi->second.hours;
            dw.customer = wi->second.customer;
        }
        if (const auto aw = acceptance_of_work.find(wh);
            aw != acceptance_of_work.end()) {
            const AcceptInfo& ai = accept_infos.at(aw->second);
            dw.accepted        = true;
            dw.acceptor        = ai.owner;
            dw.acceptance_hash = Hash{aw->second};
            dw.labor_units     = ai.labor_units;
            dw.carried         = ai.carried;
            dw.paid            = ai.paid;
        }
        if (const auto n = acceptances_of_work.find(wh); n != acceptances_of_work.end())
            dw.acceptances = n->second;
        if (const auto u = unaddressed_of_work.find(wh); u != unaddressed_of_work.end())
            dw.unaddressed = u->second;
        dw.oversold = deals.recognition.oversold.count(wh) > 0;
        deal.works.push_back(std::move(dw));
    };

    // Assemble: one deal per posted need.
    std::set<RefHash> claimed_works;   // works attached to some need
    for (const auto& [uid, profile] : profiles.chains()) {
        for (const auto& fact : profile.facts) {
            if (fact.facet != ProfileFacet::Need) continue;

            Deal deal;
            deal.seeker    = uid;
            deal.need_hash = fact.block_hash;
            deal.slug      = fact.slug;
            deal.text      = fact.text;
            deal.urgency   = ProfileView::tag_value(fact.tags, "urgency:");
            deal.closed    = fact.closed;

            const NeedKey key{uid, fact.block_hash.bytes};
            if (const auto t = takers.find(key); t != takers.end())
                deal.takers = t->second;
            for (const auto& rp : raw_pledges) {
                if (!(rp.target == key)) continue;
                DealPledge dp;
                dp.pledger     = rp.pledger;
                dp.pledge_hash = Hash{rp.hash};
                dp.units       = rp.units;
                dp.executor    = rp.executor;
                dp.revoked     = rp.revoked;
                if (const auto s = settlements.find(rp.hash);
                    s != settlements.end())
                    dp.settled = s->second;
                deal.pledges.push_back(std::move(dp));
            }
            if (const auto p = performs.find(key); p != performs.end())
                for (const auto& wh : p->second) {
                    attach_work(deal, wh);
                    claimed_works.insert(wh);
                }
            view.deals_.push_back(std::move(deal));
        }
    }

    // Direct deals (case 2): accepted work linked to no need — somebody did a
    // job that was never posted. Anchored by the deal shown for the work; a work
    // paid for twice is still one job, one deal. A void deal anchors nothing.
    std::set<RefHash> direct_works;
    for (const auto& [ah, info] : accept_infos) {
        if (claimed_works.count(info.work)) continue;
        if (!direct_works.insert(info.work).second) continue;
        Deal deal;
        deal.seeker = accept_infos.at(acceptance_of_work.at(info.work)).owner;
        deal.text   = "(прямая сделка — потребность не постилась)";
        attach_work(deal, info.work);
        view.deals_.push_back(std::move(deal));
    }

    return view;
}

std::vector<const Deal*> DealView::for_chain(const UserId& uid) const {
    std::vector<const Deal*> out;
    for (const auto& deal : deals_) {
        bool mine = deal.seeker == uid;
        for (const auto& t : deal.takers) mine = mine || t.chain == uid;
        for (const auto& w : deal.works)  mine = mine || w.worker == uid;
        if (mine) out.push_back(&deal);
    }
    return out;
}

} // namespace aggregator
