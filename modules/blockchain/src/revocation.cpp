#include "blockchain/revocation.h"
#include "blockchain/key_path.h"
#include "blockchain/crypto.h"
#include "blockchain/serializer.h"
#include "blockchain/errors.h"

namespace blockchain {

RevocationCertificate RevocationCert::build(const IStorage& storage,
                                            const BlockAddress& revocation_block) {
    RevocationCertificate cert{};
    cert.block = storage.get_block(revocation_block); // BlockNotFoundError
    if (cert.block.type != BlockType::REVOCATION)
        throw InvalidArgumentError("RevocationCert::build: not a REVOCATION block");

    cert.path = KeyPathCert::build(storage, revocation_block.user_id, revocation_block.node_index);
    return cert;
}

RevocationPayload RevocationCert::payload(const RevocationCertificate& cert) {
    return Serializer::decode_revocation_payload(cert.block.payload.data(),
                                                 cert.block.payload.size());
}

void RevocationCert::verify(const RevocationCertificate& cert) {
    if (cert.block.type != BlockType::REVOCATION)
        throw RevocationError("certificate: not a REVOCATION block");
    // The key path (blockchain.md §7.5); here it additionally names the chain.
    const UserId chain = KeyPathCert::verify(cert.path);
    if (chain != cert.block.address.user_id)
        throw RevocationError("certificate: root key does not match the block's chain");

    // Author: the block lives in the branch the path ends at, and is signed by
    // the author's key in force at the block's place.
    const NodeIndex author = KeyPathCert::node(cert.path);
    if (author != cert.block.address.node_index)
        throw ChainIntegrityError("certificate: path tip is not the block's node");
    const PublicKey key = KeyPathCert::key_at(cert.path, author, cert.block.address.block_index);
    Block to_verify = cert.block;
    to_verify.signature = Signature::null();
    auto block_bytes = Serializer::encode(to_verify);
    if (!Crypto::verify(block_bytes.data(), block_bytes.size(), cert.block.signature, key))
        throw SignatureError("certificate: block signature by author key invalid");

    // Payload semantics (blockchain.md §6.7 rule 2).
    const RevocationPayload rp = payload(cert); // SerializationError
    if (rp.revoked_node_index == 0)
        throw RevocationError("certificate: root cannot be revoked (blockchain.md §11.5)");
    if (!is_valid_node(rp.revoked_node_index))
        throw RevocationError("certificate: revoked node index is invalid");
    if (!is_ancestor(author, rp.revoked_node_index))
        throw RevocationError(
            "certificate: author is not a strict ancestor of the revoked node");
}

} // namespace blockchain
