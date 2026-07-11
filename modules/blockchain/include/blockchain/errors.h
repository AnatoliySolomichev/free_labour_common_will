#pragma once

#include "types.h"
#include <stdexcept>
#include <string>

namespace blockchain {

class BlockchainError : public std::runtime_error {
public:
    explicit BlockchainError(const std::string& msg) : std::runtime_error(msg) {}
};

class CryptoError : public BlockchainError {
public:
    using BlockchainError::BlockchainError;
};

class InvariantError : public BlockchainError {
    int inv_;
public:
    InvariantError(const std::string& msg, int invariant)
        : BlockchainError(msg), inv_(invariant) {}
    int invariant_number() const noexcept { return inv_; }
};

class SignatureError : public InvariantError {
public:
    explicit SignatureError(const std::string& msg) : InvariantError(msg, 3) {}
};

class ChainIntegrityError : public InvariantError {
public:
    explicit ChainIntegrityError(const std::string& msg) : InvariantError(msg, 2) {}
};

class TimestampError : public InvariantError {
public:
    explicit TimestampError(const std::string& msg) : InvariantError(msg, 4) {}
};

class NodeNotFoundError : public BlockchainError {
    NodeIndex idx_;
public:
    explicit NodeNotFoundError(NodeIndex idx)
        : BlockchainError("node not found: " + std::to_string(idx)), idx_(idx) {}
    NodeIndex missing_index() const noexcept { return idx_; }
};

class BlockNotFoundError : public BlockchainError {
    BlockAddress addr_;
public:
    explicit BlockNotFoundError(const BlockAddress& addr)
        : BlockchainError("block not found"), addr_(addr) {}
    const BlockAddress& missing_address() const noexcept { return addr_; }
};

class StorageError : public BlockchainError {
public:
    using BlockchainError::BlockchainError;
};

class SerializationError : public BlockchainError {
public:
    using BlockchainError::BlockchainError;
};

class InvalidArgumentError : public BlockchainError {
public:
    using BlockchainError::BlockchainError;
};

// Semantic violation of the key-revocation rules (§6.7): non-ancestor author,
// self-revocation, root revocation, revoked_pubkey mismatch.
class RevocationError : public BlockchainError {
public:
    using BlockchainError::BlockchainError;
};

} // namespace blockchain
