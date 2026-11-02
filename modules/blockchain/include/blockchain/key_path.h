#pragma once

#include "types.h"
#include "storage.h"
#include <limits>
#include <vector>

namespace blockchain {

// Where a branch's blocks start (blockchain.md §5.3): block #0 names this hash
// in prev_hash — the node's BIRTH block, or the root's own record for the root.
// A revocation line that holds no block names it too (blockchain.md §6.7 rule 12).
Hash branch_origin(const Node& node);

// The node a BIRTH block makes (blockchain.md §11.2): index and keys from the
// payload, and where the block stands. Unchecked: KeyPathCert::verify and
// Validator::validate_node check births.
// Throws: InvalidArgumentError (not a BIRTH block), SerializationError.
Node node_born(const Block& birth);

// The BIRTH blocks of the path to `node`, root's child first — own or imported.
// Throws: NodeNotFoundError, BlockNotFoundError.
std::vector<Block> births_of(const IStorage& storage, const UserId& chain, NodeIndex node);

// The key changes of a branch in this storage, in branch order: KEY_ROTATION
// and KEY_REPLACEMENT blocks (blockchain.md §6.6, blockchain.md §6.7 rule 9).
std::vector<Block> key_changes_of(const IStorage& storage, const UserId& chain, NodeIndex node);

// Self-verifying attribution of a signing key to a chain (blockchain.md §7.5):
// the key path (types.h). Built by the signer, carried by the aggregator,
// checked by the reader on its own — nobody has to be trusted for the answer
// "this key belongs to that chain, at that place of that branch".
class KeyPathCert {
public:
    KeyPathCert() = delete;

    static constexpr BlockIndex kNow = std::numeric_limits<BlockIndex>::max();

    // Assemble the path to a node present in local storage: the root's record,
    // the births, every key change of the path's nodes this storage holds and
    // the records the replacements name.
    // Throws: NodeNotFoundError, BlockNotFoundError.
    static KeyPath build(const IStorage& storage, const UserId& chain, NodeIndex node);

    // Autonomous verification — no storage access. Returns the chain identity:
    //  - the root's record is signed by its own structural key;
    //  - every birth stands in the branch of the node above it, names a child of
    //    that node and is signed by that node's key in force at its place;
    //  - every key change belongs to a node of the path, in branch order: a
    //    KEY_ROTATION is signed by the key before it; a KEY_REPLACEMENT is signed
    //    by the replacement a carried record assigned to this node, and stands
    //    right after the line that record names, if it names one; the record is
    //    signed by its author — an ancestor on the path — with the key in force
    //    at its place.
    // What it cannot prove: that a node changed no key after the last change
    // carried — such a branch publishes its path again (blockchain.md §7.5).
    // Throws: SignatureError, ChainIntegrityError, SerializationError.
    static UserId verify(const KeyPath& kp);

    // The path's nodes, root → the branch's node, as the births make them.
    // Unchecked — after verify. Throws: SerializationError.
    static std::vector<Node> nodes(const KeyPath& kp);

    // The branch's node: the last birth's child, or the root.
    // Throws: SerializationError.
    static NodeIndex node(const KeyPath& kp);

    // The key `node` signs with at `place` of its branch: the key its birth
    // names, carried through its key changes before that place — each one
    // checked on the way. `node` must be on the path; after verify.
    // Throws: SignatureError, ChainIntegrityError, SerializationError.
    static PublicKey key_at(const KeyPath& kp, NodeIndex node, BlockIndex place);

    // The key the branch signs with now: key_at(the branch's node, kNow).
    // Throws: SignatureError, ChainIntegrityError, SerializationError.
    static PublicKey signer_key(const KeyPath& kp);
};

} // namespace blockchain
