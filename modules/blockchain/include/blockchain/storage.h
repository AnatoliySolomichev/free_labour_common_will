#pragma once

#include "types.h"
#include <filesystem>
#include <memory>
#include <optional>
#include <vector>

namespace blockchain {

class IStorage {
public:
    virtual ~IStorage() = default;

    // ── Nodes ─────────────────────────────────────────────────────────────────

    // Store a node. Overwrites are forbidden.
    // Throws: StorageError, InvalidArgumentError (duplicate).
    virtual void put_node(const UserId& user_id, const Node& node) = 0;

    // Throws: NodeNotFoundError.
    virtual Node get_node(const UserId& user_id, NodeIndex index) const = 0;

    virtual bool has_node(const UserId& user_id, NodeIndex index) const noexcept = 0;

    // ── Blocks ────────────────────────────────────────────────────────────────

    // Store a block. Overwrites are forbidden.
    // Throws: StorageError, InvalidArgumentError (duplicate).
    virtual void put_block(const Block& block) = 0;

    // Throws: BlockNotFoundError.
    virtual Block get_block(const BlockAddress& address) const = 0;

    virtual bool has_block(const BlockAddress& address) const noexcept = 0;

    // Index of the last block in a branch, or nullopt if the branch is empty.
    virtual std::optional<BlockIndex> branch_tip_index(
        const UserId& user_id, NodeIndex leaf_index) const noexcept = 0;

    // ── Seals ─────────────────────────────────────────────────────────────────

    // Multiple seals per block are allowed. Throws: StorageError.
    virtual void put_seal(const Seal& seal) = 0;

    virtual std::vector<Seal> get_seals(const Hash& block_hash) const = 0;

    // ── External blocks (other users, received during sync) ───────────────────

    virtual void  put_external_block(const Block& block) = 0;
    virtual Block get_external_block(const BlockAddress& address) const = 0;
    virtual bool  has_external_block(const BlockAddress& address) const noexcept = 0;

    // ── Transactions ──────────────────────────────────────────────────────────

    // RAII transaction: commits on commit(), rolls back on destruction without commit().
    class Transaction {
    public:
        virtual ~Transaction() = default;
        virtual void commit() = 0;
    };

    // Only one write transaction may be active at a time (LMDB constraint).
    // Throws: StorageError.
    virtual std::unique_ptr<Transaction> begin_write() = 0;
};

// ── LMDB implementation ───────────────────────────────────────────────────────

class LmdbStorage : public IStorage {
public:
    // Open or create the store at db_path. Throws: StorageError.
    explicit LmdbStorage(std::filesystem::path db_path);
    ~LmdbStorage() override;

    LmdbStorage(const LmdbStorage&) = delete;
    LmdbStorage& operator=(const LmdbStorage&) = delete;
    LmdbStorage(LmdbStorage&&) noexcept;
    LmdbStorage& operator=(LmdbStorage&&) noexcept;

    void put_node(const UserId& user_id, const Node& node) override;
    Node get_node(const UserId& user_id, NodeIndex index) const override;
    bool has_node(const UserId& user_id, NodeIndex index) const noexcept override;

    void  put_block(const Block& block) override;
    Block get_block(const BlockAddress& address) const override;
    bool  has_block(const BlockAddress& address) const noexcept override;
    std::optional<BlockIndex> branch_tip_index(
        const UserId& user_id, NodeIndex leaf_index) const noexcept override;

    void              put_seal(const Seal& seal) override;
    std::vector<Seal> get_seals(const Hash& block_hash) const override;

    void  put_external_block(const Block& block) override;
    Block get_external_block(const BlockAddress& address) const override;
    bool  has_external_block(const BlockAddress& address) const noexcept override;

    std::unique_ptr<Transaction> begin_write() override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace blockchain
