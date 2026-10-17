#pragma once

// Whose blocks the aggregator takes (sync.md §10.10, решение участника
// 2026-10-17): a block must be signed by its branch — by the working key of its
// node, carried through the branch's key changes (blockchain.md §6.6), or by the
// replacement a revocation named (blockchain.md §6.7). Which keys those are the
// aggregator learns from the branch's key path (blockchain.md §7.5), checked
// signature by signature as it arrives. A filter, not a testimony: it keeps
// what nobody of that branch signed off the table, and every reader checks each
// block again on its own.
//
// Where claims conflict, the key nearer the root decides (blockchain.md §4.4,
// решение участника 2026-10-17), never the count of key changes: a path only
// replaces a known one by carrying it further; two diverging lines of the
// branch's own key changes are the same rank, and only a revocation by an
// ancestor settles them. Of several revocations of one branch the highest
// ancestor's wins (as in Validator::effective_revocation); after the moment
// of compromise only the replacement it named, and that key's own changes,
// sign.

#include "aggregator.h"

#include <blockchain/key_path.h>
#include <blockchain/revocation.h>

#include <map>
#include <mutex>
#include <optional>
#include <utility>
#include <vector>

namespace aggregator {

class BranchKeys {
public:
    enum class Verdict {
        signed_by_branch,   // one of the branch's keys signed it
        unknown_branch,     // no key path of this branch has come yet
        foreign_signature,  // none of the branch's keys signed it
    };

    // Everything the storage already holds: key paths, key changes, revocations.
    void load(const AggregatorStorage& storage);

    // A key path, checked on the spot (KeyPathCert::verify). Returns the chain.
    // Throws: SignatureError, ChainIntegrityError — a path that does not check.
    UserId learn_path(const KeyPath& kp);

    // A key change of a known branch (blockchain.md §6.6): taken when it is the
    // next one, signed by the key its line signs with now — the branch's own
    // line, or, after a compromise, the replacement's.
    void learn_rotation(const Block& b);

    // A revocation certificate, checked on the spot (RevocationCert::verify): if
    // its author is the highest ancestor heard yet, it decides the branch — from
    // its moment of compromise only its replacement signs (none: frozen).
    // Throws: SignatureError, ChainIntegrityError, RevocationError, SerializationError.
    void learn_revocation(const RevocationCertificate& cert);

    Verdict check(const Block& b) const;

    // Whether one of the branch's keys signed `message` (a block hash, as a merge
    // partner co-signs it, blockchain.md §6.4).
    bool vouches(const UserId& chain, NodeIndex node, const Hash& message,
                 const Signature& sig) const;

    // The branch's path with every key change known — what a reader checks the
    // branch's blocks by; nullopt when no path has come.
    std::optional<KeyPath> path(const UserId& chain, NodeIndex node) const;

private:
    using Key = std::pair<UserId, NodeIndex>;
    struct Branch {
        KeyPath                path;   // nodes root → node; the branch's own key changes in order
        std::vector<PublicKey> own;    // its own line: the node key, then each change
    };
    // The revocation that decides a branch (blockchain.md §6.7): the highest
    // author's; within one author the earliest compromise, and the latest block
    // names the replacement.
    struct Revocation {
        uint32_t                 author_depth = 0;
        BlockIndex               block        = 0;
        Timestamp                since        = 0;
        std::optional<PublicKey> replacement;
        std::vector<PublicKey>   line;   // the replacement, then its own changes
    };

    std::vector<PublicKey> keys_for(const Key& k, Timestamp at) const;   // under mu_

    mutable std::mutex         mu_;
    std::map<Key, Branch>      branches_;
    std::map<Key, Revocation>  revocations_;   // may come before the path
};

} // namespace aggregator
