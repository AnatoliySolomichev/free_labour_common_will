#pragma once

#include "profile_view.h"

#include <map>
#include <string>
#include <vector>

namespace aggregator {

// ── Match view (records.md §8.6/§8.7; ИР-005) ─────────────────────────────────
//
// The point of the whole thing: whose needs are closed by whose skills.
//
// It joins three sources the aggregator already has:
//   ProfileView — who can do what, who needs what (tagged Concepts);
//   the catalog — which profession closes which need (closed_by, §8.7);
//   the tags    — where and how far each party will act (geo, r, remote).
//
// Advisory and re-checkable, like every derived view: it proposes, people decide.
// A biased aggregator can only skew suggestions — never a deal.

// need slug → professions that close it ("need.electrical" → ["prof.electrician"]).
// Comes from the catalog; without it a program cannot do what a person does in
// their head.
using ClosedBy = std::map<std::string, std::vector<std::string>>;

struct MatchCandidate {
    UserId      chain{};
    Hash        block_hash{};      // the skill fact
    std::string slug;              // prof.*
    std::string text;
    std::string grade;             // grade: tag, empty when not stated
    // Distance between the two parties, km. Negative when it does not apply:
    // one side works remotely, or a coordinate is missing.
    double      distance_km = -1;
};

struct NeedMatch {
    UserId      seeker{};
    Hash        block_hash{};      // the need fact
    std::string slug;              // need.*
    std::string text;
    std::string urgency;
    std::vector<MatchCandidate> candidates;   // empty → a deficit
};

// A need nobody can close, and who could grow into it. The project's answer to
// scarcity is retraining, not despair (ИР-005): people declare what they are
// willing to learn.
struct Deficit {
    std::string              need_slug;
    std::string              text;            // one of the unmet needs, for context
    std::vector<std::string> professions;     // what would close it (closed_by)
    std::vector<UserId>      aspiring;        // declared this very profession
    std::vector<UserId>      willing;         // willing to retrain at all (retrain:yes)
};

// A closed chain of help: chains[0] is helped by chains[1], … , the last by
// chains[0]. Rings settle without anyone ending up in debt to an outsider —
// the most valuable figure in the graph.
struct Ring {
    std::vector<UserId> chains;
};

class MatchView {
public:
    // `closed_by` maps need slugs to the professions that close them. Passing it
    // in (rather than a Catalog) keeps the records module out of this header.
    static MatchView build(const AggregatorStorage& storage,
                           const ClosedBy&          closed_by);

    // Every open need, matched or not, seeker-then-slug ordered.
    const std::vector<NeedMatch>& needs() const noexcept { return needs_; }

    // Needs with no candidate, grouped by slug.
    const std::vector<Deficit>& deficits() const noexcept { return deficits_; }

    // Rings of length 2..MAX_RING, deduplicated (each ring appears once,
    // starting from its smallest member).
    const std::vector<Ring>& rings() const noexcept { return rings_; }

    static constexpr std::size_t MAX_RING = 6;

    // Distance in km between two "lat,lon" tag values; negative when either is
    // unparseable.
    static double distance_km(const std::string& geo_a, const std::string& geo_b);

private:
    std::vector<NeedMatch> needs_;
    std::vector<Deficit>   deficits_;
    std::vector<Ring>      rings_;
};

} // namespace aggregator
