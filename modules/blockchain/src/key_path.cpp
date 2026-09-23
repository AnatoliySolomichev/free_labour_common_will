#include "blockchain/key_path.h"
#include "blockchain/crypto.h"
#include "blockchain/serializer.h"
#include "blockchain/errors.h"

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

KeyPath KeyPathCert::build(const IStorage& storage, const UserId& chain, NodeIndex node) {
    KeyPath kp{};
    for (NodeIndex idx : path_indices(node))
        kp.path.push_back(storage.get_node(chain, idx));  // NodeNotFoundError
    return kp;
}

UserId KeyPathCert::verify(const KeyPath& kp) {
    return verify_node_path(kp.path);
}

const PublicKey& KeyPathCert::signer_key(const KeyPath& kp) {
    if (kp.path.empty())
        throw ChainIntegrityError("key path: empty");
    return kp.path.back().working_pubkey;
}

} // namespace blockchain
