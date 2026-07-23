#!/usr/bin/env bash
# ─────────────────────────────────────────────────────────────────────────────
# Демо проекта: полный круг «средства производства → труд → расчёт → доверие».
#
# Показывает вживую то, что в спецификации описано словами:
#   • ИР-011 — печь и мука переносят свою стоимость в цену хлеба (амортизация);
#   • строгая эквивалентность — оплата ровно = живой труд + перенос, не больше;
#   • §11.2 — в статистику ставок входит только живой труд, перенос её не двигает;
#   • ИР-010 — экономический след: слой 1 (кредитная история из нити эмиссии) и
#     слои 2–3 (кто держит бумагу цепи, замкнутость оборота).
#
# Самодостаточно: поднимает свой агрегатор на отдельном порту, работает во
# временной папке, всё за собой убирает. Ничего в системе не трогает.
#
# Запуск:   docs/pilot/demo.sh            (собери проект заранее: cmake --build build)
#           BUILD=/path/to/build docs/pilot/demo.sh
#
# Скрипт также годится как дымовой тест: в конце проверяет ключевые числа и
# падает с ненулевым кодом, если что-то разошлось.
# ─────────────────────────────────────────────────────────────────────────────
set -euo pipefail

# ── Расположение бинарей ─────────────────────────────────────────────────────
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD="${BUILD:-$here/../../build}"
BC="$BUILD/modules/cli/bc"
AGG="$BUILD/modules/aggregator/aggregator_server"
for bin in "$BC" "$AGG"; do
    [[ -x "$bin" ]] || { echo "не найден $bin — соберите: cmake --build \"$BUILD\"" >&2; exit 1; }
done
CATALOGS="$here/../catalogs"
PORT="${PORT:-19099}"
VIA="http://localhost:$PORT"

# ── Временная песочница и уборка ─────────────────────────────────────────────
work="$(mktemp -d)"
agg_pid=""
cleanup() {
    [[ -n "$agg_pid" ]] && kill "$agg_pid" 2>/dev/null || true
    rm -rf "$work"
}
trap cleanup EXIT

hr()  { printf '\n\033[1m── %s ──\033[0m\n' "$1"; }
say() { printf '   %s\n' "$*"; }

# Извлечь "chain/hash" или "hash" из вывода команды по метке.
after() { sed -n "s/.*$1//p" | head -1; }

# ── Поднять агрегатор ────────────────────────────────────────────────────────
hr "агрегатор на $VIA"
"$AGG" --port "$PORT" --db "$work/agg" --catalog "$CATALOGS" >"$work/agg.log" 2>&1 &
agg_pid=$!
for _ in $(seq 1 20); do
    curl -sf -o /dev/null "$VIA/catalog" && break || sleep 0.25
done
say "поднят (pid $agg_pid)"

# ── Действующие лица ─────────────────────────────────────────────────────────
hr "личности: Анна (пекарь) и Вера (заказчик)"
"$BC" --data-dir "$work/anna" identity create >/dev/null
"$BC" --data-dir "$work/vera" identity create >/dev/null
ANNA="$("$BC" --data-dir "$work/anna" identity show | after 'User ID: ')"
VERA="$("$BC" --data-dir "$work/vera" identity show | after 'User ID: ')"
say "Анна: ${ANNA:0:16}…"
say "Вера: ${VERA:0:16}…"

# ── Анна: специальность, разряд, средства производства ───────────────────────
hr "Анна регистрирует себя и свои средства производства"
SPEC="$("$BC" --data-dir "$work/anna" specialty add prof.cook --via "$VIA" | after 'hash: ')"
GRADE="$("$BC" --data-dir "$work/anna" grade add "$ANNA/$SPEC" 4 | after 'hash: ')"
say "специальность prof.cook, разряд 4"

# Печь: оценка владельца (est) 315ч, ресурс 8000 моточасов.
TOOL="$("$BC" --data-dir "$work/anna" tool add "Печь подовая" \
        --cost 315 --life 8000 --serial PP-2019-0417 \
        --note "120000руб / 380руб-ч ≈ 315ч" --via "$VIA" | after 'tool ref: ')"
TOOL_H="${TOOL#*/}"
say "печь: 315ч / ресурс 8000ч  (${TOOL_H:0:12}…)"

# Мука: партия 50 кг за 4ч, расходуется по килограммам.
MAT="$("$BC" --data-dir "$work/anna" material add "Мука пшеничная" \
       --unit кг --cost 4 --qty 50 --note "2100руб / 525руб-ч = 4ч" \
       --via "$VIA" | after 'material ref: ')"
MAT_H="${MAT#*/}"
say "мука:  4ч / партия 50 кг  (${MAT_H:0:12}…)"

# ── Анна печёт: печь 6ч + мука 12 кг переносят стоимость в хлеб ───────────────
hr "Анна печёт 6ч — печь 6ч, мука 12кг переносят стоимость"
WORK_H="$("$BC" --data-dir "$work/anna" work log --agent "$ANNA/$GRADE" \
          --action "выпечка партии хлеба" --hours 6 --output "хлеб:24:шт" \
          --tool "${TOOL_H:0:12}:6" --material "${MAT_H:0:12}:12" \
          --via "$VIA" | after 'hash: ')"
say "печь → 6/8000 × 315 = 0.23625ч"
say "мука → 12/50 × 4     = 0.96ч"

hr "аудит нити переноса печи (bc tool show)"
"$BC" --data-dir "$work/anna" tool show "${TOOL_H:0:12}"

# ── Вера принимает и платит ──────────────────────────────────────────────────
hr "Вера принимает работу — приёмка суммирует труд и перенос"
"$BC" --data-dir "$work/vera" fetch "$ANNA/$WORK_H" --via "$VIA" >/dev/null
"$BC" --data-dir "$work/vera" fetch "$ANNA/$TOOL_H"  --via "$VIA" >/dev/null
"$BC" --data-dir "$work/vera" fetch "$ANNA/$MAT_H"   --via "$VIA" >/dev/null
ACC="$("$BC" --data-dir "$work/vera" accept --work "$ANNA/$WORK_H" \
       --quality "пройдено" --coef 1.0 --via "$VIA" | after 'acceptance ref: ')"

hr "Вера платит — потолок = труд 6ч + печь 0.236ч + мука 0.96ч = 7.19625ч"
pay_out="$("$BC" --data-dir "$work/vera" pay --acceptance "$ACC" --via "$VIA" 2>&1)"
echo "$pay_out" | sed 's/^/   /'

# ── Взгляд на экономический след Анны ────────────────────────────────────────
hr "Вера смотрит на след Анны (bc trust): слой 1 + слои 2–3"
"$BC" --data-dir "$work/vera" trust "$ANNA" --via "$VIA"

# ── Проверки (дымовой тест) ──────────────────────────────────────────────────
hr "проверки"
fail=0
check() { # описание, факт, ожидание
    if [[ "$2" == "$3" ]]; then printf '   ✓ %s\n' "$1"
    else printf '   ✗ %s: получили «%s», ждали «%s»\n' "$1" "$2" "$3"; fail=1; fi
}
paid="$(echo "$pay_out" | grep -o 'paid [0-9.]*' | head -1 | awk '{print $2}')"
check "оплата ровно по потолку labor+carried" "$paid" "7.19625"

# Повторная оплата обязана отклониться (потолок уже исчерпан).
if "$BC" --data-dir "$work/vera" pay --acceptance "$ACC" --via "$VIA" >/dev/null 2>&1; then
    echo "   ✗ повторная оплата прошла, а должна была отклониться"; fail=1
else
    echo "   ✓ повторная оплата отклонена (потолок §12.8)"
fi

# Переоценка печи ВВЕРХ обязана отклониться (перевыпуск только вниз).
if "$BC" --data-dir "$work/anna" tool add "Печь подовая" --cost 400 --life 8000 \
        --origin "$TOOL" --note "хочу дороже" >/dev/null 2>&1; then
    echo "   ✗ переоценка вверх прошла, а должна была отклониться"; fail=1
else
    echo "   ✓ переоценка вверх отклонена (перевыпуск только вниз, §10.2)"
fi

hr "итог"
if [[ "$fail" == 0 ]]; then
    echo "   всё сошлось."
else
    echo "   ЕСТЬ РАСХОЖДЕНИЯ (см. выше)"; exit 1
fi
