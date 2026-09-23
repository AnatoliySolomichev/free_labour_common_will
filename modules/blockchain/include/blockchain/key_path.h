#pragma once

#include "types.h"
#include "storage.h"
#include <vector>

namespace blockchain {

// Self-verifying attribution of a signing key to a chain (blockchain.md §7.5):
// the node path from the root down to the node whose working key signed a seal
// (blockchain.md §7). Built by the signer, carried by the aggregator without
// inspection (sync.md §7.5), checked by the reader on its own — nobody has to
// be trusted for the answer "this key belongs to that chain".
struct KeyPath {
    std::vector<Node> path;   // nodes root → signer, inclusive
};

// Shared link check for node paths (KeyPathCert and RevocationCert):
// the path starts at a self-signed root (index 0, zero parent_hash), every next
// node is a child of the previous one, parent_hash and parent signatures link
// up. Returns the chain identity (root structural key).
// Throws: SignatureError, ChainIntegrityError.
UserId verify_node_path(const std::vector<Node>& path);

class KeyPathCert {
public:
    KeyPathCert() = delete;

    // Assemble the path for a node present in local storage.
    // Throws: NodeNotFoundError.
    static KeyPath build(const IStorage& storage, const UserId& chain, NodeIndex node);

    // Autonomous verification — no storage access. Returns the chain identity.
    // What it cannot prove: that the branch has not rotated its working key
    // since (blockchain.md §6.6) — a rotated branch republishes its path.
    // Throws: SignatureError, ChainIntegrityError.
    static UserId verify(const KeyPath& kp);

    // Working key of the path tip — the key that signs in this branch.
    // Throws: ChainIntegrityError (empty path).
    static const PublicKey& signer_key(const KeyPath& kp);
};

} // namespace blockchain
