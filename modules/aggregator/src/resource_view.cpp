#include "aggregator/resource_view.h"

#include <records/codec.h>

#include <algorithm>
#include <map>

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
        if (records::is_machine_hours(w.record)) sale_hours[wh] = used_here;
        else                                     out.keeper_used += used_here;
    }

    // 3. Кто их купил. Сделка — Transfer с приёмкой внутри; платит покупатель,
    //    и платит он ровно износ (records::meets_machine_hours).
    std::map<UserId, ResourceHolder> by_chain;
    std::map<RefHash, RefHash>       deal_of_sale;     // работа-продажа → сделка
    for (const auto& d : deals.all) {
        const auto s = sale_hours.find(d.accept().work.hash);
        if (s == sale_hours.end()) continue;
        if (!deals.moves_hours(d.hash)) continue;      // ничтожная сделка ничего не двигает
        auto& h = by_chain[d.payer];
        h.chain  = d.payer;
        h.hours += s->second;
        h.paid  += d.accept().carried_units ? *d.accept().carried_units : 0.0;
        h.deal   = d.hash;
        out.sold_hours += s->second;
        deal_of_sale[s->first] = d.hash;
    }
    for (const auto& [wh, hours] : sale_hours)
        if (!deal_of_sale.count(wh)) out.pending_hours += hours;   // отпущено, не оплачено

    // 4. Сколько дольщик уже истратил. Купленные часы он заводит материалом
    //    своей покупкой (records.md §10.1 v3) и тратит нитью переноса — значит
    //    израсходованное читается прямо по звеньям той записи.
    std::map<RefHash, UserId> hour_assets;             // запись часов → чья
    for (const auto& a : deals.assets) {
        const auto it = by_chain.find(a.owner);
        if (it == by_chain.end()) continue;
        if (!(a.src.hash == it->second.deal)) continue;
        hour_assets[a.hash] = a.owner;
    }
    for (const auto& [wh, w] : deals.works) {
        (void)wh;
        for (const auto& ce : w.record.carry) {
            const auto a = hour_assets.find(ce.src.hash);
            if (a == hour_assets.end() || !(a->second == w.worker)) continue;
            by_chain[w.worker].spent += ce.used;
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
