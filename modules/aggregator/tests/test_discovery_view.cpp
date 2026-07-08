#include "aggregator/discovery_view.h"

#include <blockchain/crypto.h>
#include <blockchain/serializer.h>
#include <records/codec.h>
#include <records/types.h>

#include <gtest/gtest.h>
#include <filesystem>

using namespace aggregator;
using namespace blockchain;

namespace {

UserId make_chain(uint8_t fill) {
    UserId u{};
    u.bytes.fill(fill);
    return u;
}

Block make_block(const UserId& owner, BlockIndex index, BlockType type,
                 std::vector<uint8_t> payload) {
    Block b{};
    b.address           = {owner, 0x7FFF'FFFFu, index};
    b.prev_hash         = Hash::zero();
    b.timestamp_claimed = static_cast<Timestamp>(index) * 1000LL;
    b.type              = type;
    b.payload           = std::move(payload);
    b.signature         = Signature::null();
    const auto bytes    = Serializer::encode(b);
    const KeyPair kp    = Crypto::generate_keypair();
    b.signature         = Crypto::sign(bytes.data(), bytes.size(), kp.sec);
    return b;
}

Block make_transfer(const UserId& from, const UserId& to, double units,
                    BlockIndex index) {
    records::Transfer t{};
    t.from    = from.bytes;
    t.to      = to.bytes;
    t.origins = { {from.bytes, units} };
    return make_block(from, index, BlockType::DATA, records::Codec::encode(t));
}

Block make_merge(const UserId& owner, const UserId& partner, BlockIndex index) {
    MergePayload mp{};
    mp.partner_last_address = {partner, 0x7FFF'FFFFu, 0};
    return make_block(owner, index, BlockType::MERGE, Serializer::encode(mp));
}

} // namespace

TEST(DiscoveryView, RanksEconomicPartnersNeighborsAndHubs) {
    const auto db = std::filesystem::temp_directory_path() / "bc_discovery_test";
    std::filesystem::remove_all(db);
    {
        AggregatorStorage storage(db);
        const UserId a = make_chain(0xA1), b = make_chain(0xB2),
                     c = make_chain(0xC3), d = make_chain(0xD4),
                     e = make_chain(0xE5), f = make_chain(0xF6);

        BlockIndex i = 0;
        storage.add_block(make_transfer(a, b, 5.0, i++));   // A↔B: economic tie
        storage.add_block(make_merge(b, c, i++));           // C: neighbor via B
        storage.add_block(make_merge(d, e, i++));           // D: hub (2 merges),
        storage.add_block(make_merge(d, f, i++));           //   unrelated to A

        const auto view = DiscoveryView::build(storage);
        const auto ranked = view.candidates_for(a, 20);

        // A itself excluded; everyone else present.
        ASSERT_EQ(ranked.size(), 5u);
        for (const auto& cand : ranked) EXPECT_FALSE(cand.chain == a);

        // Economic partner first, neighbor second, hub third.
        EXPECT_TRUE(ranked[0].chain == b);
        EXPECT_DOUBLE_EQ(ranked[0].econ_volume, 5.0);
        EXPECT_TRUE(ranked[1].chain == c);
        EXPECT_TRUE(ranked[1].neighbor);
        EXPECT_TRUE(ranked[2].chain == d);
        EXPECT_EQ(ranked[2].degree, 2u);

        // After A merges with B, B's score halves (already covered) but a
        // strong economic partner still outranks a mere neighbor.
        const double b_before = ranked[0].score;
        storage.add_block(make_merge(a, b, i++));
        const auto after = DiscoveryView::build(storage).candidates_for(a, 20);
        EXPECT_TRUE(after[0].chain == b);
        EXPECT_EQ(after[0].merges_with, 1u);
        EXPECT_LT(after[0].score, b_before);
    }
    std::filesystem::remove_all(db);
}
