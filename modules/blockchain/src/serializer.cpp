#include "blockchain/serializer.h"
#include "blockchain/errors.h"

// TODO: implement deterministic CBOR encoding (RFC 8949 §4.2.1) using libcbor.
// All encode/decode methods below are stubs that will be replaced.

namespace blockchain {

std::vector<uint8_t> Serializer::encode(const Node&) {
    throw SerializationError("Serializer::encode(Node): not implemented");
}

std::vector<uint8_t> Serializer::encode(const Block&) {
    throw SerializationError("Serializer::encode(Block): not implemented");
}

std::vector<uint8_t> Serializer::encode(const Seal&) {
    throw SerializationError("Serializer::encode(Seal): not implemented");
}

std::vector<uint8_t> Serializer::encode(const BranchTipInfo&) {
    throw SerializationError("Serializer::encode(BranchTipInfo): not implemented");
}

Node Serializer::decode_node(const uint8_t*, size_t) {
    throw SerializationError("Serializer::decode_node: not implemented");
}

Block Serializer::decode_block(const uint8_t*, size_t) {
    throw SerializationError("Serializer::decode_block: not implemented");
}

Seal Serializer::decode_seal(const uint8_t*, size_t) {
    throw SerializationError("Serializer::decode_seal: not implemented");
}

BranchTipInfo Serializer::decode_tip(const uint8_t*, size_t) {
    throw SerializationError("Serializer::decode_tip: not implemented");
}

} // namespace blockchain
