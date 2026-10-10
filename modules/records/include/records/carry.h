#pragma once
// Carry thread of a means of production (ИР-011, records.md §9.4 v2): each
// use of a Tool/Material batch is a link {seq, prev, after} transferring a
// share of the UNRECOVERED cost into a work. Pure computation, mirroring
// credit.h: the caller extracts observed links from whatever blocks it holds;
// a partial view is legitimate and reported as such (`gaps`), never presented
// as the full picture.

#include "records/types.h"

#include <cstdint>
#include <vector>

namespace records {

// ── Две шкалы износа (ИР-011 v5, решение участника 2026-10-10) ───────────────
//
// Вещь изнашивается и работой, и временем, поэтому у записи две шкалы: ресурс
// (`Tool.life` в моточасах / `Material.qty` в единицах) и календарный срок
// (`days`, со дня записи). **Износ считается по той шкале, что дала больше** —
// отсюда и то, что простой перестаёт быть бесплатным, и безопасность самой
// шкалы: добавить её можно только себе в убыток, остаток она лишь снижает.

// Доля календарного срока, прошедшая к моменту `at` (0..1). Без срока — 0.
double calendar_share(uint32_t days, int64_t started_at, int64_t at) noexcept;

// Остаток к возмещению на момент `at`: стоимость минус БОЛЬШИЙ из двух
// износов — собранного переносом и календарного. Это потолок переноса, цена
// перепродажи и потолок перевыпуска (economy.md §5б, records.md §10.2).
double residual_value(double cost, double collected,
                      uint32_t days, int64_t started_at, int64_t at) noexcept;

// The recognised transfer for one use (records.md §9.4): the share of the
// unrecovered cost, forcibly zero once the asset has recovered in full.
//   carried = min(used / capacity × cost, cost − collected_before)
// capacity: Tool.life (tool-hours) or Material.qty (batch size).
double carry_step(double cost, double capacity, double used,
                  double collected_before) noexcept;

// То же с календарной шкалой: потолок — не «стоимость минус собранное», а
// остаток по БОЛЬШЕМУ износу на момент работы. Простоявшая вещь переносит
// меньше, а вещь с истёкшим сроком — ничего: переносить уже нечего.
double carry_step(double cost, double capacity, double used,
                  double collected_before,
                  uint32_t days, int64_t started_at, int64_t at) noexcept;

// One carry link as seen in a WorkRecord block of the owner's chain.
struct ObservedCarry {
    CarryEntry              entry;
    // Момент использования — КОНЕЦ работы (`labor_interval(wr).second`): по
    // нему норма считает календарный износ (ИР-011 v5). Писать сюда начало
    // работы нельзя — потолок разойдётся с тем, что считал автор звена.
    int64_t                 timestamp = 0;
    std::array<uint8_t, 32> block_hash{};   // carrier block (equivocation evidence)
};

struct CarryHistory {
    // Total collected at the newest seen link; remainder = cost − collected
    // is the bookkeeping residual value (economy.md §5б) and the resale/
    // reissue ceiling (records.md §10.2).
    double collected = 0;

    uint64_t links_seen     = 0;      // unique seq values observed
    uint64_t links_expected = 0;      // newest seq + 1
    bool     gaps           = false;  // partial local view — honesty first

    // Objective per-link violations — checkable regardless of gaps, each one
    // alone makes the link unrecognisable:
    bool formula_mismatch = false;  // carried ≠ carry_step(...) at some link
    bool over_invariant   = false;  // after > cost at some link (money pump)
    bool after_decreasing = false;  // collected shrinks along seq

    // Set only between ADJACENT seqs actually seen (k, k+1): a break in
    // after_k + carried_{k+1} == after_{k+1} is objective there; across a gap
    // it would slander links we simply have not received.
    bool thread_inconsistent = false;

    // seq values carried by two different blocks — double-charging one asset
    // over parallel branches; objective equivocation proof.
    std::vector<uint64_t> equivocated_seqs;
};

// Digest observed links of ONE asset record (any order; the same block seen
// twice is tolerated). cost/capacity come from the Tool/Material record.
// days/started_at — календарная шкала записи (0 — её нет); каждое звено
// проверяется потолком НА СВОЁ ВРЕМЯ, поэтому задним числом норма не
// переписывается: что было законно тогда, остаётся законным.
CarryHistory carry_history(std::vector<ObservedCarry> links,
                           double cost, double capacity,
                           uint32_t days = 0, int64_t started_at = 0);

} // namespace records
