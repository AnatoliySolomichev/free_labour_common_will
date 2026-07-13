#include "aggregator/profile_view.h"

#include <records/codec.h>
#include <records/types.h>

#include <algorithm>
#include <array>
#include <set>
#include <utility>

namespace aggregator {

namespace {

constexpr const char* FACET_PREFIX   = "kind:";
constexpr const char* CATALOG_PREFIX = "cat:";
// ConceptLink kind that retires a fact — authored by the fact's owner (§8.6).
constexpr const char* CLOSE_KIND     = "закрыто";

using RefHash = std::array<uint8_t, 32>;

} // namespace

std::vector<const ProfileFact*> ChainProfile::by_facet(ProfileFacet facet) const {
    std::vector<const ProfileFact*> out;
    for (const auto& fact : facts)
        if (fact.facet == facet) out.push_back(&fact);
    return out;
}

std::optional<ProfileFacet> ProfileView::facet_from_tag(const std::string& tag) {
    const std::string prefix = FACET_PREFIX;
    if (tag.rfind(prefix, 0) != 0) return std::nullopt;
    const std::string value = tag.substr(prefix.size());
    if (value == "skill")      return ProfileFacet::Skill;
    if (value == "need")       return ProfileFacet::Need;
    if (value == "aspiration") return ProfileFacet::Aspiration;
    if (value == "industry")   return ProfileFacet::Industry;
    if (value == "hobby")      return ProfileFacet::Hobby;
    if (value == "obstacle")   return ProfileFacet::Obstacle;
    return std::nullopt;   // an unknown kind: tag is not a profile facet
}

const char* ProfileView::facet_name(ProfileFacet facet) noexcept {
    switch (facet) {
        case ProfileFacet::Skill:      return "skill";
        case ProfileFacet::Need:       return "need";
        case ProfileFacet::Aspiration: return "aspiration";
        case ProfileFacet::Industry:   return "industry";
        case ProfileFacet::Hobby:      return "hobby";
        case ProfileFacet::Obstacle:   return "obstacle";
    }
    return "unknown";
}

std::string ProfileView::tag_value(const std::vector<std::string>& tags,
                                   const std::string&              prefix) {
    for (const auto& tag : tags)
        if (tag.rfind(prefix, 0) == 0) return tag.substr(prefix.size());
    return {};
}

ProfileView ProfileView::build(const AggregatorStorage& storage) {
    ProfileView view;

    // Blocks are scanned in hash order, so a closing link may precede the fact
    // it retires — collect closures and apply them after the pass.
    std::set<std::pair<UserId, RefHash>> closed;

    for (const Hash& bh : storage.all_block_hashes()) {
        const auto block = storage.get_block_by_hash(bh);
        if (!block || block->type != BlockType::DATA) continue;
        const UserId owner = block->address.user_id;

        records::Record rec;
        try {
            rec = records::Codec::decode(block->payload.data(),
                                         block->payload.size());
        } catch (const records::CodecError&) {
            continue;
        }

        if (const auto* concept_rec = std::get_if<records::Concept>(&rec)) {
            std::optional<ProfileFacet> facet;
            for (const auto& tag : concept_rec->tags) {
                facet = facet_from_tag(tag);
                if (facet) break;
            }
            if (!facet) continue;   // an ordinary idea, not a profile fact

            ProfileFact fact;
            fact.facet      = *facet;
            fact.address    = block->address;
            fact.block_hash = bh;
            fact.text       = concept_rec->text;
            fact.tags       = concept_rec->tags;
            fact.slug       = tag_value(concept_rec->tags, CATALOG_PREFIX);
            view.chains_[owner].facts.push_back(std::move(fact));

        } else if (const auto* link = std::get_if<records::ConceptLink>(&rec)) {
            // Only the owner retires their own fact: the closing link must live
            // in the very chain it points at (records.md §8.6).
            if (link->kind == CLOSE_KIND && link->to.chain == owner.bytes)
                closed.insert({owner, link->to.hash});
        }
    }

    for (auto& [owner, profile] : view.chains_) {
        for (auto& fact : profile.facts)
            if (closed.count({owner, fact.block_hash.bytes}) != 0) fact.closed = true;

        std::sort(profile.facts.begin(), profile.facts.end(),
                  [](const ProfileFact& a, const ProfileFact& b) {
                      if (a.address.node_index != b.address.node_index)
                          return a.address.node_index < b.address.node_index;
                      return a.address.block_index < b.address.block_index;
                  });
    }
    return view;
}

std::optional<ChainProfile> ProfileView::chain(const UserId& uid) const {
    const auto it = chains_.find(uid);
    if (it == chains_.end()) return std::nullopt;
    return it->second;
}

std::vector<UserId> ProfileView::by_slug(ProfileFacet       facet,
                                         const std::string& slug) const {
    std::vector<UserId> out;
    if (slug.empty()) return out;
    for (const auto& [uid, profile] : chains_) {
        for (const auto& fact : profile.facts) {
            if (fact.facet == facet && !fact.closed && fact.slug == slug) {
                out.push_back(uid);
                break;
            }
        }
    }
    return out;   // chains_ is a std::map — the result is sorted and unique
}

} // namespace aggregator
