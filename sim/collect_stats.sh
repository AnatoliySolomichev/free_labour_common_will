#!/usr/bin/env bash
# collect_stats.sh — сводит итоги симуляции из эндпойнтов агрегатора и кошельков.
# Вызывается из economy_month.sh. Всё читается из данных, каждая цифра
# перепроверяема против цепей.
#   $1 WORK dir   $2 VIA url   $3 "имена"   $4 "p:full:spec:lev:id:dir ..."
set -u
WORK="$1"; VIA="$2"; read -r -a NAMES <<< "$3"; read -r -a PACK <<< "$4"
BC="${BC:-$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)/build/modules/cli/bc}"

declare -A FULL SPEC LEV ID DIR
for e in "${PACK[@]}"; do
  IFS=: read -r p f s l i d <<< "$e"
  FULL[$p]=$f; SPEC[$p]=$s; LEV[$p]=$l; ID[$p]=$i; DIR[$p]=$d
done

jnum() { grep -oE "\"$2\":-?[0-9.]+" <<< "$1" | head -1 | sed -E 's/.*://'; }
f2()   { awk -v x="${1:-0}" 'BEGIN{printf "%.2f", x}'; }

echo "══════════════════════════════════════════════════════════════════════════════"
echo "                    ИТОГИ СИМУЛЯЦИИ: «месяц» сообщества"
echo "══════════════════════════════════════════════════════════════════════════════"

# ── 1. Экономические досье (records.md §13) ──────────────────────────────────
echo
echo "▶ 1. ЭКОНОМИЧЕСКИЕ ДОСЬЕ ЦЕПЕЙ  (труд ⇄ именной взаимный кредит)"
echo "─────────────────────────────────────────────────────────────────────────────"
printf "%-8s %-9s %2s │ %8s %8s %8s │ %7s %7s │ %6s %9s\n" \
  "Кто" "Спец." "рз" "принял" "оценка_ч" "заработ" "эмит." "погаш." "долг" "потрачено"
echo "─────────────────────────────────────────────────────────────────────────────"
tot_issued=0; tot_redeemed=0; tot_debt=0; tot_recv=0; tot_spent=0; tot_appr=0; tot_wacc=0
for p in "${NAMES[@]}"; do
  j=$(curl -s "$VIA/economy/chain/${ID[$p]}")
  debt=$(jnum "$j" debt);     issued=$(jnum "$j" issued);   redeemed=$(jnum "$j" redeemed)
  recv=$(jnum "$j" received); spent=$(jnum "$j" spent)
  wacc=$(jnum "$j" works_accepted); appr=$(jnum "$j" labor_appraised)
  printf "%-8s %-9s %2s │ %8s %8s %8s │ %7s %7s │ %6s %9s\n" \
    "${FULL[$p]}" "${SPEC[$p]}" "${LEV[$p]}" \
    "${wacc:-0}" "$(f2 "$appr")" "$(f2 "$recv")" \
    "$(f2 "$issued")" "$(f2 "$redeemed")" "$(f2 "$debt")" "$(f2 "$spent")"
  tot_issued=$(awk -v a="$tot_issued" -v b="${issued:-0}" 'BEGIN{print a+b}')
  tot_redeemed=$(awk -v a="$tot_redeemed" -v b="${redeemed:-0}" 'BEGIN{print a+b}')
  tot_debt=$(awk -v a="$tot_debt" -v b="${debt:-0}" 'BEGIN{print a+b}')
  tot_recv=$(awk -v a="$tot_recv" -v b="${recv:-0}" 'BEGIN{print a+b}')
  tot_spent=$(awk -v a="$tot_spent" -v b="${spent:-0}" 'BEGIN{print a+b}')
  tot_appr=$(awk -v a="$tot_appr" -v b="${appr:-0}" 'BEGIN{print a+b}')
  tot_wacc=$((tot_wacc + ${wacc:-0}))
done
echo "─────────────────────────────────────────────────────────────────────────────"
printf "%-8s %-9s %2s │ %8s %8s %8s │ %7s %7s │ %6s %9s\n" \
  "ИТОГО" "" "" "$tot_wacc" "$(f2 "$tot_appr")" "$(f2 "$tot_recv")" \
  "$(f2 "$tot_issued")" "$(f2 "$tot_redeemed")" "$(f2 "$tot_debt")" "$(f2 "$tot_spent")"

# ── 2. Инвариант взаимного кредита ───────────────────────────────────────────
echo
echo "▶ 2. ИНВАРИАНТ ВЗАИМНОГО КРЕДИТА  (economy.md §4.1)"
echo "─────────────────────────────────────────────────────────────────────────────"
sum_held=0
for p in "${NAMES[@]}"; do
  held=$("$BC" --data-dir "${DIR[$p]}" wallet 2>/dev/null \
         | grep -E 'total held' | grep -oE '[0-9.]+' | head -1)
  sum_held=$(awk -v a="$sum_held" -v b="${held:-0}" 'BEGIN{print a+b}')
done
outstanding=$(awk -v i="$tot_issued" -v r="$tot_redeemed" 'BEGIN{print i-r}')
echo "  Σ эмитировано − Σ погашено (непогашенный долг в обороте) = $(f2 "$outstanding") ч"
echo "  Σ бумаги на руках у держателей (все кошельки)            = $(f2 "$sum_held") ч"
diff=$(awk -v a="$outstanding" -v b="$sum_held" 'BEGIN{d=a-b; if(d<0)d=-d; print d}')
if awk -v d="$diff" 'BEGIN{exit !(d<0.01)}'; then
  echo "  ✔ СХОДИТСЯ: долг каждого = сумма его бумаги у других; глобально долги = требования."
else
  echo "  ✗ РАСХОЖДЕНИЕ: $(f2 "$diff") ч (ожидалось 0 — проверьте незавершённые recv)."
fi
echo "  Часы рождаются парой долг/требование в момент оплаты и аннигилируют при"
echo "  возврате бумаги эмитенту (долг гасится трудом). Первичной эмиссии нет —"
echo "  система стартовала со всеобщего нуля."

# ── 3. Кошельки: чью бумагу держит каждый ────────────────────────────────────
echo
echo "▶ 3. КОШЕЛЬКИ  (именная бумага не смешивается — час = час, курсов нет)"
echo "─────────────────────────────────────────────────────────────────────────────"
for p in "${NAMES[@]}"; do
  w=$("$BC" --data-dir "${DIR[$p]}" wallet 2>/dev/null)
  held=$(grep -E 'total held' <<<"$w" | grep -oE '[0-9.]+' | head -1)
  debt=$(grep -E 'own debt' <<<"$w" | grep -oE '[0-9.]+' | head -1)
  issuers=$(grep -cE '^\s+[0-9a-f]{16}' <<<"$w")
  printf "  %-7s держит %6s ч бумаги от %s эмитент(а/ов); свой долг %6s ч\n" \
    "${FULL[$p]}" "$(f2 "${held:-0}")" "${issuers:-0}" "$(f2 "${debt:-0}")"
done

# ── 4. Доска идей (funding board, economy.md §7) ─────────────────────────────
echo
echo "▶ 4. ДОСКА ИДЕЙ  (приоритет = объём обещанного труда)"
echo "─────────────────────────────────────────────────────────────────────────────"
ideas=$(curl -s "$VIA/economy/ideas")
if [ "$ideas" = "[]" ] || [ -z "$ideas" ]; then
  echo "  (идей нет)"
else
  echo "$ideas" | sed 's/},{/}\n{/g' | while read -r it; do
    txt=$(grep -oE '"text":"[^"]*"' <<<"$it" | sed -E 's/"text":"(.*)"/\1/')
    pa=$(jnum "$it" pledged_active); ps=$(jnum "$it" pledged_settled)
    pl=$(jnum "$it" pledgers); cp=$(jnum "$it" copies); rx=$(jnum "$it" reactions)
    printf "  «%s»\n     активных пледжей %s ч · погашено %s ч · спонсоров %s · копий %s · реакции %s\n" \
      "${txt:-?}" "$(f2 "${pa:-0}")" "$(f2 "${ps:-0}")" "${pl:-0}" "${cp:-0}" "${rx:-0}"
  done
fi
echo "  Статусы пледжей по цепям:"
for p in "${NAMES[@]}"; do
  j=$(curl -s "$VIA/economy/chain/${ID[$p]}")
  a=$(jnum "$j" pledges_active); s=$(jnum "$j" pledges_settled)
  r=$(jnum "$j" pledges_revoked); e=$(jnum "$j" pledges_expired)
  tot=$(( ${a:-0} + ${s:-0} + ${r:-0} + ${e:-0} ))
  [ "$tot" -eq 0 ] && continue
  printf "     %-7s активных %s · погашено %s · отозвано %s · истекло %s\n" \
    "${FULL[$p]}" "${a:-0}" "${s:-0}" "${r:-0}" "${e:-0}"
done

# ── 5. Граф доверия / discovery (sync.md §8) ─────────────────────────────────
echo
echo "▶ 5. ГРАФ ЗАВЕРЕННОЙ ИСТОРИИ  (merge → discovery: идентичность через труд)"
echo "─────────────────────────────────────────────────────────────────────────────"
for p in "${NAMES[@]}"; do
  d=$(curl -s "$VIA/discovery/${ID[$p]}")
  echo "$d" > "$WORK/results/discovery_$p.json"
  cnt=$(grep -oE '"chain"' <<<"$d" | wc -l)
  mw=$(grep -oE '"merges_with":[0-9]+' <<<"$d" | sed -E 's/.*://' | awk '{s+=$1} END{print s+0}')
  nb=$(grep -oE '"neighbor":true' <<<"$d" | wc -l)
  printf "  %-7s кандидатов %s · торговых соседей %s · прямых merge-связей %s\n" \
    "${FULL[$p]}" "${cnt:-0}" "${nb:-0}" "${mw:-0}"
done

# ── 6. Итоговые ставки труда (сформированы рынком, не назначены) ──────────────
echo
echo "▶ 6. СТАВКИ ТРУДА НА КОНЕЦ МЕСЯЦА vs СТАРТ  (стч/час; сформированы сделками)"
echo "─────────────────────────────────────────────────────────────────────────────"
RATES="$WORK/results/rates.csv"
if [ -f "$RATES" ]; then
  printf "  %-16s %8s %8s %9s   %s\n" "специальность" "старт" "финал" "Δ%" "часов/сделок за месяц"
  awk -F, 'NR>1 {
    key=$3" р."$4;
    if(!(key in first)){first[key]=$5}
    last[key]=$5; hd[key]+=$7; dd[key]+=$8
  }
  END{
    for(k in first){
      d=(last[k]-first[k])/first[k]*100;
      printf "  %-16s %8.3f %8.3f %+8.1f%%   %.0fч / %d сделок\n",k,first[k],last[k],d,hd[k],dd[k]
    }
  }' "$RATES" | sort
  echo "  (ставка дня = усреднение вчерашних заверенных сделок; k каждого двигает её по сети)"
else
  echo "  (нет данных ставок)"
fi

# ── 7. Активность цепей ──────────────────────────────────────────────────────
echo
echo "▶ 7. ОБЪЁМ СЕТИ"
echo "─────────────────────────────────────────────────────────────────────────────"
st=$(curl -s "$VIA/stats")
echo "  агрегатор /stats: $st"
echo "  блоков в цепях (по каждому):"
for p in "${NAMES[@]}"; do
  n=$("$BC" --data-dir "${DIR[$p]}" list 2>/dev/null | grep -cE '^#[0-9]')
  printf "     %-7s %s блок(ов)\n" "${FULL[$p]}" "${n:-0}"
done
echo "══════════════════════════════════════════════════════════════════════════════"
