#include "aggregator/cloud_view.h"

#include <records/codec.h>

#include <algorithm>
#include <cmath>
#include <map>
#include <string>

namespace aggregator {

std::map<std::string, SpecialtyComposition> build_specialty_compositions(
    const AggregatorStorage& storage) {
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

    // Sums first, shares at the very end.
    struct Sum {
        std::map<RefHash, double> axes;
        double   carried = 0.0, payable = 0.0;
        uint64_t deals   = 0;
    };
    std::map<std::string, Sum> per_spec;
    for (const auto& [acc_hash, deal] : deals) {
        const auto& a = deal.acc;
        if (a.axes.empty() || a.hours_raw <= 0.0) continue;
        const double carried = a.carried_units ? *a.carried_units : 0.0;
        const double payable = a.labor_units + carried;
        if (payable <= 0.0) continue;
        const auto pit = paid.find(acc_hash);
        if (pit == paid.end() || std::abs(pit->second - payable) > 1e-6) continue;
        if (a.work.chain == deal.payer.bytes) continue;                  // self-deal

        // Разбивка ЕСТЬ цена: не сходящаяся описывает какую-то другую сделку.
        double named = 0.0;
        for (const auto& x : a.axes) named += x.units;
        if (std::abs(named - a.labor_units) > 1e-6) continue;

        const auto wit = by_hash.find(a.work.hash);
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

        auto& s = per_spec[spec->name];
        for (const auto& x : a.axes) s.axes[x.axis.hash] += x.units;
        s.carried += carried;
        s.payable += payable;
        s.deals   += 1;
    }

    std::map<std::string, SpecialtyComposition> out;
    for (const auto& [name, s] : per_spec) {
        SpecialtyComposition c{};
        for (const auto& [axis, units] : s.axes) c.axes[axis] = units / s.payable;
        c.carried = s.carried / s.payable;
        c.payable = s.payable;
        c.deals   = s.deals;
        out.emplace(name, std::move(c));
    }
    return out;
}

double composition_affinity(const SpecialtyComposition& a,
                            const SpecialtyComposition& b) noexcept {
    // Only what BOTH were paid for counts. The intersection is walked in key order
    // whichever map is the smaller, so (a, b) and (b, a) sum identically.
    const auto& small = a.axes.size() <= b.axes.size() ? a.axes : b.axes;
    const auto& large = a.axes.size() <= b.axes.size() ? b.axes : a.axes;
    double s = 0.0;
    for (const auto& [axis, x] : small) {
        const auto it = large.find(axis);
        if (it != large.end() && x > 0.0 && it->second > 0.0)
            s += std::sqrt(x * it->second);
    }
    if (a.carried > 0.0 && b.carried > 0.0) s += std::sqrt(a.carried * b.carried);
    return std::min(1.0, s);   // rounding only: Σ√(aⱼbⱼ) ≤ 1 by Cauchy–Schwarz
}

namespace {

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

records::SpecialtyCloud build_specialty_cloud(
    const std::map<std::string, SpecialtyComposition>& comps,
    int64_t                                            date,
    int64_t                                            timestamp,
    const std::array<uint8_t, 32>&                     snapshot,
    const std::vector<std::array<uint8_t, 32>>&        sources,
    unsigned                                           k,
    const std::vector<records::Catalog>*               catalogs) {
    records::SpecialtyCloud cloud{};
    cloud.date      = date;
    cloud.timestamp = timestamp;
    cloud.snapshot  = snapshot;
    cloud.sources   = sources;
    // v3: read off the deals (ИР-022). v1/v2 placed the catalog's declared axes,
    // with a danger weight and a capital weight nobody could derive from anything.
    cloud.params    = "v3;basis=deals;affinity=bhattacharyya;k=" + std::to_string(k);

    for (const auto& [slug, comp] : comps) {          // map order = slug order
        records::CloudPoint p{};
        p.slug = slug;
        if (catalogs)
            for (const auto& cat : *catalogs)
                if (const auto* e = cat.find(slug)) { p.parent = e->parent; break; }

        std::vector<records::CloudNeighbor> cand;
        for (const auto& [other, oc] : comps) {
            if (other == slug) continue;
            const double w = composition_affinity(comp, oc);
            if (w > 0.0) cand.push_back({other, w});   // nothing in common — no neighbour
        }
        std::sort(cand.begin(), cand.end(),
                  [](const records::CloudNeighbor& x, const records::CloudNeighbor& y) {
                      if (x.weight != y.weight) return x.weight > y.weight;
                      return x.slug < y.slug;
                  });
        if (cand.size() > k) cand.resize(k);
        p.neighbors = std::move(cand);
        cloud.points.push_back(std::move(p));
    }
    return cloud;
}

std::map<std::string, std::array<double, 2>> compute_spectral_coords(
    const std::map<std::string, SpecialtyComposition>& comps) {
    std::vector<const std::string*>          slugs;
    std::vector<const SpecialtyComposition*> pts;
    for (const auto& [slug, c] : comps) {
        slugs.push_back(&slug);
        pts.push_back(&c);
    }
    const int n = static_cast<int>(pts.size());
    std::map<std::string, std::array<double, 2>> out;
    if (n < 3) return out;  // too few points for a 2D map

    // Affinity W (the same one the neighbours use), then the unnormalized graph
    // Laplacian L = D − W. Laplacian eigenmaps: the eigenvectors of the two
    // smallest non-trivial eigenvalues are the 2D coordinates.
    std::vector<std::vector<double>> L(n, std::vector<double>(n, 0.0));
    std::vector<double> deg(n, 0.0);
    for (int i = 0; i < n; ++i)
        for (int j = i + 1; j < n; ++j) {
            const double w = composition_affinity(*pts[i], *pts[j]);
            L[i][j] = L[j][i] = -w;
            deg[i] += w;
            deg[j] += w;
        }
    for (int i = 0; i < n; ++i) L[i][i] = deg[i];

    std::vector<double> val;
    std::vector<std::vector<double>> vec;
    jacobi_eigen(L, val, vec);

    // Ascending eigenvalues, ties by index. Groups with nothing in common make
    // the graph fall apart, and each part brings its own zero eigenvalue; the
    // index tie-break keeps the choice among them the same for every witness.
    std::vector<int> ord(n);
    for (int i = 0; i < n; ++i) ord[i] = i;
    std::sort(ord.begin(), ord.end(), [&](int a, int b) {
        return val[a] != val[b] ? val[a] < val[b] : a < b;
    });
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
        out[*slugs[i]] = {vec[i][c1], vec[i][c2]};
    return out;
}

}  // namespace aggregator
