#pragma once

// Whose blocks the aggregator takes (sync.md §10.10, решение участника
// 2026-10-17): a block must be signed by its branch — by the working key of its
// node, carried through the branch's key changes (blockchain.md §6.6), or by the
// replacement a revocation named (blockchain.md §6.7). Which keys those are the
// aggregator learns from the branch's key path (blockchain.md §7.5), checked
// signature by signature as it arrives. A filter, not a testimony: it keeps
// what nobody of that branch signed off the table, and every reader checks each
// block again on its own.

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
    // next one, signed by the key the branch signs with now.
    void learn_rotation(const Block& b);

    // A revocation certificate, checked on the spot (RevocationCert::verify):
    // the replacement key it names joins the revoked branch.
    // Throws: SignatureError, ChainIntegrityError, RevocationError, SerializationError.
    void learn_revocation(const RevocationCertificate& cert);

    Verdict check(const Block& b) const;

    // The branch's path with every key change known — what a reader checks the
    // branch's blocks by; nullopt when no path has come.
    std::optional<KeyPath> path(const UserId& chain, NodeIndex node) const;

private:
    struct Branch {
        KeyPath                path;   // nodes root → node; key changes in order
        std::vector<PublicKey> keys;   // every key the branch signs with
    };
    using Key = std::pair<UserId, NodeIndex>;

    void rekey(const Key& k, Branch& b);

    mutable std::mutex                    mu_;
    std::map<Key, Branch>                 branches_;
    std::map<Key, std::vector<PublicKey>> replacements_;   // may come before the path
};

} // namespace aggregator
