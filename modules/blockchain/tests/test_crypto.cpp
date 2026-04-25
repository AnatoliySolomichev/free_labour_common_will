#include "blockchain/crypto.h"
#include "blockchain/errors.h"
#include <gtest/gtest.h>

using namespace blockchain;

TEST(Crypto, GenerateKeypair) {
    auto kp = Crypto::generate_keypair();
    // Public key must not be all-zero (probability of collision is negligible)
    bool nonzero = false;
    for (auto b : kp.pub.bytes) nonzero |= (b != 0);
    EXPECT_TRUE(nonzero);
}

TEST(Crypto, TwoKeypairsAreDifferent) {
    auto kp1 = Crypto::generate_keypair();
    auto kp2 = Crypto::generate_keypair();
    EXPECT_NE(kp1.pub, kp2.pub);
}

TEST(Crypto, HashIsDeterministic) {
    std::vector<uint8_t> data = {1, 2, 3, 4, 5};
    auto h1 = Crypto::hash(data.data(), data.size());
    auto h2 = Crypto::hash(data.data(), data.size());
    EXPECT_EQ(h1, h2);
}

TEST(Crypto, HashDifferentData) {
    std::vector<uint8_t> a = {1, 2, 3};
    std::vector<uint8_t> b = {1, 2, 4};
    EXPECT_NE(Crypto::hash(a.data(), a.size()), Crypto::hash(b.data(), b.size()));
}

TEST(Crypto, HashEmptyData) {
    auto h = Crypto::hash(nullptr, 0);
    bool nonzero = false;
    for (auto byte : h.bytes) nonzero |= (byte != 0);
    EXPECT_TRUE(nonzero);  // BLAKE2b of empty input is not all-zero
}

TEST(Crypto, SignAndVerify) {
    auto kp = Crypto::generate_keypair();
    std::vector<uint8_t> data = {10, 20, 30};
    auto sig = Crypto::sign(data.data(), data.size(), kp.sec);
    EXPECT_TRUE(Crypto::verify(data.data(), data.size(), sig, kp.pub));
}

TEST(Crypto, VerifyWrongKey) {
    auto kp1 = Crypto::generate_keypair();
    auto kp2 = Crypto::generate_keypair();
    std::vector<uint8_t> data = {10, 20, 30};
    auto sig = Crypto::sign(data.data(), data.size(), kp1.sec);
    EXPECT_FALSE(Crypto::verify(data.data(), data.size(), sig, kp2.pub));
}

TEST(Crypto, VerifyTamperedData) {
    auto kp = Crypto::generate_keypair();
    std::vector<uint8_t> data = {10, 20, 30};
    auto sig = Crypto::sign(data.data(), data.size(), kp.sec);
    data[0] ^= 0xFF;
    EXPECT_FALSE(Crypto::verify(data.data(), data.size(), sig, kp.pub));
}

TEST(Crypto, VerifyTamperedSig) {
    auto kp = Crypto::generate_keypair();
    std::vector<uint8_t> data = {10, 20, 30};
    auto sig = Crypto::sign(data.data(), data.size(), kp.sec);
    sig.bytes[0] ^= 0xFF;
    EXPECT_FALSE(Crypto::verify(data.data(), data.size(), sig, kp.pub));
}
