#!/usr/bin/env bash
# ─────────────────────────────────────────────────────────────────────────────
# Демо соревнований (ИР-026, contests.md): подтягивания на видео, судьи из
# своего круга, таблица у каждого своя.
#
# Показывает вживую:
#   • contests.md §2 — правила как мягкий протокол: текст и параметры, а
#     идентификатор — хеш содержания; копия правил у второго организатора —
#     то же соревнование; вступление — согласие с правилами;
#   • contests.md §3 — раунд и маяк: начало хеша блока раунда — в кадре;
#   • contests.md §4 — видео по хешу файла и его копии на площадках; подмена
#     файла видна;
#   • contests.md §5, contests.md §6 — вердикты, медиана, «себе не судья»,
#     судья без согласия не считается; у накрутчика в своём кругу — своя
#     таблица, а ферма ботов в чужом кругу весит ноль.
#
# Самодостаточно: свой агрегатор на отдельном порту, временная папка, уборка.
#
# Запуск:   docs/pilot/contest-demo.sh            (собери проект: cmake --build build)
#           FARM=20 PORT=19400 docs/pilot/contest-demo.sh
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
PORT="${PORT:-19350}"
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
after() { sed -n "s/^$1//p" | head -1; }

bcx() { local who="$1"; shift; "$BC" --data-dir "$work/$who" "$@"; }
chain_of() { bcx "$1" identity show | sed -n 's/.*User ID: //p'; }
quiet() { local who="$1"; shift; bcx "$who" "$@" --via "$VIA" >/dev/null 2>&1; }
# Видео — просто байты: важно не содержание, а хеш файла.
video() { head -c 40000 /dev/urandom > "$work/$1.mp4"; printf '%s' "$work/$1.mp4"; }

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
for p in coach zoya anna dina egor zhora boris vera gleb ira; do bcx "$p" identity create >/dev/null 2>&1; done
ANNA="$(chain_of anna)"; DINA="$(chain_of dina)"; EGOR="$(chain_of egor)"; ZHORA="$(chain_of zhora)"
GLEB="$(chain_of gleb)"
say "Тренер — организатор, клуб «Турник»; Зоя — второй организатор, те же правила"
say "Анна, Дина, Егор — судьи; Жора судит, но с правилами не согласился"
say "Борис, Вера — честные спортсмены; Глеб заявит 50 и приведёт своих «судей»; Ира — у Зои"
say "Анна — та, чьими глазами смотрим таблицу; её круг: Дина, Егор, Жора"

# ── Правила ──────────────────────────────────────────────────────────────────
hr "правила — мягкий протокол: текст для людей, параметры, хеш содержания (contests.md §2)"
cat > "$work/rules.txt" <<'TXT'
Подтягивания на турнике
Подбородок выше перекладины, внизу руки полностью прямые.
Без рывков и раскачки; спрыгнул — подход окончен.
Маяк раунда — на листке в кадре до первого повтора.
TXT
CREATE="$(bcx coach contest create --text-file "$work/rules.txt" --unit повтор --via "$VIA" 2>/dev/null)"
printf '%s\n' "$CREATE" | sed 's/^/   coach: /'
CONTEST="$(printf '%s\n' "$CREATE" | after 'соревнование ')"
RULES="$(printf '%s\n' "$CREATE" | after 'правила: ')"
say "вступают — согласием с правилами:"
for p in anna dina egor boris vera gleb; do quiet "$p" contest join "$RULES"; done
say "Анна, Дина, Егор, Борис, Вера, Глеб — согласны; Жора — нет"

hr "раунд и маяк (contests.md §3)"
ROUND_OUT="$(bcx coach contest round "$CONTEST" --name Октябрь --via "$VIA" 2>/dev/null)"
printf '%s\n' "$ROUND_OUT" | sed 's/^/   coach: /'
ROUND="$(printf '%s\n' "$ROUND_OUT" | after 'раунд ')"
BEACON="$(printf '%s\n' "$ROUND_OUT" | after 'маяк: ' | cut -c1-6)"

# ── Попытки ──────────────────────────────────────────────────────────────────
hr "попытки: видео — ключ по хешу файла, копии — «тот же предмет» (contests.md §4)"
attempt() {   # кто, сколько заявил, ссылка-копия
    local who="$1" claim="$2" mirror="$3" out
    out="$(bcx "$who" contest attempt "$ROUND" --video "$(video "$who")" --claim "$claim" \
           --mirror "$mirror" --note "маяк $BEACON в кадре" --via "$VIA" 2>/dev/null)"
    printf '%s\n' "$out" | sed "s/^/   $who: /" >&2
    printf '%s\n' "$out" | after 'попытка '
}
A_BORIS="$(attempt boris 20 "https://youtu.be/borisAAAA01")"
A_VERA="$(attempt vera 15 "https://youtu.be/veraAAAAA01")"
A_GLEB="$(attempt gleb 50 "https://youtu.be/glebAAAAA01")"
cp "$work/boris.mp4" "$work/boris-original.mp4"
head -c 40000 /dev/urandom > "$work/boris-edited.mp4"   # «смонтированная» подмена

# ── Судейство ────────────────────────────────────────────────────────────────
hr "вердикты: засчитано по правилам (contests.md §5)"
SELF="$(bcx gleb contest verdict "$A_GLEB" 50 2>&1 || true)"
say "gleb судит сам себя: $(printf '%s\n' "$SELF" | tail -1)"
verdict() { quiet "$1" contest verdict "$2" "$3" --note "${4:-}"; }
verdict anna "$A_BORIS" 18 "2 без полной амплитуды"
verdict dina "$A_BORIS" 19
verdict egor "$A_BORIS" 18
verdict anna "$A_VERA"  15
verdict dina "$A_VERA"  14
verdict egor "$A_VERA"  15
verdict anna "$A_GLEB"  12 "раскачка с 13-го"
verdict dina "$A_GLEB"  13
verdict egor "$A_GLEB"  12
verdict zhora "$A_BORIS" 30 "мне показалось 30"
say "Анна, Дина, Егор судят всех; Жора ставит Борису 30 — но он не согласился с правилами"
say "Егор передумал по Вере — последнее слово судьи:"
verdict egor "$A_VERA" 14
verdict egor "$A_VERA" 15

hr "свои «судьи» Глеба и ферма"
for b in fan1 fan2; do
    bcx "$b" identity create >/dev/null 2>&1
    quiet "$b" contest join "$RULES"
    verdict "$b" "$A_GLEB" 50 "чистые пятьдесят"
    quiet gleb social follow "$(chain_of "$b")"
done
say "fan1, fan2: согласились с правилами и засчитали Глебу 50; Глеб на них подписан"
for i in $(seq 1 "$FARM"); do
    bcx "bot$i" identity create >/dev/null 2>&1
    quiet "bot$i" contest join "$RULES"
    a="$(bcx "bot$i" contest attempt "$ROUND" --video "$(video "bot$i")" --claim 99 --via "$VIA" 2>/dev/null \
         | after 'попытка ')"
    [[ "$i" -gt 1 ]] && verdict "bot$(( i - 1 ))" "$a" 99
done
say "ферма: $FARM попыток «по 99», засуживают друг друга"

# ── Второй организатор ───────────────────────────────────────────────────────
hr "Зоя берёт те же правила к себе — то же соревнование, свой раунд"
ADOPT="$(bcx zoya contest adopt "$RULES" --via "$VIA" 2>/dev/null)"
printf '%s\n' "$ADOPT" | sed 's/^/   zoya: /'
ZOYA_KEY="$(printf '%s\n' "$ADOPT" | after 'копия правил ')"
Z_ROUND="$(bcx zoya contest round "$CONTEST" --name "Октябрь, Заречье" --via "$VIA" 2>/dev/null | after 'раунд ')"
quiet ira contest join "$RULES"
bcx ira contest attempt "$Z_ROUND" --video "$(video ira)" --claim 22 --via "$VIA" >/dev/null 2>&1
SHOW="$(bcx anna contest show "$CONTEST" --via "$VIA")"
printf '%s\n' "$SHOW" | sed 's/^/   /'

# ── Таблицы ──────────────────────────────────────────────────────────────────
hr "Анна подписана на Дину, Егора и Жору"
for c in "$DINA" "$EGOR" "$ZHORA"; do quiet anna social follow "$c"; done
hr "таблица раунда глазами Анны (contests.md §6)"
T_ANNA="$(bcx anna contest table "$ROUND" --via "$VIA")"
printf '%s\n' "$T_ANNA" | sed 's/^/   /'

hr "та же таблица глазами Глеба — его круг: fan1 и fan2"
T_GLEB="$(bcx gleb contest table "$ROUND" --via "$VIA")"
printf '%s\n' "$T_GLEB" | sed 's/^/   /'

hr "все согласные с правилами судьи, без круга (--all) — вот зачем круг"
T_ALL="$(bcx anna contest table "$ROUND" --all --via "$VIA")"
printf '%s\n' "$T_ALL" | sed 's/^/   /'

hr "тот ли файл? (contests.md §4)"
V_OK="$(bcx anna contest verify "$work/boris-original.mp4" "$A_BORIS" --via "$VIA" || true)"
V_BAD="$(bcx anna contest verify "$work/boris-edited.mp4" "$A_BORIS" --via "$VIA" || true)"
say "оригинал: $V_OK"
say "подмена:  $(printf '%s\n' "$V_BAD" | cut -c1-60)…"

# ── Проверки ─────────────────────────────────────────────────────────────────
hr "проверки"
fail=0
check() { # описание, факт, ожидание
    if [[ "$2" == "$3" ]]; then printf '   ✓ %s\n' "$1"
    else printf '   ✗ %s: получили «%s», ждали «%s»\n' "$1" "$2" "$3"; fail=1; fi
}
row() { printf '%s\n' "$1" | grep -m1 "^ *$2\. " | sed 's/^ *//'; }
short() { printf '%s...' "${1:0:8}"; }

check "копия правил у Зои — то же соревнование" "$ZOYA_KEY" "$CONTEST"
check "в соревновании два раунда двух организаторов" "$(printf '%s\n' "$SHOW" | after 'раундов: ')" "2"
check "себе не судья" "$(printf '%s\n' "$SELF" | grep -c 'себе не судья')" "1"
check "у Анны первый — Борис: медиана 18, 19, 18" \
      "$(row "$T_ANNA" 1)" "1. $(short "$(chain_of boris)")  засчитано 18 из заявленных 20  ·  судей 3"
check "Вера — 15: последнее слово Егора — 15" \
      "$(row "$T_ANNA" 2)" "2. $(short "$(chain_of vera)")  засчитано 15 из заявленных 15  ·  судей 3"
check "Глеб у Анны — 12 из заявленных 50" \
      "$(row "$T_ANNA" 3)" "3. $(short "$GLEB")  засчитано 12 из заявленных 50  ·  судей 3"
check "ферма в круге Анны никем не засужена" \
      "$(printf '%s\n' "$T_ANNA" | grep -o 'без вердиктов вашего круга — попыток: [0-9]*')" \
      "без вердиктов вашего круга — попыток: $FARM"
check "вердикт Жоры без согласия с правилами не засчитан" \
      "$(printf '%s\n' "$T_ANNA" | grep -o 'вердиктов: [0-9]*')" "вердиктов: 1"
check "в круге Глеба он чемпион — своя таблица" \
      "$(row "$T_GLEB" 1)" "1. вы  засчитано 50 из заявленных 50  ·  судей 2"
check "без круга фанаты сдвигают Глебу медиану: 12, 12, 13, 50, 50 → 13" \
      "$(printf '%s\n' "$T_ALL" | grep "$(short "$GLEB")" | sed 's/^ *[0-9]*\. //')" \
      "$(short "$GLEB")  засчитано 13 из заявленных 50  ·  судей 5"
check "без круга ферма попадает в таблицу" \
      "$(printf '%s\n' "$T_ALL" | grep -c 'из заявленных 99')" "$(( FARM - 1 ))"
check "оригинал видео сходится с попыткой" "$(printf '%s\n' "$V_OK" | grep -c '^тот самый файл')" "1"
check "подмена видна" "$(printf '%s\n' "$V_BAD" | grep -c '^НЕ тот файл')" "1"

hr "итог"
if [[ "$fail" == 0 ]]; then
    echo "   всё сошлось."
else
    echo "   ЕСТЬ РАСХОЖДЕНИЯ (см. выше)"; exit 1
fi
