#include "aggregator/match_view.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <map>
#include <optional>
#include <set>

namespace aggregator {

namespace {

// How far a party will act, and from where (records.md §8.6).
struct Reach {
    bool                  unlimited = false;  // remote:yes or r:global
    std::optional<double> radius_km;          // r:<km>; absent → not stated
    std::optional<std::pair<double, double>> geo;  // geo:<lat>,<lon>
};

std::optional<std::pair<double, double>> parse_geo(const std::string& value) {
    const auto comma = value.find(',');
    if (comma == std::string::npos) return std::nullopt;
    try {
        return std::make_pair(std::stod(value.substr(0, comma)),
                              std::stod(value.substr(comma + 1)));
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

double haversine_km(double lat1, double lon1, double lat2, double lon2) {
    constexpr double R  = 6371.0;                 // mean Earth radius, km
    constexpr double PI = 3.14159265358979323846;
    auto rad = [](double deg) { return deg * PI / 180.0; };

    const double dlat = rad(lat2 - lat1);
    const double dlon = rad(lon2 - lon1);
    const double h = std::sin(dlat / 2) * std::sin(dlat / 2) +
                     std::cos(rad(lat1)) * std::cos(rad(lat2)) *
                     std::sin(dlon / 2) * std::sin(dlon / 2);
    return 2 * R * std::asin(std::min(1.0, std::sqrt(h)));
}

Reach reach_of(const std::vector<std::string>& tags) {
    Reach r;
    const auto radius = ProfileView::tag_value(tags, "r:");
    const auto remote = ProfileView::tag_value(tags, "remote:");
    if (remote == "yes" || radius == "global") r.unlimited = true;
    else if (!radius.empty()) {
        try { r.radius_km = std::stod(radius); }
        catch (const std::exception&) { /* unparseable → not stated */ }
    }
    const auto geo = ProfileView::tag_value(tags, "geo:");
    if (!geo.empty()) r.geo = parse_geo(geo);
    return r;
}

// Reachable unless both sides stated a place AND the distance exceeds a radius
// one of them stated. Missing data never hides a person: an unstated radius or
// coordinate means "no constraint", not "no match" — silent invisibility is the
// failure mode this whole design guards against.
// Returns the distance in km, or -1 when it does not apply; nullopt = out of reach.
std::optional<double> reachable(const Reach& need, const Reach& skill) {
    if (need.unlimited || skill.unlimited) return -1.0;
    if (!need.geo || !skill.geo)           return -1.0;

    const double d = haversine_km(need.geo->first,  need.geo->second,
                                  skill.geo->first, skill.geo->second);

    if (need.radius_km  && d > *need.radius_km)  return std::nullopt;
    if (skill.radius_km && d > *skill.radius_km) return std::nullopt;
    return d;
}

} // namespace

double MatchView::distance_km(const std::string& geo_a, const std::string& geo_b) {
    const auto a = parse_geo(geo_a);
    const auto b = parse_geo(geo_b);
    if (!a || !b) return -1;
    return haversine_km(a->first, a->second, b->first, b->second);
}

MatchView MatchView::build(const AggregatorStorage& storage,
                           const ClosedBy&          closed_by) {
    MatchView  view;
    const auto profiles = ProfileView::build(storage);

    // Supply, indexed by profession slug.
    struct Supply {
        UserId      chain{};
        Hash        hash{};
        std::string slug, text, grade;
        Reach       reach;
    };
    std::map<std::string, std::vector<Supply>> supply;
    std::set<UserId>                           willing_to_retrain;
    std::map<std::string, std::set<UserId>>    aspiring;   // prof slug → chains

    for (const auto& [uid, profile] : profiles.chains()) {
        for (const auto& fact : profile.facts) {
            if (fact.closed) continue;
            if (ProfileView::tag_value(fact.tags, "retrain:") == "yes")
                willing_to_retrain.insert(uid);

            if (fact.facet == ProfileFacet::Skill && !fact.slug.empty()) {
                supply[fact.slug].push_back(
                    Supply{uid, fact.block_hash, fact.slug, fact.text,
                           ProfileView::tag_value(fact.tags, "grade:"),
                           reach_of(fact.tags)});
            } else if (fact.facet == ProfileFacet::Aspiration && !fact.slug.empty()) {
                aspiring[fact.slug].insert(uid);
            }
        }
    }

    // Demand, matched against supply through the catalog's closed_by.
    std::map<UserId, std::set<UserId>> helps;   // seeker → those who close a need
    std::map<std::string, Deficit>     deficits;

    for (const auto& [uid, profile] : profiles.chains()) {
        for (const auto& fact : profile.facts) {
            if (fact.facet != ProfileFacet::Need || fact.closed) continue;

            NeedMatch m;
            m.seeker     = uid;
            m.block_hash = fact.block_hash;
            m.slug       = fact.slug;
            m.text       = fact.text;
            m.urgency    = ProfileView::tag_value(fact.tags, "urgency:");
            const Reach need_reach = reach_of(fact.tags);

            const auto professions = closed_by.find(fact.slug);
            if (professions != closed_by.end()) {
                for (const auto& prof : professions->second) {
                    const auto found = supply.find(prof);
                    if (found == supply.end()) continue;
                    for (const auto& s : found->second) {
                        if (s.chain == uid) continue;          // not from oneself
                        const auto d = reachable(need_reach, s.reach);
                        if (!d) continue;                      // out of reach
                        m.candidates.push_back(
                            MatchCandidate{s.chain, s.hash, s.slug, s.text,
                                           s.grade, *d});
                        helps[uid].insert(s.chain);
                    }
                }
            }

            if (m.candidates.empty() && !fact.slug.empty()) {
                Deficit& d = deficits[fact.slug];
                d.need_slug = fact.slug;
                if (d.text.empty()) d.text = fact.text;
                if (professions != closed_by.end()) d.professions = professions->second;
            }
            view.needs_.push_back(std::move(m));
        }
    }

    // Who could grow into each deficit. Scarcity is answered by retraining, so
    // the seeker is NOT excluded: someone learning the missing trade serves the
    // whole circle, their own need included.
    for (auto& [slug, d] : deficits) {
        for (const auto& prof : d.professions) {
            const auto it = aspiring.find(prof);
            if (it == aspiring.end()) continue;
            d.aspiring.insert(d.aspiring.end(), it->second.begin(), it->second.end());
        }
        std::sort(d.aspiring.begin(), d.aspiring.end());
        d.aspiring.erase(std::unique(d.aspiring.begin(), d.aspiring.end()),
                         d.aspiring.end());
        d.willing.assign(willing_to_retrain.begin(), willing_to_retrain.end());
        view.deficits_.push_back(d);
    }

    // Rings: A is helped by B, B by C, C by A — they settle without anyone owing
    // an outsider. Enumerated from each start, visiting only nodes that are not
    // smaller than it, then deduplicated by canonical rotation.
    std::set<std::vector<UserId>> seen;
    std::vector<UserId>           path;

    std::function<void(const UserId&, const UserId&)> walk =
        [&](const UserId& start, const UserId& node) {
            if (path.size() > MAX_RING) return;
            const auto edges = helps.find(node);
            if (edges == helps.end()) return;

            for (const auto& next : edges->second) {
                if (next == start && path.size() >= 2) {
                    std::vector<UserId> ring = path;
                    std::rotate(ring.begin(),
                                std::min_element(ring.begin(), ring.end()),
                                ring.end());
                    if (seen.insert(ring).second) view.rings_.push_back(Ring{ring});
                    continue;
                }
                if (next < start) continue;    // rings through it were already walked
                if (std::find(path.begin(), path.end(), next) != path.end()) continue;
                path.push_back(next);
                walk(start, next);
                path.pop_back();
            }
        };

    for (const auto& [node, _] : helps) {
        path.assign(1, node);
        walk(node, node);
    }
    std::sort(view.rings_.begin(), view.rings_.end(),
              [](const Ring& a, const Ring& b) {
                  return a.chains.size() < b.chains.size();   // tightest first
              });
    return view;
}

} // namespace aggregator
