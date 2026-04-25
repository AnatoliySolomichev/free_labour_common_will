#include "blockchain/serializer.h"
#include "blockchain/errors.h"
#include <gtest/gtest.h>

using namespace blockchain;

// ── Helpers ───────────────────────────────────────────────────────────────────

static PublicKey make_pubkey(uint8_t fill) {
    PublicKey k;
    k.bytes.fill(fill);
    return k;
}

static Hash make_hash(uint8_t fill) {
    Hash h;
    h.bytes.fill(fill);
    return h;
}

static Signature make_sig(uint8_t fill) {
    Signature s;
    s.bytes.fill(fill);
    return s;
}

static Node make_node(NodeIndex idx, uint8_t fill = 0xAB) {
    Node n{};
    n.index              = idx;
    n.structural_pubkey  = make_pubkey(fill);
    n.working_pubkey     = make_pubkey(static_cast<uint8_t>(fill + 1));
    n.parent_hash        = make_hash(fill);
    n.parent_sig         = make_sig(fill);
    n.created_at         = 1'700'000'000LL;
    return n;
}

static BlockAddress make_addr(NodeIndex ni, BlockIndex bi, uint8_t uid_fill = 0x01) {
    BlockAddress a{};
    a.user_id    = make_pubkey(uid_fill);
    a.node_index  = ni;
    a.block_index = bi;
    return a;
}

static Block make_data_block(NodeIndex ni, BlockIndex bi) {
    Block b{};
    b.address           = make_addr(ni, bi);
    b.prev_hash         = make_hash(0x22);
    b.timestamp_claimed = 1'700'000'042LL;
    b.type              = BlockType::DATA;
    b.payload           = {0x01, 0x02, 0x03};
    b.signature         = make_sig(0x55);
    return b;
}

// ── CBOR integer encoding (deterministic / shortest form) ─────────────────────

// We verify the raw bytes for small integers that must use inline encoding.
// A CBOR map with 1 pair {uint(0): uint(5)} must be:  0xa1 0x00 0x05
// (not 0xa1 0x18 0x00 0x18 0x05 which libcbor would produce with fixed widths).

TEST(Serializer, NodeIndexInlineEncoding) {
    // Encode a node with index=0.  The first field key is 0 and the value is 0.
    // In the CBOR map header (6 fields = 0xa6) the key 0 → 0x00 (1 byte inline).
    Node n = make_node(0);
    auto bytes = Serializer::encode(n);

    // First byte: map(6) = 0xa6
    ASSERT_FALSE(bytes.empty());
    EXPECT_EQ(bytes[0], 0xa6u);

    // Second byte: key 0 = 0x00  (inline, not 0x18 0x00)
    ASSERT_GE(bytes.size(), 2u);
    EXPECT_EQ(bytes[1], 0x00u);

    // Third byte: value 0 (node.index=0) = 0x00  (inline)
    ASSERT_GE(bytes.size(), 3u);
    EXPECT_EQ(bytes[2], 0x00u);
}

TEST(Serializer, LargeNodeIndex) {
    // node_index = 256 → must be encoded as 0x19 0x01 0x00 (3 bytes)
    Node n = make_node(256);
    auto bytes = Serializer::encode(n);
    // map(6), key(0), uint(256): bytes[0]=0xa6, [1]=0x00, [2]=0x19, [3]=0x01, [4]=0x00
    ASSERT_GE(bytes.size(), 5u);
    EXPECT_EQ(bytes[2], 0x19u);
    EXPECT_EQ(bytes[3], 0x01u);
    EXPECT_EQ(bytes[4], 0x00u);
}

// ── Node round-trip ───────────────────────────────────────────────────────────

TEST(Serializer, NodeRoundTrip) {
    Node original = make_node(42);
    auto encoded  = Serializer::encode(original);
    Node decoded  = Serializer::decode_node(encoded.data(), encoded.size());
    EXPECT_EQ(original, decoded);
}

TEST(Serializer, NodeRoundTripRootIndex) {
    Node n = make_node(0);
    auto enc = Serializer::encode(n);
    EXPECT_EQ(n, Serializer::decode_node(enc.data(), enc.size()));
}

TEST(Serializer, NodeRoundTripLargeIndex) {
    Node n = make_node(0xFFFF'FFFFu);
    auto enc = Serializer::encode(n);
    EXPECT_EQ(n, Serializer::decode_node(enc.data(), enc.size()));
}

TEST(Serializer, NodeNegativeTimestamp) {
    Node n       = make_node(1);
    n.created_at = -1LL;  // before Unix epoch — unusual but valid
    auto enc     = Serializer::encode(n);
    Node decoded = Serializer::decode_node(enc.data(), enc.size());
    EXPECT_EQ(decoded.created_at, -1LL);
}

TEST(Serializer, NodeDeterministic) {
    Node n    = make_node(7);
    auto enc1 = Serializer::encode(n);
    auto enc2 = Serializer::encode(n);
    EXPECT_EQ(enc1, enc2);
}

// ── Block round-trip ──────────────────────────────────────────────────────────

TEST(Serializer, BlockRoundTripData) {
    Block b = make_data_block(3, 0);
    auto enc = Serializer::encode(b);
    Block d  = Serializer::decode_block(enc.data(), enc.size());

    EXPECT_EQ(b.address,           d.address);
    EXPECT_EQ(b.prev_hash,         d.prev_hash);
    EXPECT_EQ(b.timestamp_claimed, d.timestamp_claimed);
    EXPECT_EQ(b.type,              d.type);
    EXPECT_EQ(b.payload,           d.payload);
    EXPECT_EQ(b.external_refs,     d.external_refs);
    EXPECT_EQ(b.signature,         d.signature);
}

TEST(Serializer, BlockCoSignatureExcluded) {
    Block b = make_data_block(1, 0);
    b.co_signature = make_sig(0xFF);  // set a co-signature

    auto enc_with    = Serializer::encode(b);
    b.co_signature   = std::nullopt;
    auto enc_without = Serializer::encode(b);

    // Canonical encoding must be identical regardless of co_signature
    EXPECT_EQ(enc_with, enc_without);
}

TEST(Serializer, BlockEmptyPayload) {
    Block b = make_data_block(0, 0);
    b.payload.clear();
    auto enc = Serializer::encode(b);
    Block d  = Serializer::decode_block(enc.data(), enc.size());
    EXPECT_TRUE(d.payload.empty());
}

TEST(Serializer, BlockWithExternalRefs) {
    Block b = make_data_block(2, 5);
    ExternalRef ref{};
    ref.address   = make_addr(10, 3, 0x02);
    ref.block_hash = make_hash(0x33);
    b.external_refs.push_back(ref);

    auto enc = Serializer::encode(b);
    Block d  = Serializer::decode_block(enc.data(), enc.size());

    ASSERT_EQ(d.external_refs.size(), 1u);
    EXPECT_EQ(d.external_refs[0], ref);
}

TEST(Serializer, BlockAllTypes) {
    for (auto t : {BlockType::DATA, BlockType::MERGE,
                   BlockType::KEY_ROTATION, BlockType::REVOCATION}) {
        Block b  = make_data_block(0, 0);
        b.type   = t;
        auto enc = Serializer::encode(b);
        Block d  = Serializer::decode_block(enc.data(), enc.size());
        EXPECT_EQ(d.type, t);
    }
}

TEST(Serializer, BlockDeterministic) {
    Block b   = make_data_block(7, 0);
    auto enc1 = Serializer::encode(b);
    auto enc2 = Serializer::encode(b);
    EXPECT_EQ(enc1, enc2);
}

// ── Seal round-trip ───────────────────────────────────────────────────────────

TEST(Serializer, SealRoundTripBlind) {
    Seal s{};
    s.signer_id = make_pubkey(0xAA);
    s.block_hash = make_hash(0xBB);
    s.signature  = make_sig(0xCC);
    s.mode       = SealMode::BLIND;
    s.sealed_at  = 1'700'000'100LL;

    auto enc = Serializer::encode(s);
    Seal d   = Serializer::decode_seal(enc.data(), enc.size());
    EXPECT_EQ(s, d);
}

TEST(Serializer, SealRoundTripOpen) {
    Seal s{};
    s.signer_id  = make_pubkey(0x11);
    s.block_hash = make_hash(0x22);
    s.signature  = make_sig(0x33);
    s.mode       = SealMode::OPEN;
    s.sealed_at  = 0LL;

    auto enc = Serializer::encode(s);
    EXPECT_EQ(s, Serializer::decode_seal(enc.data(), enc.size()));
}

// ── BranchTipInfo round-trip ──────────────────────────────────────────────────

TEST(Serializer, BranchTipInfoRoundTrip) {
    BranchTipInfo t{};
    t.tip_address = make_addr(5, 10);
    t.tip_hash    = make_hash(0x77);
    t.path.push_back(make_node(0, 0x10));
    t.path.push_back(make_node(1, 0x20));

    auto enc = Serializer::encode(t);
    BranchTipInfo d = Serializer::decode_tip(enc.data(), enc.size());

    EXPECT_EQ(d.tip_address, t.tip_address);
    EXPECT_EQ(d.tip_hash,    t.tip_hash);
    ASSERT_EQ(d.path.size(), 2u);
    EXPECT_EQ(d.path[0], t.path[0]);
    EXPECT_EQ(d.path[1], t.path[1]);
}

TEST(Serializer, BranchTipInfoEmptyPath) {
    BranchTipInfo t{};
    t.tip_address = make_addr(0, EMPTY_BRANCH_INDEX);
    t.tip_hash    = make_hash(0x00);

    auto enc = Serializer::encode(t);
    BranchTipInfo d = Serializer::decode_tip(enc.data(), enc.size());
    EXPECT_TRUE(d.path.empty());
    EXPECT_EQ(d.tip_address.block_index, EMPTY_BRANCH_INDEX);
}

// ── Error cases ───────────────────────────────────────────────────────────────

TEST(Serializer, DecodeNodeEmptyData) {
    EXPECT_THROW(Serializer::decode_node(nullptr, 0), SerializationError);
}

TEST(Serializer, DecodeNodeTruncated) {
    Node n   = make_node(1);
    auto enc = Serializer::encode(n);
    enc.resize(enc.size() / 2);  // cut in half
    EXPECT_THROW(
        Serializer::decode_node(enc.data(), enc.size()),
        SerializationError);
}

TEST(Serializer, DecodeBlockWrongType) {
    // Feed node bytes to decode_block — should throw on field count mismatch
    Node n   = make_node(1);
    auto enc = Serializer::encode(n);
    EXPECT_THROW(
        Serializer::decode_block(enc.data(), enc.size()),
        SerializationError);
}
