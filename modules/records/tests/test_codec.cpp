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
