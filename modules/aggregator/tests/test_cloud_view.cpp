#include "aggregator/cloud_view.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <string>

using namespace aggregator;
using records::Catalog;
using records::CatalogEntry;

namespace {

CatalogEntry spec(const std::string& slug, const std::string& parent,
                  double material, double info, double people, double danger) {
    CatalogEntry e;
    e.slug          = slug;
    e.parent        = parent;
    e.axes.material = material;
    e.axes.info     = info;
    e.axes.people   = people;
    e.axes.danger   = danger;
    e.axes.present  = true;
    return e;
}

// The tiny town from the seed prototype: object-of-labour memberships.
std::vector<Catalog> tiny_catalog() {
    Catalog profs;
    profs.name = "professions";
    profs.entries = {
        spec("prof.programmer", "prof.it",     0.0, 1.0, 0.0, 0.0),
        spec("prof.accountant", "prof.lawfin", 0.0, 1.0, 0.0, 0.0),
        spec("prof.scribe",     "prof.data",   0.0, 1.0, 0.0, 0.0),
        spec("prof.doctor",     "prof.care",   0.0, 0.4, 0.6, 0.15),
        spec("prof.teacher",    "prof.teach",  0.0, 0.3, 0.7, 0.02),
        spec("prof.electrician","prof.build",  0.9, 0.1, 0.0, 0.4),
        spec("prof.welder",     "prof.build",  1.0, 0.0, 0.0, 0.5),
        spec("prof.cook",       "prof.daily",  1.0, 0.0, 0.0, 0.1),
    };
    return {profs};
}

const records::CloudPoint* point(const records::SpecialtyCloud& c, const std::string& slug) {
    for (const auto& p : c.points)
        if (p.slug == slug) return &p;
    return nullptr;
}

}  // namespace

TEST(CloudView, NearestNeighboursFromAxes) {
    std::array<uint8_t, 32> snap{};
    snap.fill(0x01);
    const auto cloud = build_specialty_cloud(tiny_catalog(), 86'400, 100, snap);

    // All 8 specialties are points, sorted by slug, each carries its parent.
    EXPECT_EQ(cloud.points.size(), 8u);
    ASSERT_TRUE(std::is_sorted(cloud.points.begin(), cloud.points.end(),
        [](const auto& a, const auto& b) { return a.slug < b.slug; }));

    const auto* prog = point(cloud, "prof.programmer");
    ASSERT_NE(prog, nullptr);
    EXPECT_EQ(prog->parent, "prof.it");
    ASSERT_FALSE(prog->neighbors.empty());
    // Programmer (pure info) is nearest another pure-info specialty.
    const std::string nn = prog->neighbors.front().slug;
    EXPECT_TRUE(nn == "prof.accountant" || nn == "prof.scribe") << nn;

    // Welder (material + high danger) is nearest the electrician (same craft),
    // not the cook (material but safe) — danger in the distance.
    const auto* weld = point(cloud, "prof.welder");
    ASSERT_NE(weld, nullptr);
    EXPECT_EQ(weld->neighbors.front().slug, "prof.electrician");

    // Doctor (people + info) is nearest the teacher.
    const auto* doc = point(cloud, "prof.doctor");
    ASSERT_NE(doc, nullptr);
    EXPECT_EQ(doc->neighbors.front().slug, "prof.teacher");
}

TEST(CloudView, DangerWeightSeparatesSafeFromRisky) {
    std::array<uint8_t, 32> snap{};
    // With danger ignored (weight 0), welder's nearest is the cook or electrician
    // (all material); with danger weighted, electrician wins over the safe cook.
    const auto strong = build_specialty_cloud(tiny_catalog(), 0, 0, snap, {}, 5, 4.0);
    const auto* w = point(strong, "prof.welder");
    ASSERT_NE(w, nullptr);
    EXPECT_EQ(w->neighbors.front().slug, "prof.electrician");
}

TEST(CloudView, Deterministic) {
    std::array<uint8_t, 32> snap{};
    snap.fill(0x09);
    const auto a = build_specialty_cloud(tiny_catalog(), 86'400, 5, snap);
    const auto b = build_specialty_cloud(tiny_catalog(), 86'400, 5, snap);
    ASSERT_EQ(a.points.size(), b.points.size());
    for (std::size_t i = 0; i < a.points.size(); ++i) {
        EXPECT_EQ(a.points[i].slug, b.points[i].slug);
        ASSERT_EQ(a.points[i].neighbors.size(), b.points[i].neighbors.size());
        for (std::size_t j = 0; j < a.points[i].neighbors.size(); ++j) {
            EXPECT_EQ(a.points[i].neighbors[j].slug, b.points[i].neighbors[j].slug);
            EXPECT_DOUBLE_EQ(a.points[i].neighbors[j].weight,
                             b.points[i].neighbors[j].weight);
        }
    }
}
