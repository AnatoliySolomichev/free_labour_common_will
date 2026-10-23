#!/usr/bin/env bash
# ─────────────────────────────────────────────────────────────────────────────
# Демо отзывов о продукции (ИР-026, reviews.md): отзыв — в своей цепи, товар —
# под любым из своих названий, картина — глазами своего круга.
#
# Показывает вживую:
#   • social.md §3.6 — ключи товара: штрихкод, модель, Ozon, Wildberries,
#     Яндекс Маркет, Amazon, AliExpress;
#   • reviews.md §3 — «тот же товар» склеивает названия по заявлениям круга;
#     «не тот же» — разногласие, а не склейка;
#   • reviews.md §2 — звёзды и текст, правка — последнее слово, «полезно» —
#     реакции круга;
#   • reviews.md §4 — у товара, сделанного в системе, покупка подтверждена
#     настоящей сделкой; о своём товаре отзыв не пишут;
#   • ферма «приклеивает» свою копию и заливает её пятёрками — в чужом кругу
#     это не работает.
#
# Самодостаточно: свой агрегатор на отдельном порту, временная папка, уборка.
#
# Запуск:   docs/pilot/review-demo.sh            (собери проект: cmake --build build)
#           FARM=20 PORT=19400 docs/pilot/review-demo.sh
#
# Годится и как дымовой тест: в конце проверяет числа и падает при расхождении.
# ─────────────────────────────────────────────────────────────────────────────
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD="${BUILD:-$here/../../build}"
BC="$BUILD/modules/cli/bc"
AGG="$BUILD/modules/aggregator/aggregator_server"
for bin in "$BC" "$AGG"; do
    [[ -x "$bin" ]] || { echo "не найден $bin — соберите: cmake --build \"$BUILD\"" >&2; exit 1; }
done
CATALOGS="$here/../catalogs"
PORT="${PORT:-19360}"
VIA="http://localhost:$PORT"
FARM="${FARM:-10}"

work="$(mktemp -d)"
agg_pid=""
cleanup() {
    [[ -n "$agg_pid" ]] && kill "$agg_pid" 2>/dev/null || true
    rm -rf "$work"
}
trap cleanup EXIT

hr()  { printf '\n\033[1m── %s ──\033[0m\n' "$1"; }
say() { printf '   %s\n' "$*"; }
after() { sed -n "s/.*$1//p" | head -1; }

bcx() { local who="$1"; shift; "$BC" --data-dir "$work/$who" "$@"; }
chain_of() { bcx "$1" identity show | sed -n 's/.*User ID: //p'; }
post() { local who="$1"; shift; bcx "$who" "$@" --via "$VIA" 2>/dev/null | head -1 | sed "s/^/   $who: /"; }
quiet() { local who="$1"; shift; bcx "$who" "$@" --via "$VIA" >/dev/null 2>&1; }

# ── Агрегатор ────────────────────────────────────────────────────────────────
hr "агрегатор на $VIA"
"$AGG" --port "$PORT" --db "$work/agg" --catalog "$CATALOGS" >"$work/agg.log" 2>&1 &
agg_pid=$!
for _ in $(seq 1 40); do
    curl -sf -o /dev/null "$VIA/catalog" && break || sleep 0.25
done
say "поднят (pid $agg_pid)"

# ── Люди ─────────────────────────────────────────────────────────────────────
hr "люди"
for p in anna boris vera gleb dina egor master; do bcx "$p" identity create >/dev/null 2>&1; done
MASTER="$(chain_of master)"
for p in boris vera gleb dina; do quiet anna social follow "$(chain_of "$p")"; done
say "Анна смотрит; её круг — Борис, Вера, Глеб, Дина; Егор ни с кем не связан"
say "Мастер — столяр, делает табуретки в системе"

# ── Ключи ────────────────────────────────────────────────────────────────────
hr "один повербанк — семь названий (social.md §3.6)"
GTIN="4601234567893"
OZON="https://www.ozon.ru/product/power-bank-xiaomi-20000-123456789/?sh=abc"
WB="https://www.wildberries.ru/catalog/654321/detail.aspx?targetUrl=GP"
YM="https://market.yandex.ru/product--power-bank-xiaomi/1779337045?sku=1"
AMZ="https://www.amazon.com/Xiaomi-Power-Bank/dp/b0194wdvhi/ref=sr_1_3"
MODEL="model:Xiaomi Power Bank 20000"
FAKE="https://aliexpress.ru/item/1005009999999999.html"
for l in "$GTIN" "$MODEL" "$OZON" "$WB" "$YM" "$AMZ" "$FAKE"; do
    printf '   %-60s → %s\n' "${l:0:60}" "$(bcx anna social key "$l")"
done
G="$(bcx anna social key "$GTIN")"

# ── Отзывы и «тот же товар» ──────────────────────────────────────────────────
hr "отзывы — каждый со своего названия; «тот же товар» — заявлением (reviews.md §3)"
post boris review add "$OZON" --stars 4 "держит заряд два дня"
post boris review same "$OZON" "$GTIN"
post vera  review add "$WB" --stars 5 "лёгкий, заряжает быстро"
post vera  review same "$WB" "$GTIN"
post gleb  review add "$MODEL" --stars 2 "грелся"
post gleb  review same "$MODEL" "$OZON"
say "Глеб передумал — последнее слово:"
post gleb  review add "$MODEL" --stars 3 "грелся, но терпимо"
post anna  review same "$AMZ" "$GTIN"
say "Маркет: Борис считает тем же, Дина — нет, там подделка:"
post boris review same "$YM" "$GTIN"
post dina  review same "$YM" "$GTIN" --no
post dina  review add "$YM" --stars 1 "на Маркете пришла подделка"
post egor  review add "$GTIN" --stars 1 "не понравился"

hr "«полезно» — реакции на отзыв (social.md §4)"
BORIS_REVIEW="$(chain_of boris)/$(bcx boris list | grep -m1 'review' | sed -n 's/.*hash://p')"
post anna social like "$BORIS_REVIEW"
post dina social like "$BORIS_REVIEW"

hr "ферма: копия с AliExpress, «тот же товар» и пятёрки"
for i in $(seq 1 "$FARM"); do
    bcx "bot$i" identity create >/dev/null 2>&1
    quiet "bot$i" review add "$FAKE" --stars 5 "лучший повербанк"
    quiet "bot$i" review same "$FAKE" "$GTIN"
done
say "$FARM цепей приклеили свою копию к штрихкоду и поставили ей по 5★"

# ── Товар из системы ─────────────────────────────────────────────────────────
hr "табуретка мастера: Вера покупает её через систему (reviews.md §4)"
AXREFS="$work/axes.tsv"
OUT="$AXREFS" BC="$BC" bash "$here/seed-axes.sh" "$work/axes" --via "$VIA" >/dev/null 2>&1
axis() { awk -F'\t' -v s="$1" '$1==s{print $2}' "$AXREFS"; }
STOOL_H="$(bcx master work log --title "столяр" --action "табурет из берёзы" --hours 2 \
           --output "табурет:1:шт" --for "$(chain_of vera)" --via "$VIA" 2>/dev/null | after 'hash: ')"
STOOL="$MASTER/$STOOL_H"
ACC="$(bcx vera accept --work "$STOOL" --quality "крепкий" --coef 1.0 \
       --axis "$(axis physical)=1.2" --axis "$(axis knowledge)=0.8" --via "$VIA" 2>/dev/null \
       | after 'acceptance ref: ')"
bcx master transfer recv "$ACC" --via "$VIA" --yes >/dev/null 2>&1
say "мастер записал работу на заказ Веры, Вера приняла и оплатила, мастер ответил — сделка состоялась"
post vera  review add "$STOOL" --stars 5 "крепкая, не скрипит"
post boris review add "$STOOL" --stars 2 "на вид хлипкая"
SELF="$(bcx master review add "$STOOL" --stars 5 "лучшая табуретка" 2>&1 || true)"
say "мастер хвалит сам себя: $(printf '%s\n' "$SELF" | tail -1)"

# ── Картины ──────────────────────────────────────────────────────────────────
hr "повербанк глазами Анны — по штрихкоду (reviews.md §5)"
P_GTIN="$(bcx anna review show "$GTIN" --via "$VIA")"
printf '%s\n' "$P_GTIN" | sed 's/^/   /'

hr "та же картина — по ссылке Ozon"
P_OZON="$(bcx anna review show "$OZON" --via "$VIA")"
printf '%s\n' "$P_OZON" | head -3 | sed 's/^/   /'

hr "без круга (--all): ферма приклеила копию и залила её пятёрками"
P_ALL="$(bcx anna review show "$GTIN" --all --via "$VIA")"
printf '%s\n' "$P_ALL" | head -4 | sed 's/^/   /'

hr "табуретка глазами Анны"
P_STOOL="$(bcx anna review show "$STOOL" --via "$VIA")"
printf '%s\n' "$P_STOOL" | sed 's/^/   /'

# ── Проверки ─────────────────────────────────────────────────────────────────
hr "проверки"
fail=0
check() { # описание, факт, ожидание
    if [[ "$2" == "$3" ]]; then printf '   ✓ %s\n' "$1"
    else printf '   ✗ %s: получили «%s», ждали «%s»\n' "$1" "$2" "$3"; fail=1; fi
}
line() { printf '%s\n' "$1" | grep -m1 "$2" | sed 's/^ *//'; }

check "ключи магазинов" "$(bcx anna social key "$OZON" "$WB" "$YM" "$AMZ" "$FAKE" | tr '\n' ' ')" \
      "ozon:123456789 wb:654321 ymarket:1779337045 amazon:B0194WDVHI ali:1005009999999999 "
check "он же — по заявлениям круга и своему" "$(line "$P_GTIN" 'он же')" \
      "он же: amazon:B0194WDVHI, model:xiaomi-power-bank-20000, ozon:123456789, wb:654321   (по заявлениям вашего круга)"
check "Маркет не склеен: разногласие" \
      "$(printf '%s\n' "$P_GTIN" | grep -c "разногласие: $G и ymarket:1779337045")" "1"
check "три отзыва круга, медиана 4, Глеб — по последнему слову" "$(line "$P_GTIN" 'отзывы круга')" \
      "отзывы круга: 3 — медиана 4★  (5★ 1 · 4★ 1 · 3★ 1 · 2★ 0 · 1★ 0)"
check "отзыв Бориса полезен двоим" "$(printf '%s\n' "$P_GTIN" | grep -c 'держит заряд два дня».*полезно +2')" "1"
check "Егор — только числом" "$(line "$P_GTIN" 'вне круга')" \
      "вне круга: 1 отзыв. — со слов агрегатора, в счёт не идёт"
check "по ссылке Ozon — та же картина" "$(line "$P_OZON" 'отзывы круга')" "$(line "$P_GTIN" 'отзывы круга')"
check "без круга копия фермы склеена, медиана — 5" "$(line "$P_ALL" 'отзывы всех')" \
      "отзывы всех: $(( FARM + 4 )) — медиана 5★  (5★ $(( FARM + 1 )) · 4★ 1 · 3★ 1 · 2★ 0 · 1★ 1)"
check "о своём товаре отзыв не пишут" "$(printf '%s\n' "$SELF" | grep -c 'отзыв о себе не считается')" "1"
check "покупка Веры подтверждена, Борис не покупал" "$(line "$P_STOOL" 'покупка подтверждена:')" \
      "покупка подтверждена: 1 из 2"
check "у Бориса — без сделки покупки" "$(printf '%s\n' "$P_STOOL" | grep -c 'хлипкая».*без сделки покупки')" "1"

hr "итог"
if [[ "$fail" == 0 ]]; then
    echo "   всё сошлось."
else
    echo "   ЕСТЬ РАСХОЖДЕНИЯ (см. выше)"; exit 1
fi
