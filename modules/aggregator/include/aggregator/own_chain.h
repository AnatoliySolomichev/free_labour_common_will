#pragma once

#include "aggregator.h"

#include <filesystem>
#include <memory>
#include <vector>

namespace aggregator {

// The aggregator's own personal chain (records.md §11.2): identity is created
// on first run under `dir` (keys in dir/keys, 96-byte files like the CLI's;
// chain in dir/db) and used to publish signed DailyAggregate blocks. The
// aggregator earns no special trust from this — its blocks are ordinary chain
// blocks anyone can fetch and verify.
class OwnChain {
public:
    // Throws StorageError/CryptoError when the identity cannot be created.
    explicit OwnChain(const std::filesystem::path& dir);
    ~OwnChain();

    const UserId& uid() const noexcept;

    // Appends a DATA block to the default branch.
    Block append_data(const std::vector<uint8_t>& payload);

    // All blocks of the default branch, oldest first; empty when none.
    std::vector<Block> branch() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace aggregator
