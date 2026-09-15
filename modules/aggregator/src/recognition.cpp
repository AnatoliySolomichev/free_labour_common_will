#include "aggregator/recognition.h"

#include <records/codec.h>

#include <algorithm>
#include <functional>

namespace aggregator {

namespace {

using Hash32 = std::array<uint8_t, 32>;

// Which lot a release or write-off takes from: the holder's purchase when it
// sells a bought lot on, else the work's own output (records.md §9.8 v4).
LotKey lot_key(const records::Ref& work, const std::optional<records::Ref>& holding,
               uint32_t out) {
    return {holding ? holding->hash : work.hash, out};
}

// How much of one lot went where.
LotSummary summarize(const DealsOnTable& deals, const LotKey& key, double qty,
                     const std::vector<std::string>& units) {
    LotSummary s{};
    s.out       = key.second;
    s.qty       = qty;
    s.remaining = qty;
    const auto lot = deals.lots.find(key);
    if (lot == deals.lots.end()) {
        s.free_units = units;
        return s;
    }
    const records::LotState& st = lot->second;
    s.faults    = st.invalid.size();
    s.gaps      = st.gaps;
    s.remaining = st.remaining();
    for (const auto& r : deals.releases)
        if (r.release.lot && lot_key(r.release.work, r.release.holding, r.release.lot->out) == key &&
            st.valid.count(r.hash) && deals.sold_releases.count(r.hash))
            s.sold += r.release.lot->qty;
    for (const auto& w : deals.writeoffs)
        if (lot_key(w.writeoff.work, w.writeoff.holding, w.writeoff.lot.out) == key &&
            st.valid.count(w.hash))
            s.written_off += w.writeoff.lot.qty;
    s.reserved = std::max(0.0, st.used - s.sold - s.written_off);
    for (const auto& u : units)
        if (!st.held.count(u)) s.free_units.push_back(u);
    return s;
}

} // namespace

DealsOnTable read_deals(const AggregatorStorage& storage) {
    DealsOnTable out;
    std::map<Hash32, std::size_t>                        release_at;   // release hash → index
    std::map<Hash32, std::size_t>                        deal_at;      // deal hash → index
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
            // The seller: the worker of the work, or the holder of a purchase (v4).
            const auto& seller = rl->holding ? rl->holding->chain : rl->work.chain;
            if (seller != owner.bytes) continue;
            release_at[bh.bytes] = out.releases.size();
            out.releases.push_back(ReleaseEntry{bh.bytes, owner, *rl});
        } else if (const auto* wo = std::get_if<records::WriteOff>(&rec)) {
            const auto& seller = wo->holding ? wo->holding->chain : wo->work.chain;
            if (seller != owner.bytes) continue;           // only one's own lot is written off
            out.writeoffs.push_back(WriteOffEntry{bh.bytes, owner, *wo});
        } else if (const auto* t = std::get_if<records::Transfer>(&rec)) {
            if (!t->accept) continue;
            if (t->from != owner.bytes) continue;          // spoofed sender
            deal_at[bh.bytes] = out.all.size();
            out.all.push_back(DealEntry{bh.bytes, owner, *t});
        }
    }

    // Each chain's goods thread (records.md §9.4 v4): its links and its hours.
    for (const auto& [chain, works] : observed)
        out.threads[chain] = records::check_labor_thread(chain.bytes, works);

    // What each deal completed. Blocks come in hash order, so the seller's
    // records are matched to the deals only after the whole scan.
    const std::size_t n = out.all.size();
    std::vector<const records::Release*>    released(n, nullptr);
    std::vector<std::optional<std::size_t>> buyback_of(n);   // the holder's purchase bought back
    for (std::size_t i = 0; i < n; ++i) {
        const records::Acceptance& a = out.all[i].accept();
        if (!a.release) continue;
        if (const auto r = release_at.find(a.release->hash);
            r != release_at.end() && out.releases[r->second].seller.bytes == a.release->chain)
            released[i] = &out.releases[r->second].release;
        else if (const auto h = deal_at.find(a.release->hash);
                 h != deal_at.end() && out.all[h->second].payer.bytes == a.release->chain &&
                 out.all[h->second].accept().buyback)
            buyback_of[i] = h->second;   // records.md §9.8 v5: the return is the purchase itself
    }
    // The purchase a resale stands on: the holder's deal (records.md §9.8 v4).
    const auto holding_deal = [&](const std::optional<records::Ref>& holding)
        -> std::optional<std::size_t> {
        if (!holding) return std::nullopt;
        const auto h = deal_at.find(holding->hash);
        if (h == deal_at.end() || out.all[h->second].payer.bytes != holding->chain)
            return std::nullopt;
        return h->second;
    };
    // The lots a purchase put in the buyer's hands. Bought back on a pre-signed
    // return (v5), they are the holder's lots, at what the addressee paid.
    std::function<std::vector<records::HoldingLot>(std::size_t)> lots_of =
        [&](std::size_t j) -> std::vector<records::HoldingLot> {
        if (buyback_of[j]) {
            auto   lots = lots_of(*buyback_of[j]);
            double cost = 0.0, paid = 0.0;
            for (const auto& l : lots) cost += l.cost;
            for (const auto& o : out.all[j].transfer.origins) paid += o.units;
            for (auto& l : lots) l.cost = cost > 0.0 ? paid * l.cost / cost : 0.0;
            return lots;
        }
        const records::Acceptance& a = out.all[j].accept();
        const auto w = out.works.find(a.work.hash);
        if (w == out.works.end() || w->second.worker.bytes != a.work.chain) return {};
        return records::holding_lots(out.all[j].transfer, w->second.record, released[j]);
    };

    // Whether the seller named the payer (records.md §9.5) on terms that were met
    // (records.md §9.8 v3, v4). A resale stands on the holder's purchase, so this
    // walks back along the purchases — a loop of them cannot be written, each
    // deal names blocks that existed before it, and is refused if met.
    std::vector<int8_t> state(n, -1);   // -1 not yet, 0 no, 1 yes, 2 being decided
    std::function<bool(std::size_t)> addressed = [&](std::size_t i) -> bool {
        if (state[i] == 0 || state[i] == 1) return state[i] == 1;
        if (state[i] == 2) return false;
        state[i] = 2;
        const auto& d = out.all[i];
        const records::Acceptance& a = d.accept();
        const records::Release* release = released[i];
        bool ok = false;
        if (buyback_of[i]) {
            // Выкуп по заранее подписанному возврату (records.md §9.8 v5): the deal
            // names the holder's purchase itself, which pre-signed it. Its price is
            // nobody's to name: what the holder paid — less, for a rent, the wear
            // by the norm until now (v6).
            const std::size_t h = *buyback_of[i];
            const auto&       p = out.all[h];
            const auto        w = out.works.find(p.accept().work.hash);
            if (addressed(h) && w != out.works.end() &&
                w->second.worker.bytes == p.accept().work.chain)
                ok = records::completes_buyback(
                    d.transfer, p.transfer,
                    records::buyback_price(lots_of(h), w->second.record, p.transfer.timestamp,
                                           d.transfer.timestamp, p.accept().buyback->wear));
        } else if (release && release->holding) {
            // Перепродажа: the holder's purchase happened, of this very product,
            // and the price is its share of what the holder paid.
            const auto h = holding_deal(release->holding);
            if (h && addressed(*h) && release->lot && release->work == out.all[*h].accept().work)
                for (const auto& hl : lots_of(*h))
                    if (hl.out == release->lot->out)
                        ok = records::addressed_to(d.transfer, std::nullopt, release) &&
                             records::meets_resale(a, records::resale_price(hl, release->lot->qty));
        } else {
            const auto w = out.works.find(a.work.hash);
            const bool here = w != out.works.end() && w->second.worker.bytes == a.work.chain;
            std::optional<Hash32> customer;
            if (here) customer = w->second.record.customer;
            ok = records::addressed_to(d.transfer, customer, release);
            // Приёмки без разбивки не существует (ИР-022): одним переносом у мастера
            // покупают только то, что он держит, — перепродажей или выкупом.
            if (a.axes.empty()) ok = false;
            // A portion from stock is sold on the lot's terms — take it or leave
            // it (records.md §9.8 v3): a deal that pays otherwise completes nothing.
            if (ok && release && release->lot) {
                const auto terms = here ? records::portion_terms(w->second.record, *release->lot)
                                        : std::optional<records::PortionTerms>{};
                ok = terms && records::meets_terms(a, *terms);
            }
        }
        // Выкуп с адресом (records.md §9.8 v5): a purchase on an addressed buyout
        // pre-signs the return — or it completes nothing.
        if (ok && release && release->addressed)
            ok = records::honours_buyout(a, *release->addressed);
        state[i] = ok ? 1 : 0;
        return ok;
    };
    for (std::size_t i = 0; i < n; ++i) {
        if (!addressed(i)) continue;
        if (released[i]) out.taken.insert(out.all[i].accept().release->hash);
        if (buyback_of[i]) out.bought_back.insert(out.all[*buyback_of[i]].hash);
    }
    // Purchases of goods that happened: the buyers' stock.
    for (std::size_t i = 0; i < n; ++i)
        if (state[i] == 1)
            if (auto lots = lots_of(i); !lots.empty()) out.holdings[out.all[i].hash] = std::move(lots);

    // Lots (records.md §9.8): outputs sold in portions and purchases sold on,
    // every sub-thread checked. A link counts toward a lot only when its author
    // is that lot's seller — nobody writes into someone else's lot.
    const auto belongs = [&](const records::Ref& work, const std::optional<records::Ref>& holding) {
        if (holding) {
            const auto h = deal_at.find(holding->hash);
            return h == deal_at.end() || out.all[h->second].payer.bytes == holding->chain;
        }
        const auto w = out.works.find(work.hash);
        return w == out.works.end() || w->second.worker.bytes == work.chain;
    };
    std::map<LotKey, std::vector<records::ObservedLotLink>> lot_links;
    for (const auto& r : out.releases)
        if (r.release.lot && belongs(r.release.work, r.release.holding))
            lot_links[lot_key(r.release.work, r.release.holding, r.release.lot->out)].push_back(
                {r.hash, *r.release.lot, r.release.timestamp, r.release.expires,
                 out.taken.count(r.hash) > 0});
    for (const auto& w : out.writeoffs)
        if (belongs(w.writeoff.work, w.writeoff.holding))
            lot_links[lot_key(w.writeoff.work, w.writeoff.holding, w.writeoff.lot.out)].push_back(
                {w.hash, w.writeoff.lot, w.writeoff.timestamp, std::nullopt, false});
    for (const auto& [key, links] : lot_links) {
        // The lot these links take from: an output of a work on the table, or a
        // lot of a purchase that happened.
        std::optional<std::pair<UserId, records::HoldingLot>> lot;
        if (const auto w = out.works.find(key.first); w != out.works.end()) {
            if (key.second < w->second.record.outputs.size()) {
                const auto& o = w->second.record.outputs[key.second];
                lot = std::make_pair(w->second.worker,
                                     records::HoldingLot{key.second, o.qty, o.units, 0.0});
            }
        } else if (const auto h = out.holdings.find(key.first); h != out.holdings.end()) {
            for (const auto& hl : h->second)
                if (hl.out == key.second) lot = std::make_pair(out.all[deal_at.at(key.first)].payer, hl);
        }
        if (!lot) {
            records::LotState none;                 // no such lot on the table: nothing checks out
            for (const auto& l : links) none.invalid.insert(l.hash);
            out.lots[key] = std::move(none);
            continue;
        }
        // A purchase on an addressed buyout (records.md §9.8 v5): until its term it
        // is sold on only by the pre-signed return, and once bought back — no more
        // at all. Write-offs stand: a loss is the holder's, at any time.
        const records::Buyback* buyback = nullptr;
        if (const auto h = deal_at.find(key.first);
            h != deal_at.end() && out.all[h->second].accept().buyback)
            buyback = &*out.all[h->second].accept().buyback;
        std::vector<records::ObservedLotLink> kept;
        std::vector<Hash32>                   refused;
        for (const auto& l : links) {
            const bool sale = l.expires.has_value();
            if (buyback && sale && (out.bought_back.count(key.first) || l.timestamp < buyback->until))
                refused.push_back(l.hash);
            else
                kept.push_back(l);
        }
        auto st = records::check_lot(lot->first.bytes, lot->second.qty, kept, lot->second.units);
        for (const auto& h : refused) st.invalid.insert(h);
        out.lots[key] = std::move(st);
    }

    // Whether a deal may count: its work in the worker's thread on hours of its
    // own; a portion on a link of its lot that checks out; a resale on a link of
    // the holder's lot, of a purchase that was sound itself.
    std::vector<int8_t> sound_state(n, -1);
    std::function<bool(std::size_t)> sound = [&](std::size_t i) -> bool {
        if (sound_state[i] == 0 || sound_state[i] == 1) return sound_state[i] == 1;
        if (sound_state[i] == 2) return false;
        sound_state[i] = 2;
        const records::Acceptance& a = out.all[i].accept();
        const records::Release* release = released[i];
        const auto link_valid = [&](const records::Release& rl) {
            const auto lot = out.lots.find(lot_key(rl.work, rl.holding, rl.lot->out));
            return lot != out.lots.end() && lot->second.valid.count(a.release->hash) > 0;
        };
        bool ok;
        if (buyback_of[i]) {
            ok = sound(*buyback_of[i]);          // what is bought back is what was bought
        } else if (release && release->holding) {
            const auto h = holding_deal(release->holding);
            ok = h && sound(*h) && link_valid(*release);
        } else {
            ok = out.sound(a.work);
            if (ok && release && release->lot) {
                const auto w = out.works.find(a.work.hash);
                ok = link_valid(*release) && w != out.works.end() &&
                     records::portion_fraction(w->second.record, *release->lot).has_value();
            }
        }
        sound_state[i] = ok ? 1 : 0;
        return ok;
    };

    // The claims — and what each deal bought: the whole result, a portion of a
    // lot, or a portion of somebody's purchase sold on.
    std::vector<records::DealClaim> claims;
    claims.reserve(n);
    for (std::size_t i = 0; i < n; ++i) {
        const auto& d = out.all[i];
        const records::Acceptance& a = d.accept();
        const records::Release* release = released[i];
        const bool buyback = buyback_of[i].has_value();
        const bool resale  = buyback || (release && release->holding);
        std::optional<Hash32> portion;
        if ((release && release->lot) || buyback) portion = a.release->hash;
        const Hash32 source = buyback ? a.release->hash                // the holder's purchase
                            : resale  ? release->holding->hash : a.work.hash;
        records::DealClaim c{d.hash, source, d.payer.bytes, d.transfer.timestamp,
                             state[i] == 1, sound(i), portion};
        c.resale = resale;
        claims.push_back(c);
    }
    out.recognition = records::recognize_deals(claims);

    // What was paid for: whole results, and portions.
    for (std::size_t i = 0; i < n; ++i) {
        const auto& d = out.all[i];
        if (!out.moves_hours(d.hash) || buyback_of[i]) continue;
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
    for (uint32_t i = 0; i < outputs.size(); ++i)
        out.push_back(summarize(deals, {work, i}, outputs[i].qty, outputs[i].units));
    return out;
}

std::vector<LotSummary> holding_summaries(const DealsOnTable& deals,
                                          const std::array<uint8_t, 32>& purchase) {
    std::vector<LotSummary> out;
    const auto h = deals.holdings.find(purchase);
    if (h == deals.holdings.end()) return out;
    const bool back = deals.bought_back.count(purchase) > 0;
    for (const auto& hl : h->second) {
        auto s = summarize(deals, {purchase, hl.out}, hl.qty, hl.units);
        if (back) {   // bought back whole on the pre-signed return (records.md §9.8 v5)
            s.sold      = std::max(0.0, hl.qty - s.written_off);
            s.reserved  = 0.0;
            s.remaining = 0.0;
            s.free_units.clear();
        }
        out.push_back(std::move(s));
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
