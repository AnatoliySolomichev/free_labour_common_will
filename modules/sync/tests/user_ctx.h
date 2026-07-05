#pragma once

// Shared per-user test context: a real chain (LMDB), a participant cache and
// a dialogue factory. Used by the merge-dialogue and dialogue-channel tests.

#include "sync/merge_dialogue.h"
#include "sync/participant_cache.h"

#include <blockchain/blockchain.h>
#include <blockchain/crypto.h>
#include <blockchain/serializer.h>

#include <deque>
#include <filesystem>
#include <map>
#include <memory>

namespace sync_tests {

static constexpr blockchain::NodeIndex LEAF = 0x7FFF'FFFFu;

struct UserCtx {
    std::filesystem::path                     db_path;
    std::unique_ptr<blockchain::LmdbStorage>  storage;
    std::unique_ptr<blockchain::Validator>    validator;
    std::unique_ptr<blockchain::Blockchain>   bc;
    std::unique_ptr<blockchain::MergeSession> ms;
    chainsync::ParticipantCache               cache;

    blockchain::KeyPair                                 root_kp;
    blockchain::KeyPair                                 leaf_kp;
    std::map<blockchain::NodeIndex, blockchain::KeyPair> path_keys;

    UserCtx(const std::filesystem::path& base, int id) {
        using namespace blockchain;
        db_path = base / ("user_" + std::to_string(id));
        std::filesystem::remove_all(db_path);
        storage   = std::make_unique<LmdbStorage>(db_path);
        validator = std::make_unique<Validator>(*storage);
        bc        = std::make_unique<Blockchain>(*storage, *validator);
        ms        = std::make_unique<MergeSession>(*storage, *validator);

        root_kp      = Crypto::generate_keypair();
        path_keys[0] = root_kp;
        bc->create_identity(root_kp);
        for (NodeIndex idx : path_indices(LEAF))
            if (path_keys.find(idx) == path_keys.end())
                path_keys[idx] = Crypto::generate_keypair();
        bc->ensure_path(root_kp.pub, LEAF,
                        [&](NodeIndex i) { return path_keys.at(i); });
        leaf_kp = path_keys.at(LEAF);
    }

    ~UserCtx() { ms.reset(); bc.reset(); validator.reset(); storage.reset(); }

    void append_block(uint8_t seed, blockchain::Timestamp ts) {
        bc->append_data_block(root_kp.pub, LEAF, {seed}, leaf_kp, ts);
    }

    chainsync::MergeDialogue dialogue(blockchain::Timestamp ts) {
        return chainsync::MergeDialogue(
            *ms, cache,
            chainsync::MergeConfig{root_kp.pub, LEAF, leaf_kp, ts, 1u});
    }

    // Committed root of the branch's latest MERGE block payload.
    blockchain::Hash committed_root(const blockchain::Block& merge_block) const {
        blockchain::MergePayload mp = blockchain::Serializer::decode_merge_payload(
            merge_block.payload.data(), merge_block.payload.size());
        return mp.merkle_root;
    }

    blockchain::ExternalRef leaf_ref_of(const blockchain::Block& block) const {
        return blockchain::ExternalRef{block.address,
                                       blockchain::Crypto::hash_block(block)};
    }
};

// Delivers all pending messages between the two dialogues until both go quiet.
inline void pump(chainsync::MergeDialogue& initiator,
                 chainsync::MergeDialogue& responder) {
    std::deque<std::vector<uint8_t>> to_responder, to_initiator;
    for (auto& m : initiator.start()) to_responder.push_back(std::move(m));
    while (!to_responder.empty() || !to_initiator.empty()) {
        if (!to_responder.empty()) {
            auto msg = std::move(to_responder.front());
            to_responder.pop_front();
            for (auto& r : responder.on_message(msg.data(), msg.size()))
                to_initiator.push_back(std::move(r));
        } else {
            auto msg = std::move(to_initiator.front());
            to_initiator.pop_front();
            for (auto& r : initiator.on_message(msg.data(), msg.size()))
                to_responder.push_back(std::move(r));
        }
    }
}

} // namespace sync_tests
