#include "blockchain/blockchain.h"
#include <cstdio>
#include <cstdint>

int main() {
    auto kp = blockchain::Crypto::generate_keypair();

    std::printf("blockchain demo\n");
    std::printf("public key: ");
    for (int i = 0; i < 8; ++i) {
        std::printf("%02x", static_cast<unsigned>(kp.pub.bytes[i]));
    }
    std::printf("...\n");

    std::vector<uint8_t> data = {1, 2, 3, 4, 5};
    auto sig  = blockchain::Crypto::sign(data.data(), data.size(), kp.sec);
    auto ok   = blockchain::Crypto::verify(data.data(), data.size(), sig, kp.pub);
    std::printf("sign+verify: %s\n", ok ? "ok" : "FAIL");

    return ok ? 0 : 1;
}
