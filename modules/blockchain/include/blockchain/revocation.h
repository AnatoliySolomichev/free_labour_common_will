#pragma once

#include "types.h"
#include "storage.h"
#include <vector>

namespace blockchain {

// Self-verifying revocation certificate (blockchain.md §6.7 rule 8): the REVOCATION block
// plus the key path from the root to its author, with the author's key changes
// (blockchain.md §7.5). Carries everything a third party needs to check the
// revocation without access to the owner's chain; distributed via the
// aggregator warehouse (sync.md §7.2, sync.md §10.3) and gossip.
struct RevocationCertificate {
    Block   block;  // the REVOCATION block in the author's branch
    KeyPath path;   // root → author, with every key change of the path's nodes
};

class RevocationCert {
public:
    RevocationCert() = delete;

    // Assemble a certificate for a REVOCATION block present in local storage.
    // Throws: BlockNotFoundError, NodeNotFoundError,
    //         InvalidArgumentError (address is not a REVOCATION block).
    static RevocationCertificate build(const IStorage& storage,
                                       const BlockAddress& revocation_block);

    // Autonomous verification — no storage access (blockchain.md §6.7 rule 8):
    //  - path: KeyPathCert::verify;
    //  - identity: root structural key == block.address.user_id;
    //  - author: the path ends at the block's node, and the block is signed by
    //    the author's key in force at its place — a rotated or replaced author
    //    signs with its new key (blockchain.md §11.2, решение 2026-11-02);
    //  - payload: decodes, revoked node is valid, not the root, and a strict
    //    descendant of the author (self-revocation impossible).
    // What it cannot check without the revoked branch: that revoked_pubkey is
    // really that branch's key — asserted by the author, who outranks the
    // branch by the priority gradient (blockchain.md §4.4).
    // Throws: SignatureError, ChainIntegrityError, RevocationError,
    //         SerializationError.
    static void verify(const RevocationCertificate& cert);

    // Decoded payload of the certificate's block. Throws: SerializationError.
    static RevocationPayload payload(const RevocationCertificate& cert);
};

} // namespace blockchain
