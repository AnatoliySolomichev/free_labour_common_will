#include "aggregator/own_chain.h"

#include <blockchain/blockchain.h>
#include <blockchain/errors.h>

#include <ctime>
#include <fstream>
#include <map>

namespace aggregator {

namespace {

constexpr NodeIndex kLeaf = 0x7FFF'FFFFu;

std::filesystem::path key_path(const std::filesystem::path& dir, NodeIndex idx) {
    return dir / "keys" / (std::to_string(idx) + ".key");
}

// Same 96-byte key-file format the CLI uses: pub(32) ‖ sec(64).
KeyPair load_or_create_keypair(const std::filesystem::path& dir, NodeIndex idx) {
    const auto path = key_path(dir, idx);
    if (std::filesystem::exists(path)) {
        std::ifstream f(path, std::ios::binary);
        KeyPair kp{};
        f.read(reinterpret_cast<char*>(kp.pub.bytes.data()), 32);
        f.read(reinterpret_cast<char*>(kp.sec.bytes.data()), 64);
        if (!f) throw StorageError("own chain: key file truncated: " + path.string());
        return kp;
    }
    std::filesystem::create_directories(path.parent_path());
    const KeyPair kp = Crypto::generate_keypair();
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    f.write(reinterpret_cast<const char*>(kp.pub.bytes.data()), 32);
    f.write(reinterpret_cast<const char*>(kp.sec.bytes.data()), 64);
    if (!f) throw StorageError("own chain: cannot write key file: " + path.string());
    return kp;
}

} // namespace

struct OwnChain::Impl {
    LmdbStorage storage;
    Validator   validator;
    Blockchain  bc;
    UserId      uid{};
    KeyPair     leaf_kp{};

    explicit Impl(const std::filesystem::path& dir)
        : storage(dir / "db"), validator(storage), bc(storage, validator) {
        std::map<NodeIndex, KeyPair> keys;
        for (NodeIndex idx : path_indices(kLeaf))
            keys[idx] = load_or_create_keypair(dir, idx);
        uid     = keys.at(0).pub;
        leaf_kp = keys.at(kLeaf);

        if (!storage.has_node(uid, 0))
            bc.create_identity(keys.at(0));
        if (!storage.has_node(uid, kLeaf))
            bc.ensure_path(uid, kLeaf,
                           [&](NodeIndex idx) { return keys.at(idx); });
    }
};

OwnChain::OwnChain(const std::filesystem::path& dir)
    : impl_(std::make_unique<Impl>(dir)) {}

OwnChain::~OwnChain() = default;

const UserId& OwnChain::uid() const noexcept { return impl_->uid; }

Block OwnChain::append_data(const std::vector<uint8_t>& payload) {
    const auto now = static_cast<Timestamp>(std::time(nullptr));
    return impl_->bc.append_data_block(impl_->uid, kLeaf, payload,
                                       impl_->leaf_kp, now);
}

std::vector<Block> OwnChain::branch() const {
    try {
        return impl_->bc.get_branch(impl_->uid, kLeaf);
    } catch (const BlockchainError&) {
        return {};
    }
}

} // namespace aggregator
