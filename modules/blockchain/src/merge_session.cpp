#include "blockchain/merge_session.h"
#include "blockchain/errors.h"

namespace blockchain {

MergeSession::MergeSession(IStorage& storage, Validator& validator)
    : storage_(storage), validator_(validator) {}

BranchTipInfo MergeSession::prepare_tip(const UserId&, NodeIndex) const {
    throw BlockchainError("MergeSession::prepare_tip: not implemented");
}

void MergeSession::verify_partner_tip(const BranchTipInfo&) const {
    throw BlockchainError("MergeSession::verify_partner_tip: not implemented");
}

PendingMergeBlock MergeSession::create_pending(
    const UserId&, NodeIndex, const BranchTipInfo&, const KeyPair&, Timestamp) {
    throw BlockchainError("MergeSession::create_pending: not implemented");
}

Signature MergeSession::co_sign(const Hash&, const KeyPair&) {
    throw BlockchainError("MergeSession::co_sign: not implemented");
}

Block MergeSession::finalize(
    const PendingMergeBlock&, const Signature&, const PublicKey&) {
    throw BlockchainError("MergeSession::finalize: not implemented");
}

} // namespace blockchain
