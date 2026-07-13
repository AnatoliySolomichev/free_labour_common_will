#include <records/draft.h>

#include <gtest/gtest.h>

using namespace records;

namespace {

const std::string CHAIN =
    "06d62aa204a267340dc7d696b924b287399533bf794d843b178ace4daa27dfcb";
const std::string HASH =
    "79ee137aa72c043f6461f6e552449a4d5ab7a452a67f898152f525b4788d119d";
const std::string REF = CHAIN + "/" + HASH;

} // namespace

// ── The happy path: what a scribe or an AI actually produces ──────────────────

TEST(DraftTest, ParsesProfileFactsWithLeafAndTags) {
    const std::string json = R"({
        "leaf": 2147483647,
        "records": [
            {"t":"concept","text":"Электромонтаж, 8 лет",
             "tags":["kind:skill","cat:prof.electrician","geo:55.75,37.62","r:30"]},
            {"t":"concept","text":"Нужно заменить проводку"}
        ]
    })";

    const Draft draft = parse_draft(json);
    ASSERT_TRUE(draft.leaf.has_value());
    EXPECT_EQ(*draft.leaf, 2147483647u);
    ASSERT_EQ(draft.records.size(), 2u);

    const auto* first = std::get_if<Concept>(&draft.records[0]);
    ASSERT_NE(first, nullptr);
    EXPECT_EQ(first->text, "Электромонтаж, 8 лет");
    ASSERT_EQ(first->tags.size(), 4u);
    EXPECT_EQ(first->tags[0], "kind:skill");
    EXPECT_EQ(first->tags[3], "r:30");

    const auto* second = std::get_if<Concept>(&draft.records[1]);
    ASSERT_NE(second, nullptr);
    EXPECT_TRUE(second->tags.empty());   // tags are optional
}

TEST(DraftTest, LeafIsOptional) {
    const Draft draft = parse_draft(
        R"({"records":[{"t":"concept","text":"без ветки"}]})");
    EXPECT_FALSE(draft.leaf.has_value());
    EXPECT_EQ(draft.records.size(), 1u);
}

TEST(DraftTest, ParsesLinksAndComposites) {
    const std::string json = R"({"records":[
        {"t":"concept-link","from":")" + REF + R"(","to":")" + REF +
        R"(","kind":"закрыто"},
        {"t":"composite","title":"Профиль","parts":[")" + REF + R"("]}
    ]})";

    const Draft draft = parse_draft(json);
    ASSERT_EQ(draft.records.size(), 2u);

    const auto* link = std::get_if<ConceptLink>(&draft.records[0]);
    ASSERT_NE(link, nullptr);
    EXPECT_EQ(link->kind, "закрыто");
    EXPECT_EQ(link->to.chain[0], 0x06);
    EXPECT_EQ(link->to.hash[0],  0x79);

    const auto* comp = std::get_if<Composite>(&draft.records[1]);
    ASSERT_NE(comp, nullptr);
    EXPECT_EQ(comp->title, "Профиль");
    ASSERT_EQ(comp->parts.size(), 1u);
}

// ── The guard that matters: value-bearing records never ride in a batch ───────

TEST(DraftTest, RefusesValueBearingRecords) {
    const std::string smuggled = R"({"records":[
        {"t":"concept","text":"Электромонтаж"},
        {"t":"concept","text":"Нужен повар"},
        {"t":"transfer","to":"deadbeef","units":500}
    ]})";

    try {
        parse_draft(smuggled);
        FAIL() << "a transfer must never be accepted in a draft";
    } catch (const DraftError& e) {
        const std::string msg = e.what();
        EXPECT_NE(msg.find("transfer"), std::string::npos);
        EXPECT_NE(msg.find("запрещён"), std::string::npos);
        EXPECT_NE(msg.find("#3"), std::string::npos);   // names the offending record
    }
}

TEST(DraftTest, RefusesPledgeAndAcceptance) {
    EXPECT_THROW(parse_draft(R"({"records":[{"t":"pledge","units":10}]})"), DraftError);
    EXPECT_THROW(parse_draft(R"({"records":[{"t":"acceptance"}]})"),        DraftError);
}

TEST(DraftTest, RefusesUnknownType) {
    EXPECT_THROW(parse_draft(R"({"records":[{"t":"nonsense","text":"x"}]})"),
                 DraftError);
}

// ── Batch stays readable ─────────────────────────────────────────────────────

TEST(DraftTest, RefusesOversizedBatch) {
    std::string json = R"({"records":[)";
    for (std::size_t i = 0; i <= MAX_DRAFT_RECORDS; ++i) {
        if (i) json += ',';
        json += R"({"t":"concept","text":"факт"})";
    }
    json += "]}";
    EXPECT_THROW(parse_draft(json), DraftError);
}

TEST(DraftTest, RefusesEmptyDraft) {
    EXPECT_THROW(parse_draft(R"({"records":[]})"), DraftError);
    EXPECT_THROW(parse_draft(R"({})"),             DraftError);
}

// ── Malformed input is rejected, never guessed at ─────────────────────────────

TEST(DraftTest, RefusesMalformedJson) {
    EXPECT_THROW(parse_draft(R"({"records":[{"t":"concept",)"), DraftError);
    EXPECT_THROW(parse_draft("[]"),                             DraftError);
    EXPECT_THROW(parse_draft(""),                               DraftError);
    EXPECT_THROW(parse_draft(R"({"records":["строка"]})"),      DraftError);
}

TEST(DraftTest, RefusesMissingOrEmptyFields) {
    EXPECT_THROW(parse_draft(R"({"records":[{"t":"concept"}]})"),           DraftError);
    EXPECT_THROW(parse_draft(R"({"records":[{"t":"concept","text":""}]})"), DraftError);
    EXPECT_THROW(parse_draft(R"({"records":[{"t":"concept","text":"x","tags":[1]}]})"),
                 DraftError);
    EXPECT_THROW(parse_draft(R"({"records":[{"t":"composite","title":"П","parts":[]}]})"),
                 DraftError);
}

TEST(DraftTest, RefusesBadRefs) {
    EXPECT_THROW(
        parse_draft(R"({"records":[{"t":"concept-link","from":"нехеш","to":"нехеш","kind":"к"}]})"),
        DraftError);
    // 64 hex, but no chain/hash separator
    EXPECT_THROW(
        parse_draft(R"({"records":[{"t":"concept-link","from":")" + HASH +
                    R"(","to":")" + REF + R"(","kind":"к"}]})"),
        DraftError);
}

// ── An AI commonly escapes Cyrillic — decode it, don't mangle it ──────────────

// The escapes are built as ordinary string literals, so the parser really does
// receive backslash-u-XXXX — writing them raw would silently test nothing.

TEST(DraftTest, DecodesUnicodeEscapes) {
    const std::string escaped =   // "Электрик"
        "\\u042d\\u043b\\u0435\\u043a\\u0442\\u0440\\u0438\\u043a";
    const Draft draft = parse_draft(
        R"({"records":[{"t":"concept","text":")" + escaped + R"("}]})");

    const auto* c = std::get_if<Concept>(&draft.records[0]);
    ASSERT_NE(c, nullptr);
    EXPECT_EQ(c->text, "Электрик");
}

TEST(DraftTest, DecodesSurrogatePair) {
    const std::string escaped = "\\ud83d\\udca1";   // U+1F4A1 💡
    const Draft draft = parse_draft(
        R"({"records":[{"t":"concept","text":")" + escaped + R"("}]})");

    const auto* c = std::get_if<Concept>(&draft.records[0]);
    ASSERT_NE(c, nullptr);
    EXPECT_EQ(c->text, "\xF0\x9F\x92\xA1");
}

TEST(DraftTest, RefusesLoneSurrogate) {
    const std::string lone = "\\ud83d";
    EXPECT_THROW(
        parse_draft(R"({"records":[{"t":"concept","text":")" + lone + R"("}]})"),
        DraftError);
}

// ── Rendering: what the owner reads before signing ───────────────────────────

TEST(DraftTest, RenderShowsTextAndEveryTag) {
    Concept c;
    c.text = "Электромонтаж";
    c.tags = {"kind:skill", "cat:prof.electrician", "r:30"};

    const std::string out = render_record(c);
    EXPECT_NE(out.find("Электромонтаж"),       std::string::npos);
    EXPECT_NE(out.find("kind:skill"),          std::string::npos);
    EXPECT_NE(out.find("cat:prof.electrician"), std::string::npos);
    EXPECT_NE(out.find("r:30"),                std::string::npos);
}
