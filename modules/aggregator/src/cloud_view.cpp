#include "aggregator/cloud_view.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <set>
#include <string>

namespace aggregator {

using records::CatalogEntry;

namespace {

// Proximity 0..1 on the declared axes (1 = identical). Danger scaled by weight so
// same-craft-but-different-danger activities are pulled apart.
double axis_proximity(const CatalogEntry& a, const CatalogEntry& b, double dw) {
    const auto& x = a.axes;
    const auto& y = b.axes;
    const double d2 = (x.material - y.material) * (x.material - y.material)
                    + (x.info     - y.info)     * (x.info     - y.info)
                    + (x.people   - y.people)   * (x.people   - y.people)
                    + dw * (x.danger - y.danger) * (x.danger - y.danger);
    return 1.0 / (1.0 + std::sqrt(d2));
}

}  // namespace

records::SpecialtyCloud build_specialty_cloud(
    const std::vector<records::Catalog>&        catalogs,
    int64_t                                     date,
    int64_t                                     timestamp,
    const std::array<uint8_t, 32>&              snapshot,
    const std::vector<std::array<uint8_t, 32>>& sources,
    unsigned                                    k,
    double                                      danger_weight) {

    // 1. Specialties with declared coordinates = leaves of the professions tree.
    std::vector<const CatalogEntry*> specs;
    for (const auto& cat : catalogs)
        for (const auto& e : cat.entries)
            if (e.axes.present) specs.push_back(&e);

    // 2. Substitution edges: specialties closing the same need are functionally
    //    near (records.md closed_by). Undirected slug → substitute slugs.
    std::map<std::string, std::set<std::string>> subs;
    for (const auto& cat : catalogs)
        for (const auto& e : cat.entries)
            for (std::size_t i = 0; i < e.closed_by.size(); ++i)
                for (std::size_t j = i + 1; j < e.closed_by.size(); ++j) {
                    subs[e.closed_by[i]].insert(e.closed_by[j]);
                    subs[e.closed_by[j]].insert(e.closed_by[i]);
                }

    records::SpecialtyCloud cloud{};
    cloud.date      = date;
    cloud.timestamp = timestamp;
    cloud.snapshot  = snapshot;
    cloud.sources   = sources;
    cloud.params    = "v1;phase=1;k=" + std::to_string(k)
                    + ";danger_w=" + std::to_string(danger_weight);

    for (const auto* a : specs) {
        records::CloudPoint p{};
        p.slug   = a->slug;
        p.parent = a->parent;

        const auto sit = subs.find(a->slug);
        std::vector<records::CloudNeighbor> cand;
        cand.reserve(specs.size());
        for (const auto* b : specs) {
            if (b == a) continue;
            double sim = axis_proximity(*a, *b, danger_weight);
            if (sit != subs.end() && sit->second.count(b->slug))
                sim = std::min(1.0, sim + 0.5);  // substitution boost
            cand.push_back({b->slug, sim});
        }
        // Top-k by proximity; tie-break by slug so the output is deterministic.
        std::sort(cand.begin(), cand.end(),
                  [](const records::CloudNeighbor& x, const records::CloudNeighbor& y) {
                      if (x.weight != y.weight) return x.weight > y.weight;
                      return x.slug < y.slug;
                  });
        if (cand.size() > k) cand.resize(k);
        p.neighbors = std::move(cand);
        cloud.points.push_back(std::move(p));
    }

    std::sort(cloud.points.begin(), cloud.points.end(),
              [](const records::CloudPoint& x, const records::CloudPoint& y) {
                  return x.slug < y.slug;
              });
    return cloud;
}

}  // namespace aggregator
