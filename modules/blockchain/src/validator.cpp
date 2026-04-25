#include "blockchain/validator.h"
#include "blockchain/errors.h"

namespace blockchain {

Validator::Validator(const IStorage& storage) : storage_(storage) {}

void Validator::validate_node(const Node&, const UserId&) const {
    throw BlockchainError("Validator::validate_node: not implemented");
}

void Validator::validate_path(const UserId&, NodeIndex) const {
    throw BlockchainError("Validator::validate_path: not implemented");
}

void Validator::validate_block(const Block&, const Hash&, const PublicKey&) const {
    throw BlockchainError("Validator::validate_block: not implemented");
}

void Validator::validate_branch(const UserId&, NodeIndex) const {
    throw BlockchainError("Validator::validate_branch: not implemented");
}

void Validator::validate_seal(const Seal&) const {
    throw BlockchainError("Validator::validate_seal: not implemented");
}

void Validator::validate_co_signature(const Block&, const PublicKey&) const {
    throw BlockchainError("Validator::validate_co_signature: not implemented");
}

} // namespace blockchain
