#include "records/codec.h"
#include "records/acceptance.h"
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

// CBOR null (0xF6) marks an absent optional field; the map key stays present
// so the layout is deterministic.
void w_null(Buf& out) { out.push_back(0xf6u); }

void w_ref(Buf& out, const Ref& r) {
    w_map(out, 2);
    w_uint(out, 0); w_fixed(out, r.chain);
    w_uint(out, 1); w_fixed(out, r.hash);
}

// Полный путь к блоку: цепь, ветка (узел), номер в ветке, хеш для контроля.
// Та же форма, что лист ExternalRef в доказательствах слияний.
void w_block_path(Buf& out, const BlockPath& p) {
    w_map(out, 4);
    w_uint(out, 0); w_fixed(out, p.chain);
    w_uint(out, 1); w_uint(out, p.node);
    w_uint(out, 2); w_uint(out, p.block);
    w_uint(out, 3); w_fixed(out, p.hash);
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

// One carry-thread link (records.md §9.4 v2, ИР-011).
void w_carry_entry(Buf& out, const CarryEntry& ce) {
    w_map(out, 6);
    w_uint(out, 0); w_ref(out, ce.src);
    w_uint(out, 1); w_float64(out, ce.used);
    w_uint(out, 2); w_float64(out, ce.carried);
    w_uint(out, 3); w_uint(out, ce.seq);
    w_uint(out, 4);
    if (ce.prev) w_ref(out, *ce.prev); else w_null(out);
    w_uint(out, 5); w_float64(out, ce.after);
}

void enc_work_record(Buf& out, const WorkRecord& wr) {
    // Key 1 (agent → Grade) is retired with the grade (ИР-022 шаг 11). The
    // optional tail — carry (7), title (8) — is written only when present and in
    // ascending key order, so the encoding stays deterministic.
    w_map(out, 6 + (wr.carry.empty() ? 0 : 1) + (wr.title.empty() ? 0 : 1));
    w_uint(out, 0); w_uint(out, static_cast<uint8_t>(RecordType::WorkRecord));
    w_uint(out, 2); w_text(out, wr.action);
    w_uint(out, 3); w_int64(out, wr.start_ts);
    w_uint(out, 4); w_float64(out, wr.hours);
    w_uint(out, 5); w_arr(out, wr.inputs.size());
    for (const auto& rq : wr.inputs) w_resource_qty(out, rq);
    w_uint(out, 6); w_arr(out, wr.outputs.size());
    for (const auto& rq : wr.outputs) w_resource_qty(out, rq);
    if (!wr.carry.empty()) {
        w_uint(out, 7); w_arr(out, wr.carry.size());
        for (const auto& ce : wr.carry) w_carry_entry(out, ce);
    }
    if (!wr.title.empty()) { w_uint(out, 8); w_text(out, wr.title); }
}

// The acceptance (records.md §9.5): since ИР-023 not a record of its own but the
// `accept` field of the buyer's Transfer — a nested map. Required: 0 work,
// 1 quality, 2 hours_raw, 3 labor_units, 4 the breakdown; optional, each only
// when set and in ascending order: 5 carried_units, 6 norm, 7 like.
void w_acceptance(Buf& out, const Acceptance& a) {
    if (a.axes.empty())
        throw CodecError("Acceptance: цена обязана быть расписана по осям (ИР-022)");
    w_map(out, 5 + (a.carried_units ? 1 : 0) + (a.norm ? 1 : 0) + (a.like ? 1 : 0));
    w_uint(out, 0); w_ref(out, a.work);
    w_uint(out, 1); w_text(out, a.quality);
    w_uint(out, 2); w_float64(out, a.hours_raw);
    w_uint(out, 3); w_float64(out, a.labor_units);
    // Цена, расписанная по осям. НЕ опционально — приёмки без разбивки не
    // существует: цена без объяснения это два разных вида записи там, где
    // должен быть один (ИР-022).
    w_uint(out, 4); w_arr(out, a.axes.size());
    for (const auto& x : a.axes) {
        w_map(out, 2);
        w_uint(out, 0); w_block_path(out, x.axis);
        w_uint(out, 1); w_float64(out, x.units);
    }
    if (a.carried_units) { w_uint(out, 5); w_float64(out, *a.carried_units); }
    if (a.norm) {
        w_uint(out, 6);
        w_map(out, 3);
        w_uint(out, 0); w_fixed(out, a.norm->agg);
        w_uint(out, 1); w_int64(out, a.norm->date);
        w_uint(out, 2); w_float64(out, a.norm->W);
    }
    if (a.like) { w_uint(out, 7); w_ref(out, *a.like); }   // ИР-022 шаг 11
}

// What a Transfer must be to exist at all (records.md §11.1 v5, ИР-023) —
// checked on write and on read, so a record that breaks it can be neither
// signed with this codec nor taken in from anyone else's.
void check_transfer(const Transfer& t) {
    if (!t.accept) {
        // Without an acceptance only own paper may move, between own branches
        // (economy.md §5а): hours never go to another chain without accepted
        // labour behind them (records.md §12.9).
        if (t.to != t.from)
            throw CodecError("Transfer: перевод другой цепи без приёмки — беспричинный "
                             "(records.md §12.9)");
        for (const auto& o : t.origins)
            if (o.issuer == t.from)
                throw CodecError("Transfer: между своими ветками своя бумага не ходит");
        return;
    }
    const Acceptance& a = *t.accept;
    if (t.to == t.from)
        throw CodecError("Transfer: самоприёмка ничего не заверяет (records.md §14.4)");
    if (a.work.chain != t.to)
        throw CodecError("Transfer: платят тому, чья работа принята — to ≠ цепь работы");
    double paid = 0.0;
    for (const auto& o : t.origins) paid += o.units;
    const double payable = a.labor_units + (a.carried_units ? *a.carried_units : 0.0);
    if (!settled_exactly(paid, payable))
        throw CodecError("Transfer: оплата обязана ровно равняться приёмке — труд плюс "
                         "перенос (records.md §12.8)");
}

void enc_material(Buf& out, const Material& m) {
    w_map(out, 10);
    w_uint(out, 0); w_uint(out, static_cast<uint8_t>(RecordType::Material));
    w_uint(out, 1); w_text(out, m.name);
    w_uint(out, 2); w_text(out, m.unit);
    w_uint(out, 3); w_text(out, m.desc);
    w_uint(out, 4); w_float64(out, m.cost);
    w_uint(out, 5); w_float64(out, m.qty);
    w_uint(out, 6); w_text(out, m.basis);
    w_uint(out, 7);
    if (m.src) w_ref(out, *m.src); else w_null(out);
    w_uint(out, 8);
    if (m.origin) w_ref(out, *m.origin); else w_null(out);
    w_uint(out, 9); w_text(out, m.note);
}

void enc_tool(Buf& out, const Tool& t) {
    w_map(out, 10);
    w_uint(out, 0); w_uint(out, static_cast<uint8_t>(RecordType::Tool));
    w_uint(out, 1); w_text(out, t.name);
    w_uint(out, 2); w_text(out, t.desc);
    w_uint(out, 3); w_text(out, t.serial);
    w_uint(out, 4); w_float64(out, t.cost);
    w_uint(out, 5); w_float64(out, t.life);
    w_uint(out, 6); w_text(out, t.basis);
    w_uint(out, 7);
    if (t.src) w_ref(out, *t.src); else w_null(out);
    w_uint(out, 8);
    if (t.origin) w_ref(out, *t.origin); else w_null(out);
    w_uint(out, 9); w_text(out, t.note);
}

void enc_transfer(Buf& out, const Transfer& t) {
    // v5 (ИР-023): the acceptance rides inside its payment (key 11); key 5 — the
    // old `reason` → Acceptance — is retired and not reused. Keys stay ascending,
    // so deterministic encoding (RFC 8949 §4.2.1) holds for every combination of
    // the optional tail: emission link (7–9), settled pledge (10), accept (11).
    check_transfer(t);
    w_map(out, 6 + (t.emission ? 3 : 0) + (t.settles ? 1 : 0) + (t.accept ? 1 : 0));
    w_uint(out, 0); w_uint(out, static_cast<uint8_t>(RecordType::Transfer));
    w_uint(out, 1); w_fixed(out, t.from);
    w_uint(out, 2); w_fixed(out, t.to);
    w_uint(out, 3); w_uint(out, t.to_node);
    w_uint(out, 4); w_arr(out, t.origins.size());
    for (const auto& o : t.origins) {
        w_map(out, 2);
        w_uint(out, 0); w_fixed(out, o.issuer);
        w_uint(out, 1); w_float64(out, o.units);
    }
    w_uint(out, 6); w_int64(out, t.timestamp);
    if (t.emission) {
        w_uint(out, 7); w_uint(out, t.emission->seq);
        w_uint(out, 8);
        if (t.emission->prev) w_ref(out, *t.emission->prev); else w_null(out);
        w_uint(out, 9); w_float64(out, t.emission->debt_after);
    }
    if (t.settles) { w_uint(out, 10); w_ref(out, *t.settles); }
    if (t.accept)  { w_uint(out, 11); w_acceptance(out, *t.accept); }
}

void enc_redemption(Buf& out, const Redemption& rd) {
    w_map(out, 7);
    w_uint(out, 0); w_uint(out, static_cast<uint8_t>(RecordType::Redemption));
    w_uint(out, 1); w_ref(out, rd.transfer);
    w_uint(out, 2); w_float64(out, rd.units);
    w_uint(out, 3); w_uint(out, rd.link.seq);
    w_uint(out, 4);
    if (rd.link.prev) w_ref(out, *rd.link.prev); else w_null(out);
    w_uint(out, 5); w_float64(out, rd.link.debt_after);
    w_uint(out, 6); w_int64(out, rd.timestamp);
}

void enc_pledge(Buf& out, const Pledge& p) {
    w_map(out, 6);
    w_uint(out, 0); w_uint(out, static_cast<uint8_t>(RecordType::Pledge));
    w_uint(out, 1); w_ref(out, p.target);
    w_uint(out, 2); w_float64(out, p.units);
    w_uint(out, 3);
    if (p.executor) w_fixed(out, *p.executor); else w_null(out);
    w_uint(out, 4);
    if (p.expires) w_int64(out, *p.expires); else w_null(out);
    w_uint(out, 5); w_int64(out, p.timestamp);
}

void enc_pledge_revoke(Buf& out, const PledgeRevoke& pr) {
    w_map(out, 3);
    w_uint(out, 0); w_uint(out, static_cast<uint8_t>(RecordType::PledgeRevoke));
    w_uint(out, 1); w_ref(out, pr.pledge);
    w_uint(out, 2); w_int64(out, pr.timestamp);
}

void enc_daily_aggregate(Buf& out, const DailyAggregate& d) {
    // v5 (ИР-022 шаг 11): prices of axes, not rates of specialties. Key 2 — the
    // per-specialty rates — is retired; the axes sit under key 6.
    w_map(out, 6);
    w_uint(out, 0); w_uint(out, static_cast<uint8_t>(RecordType::DailyAggregate));
    w_uint(out, 1); w_int64(out, d.date);
    w_uint(out, 3); w_int64(out, d.timestamp);
    w_uint(out, 4); w_float64(out, d.W);
    w_uint(out, 5); w_text(out, d.indep);
    w_uint(out, 6); w_arr(out, d.axes.size());
    for (const auto& p : d.axes) {
        w_map(out, 5);
        w_uint(out, 0); w_block_path(out, p.axis);
        w_uint(out, 1); w_float64(out, p.price);
        w_uint(out, 2); w_float64(out, p.hours);
        w_uint(out, 3); w_uint(out, p.deals);
        w_uint(out, 4); w_float64(out, p.weighted_hours);
    }
}

void enc_work_cloud(Buf& out, const WorkCloud& c) {
    w_map(out, 7);
    w_uint(out, 0); w_uint(out, static_cast<uint8_t>(RecordType::WorkCloud));
    w_uint(out, 1); w_int64(out, c.date);
    w_uint(out, 2); w_fixed(out, c.snapshot);
    w_uint(out, 3); w_text(out, c.params);
    w_uint(out, 4); w_arr(out, c.sources.size());
    for (const auto& s : c.sources) w_fixed(out, s);
    w_uint(out, 5); w_arr(out, c.points.size());
    for (const auto& p : c.points) {
        w_map(out, 4);
        w_uint(out, 0); w_ref(out, p.sample);
        w_uint(out, 1); w_text(out, p.title);
        w_uint(out, 2); w_uint(out, p.deals);
        w_uint(out, 3); w_arr(out, p.neighbors.size());
        for (const auto& n : p.neighbors) {
            w_map(out, 2);
            w_uint(out, 0); w_ref(out, n.sample);
            w_uint(out, 1); w_float64(out, n.weight);
        }
    }
    w_uint(out, 6); w_int64(out, c.timestamp);
}

void enc_axis_def(Buf& out, const AxisDef& a) {
    w_map(out, 5 + (a.parent ? 1 : 0) + (a.same_as ? 1 : 0));
    w_uint(out, 0); w_uint(out, static_cast<uint8_t>(RecordType::AxisDef));
    w_uint(out, 1); w_text(out, a.slug);
    w_uint(out, 2); w_text(out, a.ru);
    w_uint(out, 3); w_text(out, a.description);
    w_uint(out, 4); w_int64(out, a.timestamp);
    if (a.parent)  { w_uint(out, 5); w_ref(out, *a.parent); }
    if (a.same_as) { w_uint(out, 6); w_ref(out, *a.same_as); }
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

    // Raw payload bytes following an already-consumed head.
    void r_raw(uint8_t* out, size_t n) {
        need(n);
        std::memcpy(out, data_ + pos_, n);
        pos_ += n;
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

BlockPath dec_block_path(CborReader& r) {
    if (r.r_map() != 4) throw CodecError("BlockPath: expected 4 fields");
    auto index = [&r]() -> uint32_t {
        const uint64_t v = r.r_uint();
        if (v > 0xFFFF'FFFFu) throw CodecError("BlockPath: index out of range");
        return static_cast<uint32_t>(v);
    };
    BlockPath p{};
    expect_key(r, 0); r.r_fixed(p.chain);
    expect_key(r, 1); p.node  = index();
    expect_key(r, 2); p.block = index();
    expect_key(r, 3); r.r_fixed(p.hash);
    return p;
}

// ── Optional fields: CBOR null (major 7, info 22) or the value itself ─────────

bool is_null_head(const std::pair<uint8_t, uint64_t>& h) {
    return h.first == 7 && h.second == 22;
}

std::optional<Ref> dec_opt_ref(CborReader& r) {
    const auto h = r.read_head();
    if (is_null_head(h)) return std::nullopt;
    if (h.first != 5 || h.second != 2)
        throw CodecError("Ref?: expected map(2) or null");
    Ref ref{};
    expect_key(r, 0); r.r_fixed(ref.chain);
    expect_key(r, 1); r.r_fixed(ref.hash);
    return ref;
}

std::optional<std::array<uint8_t, 32>> dec_opt_bytes32(CborReader& r) {
    const auto h = r.read_head();
    if (is_null_head(h)) return std::nullopt;
    if (h.first != 2 || h.second != 32)
        throw CodecError("Bytes(32)?: expected 32-byte string or null");
    std::array<uint8_t, 32> a{};
    r.r_raw(a.data(), 32);
    return a;
}

std::optional<int64_t> dec_opt_int(CborReader& r) {
    const auto h = r.read_head();
    if (is_null_head(h)) return std::nullopt;
    if (h.first == 0) {
        if (h.second > static_cast<uint64_t>(std::numeric_limits<int64_t>::max()))
            throw CodecError("Int?: uint overflows int64_t");
        return static_cast<int64_t>(h.second);
    }
    if (h.first == 1) {
        if (h.second > static_cast<uint64_t>(std::numeric_limits<int64_t>::max()))
            throw CodecError("Int?: negint underflows int64_t");
        return -1LL - static_cast<int64_t>(h.second);
    }
    throw CodecError("Int?: expected integer or null");
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

CarryEntry dec_carry_entry(CborReader& r) {
    if (r.r_map() != 6) throw CodecError("CarryEntry: expected 6 fields");
    CarryEntry ce{};
    expect_key(r, 0); ce.src     = dec_ref(r);
    expect_key(r, 1); ce.used    = r.r_float64();
    expect_key(r, 2); ce.carried = r.r_float64();
    expect_key(r, 3); ce.seq     = r.r_uint();
    expect_key(r, 4); ce.prev    = dec_opt_ref(r);
    expect_key(r, 5); ce.after   = r.r_float64();
    return ce;
}

WorkRecord dec_work_record_fields(CborReader& r, uint64_t field_count) {
    if (field_count < 6 || field_count > 8)
        throw CodecError("WorkRecord: expected 6..8 fields");
    WorkRecord wr{};
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
    // The optional tail, by key and ascending: carry (7), title (8). Present
    // means non-empty — an empty one is never written, so reading one would mean
    // two encodings of the same record.
    uint64_t last = 6;
    for (uint64_t i = 6; i < field_count; ++i) {
        const uint64_t key = r.r_uint();
        if (key <= last) throw CodecError("WorkRecord: keys out of order");
        last = key;
        if (key == 7) {
            const uint64_t n = r.r_arr();
            if (n == 0) throw CodecError("WorkRecord: carry present but empty");
            wr.carry.reserve(static_cast<size_t>(n));
            for (uint64_t k = 0; k < n; ++k) wr.carry.push_back(dec_carry_entry(r));
        } else if (key == 8) {
            wr.title = r.r_text();
            if (wr.title.empty()) throw CodecError("WorkRecord: title present but empty");
        } else {
            throw CodecError("WorkRecord: unknown field key");
        }
    }
    return wr;
}

Acceptance dec_acceptance(CborReader& r) {
    const uint64_t n = r.r_map();
    if (n < 5 || n > 8) throw CodecError("Acceptance: expected 5..8 fields");
    Acceptance a{};
    expect_key(r, 0); a.work        = dec_ref(r);
    expect_key(r, 1); a.quality     = r.r_text();
    expect_key(r, 2); a.hours_raw   = r.r_float64();
    expect_key(r, 3); a.labor_units = r.r_float64();
    // The breakdown — REQUIRED: an acceptance without one does not exist
    // (records.md §9.5 v4).
    expect_key(r, 4);
    {
        const uint64_t k = r.r_arr();
        a.axes.reserve(static_cast<size_t>(k));
        for (uint64_t i = 0; i < k; ++i) {
            if (r.r_map() != 2) throw CodecError("AcceptanceAxis: expected 2 fields");
            AcceptanceAxis x{};
            expect_key(r, 0); x.axis  = dec_block_path(r);
            expect_key(r, 1); x.units = r.r_float64();
            a.axes.push_back(std::move(x));
        }
    }
    // The tail, by key and ascending: carried_units (5), norm (6), like (7).
    uint64_t last = 4;
    for (uint64_t i = 5; i < n; ++i) {
        const uint64_t key = r.r_uint();
        if (key <= last) throw CodecError("Acceptance: keys out of order");
        last = key;
        if (key == 5) {
            a.carried_units = r.r_float64();
        } else if (key == 6) {
            AcceptanceNorm nm{};
            if (r.r_map() != 3) throw CodecError("AcceptanceNorm: expected 3 fields");
            expect_key(r, 0); r.r_fixed(nm.agg);
            expect_key(r, 1); nm.date = r.r_int();
            expect_key(r, 2); nm.W    = r.r_float64();
            a.norm = nm;
        } else if (key == 7) {
            a.like = dec_ref(r);
        } else {
            throw CodecError("Acceptance: unknown field key");
        }
    }
    if (a.axes.empty())
        throw CodecError("Acceptance: цена обязана быть расписана по осям (ИР-022)");
    return a;
}

Material dec_material_fields(CborReader& r) {
    Material m{};
    expect_key(r, 1); m.name   = r.r_text();
    expect_key(r, 2); m.unit   = r.r_text();
    expect_key(r, 3); m.desc   = r.r_text();
    expect_key(r, 4); m.cost   = r.r_float64();
    expect_key(r, 5); m.qty    = r.r_float64();
    expect_key(r, 6); m.basis  = r.r_text();
    expect_key(r, 7); m.src    = dec_opt_ref(r);
    expect_key(r, 8); m.origin = dec_opt_ref(r);
    expect_key(r, 9); m.note   = r.r_text();
    return m;
}

Tool dec_tool_fields(CborReader& r) {
    Tool t{};
    expect_key(r, 1); t.name   = r.r_text();
    expect_key(r, 2); t.desc   = r.r_text();
    expect_key(r, 3); t.serial = r.r_text();
    expect_key(r, 4); t.cost   = r.r_float64();
    expect_key(r, 5); t.life   = r.r_float64();
    expect_key(r, 6); t.basis  = r.r_text();
    expect_key(r, 7); t.src    = dec_opt_ref(r);
    expect_key(r, 8); t.origin = dec_opt_ref(r);
    expect_key(r, 9); t.note   = r.r_text();
    return t;
}

Transfer dec_transfer_fields(CborReader& r, uint64_t field_count) {
    // v5 (ИР-023): 6 required fields, then an optional ascending tail — emission
    // link (7–9, all three), settled pledge (10), accept (11). Key 5 — the
    // retired `reason` — is refused like any unknown key.
    if (field_count < 6 || field_count > 11)
        throw CodecError("Transfer: expected 6..11 fields");
    Transfer t{};
    expect_key(r, 1); r.r_fixed(t.from);
    expect_key(r, 2); r.r_fixed(t.to);
    expect_key(r, 3);
    {
        const uint64_t v = r.r_uint();
        if (v > 0xFFFF'FFFEull) throw CodecError("Transfer: bad to_node");
        t.to_node = static_cast<uint32_t>(v);
    }
    expect_key(r, 4);
    {
        const uint64_t n = r.r_arr();
        t.origins.reserve(static_cast<size_t>(n));
        for (uint64_t i = 0; i < n; ++i) {
            if (r.r_map() != 2) throw CodecError("OriginQty: expected 2 fields");
            OriginQty o{};
            expect_key(r, 0); r.r_fixed(o.issuer);
            expect_key(r, 1); o.units = r.r_float64();
            t.origins.push_back(o);
        }
    }
    expect_key(r, 6); t.timestamp = r.r_int();
    uint64_t read = 6, last = 6;
    while (read < field_count) {
        const uint64_t key = r.r_uint();
        if (key <= last) throw CodecError("Transfer: keys out of order");
        if (key == 7) {
            if (field_count - read < 3) throw CodecError("Transfer: truncated emission link");
            EmissionLink link{};
            link.seq = r.r_uint();
            expect_key(r, 8); link.prev       = dec_opt_ref(r);
            expect_key(r, 9); link.debt_after = r.r_float64();
            t.emission = link;
            read += 3;
            last  = 9;
        } else if (key == 10) {
            t.settles = dec_ref(r);
            read += 1;
            last  = 10;
        } else if (key == 11) {
            t.accept = dec_acceptance(r);
            read += 1;
            last  = 11;
        } else {
            throw CodecError("Transfer: unknown field key");
        }
    }
    check_transfer(t);
    return t;
}

Redemption dec_redemption_fields(CborReader& r) {
    Redemption rd{};
    expect_key(r, 1); rd.transfer        = dec_ref(r);
    expect_key(r, 2); rd.units           = r.r_float64();
    expect_key(r, 3); rd.link.seq        = r.r_uint();
    expect_key(r, 4); rd.link.prev       = dec_opt_ref(r);
    expect_key(r, 5); rd.link.debt_after = r.r_float64();
    expect_key(r, 6); rd.timestamp       = r.r_int();
    return rd;
}

Pledge dec_pledge_fields(CborReader& r) {
    Pledge p{};
    expect_key(r, 1); p.target    = dec_ref(r);
    expect_key(r, 2); p.units     = r.r_float64();
    expect_key(r, 3); p.executor  = dec_opt_bytes32(r);
    expect_key(r, 4); p.expires   = dec_opt_int(r);
    expect_key(r, 5); p.timestamp = r.r_int();
    return p;
}

PledgeRevoke dec_pledge_revoke_fields(CborReader& r) {
    PledgeRevoke pr{};
    expect_key(r, 1); pr.pledge    = dec_ref(r);
    expect_key(r, 2); pr.timestamp = r.r_int();
    return pr;
}

DailyAggregate dec_daily_aggregate_fields(CborReader& r, uint64_t field_count) {
    // v5 only. Older layouts — rates by specialty, by grade, without W — are
    // refused, not half-read: no backward compatibility (decided 2026-09-11).
    if (field_count != 6) throw CodecError("DailyAggregate: expected 6 fields (v5)");
    DailyAggregate d{};
    expect_key(r, 1); d.date      = r.r_int();
    expect_key(r, 3); d.timestamp = r.r_int();
    expect_key(r, 4); d.W         = r.r_float64();
    expect_key(r, 5); d.indep     = r.r_text();
    expect_key(r, 6);
    {
        const uint64_t n = r.r_arr();
        d.axes.reserve(static_cast<size_t>(n));
        for (uint64_t i = 0; i < n; ++i) {
            if (r.r_map() != 5) throw CodecError("AxisPrice: expected 5 fields");
            AxisPrice p{};
            expect_key(r, 0); p.axis           = dec_block_path(r);
            expect_key(r, 1); p.price          = r.r_float64();
            expect_key(r, 2); p.hours          = r.r_float64();
            expect_key(r, 3); p.deals          = r.r_uint();
            expect_key(r, 4); p.weighted_hours = r.r_float64();
            d.axes.push_back(std::move(p));
        }
    }
    return d;
}

WorkCloud dec_work_cloud_fields(CborReader& r, uint64_t field_count) {
    if (field_count != 7) throw CodecError("WorkCloud: expected 7 fields");
    WorkCloud c{};
    expect_key(r, 1); c.date = r.r_int();
    expect_key(r, 2); r.r_fixed(c.snapshot);
    expect_key(r, 3); c.params = r.r_text();
    expect_key(r, 4);
    {
        const uint64_t n = r.r_arr();
        c.sources.reserve(static_cast<size_t>(n));
        for (uint64_t i = 0; i < n; ++i) {
            std::array<uint8_t, 32> s{};
            r.r_fixed(s);
            c.sources.push_back(s);
        }
    }
    expect_key(r, 5);
    {
        const uint64_t n = r.r_arr();
        c.points.reserve(static_cast<size_t>(n));
        for (uint64_t i = 0; i < n; ++i) {
            if (r.r_map() != 4) throw CodecError("CloudPoint: expected 4 fields");
            CloudPoint p{};
            expect_key(r, 0); p.sample = dec_ref(r);
            expect_key(r, 1); p.title  = r.r_text();
            expect_key(r, 2); p.deals  = r.r_uint();
            expect_key(r, 3);
            const uint64_t m = r.r_arr();
            p.neighbors.reserve(static_cast<size_t>(m));
            for (uint64_t j = 0; j < m; ++j) {
                if (r.r_map() != 2) throw CodecError("CloudNeighbor: expected 2 fields");
                CloudNeighbor nb{};
                expect_key(r, 0); nb.sample = dec_ref(r);
                expect_key(r, 1); nb.weight = r.r_float64();
                p.neighbors.push_back(std::move(nb));
            }
            c.points.push_back(std::move(p));
        }
    }
    expect_key(r, 6); c.timestamp = r.r_int();
    return c;
}

AxisDef dec_axis_def_fields(CborReader& r, uint64_t field_count) {
    AxisDef a{};
    expect_key(r, 1); a.slug        = r.r_text();
    expect_key(r, 2); a.ru          = r.r_text();
    expect_key(r, 3); a.description = r.r_text();
    expect_key(r, 4); a.timestamp   = r.r_int();
    for (uint64_t i = 5; i < field_count; ++i) {
        switch (r.r_uint()) {
            case 5:  a.parent  = dec_ref(r); break;
            case 6:  a.same_as = dec_ref(r); break;
            default: throw CodecError("AxisDef: unknown field key");
        }
    }
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
        else if constexpr (std::is_same_v<T, WorkRecord>)  enc_work_record(out, r);
        else if constexpr (std::is_same_v<T, Material>)    enc_material(out, r);
        else if constexpr (std::is_same_v<T, Tool>)        enc_tool(out, r);
        else if constexpr (std::is_same_v<T, Transfer>)    enc_transfer(out, r);
        else if constexpr (std::is_same_v<T, Pledge>)      enc_pledge(out, r);
        else if constexpr (std::is_same_v<T, PledgeRevoke>) enc_pledge_revoke(out, r);
        else if constexpr (std::is_same_v<T, DailyAggregate>) enc_daily_aggregate(out, r);
        else if constexpr (std::is_same_v<T, Redemption>)  enc_redemption(out, r);
        else if constexpr (std::is_same_v<T, WorkCloud>)      enc_work_cloud(out, r);
        else if constexpr (std::is_same_v<T, AxisDef>)        enc_axis_def(out, r);
    }, rec);
    return out;
}

Record Codec::decode(const uint8_t* data, size_t len) {
    CborReader r(data, len);
    // Field count is owned by per-type decoders; Transfer needs it to know how
    // long its optional tail is (emission, settles, accept).
    const uint64_t field_count = r.r_map();
    expect_key(r, 0);   // type discriminator is always key 0
    const uint8_t disc = r.r_uint8();

    if (disc == 0x54)   // Acceptance — снята (ИР-023), номер не переиспользуется
        throw CodecError("Acceptance (0x54) снята: приёмка — поле перевода "
                         "Transfer.accept (records.md §9.5)");
    switch (static_cast<RecordType>(disc)) {
        case RecordType::FraudClaim:  return dec_fraud_claim_fields(r);
        case RecordType::Concept:     return dec_concept_fields(r);
        case RecordType::ConceptLink: return dec_concept_link_fields(r);
        case RecordType::Composite:   return dec_composite_fields(r);
        case RecordType::Copy:        return dec_copy_fields(r);
        case RecordType::Reaction:    return dec_reaction_fields(r);
        case RecordType::WorkRecord:  return dec_work_record_fields(r, field_count);
        case RecordType::Material:    return dec_material_fields(r);
        case RecordType::Tool:        return dec_tool_fields(r);
        case RecordType::Transfer:    return dec_transfer_fields(r, field_count);
        case RecordType::Pledge:      return dec_pledge_fields(r);
        case RecordType::PledgeRevoke: return dec_pledge_revoke_fields(r);
        case RecordType::DailyAggregate: return dec_daily_aggregate_fields(r, field_count);
        case RecordType::Redemption: return dec_redemption_fields(r);
        case RecordType::WorkCloud:  return dec_work_cloud_fields(r, field_count);
        case RecordType::AxisDef:
            return dec_axis_def_fields(r, field_count);
        default:
            throw CodecError("CBOR: unknown record type discriminator");
    }
}

} // namespace records
