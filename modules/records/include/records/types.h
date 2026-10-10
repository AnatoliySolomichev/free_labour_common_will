#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace records {

// ── Discriminator values (records.md §3) ─────────────────────────────────────

enum class RecordType : uint8_t {
    // Structural / protocol
    FraudClaim   = 0x01,
    // Knowledge graph
    Concept      = 0x40,
    ConceptLink  = 0x41,
    Composite    = 0x42,
    Copy         = 0x43,
    Reaction     = 0x44,
    Attest       = 0x45,   // пломба: «я видел это состояние» (blockchain.md §7)
    Consent      = 0x46,   // согласие на чужую запись: «я не против» (ИР-012)
    // Labor
    // 0x50 Specialty, 0x51 Grade, 0x52 Worker — сняты (ИР-022 шаг 11): какой
    //      была работа, говорит разбивка её цены по осям, а не справочник и
    //      не ступень лестницы (records.md §9.1); номера не переиспользуются.
    WorkRecord   = 0x53,
    // 0x54 — Acceptance, снята (2026-09-14, ИР-023): приёмка — поле перевода
    //        `Transfer.accept`, оценка и оплата одной записью (records.md §9.5);
    //        номер не переиспользуется.
    Release      = 0x57,   // отпуск: продавец называет покупателя (records.md §9.8)
    WriteOff     = 0x58,   // списание части партии (records.md §9.9)
    // Production
    Material     = 0x60,
    Tool         = 0x61,
    // Economy
    Transfer       = 0x70,
    DailyAggregate = 0x71,
    Pledge         = 0x72,
    PledgeRevoke   = 0x73,
    Redemption     = 0x74,
    WorkCloud      = 0x75,
    // 0x76 — AxisAttestation, снята: заверяла значения осей `x`, а у `x` не
    //        осталось читателя — облако стоит на разбивках сделок
    //        (records.md §11.8); номер не переиспользуется.
    // 0x77 — AxisPrices, снята вместе с регрессией (records.md §11.9);
    //        номер не переиспользуется.
    // 0x78 — DealProfile, снята: денег не несла, читателя не осталось
    //        (records.md §11.10); номер не переиспользуется.
    AxisDef         = 0x79,
};

// ── Cross-chain reference (records.md §4) ────────────────────────────────────

// Points to a specific block in any user's blockchain.
// chain: UserId (root public key, 32 bytes)
// hash:  BLAKE2b-256 of the referenced block
struct Ref {
    std::array<uint8_t, 32> chain{};
    std::array<uint8_t, 32> hash{};

    bool operator==(const Ref& o) const noexcept {
        return chain == o.chain && hash == o.hash;
    }
    bool operator!=(const Ref& o) const noexcept { return !(*this == o); }
};

// The FULL path to a block: whose chain, which branch of its binary tree, which
// position in that branch — plus the content hash, so the path can be checked.
// Same shape as the blockchain layer's ExternalRef, which the protocol already
// uses to address merge leaves.
//
// Why both. By protocol a branch cannot hold two blocks at one index
// (blockchain.md §9.5), so for a VALID chain the address alone is unique, and it
// lets anyone walk to the block by hand without any index. But a stolen key, or
// an owner forking their own branch, can still SIGN a second block at the same
// address (blockchain.md §6.7) — not a valid block, evidence of fraud, yet it
// exists. The hash says which of the two was meant, and proves the block has
// not been re-signed since.
struct BlockPath {
    std::array<uint8_t, 32> chain{};   // UserId: root public key of the owner
    uint32_t                node  = 0; // heap index of the leaf node — the branch
    uint32_t                block = 0; // position of the block in that branch
    std::array<uint8_t, 32> hash{};    // BLAKE2b-256 of the block, as a check

    bool operator==(const BlockPath& o) const noexcept {
        return chain == o.chain && node == o.node && block == o.block && hash == o.hash;
    }
    bool operator!=(const BlockPath& o) const noexcept { return !(*this == o); }
};

// ── Structural / protocol (records.md §3A) ───────────────────────────────────

// Accusation that a merge block's committed snapshot is cryptographically bogus.
// The proof is an opaque blob defined by the blockchain layer (a serialized
// FraudProofData); records carries it without interpreting it. Verification is
// done by blockchain::FraudProof::verify(kind, proof, merkle_root_of(target)).
struct FraudClaim {
    static constexpr RecordType TYPE = RecordType::FraudClaim;

    Ref                  target;   // accused merge block (chain + block hash)
    std::string          kind;     // "bad_sig" | "hash_mismatch"
    std::vector<uint8_t> proof;    // opaque proof blob (blockchain FraudProofData)
    std::string          reason;   // human-readable note (may be empty)
};

// ── Knowledge graph (records.md §8) ──────────────────────────────────────────

struct Concept {
    static constexpr RecordType TYPE = RecordType::Concept;

    std::string              text;
    std::vector<std::string> tags;   // may be empty
};

struct ConceptLink {
    static constexpr RecordType TYPE = RecordType::ConceptLink;

    Ref         from;
    Ref         to;
    std::string kind;   // "уточняет", "противоречит", "применяет", "порождает", ...
};

struct Composite {
    static constexpr RecordType TYPE = RecordType::Composite;

    std::string       title;
    std::vector<Ref>  parts;  // Concept / ConceptLink / other Composite refs
};

// Copy of someone else's record — required before reacting to it (records.md §8.4)
struct Copy {
    static constexpr RecordType TYPE = RecordType::Copy;

    Ref source;  // original record in another chain
};

struct Reaction {
    static constexpr RecordType TYPE = RecordType::Reaction;

    Ref    target;  // Copy or own record
    int8_t value;   // -128..+127  (negative = disagree, 0 = neutral, positive = agree)
};

// Пломба (blockchain.md §7.4 v2, решение участника 2026-10-10): одностороннее
// «я видел это состояние», записанное БЛОКОМ в ветке самого свидетеля, а не
// строкой в отдельной таблице. Из формы следует всё, чего у таблицы не было:
// у пломбы есть место во времени (задним числом не вставить, от своей не
// отпереться), её можно сосчитать, и чья она — говорит сама цепь, в которой
// она лежит, без привязки ключа к цепи (blockchain.md §7.5).
//
// Заверение чужой работы теперь и стоит чего-то свидетелю: ложная пломба
// остаётся в его собственной истории рядом со всем, что он о себе утверждает
// (records.md §12.11).
struct Attest {
    static constexpr RecordType TYPE = RecordType::Attest;

    Ref         target;  // что заверено: цепь и хеш блока
    std::string mode;    // "blind" — видел только хеш; "open" — видел содержание
    std::string note;    // что именно видел; может быть пустым
};

// Согласие на чужую запись (ИР-012, форма «каждый пишет у себя», решение
// участника 2026-10-10): «я не против — считайте это общим». Пишется в СВОЕЙ
// ветке, как пломба, но говорит другое: пломба — «я видел», согласие — «я
// согласен, действуй». Отказ пишется тем же типом (`agreed = false`), и это
// нарочно: и согласие, и отказ равно остаются в истории подписавшего.
//
// Общего объекта «решение артели» не образуется: «собрался ли порог» —
// ВЫЧИСЛЯЕМЫЙ ФАКТ из N подписанных записей, как признанность сделки. Иначе
// этот объект пришлось бы кому-то подписывать — и мы вернулись бы к вопросу
// «кому» (ИР-012: привилегированной цепи нет).
//
// Чем это полезно: редкие дорогие решения об общей вещи — ремонт, продажа
// самой вещи, выпуск календаря смен (ИР-013) — держатель составляет записью в
// своей цепи, а совладельцы подписывают согласие у себя. Перенос засчитывается
// только в записи владельца, поэтому «выпустить вместе» иначе и не выразить.
struct Consent {
    static constexpr RecordType TYPE = RecordType::Consent;

    Ref         target;          // с чем согласен: цепь и хеш записи
    bool        agreed = true;   // true — согласен, false — против
    std::string note;            // чем именно; может быть пустым
};

// ── Labor records (records.md §9) ────────────────────────────────────────────

// Named resource with quantity and unit — used in WorkRecord inputs/outputs.
// An output is a lot (records.md §9.4 v5, ИР-023): its identity is the path to
// the WorkRecord plus its index, and it is sold whole or in portions — links of
// the lot's sub-thread (records.md §9.8 v2).
struct ResourceQty {
    std::string resource;  // name or description of the material/product
    double      qty = 0;
    std::string unit;      // "кг", "м³", "шт", "л", ...
    // Outputs only (v5). `share` — the part of the work's hours this output
    // carries when one work yields several (the sawmill's boards and sawdust):
    // declared by the producer, confirmed by the buyer; Σ over the outputs = 1.
    std::optional<double>    share;
    // v6: the price tag — labor-hours for the whole output, fixed at production
    // (the client computes it from k over the axis prices of the production day,
    // economy.md §2а, ИР-023). A portion from stock is sold at its share of the
    // tag, take it or leave it (records.md §9.8 v3).
    std::optional<double>    price;
    // v6: the numbers of the lot's units — a frame number, a stamp — for a lot
    // counted piece by piece (ИР-023, вопрос 10); qty is their count.
    std::vector<std::string> units;
    // v7: the service life, days, declared by the maker (records.md §9.4) — the
    // norm a rent's wear is counted by: straight-line from the thing's birth
    // (records.md §9.8 v6, ИР-023, пункт 8).
    std::optional<double>    life;

    bool operator==(const ResourceQty& o) const noexcept {
        return resource == o.resource && qty == o.qty && unit == o.unit
            && share == o.share && price == o.price && units == o.units && life == o.life;
    }
};

// One link of a carry thread (records.md §9.4 v2, ИР-011): the use of one
// means of production transfers a share of its UNRECOVERED cost into the
// work. One thread per Tool/Material record across the whole chain (prev is
// a Ref, it crosses branches); two links with one prev = equivocation =
// double-charging one asset over parallel branches.
struct CarryEntry {
    Ref                src;      // Tool (records.md §10.2) or Material batch (records.md §10.1)
    double             used;     // tool-hours worked / material quantity spent
    double             carried;  // labor-hours transferred into this work
    uint64_t           seq;      // link counter of the asset's carry thread
    std::optional<Ref> prev;     // previous link; absent for the first
    double             after;    // total carried over the asset's life; ≤ cost

    bool operator==(const CarryEntry& o) const noexcept {
        return src == o.src && used == o.used && carried == o.carried
            && seq == o.seq && prev == o.prev && after == o.after;
    }
};

// The labor link of the chain's goods thread (records.md §9.4 v4, ИР-023): one
// thread per chain, written from any branch. `seq` is one past the highest of
// `prev` — 0 for the first link, the only one without prev; several `prev`
// merge branches that continued the thread without seeing each other. The
// thread is what lets a buyer see every interval the worker ever claimed before
// paying for one of them.
struct ThreadLink {
    uint64_t         seq = 0;
    std::vector<Ref> prev;   // strictly ascending by (chain, hash); empty ⇔ seq == 0

    bool operator==(const ThreadLink& o) const noexcept {
        return seq == o.seq && prev == o.prev;
    }
};

// The share of a work's hours the worker proposes for one axis (records.md §9.4
// v4): «труд по осям», not yet confirmed by anyone. The buyer confirms it or
// writes their own breakdown in the acceptance (records.md §9.5).
struct AxisShare {
    BlockPath axis;
    double    share = 0;   // > 0; Σ over the work = 1

    bool operator==(const AxisShare& o) const noexcept {
        return axis == o.axis && share == o.share;
    }
};

// A link of a lot's sub-thread (records.md §9.8 v2, ИР-023): a portion taken from
// one output of a work — released to a buyer or written off. One sub-thread per
// lot across the whole chain (prev is a Ref, it crosses branches) and strict,
// like the carry thread: two links with one seq are two sales from one
// remainder. `after` checks out against the previous link alone, so the
// remainder of a lot is known in O(1).
struct LotLink {
    uint32_t           out   = 0;   // index into the work's outputs
    double             qty   = 0;   // taken by this link, in the output's unit
    uint64_t           seq   = 0;   // link counter of the lot's sub-thread
    std::optional<Ref> prev;        // the previous link; absent for the first
    double             after = 0;   // taken from the lot after this link; ≤ the output's qty
    std::vector<Ref>   returns;     // expired releases of this lot nobody took — back in stock
    // v3: the numbered units taken, for a lot counted piece by piece; qty is
    // their count. A unit is sold once (records.md §9.8).
    std::vector<std::string> units;

    bool operator==(const LotLink& o) const noexcept {
        return out == o.out && qty == o.qty && seq == o.seq && prev == o.prev
            && after == o.after && returns == o.returns && units == o.units;
    }
};

// v7 (ИР-023): звено расхода своей продукции — какая своя работа и сколько из её
// партии ушло в эту. Подниточка та же, что у продаж и списаний: что израсходовано,
// то уже не продать; а в изделие приходит не цена детали, а её труд.
struct SpendLink {
    Ref     work;   // своя запись труда, чей выход расходуется
    LotLink lot;    // звено подниточки её партии

    bool operator==(const SpendLink& o) const noexcept {
        return work == o.work && lot == o.lot;
    }
};

// One act of work (records.md §9.4). The worker is the chain that signed the
// block. There is no reference to a specialty or a grade (ИР-022 шаг 11): what
// the work was is said by the breakdown of its price in the Acceptance
// (records.md §9.5), not by a catalogue slug or a rung of a ladder.
struct WorkRecord {
    static constexpr RecordType TYPE = RecordType::WorkRecord;

    std::string action;         // description of the work performed
    int64_t     start_ts = 0;   // Unix timestamp UTC
    double      hours    = 0;   // duration in hours

    std::vector<ResourceQty> inputs;   // may be empty
    std::vector<ResourceQty> outputs;  // may be empty
    std::vector<CarryEntry>  carry;    // cost carried from tools/materials (ИР-011)
    // «Кем работал» — free text for people («электрик, 5-й разряд»), a rudiment
    // of the specialty and the grade. Nothing computes on it. Empty = not
    // stated, and the field is then absent from the encoding.
    std::string              title;
    // v3 (ИР-023): the customer, when the work was ordered in advance — the
    // result is released to them by this very record (records.md §9.4), no
    // separate Release needed. Absent by default: work for stock, or without an
    // agreement, is released afterwards (records.md §9.8).
    std::optional<std::array<uint8_t, 32>> customer;
    // v4 (ИР-023): the labor link of the chain's goods thread. A work outside
    // the thread is not recognized for a deal (records.md §9.5): its buyer could
    // not have seen whether its hours were already sold.
    std::optional<ThreadLink> thread;
    // v4 (ИР-023): the worker's proposed breakdown of these hours by axis.
    std::vector<AxisShare>    axes;
    // v6 (ИР-015): работа над своей покупкой — ремонт, наладка, улучшение. Нового
    // изделия не рождается: дорожает сама вещь, и труд оплачивается той же
    // сделкой, которой её выкупают назад (records.md §9.8 v7).
    std::optional<Ref>        improves;
    // v7 (ИР-023): расход своей продукции в своё производство. Деталь не
    // становится «товаром с ценой»: в изделие приходит её труд — часы той работы,
    // — и подтверждает их покупатель готового изделия (records.md §9.4 v7).
    std::vector<SpendLink>    spends;
};

// An addressed buyout (records.md §9.8 v5, ИР-023, пункт 8): ownership and risk
// go to the buyer, but until `until` its next sale is recognized only to the
// addressee — the seller itself (storage, repair, rent) or a named recipient
// (carriage). The buyer pre-signs that return in the very purchase (Buyback).
struct Addressed {
    std::array<uint8_t, 32> to{};
    int64_t                 until = 0;
    // v6: a rent — the thing is used, so it comes back at what is left of it
    // after the wear by the norm, and nothing above the wear is paid
    // (records.md §9.8 v6). Only back to the seller itself.
    bool                    wear = false;

    bool operator==(const Addressed& o) const noexcept {
        return to == o.to && until == o.until && wear == o.wear;
    }
};

// A release (records.md §9.8, ИР-023): the seller gives the result of an own work
// to a named buyer. Only that buyer's deal can complete it, and only before
// `expires`. Without a release — or a customer on the WorkRecord itself — a deal
// on the work is void (records.md §9.5): who gets the result is decided by the
// one who made it, not by whoever is willing to pay.
struct Release {
    static constexpr RecordType TYPE = RecordType::Release;

    Ref                     work;           // own WorkRecord whose result is released
    std::array<uint8_t, 32> to{};           // the buyer — only their deal completes it
    int64_t                 expires   = 0;  // the deal counts only if its timestamp < expires
    int64_t                 timestamp = 0;
    // v2 (ИР-023): a portion of one output instead of the whole result — a link
    // of that lot's sub-thread (records.md §9.8).
    std::optional<LotLink>  lot;
    // v4 (ИР-023): the own purchase the portion comes from — a resale. `work`
    // still names the product's birth record: the serial does not change hands;
    // the lot link is one of the purchase's sub-thread.
    std::optional<Ref>      holding;
    // v5 (ИР-023, пункт 8): an addressed buyout — the buyer may sell on only to
    // the addressee until the term, and pre-signs that return in the very deal
    // by which it buys (Acceptance.buyback).
    std::optional<Addressed> addressed;
};

// A write-off (records.md §9.9, ИР-023): part of a lot leaves it without a sale —
// spoiled, lost, used up. A link of the lot's sub-thread like the release of a
// portion, so the remainder stays checkable and the loss stays visible in the
// seller's own chain.
struct WriteOff {
    static constexpr RecordType TYPE = RecordType::WriteOff;

    Ref                work;       // the WorkRecord whose output the lot is
    LotLink            lot;
    std::string        reason;     // «подгорели», «бой при перевозке»
    int64_t            timestamp = 0;
    std::optional<Ref> holding;    // v2: a bought lot written off — the own purchase
};

// v3 (economy.md §2б): the normalizer this appraisal was made against, so a
// historical оценка reads unambiguously ("10.5 стч at W=1.03") after the method
// changes. Points at the DailyAggregate used: aggregator chain + its date + W.
struct AcceptanceNorm {
    std::array<uint8_t, 32> agg;   // aggregator chain UID
    int64_t                 date;  // DailyAggregate date used
    double                  W;     // its normalizer

    bool operator==(const AcceptanceNorm& o) const noexcept {
        return agg == o.agg && date == o.date && W == o.W;
    }
};

// One line of the itemized price (ИР-022): how many of the deal's labour hours
// went to this axis. No intensity here and no share — just hours, because hours
// are what was paid.
//
// The axis is a BlockPath — the full path to the AxisDef record plus its hash —
// and NOT a slug. Slugs collide, and there is no principled way to pick between
// two chains that both call their axis "danger": the tie-break would be
// somebody's decision, which is exactly what this design has no room for. The
// path leads to the record that DEFINES the axis, so using an axis and pointing
// at its argument are the same act; anyone can walk it by hand, and the hash
// shows whether the block at that address is still the one that was meant.
struct AcceptanceAxis {
    BlockPath axis;  // the AxisDef record this line is about
    double units;    // labour hours of THIS deal's price attributed to it

    bool operator==(const AcceptanceAxis& o) const noexcept {
        return axis == o.axis && units == o.units;
    }
};

// The pre-signed return (records.md §9.8 v5, v6; ИР-023, пункт 8): buying on an
// addressed buyout, the buyer signs in that very deal that it releases what it
// bought to `to` until `until`. The addressee then buys back on its own, and no
// holder can stall it. Its price is nobody's to name (v6): what the holder paid
// — less, for a rent (`wear`), the wear by the norm over the holding.
struct Buyback {
    std::array<uint8_t, 32> to{};
    int64_t                 until = 0;
    bool                    wear  = false;

    bool operator==(const Buyback& o) const noexcept {
        return to == o.to && until == o.until && wear == o.wear;
    }
};

// The acceptance: what was received, how good, what it is worth and why
// (records.md §9.5). Since 2026-09-14 (ИР-023) not a record of its own but the
// `accept` field of the buyer's Transfer — the appraisal and its payment are one
// signed record, so the payment equals the appraisal by construction and the one
// who accepts is the one who pays. Who accepted, and when, is that Transfer's
// `from` and `timestamp`.
struct Acceptance {
    Ref         work;         // Ref to the WorkRecord, in the chain the Transfer pays
    std::string quality;      // e.g. "пройдено", "отклонено"
    double      hours_raw;    // raw hours from WorkRecord
    double      labor_units;  // price of live labour; by default hours × Σ network axis prices × k
    // v2 (ИР-011): Σ carried of the accepted work. The payment is labor_units +
    // carried_units exactly; rates take labor_units only (records.md §11.2).
    std::optional<double>          carried_units;
    // v3 (economy.md §2б): normalizer provenance, best-effort at accept time.
    std::optional<AcceptanceNorm>  norm;
    // v4 (ИР-022): the price, itemized by axis. Σ units MUST equal labor_units.
    //
    // This is what makes the breakdown protocol rather than commentary: it is
    // signed together with the price, in the same record, so nobody can accept a
    // deal and then remember a convenient story about it later. There is no
    // residual because there is nothing to add up beyond what is named here, and
    // nothing is inferred, so there is nothing to draw a profile against.
    //
    // Empty is not a record: the codec refuses to write it and to read it.
    std::vector<AcceptanceAxis>    axes;
    // The earlier acceptance whose breakdown this one repeats — «такая же
    // работа, как та» (ИР-022 шаг 11). Provenance only: the breakdown above is
    // still written out in full; `like` says where it came from. Acceptances
    // linked by `like` form the families the work cloud is made of
    // (records.md §11.7).
    std::optional<Ref>             like;
    // The release this deal completes (records.md §9.8, ИР-023): the seller's
    // record naming the payer as the buyer. Absent when the work itself names the
    // payer as its customer (records.md §9.4).
    std::optional<Ref>             release;
    // v5: the pre-signed return, when the release bought is an addressed
    // buyout (records.md §9.8 v5).
    std::optional<Buyback>         buyback;
};

// ── Production records (records.md §10, v2 — ИР-011) ─────────────────────────

// A batch of consumables with a carryable remainder (records.md §10.1): the batch cost
// flows into products as the quantity is spent. Field layout mirrors Tool;
// the thread capacity is the batch size qty.
struct Material {
    static constexpr RecordType TYPE = RecordType::Material;

    std::string        name;
    std::string        unit;    // "кг", "л", "кВт·ч", "шт", ...
    std::string        desc;    // may be empty
    double             cost;    // batch cost, labor-hours
    double             qty;     // batch size in unit (carry-thread capacity)
    std::string        basis;   // "paid" | "est"
    std::optional<Ref> src;     // paid: the purchase — the Transfer carrying its acceptance
    std::optional<Ref> origin;  // previous record of this batch (reissue)
    std::string        note;    // est: how the estimate was made; may be empty
    // v3 (ИР-023): купленное — в своё производство. The link of the purchase's
    // lot sub-thread this batch was taken by: what is spent into production is
    // no longer for sale, and the cost comes from the deal (records.md §10.1).
    std::optional<LotLink> lot;
    // v4 (ИР-011 v5, 2026-10-10): календарный срок партии — скоропортящееся
    // тоже изнашивается временем. Износ — больший из двух шкал, как у
    // инструмента (records.md §10.1).
    std::optional<uint32_t> days;
};

// A tool/equipment instance with cost and design life (records.md §10.2). Wear carries
// cost into products via WorkRecord.carry. Reissue (origin) covers resale,
// downward revaluation and re-entry: new cost ≤ previous remainder.
struct Tool {
    static constexpr RecordType TYPE = RecordType::Tool;

    std::string        name;
    std::string        desc;    // may be empty
    std::string        serial;  // instance id across chains; may be empty
    double             cost;    // acquisition cost, labor-hours
    double             life;    // design life, tool-hours (thread capacity)
    std::string        basis;   // "paid" | "est"
    std::optional<Ref> src;     // paid: the purchase — the Transfer carrying its acceptance
    std::optional<Ref> origin;  // previous record of this instance (reissue)
    std::string        note;    // est: how the estimate was made; may be empty
    // v3 (ИР-023): купленное — в своё производство, как у материала
    // (records.md §10.2): звено подниточки покупки, которым взят этот экземпляр.
    std::optional<LotLink> lot;
    // v4 (ИР-015): ремонт своего средства производства — единственный законный
    // путь стоимости вверх. При перевыпуске (`origin`) новая стоимость может
    // превысить остаток прежней ровно на принятый и оплаченный ремонт: `repair`
    // — своя сделка, которой он оплачен (records.md §10.2).
    std::optional<Ref>     repair;
    // v5 (ИР-011, решение участника 2026-10-10): ВТОРАЯ ШКАЛА ИЗНОСА —
    // календарный срок службы в днях, со дня этой записи. Износ считается по
    // той шкале, что дала больше: вещь изнашивается и работой, и временем,
    // поэтому простой перестаёт быть бесплатным. Правило «больший из двух»
    // делает шкалу безопасной — добавить её можно только себе в убыток:
    // остаток она способна лишь снизить (records.md §10.2).
    std::optional<uint32_t> days;
};

// ── Economy records (records.md §11) ─────────────────────────────────────────

// Named portion of labor-hours: `units` hours backed by the debt of `issuer`
// (records.md §12.7). Portions of different issuers never mix.
struct OriginQty {
    std::array<uint8_t, 32> issuer;  // debtor chain (UserId)
    double                  units;

    bool operator==(const OriginQty& o) const noexcept {
        return issuer == o.issuer && units == o.units;
    }
};

// One link of the chain-wide emission thread (economy.md §4.3): every
// self-issue ("−") and every redemption receipt ("+") carries a sequential
// number, a reference to the previous link (whatever branch it lives in) and
// the declared debt level after the operation. Two links with one seq =
// equivocation, an objective fraud proof.
struct EmissionLink {
    uint64_t           seq        = 0;  // chain-wide link counter
    std::optional<Ref> prev;            // previous link; absent for the first
    double             debt_after = 0;  // declared debt (negative = owing)

    bool operator==(const EmissionLink& o) const noexcept {
        return seq == o.seq && prev == o.prev && debt_after == o.debt_after;
    }
};

// The only way value moves (records.md §11.1). Lives in the sender's chain;
// the SPENDING branch is the branch the block is written into (its key signs
// the spend — per-branch purses, economy.md §5а). Debt stays chain-level:
// issuer == from → self-issue: a new debt/claim pair is born (records.md §12.2);
// issuer == to   → redemption: the paper returns to its debtor and annihilates;
// otherwise      → endorsement: someone else's paper passed along.
// A self-issuing transfer carries its emission-thread link (v3, economy.md §4.3).
//
// v5 (ИР-023, 2026-09-14): WHAT is paid for is not a reference to an acceptance
// but the acceptance itself — `accept`. A deal is one record: the appraisal and
// its payment signed together, so the payment equals the appraisal exactly and
// the one who accepts is the one who pays — «платит один, принимает другой» cannot
// even be written. A transfer without `accept` may only move own paper between
// own branches (economy.md §5а). The codec refuses anything else; the old
// `reason` (key 5) is retired. `settles` still names the Pledge a deal closes (v4).
struct Transfer {
    static constexpr RecordType TYPE = RecordType::Transfer;

    std::array<uint8_t, 32>   from;      // sender chain (= owner of this record) = the acceptor
    std::array<uint8_t, 32>   to;        // receiver chain = the worker
    uint32_t                  to_node;   // receiver branch — whose purse is credited
    std::vector<OriginQty>    origins;   // named portions; total = labor + carried of `accept`
    std::optional<Acceptance> accept;    // WHAT for: the acceptance itself (v5)
    std::optional<Ref>        settles;   // WHICH promise this closes: Pledge (v4)
    int64_t                   timestamp; // Unix timestamp UTC
    // Present iff origins contain a self-issued portion (issuer == from).
    std::optional<EmissionLink> emission;
};

// Issuer's receipt for own paper returned by a payer's Transfer — the "+" link
// of the emission thread (records.md §11.5, economy.md §4.3): the debt shrinks
// by `units`. Written by the issuer upon receiving own paper; a chain's credit
// history (max closed debt, repayment speed) reads straight off the thread.
struct Redemption {
    static constexpr RecordType TYPE = RecordType::Redemption;

    Ref          transfer;   // the payer's Transfer that returned the paper
    double       units;      // own paper annihilated
    EmissionLink link;       // mandatory: every redemption is threaded
    int64_t      timestamp;  // Unix timestamp UTC
};

// Public promise to pay (part of) the cost of future work (records.md §11.3).
// Transfers nothing by itself: only settlement Transfers (reason → this
// pledge) carry weight. Revocable until settled; auto-expires.
struct Pledge {
    static constexpr RecordType TYPE = RecordType::Pledge;

    Ref                                    target;    // funded work/idea
    double                                 units;     // promised labor-hours
    std::optional<std::array<uint8_t, 32>> executor;  // specific chain, if any
    std::optional<int64_t>                 expires;   // after this — auto-revoked
    int64_t                                timestamp; // Unix timestamp UTC
};

// Revokes the unsettled remainder of an own pledge (records.md §11.4).
struct PledgeRevoke {
    static constexpr RecordType TYPE = RecordType::PledgeRevoke;

    Ref     pledge;     // own Pledge record
    int64_t timestamp;  // Unix timestamp UTC
};

// The network's price of an hour of one axis, for one day (records.md §11.2 v5).
//
// v5 (ИР-022 шаг 11): there are no specialty rates any more. A deal names what
// it paid for, axis by axis, so the network publishes what an hour of each axis
// pays — and the default price of a work is the sum over the axes it names. A
// specialty was a label a person picked from a list; an axis is what the deal
// itself said.
struct AxisPrice {
    BlockPath axis;             // the AxisDef record (ИР-022)
    double    price;            // стч per hour of work naming it, smoothed, RAW (÷ W)
    double    hours;            // Σ hours_raw of the day's settled deals naming it
    uint64_t  deals;            // how many of them
    // Σ weight·hours_raw — the volume that survived counterparty-independence
    // weighting (ИР-021). credibility = weighted_hours / hours ∈ [0,1].
    double    weighted_hours = 0.0;

    bool operator==(const AxisPrice& o) const noexcept {
        return axis == o.axis && price == o.price && hours == o.hours
            && deals == o.deals && weighted_hours == o.weighted_hours;
    }
};

// Signed daily axis prices, written into the aggregator's own chain
// (records.md §11.2): the block author IS the aggregator. Only settled deals
// count; every figure is re-checkable against the chains.
struct DailyAggregate {
    static constexpr RecordType TYPE = RecordType::DailyAggregate;

    int64_t                date;       // UTC day start (ts − ts mod 86400)
    std::vector<AxisPrice> axes;       // v5: RAW prices (normalized = price / W)
    int64_t                timestamp;  // Unix timestamp UTC
    // economy.md §2б: the network's average paper per labour-hour —
    // Σ price·hours over the axes / Σ hours of the day's deals. The client divides
    // by W so the average labour-hour equals 1.
    double                 W = 1.0;
    // ИР-021: the counterparty-independence parameters this aggregate was
    // computed with, so a witness recomputes by the same methodology instead of
    // guessing it — the same discipline as WorkCloud::params.
    std::string            indep;
};

// One neighbour of a sample in the work cloud: another sample and how much the
// two are paid for the same things (0..1).
struct CloudNeighbor {
    Ref    sample;   // the neighbour sample — an Acceptance
    double weight;   // affinity 0..1 (1 = the same composition of price)

    bool operator==(const CloudNeighbor& o) const noexcept {
        return sample == o.sample && weight == o.weight;
    }
};

// One point of the work cloud (ИР-022 шаг 11): a SAMPLE — an acceptance other
// deals named with `like`, «такая же работа, как та» — the size of its family and
// its k nearest samples. A profession is no longer a line in a catalogue: it is a
// description of work that people keep reusing. Where a sample sits is read off
// its family's breakdowns, which the snapshot commits (records.md §11.7).
struct CloudPoint {
    Ref                        sample;     // the sample acceptance
    std::string                title;      // its work's free-text title, for people
    uint64_t                   deals = 0;  // the family: the sample and every deal naming it
    std::vector<CloudNeighbor> neighbors;  // k nearest, by affinity of price composition
};

// Signed work cloud, written into the aggregator's own chain — twin of
// DailyAggregate (specialty-axes.md §10). `snapshot` commits the input block set
// so anyone recomputes and verifies.
struct WorkCloud {
    static constexpr RecordType TYPE = RecordType::WorkCloud;

    int64_t                             date;      // UTC day start
    std::array<uint8_t, 32>             snapshot;  // hash of the input block set
    std::string                         params;    // algorithm version + parameters
    std::vector<std::array<uint8_t, 32>> sources;  // chains the input came from
    std::vector<CloudPoint>             points;
    int64_t                             timestamp; // Unix timestamp UTC
};

// An axis, defined in somebody's chain (ИР-022).
//
// The vocabulary of labour is not the aggregator's to decide. It used to be a
// file the aggregator shipped, so whoever edited that file decided which facets
// of work exist — the last centralized point left in the valuation of labour,
// after the VALUES became practitioners' (ИР-019) and the PRICES became the
// deals' (ИР-020, ИР-022). Now an axis is an ordinary record: anyone writes one,
// anyone references it, and it gains force by being used, not by being blessed.
//
// THE ARGUMENT IS THE POINT. Nothing stops anyone inventing an axis, so what
// separates a good one from a bad one is how well it is argued: what it means,
// why it is one axis and not two, what it is not. An axis reused again and again
// with nothing written behind it is visible as exactly that (`described` in the
// axis ledger) — and that visibility is the whole enforcement. There is no
// "other" bucket anywhere in the protocol, deliberately: what you cannot name,
// you name.
//
// `same_as` is a MERGE CLAIM, signed like anything else: "this axis and that one
// are the same thing". Anyone may publish one — the author of the original, or
// anybody at all if that author is gone or has lost their keys. Claims are not
// applied automatically; they are weighed the way everything here is weighed, by
// who uses them.
struct AxisDef {
    static constexpr RecordType TYPE = RecordType::AxisDef;

    std::string        slug;         // join key everything references
    std::string        ru;           // display name
    std::string        description;  // the argument: what it means and why it is one
    std::optional<Ref> parent;       // broader axis / family, when there is one
    std::optional<Ref> same_as;      // merge claim, never applied automatically
    int64_t            timestamp = 0;
};

// ── Record variant ────────────────────────────────────────────────────────────

using Record = std::variant<
    FraudClaim,
    Concept,
    ConceptLink,
    Composite,
    Copy,
    Reaction,
    Attest,
    Consent,
    WorkRecord,
    Release,
    WriteOff,
    Material,
    Tool,
    Transfer,
    DailyAggregate,
    Pledge,
    PledgeRevoke,
    Redemption,
    WorkCloud,
    AxisDef
>;

} // namespace records
