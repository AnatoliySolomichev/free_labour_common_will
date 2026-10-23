#!/usr/bin/env bash
# ─────────────────────────────────────────────────────────────────────────────
# Демо соцслоя (ИР-026, social.md): лайки, дизлайки и комментарии к роликам и
# постам — не на площадке, а каждый в своей цепи.
#
# Показывает вживую:
#   • social.md §3 — один ролик под пятью формами ссылки даёт один ключ yt:…,
#     пост Telegram — один ключ tg:…, метки слежки из ссылки уходят;
#   • social.md §5 — круг: публичные подписки, свой список, второй шаг;
#   • social.md §6 — картина ролика у Анны: её круг поимённо, а ферма из
#     $FARM ботов — только числом «вне круга»;
#   • records.md §8.11 — ответ на комментарий, снятие своего комментария,
#     последнее слово (передумал — переставил реакцию), локальный игнор.
#
# Самодостаточно: свой агрегатор на отдельном порту, временная папка, уборка.
#
# Запуск:   docs/pilot/social-demo.sh            (собери проект: cmake --build build)
#           FARM=50 PORT=19400 docs/pilot/social-demo.sh
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
PORT="${PORT:-19340}"
VIA="http://localhost:$PORT"
FARM="${FARM:-25}"

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

# Человек = каталог данных; bcx ИМЯ команда… — от его лица.
bcx() { local who="$1"; shift; "$BC" --data-dir "$work/$who" "$@"; }
chain_of() { bcx "$1" identity show | after 'User ID: '; }
# Пишущая команда с выгрузкой; на экран — первая строка (что записано).
post() { local who="$1"; shift; bcx "$who" "$@" --via "$VIA" 2>/dev/null | head -1 | sed "s/^/   $who: /"; }
# Хеш блока записанного комментария.
comment_hash() { local who="$1"; shift; bcx "$who" social comment "$@" --via "$VIA" 2>/dev/null | after 'hash: '; }

# ── Агрегатор ────────────────────────────────────────────────────────────────
hr "агрегатор на $VIA"
"$AGG" --port "$PORT" --db "$work/agg" --catalog "$CATALOGS" >"$work/agg.log" 2>&1 &
agg_pid=$!
for _ in $(seq 1 40); do
    curl -sf -o /dev/null "$VIA/catalog" && break || sleep 0.25
done
say "поднят (pid $agg_pid)"

# ── Люди ─────────────────────────────────────────────────────────────────────
hr "люди: Анна смотрит, остальные пишут"
for p in anna boris vera gleb dina egor; do bcx "$p" identity create >/dev/null 2>&1; done
ANNA="$(chain_of anna)"; BORIS="$(chain_of boris)"; VERA="$(chain_of vera)"
GLEB="$(chain_of gleb)"; DINA="$(chain_of dina)";   EGOR="$(chain_of egor)"
say "Анна  ${ANNA:0:8}…  — та, чьими глазами смотрим"
say "Борис ${BORIS:0:8}…, Вера ${VERA:0:8}… — Анна подписана на них публично"
say "Глеб  ${GLEB:0:8}…  — в своём списке Анны, без записи в цепь"
say "Дина  ${DINA:0:8}…  — на неё подписан Борис: для Анны это второй шаг"
say "Егор  ${EGOR:0:8}…  — ни с кем не связан"

# ── Ключи ────────────────────────────────────────────────────────────────────
hr "один ролик — пять ссылок, один ключ (social.md §3.2)"
LINKS=(
    "https://www.youtube.com/watch?v=dQw4w9WgXcQ&t=42s"
    "https://youtu.be/dQw4w9WgXcQ?si=ShareTracking"
    "https://m.youtube.com/shorts/dQw4w9WgXcQ"
    "youtube.com/embed/dQw4w9WgXcQ"
    "https://music.youtube.com/watch?feature=share&v=dQw4w9WgXcQ"
)
KEYS=()
for l in "${LINKS[@]}"; do
    k="$(bcx anna social key "$l")"; KEYS+=("$k")
    printf '   %-58s → %s\n' "$l" "$k"
done
VIDEO="${KEYS[0]}"
hr "пост Telegram и статья с метками слежки (social.md §3.3, social.md §3.4)"
TG1="https://t.me/s/Durov/042"; TG2="t.me/durov/42?single"
for l in "$TG1" "$TG2" "https://www.Example.com/news/42/?utm_source=tg&b=2&a=1#top"; do
    printf '   %-58s → %s\n' "$l" "$(bcx anna social key "$l")"
done

# ── Подписки ─────────────────────────────────────────────────────────────────
hr "подписки: публичные — записью в цепи, свои — только у себя"
post anna  social follow "$BORIS"
post anna  social follow "$VERA"
bcx  anna  social follow "$GLEB" --local | sed 's/^/   anna: /'
post boris social follow "$DINA"
bcx anna social circle --hops 2 --via "$VIA" | sed 's/^/   /'

# ── Реакции и комментарии ────────────────────────────────────────────────────
hr "реакции на ролик — каждый со своей формы ссылки"
post boris social like    "${LINKS[1]}"
post vera  social like    "${LINKS[0]}"
post gleb  social dislike "${LINKS[2]}"
post dina  social like    "${LINKS[3]}"
post egor  social dislike "${LINKS[4]}"
post anna  social like    "$VIDEO"
say "Вера передумала… и вернулась: действует последнее слово"
post vera  social unlike  "${LINKS[1]}"
post vera  social like    "${LINKS[2]}"

hr "комментарии: ответ, лайк комментарию, снятие своего"
VC="$(comment_hash vera "${LINKS[0]}" "Лучший разбор этой темы, что я видела")"
VERA_C="$VERA/$VC"
say "vera: комментарий ${VC:0:12}…"
BC_H="$(comment_hash boris "$VIDEO" "Согласен, особенно про вторую часть" --re "$VERA_C")"
say "boris: ответ Вере ${BC_H:0:12}…"
post anna  social like "$VERA_C"
GC="$(comment_hash gleb "${LINKS[3]}" "Слишком длинно")"
say "gleb: комментарий ${GC:0:12}… — и тут же снимает его"
post gleb  social retract "$GLEB/$GC"
EC="$(comment_hash egor "$VIDEO" "Подписывайтесь на мой канал!")"
comment_hash egor "$VIDEO" "Вера, загляни ко мне" --re "$VERA_C" >/dev/null
say "egor: два комментария — свой и ответ Вере (${EC:0:12}…)"

# ── Ферма ────────────────────────────────────────────────────────────────────
hr "ферма: $FARM свежих цепей лайкают ролик и спамят комментариями"
for i in $(seq 1 "$FARM"); do
    bcx "bot$i" identity create >/dev/null 2>&1
    bcx "bot$i" social like "${LINKS[$(( i % 5 ))]}" --via "$VIA" >/dev/null 2>&1
    bcx "bot$i" social comment "${LINKS[$(( (i + 2) % 5 ))]}" "крутое видео!!! заходи ко мне" \
        --via "$VIA" >/dev/null 2>&1
done
say "готово: $FARM лайков и $FARM комментариев от цепей, которых никто не читает"

# ── Картина ──────────────────────────────────────────────────────────────────
hr "картина ролика глазами Анны (social.md §6)"
SHOW1="$(bcx anna social show "${LINKS[4]}" --via "$VIA")"
printf '%s\n' "$SHOW1" | sed 's/^/   /'

hr "то же со вторым шагом: подписки подписок (--hops 2)"
SHOW2="$(bcx anna social show "$VIDEO" --hops 2 --via "$VIA")"
printf '%s\n' "$SHOW2" | sed 's/^/   /'

hr "Анна игнорирует Глеба — его дизлайк уходит из её картины"
bcx anna social ignore "$GLEB" | sed 's/^/   anna: /'
SHOW3="$(bcx anna social show "$VIDEO" --via "$VIA")"
printf '%s\n' "$SHOW3" | head -3 | sed 's/^/   /'

hr "пост Telegram: комментарий с одной ссылки, лайк — с другой"
post vera  social comment "$TG1" "Важный пост, перечитайте"
post boris social like    "$TG2"
SHOW_TG="$(bcx anna social show "https://telegram.me/durov/42" --via "$VIA")"
printf '%s\n' "$SHOW_TG" | sed 's/^/   /'

hr "лента круга Анны"
FEED="$(bcx anna social feed --hops 2 --via "$VIA")"
printf '%s\n' "$FEED" | sed 's/^/   /'

# ── Проверки ─────────────────────────────────────────────────────────────────
hr "проверки"
fail=0
check() { # описание, факт, ожидание
    if [[ "$2" == "$3" ]]; then printf '   ✓ %s\n' "$1"
    else printf '   ✗ %s: получили «%s», ждали «%s»\n' "$1" "$2" "$3"; fail=1; fi
}
line() { printf '%s\n' "$1" | grep -m1 "$2" | sed 's/^ *//'; }

check "пять ссылок — один ключ" "$(printf '%s\n' "${KEYS[@]}" | sort -u | wc -l | tr -d ' ')" "1"
check "пост Telegram с двух ссылок — один ключ" \
      "$(bcx anna social key "$TG1" "$TG2" | sort -u | tr -d '\n')" "tg:durov/42"
check "реакции круга: вы, Борис, Вера «за», Глеб «против»" \
      "$(line "$SHOW1" 'реакции круга')" "реакции круга: +3 −1   (вы +1 · шаг 1 +2 −1)"
check "ферма, Егор и Дина — только числом" \
      "$(line "$SHOW1" '^ *вне круга: +')" \
      "вне круга: +$(( FARM + 1 )) −1 от $(( FARM + 2 )) цеп. — со слов агрегатора, в счёт не идёт"
check "в круге три комментария: Веры, ответ Бориса, снятый Глеба" \
      "$(line "$SHOW1" 'комментарии круга')" "комментарии круга: 3"
check "у комментария Веры лайк Анны" \
      "$(printf '%s\n' "$SHOW1" | grep -c 'Лучший разбор этой темы, что я видела»  +1')" "1"
check "снятый комментарий Глеба виден как снятый" \
      "$(printf '%s\n' "$SHOW1" | grep -c 'снят автором')" "1"
check "спам фермы и Егора — числом, не текстом" \
      "$(line "$SHOW1" 'коммент. — со слов')" \
      "вне круга: $(( FARM + 2 )) коммент. — со слов агрегатора (--all — показать)"
check "второй шаг добавил Дину" \
      "$(line "$SHOW2" 'реакции круга')" "реакции круга: +4 −1   (вы +1 · шаг 1 +2 −1 · шаг 2 +1)"
check "игнор убрал дизлайк Глеба" \
      "$(line "$SHOW3" 'реакции круга')" "реакции круга: +3   (вы +1 · шаг 1 +2)"
check "пост Telegram сошёлся с двух ссылок" \
      "$(line "$SHOW_TG" 'реакции круга')" "реакции круга: +1   (вы — · шаг 1 +1)"
check "лента не видит ферму" "$(printf '%s\n' "$FEED" | grep -c 'крутое видео')" "0"

hr "итог"
if [[ "$fail" == 0 ]]; then
    echo "   всё сошлось."
else
    echo "   ЕСТЬ РАСХОЖДЕНИЯ (см. выше)"; exit 1
fi
