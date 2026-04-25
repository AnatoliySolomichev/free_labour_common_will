#pragma once

#include "storage.h"
#include "validator.h"

namespace blockchain {

// Two-round bilateral merge protocol (§6.4).
//
// Typical call order:
//
//   Alice                               Bob
//   prepare_tip()  ──────────────────►  verify_partner_tip()
//   verify_partner_tip()  ◄────────────  prepare_tip()
//   create_pending() ─── draft_hash ───► co_sign()
//   co_sign()       ◄── draft_hash ────  create_pending()
//   finalize()      ◄──── co_sig ──────  finalize()
//                   ─────── co_sig ────►
class MergeSession {
public:
    MergeSession(IStorage& storage, Validator& validator);

    // Step 1a: build BranchTipInfo for own branch to send to partner.
    // Throws: NodeNotFoundError, BlockNotFoundError.
    BranchTipInfo prepare_tip(const UserId& user_id, NodeIndex leaf_index) const;

    // Step 1b: verify partner's BranchTipInfo (path signatures + chain).
    // Throws: SignatureError, ChainIntegrityError, NodeNotFoundError.
    void verify_partner_tip(const BranchTipInfo& partner_tip) const;

    // Step 2: create a MERGE block draft (own signature; co_signature absent).
    // The draft is saved to storage immediately (without co_signature).
    // Call finalize() once the partner's co-signature is received.
    // Throws: CryptoError, StorageError, NodeNotFoundError.
    PendingMergeBlock create_pending(
        const UserId&        user_id,
        NodeIndex            leaf_index,
        const BranchTipInfo& partner_tip,
        const KeyPair&       own_working_keypair,
        Timestamp            merge_timestamp
    );

    // Step 3: co-sign the partner's draft hash with own key.
    // Returns the Signature to send back to the partner.
    // Throws: CryptoError.
    Signature co_sign(const Hash& partner_draft_hash, const KeyPair& own_working_keypair);

    // Step 4: attach partner's co_signature to own pending block and finalize.
    // Throws: SignatureError (invalid co_sig), StorageError.
    Block finalize(const PendingMergeBlock& pending,
                   const Signature&         partner_co_sig,
                   const PublicKey&         partner_pubkey);

private:
    IStorage&  storage_;
    Validator& validator_;
};

} // namespace blockchain
