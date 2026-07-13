#include <records/catalog.h>

#include <gtest/gtest.h>

using namespace records;

namespace {

const std::string PROFESSIONS = R"({
  "catalog": "professions",
  "version": "2026.07",
  "entries": [
    {"slug":"prof.electrician","ru":"Электрик","group":"Ремонт",
     "aliases":["электромонтёр"]},
    {"slug":"prof.cook","ru":"Повар","group":"Быт"}
  ]
})";

const std::string NEEDS = R"({
  "catalog": "needs",
  "version": "2026.07",
  "entries": [
    {"slug":"need.electrical","ru":"Электрика","group":"Ремонт",
     "closed_by":["prof.electrician"]},
    {"slug":"need.appliances","ru":"Бытовая техника","group":"Вещи",
     "closed_by":[]}
  ]
})";

} // namespace

TEST(CatalogTest, ParsesEntriesAndFindsBySlug) {
    const Catalog cat = parse_catalog(PROFESSIONS);
    EXPECT_EQ(cat.name,    "professions");
    EXPECT_EQ(cat.version, "2026.07");
    ASSERT_EQ(cat.entries.size(), 2u);

    const CatalogEntry* e = cat.find("prof.electrician");
    ASSERT_NE(e, nullptr);
    EXPECT_EQ(e->ru,    "Электрик");
    EXPECT_EQ(e->group, "Ремонт");
    ASSERT_EQ(e->aliases.size(), 1u);
    EXPECT_EQ(e->aliases[0], "электромонтёр");

    EXPECT_EQ(cat.find("prof.nonexistent"), nullptr);
}

// closed_by is what lets a program do what a person does in their head:
// "wiring is fixed by an electrician".
TEST(CatalogTest, NeedsCarryClosedBy) {
    const Catalog cat = parse_catalog(NEEDS);

    const CatalogEntry* wiring = cat.find("need.electrical");
    ASSERT_NE(wiring, nullptr);
    ASSERT_EQ(wiring->closed_by.size(), 1u);
    EXPECT_EQ(wiring->closed_by[0], "prof.electrician");

    // A thing, not a service — closed by no profession.
    const CatalogEntry* goods = cat.find("need.appliances");
    ASSERT_NE(goods, nullptr);
    EXPECT_TRUE(goods->closed_by.empty());
}

TEST(CatalogTest, ParsesBundleAndCollectsSlugs) {
    const std::string bundle =
        R"({"professions":)" + PROFESSIONS + R"(,"needs":)" + NEEDS + "}";

    const auto catalogs = parse_catalog_bundle(bundle);
    ASSERT_EQ(catalogs.size(), 2u);

    const auto slugs = all_slugs(catalogs);
    EXPECT_EQ(slugs.size(), 4u);
    EXPECT_TRUE(slugs.count("prof.electrician"));
    EXPECT_TRUE(slugs.count("need.appliances"));
    EXPECT_FALSE(slugs.count("prof.typo"));   // the validation set is exact
}

TEST(CatalogTest, SearchMatchesSlugNameAndAlias) {
    const auto catalogs = parse_catalog_bundle(
        R"({"professions":)" + PROFESSIONS + R"(,"needs":)" + NEEDS + "}");

    EXPECT_EQ(search(catalogs, "электромонтёр").size(), 1u);   // by alias
    EXPECT_EQ(search(catalogs, "Повар").size(),         1u);   // by name
    EXPECT_EQ(search(catalogs, "need.").size(),         2u);   // by slug prefix
    EXPECT_TRUE(search(catalogs, "водолаз").empty());
    EXPECT_EQ(search(catalogs, "").size(),              4u);   // empty → everything
}

TEST(CatalogTest, RefusesMalformedCatalogs) {
    EXPECT_THROW(parse_catalog("{"),                             CatalogError);
    EXPECT_THROW(parse_catalog("[]"),                            CatalogError);
    EXPECT_THROW(parse_catalog(R"({"catalog":"x"})"),            CatalogError);
    EXPECT_THROW(parse_catalog(R"({"catalog":"x","version":"1"})"), CatalogError);
    // an entry without a slug
    EXPECT_THROW(parse_catalog(
        R"({"catalog":"x","version":"1","entries":[{"ru":"Без слага"}]})"),
        CatalogError);
    EXPECT_THROW(parse_catalog_bundle("{}"), CatalogError);
}
