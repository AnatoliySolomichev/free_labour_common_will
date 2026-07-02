#pragma once

#include "types.h"
#include "merge_snapshot.h"
#include "fraud.h"
#include <cstdint>
#include <vector>

namespace blockchain {

// Deterministic CBOR encoding (RFC 8949 §4.2.1).
// Used internally by Crypto::hash_node / hash_block and for network exchange.
class Serializer {
public:
    Serializer() = delete;

    // Serialize to CBOR. Throws: SerializationError.
    static std::vector<uint8_t> encode(const Node& node);
    static std::vector<uint8_t> encode(const Block& block);
    static std::vector<uint8_t> encode(const Seal& seal);
    static std::vector<uint8_t> encode(const BranchTipInfo& tip);
    static std::vector<uint8_t> encode(const MergePayload& payload);
    static std::vector<uint8_t> encode(const ExternalRef& ref);
    static std::vector<uint8_t> encode(const MergeSnapshot& snapshot);
    static std::vector<uint8_t> encode(const FraudProofData& proof);

    // Deserialize from CBOR. Throws: SerializationError on malformed input.
    static Node          decode_node         (const uint8_t* data, size_t len);
    static Block         decode_block        (const uint8_t* data, size_t len);
    static Seal          decode_seal         (const uint8_t* data, size_t len);
    static BranchTipInfo decode_tip          (const uint8_t* data, size_t len);
    static MergePayload  decode_merge_payload(const uint8_t* data, size_t len);
    static MergeSnapshot decode_snapshot     (const uint8_t* data, size_t len);
    static FraudProofData decode_fraud_proof (const uint8_t* data, size_t len);
};

} // namespace blockchain
