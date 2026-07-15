#include "aggregator/attestation_view.h"

#include <records/codec.h>
#include <records/types.h>

#include <array>
#include <set>

namespace aggregator {

namespace {

using RefHash = std::array<uint8_t, 32>;

struct GradeInfo {
    UserId  owner{};
    uint8_t level = 0;
    RefHash specialty{};
    RefHash spec_chain{};   // Ref.chain of the specialty — must be the worker
};

struct WorkInfo {
    UserId  owner{};
    RefHash agent{};        // Grade block hash
    RefHash agent_chain{};  // Ref.chain of the agent — must be the worker
};

} // namespace

AttestationView AttestationView::build(const AggregatorStorage& storage) {
    AttestationView view;

    std::map<RefHash, std::pair<UserId, std::string>> specialties; // hash → (owner, slug)
    std::map<RefHash, GradeInfo>                      grades;
    std::map<RefHash, WorkInfo>                       works;
    struct Accept { UserId acceptor{}; RefHash work{}; double hours_raw = 0; };
    std::vector<Accept>                               accepts;

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

        if (const auto* s = std::get_if<records::Specialty>(&rec)) {
            specialties[bh.bytes] = {owner, s->name};   // name = catalog slug (§9.1)
        } else if (const auto* g = std::get_if<records::Grade>(&rec)) {
            grades[bh.bytes] =
                GradeInfo{owner, g->level, g->specialty.hash, g->specialty.chain};
        } else if (const auto* w = std::get_if<records::WorkRecord>(&rec)) {
            works[bh.bytes] = WorkInfo{owner, w->agent.hash, w->agent.chain};
        } else if (const auto* a = std::get_if<records::Acceptance>(&rec)) {
            if (a->receiver != owner.bytes) continue;   // spoofed receiver
            accepts.push_back(Accept{owner, a->work.hash, a->hours_raw});
        }
    }

    // Join every acceptance down to (worker, slug, level); refuse any hop
    // that leaves the worker's own chain — you attest labour, not paperwork.
    std::map<std::tuple<UserId, std::string, uint8_t>, std::set<UserId>> witnesses;
    for (const auto& acc : accepts) {
        const auto work = works.find(acc.work);
        if (work == works.end()) continue;
        const UserId& worker = work->second.owner;
        if (acc.acceptor == worker) continue;           // the free self-vouch

        if (work->second.agent_chain != worker.bytes) continue;
        const auto grade = grades.find(work->second.agent);
        if (grade == grades.end() || !(grade->second.owner == worker)) continue;

        if (grade->second.spec_chain != worker.bytes) continue;
        const auto spec = specialties.find(grade->second.specialty);
        if (spec == specialties.end() || !(spec->second.first == worker)) continue;

        const auto key = std::make_tuple(worker, spec->second.second,
                                         grade->second.level);
        auto& att = view.att_[key];
        att.level  = grade->second.level;
        att.hours += acc.hours_raw;
        witnesses[key].insert(acc.acceptor);
    }
    for (const auto& [key, who] : witnesses)
        view.att_[key].chains = who.size();

    return view;
}

std::optional<GradeAttestation> AttestationView::at_level(
        const UserId& worker, const std::string& slug, uint8_t level) const {
    const auto it = att_.find(std::make_tuple(worker, slug, level));
    if (it == att_.end()) return std::nullopt;
    return it->second;
}

std::optional<GradeAttestation> AttestationView::best(
        const UserId& worker, const std::string& slug) const {
    std::optional<GradeAttestation> out;
    for (const auto& [key, att] : att_)
        if (std::get<0>(key) == worker && std::get<1>(key) == slug)
            if (!out || att.level > out->level) out = att;
    return out;
}

} // namespace aggregator
