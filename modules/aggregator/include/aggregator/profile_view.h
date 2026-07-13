#pragma once

#include "aggregator.h"

#include <map>
#include <optional>
#include <string>
#include <vector>

namespace aggregator {

// ── Profile view (records.md §8.6/§8.7; role: §13) ────────────────────────────
//
// Participants' self-description: skills, needs, aspirations, industries.
// It lives at the level of ideas, NOT the protocol — there is no dedicated
// record type. A profile fact is an ordinary Concept carrying a reserved facet
// tag (kind:skill, kind:need, ...) and, usually, a catalog entry (cat:<slug>).
//
// Tags are carried through VERBATIM. New attributes (geo, radius, working
// hours, ...) are just new tags — they reach consumers without any change to
// this view. That is deliberate: the convention evolves in docs and catalogs,
// not in aggregator code.
//
// Like every aggregator view — a cache and an index, never a source of truth:
// every fact is re-checkable against the chain whose key signed the block.

// How the author relates to the catalogued entry (records.md §8.6). The same
// catalog entry means different things under different facets: cat:prof.welder
// under Skill = "I can weld", under Aspiration = "I want to learn welding".
enum class ProfileFacet {
    Skill,       // kind:skill      — владею навыком / профессией
    Need,        // kind:need       — насущная потребность
    Aspiration,  // kind:aspiration — хочу освоить
    Industry,    // kind:industry   — хочу развивать отрасль
    Hobby,       // kind:hobby      — люблю делать
    Obstacle,    // kind:obstacle   — что мешает
};

// One profile fact = one Concept block carrying a kind:* tag.
struct ProfileFact {
    ProfileFacet             facet{};
    BlockAddress             address{};     // which branch declared it (+ ordering)
    Hash                     block_hash{};  // Ref = (address.user_id, block_hash)
    std::string              text;          // the human description (Concept.text)
    std::string              slug;          // cat:<slug>; empty when free-form
    std::vector<std::string> tags;          // every tag verbatim, unknown ones included
    bool                     closed = false;// owner closed it (ConceptLink "закрыто")
};

// Everything one chain says about itself, ordered by (node_index, block_index).
struct ChainProfile {
    std::vector<ProfileFact> facts;

    // Non-owning pointers into `facts` — valid while this ChainProfile lives.
    std::vector<const ProfileFact*> by_facet(ProfileFacet facet) const;
};

class ProfileView {
public:
    // Scans every known block once and decodes the Concept/ConceptLink records.
    static ProfileView build(const AggregatorStorage& storage);

    std::optional<ChainProfile> chain(const UserId& uid) const;

    const std::map<UserId, ChainProfile>& chains() const noexcept { return chains_; }

    // The matching primitive: chains that declared `slug` under `facet`.
    // Closed facts are excluded. Sorted, deduplicated.
    // Stitching demand to supply = by_slug(Need, s) against by_slug(Skill, s').
    std::vector<UserId> by_slug(ProfileFacet facet, const std::string& slug) const;

    // ── Reserved-tag parsing (records.md §8.6) ────────────────────────────────

    // "kind:skill" → Skill; nullopt for any other tag.
    static std::optional<ProfileFacet> facet_from_tag(const std::string& tag);

    // "skill", "need", ... — the JSON/CLI name of a facet.
    static const char* facet_name(ProfileFacet facet) noexcept;

    // Value of the first "<prefix><value>" tag, e.g. tag_value(tags, "cat:").
    // Empty when absent. Lets callers read attributes the view does not model.
    static std::string tag_value(const std::vector<std::string>& tags,
                                 const std::string&              prefix);

private:
    std::map<UserId, ChainProfile> chains_;
};

} // namespace aggregator
