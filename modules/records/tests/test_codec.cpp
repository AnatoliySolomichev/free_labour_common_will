#include "records/codec.h"
#include "records/types.h"
#include <gtest/gtest.h>

#include <algorithm>

using namespace records;

// ── Helpers ───────────────────────────────────────────────────────────────────

static Ref make_ref(uint8_t chain_fill, uint8_t hash_fill) {
    Ref r{};
    r.chain.fill(chain_fill);
    r.hash.fill(hash_fill);
    return r;
}

// Round-trip helper: encode then decode, return the decoded Record.
static Record roundtrip(const Record& rec) {
    const auto bytes = Codec::encode(rec);
    return Codec::decode(bytes);
}

// Полный путь к блоку (records.md §4): цепь, ветка, номер, хеш.
static records::BlockPath make_path(uint8_t chain_fill, uint8_t hash_fill,
                                    uint32_t node = 7, uint32_t block = 3) {
    records::BlockPath p{};
    p.chain.fill(chain_fill);
    p.node  = node;
    p.block = block;
    p.hash.fill(hash_fill);
    return p;
}

// ── Ref encoding ──────────────────────────────────────────────────────────────

TEST(RecordsCodec, RefRoundtrip) {
    const Ref r = make_ref(0xAA, 0xBB);
    // Ref is not a Record; verify it encodes as part of Copy
    Copy c{ r };
    const auto decoded = std::get<Copy>(roundtrip(c));
    EXPECT_EQ(decoded.source, r);
}

// ── FraudClaim (structural, records.md §3A) ───────────────────────────────────

TEST(RecordsCodec, FraudClaimRoundtrip) {
    FraudClaim f{};
    f.target = make_ref(0x11, 0x22);
    f.kind   = "bad_sig";
    f.proof  = {0xDE, 0xAD, 0xBE, 0xEF};   // opaque blob (blockchain FraudProofData)
    f.reason = "signature does not verify";

    const auto decoded = std::get<FraudClaim>(roundtrip(f));
    EXPECT_EQ(decoded.target, f.target);
    EXPECT_EQ(decoded.kind,   f.kind);
    EXPECT_EQ(decoded.proof,  f.proof);
    EXPECT_EQ(decoded.reason, f.reason);
}

TEST(RecordsCodec, FraudClaimEmptyReasonAndProof) {
    FraudClaim f{};
    f.target = make_ref(0x01, 0x02);
    f.kind   = "hash_mismatch";
    // proof and reason left empty

    const auto decoded = std::get<FraudClaim>(roundtrip(f));
    EXPECT_EQ(decoded.kind, "hash_mismatch");
    EXPECT_TRUE(decoded.proof.empty());
    EXPECT_TRUE(decoded.reason.empty());
}

// ── Concept ───────────────────────────────────────────────────────────────────

TEST(RecordsCodec, ConceptNoTags) {
    Concept c{ "Алгоритм Дейкстры", {} };
    const auto decoded = std::get<Concept>(roundtrip(c));
    EXPECT_EQ(decoded.text, c.text);
    EXPECT_TRUE(decoded.tags.empty());
}

TEST(RecordsCodec, ConceptWithTags) {
    Concept c{ "Бесплатный труд", {"экономика", "кооператив"} };
    const auto decoded = std::get<Concept>(roundtrip(c));
    EXPECT_EQ(decoded.text, c.text);
    EXPECT_EQ(decoded.tags, c.tags);
}

TEST(RecordsCodec, ConceptDeterministic) {
    Concept c{ "test", {"a", "b"} };
    EXPECT_EQ(Codec::encode(c), Codec::encode(c));
}

// ── ConceptLink ───────────────────────────────────────────────────────────────

TEST(RecordsCodec, ConceptLink) {
    ConceptLink cl{ make_ref(0x01, 0x02), make_ref(0x03, 0x04), "уточняет" };
    const auto decoded = std::get<ConceptLink>(roundtrip(cl));
    EXPECT_EQ(decoded.from, cl.from);
    EXPECT_EQ(decoded.to,   cl.to);
    EXPECT_EQ(decoded.kind, cl.kind);
}

// ── Composite ─────────────────────────────────────────────────────────────────

TEST(RecordsCodec, CompositeEmpty) {
    Composite c{ "Группа", {} };
    const auto decoded = std::get<Composite>(roundtrip(c));
    EXPECT_EQ(decoded.title, c.title);
    EXPECT_TRUE(decoded.parts.empty());
}

TEST(RecordsCodec, CompositeTwoParts) {
    Composite c{ "Производство стула", { make_ref(0x10, 0x11), make_ref(0x20, 0x21) } };
    const auto decoded = std::get<Composite>(roundtrip(c));
    EXPECT_EQ(decoded.title,    c.title);
    EXPECT_EQ(decoded.parts[0], c.parts[0]);
    EXPECT_EQ(decoded.parts[1], c.parts[1]);
}

// ── Copy ──────────────────────────────────────────────────────────────────────

TEST(RecordsCodec, Copy) {
    Copy c{ make_ref(0xCC, 0xDD) };
    const auto decoded = std::get<Copy>(roundtrip(c));
    EXPECT_EQ(decoded.source, c.source);
}

// ── Reaction ──────────────────────────────────────────────────────────────────

TEST(RecordsCodec, ReactionPositive) {
    Reaction r{ make_ref(0x01, 0x02), 100 };
    const auto decoded = std::get<Reaction>(roundtrip(r));
    EXPECT_EQ(decoded.target, r.target);
    EXPECT_EQ(decoded.value,  100);
}

TEST(RecordsCodec, ReactionNegative) {
    Reaction r{ make_ref(0x01, 0x02), -100 };
    const auto decoded = std::get<Reaction>(roundtrip(r));
    EXPECT_EQ(decoded.value, -100);
}

TEST(RecordsCodec, ReactionMinMax) {
    {
        Reaction r{ make_ref(0x01, 0x02), -128 };
        EXPECT_EQ(std::get<Reaction>(roundtrip(r)).value, -128);
    }
    {
        Reaction r{ make_ref(0x01, 0x02), 127 };
        EXPECT_EQ(std::get<Reaction>(roundtrip(r)).value, 127);
    }
}

TEST(RecordsCodec, ReactionNeutral) {
    Reaction r{ make_ref(0x01, 0x02), 0 };
    EXPECT_EQ(std::get<Reaction>(roundtrip(r)).value, 0);
}

// ── WorkRecord ────────────────────────────────────────────────────────────────

TEST(RecordsCodec, WorkRecordMinimal) {
    WorkRecord wr{};
    wr.action   = "установка розетки";
    wr.start_ts = 1'700'000'000LL;
    wr.hours    = 2.5;
    // empty inputs/outputs
    const auto decoded = std::get<WorkRecord>(roundtrip(wr));
    EXPECT_TRUE(decoded.title.empty());
    EXPECT_EQ(decoded.action,   wr.action);
    EXPECT_EQ(decoded.start_ts, wr.start_ts);
    EXPECT_DOUBLE_EQ(decoded.hours, wr.hours);
    EXPECT_TRUE(decoded.inputs.empty());
    EXPECT_TRUE(decoded.outputs.empty());
}

TEST(RecordsCodec, WorkRecordWithResources) {
    WorkRecord wr{};
    wr.action   = "фрезеровка";
    wr.start_ts = 1'700'000'000LL;
    wr.hours    = 3.5;
    wr.inputs   = { {"Доска сосновая", 0.02, "м³"} };
    wr.outputs  = { {"Детали стула", 1.0, "компл."} };

    const auto decoded = std::get<WorkRecord>(roundtrip(wr));
    ASSERT_EQ(decoded.inputs.size(),  1u);
    ASSERT_EQ(decoded.outputs.size(), 1u);
    EXPECT_EQ(decoded.inputs[0],  wr.inputs[0]);
    EXPECT_EQ(decoded.outputs[0], wr.outputs[0]);
}

// Разряда и специальности больше нет (ИР-022 шаг 11): запись работы ни на что не
// ссылается. «Кем работал» — вольный текст для людей, по которому ничего не
// считается; пустой поля не занимает.
TEST(RecordsCodec, WorkRecordTitleIsFreeTextAndOptional) {
    WorkRecord wr{};
    wr.action   = "замена проводки";
    wr.start_ts = 1;
    wr.hours    = 3.0;
    EXPECT_EQ(Codec::encode(Record{wr})[0], 0xA6);           // map(6): без хвоста
    wr.title    = "электрик, 5-й разряд";
    EXPECT_EQ(std::get<WorkRecord>(roundtrip(wr)).title, wr.title);

    CarryEntry ce{};                                          // оба необязательных
    ce.src = make_ref(0x61, 0x01); ce.used = 1.0; ce.carried = 0.1; ce.after = 0.1;
    wr.carry.push_back(ce);
    const auto bytes = Codec::encode(Record{wr});
    EXPECT_EQ(bytes[0], 0xA8);                                // 6 + перенос + название
    const auto d = std::get<WorkRecord>(Codec::decode(bytes));
    EXPECT_EQ(d.title, wr.title);
    ASSERT_EQ(d.carry.size(), 1u);
}

// ── Сделка: приёмка внутри перевода (records.md §9.5, ИР-023) ────────────────

static std::array<uint8_t, 32> make_uid(uint8_t fill) {
    std::array<uint8_t, 32> a{};
    a.fill(fill);
    return a;
}

// Сделка: приёмка — внутри своей оплаты. Платит принимающий (0xA1) тому, чья
// работа принята, ровно труд плюс перенос.
static Transfer deal_of(const Acceptance& a) {
    Transfer t{};
    t.from      = make_uid(0xA1);
    t.to        = a.work.chain;
    t.origins   = { {t.from, a.labor_units + (a.carried_units ? *a.carried_units : 0.0)} };
    t.accept    = a;
    t.timestamp = 1'700'000'100LL;
    return t;
}

// Приёмка после круга через кодек — внутри своего перевода.
static Acceptance roundtrip_accept(const Acceptance& a) {
    const auto t = std::get<Transfer>(roundtrip(Record{deal_of(a)}));
    EXPECT_TRUE(t.accept.has_value());
    return t.accept ? *t.accept : Acceptance{};
}

static Acceptance plain_acceptance(double units) {
    Acceptance a{};
    a.work        = make_ref(0x53, 0x54);
    a.quality     = "пройдено";
    a.hours_raw   = units;
    a.labor_units = units;
    a.axes        = {{make_path(0x79, 0x01), units}};
    return a;
}

TEST(RecordsCodec, DealRoundtrip) {
    Acceptance a = plain_acceptance(3.0);
    Transfer   t = deal_of(a);
    t.to_node = 5;                                    // credited branch (per-branch purse)
    // Все три вида порций: своя (эмиссия), бумага получателя (погашение), чужая.
    t.origins = { {make_uid(0xA1), 1.5}, {t.to, 0.5}, {make_uid(0xC3), 1.0} };

    const auto d = std::get<Transfer>(roundtrip(Record{t}));
    EXPECT_EQ(d.from, t.from);
    EXPECT_EQ(d.to, t.to);
    EXPECT_EQ(d.to_node, 5u);
    EXPECT_EQ(d.origins, t.origins);
    EXPECT_EQ(d.timestamp, t.timestamp);
    ASSERT_TRUE(d.accept.has_value());
    EXPECT_EQ(d.accept->work, a.work);
    EXPECT_EQ(d.accept->quality, a.quality);
    EXPECT_DOUBLE_EQ(d.accept->hours_raw, a.hours_raw);
    EXPECT_DOUBLE_EQ(d.accept->labor_units, a.labor_units);
    EXPECT_EQ(d.accept->axes, a.axes);
}

// «Такая же работа, как та» (ИР-022 шаг 11): приёмка может назвать образец, чью
// разбивку повторяет. Хвост приёмки читается по ключу: 5, 6, 7 по возрастанию.
TEST(RecordsCodec, AcceptanceLikeNamesTheSample) {
    Acceptance a = plain_acceptance(2.0);
    a.quality = "как новый";
    a.like    = make_ref(0xB1, 0x5A);
    const auto d = roundtrip_accept(a);
    ASSERT_TRUE(d.like.has_value());
    EXPECT_EQ(*d.like, *a.like);

    a.carried_units = 0.5;
    AcceptanceNorm n{};
    n.agg.fill(0x33); n.date = 86'400; n.W = 1.0;
    a.norm = n;
    const auto all = roundtrip_accept(a);
    EXPECT_EQ(*all.like, *a.like);
    EXPECT_DOUBLE_EQ(*all.carried_units, 0.5);
    ASSERT_TRUE(all.norm.has_value());
}

// Приёмки без разбивки не существует: кодек её не пишет...
TEST(RecordsCodec, AcceptanceWithoutAxesIsRefused) {
    Acceptance a = plain_acceptance(2.0);
    a.axes.clear();
    EXPECT_THROW(Codec::encode(Record{deal_of(a)}), CodecError);
    a.axes = {{make_path(0x79, 0x03), 2.0}};
    EXPECT_NO_THROW(Codec::encode(Record{deal_of(a)}));
}

// ...и не читает. Две сделки, различающиеся лишь числом строк разбивки (1 и 2),
// дают длину одной строки; из первой её вырезают и ставят пустой массив.
TEST(RecordsCodec, AcceptanceWithoutAxesIsNotRead) {
    const Acceptance one = plain_acceptance(1.0);
    Acceptance       two = one;
    two.axes.push_back(one.axes[0]);
    auto       b1 = Codec::encode(Record{deal_of(one)});
    const auto b2 = Codec::encode(Record{deal_of(two)});
    const auto line = static_cast<std::ptrdiff_t>(b2.size() - b1.size());
    const std::vector<uint8_t> head = {0x04, 0x81, 0xA2};   // ключ 4, массив из одной, map(2)
    const auto at = std::find_end(b1.begin(), b1.end(), head.begin(), head.end());
    ASSERT_NE(at, b1.end());
    const auto pos = at - b1.begin();
    b1.erase(b1.begin() + pos + 2, b1.begin() + pos + 2 + line);
    b1[static_cast<size_t>(pos) + 1] = 0x80;                  // пустой массив
    EXPECT_THROW(Codec::decode(b1), CodecError);
}

// Беспричинных переводов нет (records.md §12.9): без приёмки часы не уходят в
// другую цепь — между своими ветками ходит только чужая бумага.
TEST(RecordsCodec, TransferWithoutAcceptanceOnlyBetweenOwnBranches) {
    Transfer t{};
    t.from      = make_uid(0x01);
    t.to        = make_uid(0x02);
    t.origins   = { {make_uid(0x03), 3.0} };
    t.timestamp = 1LL;
    EXPECT_THROW(Codec::encode(Record{t}), CodecError);        // в чужую цепь — нельзя

    t.to      = t.from;                                        // своя ветка → своя ветка
    t.to_node = 9;
    const auto d = std::get<Transfer>(roundtrip(Record{t}));
    EXPECT_FALSE(d.accept.has_value());
    EXPECT_EQ(d.to_node, 9u);

    t.origins = { {t.from, 3.0} };                              // своя бумага себе — нет
    EXPECT_THROW(Codec::encode(Record{t}), CodecError);
}

// Треугольника нет по построению: платит принимающий, и платит ровно тому, чья
// работа принята, ровно оценку — ни меньше, ни больше.
TEST(RecordsCodec, DealPaysTheWorkerExactlyTheAppraisal) {
    const Acceptance a = plain_acceptance(3.0);
    EXPECT_NO_THROW(Codec::encode(Record{deal_of(a)}));

    Transfer under = deal_of(a);
    under.origins = { {under.from, 2.0} };
    EXPECT_THROW(Codec::encode(Record{under}), CodecError);    // недоплата

    Transfer over = deal_of(a);
    over.origins = { {over.from, 4.0} };
    EXPECT_THROW(Codec::encode(Record{over}), CodecError);     // переплата

    Transfer elsewhere = deal_of(a);
    elsewhere.to = make_uid(0xEE);                              // платит не работнику
    EXPECT_THROW(Codec::encode(Record{elsewhere}), CodecError);

    Acceptance own = a;
    own.work = make_ref(0xA1, 0x54);                            // своя работа
    EXPECT_THROW(Codec::encode(Record{deal_of(own)}), CodecError);
}

// Перенос входит в оплату: оплата = труд + перенос (records.md §9.5 v2).
TEST(RecordsCodec, DealPaysLabourPlusCarried) {
    Acceptance a = plain_acceptance(6.0);
    a.carried_units = 1.19625;
    const Transfer t = deal_of(a);
    EXPECT_DOUBLE_EQ(t.origins[0].units, 7.19625);
    const auto d = roundtrip_accept(a);
    ASSERT_TRUE(d.carried_units.has_value());
    EXPECT_DOUBLE_EQ(*d.carried_units, 1.19625);

    Transfer labour_only = t;
    labour_only.origins = { {t.from, 6.0} };
    EXPECT_THROW(Codec::encode(Record{labour_only}), CodecError);
}

// Прежний тип 0x54 снят: отдельная приёмка не читается (обратной совместимости
// нет — решение участника 2026-09-14).
TEST(RecordsCodec, RetiredAcceptanceRecordIsRefused) {
    const std::vector<uint8_t> old = { 0xa1, 0x00, 0x18, 0x54 };   // map(1) {0: 0x54}
    EXPECT_THROW(Codec::decode(old), CodecError);
}

// Хвост перевода — эмиссия (7–9), обещание (10), приёмка (11) — по возрастанию
// ключей в любом сочетании (records.md §11.1 v4/v5).
TEST(RecordsCodec, TransferTailRoundtripsInEveryCombination) {
    const Transfer base = deal_of(plain_acceptance(3.0));      // самоэмиссия на 3ч
    EmissionLink link{};
    link.seq        = 7;
    link.prev       = make_ref(0x33, 0x44);
    link.debt_after = -3.0;
    const Ref pledge = make_ref(0x55, 0x66);

    {   // map(7) — только приёмка
        const auto bytes = Codec::encode(Record{base});
        EXPECT_EQ(bytes[0], 0xA7);
        const auto d = std::get<Transfer>(Codec::decode(bytes));
        EXPECT_FALSE(d.settles.has_value());
        EXPECT_FALSE(d.emission.has_value());
    }
    {   // map(8) — и обещание
        Transfer t = base;
        t.settles  = pledge;
        const auto d = std::get<Transfer>(roundtrip(Record{t}));
        ASSERT_TRUE(d.settles.has_value());
        EXPECT_EQ(*d.settles, pledge);
        EXPECT_FALSE(d.emission.has_value());
    }
    {   // map(10) — и звено эмиссии
        Transfer t = base;
        t.emission = link;
        const auto d = std::get<Transfer>(roundtrip(Record{t}));
        ASSERT_TRUE(d.emission.has_value());
        EXPECT_EQ(d.emission->seq, 7u);
        EXPECT_FALSE(d.settles.has_value());
    }
    {   // map(11) — всё сразу: закрыть обещание свежевыпущенной бумагой
        Transfer t = base;
        t.emission = link;
        t.settles  = pledge;
        const auto bytes = Codec::encode(Record{t});
        EXPECT_EQ(bytes[0], 0xAB);
        const auto d = std::get<Transfer>(Codec::decode(bytes));
        ASSERT_TRUE(d.settles.has_value());
        ASSERT_TRUE(d.emission.has_value());
        EXPECT_DOUBLE_EQ(d.emission->debt_after, -3.0);
        ASSERT_TRUE(d.accept.has_value());                     // «за что» — сама приёмка
        EXPECT_EQ(d.accept->work, base.accept->work);
    }
}

TEST(RecordsCodec, PledgeRoundtripWithAndWithoutOptionals) {
    Pledge full{};
    full.target    = make_ref(0x33, 0x44);
    full.units     = 12.5;
    full.executor  = make_uid(0xEE);
    full.expires   = 1'800'000'000LL;
    full.timestamp = 1'700'000'000LL;

    const auto d1 = std::get<Pledge>(roundtrip(full));
    EXPECT_EQ(d1.target, full.target);
    EXPECT_DOUBLE_EQ(d1.units, full.units);
    ASSERT_TRUE(d1.executor.has_value());
    EXPECT_EQ(*d1.executor, *full.executor);
    ASSERT_TRUE(d1.expires.has_value());
    EXPECT_EQ(*d1.expires, *full.expires);

    Pledge bare{};
    bare.target    = make_ref(0x55, 0x66);
    bare.units     = 1.0;
    bare.timestamp = 2LL;

    const auto d2 = std::get<Pledge>(roundtrip(bare));
    EXPECT_FALSE(d2.executor.has_value());
    EXPECT_FALSE(d2.expires.has_value());
}

TEST(RecordsCodec, PledgeRevokeRoundtrip) {
    PledgeRevoke pr{};
    pr.pledge    = make_ref(0x77, 0x88);
    pr.timestamp = 3LL;

    const auto decoded = std::get<PledgeRevoke>(roundtrip(pr));
    EXPECT_EQ(decoded.pledge,    pr.pledge);
    EXPECT_EQ(decoded.timestamp, pr.timestamp);
}

// DailyAggregate v5 (ИР-022 шаг 11): цены осей, а не ставки специальностей.
TEST(RecordsCodec, DailyAggregateRoundtrip) {
    DailyAggregate d{};
    d.date      = 1'700'000'000LL - 1'700'000'000LL % 86400;
    d.timestamp = 1'700'000'042LL;
    d.axes      = { {make_path(0x79, 0x01), 0.41, 6.0, 3, 5.5},
                    {make_path(0x79, 0x02, 9, 4), 0.34, 2.0, 1, 2.0} };
    d.W         = 1.0509;
    d.indep     = "v3;ref=axis_prices;window_days=365";

    const auto decoded = std::get<DailyAggregate>(roundtrip(d));
    EXPECT_EQ(decoded.date,      d.date);
    EXPECT_EQ(decoded.timestamp, d.timestamp);
    EXPECT_DOUBLE_EQ(decoded.W,  1.0509);
    EXPECT_EQ(decoded.indep,     d.indep);
    ASSERT_EQ(decoded.axes.size(), 2u);
    EXPECT_EQ(decoded.axes[0], d.axes[0]);
    EXPECT_EQ(decoded.axes[1], d.axes[1]);
    EXPECT_EQ(decoded.axes[1].axis.node, 9u);                 // полный путь уцелел

    DailyAggregate empty{};
    empty.date = 0;
    const auto de = std::get<DailyAggregate>(roundtrip(empty));
    EXPECT_TRUE(de.axes.empty());
    EXPECT_DOUBLE_EQ(de.W, 1.0);
    EXPECT_TRUE(de.indep.empty());
}

// Старые раскладки — ставки по специальности под ключом 2, с разрядом, без W —
// не читаются: обратной совместимости нет (решение участника 2026-09-11).
TEST(RecordsCodec, DailyAggregateOldLayoutsAreRefused) {
    std::vector<uint8_t> no_w;                                // map(4) без W и indep
    no_w.push_back(0xA4);
    no_w.push_back(0x00); no_w.push_back(0x18); no_w.push_back(0x71);
    no_w.push_back(0x01); no_w.push_back(0x00);
    no_w.push_back(0x02); no_w.push_back(0x80);
    no_w.push_back(0x03); no_w.push_back(0x00);
    EXPECT_THROW(Codec::decode(no_w), CodecError);

    std::vector<uint8_t> graded;                              // ставка с разрядом
    graded.push_back(0xA5);
    graded.push_back(0x00); graded.push_back(0x18); graded.push_back(0x71);
    graded.push_back(0x01); graded.push_back(0x00);
    graded.push_back(0x02); graded.push_back(0x81);
    graded.push_back(0xA6);
    graded.push_back(0x00); graded.push_back(0x61); graded.push_back(0x61);
    graded.push_back(0x01); graded.push_back(0x06);
    graded.push_back(0x02); graded.push_back(0xFB);
    for (uint8_t x : {0x40,0x00,0x00,0x00,0x00,0x00,0x00,0x00}) graded.push_back(x);
    graded.push_back(0x03); graded.push_back(0xFB);
    for (uint8_t x : {0x40,0x10,0x00,0x00,0x00,0x00,0x00,0x00}) graded.push_back(x);
    graded.push_back(0x04); graded.push_back(0x02);
    graded.push_back(0x05); graded.push_back(0xFB);
    for (uint8_t x : {0x40,0x08,0x00,0x00,0x00,0x00,0x00,0x00}) graded.push_back(x);
    graded.push_back(0x03); graded.push_back(0x00);
    graded.push_back(0x04); graded.push_back(0xFB);
    for (uint8_t x : {0x3F,0xF0,0x00,0x00,0x00,0x00,0x00,0x00}) graded.push_back(x);
    EXPECT_THROW(Codec::decode(graded), CodecError);
}

TEST(RecordsCodec, EconomyDeterminism) {
    const Transfer t = deal_of(plain_acceptance(0.1));
    EXPECT_EQ(Codec::encode(t), Codec::encode(t));

    Pledge p{};
    p.target = make_ref(0x03, 0x04);
    p.units  = 5.0;
    EXPECT_EQ(Codec::encode(p), Codec::encode(p));
}

// ── Error handling ────────────────────────────────────────────────────────────

TEST(RecordsCodec, EmptyDataThrows) {
    EXPECT_THROW(Codec::decode(nullptr, 0), CodecError);
}

TEST(RecordsCodec, UnknownTypeThrows) {
    // Manually craft: map(2) {0: uint(0xFF), 1: uint(0)}
    const std::vector<uint8_t> bad = { 0xa2, 0x00, 0x18, 0xFF, 0x01, 0x00 };
    EXPECT_THROW(Codec::decode(bad), CodecError);
}

TEST(RecordsCodec, TruncatedDataThrows) {
    Concept c{ "hello", {} };
    auto bytes = Codec::encode(c);
    bytes.resize(bytes.size() / 2);
    EXPECT_THROW(Codec::decode(bytes), CodecError);
}

// ── Determinism ───────────────────────────────────────────────────────────────

TEST(RecordsCodec, SameInputSameBytes) {
    WorkRecord wr{};
    wr.action   = "работа";
    wr.start_ts = 1234567890LL;
    wr.hours    = 1.0;
    wr.inputs   = { {"кабель", 10.0, "м"} };
    wr.outputs  = { {"розетка", 1.0, "шт"} };

    EXPECT_EQ(Codec::encode(wr), Codec::encode(wr));
}

// ── Transfer v3: emission thread (economy.md §4.3) ────────────────────────────

TEST(RecordsCodec, TransferV3EmissionLinkRoundtrip) {
    Transfer t = deal_of(plain_acceptance(10.0));   // self-issue → threaded
    t.to_node = 7;
    EmissionLink link{};
    link.seq        = 3;
    link.prev       = make_ref(0x33, 0x44);
    link.debt_after = -17.0;                          // "+10 from level -7 → -17"
    t.emission = link;

    const auto decoded = std::get<Transfer>(roundtrip(Record{t}));
    ASSERT_TRUE(decoded.emission.has_value());
    EXPECT_EQ(decoded.emission->seq, 3u);
    ASSERT_TRUE(decoded.emission->prev.has_value());
    EXPECT_EQ(*decoded.emission->prev, *link.prev);
    EXPECT_DOUBLE_EQ(decoded.emission->debt_after, -17.0);
}

TEST(RecordsCodec, TransferV3FirstLinkHasNoPrev) {
    Transfer t = deal_of(plain_acceptance(7.0));
    EmissionLink link{};
    link.seq        = 0;
    link.debt_after = -7.0;
    t.emission = link;

    const auto decoded = std::get<Transfer>(roundtrip(Record{t}));
    ASSERT_TRUE(decoded.emission.has_value());
    EXPECT_EQ(decoded.emission->seq, 0u);
    EXPECT_FALSE(decoded.emission->prev.has_value());
    EXPECT_DOUBLE_EQ(decoded.emission->debt_after, -7.0);
}

TEST(RecordsCodec, TransferWithoutEmissionDecodes) {
    Transfer t = deal_of(plain_acceptance(1.0));
    t.origins = { {make_uid(0x03), 1.0} };            // pure endorsement, no thread
    const auto decoded = std::get<Transfer>(roundtrip(Record{t}));
    EXPECT_FALSE(decoded.emission.has_value());
}

// ── Redemption (0x74): the "+" link of the thread ─────────────────────────────

TEST(RecordsCodec, RedemptionRoundtrip) {
    Redemption rd{};
    rd.transfer        = make_ref(0x55, 0x66);
    rd.units           = 4.5;
    rd.link.seq        = 8;
    rd.link.prev       = make_ref(0x77, 0x88);
    rd.link.debt_after = -12.5;
    rd.timestamp       = 1'700'000'123LL;

    const auto decoded = std::get<Redemption>(roundtrip(rd));
    EXPECT_EQ(decoded.transfer, rd.transfer);
    EXPECT_DOUBLE_EQ(decoded.units, 4.5);
    EXPECT_EQ(decoded.link.seq, 8u);
    ASSERT_TRUE(decoded.link.prev.has_value());
    EXPECT_EQ(*decoded.link.prev, *rd.link.prev);
    EXPECT_DOUBLE_EQ(decoded.link.debt_after, -12.5);
    EXPECT_EQ(decoded.timestamp, rd.timestamp);
}

TEST(RecordsCodec, RedemptionFirstLinkNoPrev) {
    Redemption rd{};
    rd.transfer        = make_ref(0x01, 0x02);
    rd.units           = 1.0;
    rd.link.seq        = 0;
    rd.link.debt_after = 1.0;
    rd.timestamp       = 5LL;

    const auto decoded = std::get<Redemption>(roundtrip(rd));
    EXPECT_FALSE(decoded.link.prev.has_value());
}

// ── Production v2: Tool / Material / carry (records.md §9.4, records.md §10, ИР-011) ─────

TEST(RecordsCodec, ToolRoundtripEstNoRefs) {
    Tool t{};
    t.name   = "Печь подовая";
    t.desc   = "2019 г., под №2";
    t.serial = "PP-2019-0417";
    t.cost   = 315.0;
    t.life   = 8000.0;
    t.basis  = "est";
    t.note   = "оценка: 120000₽ / 380₽·ч ≈ 315 ч";

    const auto decoded = std::get<Tool>(roundtrip(t));
    EXPECT_EQ(decoded.name,   t.name);
    EXPECT_EQ(decoded.desc,   t.desc);
    EXPECT_EQ(decoded.serial, t.serial);
    EXPECT_DOUBLE_EQ(decoded.cost, t.cost);
    EXPECT_DOUBLE_EQ(decoded.life, t.life);
    EXPECT_EQ(decoded.basis,  t.basis);
    EXPECT_FALSE(decoded.src.has_value());
    EXPECT_FALSE(decoded.origin.has_value());
    EXPECT_EQ(decoded.note,   t.note);
}

TEST(RecordsCodec, ToolRoundtripPaidWithOrigin) {
    Tool t{};
    t.name   = "Печь подовая";
    t.serial = "PP-2019-0417";
    t.cost   = 120.0;                     // купил по остатку продавца
    t.life   = 5000.0;
    t.basis  = "paid";
    t.src    = make_ref(0x21, 0x22);      // приёмка покупки
    t.origin = make_ref(0x23, 0x24);      // запись экземпляра у продавца

    const auto decoded = std::get<Tool>(roundtrip(t));
    ASSERT_TRUE(decoded.src.has_value());
    ASSERT_TRUE(decoded.origin.has_value());
    EXPECT_EQ(*decoded.src,    *t.src);
    EXPECT_EQ(*decoded.origin, *t.origin);
    EXPECT_EQ(decoded.basis, "paid");
}

TEST(RecordsCodec, MaterialBatchRoundtrip) {
    Material m{};
    m.name  = "Мука пшеничная";
    m.unit  = "кг";
    m.cost  = 4.0;
    m.qty   = 50.0;
    m.basis = "est";
    m.note  = "чек 2100₽ / 525₽·ч";

    const auto decoded = std::get<Material>(roundtrip(m));
    EXPECT_EQ(decoded.name, m.name);
    EXPECT_EQ(decoded.unit, m.unit);
    EXPECT_DOUBLE_EQ(decoded.cost, m.cost);
    EXPECT_DOUBLE_EQ(decoded.qty,  m.qty);
    EXPECT_EQ(decoded.basis, m.basis);
    EXPECT_FALSE(decoded.src.has_value());
    EXPECT_FALSE(decoded.origin.has_value());
    EXPECT_EQ(decoded.note, m.note);
}

TEST(RecordsCodec, MaterialBatchPaidWithOrigin) {
    Material m{};
    m.name   = "Мука пшеничная";
    m.unit   = "кг";
    m.cost   = 3.04;                      // купил остаток предыдущей партии
    m.qty    = 38.0;
    m.basis  = "paid";
    m.src    = make_ref(0x31, 0x32);      // приёмка покупки
    m.origin = make_ref(0x33, 0x34);      // предыдущая запись партии

    const auto decoded = std::get<Material>(roundtrip(m));
    ASSERT_TRUE(decoded.src.has_value());
    ASSERT_TRUE(decoded.origin.has_value());
    EXPECT_EQ(*decoded.src,    *m.src);
    EXPECT_EQ(*decoded.origin, *m.origin);
    EXPECT_EQ(decoded.basis, "paid");
}

TEST(RecordsCodec, WorkRecordV2CarryRoundtrip) {
    WorkRecord wr{};
    wr.action   = "выпечка хлеба";
    wr.start_ts = 1'700'000'000LL;
    wr.hours    = 6.0;

    CarryEntry ce{};
    ce.src     = make_ref(0x61, 0x01);
    ce.used    = 6.0;
    ce.carried = 0.23625;                 // 6/8000 × 315
    ce.seq     = 0;
    ce.after   = 0.23625;
    wr.carry.push_back(ce);

    CarryEntry ce2{};
    ce2.src     = make_ref(0x60, 0x02);
    ce2.used    = 12.0;                   // кг муки
    ce2.carried = 0.96;
    ce2.seq     = 3;
    ce2.prev    = make_ref(0x99, 0x9A);
    ce2.after   = 2.4;
    wr.carry.push_back(ce2);

    const auto decoded = std::get<WorkRecord>(roundtrip(wr));
    ASSERT_EQ(decoded.carry.size(), 2u);
    EXPECT_EQ(decoded.carry[0], ce);
    EXPECT_EQ(decoded.carry[1], ce2);
    EXPECT_FALSE(decoded.carry[0].prev.has_value());
    ASSERT_TRUE(decoded.carry[1].prev.has_value());
}

TEST(RecordsCodec, WorkRecordEmptyCarryTakesNoField) {
    // An empty carry is absent from the encoding, not an empty array.
    WorkRecord wr{};
    wr.action   = "розетка";
    wr.start_ts = 1LL;
    wr.hours    = 1.0;

    const auto bytes = Codec::encode(Record{wr});
    // map(6): 0xA6 head — no carry, no title
    ASSERT_FALSE(bytes.empty());
    EXPECT_EQ(bytes[0], 0xA6);
    const auto decoded = std::get<WorkRecord>(Codec::decode(bytes));
    EXPECT_TRUE(decoded.carry.empty());
}

// ── WorkCloud (0x75) — облако образцов работ (ИР-022 шаг 11, specialty-axes.md §10)
TEST(RecordsCodec, WorkCloudRoundtrip) {
    WorkCloud c{};
    c.date      = 1'700'000'000LL - (1'700'000'000LL % 86'400);
    c.snapshot.fill(0xAB);
    c.params    = "v4;basis=samples;affinity=bhattacharyya;k=5";
    std::array<uint8_t, 32> src{}; src.fill(0x11);
    c.sources.push_back(src);

    CloudPoint p{};
    p.sample = make_ref(0xB1, 0x5A);
    p.title  = "электрик, работа под напряжением";
    p.deals  = 7;
    p.neighbors.push_back({make_ref(0xB2, 0x5B), 0.82});
    p.neighbors.push_back({make_ref(0xB3, 0x5C), 0.61});
    c.points.push_back(p);

    CloudPoint q{};                                           // без названия и соседей
    q.sample = make_ref(0xB4, 0x5D);
    q.deals  = 2;
    c.points.push_back(q);
    c.timestamp = 1'700'000'050LL;

    const auto d = std::get<WorkCloud>(roundtrip(Record{c}));
    EXPECT_EQ(d.date, c.date);
    EXPECT_EQ(d.snapshot, c.snapshot);
    EXPECT_EQ(d.params, c.params);
    ASSERT_EQ(d.sources.size(), 1u);
    EXPECT_EQ(d.sources[0], src);
    ASSERT_EQ(d.points.size(), 2u);
    EXPECT_EQ(d.points[0].sample, p.sample);
    EXPECT_EQ(d.points[0].title, p.title);
    EXPECT_EQ(d.points[0].deals, 7u);
    ASSERT_EQ(d.points[0].neighbors.size(), 2u);
    EXPECT_EQ(d.points[0].neighbors[0], p.neighbors[0]);
    EXPECT_EQ(d.points[1].title, "");
    EXPECT_TRUE(d.points[1].neighbors.empty());
    EXPECT_EQ(d.timestamp, 1'700'000'050LL);
}

TEST(RecordsCodec, WorkCloudDeterminism) {
    WorkCloud c{};
    c.date = 86'400; c.snapshot.fill(1); c.params = "v4"; c.timestamp = 100;
    EXPECT_EQ(Codec::encode(Record{c}), Codec::encode(Record{c}));
}

// Приёмка v3: norm{agg,date,W} (economy.md §2б) — независимо от переноса.
TEST(RecordsCodec, AcceptanceNormRoundtrip) {
    Acceptance a = plain_acceptance(6.0);
    a.work = records::Ref{};
    AcceptanceNorm n{};
    n.agg.fill(0x33); n.date = 86'400; n.W = 1.0509;
    a.norm = n;

    const auto d = roundtrip_accept(a);                        // norm без переноса
    EXPECT_FALSE(d.carried_units.has_value());
    ASSERT_TRUE(d.norm.has_value());
    EXPECT_EQ(d.norm->agg, n.agg);
    EXPECT_EQ(d.norm->date, 86'400);
    EXPECT_DOUBLE_EQ(d.norm->W, 1.0509);

    a.carried_units = 1.2;                                     // и перенос, и norm
    const auto both = roundtrip_accept(a);
    ASSERT_TRUE(both.carried_units.has_value());
    EXPECT_DOUBLE_EQ(*both.carried_units, 1.2);
    ASSERT_TRUE(both.norm.has_value());

    a.carried_units.reset(); a.norm.reset();                   // ни того, ни другого
    const auto bare = roundtrip_accept(a);
    EXPECT_FALSE(bare.carried_units.has_value());
    EXPECT_FALSE(bare.norm.has_value());
}

// Приёмка v4 (ИР-022) — цена, расписанная по осям, подписана ВМЕСТЕ с ценой.
TEST(RecordsCodec, AcceptanceAxesRoundtrip) {
    Acceptance a = plain_acceptance(10.0);
    a.labor_units = 14.0;
    a.axes = {{make_path(0x79, 0x01), 2.8}, {make_path(0x79, 0x02), 5.5},
              {make_path(0x79, 0x03), 5.7}};
    const auto d = roundtrip_accept(a);
    EXPECT_EQ(d.axes, a.axes);
    EXPECT_DOUBLE_EQ(d.labor_units, 14.0);
    EXPECT_FALSE(d.carried_units.has_value());
}

// Ось — полный путь к её определению, а не слаг: слаги сталкиваются, и выбирать
// между двумя цепями, назвавшими свою ось одинаково, значило бы чьё-то решение.
TEST(RecordsCodec, AcceptanceAxisIsAReferenceToItsDefinition) {
    Acceptance a = plain_acceptance(1.0);
    a.axes = {{make_path(0xAA, 0xBB), 1.0}};
    const auto d = roundtrip_accept(a);
    ASSERT_EQ(d.axes.size(), 1u);
    EXPECT_EQ(d.axes[0].axis, a.axes[0].axis);      // путь И хеш, целиком
}

// AxisDef (0x79) — ось, заведённая в цепи (ИР-022)
TEST(RecordsCodec, AxisDefRoundtrip) {
    records::AxisDef a{};
    a.slug        = "danger";
    a.ru          = "Опасность";
    a.description = "риск для самого работника, а не для других";
    a.parent      = make_ref(0x79, 0x01);
    a.same_as     = make_ref(0x79, 0x02);
    a.timestamp   = 1'700'000'000LL;

    const auto d = std::get<records::AxisDef>(roundtrip(Record{a}));
    EXPECT_EQ(d.slug, a.slug);
    EXPECT_EQ(d.ru, a.ru);
    EXPECT_EQ(d.description, a.description);
    ASSERT_TRUE(d.parent.has_value());
    EXPECT_EQ(*d.parent, *a.parent);
    ASSERT_TRUE(d.same_as.has_value());
    EXPECT_EQ(*d.same_as, *a.same_as);
}

// Хвост по ключу: `parent` (5) и `same_as` (6) независимы.
TEST(RecordsCodec, AxisDefSameAsWithoutParent) {
    records::AxisDef a{};
    a.slug = "хитрость"; a.ru = "Хитрость";
    a.description = "то же, что изворотливость";
    a.same_as = make_ref(0x79, 0x03);
    a.timestamp = 8;

    const auto d = std::get<records::AxisDef>(roundtrip(Record{a}));
    EXPECT_FALSE(d.parent.has_value());
    ASSERT_TRUE(d.same_as.has_value());
    EXPECT_EQ(*d.same_as, *a.same_as);
}

// Путь переживает кодек целиком: ветка и номер — не декорация, по ним к блоку
// идут руками, а хеш проверяет, что там лежит именно он.
TEST(RecordsCodec, AcceptanceAxisPathKeepsBranchAndIndex) {
    Acceptance a = plain_acceptance(1.0);
    a.axes = {{make_path(0x79, 0x01, 0x7FFF'FFFFu, 41), 1.0}};
    const auto d = roundtrip_accept(a);
    ASSERT_EQ(d.axes.size(), 1u);
    EXPECT_EQ(d.axes[0].axis.node,  0x7FFF'FFFFu);
    EXPECT_EQ(d.axes[0].axis.block, 41u);
    EXPECT_EQ(d.axes[0].axis, a.axes[0].axis);
}

// ── Кому достаётся результат (records.md §9.4 v3, records.md §9.8, ИР-023) ───

// Работа на заказ: заказчик — необязательный хвост; без него поля нет вовсе.
TEST(RecordsCodec, WorkRecordCustomerIsOptionalTail) {
    WorkRecord wr{};
    wr.action   = "торт на заказ";
    wr.start_ts = 1;
    wr.hours    = 4.0;
    EXPECT_EQ(Codec::encode(Record{wr})[0], 0xA6);           // map(6)
    wr.customer = make_uid(0xB2);
    const auto bytes = Codec::encode(Record{wr});
    EXPECT_EQ(bytes[0], 0xA7);                                // + заказчик
    const auto d = std::get<WorkRecord>(Codec::decode(bytes));
    ASSERT_TRUE(d.customer.has_value());
    EXPECT_EQ(*d.customer, make_uid(0xB2));
}

static Release release_of(uint8_t buyer) {
    Release rl{};
    rl.work      = make_ref(0x53, 0x54);                      // работа в цепи 0x53
    rl.to        = make_uid(buyer);
    rl.timestamp = 1'700'000'000LL;
    rl.expires   = 1'700'604'800LL;                           // неделя
    return rl;
}

TEST(RecordsCodec, ReleaseRoundtrip) {
    const Release rl = release_of(0xB2);
    const auto d = std::get<Release>(roundtrip(Record{rl}));
    EXPECT_EQ(d.work, rl.work);
    EXPECT_EQ(d.to, rl.to);
    EXPECT_EQ(d.expires, rl.expires);
    EXPECT_EQ(d.timestamp, rl.timestamp);
}

// Отпуск, истекающий не позже самого себя, ничего не отпускает; отпуск себе —
// тоже. Кодек таких не пишет.
TEST(RecordsCodec, ReleaseThatExpiresAtOnceOrGoesToItselfIsRefused) {
    Release rl = release_of(0xB2);
    rl.expires = rl.timestamp;
    EXPECT_THROW(Codec::encode(Record{rl}), CodecError);
    EXPECT_THROW(Codec::encode(Record{release_of(0x53)}), CodecError);   // себе
}

// Сделка по отпуску: приёмка называет отпуск продавца — он лежит в цепи того,
// кому платят; отпуск чужой цепи ничего не отпускает, и такую сделку не записать.
TEST(RecordsCodec, DealNamesTheSellersRelease) {
    Acceptance a = plain_acceptance(2.0);
    a.release    = make_ref(0x53, 0x57);
    EXPECT_EQ(roundtrip_accept(a).release, a.release);

    a.release = make_ref(0xEE, 0x57);
    EXPECT_THROW(Codec::encode(Record{deal_of(a)}), CodecError);
}

// ── Товарная нить: звено труда (records.md §9.4 v4, ИР-023) ──────────────────

TEST(RecordsCodec, WorkRecordThreadLinkRoundtrip) {
    WorkRecord wr{};
    wr.action   = "выпечка";
    wr.start_ts = 1;
    wr.hours    = 2.0;
    wr.thread   = ThreadLink{0, {}};                                  // первое звено
    EXPECT_EQ(std::get<WorkRecord>(roundtrip(wr)).thread, wr.thread);

    wr.thread = ThreadLink{4, {make_ref(0x53, 0x01), make_ref(0x53, 0x02)}};   // слияние
    wr.axes   = {{make_path(0x79, 0x01), 0.25}, {make_path(0x79, 0x02), 0.75}};
    const auto bytes = Codec::encode(Record{wr});
    EXPECT_EQ(bytes[0], 0xA8);                                        // 6 + нить + оси
    const auto d = std::get<WorkRecord>(Codec::decode(bytes));
    EXPECT_EQ(d.thread, wr.thread);
    EXPECT_EQ(d.axes, wr.axes);
}

// Первое звено — без предыдущих, остальные — с ними; предыдущие — по порядку и
// без повторов, иначе одну и ту же запись можно было бы закодировать по-разному.
TEST(RecordsCodec, ThreadLinkThatCouldNotBeWrittenIsRefused) {
    WorkRecord wr{};
    wr.action = "работа";
    wr.hours  = 1.0;
    wr.thread = ThreadLink{3, {}};                                    // не первое, а предыдущих нет
    EXPECT_THROW(Codec::encode(Record{wr}), CodecError);
    wr.thread = ThreadLink{0, {make_ref(0x53, 0x01)}};                // первое, а предыдущее есть
    EXPECT_THROW(Codec::encode(Record{wr}), CodecError);
    wr.thread = ThreadLink{2, {make_ref(0x53, 0x02), make_ref(0x53, 0x01)}};   // не по порядку
    EXPECT_THROW(Codec::encode(Record{wr}), CodecError);
    wr.thread = ThreadLink{2, {make_ref(0x53, 0x01), make_ref(0x53, 0x01)}};   // повтор
    EXPECT_THROW(Codec::encode(Record{wr}), CodecError);
}

// Предложенная разбивка — это разбивка: доли положительны, в сумме 1, ось не дважды.
TEST(RecordsCodec, ProposedAxesMustBeABreakdown) {
    WorkRecord wr{};
    wr.action = "работа";
    wr.hours  = 1.0;
    wr.axes   = {{make_path(0x79, 0x01), 0.5}, {make_path(0x79, 0x02), 0.4}};
    EXPECT_THROW(Codec::encode(Record{wr}), CodecError);              // 0.9
    wr.axes   = {{make_path(0x79, 0x01), 1.2}, {make_path(0x79, 0x02), -0.2}};
    EXPECT_THROW(Codec::encode(Record{wr}), CodecError);              // доля < 0
    wr.axes   = {{make_path(0x79, 0x01), 0.5}, {make_path(0x79, 0x01), 0.5}};
    EXPECT_THROW(Codec::encode(Record{wr}), CodecError);              // ось дважды
    wr.axes   = {{make_path(0x79, 0x01), 1.0}};
    EXPECT_EQ(std::get<WorkRecord>(roundtrip(wr)).axes, wr.axes);
}

// ── Партии: доли выходов, ценник, отпуск порции, списание (records.md §9.4 v5,
//    records.md §9.8 v2, records.md §9.9) ─────────────────────────────────────

TEST(RecordsCodec, OutputShareAndPriceTagRoundtrip) {
    WorkRecord wr{};
    wr.action  = "распил";
    wr.hours   = 8.0;
    wr.outputs = {{"доски", 2.0, "м³", 0.9, 1.1}, {"опилки", 0.5, "м³", 0.1, std::nullopt}};
    EXPECT_EQ(std::get<WorkRecord>(roundtrip(wr)).outputs, wr.outputs);
}

// Доля и ценник — у партии, то есть у выхода; доли — у всех выходов и в сумме 1.
TEST(RecordsCodec, SharesAndTagsBelongToLotsAndAddUp) {
    WorkRecord wr{};
    wr.action = "распил";
    wr.hours  = 8.0;
    wr.inputs = {{"бревно", 3.0, "м³", 1.0, std::nullopt}};                 // доля у входа
    EXPECT_THROW(Codec::encode(Record{wr}), CodecError);
    wr.inputs  = {};
    wr.outputs = {{"доски", 2.0, "м³", 0.9, std::nullopt}, {"опилки", 0.5, "м³"}};   // не у всех
    EXPECT_THROW(Codec::encode(Record{wr}), CodecError);
    wr.outputs = {{"доски", 2.0, "м³", 0.7, std::nullopt},
                  {"опилки", 0.5, "м³", 0.2, std::nullopt}};                // в сумме 0.9
    EXPECT_THROW(Codec::encode(Record{wr}), CodecError);
    wr.outputs = {{"хлеб", 100.0, "шт", std::nullopt, 0.0}};                 // ценник 0
    EXPECT_THROW(Codec::encode(Record{wr}), CodecError);
}

static LotLink lot_link(uint64_t seq, double qty, double after) {
    LotLink l{};
    l.qty   = qty;
    l.seq   = seq;
    l.after = after;
    if (seq > 0) l.prev = make_ref(0x53, static_cast<uint8_t>(0x60 + seq));
    return l;
}

TEST(RecordsCodec, ReleaseOfAPortionRoundtrip) {
    Release rl = release_of(0xB2);
    rl.lot = lot_link(2, 5.0, 17.0);
    rl.lot->returns = {make_ref(0x53, 0x61)};
    const auto d = std::get<Release>(roundtrip(Record{rl}));
    ASSERT_TRUE(d.lot.has_value());
    EXPECT_EQ(*d.lot, *rl.lot);
}

TEST(RecordsCodec, WriteOffRoundtrip) {
    WriteOff wo{};
    wo.work      = make_ref(0x53, 0x54);
    wo.lot       = lot_link(0, 2.0, 2.0);
    wo.reason    = "подгорели";
    wo.timestamp = 1;
    const auto d = std::get<WriteOff>(roundtrip(Record{wo}));
    EXPECT_EQ(d.work, wo.work);
    EXPECT_EQ(d.lot, wo.lot);
    EXPECT_EQ(d.reason, wo.reason);
}

// Звено партии, которое нельзя было написать по правилу, — не запись.
TEST(RecordsCodec, LotLinkThatCouldNotBeWrittenIsRefused) {
    WriteOff wo{};
    wo.work      = make_ref(0x53, 0x54);
    wo.reason    = "бой";
    wo.timestamp = 1;
    wo.lot = lot_link(0, 0.0, 0.0);                                           // пустая порция
    EXPECT_THROW(Codec::encode(Record{wo}), CodecError);
    wo.lot = lot_link(0, 1.0, 1.0);
    wo.lot.prev = make_ref(0x53, 0x61);                                       // первое, а предыдущее есть
    EXPECT_THROW(Codec::encode(Record{wo}), CodecError);
    wo.lot = lot_link(0, 1.0, 1.0);
    wo.lot.returns = {make_ref(0x53, 0x61)};                                  // первому нечего возвращать
    EXPECT_THROW(Codec::encode(Record{wo}), CodecError);
    wo.lot = lot_link(3, 1.0, 4.0);
    wo.lot.returns = {make_ref(0x53, 0x62), make_ref(0x53, 0x61)};            // не по порядку
    EXPECT_THROW(Codec::encode(Record{wo}), CodecError);
}

// ── Ценник числом и партия со штучным учётом (records.md §9.4 v6,
//    records.md §9.8 v3) ──────────────────────────────────────────────────────

TEST(RecordsCodec, NumberedLotAndPriceTagRoundtrip) {
    WorkRecord wr{};
    wr.action  = "табуреты";
    wr.hours   = 3.0;
    wr.outputs = {{"табурет", 3.0, "шт", std::nullopt, 3.3, {"Т-1", "Т-2", "Т-3"}}};
    EXPECT_EQ(std::get<WorkRecord>(roundtrip(wr)).outputs, wr.outputs);

    wr.outputs[0].units = {"Т-1", "Т-2"};                     // три штуки, два номера
    EXPECT_THROW(Codec::encode(Record{wr}), CodecError);
    wr.outputs[0].units = {"Т-1", "Т-1", "Т-2"};              // номер дважды
    EXPECT_THROW(Codec::encode(Record{wr}), CodecError);
}

TEST(RecordsCodec, PortionOfNumberedUnitsRoundtrip) {
    Release rl = release_of(0xB2);
    rl.lot = lot_link(0, 2.0, 2.0);
    rl.lot->units = {"R-1", "R-2"};
    EXPECT_EQ(*std::get<Release>(roundtrip(Record{rl})).lot, *rl.lot);
    rl.lot->units = {"R-2", "R-1"};                            // не по порядку
    EXPECT_THROW(Codec::encode(Record{rl}), CodecError);
    rl.lot->units = {"R-1"};                                   // две штуки, один номер
    EXPECT_THROW(Codec::encode(Record{rl}), CodecError);
}
