#pragma once

#include "types.h"
#include "storage.h"
#include <vector>

namespace blockchain {

// Self-verifying revocation certificate (§6.7 rule 8): the REVOCATION block
// plus the node path from root to its author. Carries everything a third party
// needs to check the revocation without access to the owner's chain;
// distributed via the aggregator warehouse (sync.md §7.2, §10.3) and gossip.
struct RevocationCertificate {
    Block             block;  // the REVOCATION block in the author's branch
    std::vector<Node> path;   // nodes root → author, inclusive
};

class RevocationCert {
public:
    RevocationCert() = delete;

    // Assemble a certificate for a REVOCATION block present in local storage.
    // Throws: BlockNotFoundError, NodeNotFoundError,
    //         InvalidArgumentError (address is not a REVOCATION block).
    static RevocationCertificate build(const IStorage& storage,
                                       const BlockAddress& revocation_block);

    // Autonomous verification — no storage access (§6.7 rule 8):
    //  - path: starts at a self-signed root (index 0, zero parent_hash), every
    //    next node is a child of the previous one, parent_hash and parent
    //    signatures link up;
    //  - identity: root structural key == block.address.user_id;
    //  - author: the path tip is the block's node, and the block is signed by
    //    the tip's working key (a rotated author branch needs a certificate
    //    re-issued from a fresh block — MVP limitation);
    //  - payload: decodes, revoked node is valid, not the root, and a strict
    //    descendant of the author (self-revocation impossible).
    // What it cannot check without the revoked branch: that revoked_pubkey is
    // really that branch's key — asserted by the author, who outranks the
    // branch by the priority gradient (§4.4).
    // Throws: SignatureError, ChainIntegrityError, RevocationError,
    //         SerializationError.
    static void verify(const RevocationCertificate& cert);

    // Decoded payload of the certificate's block. Throws: SerializationError.
    static RevocationPayload payload(const RevocationCertificate& cert);
};

} // namespace blockchain
