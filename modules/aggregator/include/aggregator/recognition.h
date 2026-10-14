#pragma once

// The deals on the table (records.md §9.5, ИР-023). A deal is a Transfer that
// carries the acceptance it pays for: appraisal and payment in one record, so it
// is paid exactly by construction and the acceptor is the payer. It happens only
// when the seller addressed the result to that payer — the work's customer
// (records.md §9.4) or a live release to them (records.md §9.8); a deal nobody
// addressed is void. A result is the work as a whole, one portion of one of its
// lots (records.md §9.8 v2), or a portion of somebody's purchase sold on — a
// resale, carried cost only, counted in no labor statistic (v4). A deal counts
// only on a work in its
// worker's goods thread, on hours no other work of that chain claims
// (records.md §9.4 v4) — for a portion, also on a link of its lot that checks
// out. Since v6 (economy.md §4.4) a deal is an offer: it happens only once its
// seller accepted it by its term, and only if every portion of someone else's
// paper is a spend its bill covers. Every view that reads deals reads them here,
// so that no two views disagree about which deal happened. The rules themselves
// are records' — the ones the client applies (records/acceptance.h,
// records/goods_thread.h, records/settlement.h); this only gathers the seller's
// records, the buyers' claims and the answers.

#include "aggregator.h"

#include <records/acceptance.h>
#include <records/carry.h>
#include <records/goods_thread.h>
#include <records/settlement.h>
#include <records/types.h>

#include <array>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace aggregator {

struct DealEntry {
    std::array<uint8_t, 32> hash{};    // the deal block: the Transfer carrying the acceptance
    UserId                  payer{};   // the chain that wrote it — who accepted and paid
    records::Transfer       transfer;  // transfer.accept is always set
    uint32_t                node  = 0; // the paying branch — the purse it spends from
    uint64_t                index = 0; // the block's position in that branch

    const records::Acceptance& accept() const noexcept { return *transfer.accept; }
};

// A move of someone else's paper between own branches (economy.md §5а): a
// Transfer without an acceptance, from the chain to itself. Nobody answers it.
struct MoveEntry {
    std::array<uint8_t, 32> hash{};
    UserId                  owner{};
    records::Transfer       transfer;  // from == to == owner, no accept
    uint32_t                node  = 0; // the branch it spends from
    uint64_t                index = 0;
};

// A seller's answer to a deal (records.md §11.13), and the block it lies in.
struct ReplyEntry {
    std::array<uint8_t, 32> hash{};
    records::ObservedReply  seen;
};

// One bill (economy.md §4.4): whose hands, the block the paper came in, whose
// paper. Spends by anyone but the one it came to are not spends of this bill.
using BillKey = std::tuple<std::array<uint8_t, 32>,    // holder
                           std::array<uint8_t, 32>,    // src chain
                           std::array<uint8_t, 32>,    // src block hash
                           std::array<uint8_t, 32>>;   // issuer
struct BillOnTable {
    uint32_t           purse = 0;      // the branch the paper came to
    records::BillState state;
    std::string        why;            // why nothing came at all: no source, or it did not happen
    bool               upstream = false;  // the source itself fell through — not the holder's lie
};

// The seller's side of a deal.
struct WorkOnTable {
    UserId              worker{};   // the chain that signed the WorkRecord
    records::WorkRecord record;     // its customer (v3), thread link and proposed axes (v4), lots (v5)
};

struct ReleaseEntry {
    std::array<uint8_t, 32> hash{};    // the Release block
    UserId                  seller{};  // who wrote it: the worker of its work, or the holder of a purchase
    records::Release        release;
};

struct WriteOffEntry {
    std::array<uint8_t, 32> hash{};    // the WriteOff block
    UserId                  seller{};  // who wrote it: the worker of its work, or the holder of a purchase
    records::WriteOff       writeoff;
};

// A lot (records.md §9.8): one output of one work — the work's block hash and
// the output's index — or one lot of a purchase sold on (v4) — the purchase's
// deal hash and the product's output index.
using LotKey = std::pair<std::array<uint8_t, 32>, uint32_t>;
// Запись средства производства или материала на столе (ИР-011, ИР-015): её
// стоимость, сколько из неё уже перенесено в продукцию и подкреплён ли
// перевыпуск. Вниз стоимость перевыпускают всегда, вверх — только на принятый и
// оплаченный ремонт (records.md §10.2).
struct AssetRecord {
    UserId                      owner;
    double                      cost = 0;
    std::optional<records::Ref> origin;
    std::optional<records::Ref> repair;
    double                      collected = 0;   // собрано нитью переноса
    bool                        sound = true;
    // Вторая шкала износа (ИР-011 v5, решение участника 2026-10-10):
    // календарный срок записи и день, с которого он идёт. Остаток — по
    // БОЛЬШЕМУ из двух износов, и это же потолок перевыпуска.
    uint32_t                    days = 0;
    int64_t                     started_at = 0;

    // Остаток к возмещению на момент `at` (records.md §10.2).
    double residual(int64_t at) const noexcept {
        return records::residual_value(cost, collected, days, started_at, at);
    }
};

// Купленное, заведённое в своё производство (records.md §10.1 v3, ИР-023):
// запись материала или средства производства, взятая звеном подниточки своей
// покупки. Стоимость такой записи не объявляют — это доля уплаченного за взятое.
struct AssetEntry {
    std::array<uint8_t, 32> hash{};        // the Material or Tool block
    UserId                  owner;
    records::Ref            src;           // the purchase it was taken from
    records::LotLink        lot;
    double                  cost = 0;      // what the record claims
    int64_t                 timestamp = 0; // the block's own time
    bool                    sound = false; // the claim checks out against the deal
};


struct DealsOnTable {
    std::vector<DealEntry>   all;          // every deal in the storage, in block-hash order
    records::DealRecognition recognition;  // which of them count, which are void
    std::map<std::array<uint8_t, 32>, WorkOnTable> works;   // every WorkRecord by block hash
    std::vector<ReleaseEntry>  releases;   // releases written by the worker of their work
    std::vector<WriteOffEntry> writeoffs;  // write-offs written by the worker of their work
    std::map<UserId, records::LaborThread> threads;   // each chain's goods thread, labor links
    std::map<LotKey, records::LotState>    lots;      // every lot with links, its sub-thread checked
    std::set<std::array<uint8_t, 32>> taken;          // releases an addressed deal completed
    std::set<std::array<uint8_t, 32>> sold_releases;  // releases of portions a deal paid for
    std::set<std::array<uint8_t, 32>> sold_whole;     // works whose whole result a deal paid for
    // Purchases of goods that happened — the buyers' stock, to sell on (v4).
    std::map<std::array<uint8_t, 32>, std::vector<records::HoldingLot>> holdings;
    // Purchases bought back by their addressee on the pre-signed return (v5).
    std::set<std::array<uint8_t, 32>> bought_back;
    // Записи средств производства и материалов, заведённые своей покупкой (v3).
    std::vector<AssetEntry> assets;
    // Все записи средств производства и материалов: стоимость, собранное нитью
    // переноса и законность перевыпуска (ИР-011, ИР-015).
    std::map<std::array<uint8_t, 32>, AssetRecord> asset_records;
    // Согласия на записи (ИР-012, решение участника 2026-10-10): запись → кто
    // согласен (true) или против (false). Общего объекта «решение» нет — порог
    // считается здесь, по N подписанным записям. Позднее согласие той же цепи
    // отменяет прежнее (по времени блока), как у реакций (records.md §8.5).
    std::map<std::array<uint8_t, 32>, std::map<UserId, bool>> consents;

    // ── Settlement (economy.md §4.4, records.md §11.13) ────────────────────
    // A deal is an offer until the seller answers it by its term; paper someone
    // else issued is spent from bills, and a spend no bill covers is no paper.
    std::vector<MoveEntry> moves;      // moves between own branches, written by their owner
    std::map<std::array<uint8_t, 32>, std::vector<ReplyEntry>> replies;   // deal → answers seen
    std::map<std::array<uint8_t, 32>, records::DealAnswer>     answers;   // deal → the answer that counts
    std::set<std::array<uint8_t, 32>> pre_accepted;   // buybacks paid with agreed paper: no answer needed
    std::set<std::array<uint8_t, 32>> unbacked;       // deals and moves spending paper no bill covers
    std::set<std::array<uint8_t, 32>> unbacked_upstream;   // ...only because a source fell through
    std::map<std::array<uint8_t, 32>, std::string> unbacked_why;
    std::set<std::array<uint8_t, 32>> moves_backed;   // moves that stand
    std::map<BillKey, BillOnTable>    bills;

    // The seller's answer that counts for a deal (none: silent, late, or not his).
    records::Answer answer(const std::array<uint8_t, 32>& deal) const {
        if (pre_accepted.count(deal)) return records::Answer::accepted;
        const auto a = answers.find(deal);
        return a == answers.end() ? records::Answer::none : a->second.answer;
    }
    // The reply block that counted, if any.
    const ReplyEntry* counted_reply(const std::array<uint8_t, 32>& deal) const {
        const auto a = answers.find(deal);
        const auto r = replies.find(deal);
        if (a == answers.end() || !a->second.reply || r == replies.end() ||
            *a->second.reply >= r->second.size())
            return nullptr;
        return &r->second[*a->second.reply];
    }

    // Кто согласился с этой записью (без тех, кто написал «против»).
    std::set<UserId> consented(const std::array<uint8_t, 32>& target) const {
        std::set<UserId> out;
        const auto it = consents.find(target);
        if (it == consents.end()) return out;
        for (const auto& [chain, agreed] : it->second)
            if (agreed) out.insert(chain);
        return out;
    }

    // The deal that counts for its result: statistics read only these.
    bool counts(const std::array<uint8_t, 32>& deal) const {
        return recognition.recognized.count(deal) > 0;
    }
    // Hours really moved: every addressed deal. Money views read these; a void
    // deal neither pays nor settles anything.
    bool moves_hours(const std::array<uint8_t, 32>& deal) const {
        return recognition.moves_hours(deal);
    }
    // Перенос работы считается, только если записи средств, из которых он идёт,
    // законны: перевыпуск вверх — лишь на оплаченный ремонт (records.md §10.2).
    // Записи, которой нет на столе, верим — это пробел, а не вина.
    bool carry_sound(const std::array<uint8_t, 32>& work) const {
        const auto w = works.find(work);
        if (w == works.end()) return true;
        for (const auto& ce : w->second.record.carry) {
            const auto a = asset_records.find(ce.src.hash);
            if (a != asset_records.end() && !a->second.sound) return false;
        }
        return true;
    }
    // A work whose deal may count: on the table in the chain its ref names, in
    // that chain's thread, on hours of its own (records.md §9.4 v4).
    bool sound(const records::Ref& work) const {
        const auto w = works.find(work.hash);
        if (w == works.end() || w->second.worker.bytes != work.chain) return false;
        const auto t = threads.find(w->second.worker);
        return t != threads.end() && t->second.sound(work.hash);
    }
};

// One pass over the storage. A deal counts as written only by its own sender, a
// release or write-off only by the worker of its work (the spoof guards every
// view applies); the codec has already refused a deal that does not pay
// exactly, pays itself or pays anyone but the worker. A deal moves hours only
// when, besides, its seller accepted it by its term (or a buyback needed no
// answer) and every portion of someone else's paper is a spend its bill covers
// (economy.md §4.4).
DealsOnTable read_deals(const AggregatorStorage& storage);

// How a deal stands at `now`, as one word for the views: "recognized",
// "resold", "repeated", "contested", "unsound" (hours moved), "unaddressed"
// (void), or for an offer that moved nothing — "pending", "refused",
// "expired", "unbacked".
const char* deal_standing(const DealsOnTable& deals, const DealEntry& d, int64_t now);

// One output of a work as a lot (records.md §9.8 v2): how much of it went where.
struct LotSummary {
    uint32_t    out = 0;
    double      qty = 0;          // the output's size
    double      sold = 0;         // portions a deal paid for
    double      written_off = 0;  // write-offs
    double      consumed = 0;     // v3: взято в своё производство (records.md §10.1)
    double      reserved = 0;     // released and not yet paid for (live, or expired and not returned)
    double      remaining = 0;    // still in stock
    std::size_t faults = 0;       // links of the lot that do not check out
    bool        gaps   = false;
    std::vector<std::string> free_units;   // a numbered lot: the units still in stock
};
std::vector<LotSummary> lot_summaries(const DealsOnTable& deals,
                                      const std::array<uint8_t, 32>& work);

// The lots of a purchase as sold on (records.md §9.8 v4).
std::vector<LotSummary> holding_summaries(const DealsOnTable& deals,
                                          const std::array<uint8_t, 32>& purchase);

// Hours of a work no deal has confirmed yet (records.md §9.4 v4): none once its
// whole result was paid for; otherwise the part of its lots neither sold nor
// written off — written-off labor is lost, not waiting.
double unconfirmed_hours(const DealsOnTable& deals, const std::array<uint8_t, 32>& work);

} // namespace aggregator
