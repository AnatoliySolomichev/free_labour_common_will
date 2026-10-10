#include "blockchain/key_path.h"
#include "blockchain/crypto.h"
#include "blockchain/serializer.h"
#include "blockchain/errors.h"

#include <algorithm>
#include <optional>

namespace blockchain {

UserId verify_node_path(const std::vector<Node>& path) {
    if (path.empty())
        throw ChainIntegrityError("node path: empty");

    const Node& root = path.front();
    if (root.index != 0)
        throw ChainIntegrityError("node path: does not start at the root");
    if (root.parent_hash != Hash::zero())
        throw ChainIntegrityError("node path: root parent_hash is not zero");

    for (size_t k = 0; k < path.size(); ++k) {
        const Node& n = path[k];
        Node to_sign = n;
        to_sign.parent_sig = Signature::null();
        const auto bytes = Serializer::encode(to_sign);

        if (k == 0) {
            if (!Crypto::verify(bytes.data(), bytes.size(), n.parent_sig,
                                n.structural_pubkey))
                throw SignatureError("node path: root self-signature invalid");
            continue;
        }
        const Node& parent = path[k - 1];
        if (n.parent_index() != parent.index)
            throw ChainIntegrityError("node path: indices are not parent→child");
        if (n.parent_hash != Crypto::hash_node(parent))
            throw ChainIntegrityError("node path: parent_hash mismatch");
        if (!Crypto::verify(bytes.data(), bytes.size(), n.parent_sig,
                            parent.working_pubkey))
            throw SignatureError("node path: signature by parent key invalid");
    }
    return root.structural_pubkey;
}

namespace {

// Новый рабочий ключ из payload блока смены (blockchain.md §6.6): 32 сырых байта.
std::optional<PublicKey> rotated_key(const Block& b) {
    if (b.type != BlockType::KEY_ROTATION || b.payload.size() < 32)
        return std::nullopt;
    PublicKey k{};
    std::copy(b.payload.begin(), b.payload.begin() + 32, k.bytes.begin());
    return k;
}

bool signed_by(const Block& block, const PublicKey& key) {
    Block to_verify = block;
    to_verify.signature = Signature::null();
    const auto bytes = Serializer::encode(to_verify);
    return Crypto::verify(bytes.data(), bytes.size(), block.signature, key);
}

} // namespace

KeyPath KeyPathCert::build(const IStorage& storage, const UserId& chain, NodeIndex node) {
    KeyPath kp{};
    for (NodeIndex idx : path_indices(node))
        kp.path.push_back(storage.get_node(chain, idx));  // NodeNotFoundError

    // Звенья смены рабочего ключа этой ветки — по порядку блоков. Без них путь
    // привёл бы к первоначальному ключу узла, а подписывает ветка уже новым
    // (blockchain.md §6.6).
    if (const auto tip = storage.branch_tip_index(chain, node)) {
        for (BlockIndex i = 0; i <= *tip; ++i) {
            try {
                const Block b = storage.get_block({chain, node, i});
                if (b.type == BlockType::KEY_ROTATION) kp.rotations.push_back(b);
            } catch (const BlockchainError&) { continue; }  // пробел в ветке — не беда
        }
    }
    return kp;
}

UserId KeyPathCert::verify(const KeyPath& kp) {
    const UserId chain = verify_node_path(kp.path);

    // Звенья смены ключа: каждое подписано ПРЕЖНИМ ключом и называет новый.
    const Node& tip = kp.path.back();
    PublicKey   key = tip.working_pubkey;
    bool        first = true;
    BlockIndex  prev_index = 0;
    for (const Block& b : kp.rotations) {
        if (b.type != BlockType::KEY_ROTATION)
            throw ChainIntegrityError("key path: не блок смены ключа");
        if (!(b.address.user_id == chain) || b.address.node_index != tip.index)
            throw ChainIntegrityError("key path: смена ключа из чужой ветки");
        if (!first && b.address.block_index <= prev_index)
            throw ChainIntegrityError("key path: смены ключа не по порядку");
        if (!signed_by(b, key))
            throw SignatureError("key path: смена ключа не подписана прежним ключом");
        const auto next = rotated_key(b);
        if (!next)
            throw ChainIntegrityError("key path: в смене ключа нет нового ключа");
        key        = *next;
        prev_index = b.address.block_index;
        first      = false;
    }
    return chain;
}

PublicKey KeyPathCert::signer_key(const KeyPath& kp) {
    if (kp.path.empty())
        throw ChainIntegrityError("key path: empty");
    PublicKey key = kp.path.back().working_pubkey;
    for (const Block& b : kp.rotations)
        if (const auto next = rotated_key(b)) key = *next;
    return key;
}

} // namespace blockchain
