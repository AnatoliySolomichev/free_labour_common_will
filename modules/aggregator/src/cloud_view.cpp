#include "aggregator/cloud_view.h"

#include <records/codec.h>

#include <algorithm>
#include <cmath>
#include <map>
#include <set>
#include <string>

namespace aggregator {

using records::CatalogEntry;

// Per-specialty capital-intensity from SETTLED accepted works (ИР-018 phase 2).
// Mirrors the rates traversal: an Acceptance counts only if a Transfer settles it
// exactly (paid == labor + carried) and it is non-self; resolved to its specialty
// via WorkRecord → Grade → Specialty.
std::map<std::string, double> build_capital_intensity(const AggregatorStorage& storage) {
    using RefHash = std::array<uint8_t, 32>;
    std::map<RefHash, records::Record> by_hash;
    struct Deal { records::Acceptance acc; UserId payer; };
    std::map<RefHash, Deal>   deals;
    std::map<RefHash, double> paid;

    for (const Hash& bh : storage.all_block_hashes()) {
        const auto block = storage.get_block_by_hash(bh);
        if (!block || block->type != BlockType::DATA) continue;
        records::Record rec;
        try { rec = records::Codec::decode(block->payload.data(), block->payload.size()); }
        catch (const records::CodecError&) { continue; }
        if (const auto* a = std::get_if<records::Acceptance>(&rec)) {
            deals[bh.bytes] = Deal{*a, block->address.user_id};
        } else if (const auto* t = std::get_if<records::Transfer>(&rec)) {
            if (t->from == block->address.user_id.bytes && t->reason)
                for (const auto& o : t->origins) paid[t->reason->hash] += o.units;
        }
        by_hash[bh.bytes] = std::move(rec);
    }

    struct Accum { double sum = 0.0; uint64_t n = 0; };
    std::map<std::string, Accum> per_spec;
    for (const auto& [acc_hash, deal] : deals) {
        const double carried = deal.acc.carried_units ? *deal.acc.carried_units : 0.0;
        const double payable = deal.acc.labor_units + carried;
        const auto pit = paid.find(acc_hash);
        if (pit == paid.end() || std::abs(pit->second - payable) > 1e-6) continue;
        if (deal.acc.work.chain == deal.payer.bytes) continue;             // self-deal
        const auto wit = by_hash.find(deal.acc.work.hash);
        if (wit == by_hash.end()) continue;
        const auto* wr = std::get_if<records::WorkRecord>(&wit->second);
        if (!wr) continue;
        const auto git = by_hash.find(wr->agent.hash);
        if (git == by_hash.end()) continue;
        const auto* grade = std::get_if<records::Grade>(&git->second);
        if (!grade) continue;
        const auto sit = by_hash.find(grade->specialty.hash);
        if (sit == by_hash.end()) continue;
        const auto* spec = std::get_if<records::Specialty>(&sit->second);
        if (!spec) continue;

        auto& a = per_spec[spec->name];
        a.sum += payable > 0.0 ? carried / payable : 0.0;
        a.n   += 1;
    }
    std::map<std::string, double> out;
    for (const auto& [name, a] : per_spec)
        if (a.n) out[name] = a.sum / static_cast<double>(a.n);
    return out;
}

namespace {

// Effective declared axes of an activity (bootstrap ⊕ attestation overrides).
struct AxisVec { double material = 0, info = 0, people = 0, danger = 0; };

// Proximity 0..1 on the declared axes (1 = identical). Danger scaled by weight so
// same-craft-but-different-danger activities are pulled apart; capital-intensity
// (derived, phase 2) added as a further axis scaled by capital_weight.
double axis_proximity(const AxisVec& x, const AxisVec& y, double dw,
                      double ci_a, double ci_b, double cw) {
    const double d2 = (x.material - y.material) * (x.material - y.material)
                    + (x.info     - y.info)     * (x.info     - y.info)
                    + (x.people   - y.people)   * (x.people   - y.people)
                    + dw * (x.danger - y.danger) * (x.danger - y.danger)
                    + cw * (ci_a - ci_b) * (ci_a - ci_b);
    return 1.0 / (1.0 + std::sqrt(d2));
}

using AttestMap = std::map<std::pair<std::string, std::string>, double>;

// Effective declared axes: catalog bootstrap overridden by attestations (ИР-019).
AxisVec effective_axes(const CatalogEntry& e, const AttestMap* attested) {
    AxisVec v{e.axes.material, e.axes.info, e.axes.people, e.axes.danger};
    if (attested) {
        auto ov = [&](const char* ax, double& dst) {
            const auto it = attested->find({e.slug, ax});
            if (it != attested->end()) dst = it->second;
        };
        ov("material", v.material); ov("info", v.info);
        ov("people", v.people);     ov("danger", v.danger);
    }
    return v;
}

// Jacobi eigenvalue algorithm for a real symmetric matrix (deterministic — no
// randomness, converges by zeroing off-diagonals). Fills `val` (eigenvalues) and
// `vec` (vec[i] = i-th eigenvector). Small n (specialties), O(n³·sweeps) is ample.
void jacobi_eigen(std::vector<std::vector<double>> a,
                  std::vector<double>& val,
                  std::vector<std::vector<double>>& vec) {
    const int n = static_cast<int>(a.size());
    vec.assign(n, std::vector<double>(n, 0.0));
    for (int i = 0; i < n; ++i) vec[i][i] = 1.0;
    for (int sweep = 0; sweep < 100; ++sweep) {
        double off = 0.0;
        for (int p = 0; p < n; ++p)
            for (int q = p + 1; q < n; ++q) off += a[p][q] * a[p][q];
        if (off < 1e-18) break;
        for (int p = 0; p < n; ++p) {
            for (int q = p + 1; q < n; ++q) {
                if (std::abs(a[p][q]) < 1e-300) continue;
                const double theta = (a[q][q] - a[p][p]) / (2.0 * a[p][q]);
                const double t = (theta >= 0 ? 1.0 : -1.0)
                               / (std::abs(theta) + std::sqrt(theta * theta + 1.0));
                const double c = 1.0 / std::sqrt(t * t + 1.0);
                const double s = t * c;
                for (int i = 0; i < n; ++i) {
                    const double aip = a[i][p], aiq = a[i][q];
                    a[i][p] = c * aip - s * aiq;
                    a[i][q] = s * aip + c * aiq;
                }
                for (int i = 0; i < n; ++i) {
                    const double api = a[p][i], aqi = a[q][i];
                    a[p][i] = c * api - s * aqi;
                    a[q][i] = s * api + c * aqi;
                }
                for (int i = 0; i < n; ++i) {
                    const double vip = vec[i][p], viq = vec[i][q];
                    vec[i][p] = c * vip - s * viq;
                    vec[i][q] = s * vip + c * viq;
                }
            }
        }
    }
    val.assign(n, 0.0);
    for (int i = 0; i < n; ++i) val[i] = a[i][i];
}

}  // namespace

std::map<std::pair<std::string, std::string>, double> build_axis_attestations(
    const AggregatorStorage& storage, unsigned min_attesters) {
    using RefHash = std::array<uint8_t, 32>;
    std::map<RefHash, records::Record> by_hash;
    for (const Hash& bh : storage.all_block_hashes()) {
        const auto block = storage.get_block_by_hash(bh);
        if (!block || block->type != BlockType::DATA) continue;
        try { by_hash[bh.bytes] = records::Codec::decode(block->payload.data(),
                                                         block->payload.size()); }
        catch (const records::CodecError&) {}
    }

    // Per (activity, axis): keep one entry per attester chain — the latest — so a
    // single person cannot ballot-stuff. weight = attester's grade level here.
    struct Att { double value; double weight; int64_t ts; };
    std::map<std::pair<std::string, std::string>,
             std::map<RefHash, Att>> groups;
    for (const auto& [h, rec] : by_hash) {
        const auto* a = std::get_if<records::AxisAttestation>(&rec);
        if (!a) continue;
        double weight = 1.0;
        const auto git = by_hash.find(a->grade.hash);
        if (git != by_hash.end())
            if (const auto* g = std::get_if<records::Grade>(&git->second))
                weight = static_cast<double>(g->level);
        auto& per = groups[{a->activity, a->axis}][a->grade.chain];
        if (a->timestamp >= per.ts)              // latest attestation of this attester
            per = Att{a->value, weight, a->timestamp};
    }

    std::map<std::pair<std::string, std::string>, double> out;
    for (auto& [key, per] : groups) {
        if (per.size() < min_attesters) continue;   // preliminary — bootstrap stands
        std::vector<Att> v;
        v.reserve(per.size());
        for (const auto& [chain, a] : per) v.push_back(a);
        std::sort(v.begin(), v.end(),
                  [](const Att& x, const Att& y) { return x.value < y.value; });
        double total = 0.0;
        for (const auto& e : v) total += e.weight;
        double cum = 0.0, med = v.back().value;
        for (const auto& e : v) { cum += e.weight; if (cum >= total / 2.0) { med = e.value; break; } }
        out[key] = med;
    }
    return out;
}

records::SpecialtyCloud build_specialty_cloud(
    const std::vector<records::Catalog>&        catalogs,
    int64_t                                     date,
    int64_t                                     timestamp,
    const std::array<uint8_t, 32>&              snapshot,
    const std::vector<std::array<uint8_t, 32>>& sources,
    unsigned                                    k,
    double                                      danger_weight,
    const std::map<std::string, double>*        capital,
    double                                      capital_weight,
    const std::map<std::pair<std::string, std::string>, double>* attested) {
    auto eff = [&](const CatalogEntry& e) { return effective_axes(e, attested); };
    auto cap = [&](const std::string& slug) -> double {
        if (!capital) return 0.0;
        const auto it = capital->find(slug);
        return it == capital->end() ? 0.0 : it->second;
    };

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
    cloud.params    = std::string(capital ? "v2;phase=2" : "v1;phase=1")
                    + ";k=" + std::to_string(k)
                    + ";danger_w=" + std::to_string(danger_weight)
                    + ";capital_w=" + std::to_string(capital ? capital_weight : 0.0);

    for (const auto* a : specs) {
        records::CloudPoint p{};
        p.slug   = a->slug;
        p.parent = a->parent;

        const auto sit = subs.find(a->slug);
        std::vector<records::CloudNeighbor> cand;
        cand.reserve(specs.size());
        const double  ci_a  = cap(a->slug);
        const AxisVec eff_a = eff(*a);
        for (const auto* b : specs) {
            if (b == a) continue;
            double sim = axis_proximity(eff_a, eff(*b), danger_weight,
                                        ci_a, cap(b->slug), capital_weight);
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

std::map<std::string, std::array<double, 2>> compute_spectral_coords(
    const std::vector<records::Catalog>&        catalogs,
    double                                      danger_weight,
    const std::map<std::string, double>*        capital,
    double                                      capital_weight,
    const std::map<std::pair<std::string, std::string>, double>* attested) {

    std::vector<const CatalogEntry*> specs;
    for (const auto& cat : catalogs)
        for (const auto& e : cat.entries)
            if (e.axes.present) specs.push_back(&e);
    const int n = static_cast<int>(specs.size());
    std::map<std::string, std::array<double, 2>> out;
    if (n < 3) return out;  // too few points for a 2D map

    std::map<std::string, std::set<std::string>> subs;
    for (const auto& cat : catalogs)
        for (const auto& e : cat.entries)
            for (std::size_t i = 0; i < e.closed_by.size(); ++i)
                for (std::size_t j = i + 1; j < e.closed_by.size(); ++j) {
                    subs[e.closed_by[i]].insert(e.closed_by[j]);
                    subs[e.closed_by[j]].insert(e.closed_by[i]);
                }
    auto cap = [&](const std::string& slug) -> double {
        if (!capital) return 0.0;
        const auto it = capital->find(slug);
        return it == capital->end() ? 0.0 : it->second;
    };

    // Affinity W (same proximity as the neighbour weights), then the unnormalized
    // graph Laplacian L = D − W. Laplacian eigenmaps: the eigenvectors for the two
    // smallest non-trivial eigenvalues are the 2D coordinates (the classic layout).
    std::vector<std::vector<double>> L(n, std::vector<double>(n, 0.0));
    std::vector<double> deg(n, 0.0);
    for (int i = 0; i < n; ++i) {
        const AxisVec ai = effective_axes(*specs[i], attested);
        const auto    si = subs.find(specs[i]->slug);
        for (int j = i + 1; j < n; ++j) {
            double w = axis_proximity(ai, effective_axes(*specs[j], attested),
                                      danger_weight, cap(specs[i]->slug),
                                      cap(specs[j]->slug), capital_weight);
            if (si != subs.end() && si->second.count(specs[j]->slug))
                w = std::min(1.0, w + 0.5);
            L[i][j] = L[j][i] = -w;
            deg[i] += w;
            deg[j] += w;
        }
    }
    for (int i = 0; i < n; ++i) L[i][i] = deg[i];

    std::vector<double> val;
    std::vector<std::vector<double>> vec;
    jacobi_eigen(L, val, vec);

    // Order eigenvalues ascending; skip index 0 (≈0, the constant vector) and take
    // the next two eigenvectors as coordinates.
    std::vector<int> ord(n);
    for (int i = 0; i < n; ++i) ord[i] = i;
    std::sort(ord.begin(), ord.end(),
              [&](int a, int b) { return val[a] < val[b]; });
    const int c1 = ord[1], c2 = ord[2];

    // Canonical sign: make the largest-magnitude component positive (deterministic).
    auto canon = [&](int col) {
        int best = 0;
        for (int i = 1; i < n; ++i)
            if (std::abs(vec[i][col]) > std::abs(vec[best][col])) best = i;
        if (vec[best][col] < 0)
            for (int i = 0; i < n; ++i) vec[i][col] = -vec[i][col];
    };
    canon(c1);
    canon(c2);

    for (int i = 0; i < n; ++i)
        out[specs[i]->slug] = {vec[i][c1], vec[i][c2]};
    return out;
}

}  // namespace aggregator
