#pragma once

// Кому сколько ресурса вещи принадлежит (ИР-013, решение участника 2026-10-06:
// доля — не дробь, а КУПЛЕННАЯ ПОРЦИЯ РЕСУРСА).
//
// Новых записей для этого не понадобилось. Вещь остаётся записью одной цепи —
// того, у кого она физически, — а доля выражается машино-часами, проданными
// вперёд (records.md §9.4 v8): трое скинулись на печь, значит каждый купил
// свою часть её ресурса и работает в её пределах. Отсюда и всё остальное:
// амортизацию никто не распределяет (каждый возместил своё, продав или
// истратив свои часы), согласие на смену не нужно (в своих часах не
// спрашивают никого), а перепродать больше купленного не даст подниточка
// покупки (records.md §9.8).
//
// Эта витрина — производная: считается сканом по сырым записям и
// перепроверяема по цепям, как всё у агрегатора (records.md §13).

#include "recognition.h"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace aggregator {

// Один дольщик: сколько моточасов купил, за сколько, сколько истратил.
struct ResourceHolder {
    UserId                  chain{};
    double                  hours = 0;   // куплено моточасов
    double                  paid  = 0;   // уплачено за них — ровно износ
    double                  spent = 0;   // израсходовано в свои работы
    double                  left  = 0;   // осталось на руках
    std::array<uint8_t, 32> deal{};      // последняя сделка, которой куплено
};

struct ResourceSplit {
    bool        known = false;    // запись вещи нашлась
    UserId      keeper{};         // в чьей цепи запись — у того вещь физически
    std::string name;
    double      cost = 0;
    double      life = 0;         // весь ресурс, моточасов
    uint32_t    days = 0;         // календарный срок, если назван (ИР-011 v5)
    double      collected = 0;    // собрано нитью переноса
    double      residual = 0;     // остаток по большей из двух шкал
    double      sold_hours = 0;   // продано моточасов (оплаченными сделками)
    double      keeper_used = 0;  // израсходовано самим держателем в своих работах
    double      pending_hours = 0;// отпущено и ещё не оплачено
    double      free_hours = 0;   // ресурс, которым никто не распорядился
    bool        over_used = false;// распоряжались большим ресурсом, чем есть
    std::vector<ResourceHolder> holders;   // по убыванию купленных часов
};

class ResourceView {
public:
    ResourceView() = delete;

    // `now` — момент, на который считается календарный износ.
    static ResourceSplit build(const AggregatorStorage& storage,
                               const UserId& chain,
                               const std::array<uint8_t, 32>& tool,
                               int64_t now);
};

} // namespace aggregator
