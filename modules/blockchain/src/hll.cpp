#include "blockchain/hll.h"
#include "blockchain/crypto.h"

#include <algorithm>
#include <cmath>

namespace blockchain {
namespace {

// First 8 bytes of a hash as a big-endian 64-bit value.
uint64_t hash_to_u64(const Hash& h) noexcept {
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i)
        v = (v << 8) | h.bytes[static_cast<size_t>(i)];
    return v;
}

} // namespace (anonymous)

void HllSketch::add_hash(const Hash& h) noexcept {
    const uint64_t x = hash_to_u64(h);

    // Top PRECISION bits select the register.
    const uint64_t idx = x >> (64 - HllSketch::PRECISION);

    // Rank = position of the leftmost 1-bit in the remaining (64 - p) bits + 1.
    const uint64_t rest = x << HllSketch::PRECISION;
    const uint8_t rank = (rest == 0)
        ? static_cast<uint8_t>(64 - HllSketch::PRECISION + 1)
        : static_cast<uint8_t>(__builtin_clzll(rest) + 1);

    uint8_t& reg = regs_[static_cast<size_t>(idx)];
    if (rank > reg) reg = rank;
}

void HllSketch::add(const UserId& id) noexcept {
    add_hash(Crypto::hash(id.bytes.data(), id.bytes.size()));
}

void HllSketch::merge(const HllSketch& other) noexcept {
    for (size_t i = 0; i < REGISTERS; ++i)
        regs_[i] = std::max(regs_[i], other.regs_[i]);
}

uint64_t HllSketch::estimate() const noexcept {
    const double m = static_cast<double>(REGISTERS);

    double   sum   = 0.0;
    uint32_t zeros = 0;
    for (uint8_t r : regs_) {
        sum += std::ldexp(1.0, -static_cast<int>(r));   // 2^{-r}
        if (r == 0) ++zeros;
    }

    const double alpha = 0.7213 / (1.0 + 1.079 / m);
    double e = alpha * m * m / sum;

    // Small-range correction: linear counting when many registers are still zero.
    if (e <= 2.5 * m && zeros != 0)
        e = m * std::log(m / static_cast<double>(zeros));
    // Large-range correction is unnecessary with a 64-bit hash.

    return static_cast<uint64_t>(e + 0.5);
}

Hash HllSketch::sketch_hash() const noexcept {
    return Crypto::hash(regs_.data(), regs_.size());
}

HllSketch HllSketch::from_registers(
    const std::array<uint8_t, REGISTERS>& regs) noexcept {
    HllSketch s;
    s.regs_ = regs;
    return s;
}

} // namespace blockchain
