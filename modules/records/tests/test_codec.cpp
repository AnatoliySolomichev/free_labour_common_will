#include "records/codec.h"
#include "records/types.h"
#include <gtest/gtest.h>

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

// ── Specialty ─────────────────────────────────────────────────────────────────

TEST(RecordsCodec, Specialty) {
    Specialty s{ "Электрик" };
    const auto decoded = std::get<Specialty>(roundtrip(s));
    EXPECT_EQ(decoded.name, s.name);
}

// ── Grade ─────────────────────────────────────────────────────────────────────

TEST(RecordsCodec, Grade) {
    Grade g{ make_ref(0x50, 0x51), 3 };
    const auto decoded = std::get<Grade>(roundtrip(g));
    EXPECT_EQ(decoded.specialty, g.specialty);
    EXPECT_EQ(decoded.level,     g.level);
}

// ── Worker ────────────────────────────────────────────────────────────────────

TEST(RecordsCodec, Worker) {
    Worker w{};
    w.chain.fill(0xAB);
    const auto decoded = std::get<Worker>(roundtrip(w));
    EXPECT_EQ(decoded.chain, w.chain);
}

// ── WorkRecord ────────────────────────────────────────────────────────────────

TEST(RecordsCodec, WorkRecordMinimal) {
    WorkRecord wr{};
    wr.agent    = make_ref(0x10, 0x11);
    wr.action   = "установка розетки";
    wr.start_ts = 1'700'000'000LL;
    wr.hours    = 2.5;
    // empty inputs/outputs
    const auto decoded = std::get<WorkRecord>(roundtrip(wr));
    EXPECT_EQ(decoded.agent,    wr.agent);
    EXPECT_EQ(decoded.action,   wr.action);
    EXPECT_EQ(decoded.start_ts, wr.start_ts);
    EXPECT_DOUBLE_EQ(decoded.hours, wr.hours);
    EXPECT_TRUE(decoded.inputs.empty());
    EXPECT_TRUE(decoded.outputs.empty());
}

TEST(RecordsCodec, WorkRecordWithResources) {
    WorkRecord wr{};
    wr.agent    = make_ref(0x10, 0x11);
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

// ── Acceptance ────────────────────────────────────────────────────────────────

TEST(RecordsCodec, Acceptance) {
    Acceptance a{};
    a.work        = make_ref(0x53, 0x54);
    a.receiver.fill(0xCC);
    a.quality     = "пройдено";
    a.hours_raw   = 2.5;
    a.labor_units = 2.5 * 1.2;
    a.timestamp   = 1'700'000'100LL;

    const auto decoded = std::get<Acceptance>(roundtrip(a));
    EXPECT_EQ(decoded.work,      a.work);
    EXPECT_EQ(decoded.receiver,  a.receiver);
    EXPECT_EQ(decoded.quality,   a.quality);
    EXPECT_DOUBLE_EQ(decoded.hours_raw,   a.hours_raw);
    EXPECT_DOUBLE_EQ(decoded.labor_units, a.labor_units);
    EXPECT_EQ(decoded.timestamp, a.timestamp);
}

// ── Economy (records.md §11) ──────────────────────────────────────────────────

static std::array<uint8_t, 32> make_uid(uint8_t fill) {
    std::array<uint8_t, 32> a{};
    a.fill(fill);
    return a;
}

TEST(RecordsCodec, TransferRoundtripAllOriginKinds) {
    Transfer t{};
    t.from      = make_uid(0xA1);
    t.to        = make_uid(0xB2);
    t.to_node   = 5;                          // credited branch (per-branch purse)
    t.origins   = { {make_uid(0xA1), 1.5},    // self-issue (issuer == from)
                    {make_uid(0xB2), 0.25},   // redemption (issuer == to)
                    {make_uid(0xC3), 2.0} };  // endorsement
    t.reason    = make_ref(0x11, 0x22);
    t.timestamp = 1'700'000'000LL;

    const auto decoded = std::get<Transfer>(roundtrip(t));
    EXPECT_EQ(decoded.from,      t.from);
    EXPECT_EQ(decoded.to,        t.to);
    EXPECT_EQ(decoded.to_node,   5u);
    EXPECT_EQ(decoded.origins,   t.origins);
    ASSERT_TRUE(decoded.reason.has_value());
    EXPECT_EQ(*decoded.reason,   *t.reason);
    EXPECT_EQ(decoded.timestamp, t.timestamp);
}

// Transfer v4 (records.md §11.1): `settles` is key 10 and rides after the
// emission group (7-9), so every combination keeps CBOR keys ascending —
// deterministic encoding holds for map sizes 7, 8, 10 and 11 alike.
TEST(RecordsCodec, TransferV4SettlesRoundtripsInEveryCombination) {
    Transfer base{};
    base.from      = make_uid(0xA1);
    base.to        = make_uid(0xB2);
    base.origins   = { {make_uid(0xA1), 3.0} };   // self-issue
    base.reason    = make_ref(0x11, 0x22);        // the acceptance being paid
    base.timestamp = 1'700'000'000LL;

    EmissionLink link{};
    link.seq        = 7;
    link.prev       = make_ref(0x33, 0x44);
    link.debt_after = -3.0;

    const Ref pledge = make_ref(0x55, 0x66);

    {   // map(7) — v2/v3 without either
        const auto d = std::get<Transfer>(roundtrip(base));
        EXPECT_FALSE(d.settles.has_value());
        EXPECT_FALSE(d.emission.has_value());
    }
    {   // map(8) — settles only
        Transfer t = base;
        t.settles  = pledge;
        const auto d = std::get<Transfer>(roundtrip(t));
        ASSERT_TRUE(d.settles.has_value());
        EXPECT_EQ(*d.settles, pledge);
        EXPECT_FALSE(d.emission.has_value());
    }
    {   // map(10) — emission only
        Transfer t = base;
        t.emission = link;
        const auto d = std::get<Transfer>(roundtrip(t));
        ASSERT_TRUE(d.emission.has_value());
        EXPECT_EQ(d.emission->seq, 7u);
        EXPECT_FALSE(d.settles.has_value());
    }
    {   // map(11) — both: paying off a promise with freshly issued paper
        Transfer t = base;
        t.emission = link;
        t.settles  = pledge;
        const auto d = std::get<Transfer>(roundtrip(t));
        ASSERT_TRUE(d.settles.has_value());
        EXPECT_EQ(*d.settles, pledge);
        ASSERT_TRUE(d.emission.has_value());
        EXPECT_DOUBLE_EQ(d.emission->debt_after, -3.0);
        // reason still answers a different question: what the payment is for.
        ASSERT_TRUE(d.reason.has_value());
        EXPECT_EQ(*d.reason, *base.reason);
    }
}

TEST(RecordsCodec, TransferWithoutReason) {
    Transfer t{};
    t.from      = make_uid(0x01);
    t.to        = make_uid(0x02);
    t.origins   = { {make_uid(0x01), 3.0} };
    t.timestamp = 1LL;

    const auto decoded = std::get<Transfer>(roundtrip(t));
    EXPECT_FALSE(decoded.reason.has_value());
    ASSERT_EQ(decoded.origins.size(), 1u);
    EXPECT_DOUBLE_EQ(decoded.origins[0].units, 3.0);
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

TEST(RecordsCodec, DailyAggregateRoundtrip) {
    DailyAggregate d{};
    d.date      = 1'700'000'000LL - 1'700'000'000LL % 86400;
    d.timestamp = 1'700'000'042LL;
    d.rates     = { {"хлебопёк", 3, 1.333, 0.6, 2},
                    {"кардиохирург", 3, 14.0, 0.75, 1} };

    const auto decoded = std::get<DailyAggregate>(roundtrip(d));
    EXPECT_EQ(decoded.date,      d.date);
    EXPECT_EQ(decoded.timestamp, d.timestamp);
    ASSERT_EQ(decoded.rates.size(), 2u);
    EXPECT_EQ(decoded.rates[0], d.rates[0]);
    EXPECT_EQ(decoded.rates[1], d.rates[1]);

    DailyAggregate empty{};
    empty.date = 0;
    EXPECT_TRUE(std::get<DailyAggregate>(roundtrip(empty)).rates.empty());
}

TEST(RecordsCodec, EconomyDeterminism) {
    Transfer t{};
    t.from    = make_uid(0x01);
    t.to      = make_uid(0x02);
    t.origins = { {make_uid(0x01), 0.1} };
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
    wr.agent    = make_ref(0xAA, 0xBB);
    wr.action   = "работа";
    wr.start_ts = 1234567890LL;
    wr.hours    = 1.0;
    wr.inputs   = { {"кабель", 10.0, "м"} };
    wr.outputs  = { {"розетка", 1.0, "шт"} };

    EXPECT_EQ(Codec::encode(wr), Codec::encode(wr));
}

// ── Transfer v3: emission thread (economy.md §4.3) ────────────────────────────

TEST(RecordsCodec, TransferV3EmissionLinkRoundtrip) {
    Transfer t{};
    t.from      = make_uid(0xA1);
    t.to        = make_uid(0xB2);
    t.to_node   = 7;
    t.origins   = { {make_uid(0xA1), 10.0} };   // self-issue → threaded
    t.reason    = make_ref(0x11, 0x22);
    t.timestamp = 1'700'000'000LL;

    EmissionLink link{};
    link.seq        = 3;
    link.prev       = make_ref(0x33, 0x44);
    link.debt_after = -17.0;                    // "+10 from level -7 → -17"
    t.emission = link;

    const auto decoded = std::get<Transfer>(roundtrip(t));
    ASSERT_TRUE(decoded.emission.has_value());
    EXPECT_EQ(decoded.emission->seq, 3u);
    ASSERT_TRUE(decoded.emission->prev.has_value());
    EXPECT_EQ(*decoded.emission->prev, *link.prev);
    EXPECT_DOUBLE_EQ(decoded.emission->debt_after, -17.0);
}

TEST(RecordsCodec, TransferV3FirstLinkHasNoPrev) {
    Transfer t{};
    t.from      = make_uid(0x01);
    t.to        = make_uid(0x02);
    t.origins   = { {make_uid(0x01), 7.0} };
    t.timestamp = 1LL;
    EmissionLink link{};
    link.seq        = 0;
    link.debt_after = -7.0;
    t.emission = link;

    const auto decoded = std::get<Transfer>(roundtrip(t));
    ASSERT_TRUE(decoded.emission.has_value());
    EXPECT_EQ(decoded.emission->seq, 0u);
    EXPECT_FALSE(decoded.emission->prev.has_value());
    EXPECT_DOUBLE_EQ(decoded.emission->debt_after, -7.0);
}

TEST(RecordsCodec, TransferV2WithoutEmissionStillDecodes) {
    Transfer t{};
    t.from      = make_uid(0x01);
    t.to        = make_uid(0x02);
    t.origins   = { {make_uid(0x03), 1.0} };    // pure endorsement, no thread
    t.timestamp = 2LL;

    const auto decoded = std::get<Transfer>(roundtrip(t));
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
