#include "blockchain/serializer.h"
#include "blockchain/errors.h"
#include "blockchain/hll.h"
#include <array>
#include <cstring>
#include <limits>

// CBOR encoding is implemented manually to guarantee deterministic (shortest-form)
// output per RFC 8949 §4.2.1. External CBOR libraries (e.g. libcbor) fix the integer
// width at allocation time and cannot produce the inline 1-byte form for values 0–23.
//
// Map key ordering: all maps use integer keys 0, 1, 2, ... in ascending order.
// No indefinite-length items are used.

namespace blockchain {
namespace {

using Buf = std::vector<uint8_t>;

// ── CBOR writer ───────────────────────────────────────────────────────────────

// Emit the CBOR initial byte + optional additional bytes for a given major type
// and numeric value, using the shortest possible encoding.
//   major  0 = uint      1 = negint    2 = bytes
//          3 = text      4 = array     5 = map
void write_head(Buf& out, uint8_t major, uint64_t val) {
    const uint8_t base = static_cast<uint8_t>(major << 5);
    if (val <= 23) {
        out.push_back(base | static_cast<uint8_t>(val));
    } else if (val <= 0xFFu) {
        out.push_back(base | 24u);
        out.push_back(static_cast<uint8_t>(val));
    } else if (val <= 0xFFFFu) {
        out.push_back(base | 25u);
        out.push_back(static_cast<uint8_t>(val >> 8));
        out.push_back(static_cast<uint8_t>(val));
    } else if (val <= 0xFFFF'FFFFu) {
        out.push_back(base | 26u);
        out.push_back(static_cast<uint8_t>(val >> 24));
        out.push_back(static_cast<uint8_t>(val >> 16));
        out.push_back(static_cast<uint8_t>(val >> 8));
        out.push_back(static_cast<uint8_t>(val));
    } else {
        out.push_back(base | 27u);
        for (int i = 7; i >= 0; --i)
            out.push_back(static_cast<uint8_t>(val >> (8 * i)));
    }
}

void w_uint (Buf& out, uint64_t v)   { write_head(out, 0, v); }
void w_map  (Buf& out, uint64_t n)   { write_head(out, 5, n); }
void w_arr  (Buf& out, uint64_t n)   { write_head(out, 4, n); }

void w_int64(Buf& out, int64_t v) {
    if (v >= 0) write_head(out, 0, static_cast<uint64_t>(v));
    else        write_head(out, 1, static_cast<uint64_t>(-1 - v));
}

void w_bytes(Buf& out, const uint8_t* data, size_t len) {
    write_head(out, 2, len);
    out.insert(out.end(), data, data + len);
}

template<size_t N>
void w_fixed(Buf& out, const std::array<uint8_t, N>& a) {
    w_bytes(out, a.data(), N);
}

// ── Sub-type encoders ─────────────────────────────────────────────────────────

void enc_address(Buf& out, const BlockAddress& a) {
    w_map(out, 3);
    w_uint(out, 0); w_fixed(out, a.user_id.bytes);
    w_uint(out, 1); w_uint(out, a.node_index);
    w_uint(out, 2); w_uint(out, a.block_index);
}

void enc_ext_ref(Buf& out, const ExternalRef& r) {
    w_map(out, 2);
    w_uint(out, 0); enc_address(out, r.address);
    w_uint(out, 1); w_fixed(out, r.block_hash.bytes);
}

void enc_node(Buf& out, const Node& n) {
    w_map(out, 6);
    w_uint(out, 0); w_uint(out,  n.index);
    w_uint(out, 1); w_fixed(out, n.structural_pubkey.bytes);
    w_uint(out, 2); w_fixed(out, n.working_pubkey.bytes);
    w_uint(out, 3); w_fixed(out, n.parent_hash.bytes);
    w_uint(out, 4); w_fixed(out, n.parent_sig.bytes);
    w_uint(out, 5); w_int64(out, n.created_at);
}

void enc_block(Buf& out, const Block& b) {
    // co_signature is excluded from canonical encoding (blockchain-api.md §5.4)
    w_map(out, 7);
    w_uint(out, 0); enc_address(out, b.address);
    w_uint(out, 1); w_fixed(out, b.prev_hash.bytes);
    w_uint(out, 2); w_int64(out, b.timestamp_claimed);
    w_uint(out, 3); w_uint(out, static_cast<uint64_t>(b.type));
    w_uint(out, 4); w_bytes(out, b.payload.data(), b.payload.size());
    w_uint(out, 5);
    w_arr(out, b.external_refs.size());
    for (const auto& ref : b.external_refs) enc_ext_ref(out, ref);
    w_uint(out, 6); w_fixed(out, b.signature.bytes);
}

void enc_seal(Buf& out, const Seal& s) {
    w_map(out, 5);
    w_uint(out, 0); w_fixed(out, s.signer_id.bytes);
    w_uint(out, 1); w_fixed(out, s.block_hash.bytes);
    w_uint(out, 2); w_fixed(out, s.signature.bytes);
    w_uint(out, 3); w_uint(out, static_cast<uint64_t>(s.mode));
    w_uint(out, 4); w_int64(out, s.sealed_at);
}

void enc_merge_payload(Buf& out, const MergePayload& mp) {
    w_map(out, 6);
    w_uint(out, 0); enc_address(out, mp.partner_last_address);
    w_uint(out, 1); w_fixed(out, mp.partner_last_hash.bytes);
    w_uint(out, 2); w_int64(out, mp.merge_timestamp);
    w_uint(out, 3); w_fixed(out, mp.merkle_root.bytes);
    w_uint(out, 4); w_fixed(out, mp.hll_hash.bytes);
    w_uint(out, 5); w_uint(out, mp.validated_depth);
}

void enc_revocation_payload(Buf& out, const RevocationPayload& rp) {
    // Map of 3 fields (emergency stop) or 4 (with replacement) — §6.7.
    w_map(out, rp.replacement_pubkey.has_value() ? 4 : 3);
    w_uint(out, 0); w_uint(out, rp.revoked_node_index);
    w_uint(out, 1); w_fixed(out, rp.revoked_pubkey.bytes);
    w_uint(out, 2); w_int64(out, rp.compromised_since);
    if (rp.replacement_pubkey.has_value()) {
        w_uint(out, 3); w_fixed(out, rp.replacement_pubkey->bytes);
    }
}

void enc_merkle_proof(Buf& out, const MerkleTree::Proof& p) {
    w_map(out, 2);
    w_uint(out, 0);
    w_arr(out, p.path.size());
    for (const auto& h : p.path) w_fixed(out, h.bytes);
    w_uint(out, 1);
    w_arr(out, p.sibling_is_right.size());
    for (bool b : p.sibling_is_right) w_uint(out, b ? 1u : 0u);
}

void enc_fraud_proof(Buf& out, const FraudProofData& d) {
    w_map(out, 4);
    w_uint(out, 0); enc_ext_ref(out, d.leaf);
    w_uint(out, 1); enc_merkle_proof(out, d.merkle_path);
    w_uint(out, 2);
    w_arr(out, d.node_path.size());
    for (const auto& n : d.node_path) enc_node(out, n);
    w_uint(out, 3); enc_block(out, d.evidence);
}

void enc_snapshot(Buf& out, const MergeSnapshot& s) {
    w_map(out, 2);
    w_uint(out, 0); w_fixed(out, s.merkle_root.bytes);
    w_uint(out, 1); w_bytes(out, s.hll.registers().data(), HllSketch::REGISTERS);
}

void enc_tip(Buf& out, const BranchTipInfo& t) {
    w_map(out, 4);
    w_uint(out, 0); enc_address(out, t.tip_address);
    w_uint(out, 1); w_fixed(out, t.tip_hash.bytes);
    w_uint(out, 2);
    w_arr(out, t.path.size());
    for (const auto& n : t.path) enc_node(out, n);
    w_uint(out, 3);
    if (t.tip_block.has_value()) {
        Buf block_bytes;
        enc_block(block_bytes, *t.tip_block);
        w_bytes(out, block_bytes.data(), block_bytes.size());
    } else {
        w_bytes(out, nullptr, 0);
    }
}

// ── CBOR reader ───────────────────────────────────────────────────────────────

class CborReader {
public:
    CborReader(const uint8_t* data, size_t size)
        : data_(data), size_(size), pos_(0) {}

    // Reads the initial byte and any extension bytes.
    // Returns {major_type, numeric_value}.
    std::pair<uint8_t, uint64_t> read_head() {
        need(1);
        const uint8_t initial = data_[pos_++];
        const uint8_t major   = initial >> 5;
        const uint8_t info    = initial & 0x1fu;
        if (info <= 23) return {major, info};
        if (info == 24) { need(1); return {major, data_[pos_++]}; }
        if (info == 25) {
            need(2);
            const uint64_t v = (static_cast<uint64_t>(data_[pos_]) << 8)
                             |  static_cast<uint64_t>(data_[pos_+1]);
            pos_ += 2; return {major, v};
        }
        if (info == 26) {
            need(4);
            const uint64_t v = (static_cast<uint64_t>(data_[pos_  ]) << 24)
                             | (static_cast<uint64_t>(data_[pos_+1]) << 16)
                             | (static_cast<uint64_t>(data_[pos_+2]) <<  8)
                             |  static_cast<uint64_t>(data_[pos_+3]);
            pos_ += 4; return {major, v};
        }
        if (info == 27) {
            need(8);
            uint64_t v = 0;
            for (int i = 0; i < 8; ++i) v = (v << 8) | data_[pos_++];
            return {major, v};
        }
        throw SerializationError("CBOR: unsupported additional info (indefinite/reserved)");
    }

    uint64_t r_uint() {
        const auto [m, v] = read_head();
        if (m != 0) throw SerializationError("CBOR: expected uint");
        return v;
    }

    // Decodes both unsigned (major 0) and negative (major 1) as int64_t.
    int64_t r_int() {
        const auto [m, v] = read_head();
        if (m == 0) {
            if (v > static_cast<uint64_t>(std::numeric_limits<int64_t>::max()))
                throw SerializationError("CBOR: uint value overflows int64_t");
            return static_cast<int64_t>(v);
        }
        if (m == 1) {
            if (v > static_cast<uint64_t>(std::numeric_limits<int64_t>::max()))
                throw SerializationError("CBOR: negative int underflows int64_t");
            return -1LL - static_cast<int64_t>(v);
        }
        throw SerializationError("CBOR: expected integer");
    }

    void r_bytes_exact(uint8_t* out, size_t expected) {
        const auto [m, len] = read_head();
        if (m != 2) throw SerializationError("CBOR: expected byte string");
        if (len != static_cast<uint64_t>(expected))
            throw SerializationError("CBOR: byte string length mismatch");
        need(static_cast<size_t>(len));
        std::memcpy(out, data_ + pos_, len);
        pos_ += static_cast<size_t>(len);
    }

    std::vector<uint8_t> r_bytes() {
        const auto [m, len] = read_head();
        if (m != 2) throw SerializationError("CBOR: expected byte string");
        need(static_cast<size_t>(len));
        std::vector<uint8_t> result(data_ + pos_, data_ + pos_ + len);
        pos_ += static_cast<size_t>(len);
        return result;
    }

    uint64_t r_map() {
        const auto [m, n] = read_head();
        if (m != 5) throw SerializationError("CBOR: expected map");
        return n;
    }

    uint64_t r_arr() {
        const auto [m, n] = read_head();
        if (m != 4) throw SerializationError("CBOR: expected array");
        return n;
    }

    template<size_t N>
    void r_fixed(std::array<uint8_t, N>& arr) { r_bytes_exact(arr.data(), N); }

private:
    const uint8_t* data_;
    size_t         size_;
    size_t         pos_;

    void need(size_t n) const {
        if (pos_ + n > size_)
            throw SerializationError("CBOR: unexpected end of data");
    }
};

// ── Sub-type decoders ─────────────────────────────────────────────────────────

void expect_key(CborReader& r, uint64_t expected) {
    if (r.r_uint() != expected)
        throw SerializationError("CBOR: unexpected map key (non-canonical or wrong schema)");
}

BlockAddress dec_address(CborReader& r) {
    if (r.r_map() != 3) throw SerializationError("BlockAddress: expected 3 fields");
    BlockAddress a{};
    expect_key(r, 0); r.r_fixed(a.user_id.bytes);
    expect_key(r, 1); a.node_index  = static_cast<NodeIndex> (r.r_uint());
    expect_key(r, 2); a.block_index = static_cast<BlockIndex>(r.r_uint());
    return a;
}

ExternalRef dec_ext_ref(CborReader& r) {
    if (r.r_map() != 2) throw SerializationError("ExternalRef: expected 2 fields");
    ExternalRef ref{};
    expect_key(r, 0); ref.address = dec_address(r);
    expect_key(r, 1); r.r_fixed(ref.block_hash.bytes);
    return ref;
}

Node dec_node(CborReader& r) {
    if (r.r_map() != 6) throw SerializationError("Node: expected 6 fields");
    Node n{};
    expect_key(r, 0); n.index = static_cast<NodeIndex>(r.r_uint());
    expect_key(r, 1); r.r_fixed(n.structural_pubkey.bytes);
    expect_key(r, 2); r.r_fixed(n.working_pubkey.bytes);
    expect_key(r, 3); r.r_fixed(n.parent_hash.bytes);
    expect_key(r, 4); r.r_fixed(n.parent_sig.bytes);
    expect_key(r, 5); n.created_at = r.r_int();
    return n;
}

Block dec_block(CborReader& r) {
    if (r.r_map() != 7) throw SerializationError("Block: expected 7 fields");
    Block b{};
    expect_key(r, 0); b.address = dec_address(r);
    expect_key(r, 1); r.r_fixed(b.prev_hash.bytes);
    expect_key(r, 2); b.timestamp_claimed = r.r_int();
    expect_key(r, 3); b.type = static_cast<BlockType>(r.r_uint());
    expect_key(r, 4); b.payload = r.r_bytes();
    expect_key(r, 5);
    const uint64_t ref_count = r.r_arr();
    b.external_refs.reserve(static_cast<size_t>(ref_count));
    for (uint64_t i = 0; i < ref_count; ++i)
        b.external_refs.push_back(dec_ext_ref(r));
    expect_key(r, 6); r.r_fixed(b.signature.bytes);
    return b;
}

Seal dec_seal(CborReader& r) {
    if (r.r_map() != 5) throw SerializationError("Seal: expected 5 fields");
    Seal s{};
    expect_key(r, 0); r.r_fixed(s.signer_id.bytes);
    expect_key(r, 1); r.r_fixed(s.block_hash.bytes);
    expect_key(r, 2); r.r_fixed(s.signature.bytes);
    expect_key(r, 3); s.mode = static_cast<SealMode>(r.r_uint());
    expect_key(r, 4); s.sealed_at = r.r_int();
    return s;
}

MergePayload dec_merge_payload(CborReader& r) {
    if (r.r_map() != 6) throw SerializationError("MergePayload: expected 6 fields");
    MergePayload mp{};
    expect_key(r, 0); mp.partner_last_address = dec_address(r);
    expect_key(r, 1); r.r_fixed(mp.partner_last_hash.bytes);
    expect_key(r, 2); mp.merge_timestamp = r.r_int();
    expect_key(r, 3); r.r_fixed(mp.merkle_root.bytes);
    expect_key(r, 4); r.r_fixed(mp.hll_hash.bytes);
    expect_key(r, 5); mp.validated_depth = static_cast<uint32_t>(r.r_uint());
    return mp;
}

RevocationPayload dec_revocation_payload(CborReader& r) {
    const uint64_t n = r.r_map();
    if (n != 3 && n != 4)
        throw SerializationError("RevocationPayload: expected 3 or 4 fields");
    RevocationPayload rp{};
    expect_key(r, 0); rp.revoked_node_index = static_cast<NodeIndex>(r.r_uint());
    expect_key(r, 1); r.r_fixed(rp.revoked_pubkey.bytes);
    expect_key(r, 2); rp.compromised_since = r.r_int();
    if (n == 4) {
        PublicKey repl{};
        expect_key(r, 3); r.r_fixed(repl.bytes);
        rp.replacement_pubkey = repl;
    }
    return rp;
}

MerkleTree::Proof dec_merkle_proof(CborReader& r) {
    if (r.r_map() != 2) throw SerializationError("MerkleProof: expected 2 fields");
    MerkleTree::Proof p{};
    expect_key(r, 0);
    const uint64_t np = r.r_arr();
    p.path.reserve(static_cast<size_t>(np));
    for (uint64_t i = 0; i < np; ++i) { Hash h{}; r.r_fixed(h.bytes); p.path.push_back(h); }
    expect_key(r, 1);
    const uint64_t nd = r.r_arr();
    p.sibling_is_right.reserve(static_cast<size_t>(nd));
    for (uint64_t i = 0; i < nd; ++i) p.sibling_is_right.push_back(r.r_uint() != 0);
    return p;
}

FraudProofData dec_fraud_proof(CborReader& r) {
    if (r.r_map() != 4) throw SerializationError("FraudProofData: expected 4 fields");
    FraudProofData d{};
    expect_key(r, 0); d.leaf        = dec_ext_ref(r);
    expect_key(r, 1); d.merkle_path = dec_merkle_proof(r);
    expect_key(r, 2);
    const uint64_t nn = r.r_arr();
    d.node_path.reserve(static_cast<size_t>(nn));
    for (uint64_t i = 0; i < nn; ++i) d.node_path.push_back(dec_node(r));
    expect_key(r, 3); d.evidence = dec_block(r);
    return d;
}

MergeSnapshot dec_snapshot(CborReader& r) {
    if (r.r_map() != 2) throw SerializationError("MergeSnapshot: expected 2 fields");
    MergeSnapshot s{};
    expect_key(r, 0); r.r_fixed(s.merkle_root.bytes);
    expect_key(r, 1);
    std::array<uint8_t, HllSketch::REGISTERS> regs{};
    r.r_bytes_exact(regs.data(), HllSketch::REGISTERS);
    s.hll = HllSketch::from_registers(regs);
    return s;
}

BranchTipInfo dec_tip(CborReader& r) {
    if (r.r_map() != 4) throw SerializationError("BranchTipInfo: expected 4 fields");
    BranchTipInfo t{};
    expect_key(r, 0); t.tip_address = dec_address(r);
    expect_key(r, 1); r.r_fixed(t.tip_hash.bytes);
    expect_key(r, 2);
    const uint64_t path_count = r.r_arr();
    t.path.reserve(static_cast<size_t>(path_count));
    for (uint64_t i = 0; i < path_count; ++i)
        t.path.push_back(dec_node(r));
    expect_key(r, 3);
    std::vector<uint8_t> block_bytes = r.r_bytes();
    if (!block_bytes.empty()) {
        CborReader br(block_bytes.data(), block_bytes.size());
        t.tip_block = dec_block(br);
    }
    return t;
}

} // namespace (anonymous)

// ── Public Serializer methods ─────────────────────────────────────────────────

std::vector<uint8_t> Serializer::encode(const Node& node) {
    Buf out; enc_node(out, node); return out;
}

std::vector<uint8_t> Serializer::encode(const Block& block) {
    Buf out; enc_block(out, block); return out;
}

std::vector<uint8_t> Serializer::encode(const Seal& seal) {
    Buf out; enc_seal(out, seal); return out;
}

std::vector<uint8_t> Serializer::encode(const BranchTipInfo& tip) {
    Buf out; enc_tip(out, tip); return out;
}

Node Serializer::decode_node(const uint8_t* data, size_t len) {
    CborReader r(data, len); return dec_node(r);
}

Block Serializer::decode_block(const uint8_t* data, size_t len) {
    CborReader r(data, len); return dec_block(r);
}

Seal Serializer::decode_seal(const uint8_t* data, size_t len) {
    CborReader r(data, len); return dec_seal(r);
}

BranchTipInfo Serializer::decode_tip(const uint8_t* data, size_t len) {
    CborReader r(data, len); return dec_tip(r);
}

std::vector<uint8_t> Serializer::encode(const MergePayload& payload) {
    Buf out; enc_merge_payload(out, payload); return out;
}

MergePayload Serializer::decode_merge_payload(const uint8_t* data, size_t len) {
    CborReader r(data, len); return dec_merge_payload(r);
}

std::vector<uint8_t> Serializer::encode(const ExternalRef& ref) {
    Buf out; enc_ext_ref(out, ref); return out;
}

std::vector<uint8_t> Serializer::encode(const MergeSnapshot& snapshot) {
    Buf out; enc_snapshot(out, snapshot); return out;
}

MergeSnapshot Serializer::decode_snapshot(const uint8_t* data, size_t len) {
    CborReader r(data, len); return dec_snapshot(r);
}

std::vector<uint8_t> Serializer::encode(const FraudProofData& proof) {
    Buf out; enc_fraud_proof(out, proof); return out;
}

std::vector<uint8_t> Serializer::encode(const RevocationPayload& payload) {
    Buf out; enc_revocation_payload(out, payload); return out;
}

RevocationPayload Serializer::decode_revocation_payload(const uint8_t* data, size_t len) {
    CborReader r(data, len); return dec_revocation_payload(r);
}

FraudProofData Serializer::decode_fraud_proof(const uint8_t* data, size_t len) {
    CborReader r(data, len); return dec_fraud_proof(r);
}

} // namespace blockchain
