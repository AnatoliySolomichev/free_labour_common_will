#pragma once

// Minimal deterministic-CBOR (RFC 8949 §4.2.1) helpers shared by the wire
// codecs of this module (merge dialogue messages, relay envelopes).
// Decoding is strict: minimal-length heads only, exact sizes, no trailing
// bytes — anything else throws SerializationError.

#include <blockchain/errors.h>

#include <cstddef>
#include <cstdint>
#include <vector>

namespace chainsync::wire {

inline void put_head(std::vector<uint8_t>& out, uint8_t major, uint64_t v) {
    const uint8_t m = static_cast<uint8_t>(major << 5);
    if (v < 24) {
        out.push_back(static_cast<uint8_t>(m | v));
        return;
    }
    int extra = v <= 0xFF ? 1 : v <= 0xFFFF ? 2 : v <= 0xFFFF'FFFFull ? 4 : 8;
    out.push_back(static_cast<uint8_t>(m | (extra == 1 ? 24 : extra == 2 ? 25
                                          : extra == 4 ? 26 : 27)));
    for (int i = extra - 1; i >= 0; --i)
        out.push_back(static_cast<uint8_t>(v >> (8 * i)));
}

inline void put_bstr(std::vector<uint8_t>& out, const std::vector<uint8_t>& bytes) {
    put_head(out, 2, bytes.size());
    out.insert(out.end(), bytes.begin(), bytes.end());
}

struct CborReader {
    const uint8_t* data;
    size_t         len;
    size_t         pos = 0;

    uint8_t byte() {
        if (pos >= len) throw blockchain::SerializationError("wire: truncated");
        return data[pos++];
    }

    uint64_t head(uint8_t expected_major) {
        const uint8_t b = byte();
        if ((b >> 5) != expected_major)
            throw blockchain::SerializationError("wire: unexpected CBOR type");
        const uint8_t ai = b & 0x1F;
        if (ai < 24) return ai;
        if (ai > 27) throw blockchain::SerializationError("wire: bad CBOR head");
        const int extra = ai == 24 ? 1 : ai == 25 ? 2 : ai == 26 ? 4 : 8;
        uint64_t v = 0;
        for (int i = 0; i < extra; ++i) v = (v << 8) | byte();
        // Deterministic encoding: reject non-minimal heads.
        static constexpr uint64_t min_for[] = {24, 0x100, 0x1'0000, 0x1'0000'0000ull};
        if (v < min_for[ai - 24])
            throw blockchain::SerializationError("wire: non-minimal CBOR head");
        return v;
    }

    std::vector<uint8_t> bstr() {
        const uint64_t n = head(2);
        if (n > len - pos) throw blockchain::SerializationError("wire: truncated bstr");
        std::vector<uint8_t> out(data + pos, data + pos + n);
        pos += n;
        return out;
    }

    void expect_end() const {
        if (pos != len) throw blockchain::SerializationError("wire: trailing bytes");
    }
};

} // namespace chainsync::wire
