#include "blockchain/blockchain.h"

namespace blockchain {

Blockchain::Blockchain(IStorage& storage, Validator& validator)
    : storage_(storage), validator_(validator) {}

Node Blockchain::create_identity(const KeyPair&) {
    throw BlockchainError("Blockchain::create_identity: not implemented");
}

std::vector<Node> Blockchain::ensure_path(
    const UserId&, NodeIndex, std::function<KeyPair(NodeIndex)>) {
    throw BlockchainError("Blockchain::ensure_path: not implemented");
}

Node Blockchain::get_node(const UserId& user_id, NodeIndex index) const {
    return storage_.get_node(user_id, index);
}

std::vector<Node> Blockchain::get_path(const UserId&, NodeIndex) const {
    throw BlockchainError("Blockchain::get_path: not implemented");
}

Hash Blockchain::branch_tip_hash(const UserId&, NodeIndex) const {
    throw BlockchainError("Blockchain::branch_tip_hash: not implemented");
}

Block Blockchain::append_data_block(
    const UserId&, NodeIndex, std::vector<uint8_t>, const KeyPair&, Timestamp) {
    throw BlockchainError("Blockchain::append_data_block: not implemented");
}

Block Blockchain::rotate_key(
    const UserId&, NodeIndex, const KeyPair&, const KeyPair&, Timestamp) {
    throw BlockchainError("Blockchain::rotate_key: not implemented");
}

Block Blockchain::revoke_node(
    const UserId&, NodeIndex, Timestamp, const PublicKey&, const KeyPair&, Timestamp) {
    throw BlockchainError("Blockchain::revoke_node: not implemented");
}

Block Blockchain::get_block(const BlockAddress& address) const {
    return storage_.get_block(address);
}

std::vector<Block> Blockchain::get_branch(const UserId&, NodeIndex) const {
    throw BlockchainError("Blockchain::get_branch: not implemented");
}

void Blockchain::validate_path(const UserId& user_id, NodeIndex leaf_index) const {
    validator_.validate_path(user_id, leaf_index);
}

void Blockchain::validate_branch(const UserId& user_id, NodeIndex leaf_index) const {
    validator_.validate_branch(user_id, leaf_index);
}

} // namespace blockchain
