#pragma once

#include "types.h"
#include <array>
#include <cstddef>
#include <cstdint>

namespace blockchain {

// HyperLogLog cardinality sketch over participant identities (blockchain.md §6.5).
//
// Estimates the number of UNIQUE participants in a merge snapshot with ~2% error
// using a fixed register array. Two sketches combine by element-wise max, which
// is exactly set union with automatic dedup — the operation the DAG needs when a
// participant appears in several merged branches.
//
// PRECISION is a protocol constant: every peer must use the same value, otherwise
// sketches cannot be merged and hll_hash commitments cannot be compared.
class HllSketch {
public:
    static constexpr uint32_t PRECISION = 11;                   // p
    static constexpr uint32_t REGISTERS = 1u << PRECISION;      // m = 2048 (2 KB sketch)

    HllSketch() = default;   // empty set (all registers zero)

    // Add a participant identity (hashed internally).
    void add(const UserId& id) noexcept;

    // Add a pre-hashed identity (first 8 bytes used as the 64-bit value).
    void add_hash(const Hash& h) noexcept;

    // Merge another sketch into this one (element-wise max = union of sets).
    void merge(const HllSketch& other) noexcept;

    // Estimated number of unique identities added.
    uint64_t estimate() const noexcept;

    // Commitment placed into the MERGE block: BLAKE2b of the register array.
    Hash sketch_hash() const noexcept;

    const std::array<uint8_t, REGISTERS>& registers() const noexcept { return regs_; }

    // Reconstruct a sketch from received register bytes (for cache/gossip transport).
    static HllSketch from_registers(const std::array<uint8_t, REGISTERS>& regs) noexcept;

    bool operator==(const HllSketch& o) const noexcept { return regs_ == o.regs_; }
    bool operator!=(const HllSketch& o) const noexcept { return regs_ != o.regs_; }

private:
    std::array<uint8_t, REGISTERS> regs_{};
};

} // namespace blockchain
