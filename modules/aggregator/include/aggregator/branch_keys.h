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
// ancestor's wins (as in Validator::effective_revocation). The branch's keys
// come in epochs (blockchain.md §6.7 rule 9): its own line, then each replacement a
// revocation named, with that key's own changes. A key of a revoked epoch
// signs only up to the moment of compromise — or, past it, the owner's line
// up to the last valid block the revocation named (rule 12): the owner's late
// uploads come in, the thief's do not. A revocation counts only if its own
// block is its author's (rule 10): a record written with a stolen ancestor key
// stops counting the moment that ancestor is revoked, both here at the door
// and in what the views show. Readers check every block again all the same.

#include "aggregator.h"

#include <blockchain/key_path.h>
#include <blockchain/revocation.h>

#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <utility>
#include <vector>

namespace aggregator {

class BranchKeys {
public:
    enum class Verdict {
        signed_by_branch,   // one of the branch's keys signed it
        unknown_branch,     // no key path of this branch has come yet
        foreign_signature,  // none of the branch's keys signed it
        revoked_key,        // a revoked key signed it past its compromise and the owner's line
    };

    // Everything the storage already holds: key paths, key changes, revocations —
    // of the chains in `only`, when given.
    void load(const AggregatorStorage& storage, const std::set<UserId>* only = nullptr);

    // A key path, checked on the spot (KeyPathCert::verify). Returns the chain.
    // Throws: SignatureError, ChainIntegrityError — a path that does not check.
    UserId learn_path(const KeyPath& kp);

    // A key change of a known branch (blockchain.md §6.6): taken when it is the
    // next one, signed by the key its line signs with now — the branch's own
    // line, or, after a compromise, the replacement's.
    void learn_rotation(const Block& b);

    // A revocation certificate, checked on the spot (RevocationCert::verify): if
    // its author is the highest ancestor heard yet, it decides the branch — from
    // its moment of compromise the revoked epoch signs only the owner's line it
    // named, and its replacement opens the next epoch (none: frozen).
    // Throws: SignatureError, ChainIntegrityError, RevocationError, SerializationError.
    void learn_revocation(const RevocationCertificate& cert);

    // `storage` — where the owner's line is walked (rule 10, for an author under
    // revocation); without it the block's number against the line's decides.
    Verdict check(const Block& b, const AggregatorStorage* storage = nullptr) const;

    // The branch's path with every key change known — what a reader checks the
    // branch's blocks by; nullopt when no path has come.
    std::optional<KeyPath> path(const UserId& chain, NodeIndex node) const;

    // Whose a block is — for the views (blockchain.md §6.7 rules 5, 9, 10, 12): the owner's
    // (a key of an epoch not revoked, or before its compromise, or on the
    // owner's line named for it), not the owner's (a revoked key past both, or
    // a key no counting record gave the branch: one-sided records lose weight),
    // or unknown (nothing is known of this branch). A revocation record counts
    // only if its own block is its author's — the author itself may be under
    // revocation (rule 10), checked up to the root. The owner's line is walked
    // through `storage`; where its blocks are not there, the block's number
    // against the line's decides.
    enum class Whose { owner, not_owner, unknown };
    Whose whose(const Block& b, const AggregatorStorage& storage) const;

private:
    using Key = std::pair<UserId, NodeIndex>;
    struct Branch {
        KeyPath                path;   // nodes root → node; the branch's own key changes in order
        std::vector<PublicKey> own;    // its own line: the node key, then each change
    };
    // One record of the deciding author (blockchain.md §6.7), in its branch order.
    struct Record {
        BlockIndex               block = 0;
        PublicKey                revoked{};
        Timestamp                since = 0;
        std::optional<PublicKey> replacement;
        std::optional<LastGood>  last_good;
        Block                    written;       // the REVOCATION block itself (rule 10)
        PublicKey                author_key{};  // its author's node key, from the certificate's path
    };
    // The revocations that decide a branch: the highest author's records, and
    // the replacement lines they opened — each replacement, then its own changes.
    struct Revocation {
        uint32_t                            author_depth = 0;
        std::vector<Record>                 records;
        std::vector<std::vector<PublicKey>> lines;
    };

    // Whether a block a key of `epoch` signed may come in: the epoch is not
    // revoked, or the block is before the compromise, or on the owner's line.
    static bool admits(const std::vector<Record>& records,
                       const std::vector<PublicKey>& epoch, const Block& b);

    // whose(), under mu_: `own` — the branch's own line when its path has not
    // come (an author known only from a certificate).
    Whose whose_locked(const Block& b, const std::vector<PublicKey>* own,
                       const AggregatorStorage* storage, int depth) const;
    // The records of `rev` that count: their author not under revocation, or
    // the record its author's own (rule 10).
    std::vector<Record> counting(const UserId& chain, const Revocation& rev,
                                 const AggregatorStorage* storage, int depth) const;

    mutable std::mutex         mu_;
    std::map<Key, Branch>      branches_;
    std::map<Key, Revocation>  revocations_;   // may come before the path
};

} // namespace aggregator
