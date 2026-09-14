#include "aggregator/recognition.h"

#include <records/codec.h>

#include <algorithm>

namespace aggregator {

DealsOnTable read_deals(const AggregatorStorage& storage) {
    DealsOnTable out;
    std::map<std::array<uint8_t, 32>, std::size_t>       release_at;   // release hash → index
    std::map<UserId, std::vector<records::ObservedWork>> observed;     // chain → its works
    for (const Hash& bh : storage.all_block_hashes()) {
        const auto block = storage.get_block_by_hash(bh);
        if (!block || block->type != BlockType::DATA) continue;
        const UserId owner = block->address.user_id;
        records::Record rec;
        try { rec = records::Codec::decode(block->payload.data(), block->payload.size()); }
        catch (const records::CodecError&) { continue; }

        if (const auto* w = std::get_if<records::WorkRecord>(&rec)) {
            out.works[bh.bytes] = WorkOnTable{owner, *w};
            observed[owner].push_back({bh.bytes, records::labor_interval(*w), w->thread});
        } else if (const auto* rl = std::get_if<records::Release>(&rec)) {
            if (rl->work.chain != owner.bytes) continue;   // only one's own work is released
            release_at[bh.bytes] = out.releases.size();
            out.releases.push_back(ReleaseEntry{bh.bytes, owner, *rl});
        } else if (const auto* wo = std::get_if<records::WriteOff>(&rec)) {
            if (wo->work.chain != owner.bytes) continue;   // only one's own lot is written off
            out.writeoffs.push_back(WriteOffEntry{bh.bytes, owner, *wo});
        } else if (const auto* t = std::get_if<records::Transfer>(&rec)) {
            if (!t->accept) continue;
            if (t->from != owner.bytes) continue;          // spoofed sender
            out.all.push_back(DealEntry{bh.bytes, owner, *t});
        }
    }

    // Each chain's goods thread (records.md §9.4 v4): its links and its hours.
    for (const auto& [chain, works] : observed)
        out.threads[chain] = records::check_labor_thread(chain.bytes, works);

    // Who the seller named (records.md §9.5), and which releases a deal
    // completed. Blocks come in hash order, so the seller's records are matched
    // to the deals only after the whole scan.
    std::vector<char>                    addressed(out.all.size(), 0);
    std::vector<const records::Release*> released(out.all.size(), nullptr);
    for (std::size_t i = 0; i < out.all.size(); ++i) {
        const auto& d = out.all[i];
        const records::Acceptance& a = d.accept();
        std::optional<std::array<uint8_t, 32>> customer;
        if (const auto w = out.works.find(a.work.hash);
            w != out.works.end() && w->second.worker.bytes == a.work.chain)
            customer = w->second.record.customer;
        const records::Release* release = nullptr;
        if (a.release)
            if (const auto r = release_at.find(a.release->hash);
                r != release_at.end() && out.releases[r->second].seller.bytes == a.release->chain)
                release = &out.releases[r->second].release;
        released[i]  = release;
        addressed[i] = records::addressed_to(d.transfer, customer, release) ? 1 : 0;
        if (addressed[i] && release) out.taken.insert(a.release->hash);
    }

    // Lots: every output with links, its sub-thread checked (records.md §9.8 v2).
    // A link counts toward a lot only when its author is the worker of the work
    // it names — nobody writes into someone else's lot.
    const auto own_work = [&](const records::Ref& work) {
        const auto w = out.works.find(work.hash);
        return w == out.works.end() || w->second.worker.bytes == work.chain;
    };
    std::map<LotKey, std::vector<records::ObservedLotLink>> lot_links;
    for (const auto& r : out.releases)
        if (r.release.lot && own_work(r.release.work))
            lot_links[{r.release.work.hash, r.release.lot->out}].push_back(
                {r.hash, *r.release.lot, r.release.timestamp, r.release.expires,
                 out.taken.count(r.hash) > 0});
    for (const auto& w : out.writeoffs)
        if (own_work(w.writeoff.work))
            lot_links[{w.writeoff.work.hash, w.writeoff.lot.out}].push_back(
                {w.hash, w.writeoff.lot, w.writeoff.timestamp, std::nullopt, false});
    for (const auto& [key, links] : lot_links) {
        const auto w = out.works.find(key.first);
        if (w == out.works.end() || key.second >= w->second.record.outputs.size()) {
            records::LotState none;                 // no such lot on the table: nothing checks out
            for (const auto& l : links) none.invalid.insert(l.hash);
            out.lots[key] = std::move(none);
            continue;
        }
        out.lots[key] = records::check_lot(w->second.worker.bytes,
                                           w->second.record.outputs[key.second].qty, links);
    }

    // The claims — and what each deal bought: the whole result, or a portion.
    std::vector<records::DealClaim> claims;
    claims.reserve(out.all.size());
    for (std::size_t i = 0; i < out.all.size(); ++i) {
        const auto& d = out.all[i];
        const records::Acceptance& a = d.accept();
        const records::Release* release = released[i];
        std::optional<std::array<uint8_t, 32>> portion;
        bool sound = out.sound(a.work);
        if (release && release->lot) {
            portion = a.release->hash;
            const auto lot = out.lots.find({a.work.hash, release->lot->out});
            const auto w   = out.works.find(a.work.hash);
            sound = sound && lot != out.lots.end() && lot->second.valid.count(a.release->hash) > 0
                 && w != out.works.end()
                 && records::portion_fraction(w->second.record, *release->lot).has_value();
        }
        claims.push_back({d.hash, a.work.hash, d.payer.bytes, d.transfer.timestamp,
                          addressed[i] != 0, sound, portion});
    }
    out.recognition = records::recognize_deals(claims);

    // What was paid for: whole results, and portions.
    for (std::size_t i = 0; i < out.all.size(); ++i) {
        const auto& d = out.all[i];
        if (!out.moves_hours(d.hash)) continue;
        if (released[i] && released[i]->lot) out.sold_releases.insert(d.accept().release->hash);
        else                                 out.sold_whole.insert(d.accept().work.hash);
    }
    return out;
}

std::vector<LotSummary> lot_summaries(const DealsOnTable& deals,
                                      const std::array<uint8_t, 32>& work) {
    std::vector<LotSummary> out;
    const auto w = deals.works.find(work);
    if (w == deals.works.end()) return out;
    const auto& outputs = w->second.record.outputs;
    for (uint32_t i = 0; i < outputs.size(); ++i) {
        LotSummary s{};
        s.out       = i;
        s.qty       = outputs[i].qty;
        s.remaining = outputs[i].qty;
        if (const auto lot = deals.lots.find({work, i}); lot != deals.lots.end()) {
            const records::LotState& st = lot->second;
            s.faults    = st.invalid.size();
            s.gaps      = st.gaps;
            s.remaining = st.remaining();
            for (const auto& r : deals.releases)
                if (r.release.lot && r.release.work.hash == work && r.release.lot->out == i &&
                    st.valid.count(r.hash) && deals.sold_releases.count(r.hash))
                    s.sold += r.release.lot->qty;
            for (const auto& wo : deals.writeoffs)
                if (wo.writeoff.work.hash == work && wo.writeoff.lot.out == i &&
                    st.valid.count(wo.hash))
                    s.written_off += wo.writeoff.lot.qty;
            s.reserved = std::max(0.0, st.used - s.sold - s.written_off);
        }
        out.push_back(s);
    }
    return out;
}

double unconfirmed_hours(const DealsOnTable& deals, const std::array<uint8_t, 32>& work) {
    const auto w = deals.works.find(work);
    if (w == deals.works.end() || deals.sold_whole.count(work)) return 0.0;
    const records::WorkRecord& r = w->second.record;
    double gone = 0.0;   // the part of the work sold in portions or written off
    for (const auto& s : lot_summaries(deals, work)) {
        const auto& o = r.outputs[s.out];
        if (!(o.qty > 0.0)) continue;
        const double share = o.share ? *o.share : (r.outputs.size() == 1 ? 1.0 : 0.0);
        gone += share * (s.sold + s.written_off) / o.qty;
    }
    return r.hours * std::max(0.0, 1.0 - gone);
}

} // namespace aggregator
