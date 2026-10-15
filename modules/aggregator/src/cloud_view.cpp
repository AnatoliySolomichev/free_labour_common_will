#include "aggregator/cloud_view.h"
#include "aggregator/recognition.h"

#include <records/codec.h>

#include <algorithm>
#include <cmath>
#include <map>
#include <string>

namespace aggregator {

std::map<SampleKey, WorkComposition> build_work_compositions(
    const AggregatorStorage& storage) {
    using RefHash = std::array<uint8_t, 32>;
    std::map<RefHash, records::WorkRecord> works;
    for (const Hash& bh : storage.all_block_hashes()) {
        const auto block = storage.get_block_by_hash(bh);
        if (!block || block->type != BlockType::DATA) continue;
        records::Record rec;
        try { rec = records::Codec::decode(block->payload.data(), block->payload.size()); }
        catch (const records::CodecError&) { continue; }
        if (const auto* w = std::get_if<records::WorkRecord>(&rec)) works[bh.bytes] = *w;
    }
    // Every deal is paid exactly by construction (records.md §9.5, ИР-023).
    const auto  table = deals_on_table(storage);   // shared, recomputed only on new blocks
    const auto& deals = *table;
    std::map<RefHash, const DealEntry*> deal_at;   // deal block hash → the deal
    for (const auto& d : deals.all) deal_at[d.hash] = &d;

    // The sample of a deal: follow `like` while it leads to a known acceptance.
    // `like` names an EARLIER block by its hash, so a cycle would need a hash
    // collision; the depth bound is only a guard.
    const auto sample_of = [&](const RefHash& h, const UserId& author) {
        records::Ref cur{};
        cur.chain = author.bytes;
        cur.hash  = h;
        for (int depth = 0; depth < 64; ++depth) {
            const auto it = deal_at.find(cur.hash);
            if (it == deal_at.end() || !it->second->accept().like) break;
            cur = *it->second->accept().like;
        }
        return cur;
    };
    const auto title_of = [&](const records::Acceptance& a) -> std::string {
        const auto it = works.find(a.work.hash);
        return it == works.end() ? std::string{} : it->second.title;
    };

    struct Sum {
        records::Ref              sample;
        std::map<RefHash, double> axes;
        double                    carried = 0.0, payable = 0.0;
        uint64_t                  deals   = 0;
        std::string               first_title;   // of the family, in block-hash order
    };
    std::map<RefHash, Sum> fam;
    // One work — one deal (records.md §9.5, ИР-023).
    for (const auto& deal : deals.all) {
        if (!deals.counts(deal.hash)) continue;
        const auto& a = deal.accept();
        if (a.axes.empty() || a.hours_raw <= 0.0) continue;
        const double carried = a.carried_units ? *a.carried_units : 0.0;
        const double payable = a.labor_units + carried;
        if (payable <= 0.0) continue;
        double named = 0.0;
        for (const auto& x : a.axes) named += x.units;
        if (std::abs(named - a.labor_units) > 1e-6) continue;           // another deal
        if (!works.count(a.work.hash)) continue;       // the work is not on the table

        const records::Ref sample = sample_of(deal.hash, deal.payer);
        auto& s = fam[sample.hash];
        s.sample = sample;
        for (const auto& x : a.axes) s.axes[x.axis.hash] += x.units;
        s.carried += carried;
        s.payable += payable;
        s.deals   += 1;
        if (s.first_title.empty()) s.first_title = title_of(a);
    }

    std::map<SampleKey, WorkComposition> out;
    for (const auto& [key, s] : fam) {
        if (s.deals < kMinFamily) continue;
        WorkComposition c{};
        c.sample = s.sample;
        // The sample's own title when the sample is known, else the family's.
        if (const auto it = deal_at.find(key); it != deal_at.end())
            c.title = title_of(it->second->accept());
        if (c.title.empty()) c.title = s.first_title;
        for (const auto& [axis, units] : s.axes) c.axes[axis] = units / s.payable;
        c.carried = s.carried / s.payable;
        c.payable = s.payable;
        c.deals   = s.deals;
        out.emplace(key, std::move(c));
    }
    return out;
}

double composition_affinity(const WorkComposition& a,
                            const WorkComposition& b) noexcept {
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
// `vec` (vec[i] = i-th eigenvector). Small n (samples), O(n³·sweeps) is ample.
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

records::WorkCloud build_work_cloud(
    const std::map<SampleKey, WorkComposition>&  comps,
    int64_t                                      date,
    int64_t                                      timestamp,
    const std::array<uint8_t, 32>&               snapshot,
    const std::vector<std::array<uint8_t, 32>>&  sources,
    unsigned                                     k) {
    records::WorkCloud cloud{};
    cloud.date      = date;
    cloud.timestamp = timestamp;
    cloud.snapshot  = snapshot;
    cloud.sources   = sources;
    // v4: points are samples of work, read off their families (ИР-022 шаг 11).
    // v3 placed specialties; v1/v2 the catalogue's declared axes.
    cloud.params    = "v4;basis=samples;affinity=bhattacharyya;k=" + std::to_string(k)
                    + ";min_family=" + std::to_string(kMinFamily);

    for (const auto& [key, comp] : comps) {           // map order = sample-hash order
        records::CloudPoint p{};
        p.sample = comp.sample;
        p.title  = comp.title;
        p.deals  = comp.deals;

        std::vector<std::pair<SampleKey, records::CloudNeighbor>> cand;
        for (const auto& [other, oc] : comps) {
            if (other == key) continue;
            const double w = composition_affinity(comp, oc);
            if (w > 0.0) cand.push_back({other, records::CloudNeighbor{oc.sample, w}});
        }
        std::sort(cand.begin(), cand.end(), [](const auto& x, const auto& y) {
            if (x.second.weight != y.second.weight) return x.second.weight > y.second.weight;
            return x.first < y.first;
        });
        if (cand.size() > k) cand.resize(k);
        for (auto& [other, n] : cand) {
            (void)other;
            p.neighbors.push_back(std::move(n));
        }
        cloud.points.push_back(std::move(p));
    }
    return cloud;
}

std::map<SampleKey, std::array<double, 2>> compute_spectral_coords(
    const std::map<SampleKey, WorkComposition>& comps) {
    std::vector<SampleKey>                   keys;
    std::vector<const WorkComposition*>      pts;
    for (const auto& [key, c] : comps) {
        keys.push_back(key);
        pts.push_back(&c);
    }
    const int n = static_cast<int>(pts.size());
    std::map<SampleKey, std::array<double, 2>> out;
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
        out[keys[i]] = {vec[i][c1], vec[i][c2]};
    return out;
}

}  // namespace aggregator
