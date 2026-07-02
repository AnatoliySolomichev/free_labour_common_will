#include "records/codec.h"
#include <cstring>
#include <limits>

// CBOR encoding follows RFC 8949 §4.2.1 (deterministic, shortest form).
// Map keys are unsigned integers 0, 1, 2, ... in ascending order.
// Key 0 is always the RecordType discriminator.

namespace records {
namespace {

using Buf = std::vector<uint8_t>;

// ── CBOR writer ───────────────────────────────────────────────────────────────

void write_head(Buf& out, uint8_t major, uint64_t val) {
    const uint8_t base = static_cast<uint8_t>(major << 5);
    if (val <= 23) {
        out.push_back(base | static_cast<uint8_t>(val));
    } else if (val <= 0xFFu) {
        out.push_back(base | 24u);
        out.push_back(static_cast<uint8_t>(val));
    } else if (val <= 0xFFFFu) {
        out.push_back(base | 25u);
        out.push_back(static_cast<uint8_t>(val >> 8));
        out.push_back(static_cast<uint8_t>(val));
    } else if (val <= 0xFFFF'FFFFu) {
        out.push_back(base | 26u);
        out.push_back(static_cast<uint8_t>(val >> 24));
        out.push_back(static_cast<uint8_t>(val >> 16));
        out.push_back(static_cast<uint8_t>(val >> 8));
        out.push_back(static_cast<uint8_t>(val));
    } else {
        out.push_back(base | 27u);
        for (int i = 7; i >= 0; --i)
            out.push_back(static_cast<uint8_t>(val >> (8 * i)));
    }
}

void w_uint (Buf& out, uint64_t v)  { write_head(out, 0, v); }
void w_map  (Buf& out, uint64_t n)  { write_head(out, 5, n); }
void w_arr  (Buf& out, uint64_t n)  { write_head(out, 4, n); }

void w_int64(Buf& out, int64_t v) {
    if (v >= 0) write_head(out, 0, static_cast<uint64_t>(v));
    else        write_head(out, 1, static_cast<uint64_t>(-1 - v));
}

void w_bytes(Buf& out, const uint8_t* data, size_t len) {
    write_head(out, 2, len);
    out.insert(out.end(), data, data + len);
}

template<size_t N>
void w_fixed(Buf& out, const std::array<uint8_t, N>& a) {
    w_bytes(out, a.data(), N);
}

void w_text(Buf& out, const std::string& s) {
    write_head(out, 3, s.size());
    out.insert(out.end(), s.begin(), s.end());
}

// IEEE 754 double encoded as CBOR float64 (major type 7, additional info 27).
void w_float64(Buf& out, double v) {
    out.push_back(0xfbu);  // 0xFB = major 7, info 27
    uint64_t bits;
    std::memcpy(&bits, &v, 8);
    for (int i = 7; i >= 0; --i)
        out.push_back(static_cast<uint8_t>(bits >> (8 * i)));
}

void w_ref(Buf& out, const Ref& r) {
    w_map(out, 2);
    w_uint(out, 0); w_fixed(out, r.chain);
    w_uint(out, 1); w_fixed(out, r.hash);
}

void w_resource_qty(Buf& out, const ResourceQty& rq) {
    w_map(out, 3);
    w_uint(out, 0); w_text(out, rq.resource);
    w_uint(out, 1); w_float64(out, rq.qty);
    w_uint(out, 2); w_text(out, rq.unit);
}

// ── Per-type encoders ─────────────────────────────────────────────────────────

void enc_fraud_claim(Buf& out, const FraudClaim& f) {
    w_map(out, 5);
    w_uint(out, 0); w_uint(out, static_cast<uint8_t>(RecordType::FraudClaim));
    w_uint(out, 1); w_ref(out, f.target);
    w_uint(out, 2); w_text(out, f.kind);
    w_uint(out, 3); w_bytes(out, f.proof.data(), f.proof.size());
    w_uint(out, 4); w_text(out, f.reason);
}

void enc_concept(Buf& out, const Concept& c) {
    w_map(out, 3);
    w_uint(out, 0); w_uint(out, static_cast<uint8_t>(RecordType::Concept));
    w_uint(out, 1); w_text(out, c.text);
    w_uint(out, 2); w_arr(out, c.tags.size());
    for (const auto& tag : c.tags) w_text(out, tag);
}

void enc_concept_link(Buf& out, const ConceptLink& cl) {
    w_map(out, 4);
    w_uint(out, 0); w_uint(out, static_cast<uint8_t>(RecordType::ConceptLink));
    w_uint(out, 1); w_ref(out, cl.from);
    w_uint(out, 2); w_ref(out, cl.to);
    w_uint(out, 3); w_text(out, cl.kind);
}

void enc_composite(Buf& out, const Composite& c) {
    w_map(out, 3);
    w_uint(out, 0); w_uint(out, static_cast<uint8_t>(RecordType::Composite));
    w_uint(out, 1); w_text(out, c.title);
    w_uint(out, 2); w_arr(out, c.parts.size());
    for (const auto& part : c.parts) w_ref(out, part);
}

void enc_copy(Buf& out, const Copy& c) {
    w_map(out, 2);
    w_uint(out, 0); w_uint(out, static_cast<uint8_t>(RecordType::Copy));
    w_uint(out, 1); w_ref(out, c.source);
}

void enc_reaction(Buf& out, const Reaction& r) {
    w_map(out, 3);
    w_uint(out, 0); w_uint(out, static_cast<uint8_t>(RecordType::Reaction));
    w_uint(out, 1); w_ref(out, r.target);
    w_uint(out, 2); w_int64(out, static_cast<int64_t>(r.value));
}

void enc_specialty(Buf& out, const Specialty& s) {
    w_map(out, 2);
    w_uint(out, 0); w_uint(out, static_cast<uint8_t>(RecordType::Specialty));
    w_uint(out, 1); w_text(out, s.name);
}

void enc_grade(Buf& out, const Grade& g) {
    w_map(out, 3);
    w_uint(out, 0); w_uint(out, static_cast<uint8_t>(RecordType::Grade));
    w_uint(out, 1); w_ref(out, g.specialty);
    w_uint(out, 2); w_uint(out, g.level);
}

void enc_worker(Buf& out, const Worker& w) {
    w_map(out, 2);
    w_uint(out, 0); w_uint(out, static_cast<uint8_t>(RecordType::Worker));
    w_uint(out, 1); w_fixed(out, w.chain);
}

void enc_work_record(Buf& out, const WorkRecord& wr) {
    w_map(out, 7);
    w_uint(out, 0); w_uint(out, static_cast<uint8_t>(RecordType::WorkRecord));
    w_uint(out, 1); w_ref(out, wr.agent);
    w_uint(out, 2); w_text(out, wr.action);
    w_uint(out, 3); w_int64(out, wr.start_ts);
    w_uint(out, 4); w_float64(out, wr.hours);
    w_uint(out, 5); w_arr(out, wr.inputs.size());
    for (const auto& rq : wr.inputs) w_resource_qty(out, rq);
    w_uint(out, 6); w_arr(out, wr.outputs.size());
    for (const auto& rq : wr.outputs) w_resource_qty(out, rq);
}

void enc_acceptance(Buf& out, const Acceptance& a) {
    w_map(out, 7);
    w_uint(out, 0); w_uint(out, static_cast<uint8_t>(RecordType::Acceptance));
    w_uint(out, 1); w_ref(out, a.work);
    w_uint(out, 2); w_fixed(out, a.receiver);
    w_uint(out, 3); w_text(out, a.quality);
    w_uint(out, 4); w_float64(out, a.hours_raw);
    w_uint(out, 5); w_float64(out, a.labor_units);
    w_uint(out, 6); w_int64(out, a.timestamp);
}

// ── CBOR reader ───────────────────────────────────────────────────────────────

class CborReader {
public:
    CborReader(const uint8_t* data, size_t size) : data_(data), size_(size), pos_(0) {}

    std::pair<uint8_t, uint64_t> read_head() {
        need(1);
        const uint8_t initial = data_[pos_++];
        const uint8_t major   = initial >> 5;
        const uint8_t info    = initial & 0x1fu;
        if (info <= 23) return {major, info};
        if (info == 24) { need(1); return {major, data_[pos_++]}; }
        if (info == 25) {
            need(2);
            const uint64_t v = (static_cast<uint64_t>(data_[pos_]) << 8)
                             |  static_cast<uint64_t>(data_[pos_+1]);
            pos_ += 2; return {major, v};
        }
        if (info == 26) {
            need(4);
            const uint64_t v = (static_cast<uint64_t>(data_[pos_  ]) << 24)
                             | (static_cast<uint64_t>(data_[pos_+1]) << 16)
                             | (static_cast<uint64_t>(data_[pos_+2]) <<  8)
                             |  static_cast<uint64_t>(data_[pos_+3]);
            pos_ += 4; return {major, v};
        }
        if (info == 27) {
            need(8);
            uint64_t v = 0;
            for (int i = 0; i < 8; ++i) v = (v << 8) | data_[pos_++];
            return {major, v};
        }
        throw CodecError("CBOR: unsupported additional info");
    }

    uint64_t r_uint() {
        const auto [m, v] = read_head();
        if (m != 0) throw CodecError("CBOR: expected uint");
        return v;
    }

    int64_t r_int() {
        const auto [m, v] = read_head();
        if (m == 0) {
            if (v > static_cast<uint64_t>(std::numeric_limits<int64_t>::max()))
                throw CodecError("CBOR: uint overflows int64_t");
            return static_cast<int64_t>(v);
        }
        if (m == 1) {
            if (v > static_cast<uint64_t>(std::numeric_limits<int64_t>::max()))
                throw CodecError("CBOR: negint underflows int64_t");
            return -1LL - static_cast<int64_t>(v);
        }
        throw CodecError("CBOR: expected integer");
    }

    int8_t r_int8() {
        const int64_t v = r_int();
        if (v < -128 || v > 127) throw CodecError("CBOR: value out of int8_t range");
        return static_cast<int8_t>(v);
    }

    uint8_t r_uint8() {
        const uint64_t v = r_uint();
        if (v > 255) throw CodecError("CBOR: value out of uint8_t range");
        return static_cast<uint8_t>(v);
    }

    // Reads CBOR float64 (major 7, additional info 27, 8-byte IEEE 754 big-endian).
    double r_float64() {
        const auto [m, bits] = read_head();
        if (m != 7) throw CodecError("CBOR: expected float64");
        double v;
        std::memcpy(&v, &bits, 8);
        return v;
    }

    void r_bytes_exact(uint8_t* out, size_t expected) {
        const auto [m, len] = read_head();
        if (m != 2) throw CodecError("CBOR: expected byte string");
        if (len != static_cast<uint64_t>(expected))
            throw CodecError("CBOR: byte string length mismatch");
        need(static_cast<size_t>(len));
        std::memcpy(out, data_ + pos_, len);
        pos_ += static_cast<size_t>(len);
    }

    std::vector<uint8_t> r_bytes() {
        const auto [m, len] = read_head();
        if (m != 2) throw CodecError("CBOR: expected byte string");
        need(static_cast<size_t>(len));
        std::vector<uint8_t> v(data_ + pos_, data_ + pos_ + len);
        pos_ += static_cast<size_t>(len);
        return v;
    }

    template<size_t N>
    void r_fixed(std::array<uint8_t, N>& arr) { r_bytes_exact(arr.data(), N); }

    std::string r_text() {
        const auto [m, len] = read_head();
        if (m != 3) throw CodecError("CBOR: expected text string");
        need(static_cast<size_t>(len));
        std::string s(reinterpret_cast<const char*>(data_ + pos_), len);
        pos_ += static_cast<size_t>(len);
        return s;
    }

    uint64_t r_map() {
        const auto [m, n] = read_head();
        if (m != 5) throw CodecError("CBOR: expected map");
        return n;
    }

    uint64_t r_arr() {
        const auto [m, n] = read_head();
        if (m != 4) throw CodecError("CBOR: expected array");
        return n;
    }

private:
    const uint8_t* data_;
    size_t         size_;
    size_t         pos_;

    void need(size_t n) const {
        if (pos_ + n > size_)
            throw CodecError("CBOR: unexpected end of data");
    }
};

// ── Per-type decoders ─────────────────────────────────────────────────────────

// Called after map header and key-0 (type) have already been consumed.

void expect_key(CborReader& r, uint64_t expected) {
    if (r.r_uint() != expected)
        throw CodecError("CBOR: unexpected map key");
}

Ref dec_ref(CborReader& r) {
    if (r.r_map() != 2) throw CodecError("Ref: expected 2 fields");
    Ref ref{};
    expect_key(r, 0); r.r_fixed(ref.chain);
    expect_key(r, 1); r.r_fixed(ref.hash);
    return ref;
}

ResourceQty dec_resource_qty(CborReader& r) {
    if (r.r_map() != 3) throw CodecError("ResourceQty: expected 3 fields");
    ResourceQty rq{};
    expect_key(r, 0); rq.resource = r.r_text();
    expect_key(r, 1); rq.qty      = r.r_float64();
    expect_key(r, 2); rq.unit     = r.r_text();
    return rq;
}

FraudClaim dec_fraud_claim_fields(CborReader& r) {
    FraudClaim f{};
    expect_key(r, 1); f.target = dec_ref(r);
    expect_key(r, 2); f.kind   = r.r_text();
    expect_key(r, 3); f.proof  = r.r_bytes();
    expect_key(r, 4); f.reason = r.r_text();
    return f;
}

Concept dec_concept_fields(CborReader& r) {
    // map(3) and key 0 already consumed; reads keys 1, 2
    Concept c{};
    expect_key(r, 1); c.text = r.r_text();
    expect_key(r, 2);
    const uint64_t n = r.r_arr();
    c.tags.reserve(static_cast<size_t>(n));
    for (uint64_t i = 0; i < n; ++i) c.tags.push_back(r.r_text());
    return c;
}

ConceptLink dec_concept_link_fields(CborReader& r) {
    ConceptLink cl{};
    expect_key(r, 1); cl.from = dec_ref(r);
    expect_key(r, 2); cl.to   = dec_ref(r);
    expect_key(r, 3); cl.kind = r.r_text();
    return cl;
}

Composite dec_composite_fields(CborReader& r) {
    Composite c{};
    expect_key(r, 1); c.title = r.r_text();
    expect_key(r, 2);
    const uint64_t n = r.r_arr();
    c.parts.reserve(static_cast<size_t>(n));
    for (uint64_t i = 0; i < n; ++i) c.parts.push_back(dec_ref(r));
    return c;
}

Copy dec_copy_fields(CborReader& r) {
    Copy c{};
    expect_key(r, 1); c.source = dec_ref(r);
    return c;
}

Reaction dec_reaction_fields(CborReader& r) {
    Reaction rx{};
    expect_key(r, 1); rx.target = dec_ref(r);
    expect_key(r, 2); rx.value  = r.r_int8();
    return rx;
}

Specialty dec_specialty_fields(CborReader& r) {
    Specialty s{};
    expect_key(r, 1); s.name = r.r_text();
    return s;
}

Grade dec_grade_fields(CborReader& r) {
    Grade g{};
    expect_key(r, 1); g.specialty = dec_ref(r);
    expect_key(r, 2); g.level     = r.r_uint8();
    return g;
}

Worker dec_worker_fields(CborReader& r) {
    Worker w{};
    expect_key(r, 1); r.r_fixed(w.chain);
    return w;
}

WorkRecord dec_work_record_fields(CborReader& r) {
    WorkRecord wr{};
    expect_key(r, 1); wr.agent    = dec_ref(r);
    expect_key(r, 2); wr.action   = r.r_text();
    expect_key(r, 3); wr.start_ts = r.r_int();
    expect_key(r, 4); wr.hours    = r.r_float64();
    expect_key(r, 5);
    {
        const uint64_t n = r.r_arr();
        wr.inputs.reserve(static_cast<size_t>(n));
        for (uint64_t i = 0; i < n; ++i) wr.inputs.push_back(dec_resource_qty(r));
    }
    expect_key(r, 6);
    {
        const uint64_t n = r.r_arr();
        wr.outputs.reserve(static_cast<size_t>(n));
        for (uint64_t i = 0; i < n; ++i) wr.outputs.push_back(dec_resource_qty(r));
    }
    return wr;
}

Acceptance dec_acceptance_fields(CborReader& r) {
    Acceptance a{};
    expect_key(r, 1); a.work         = dec_ref(r);
    expect_key(r, 2); r.r_fixed(a.receiver);
    expect_key(r, 3); a.quality      = r.r_text();
    expect_key(r, 4); a.hours_raw    = r.r_float64();
    expect_key(r, 5); a.labor_units  = r.r_float64();
    expect_key(r, 6); a.timestamp    = r.r_int();
    return a;
}

} // namespace (anonymous)

// ── Codec public methods ──────────────────────────────────────────────────────

std::vector<uint8_t> Codec::encode(const Record& rec) {
    Buf out;
    std::visit([&out](const auto& r) {
        using T = std::decay_t<decltype(r)>;
        if      constexpr (std::is_same_v<T, FraudClaim>)  enc_fraud_claim(out, r);
        else if constexpr (std::is_same_v<T, Concept>)     enc_concept(out, r);
        else if constexpr (std::is_same_v<T, ConceptLink>) enc_concept_link(out, r);
        else if constexpr (std::is_same_v<T, Composite>)   enc_composite(out, r);
        else if constexpr (std::is_same_v<T, Copy>)        enc_copy(out, r);
        else if constexpr (std::is_same_v<T, Reaction>)    enc_reaction(out, r);
        else if constexpr (std::is_same_v<T, Specialty>)   enc_specialty(out, r);
        else if constexpr (std::is_same_v<T, Grade>)       enc_grade(out, r);
        else if constexpr (std::is_same_v<T, Worker>)      enc_worker(out, r);
        else if constexpr (std::is_same_v<T, WorkRecord>)  enc_work_record(out, r);
        else if constexpr (std::is_same_v<T, Acceptance>)  enc_acceptance(out, r);
    }, rec);
    return out;
}

Record Codec::decode(const uint8_t* data, size_t len) {
    CborReader r(data, len);
    r.r_map();          // consume map header (field count not checked here; per-type decoders own their layout)
    expect_key(r, 0);   // type discriminator is always key 0
    const uint8_t disc = r.r_uint8();

    switch (static_cast<RecordType>(disc)) {
        case RecordType::FraudClaim:  return dec_fraud_claim_fields(r);
        case RecordType::Concept:     return dec_concept_fields(r);
        case RecordType::ConceptLink: return dec_concept_link_fields(r);
        case RecordType::Composite:   return dec_composite_fields(r);
        case RecordType::Copy:        return dec_copy_fields(r);
        case RecordType::Reaction:    return dec_reaction_fields(r);
        case RecordType::Specialty:   return dec_specialty_fields(r);
        case RecordType::Grade:       return dec_grade_fields(r);
        case RecordType::Worker:      return dec_worker_fields(r);
        case RecordType::WorkRecord:  return dec_work_record_fields(r);
        case RecordType::Acceptance:  return dec_acceptance_fields(r);
        default:
            throw CodecError("CBOR: unknown record type discriminator");
    }
}

} // namespace records
