#pragma once

#include "types.h"
#include <stdexcept>
#include <vector>

namespace records {

// Thrown on malformed CBOR input or unexpected record structure.
class CodecError : public std::runtime_error {
public:
    explicit CodecError(const char* msg) : std::runtime_error(msg) {}
    explicit CodecError(const std::string& msg) : std::runtime_error(msg) {}
};

// Deterministic CBOR encode/decode for Record types (records.md §2).
// Encoding follows RFC 8949 §4.2.1 (shortest-form integers, sorted integer map keys).
// Each record is a CBOR map with key 0 = RecordType discriminator.
class Codec {
public:
    Codec() = delete;

    // Serialize a Record to CBOR bytes.
    // Throws: CodecError
    static std::vector<uint8_t> encode(const Record& rec);

    // Deserialize a Record from CBOR bytes.
    // Reads the type discriminator (key 0) and dispatches to the correct decoder.
    // Throws: CodecError on malformed data or unknown type.
    static Record decode(const uint8_t* data, size_t len);

    static Record decode(const std::vector<uint8_t>& data) {
        return decode(data.data(), data.size());
    }

    // Блок со списком записей (records.md §2): CBOR-массив записей. Пока — только
    // заметки и не комментарии: на запись внутри списка не сослаться Ref-ом.
    // Пустой список — блок-заглушка (blockchain.md §5.4).
    // Throws: CodecError
    static std::vector<uint8_t> encode_list(const std::vector<Record>& recs);

    // Payload блока целиком: одна запись — список из одной, CBOR-массив — его
    // записи по порядку, пустой массив — ни одной. decode() на массиве по-прежнему
    // отказывает: читатели одиночных записей такие блоки пропускают.
    // Throws: CodecError
    static std::vector<Record> decode_list(const uint8_t* data, size_t len);

    static std::vector<Record> decode_list(const std::vector<uint8_t>& data) {
        return decode_list(data.data(), data.size());
    }
};

} // namespace records
