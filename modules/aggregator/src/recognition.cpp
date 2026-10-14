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
    for (const auto& a : deals.assets)
        if (LotKey{a.src.hash, a.lot.out} == key && st.valid.count(a.hash))
            s.consumed += a.lot.qty;
    for (const auto& [wh, w] : deals.works)       // расход своей продукции (v7)
        for (const auto& sp : w.record.spends)
            if (LotKey{sp.work.hash, sp.lot.out} == key && st.valid.count(wh))
                s.consumed += sp.lot.qty;
    s.reserved = std::max(0.0, st.used - s.sold - s.written_off - s.consumed);
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
    // Позднее согласие цепи отменяет прежнее — порядок блоков в хранилище
    // произволен, поэтому сравниваем по времени блока (ИР-012).
    std::map<std::pair<Hash32, UserId>, int64_t>          consent_at;
    for (const Hash& bh : storage.all_block_hashes()) {
        const auto block = storage.get_block_by_hash(bh);
        if (!block || block->type != BlockType::DATA) continue;
        const UserId owner = block->address.user_id;
        records::Record rec;
        try { rec = records::Codec::decode(block->payload.data(), block->payload.size()); }
        catch (const records::CodecError&) { continue; }

        if (const auto* w = std::get_if<records::WorkRecord>(&rec)) {
            out.works[bh.bytes] = WorkOnTable{owner, *w};
            observed[owner].push_back({bh.bytes, records::labor_interval(*w), w->thread,
                                       block->timestamp_claimed});
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
        } else if (const auto* c = std::get_if<records::Consent>(&rec)) {
            // Согласие пишут у себя (ИР-012): чьё оно — говорит адрес блока.
            // Себя не спрашивают: подпись под своей же записью и есть согласие.
            if (c->target.chain == owner.bytes) continue;
            auto& slot = out.consents[c->target.hash];
            const auto prev = consent_at.find({c->target.hash, owner});
            if (prev == consent_at.end() || prev->second <= block->timestamp_claimed) {
                slot[owner] = c->agreed;
                consent_at[{c->target.hash, owner}] = block->timestamp_claimed;
            }
        } else if (const auto* m = std::get_if<records::Material>(&rec)) {
            out.asset_records[bh.bytes] =
                AssetRecord{owner, m->cost, m->origin, std::nullopt, 0.0, true,
                            m->days ? *m->days : 0u, block->timestamp_claimed};
            // Купленное — в своё производство (records.md §10.1 v3): запись заведена
            // звеном подниточки своей же покупки.
            if (m->src && m->lot && m->src->chain == owner.bytes)
                out.assets.push_back({bh.bytes, owner, *m->src, *m->lot, m->cost,
                                      block->timestamp_claimed, false});
        } else if (const auto* tl = std::get_if<records::Tool>(&rec)) {
            out.asset_records[bh.bytes] =
                AssetRecord{owner, tl->cost, tl->origin, tl->repair, 0.0, true,
                            tl->days ? *tl->days : 0u, block->timestamp_claimed};
            if (tl->src && tl->lot && tl->src->chain == owner.bytes)
                out.assets.push_back({bh.bytes, owner, *tl->src, *tl->lot, tl->cost,
                                      block->timestamp_claimed, false});
        } else if (const auto* t = std::get_if<records::Transfer>(&rec)) {
            if (t->from != owner.bytes) continue;          // spoofed sender
            if (!t->accept) {
                // Чужая бумага между своими ветками (economy.md §5а): кодек пускает
                // перевод без приёмки только из цепи в саму себя.
                if (t->to == owner.bytes)
                    out.moves.push_back(MoveEntry{bh.bytes, owner, *t, block->address.node_index,
                                                  block->address.block_index});
                continue;
            }
            deal_at[bh.bytes] = out.all.size();
            out.all.push_back(DealEntry{bh.bytes, owner, *t, block->address.node_index,
                                        block->address.block_index});
        } else if (const auto* dr = std::get_if<records::DealReply>(&rec)) {
            // Ответ продавца (records.md §11.13): чей он и в какой ветке — говорит
            // адрес блока; засчитывает его records::answer_of.
            out.replies[dr->deal.hash].push_back(
                ReplyEntry{bh.bytes, records::ObservedReply{*dr, owner.bytes,
                                                            block->address.node_index,
                                                            block->address.block_index}});
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

    // ── The seller's answer (records.md §11.13) ──────────────────────────────
    // A deal is an offer: the purse being paid accepts it by its term, or it did
    // not happen. A buyback paid with paper the holder agreed to when he signed
    // the return needs no answer (records::pre_accepted_buyback).
    std::vector<bool> answered(n, false);
    for (std::size_t i = 0; i < n; ++i) {
        const auto& d = out.all[i];
        std::vector<records::ObservedReply> seen;
        if (const auto r = out.replies.find(d.hash); r != out.replies.end())
            for (const auto& e : r->second) seen.push_back(e.seen);
        const auto a = records::answer_of(d.transfer, records::Ref{d.payer.bytes, d.hash}, seen);
        out.answers[d.hash] = a;
        const bool pre = buyback_of[i] && records::pre_accepted_buyback(d.transfer);
        if (pre) out.pre_accepted.insert(d.hash);
        answered[i] = pre || a.answer == records::Answer::accepted;
    }

    // ── Bills (economy.md §4.4) ──────────────────────────────────────────────
    // Spends of someone else's paper: by deals (carriers 0..n-1) and by moves
    // between own branches (n..n+m-1). Each names its bill and the spend before.
    const std::size_t m = out.moves.size();
    std::map<Hash32, std::size_t> move_at;
    for (std::size_t j = 0; j < m; ++j) move_at[out.moves[j].hash] = j;
    const auto carrier_transfer = [&](std::size_t c) -> const records::Transfer& {
        return c < n ? out.all[c].transfer : out.moves[c - n].transfer;
    };
    const auto carrier_author = [&](std::size_t c) -> const UserId& {
        return c < n ? out.all[c].payer : out.moves[c - n].owner;
    };
    const auto carrier_hash = [&](std::size_t c) -> const Hash32& {
        return c < n ? out.all[c].hash : out.moves[c - n].hash;
    };
    const auto carrier_index = [&](std::size_t c) -> uint64_t {
        return c < n ? out.all[c].index : out.moves[c - n].index;
    };
    const auto key_of = [&](std::size_t c, const records::OriginQty& o) {
        return BillKey{carrier_author(c).bytes, o.src->chain, o.src->hash, o.issuer};
    };
    // Each bill's spends in branch order: (carrier, portion).
    std::map<BillKey, std::vector<std::pair<std::size_t, std::size_t>>> spends_of;
    for (std::size_t c = 0; c < n + m; ++c) {
        const auto& t = carrier_transfer(c);
        for (std::size_t p = 0; p < t.origins.size(); ++p)
            if (t.origins[p].src) spends_of[key_of(c, t.origins[p])].push_back({c, p});
    }
    for (auto& [key, list] : spends_of) {
        (void)key;
        std::sort(list.begin(), list.end(), [&](const auto& a, const auto& b) {
            const auto ia = carrier_index(a.first), ib = carrier_index(b.first);
            return ia != ib ? ia < ib : carrier_hash(a.first) < carrier_hash(b.first);
        });
    }

    // Whether a deal happened — addressed, answered, backed — whether a
    // carrier's paper is backed, and whether a spend holds its hours. A bill's
    // credit stands on its source having happened, and whether a spend fits
    // depends only on the spends before it: every reference names an earlier
    // block, so this recursion ends; a loop could not be written, and is
    // refused if met.
    std::vector<int8_t> eff_state(n, -1), back_state(n + m, -1), hold_state(n + m, -1);
    std::function<bool(std::size_t)> addressed;
    std::function<bool(std::size_t)> effective;
    std::function<bool(std::size_t)> backed;

    struct BillCredit {
        uint32_t                purse = 0;
        double                  credited = 0.0;
        std::optional<uint64_t> arrived;   // the holder's acceptance of the paying deal
        std::string             why;
        bool                    upstream = false;
    };
    std::map<BillKey, BillCredit> credits;
    std::function<BillCredit(const BillKey&)> credit_of = [&](const BillKey& key) -> BillCredit {
        if (const auto it = credits.find(key); it != credits.end()) return it->second;
        credits[key] = BillCredit{0, 0.0, std::nullopt, "вексель решается", false};   // a loop credits nothing
        const auto& [holder, src_chain, src_hash, issuer] = key;
        BillCredit b;
        if (const auto d = deal_at.find(src_hash);
            d != deal_at.end() && out.all[d->second].payer.bytes == src_chain) {
            const auto& sd = out.all[d->second];
            b.purse = sd.transfer.to_node;
            if (sd.transfer.to != holder) {
                b.why = "сделка-источник платила не этому держателю";
            } else if (!effective(d->second)) {
                b.why      = "сделка-источник не состоялась";
                b.upstream = true;
            } else {
                b.credited = records::bill_credit(sd.transfer, issuer);
                // Paper paid by a deal is the holder's from his acceptance on.
                if (const auto* r = out.counted_reply(sd.hash)) b.arrived = r->seen.index;
            }
        } else if (const auto mv = move_at.find(src_hash);
                   mv != move_at.end() && out.moves[mv->second].owner.bytes == src_chain) {
            const auto& sm = out.moves[mv->second];
            b.purse = sm.transfer.to_node;
            if (sm.owner.bytes != holder) {
                b.why = "перемещение-источник — чужое";
            } else if (!backed(n + mv->second)) {
                b.why      = "перемещение-источник не обеспечено";
                b.upstream = true;
            } else {
                b.credited = records::bill_credit(sm.transfer, issuer);
            }
        } else {
            b.why = "векселя-источника нет на столе";
        }
        return credits[key] = b;
    };
    // Whether a spend could hold hours at all: a move that stands, a deal that
    // completes something and is paid with paper — the term and the answer are
    // records::check_bill's to judge.
    std::function<bool(std::size_t)> holds = [&](std::size_t c) -> bool {
        if (hold_state[c] == 0 || hold_state[c] == 1) return hold_state[c] == 1;
        if (hold_state[c] == 2) return false;
        hold_state[c] = 2;
        const bool ok = (c >= n || addressed(c)) && backed(c);
        hold_state[c] = ok ? 1 : 0;
        return ok;
    };
    const auto spend_of = [&](std::size_t c, std::size_t p, bool judge) {
        const auto& t = carrier_transfer(c);
        records::BillSpend sp{};
        sp.hash      = carrier_hash(c);
        sp.node      = c < n ? out.all[c].node : out.moves[c - n].node;
        sp.index     = carrier_index(c);
        sp.timestamp = t.timestamp;
        sp.units     = t.origins[p].units;
        sp.prev      = t.origins[p].prev;
        sp.move      = c >= n;
        if (c < n) {
            sp.answer = out.answer(out.all[c].hash);
            sp.until  = t.until.value_or(0);
        }
        if (judge) sp.stands = holds(c);
        return sp;
    };
    // The bill as it stood when spend `c` was made: its credit, and the spends
    // before it with what each still holds.
    const auto bill_at = [&](const BillKey& key, std::size_t c) {
        const BillCredit credit = credit_of(key);
        std::vector<records::BillSpend> prefix;
        for (const auto& [sc, sp] : spends_of[key]) {
            const bool self = sc == c;
            prefix.push_back(spend_of(sc, sp, !self));
            if (self) break;
        }
        auto st = records::check_bill(std::get<0>(key), credit.purse, credit.credited,
                                      credit.arrived, std::move(prefix));
        if (!credit.why.empty()) st.fault = credit.why;
        return std::make_pair(credit, st);
    };
    backed = [&](std::size_t c) -> bool {
        if (back_state[c] == 0 || back_state[c] == 1) return back_state[c] == 1;
        if (back_state[c] == 2) return false;
        back_state[c] = 2;
        const auto& t = carrier_transfer(c);
        bool        ok = true, upstream = false;
        std::string why;
        for (const auto& o : t.origins) {
            if (!o.src) continue;
            const auto [credit, st] = bill_at(key_of(c, o), c);
            if (st.backed.count(carrier_hash(c))) continue;
            ok       = false;
            why      = st.fault;
            upstream = credit.upstream;
            break;
        }
        back_state[c] = ok ? 1 : 0;
        if (!ok) {
            out.unbacked.insert(carrier_hash(c));
            out.unbacked_why[carrier_hash(c)] = why;
            if (upstream) out.unbacked_upstream.insert(carrier_hash(c));
        } else if (c >= n) {
            out.moves_backed.insert(carrier_hash(c));
        }
        return ok;
    };
    effective = [&](std::size_t i) -> bool {
        if (eff_state[i] == 0 || eff_state[i] == 1) return eff_state[i] == 1;
        if (eff_state[i] == 2) return false;
        eff_state[i] = 2;
        const bool ok = addressed(i) && answered[i] && backed(i);
        eff_state[i] = ok ? 1 : 0;
        return ok;
    };

    // Whether the seller named the payer (records.md §9.5) on terms that were met
    // (records.md §9.8 v3, v4). A resale stands on the holder's purchase, so this
    // walks back along the purchases — a loop of them cannot be written, each
    // deal names blocks that existed before it, and is refused if met. The
    // purchase must have happened: accepted by its seller and backed (v6).
    std::vector<int8_t> state(n, -1);   // -1 not yet, 0 no, 1 yes, 2 being decided
    addressed = [&](std::size_t i) -> bool {
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
            // Выкуп с ремонтом (ИР-015): сделка называет работу держателя над этой
            // покупкой — тогда платят и за вещь, и за труд над ней одной записью.
            const records::WorkRecord* repair = nullptr;
            if (!(a.work == p.accept().work))
                if (const auto rw = out.works.find(a.work.hash);
                    rw != out.works.end() && rw->second.worker.bytes == a.work.chain)
                    repair = &rw->second.record;
            if (effective(h) && w != out.works.end() &&
                w->second.worker.bytes == p.accept().work.chain)
                ok = records::completes_buyback(
                    d.transfer, p.transfer,
                    records::buyback_price(lots_of(h), w->second.record, p.transfer.timestamp,
                                           d.transfer.timestamp, p.accept().buyback->wear),
                    repair);
        } else if (release && release->holding) {
            // Перепродажа: the holder's purchase happened, of this very product,
            // and the price is its share of what the holder paid.
            const auto h = holding_deal(release->holding);
            if (h && effective(*h) && release->lot && release->work == out.all[*h].accept().work)
                for (const auto& hl : lots_of(*h))
                    if (hl.out == release->lot->out)
                        ok = records::addressed_to(d.transfer, std::nullopt, release) &&
                             records::meets_resale(a, records::resale_price(hl, release->lot->qty));
            // Покупка с адресом (records.md §9.8 v5, решение участника 2026-09-16):
            // до срока её продают только адресату, а выкупленную — никому. Такая
            // сделка ничтожна, а не просто несостоятельна: препятствие видно
            // покупателю заранее, часы ему терять не за что.
            if (ok && h)
                if (const auto& b = out.all[*h].accept().buyback) {
                    if (release->timestamp < b->until && release->to != b->to) {
                        ok = false;
                    } else {
                        for (std::size_t j = 0; j < n && ok; ++j)
                            if (buyback_of[j] && *buyback_of[j] == *h && effective(j)) ok = false;
                    }
                }
        } else {
            const auto w = out.works.find(a.work.hash);
            const bool here = w != out.works.end() && w->second.worker.bytes == a.work.chain;
            std::optional<Hash32> customer;
            if (here) customer = w->second.record.customer;
            ok = records::addressed_to(d.transfer, customer, release);
            // Машино-часы (ИР-023, пункт 9): труда в такой записи нет — платят
            // ровно записанный износ, и разбивать нечего; приёмка с трудом по ней
            // лгала бы о часах, которых никто не работал.
            if (here && records::is_machine_hours(w->second.record)) {
                // Целиком — платят весь записанный износ; сменой (порцией
                // номерной партии, ИР-013) — её долю, и это проверяют условия
                // порции ниже.
                if (!(release && release->lot))
                    ok = ok && records::meets_machine_hours(a, w->second.record);
            } else if (a.axes.empty()) {
                // Приёмки без разбивки не существует (ИР-022): одним переносом у
                // мастера покупают только то, что он держит, — перепродажей или
                // выкупом.
                ok = false;
            }
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
    // A release is taken, a purchase bought back, only by a deal that happened.
    for (std::size_t i = 0; i < n; ++i) {
        if (!effective(i)) continue;
        if (released[i]) out.taken.insert(out.all[i].accept().release->hash);
        if (buyback_of[i]) out.bought_back.insert(out.all[*buyback_of[i]].hash);
    }
    // Every carrier's paper judged, whether or not anything reads it: an offer
    // nobody accepted still shows what its payer tried to pay with. Then each
    // bill as it stands now, for the views.
    for (std::size_t c = 0; c < n + m; ++c) backed(c);
    for (const auto& [key, list] : spends_of) {
        const BillCredit credit = credit_of(key);
        std::vector<records::BillSpend> all;
        for (const auto& [sc, sp] : list) all.push_back(spend_of(sc, sp, true));
        BillOnTable b;
        b.purse    = credit.purse;
        b.why      = credit.why;
        b.upstream = credit.upstream;
        b.state    = records::check_bill(std::get<0>(key), credit.purse, credit.credited,
                                         credit.arrived, std::move(all));
        if (!credit.why.empty()) b.state.fault = credit.why;
        out.bills[key] = std::move(b);
    }
    // Purchases of goods that happened: the buyers' stock.
    for (std::size_t i = 0; i < n; ++i)
        if (effective(i))
            if (auto lots = lots_of(i); !lots.empty()) out.holdings[out.all[i].hash] = std::move(lots);

    // Стоимость купленного, заведённого в производство (records.md §10.1 v3): её
    // не объявляют — это доля уплаченного за взятое, как при перепродаже. Запись
    // с завышенной стоимостью не признаётся: перенос из неё был бы насосом (ИР-011).
    for (auto& a : out.assets) {
        const auto d = deal_at.find(a.src.hash);
        if (d == deal_at.end() || out.all[d->second].payer.bytes != a.src.chain) continue;
        if (!effective(d->second)) continue;              // покупки не было
        const auto h = out.holdings.find(a.src.hash);
        if (h == out.holdings.end()) continue;
        for (const auto& hl : h->second)
            if (hl.out == a.lot.out)
                a.sound = std::abs(a.cost - records::resale_price(hl, a.lot.qty)) <= 1e-6;
    }

    // Нить переноса (ИР-011): сколько стоимости записи уже ушло в продукцию —
    // последнее звено, написанное самим владельцем.
    for (const auto& [wh, w] : out.works) {
        (void)wh;
        for (const auto& ce : w.record.carry) {
            const auto a = out.asset_records.find(ce.src.hash);
            if (a == out.asset_records.end() || ce.src.chain != w.worker.bytes ||
                a->second.owner != w.worker)
                continue;
            a->second.collected = std::max(a->second.collected, ce.after);
        }
    }
    // Ремонт — единственный законный путь стоимости вверх (ИР-015,
    // records.md §10.2): перевыпуск не выше остатка прежней записи, а выше —
    // ровно на принятый и оплаченный ремонт, и одна сделка оправдывает одно
    // удорожание.
    std::set<Hash32> spent_repairs;
    for (auto& [ah, a] : out.asset_records) {
        (void)ah;
        if (!a.origin) continue;
        const auto prev = out.asset_records.find(a.origin->hash);
        if (prev == out.asset_records.end() || a.origin->chain != a.owner.bytes ||
            prev->second.owner != a.owner) {
            a.sound = false;                       // перевыпуск не своей записи
            continue;
        }
        // Потолок перевыпуска — остаток прежней записи по БОЛЬШЕЙ из двух
        // шкал на день новой (ИР-011 v5): простоявшую вещь нельзя
        // перевыпустить по прежней цене, календарь уже съел часть.
        double allowed = prev->second.residual(a.started_at);
        if (a.repair) {
            const auto d = deal_at.find(a.repair->hash);
            if (d != deal_at.end() && a.repair->chain == a.owner.bytes &&
                out.all[d->second].payer == a.owner && effective(d->second) &&
                spent_repairs.insert(a.repair->hash).second) {
                double paid = 0.0;
                for (const auto& o : out.all[d->second].transfer.origins) paid += o.units;
                allowed += paid;
            }
        }
        a.sound = a.cost <= allowed + 1e-9;
    }

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
    // Расход своей продукции в своё производство (records.md §9.4 v7): звено той
    // же подниточки, что продажи и списания — израсходованного уже не продать.
    for (const auto& [wh, w] : out.works)
        for (const auto& sp : w.record.spends) {
            if (sp.work.chain != w.worker.bytes) continue;         // расходуют только своё
            lot_links[{sp.work.hash, sp.lot.out}].push_back(
                {wh, sp.lot, records::labor_interval(w.record).second, std::nullopt, false, true});
        }
    for (const auto& a : out.assets) {   // взятое в производство — звено той же подниточки
        const auto d = deal_at.find(a.src.hash);
        if (d != deal_at.end() && out.all[d->second].payer.bytes != a.src.chain) continue;
        lot_links[{a.src.hash, a.lot.out}].push_back(
            {a.hash, a.lot, a.timestamp, std::nullopt, false, true});
    }
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
            // Выкуп с ремонтом (ИР-015): труд держателя судится как любой другой —
            // он обязан быть в его нити и на своих часах.
            if (ok && !(a.work == out.all[*buyback_of[i]].accept().work)) ok = out.sound(a.work);
        } else if (release && release->holding) {
            const auto h = holding_deal(release->holding);
            ok = h && sound(*h) && link_valid(*release);
        } else {
            // Перенос из записи, перевыпущенной вверх без оплаченного ремонта, —
            // стоимость из воздуха (ИР-015): такая работа к сделке не признаётся.
            ok = out.sound(a.work) && out.carry_sound(a.work.hash);
            // Расход своей продукции (records.md §9.4 v7): труд детали приходит в
            // изделие, поэтому и работа-источник обязана быть в счёт, и её звено —
            // сходиться: иначе одни и те же часы продали бы дважды.
            if (ok)
                if (const auto w = out.works.find(a.work.hash); w != out.works.end())
                    for (const auto& sp : w->second.record.spends) {
                        const auto lot = out.lots.find({sp.work.hash, sp.lot.out});
                        if (!out.sound(sp.work) || lot == out.lots.end() ||
                            !lot->second.valid.count(a.work.hash)) {
                            ok = false;
                            break;
                        }
                    }
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
        // Выкуп с ремонтом (ИР-015) — сделка за труд: источник у неё сама работа
        // держателя, и в статистику она идёт как любая оплата труда. Выкуп без
        // ремонта — только перенос, источник — покупка держателя.
        const bool repaired = buyback && !(a.work == out.all[*buyback_of[i]].accept().work);
        const bool resale   = (buyback && !repaired) || (release && release->holding);
        std::optional<Hash32> portion;
        if ((release && release->lot) || (buyback && !repaired)) portion = a.release->hash;
        const Hash32 source = repaired ? a.work.hash
                            : buyback  ? a.release->hash                // the holder's purchase
                            : resale   ? release->holding->hash : a.work.hash;
        records::DealClaim c{d.hash, source, d.payer.bytes, d.transfer.timestamp,
                             state[i] == 1, sound(i), portion};
        c.resale  = resale;
        c.settled = answered[i] && backed(i);    // v6: accepted in time and backed
        claims.push_back(c);
    }
    out.recognition = records::recognize_deals(claims);

    // What was paid for: whole results, and portions.
    for (std::size_t i = 0; i < n; ++i) {
        const auto& d = out.all[i];
        const bool repaired = buyback_of[i] &&
            !(d.accept().work == out.all[*buyback_of[i]].accept().work);
        if (!out.moves_hours(d.hash) || (buyback_of[i] && !repaired)) continue;
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

namespace aggregator {

const char* deal_standing(const DealsOnTable& deals, const DealEntry& d, int64_t now) {
    const auto& rc = deals.recognition;
    if (rc.recognized.count(d.hash))  return "recognized";
    if (rc.resold.count(d.hash))      return "resold";
    if (rc.repeated.count(d.hash))    return "repeated";
    if (rc.contested.count(d.hash))   return "contested";
    if (rc.unsound.count(d.hash))     return "unsound";
    if (rc.unaddressed.count(d.hash)) return "unaddressed";
    // An offer that moved nothing (economy.md §4.4): paper no bill covers says
    // more than the answer, then the answer — or the silence — itself.
    if (deals.unbacked.count(d.hash)) return "unbacked";
    return records::deal_status_name(records::deal_status(d.transfer, deals.answer(d.hash), now));
}

} // namespace aggregator
