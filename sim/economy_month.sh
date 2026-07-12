#!/usr/bin/env bash
# =============================================================================
#  economy_month.sh — расширенная симуляция экономики блокчейн-сообщества.
#
#  Пять человек разных специальностей живут и работают "месяц" (NDAYS дней),
#  производят продукцию, обменивают её на эквивалент затраченных трудо-часов,
#  эмитируют и погашают именные трудо-часы (взаимный кредит, economy.md §4.1),
#  свидетельствуют труд друг друга (пломбы, §3), сливают цепи (merge → discovery),
#  финансируют общую идею (pledges → funding board) и ротируют ключ (revocation).
#
#  Симуляция гоняет НАСТОЯЩИЕ бинарники проекта:
#     bc                — персональная цепь каждого человека (свой --data-dir)
#     aggregator_server — общий релей (публикация блоков, ставки, discovery, экономика)
#
#  Время управляется через libfaketime у клиентов (агрегатор — реальное время):
#  так у каждой сделки честный timestamp своего дня, и дневные ставки труда
#  (records.md §11.2, economy.md §2а: ставка = усреднение вчерашних сделок)
#  формируются реальным кодом build_daily_rates.
#
#  Итог: RESULTS/ledger.csv (все сделки), RESULTS/rates.csv (эволюция ставок
#  по дням), RESULTS/summary.txt (финальные досье, инвариант долга, доска идей,
#  граф discovery). Печатается сводная статистика.
# =============================================================================
set -u

# ── Пути и конфиг ────────────────────────────────────────────────────────────
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BC="${BC:-$ROOT/build/modules/cli/bc}"
AGG="${AGG:-$ROOT/build/modules/aggregator/aggregator_server}"
FT="${FT:-/usr/lib/x86_64-linux-gnu/faketime/libfaketime.so.1}"
PORT="${PORT:-8080}"
VIA="http://localhost:$PORT"
NDAYS="${NDAYS:-30}"
START="${START:-2026-01-01}"          # день 0
WORK="${WORK:-/tmp/economy_sim}"      # рабочая директория симуляции
RESULTS="$WORK/results"
LEDGER="$RESULTS/ledger.csv"
RATES="$RESULTS/rates.csv"
SUMMARY="$RESULTS/summary.txt"
EVENTS="$RESULTS/events.log"

for bin in "$BC" "$AGG"; do
  [ -x "$bin" ] || { echo "НЕ НАЙДЕН бинарник: $bin — сначала соберите проект (cmake --build build)"; exit 1; }
done
[ -f "$FT" ] || { echo "НЕ НАЙДЕН libfaketime: $FT (apt-get install -y libfaketime)"; exit 1; }

rm -rf "$WORK"; mkdir -p "$RESULTS"
: > "$EVENTS"
echo "day,worker,specialty,level,buyer,product,hours,coef(rate_yday),k,appraisal_labor_h" > "$LEDGER"
echo "day,date,specialty,level,rate,realized,hours,deals" > "$RATES"

log()   { echo "[$(printf 'д%02d' "${CURDAY:-0}")] $*"; echo "[day ${CURDAY:-0}] $*" >> "$EVENTS"; }

# ── Управление агрегатором (безопасный kill: comm, НЕ pkill -f) ───────────────
kill_aggs() { for p in $(pgrep -x aggregator_serv 2>/dev/null); do kill "$p" 2>/dev/null; done; }

# ── Обёртки ──────────────────────────────────────────────────────────────────
# bcd DATE DATADIR ARGS... — запустить bc за конкретный день (faketime).
bcd() { FAKETIME="@$1 12:00:00" LD_PRELOAD="$FT" "$BC" --data-dir "$2" "${@:3}"; }

day_date()  { date -u -d "$START +$1 days" +%Y-%m-%d; }
day_epoch() { date -u -d "$(day_date "$1") 12:00:00" +%s; }
hexref()    { grep -oE '[0-9a-f]{64}' | head -1; }   # первый 64-hex из вывода

# ── Действующие лица ──────────────────────────────────────────────────────────
PEOPLE=(anna boris vera gleb daria)
declare -A FULL SPEC LEV DIR ID GRADE
FULL[anna]="Анна";  SPEC[anna]="Фермер";  LEV[anna]=3
FULL[boris]="Борис"; SPEC[boris]="Пекарь"; LEV[boris]=3
FULL[vera]="Вера";  SPEC[vera]="Плотник"; LEV[vera]=4
FULL[gleb]="Глеб";  SPEC[gleb]="Механик"; LEV[gleb]=2
FULL[daria]="Дарья"; SPEC[daria]="Врач";  LEV[daria]=5
for p in "${PEOPLE[@]}"; do DIR[$p]="$WORK/$p"; done

# k — личный коэффициент сделки (economy.md §2а): заявка "мой час дороже/дешевле
# среднего". Детерминирован (воспроизводимо), с лёгким структурным трендом на
# специальность + шум. Ставка следующего дня = усреднение сегодняшних (coef*k).
kval() { # name day
  awk -v n="$1" -v d="$2" -v N="$NDAYS" 'BEGIN{
    split("anna boris vera gleb daria", nm, " ");
    idx=0; for(i=1;i<=5;i++) if(nm[i]==n) idx=i;
    drift=0;
    if(n=="daria") drift=0.006;        # дефицитная экспертиза дорожает
    else if(n=="vera")  drift=0.0030;  # мастеровитость слегка растёт
    else if(n=="boris") drift=-0.0025; # хлеб — конкурентный стейпл, дешевеет
    else if(n=="gleb")  drift=0.0;     # рост через повышение разряда, не через k
    seas=(n=="anna")? 0.03*sin(6.28318*d/N):0;   # сезонность у фермера
    seed=(d*7 + idx*13) % 101;
    noise=((seed/101.0)-0.5)*0.05;
    k=1.0 + drift*d + seas + noise;
    if(k<0.90)k=0.90; if(k>1.15)k=1.15;
    printf("%.4f", k);
  }'
}

# Сетевая ставка (стч/час) хранится персистентно per (специальность, разряд):
# CURRATE — сглаженная ставка "на сегодня", как в реальном build_daily_rates
# (перенос вчерашней + EMA-сглаживание alpha). Оценка сделки берёт её как базу.
declare -A CURRATE
ALPHA="${ALPHA:-0.30}"

# После дня: пересчитать ставки из РЕАЛЬНЫХ сделок дня (preview-эндпойнт агрегатора,
# честный build_daily_rates) и сгладить в CURRATE. Специальности без сделок дня
# сохраняют вчерашнюю ставку (перенос). Пишем в rates.csv.
update_rates() { # day
  local day="$1" je; je=$(day_epoch "$day")
  local json; json=$(curl -s "$VIA/economy/rates?day=$je")
  local entry spec lev realized hrs deals key cur new
  while IFS= read -r entry; do
    [ -z "$entry" ] && continue
    spec=$(sed -E 's/.*"specialty":"([^"]*)".*/\1/' <<<"$entry")
    lev=$(sed  -E 's/.*"level":([0-9]+).*/\1/'      <<<"$entry")
    realized=$(sed -E 's/.*"rate":([0-9.]+).*/\1/'  <<<"$entry")
    hrs=$(sed  -E 's/.*"hours":([0-9.]+).*/\1/'     <<<"$entry")
    deals=$(sed -E 's/.*"deals":([0-9]+).*/\1/'     <<<"$entry")
    [ "${deals:-0}" -eq 0 ] && continue
    key="$spec|$lev"
    cur="${CURRATE[$key]:-$realized}"
    new=$(awk -v a="$ALPHA" -v r="$realized" -v c="$cur" 'BEGIN{printf "%.4f", a*r+(1-a)*c}')
    CURRATE[$key]="$new"
    echo "$day,$(day_date "$day"),$spec,$lev,$new,$realized,$hrs,$deals" >> "$RATES"
  done < <(echo "$json" | grep -oE '\{"specialty":"[^"]*","level":[0-9]+,"rate":[0-9.]+,"hours":[0-9.]+,"deals":[0-9]+\}')
}

# ── Одна полная сделка: труд → приёмка → оплата → приём (двусторонняя запись) ──
# deal DAY WORKER BUYER PRODUCT HOURS [WITNESS]
deal() {
  local day="$1" w="$2" b="$3" prod="$4" hours="$5" witness="${6:-}"
  local dfmt; dfmt=$(day_date "$day")
  # база оценки — вчерашняя сетевая ставка этой (специальности, разряда); §2а
  local coef; coef="${CURRATE[${SPEC[$w]}|${LEV[$w]}]:-}"
  [ -z "$coef" ] && coef=1.0
  local k; k=$(kval "$w" "$day")
  local ptoken="${prod// /_}_SN${day}"          # серийник как внешний якорь (§2)

  # 1) работник логирует труд и публикует блок
  local wh; wh=$(bcd "$dfmt" "${DIR[$w]}" work log \
      --agent "${GRADE[$w]}" --action "$prod" --hours "$hours" \
      --output "${ptoken}:1:ед" --via "$VIA" 2>/dev/null | hexref)
  [ -z "$wh" ] && { log "!! work log провалился: $w/$prod"; return 1; }
  local wref="${ID[$w]}/$wh"

  # 1a) свидетель пломбирует work-запись (позитивное заверение, §3)
  if [ -n "$witness" ]; then
    bcd "$dfmt" "${DIR[$witness]}" seal add "$wh" --via "$VIA" >/dev/null 2>&1 \
      && log "пломба: ${FULL[$witness]} заверил труд ${FULL[$w]} ($prod)"
  fi

  # 2) покупатель забирает запись, оценивает (ставка_вчера × k × часы) и платит
  bcd "$dfmt" "${DIR[$b]}" fetch "$wref" --via "$VIA" >/dev/null 2>&1
  local acc; acc=$(bcd "$dfmt" "${DIR[$b]}" accept --work "$wref" \
      --quality "принято" --coef "$coef" --k "$k" --via "$VIA" 2>/dev/null \
      | grep -oE 'acceptance ref: .*' | awk '{print $3}')
  [ -z "$acc" ] && { log "!! accept провалился: $b←$w"; return 1; }
  local tref; tref=$(bcd "$dfmt" "${DIR[$b]}" pay --acceptance "$acc" --via "$VIA" 2>/dev/null \
      | grep -oE 'transfer ref: [0-9a-f]{64}/[0-9a-f]{64}' | awk '{print $3}')
  [ -z "$tref" ] && { log "!! pay провалился: $b←$w"; return 1; }

  # 3) работник принимает перевод (Copy-ack; если вернулась своя бумага — Redemption)
  bcd "$dfmt" "${DIR[$w]}" transfer recv "$tref" --via "$VIA" >/dev/null 2>&1

  local appr; appr=$(awk -v h="$hours" -v c="$coef" -v k="$k" 'BEGIN{printf "%.3f", h*c*k}')
  echo "$day,$w,${SPEC[$w]},${LEV[$w]},$b,$prod,$hours,$coef,$k,$appr" >> "$LEDGER"
  return 0
}

# =============================================================================
#  ЗАПУСК
# =============================================================================
echo "════════════════════════════════════════════════════════════════════"
echo " СИМУЛЯЦИЯ ЭКОНОМИКИ СООБЩЕСТВА — $NDAYS дней, ${#PEOPLE[@]} человека"
echo " старт: $START   агрегатор: $VIA"
echo "════════════════════════════════════════════════════════════════════"

kill_aggs
"$AGG" --port "$PORT" --db "$WORK/agg" > "$WORK/agg.log" 2>&1 &
AGGPID=$!
for i in $(seq 1 80); do curl -s "$VIA/stats" >/dev/null 2>&1 && break; done
curl -s "$VIA/stats" >/dev/null 2>&1 || { echo "агрегатор не поднялся"; cat "$WORK/agg.log"; exit 1; }
echo "агрегатор поднят (pid $AGGPID)"

# ── День 0: идентичности, специальности, разряды ─────────────────────────────
CURDAY=0
D0=$(day_date 0)
log "── Основание сообщества: идентичности и специальности ──"
for p in "${PEOPLE[@]}"; do
  bcd "$D0" "${DIR[$p]}" identity create >/dev/null 2>&1
  ID[$p]=$(bcd "$D0" "${DIR[$p]}" identity show 2>/dev/null | hexref)
  local_spec=$(bcd "$D0" "${DIR[$p]}" specialty add "${SPEC[$p]}" --via "$VIA" 2>/dev/null | hexref)
  GRADE[$p]=$(bcd "$D0" "${DIR[$p]}" grade add "${ID[$p]}/$local_spec" "${LEV[$p]}" --via "$VIA" 2>/dev/null | hexref)
  GRADE[$p]="${ID[$p]}/${GRADE[$p]}"
  log "${FULL[$p]} — ${SPEC[$p]} (разряд ${LEV[$p]})  id=${ID[$p]:0:12}…"
done

# ── Ежедневная жизнь ─────────────────────────────────────────────────────────
IDEA_REF=""; declare -A PLEDGE
for ((day=1; day<NDAYS; day++)); do
  CURDAY=$day
  dow=$((day % 7))

  # Стейпл: пекарю нужно зерно (Борис ← Анна); хлеб покупает по очереди каждый.
  deal "$day" anna boris "зерно" 2
  bread_buyer=${PEOPLE[$((day % 5))]}
  [ "$bread_buyer" = "boris" ] && bread_buyer="daria"
  deal "$day" boris "$bread_buyer" "хлеб" 3

  # Через день — плотник строит/чинит для клиента по очереди.
  if (( day % 2 == 0 )); then
    carp_buyer=${PEOPLE[$(((day/2) % 5))]}
    [ "$carp_buyer" = "vera" ] && carp_buyer="gleb"
    deal "$day" vera "$carp_buyer" "мебель" 5 gleb   # Глеб рядом — пломбирует
  fi

  # Каждый 3-й день — механик обслуживает технику (ферма/мастерская).
  if (( day % 3 == 0 )); then
    mech_buyer=$([ $((day % 2)) -eq 0 ] && echo anna || echo vera)
    deal "$day" gleb "$mech_buyer" "ремонт_техники" 4 "$mech_buyer"
  fi

  # Каждый 4-й день — врач лечит пациента (чистая услуга, без продукции).
  if (( day % 4 == 0 )); then
    patient=${PEOPLE[$(( (day/4 + 1) % 5 ))]}
    [ "$patient" = "daria" ] && patient="boris"
    deal "$day" daria "$patient" "лечение" 2
  fi

  # ── Событие: доска идей (день 2) — общая мельница ──
  if (( day == 2 )); then
    log "── Дарья предлагает идею: «Построить общую мельницу» ──"
    ih=$(bcd "$(day_date "$day")" "${DIR[daria]}" concept add "Построить общую мельницу" \
         --tag инфраструктура --via "$VIA" 2>/dev/null | hexref)
    IDEA_REF="${ID[daria]}/$ih"
    # Пледжи (взносы трудо-часов). anna/boris — бессрочные (погасят), vera — отзовёт,
    # gleb — с истёкшим сроком (демонстрация всех статусов пледжа).
    PLEDGE[anna]=$(bcd "$(day_date "$day")" "${DIR[anna]}" pledge add --target "$IDEA_REF" --units 20 --via "$VIA" 2>/dev/null | grep -oE 'pledge ref: .*' | awk '{print $3}')
    PLEDGE[boris]=$(bcd "$(day_date "$day")" "${DIR[boris]}" pledge add --target "$IDEA_REF" --units 15 --via "$VIA" 2>/dev/null | grep -oE 'pledge ref: .*' | awk '{print $3}')
    PLEDGE[vera]=$(bcd "$(day_date "$day")" "${DIR[vera]}" pledge add --target "$IDEA_REF" --units 10 --via "$VIA" 2>/dev/null | grep -oE 'pledge ref: .*' | awk '{print $3}')
    PLEDGE[gleb]=$(bcd "$(day_date "$day")" "${DIR[gleb]}" pledge add --target "$IDEA_REF" --units 8 --expires "$(day_epoch 12)" --via "$VIA" 2>/dev/null | grep -oE 'pledge ref: .*' | awk '{print $3}')
    # Реакции и копии — «вес идеи».
    bcd "$(day_date "$day")" "${DIR[gleb]}"  react "$ih" --value 40 --chain "${ID[daria]}" --via "$VIA" >/dev/null 2>&1
    bcd "$(day_date "$day")" "${DIR[anna]}"  react "$ih" --value 30 --chain "${ID[daria]}" --via "$VIA" >/dev/null 2>&1
    bcd "$(day_date "$day")" "${DIR[boris]}" copy "$IDEA_REF" --via "$VIA" >/dev/null 2>&1
    log "пледжи: Анна 20ч, Борис 15ч, Вера 10ч, Глеб 8ч(срочный); реакции +70; 1 копия"
  fi

  # ── Событие: недельный merge пары цепей (накопленная заверенная история) ──
  if (( dow == 0 )); then
    a=${PEOPLE[$(( (day/7 - 1) % 5 ))]}; b=${PEOPLE[$(( (day/7) % 5 ))]}
    if [ "$a" != "$b" ]; then
      dfmt=$(day_date "$day")
      bcd "$dfmt" "${DIR[$b]}" merge serve --via "$VIA" --once --timeout 25 >/dev/null 2>&1 &
      sp=$!
      bcd "$dfmt" "${DIR[$a]}" merge run --peer "${ID[$b]}" --via "$VIA" --timeout 25 >/dev/null 2>&1
      wait $sp 2>/dev/null
      log "merge: цепи ${FULL[$a]} ⇄ ${FULL[$b]} слиты (→ discovery)"
    fi
  fi

  # ── Событие: повышение разряда механика (день 15): 2 → 3 по лестнице ──
  if (( day == 15 )); then
    dfmt=$(day_date "$day")
    # экзамен принимает цепь с этим/высшим разрядом — здесь просто новый Grade-блок
    spec_g=$(bcd "$dfmt" "${DIR[gleb]}" list 2>/dev/null | grep -i Specialty | head -1 | grep -oE 'hash:[0-9a-f]{64}' | cut -d: -f2)
    ng=$(bcd "$dfmt" "${DIR[gleb]}" grade add "${ID[gleb]}/$spec_g" 3 --via "$VIA" 2>/dev/null | hexref)
    GRADE[gleb]="${ID[gleb]}/$ng"; LEV[gleb]=3
    log "── Глеб повышен: Механик разряд 2 → 3 (лестница разрядов, §2а) ──"
  fi

  # ── Событие: исполнение идеи и расчёт по пледжам (день 18) ──
  if (( day == 18 )) && [ -n "$IDEA_REF" ]; then
    dfmt=$(day_date "$day")
    log "── Глеб строит механизм мельницы; спонсоры гасят пледжи ──"
    # Спонсоры переводят обещанные часы исполнителю (Глебу), reason = их пледж.
    for sp in anna boris; do
      [ -n "${PLEDGE[$sp]:-}" ] || continue
      units=$([ "$sp" = anna ] && echo 20 || echo 15)
      tr=$(bcd "$dfmt" "${DIR[$sp]}" transfer send --to "${ID[gleb]}" --units "$units" \
           --reason "${PLEDGE[$sp]}" --via "$VIA" 2>/dev/null \
           | grep -oE 'transfer ref: [0-9a-f]{64}/[0-9a-f]{64}' | awk '{print $3}')
      [ -n "$tr" ] && bcd "$dfmt" "${DIR[gleb]}" transfer recv "$tr" --via "$VIA" >/dev/null 2>&1
    done
    # Вера отзывает неисполненный остаток своего пледжа.
    bcd "$dfmt" "${DIR[vera]}" pledge revoke --pledge "${PLEDGE[vera]}" --via "$VIA" >/dev/null 2>&1
    log "пледжи погашены: Анна 20ч, Борис 15ч → Глебу; Вера отозвала 10ч; Глеб(8ч) истёк"
  fi

  # Пересчёт сетевых ставок из реальных сделок дня (перенос + EMA-сглаживание).
  update_rates "$day"
done

echo
echo "симуляция завершена — сбор статистики…"
# Статистика снимается ДО ротации ключа: revoke --replace основного кошелька
# ломает локальный подсчёт compute_wallet (local_branches даёт ветку дважды),
# поэтому экономические цифры читаем на чистом состоянии.
"$ROOT/sim/collect_stats.sh" "$WORK" "$VIA" "${PEOPLE[*]}" \
  "$(for p in "${PEOPLE[@]}"; do echo -n "$p:${FULL[$p]}:${SPEC[$p]}:${LEV[$p]}:${ID[$p]}:${DIR[$p]} "; done)" \
  | tee "$SUMMARY"

# ── Эпилог-демонстрация: ротация ключа (revocation §6.7) ─────────────────────
CURDAY=$NDAYS
DL=$(day_date $((NDAYS-1)))
log "── Эпилог: Глеб подозревает компрометацию устройства → ротация ключа ──"
CERT="$RESULTS/gleb_revocation.cbor"
{
  echo "### БЫЛО (до отзыва) — кошелёк Глеба:"
  bcd "$DL" "${DIR[gleb]}" wallet 2>&1
  echo; echo "### revoke create --node 0x7FFFFFFF --replace:"
  bcd "$DL" "${DIR[gleb]}" revoke create --node 0x7FFFFFFF --replace --out "$CERT" --via "$VIA" 2>&1
  echo; echo "### revoke verify --cert (автономная проверка сертификата):"
  bcd "$DL" "${DIR[gleb]}" revoke verify --cert "$CERT" 2>&1
  echo; echo "### revoke status --node 0x7FFFFFFF:"
  bcd "$DL" "${DIR[gleb]}" revoke status --node 0x7FFFFFFF 2>&1
  echo; echo "### СТАЛО (после отзыва) — кошелёк помечен REPLACED:"
  bcd "$DL" "${DIR[gleb]}" wallet 2>&1 | head -3
} > "$RESULTS/revoke.out" 2>&1
echo
echo "▶ ЭПИЛОГ: РОТАЦИЯ КЛЮЧА (revocation §6.7)  — подробности в results/revoke.out"
echo "─────────────────────────────────────────────────────────────────────────────"
grep -E 'REVOCATION|verified|VALID|valid|revoked|replacement|frozen|FROZEN|REPLACED|certificate|Effective' "$RESULTS/revoke.out" | head -8 | sed 's/^/  /'
log "сертификат отзыва создан, проверен автономно и опубликован в склад (см. revoke.out)"

echo
echo "Результаты: $RESULTS/"
echo "  ledger.csv   — все сделки (день, кто, что, часы, ставка, k, оценка)"
echo "  rates.csv    — эволюция ставок труда по дням"
echo "  summary.txt  — финальные досье, инвариант долга, доска идей, discovery"
echo "  events.log   — хронология событий месяца"
kill "$AGGPID" 2>/dev/null
