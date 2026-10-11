#include "aggregator/resource_view.h"

#include <records/codec.h>

#include <algorithm>
#include <map>
#include <set>

namespace aggregator {

namespace {
using RefHash = std::array<uint8_t, 32>;
}

ResourceSplit ResourceView::build(const AggregatorStorage& storage,
                                  const UserId& chain,
                                  const RefHash& tool,
                                  int64_t now) {
    ResourceSplit out{};

    // 1. Сама запись вещи: имя и ресурс берутся из неё, стоимость и шкалы —
    //    со стола (там же посчитано собранное нитью).
    const auto block = storage.get_block_by_hash(Hash{tool});
    if (!block || block->type != BlockType::DATA) return out;
    records::Record rec;
    try { rec = records::Codec::decode(block->payload.data(), block->payload.size()); }
    catch (const records::CodecError&) { return out; }
    if (!(block->address.user_id == chain)) return out;

    if (const auto* t = std::get_if<records::Tool>(&rec)) {
        out.name = t->name;
        out.life = t->life;
        out.cost = t->cost;
        out.days = t->days ? *t->days : 0u;
    } else if (const auto* m = std::get_if<records::Material>(&rec)) {
        out.name = m->name;
        out.life = m->qty;            // у партии «ресурс» — её размер
        out.cost = m->cost;
        out.days = m->days ? *m->days : 0u;
    } else {
        return out;                   // не средство производства
    }
    out.known  = true;
    out.keeper = chain;

    const auto deals = read_deals(storage);
    if (const auto a = deals.asset_records.find(tool); a != deals.asset_records.end()) {
        out.collected = a->second.collected;
        out.residual  = a->second.residual(now);
    } else {
        out.residual = out.cost;
    }

    // 2. Машино-часы этой вещи — её ресурс, проданный вперёд: записи без труда
    //    с одним переносом из неё (records.md §9.4 v8). Остальной перенос —
    //    расход ресурса самим держателем в своей продукции.
    std::map<RefHash, double> sale_hours;      // работа-продажа → моточасов
    // Единица партии бывает не часом: календарь продаётся СМЕНАМИ (ИР-013), и
    // одна смена несёт used/qty моточасов. Масштаб нужен, чтобы расход
    // дольщика считался в часах ресурса, а не в сменах.
    std::map<RefHash, double> hours_per_unit;
    for (const auto& [wh, w] : deals.works) {
        if (!(w.worker == chain)) continue;
        double used_here = 0;
        bool   touches    = false;
        for (const auto& ce : w.record.carry)
            if (ce.src.hash == tool && ce.src.chain == chain.bytes) {
                used_here += ce.used;
                touches = true;
            }
        if (!touches) continue;
        if (records::is_machine_hours(w.record)) {
            sale_hours[wh] = used_here;
            const double qty = w.record.outputs.empty() ? 0.0 : w.record.outputs[0].qty;
            hours_per_unit[wh] = qty > 0.0 ? used_here / qty : 1.0;
        } else {
            out.keeper_used += used_here;
        }
    }

    // 3. Кто их купил. Сделка — Transfer с приёмкой внутри; платит покупатель,
    //    и платит он ровно износ (records::meets_machine_hours). Календарь смен
    //    (ИР-013) продаётся порциями — тогда покупателю ушла ДОЛЯ ресурса этой
    //    записи, а не весь: сколько именно, говорит звено отпуска.
    std::map<RefHash, const records::Release*> release_by_hash;
    for (const auto& r : deals.releases) release_by_hash[r.hash] = &r.release;

    std::map<UserId, ResourceHolder>       by_chain;
    std::map<UserId, std::set<RefHash>>   deals_of;    // дольщик → все его покупки
    std::map<RefHash, double>             deal_scale;  // покупка → моточасов в единице
    std::map<RefHash, double>             sold_share;  // работа-продажа → доля оплаченного
    for (const auto& d : deals.all) {
        const auto s = sale_hours.find(d.accept().work.hash);
        if (s == sale_hours.end()) continue;
        if (!deals.moves_hours(d.hash)) continue;      // ничтожная сделка ничего не двигает
        double share = 1.0;                            // куплено целиком
        if (const auto& rr = d.accept().release) {
            const auto r = release_by_hash.find(rr->hash);
            if (r != release_by_hash.end() && r->second->lot) {
                const auto w = deals.works.find(d.accept().work.hash);
                const auto f = w != deals.works.end()
                    ? records::portion_fraction(w->second.record, *r->second->lot)
                    : std::optional<double>{};
                if (f) share = *f;
            }
        }
        const double hours = s->second * share;
        auto& h = by_chain[d.payer];
        h.chain  = d.payer;
        h.hours += hours;
        h.paid  += d.accept().carried_units ? *d.accept().carried_units : 0.0;
        h.deal   = d.hash;              // для показа — последняя
        deals_of[d.payer].insert(d.hash);
        const auto hp = hours_per_unit.find(s->first);
        deal_scale[d.hash] = hp == hours_per_unit.end() ? 1.0 : hp->second;
        out.sold_hours   += hours;
        sold_share[s->first] += share;
    }
    // Непроданное из уже выпущенного: ресурс записан нитью и обещан календарём,
    // но ещё никем не куплен.
    for (const auto& [wh, hours] : sale_hours) {
        const auto it = sold_share.find(wh);
        const double left = hours * (1.0 - (it == sold_share.end() ? 0.0 : it->second));
        if (left > 1e-9) out.pending_hours += left;
    }

    // 4. Сколько дольщик уже истратил. Купленные часы он заводит материалом
    //    своей покупкой (records.md §10.1 v3) и тратит нитью переноса — значит
    //    израсходованное читается прямо по звеньям той записи.
    std::map<RefHash, std::pair<UserId, double>> hour_assets;  // запись → чья и масштаб
    for (const auto& a : deals.assets) {
        // Покупок у дольщика может быть несколько — и смена, и запас часов;
        // материал ищем по ВСЕМ его покупкам этой вещи.
        const auto it = deals_of.find(a.owner);
        if (it == deals_of.end() || !it->second.count(a.src.hash)) continue;
        const auto sc = deal_scale.find(a.src.hash);
        hour_assets[a.hash] = {a.owner, sc == deal_scale.end() ? 1.0 : sc->second};
    }
    for (const auto& [wh, w] : deals.works) {
        (void)wh;
        for (const auto& ce : w.record.carry) {
            const auto a = hour_assets.find(ce.src.hash);
            if (a == hour_assets.end() || !(a->second.first == w.worker)) continue;
            by_chain[w.worker].spent += ce.used * a->second.second;
        }
    }

    for (auto& [c, h] : by_chain) {
        (void)c;
        h.left = std::max(0.0, h.hours - h.spent);
        out.holders.push_back(h);
    }
    std::sort(out.holders.begin(), out.holders.end(),
              [](const ResourceHolder& a, const ResourceHolder& b) {
                  if (a.hours != b.hours) return a.hours > b.hours;
                  return a.chain < b.chain;
              });

    const double disposed = out.sold_hours + out.keeper_used + out.pending_hours;
    out.free_hours = out.life - disposed;
    // Ресурсом распорядились больше, чем его есть: протокол физически не
    // помешает держателю работать на том, что продано, — но это видно числом
    // (ИР-013: отказ в доступе и перебор одинаково светятся в следе).
    if (out.free_hours < -1e-9) {
        out.over_used  = true;
        out.free_hours = 0;
    }
    return out;
}

} // namespace aggregator
