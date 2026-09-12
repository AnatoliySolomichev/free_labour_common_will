#!/usr/bin/env bash
# ═════════════════════════════════════════════════════════════════════════════
# ЗАРЕЧЬЕ И ДВЕ ДЕРЕВНИ — сюжетная симуляция осей труда (ИР-022).
#
# sim-year.sh показывает масштаб: 200 человек, год, доли осей по профессии.
# Этот прогон меньше — 34 цепи, 12 месяцев, — но каждый в нём играет сюжет, ради
# которого оси и затевались:
#
#   • задуманные цены — у каждой оси здесь есть цена часа, которую люди в
#     среднем готовы за неё платить (P ниже). Сеть её НЕ знает: восстанавливает
#     из подписанных разбивок. «Задумано — найдено» — проверка всего счёта;
#   • новая ось — Арсений заводит «Под напряжением 400 В» и называет её в щитах;
#   • названо или спрятано — Гриша делает те же щиты по той же цене, но
#     размазывает надбавку по общим осям;
#   • мастер в деревне — Кузьмич (Верхняя) единственный электрик и берёт на 35%
#     дороже, пряча надбавку; Степан (Нижняя) берёт цену по умолчанию (часы ×
#     Σ сетевых цен осей) и оформляет поездку в город за деталями отдельной
#     работой;
#   • сговор — Лёва и Мила каждый месяц платят друг другу на 30% дороже;
#   • канал — контора «Вектор» каждый месяц переплачивает бухгалтеру Зое 60%;
#   • дефицит — Трофим единственный сантехник, и на 40% дороже платят все;
#   • разовый всплеск — Лиза один раз шьёт срочное платье за двойную цену;
#   • материалы — Вера печёт хлеб: печь и мука переносят стоимость в работу;
#   • образцы — первая сделка каждого вида работ становится образцом, дальше
#     «как та» (--like); из этих семей складывается облако работ.
#
# Даты — последние MONTHS×30 суток до сегодняшнего дня: карта ренты смотрит на
# год назад от настоящего «сейчас» агрегатора. Один SEED — один журнал сделок.
#
# На выходе (OUT, по умолчанию docs/pilot/sim-out/axes-town/):
#   deals.tsv         журнал: кто, кому, что, часы, цена, сюжет, доли осей
#   prices-M.json     цены осей месяца M (превью агрегатора, с весом ИР-021)
#   ledger.json       книга осей и карта ренты
#   cloud.json        облако образцов с координатами
#   electricians.json электрики в каталоге: заявленный разряд и принятая работа
#   world.json        люди, виды работ, оси, задуманные цены, образцы
# Презентация из этого и sim-year: python3 docs/pilot/build-axes-story.py
#
#   MONTHS=12 SEED=42 PORT=19320 OUT=<путь> BUILD=<путь к build>   ~1 мин
# ═════════════════════════════════════════════════════════════════════════════
set -uo pipefail   # без -e: единичный сбой сделки не должен рушить весь прогон

MONTHS="${MONTHS:-12}"
SEED="${SEED:-42}"
PORT="${PORT:-19320}"
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD="${BUILD:-$here/../../build}"
OUT="${OUT:-$here/sim-out/axes-town}"
BC="$BUILD/modules/cli/bc"
AGG="$BUILD/modules/aggregator/aggregator_server"
CATALOGS="$here/../catalogs"
VIA="http://localhost:$PORT"

command -v faketime >/dev/null || { echo "нужен faketime: apt-get install faketime" >&2; exit 1; }
for bin in "$BC" "$AGG"; do
    [[ -x "$bin" ]] || { echo "не найден $bin — соберите: cmake --build \"$BUILD\"" >&2; exit 1; }
done
export FAKETIME_DONT_FAKE_MONOTONIC=1

work="$(mktemp -d)"
agg_pid=""
cleanup(){ [[ -n "$agg_pid" ]] && kill "$agg_pid" 2>/dev/null; rm -rf "$work"; }
trap cleanup EXIT
mkdir -p "$OUT"
rm -f "$OUT"/*.json "$OUT"/*.tsv 2>/dev/null

log(){ printf '%s %s\n' "$(date +%H:%M:%S)" "$*"; }
after(){ sed -n "s/.*$1//p" | head -1; }

# ── Случайность по SEED (почему не RANDOM и не awk — см. sim-year.sh) ────────
exec {RND_FD}< <(x=$SEED; while :; do x=$(( (x*1103515245 + 12345) % 2147483648 )); echo $(( x >> 8 )); done)
rnd(){ local x; read -r x <&"$RND_FD"; echo $(( x % $1 )); }        # 0..n-1

# ── Календарь: месяц m — полдень дня за (MONTHS−m)×30 суток до сегодняшнего ──
NOW="$(date -u +%s)"; DAY0=$(( NOW - NOW % 86400 ))
month_ts(){ echo $(( DAY0 - (MONTHS - $1) * 30 * 86400 + 43200 )); }
set_month(){ TS="$(month_ts "$1")"; FT=(faketime "$(date -u -d "@$TS" '+%Y-%m-%d %H:%M:%S')"); }
M=0; set_month 0
ftbc(){ local who="$1"; shift; "${FT[@]}" "$BC" --data-dir "$work/$who" "$@"; }

# ── Задуманные цены осей: стч за час работы, назвавшей ось ───────────────────
# Их знает только генератор. Сеть видит лишь подписанные разбивки сделок.
declare -A P=(
  [physical]=0.30 [endurance]=0.20 [precision]=0.30 [pace]=0.20
  [info]=0.25 [people]=0.25 [nature]=0.25 [energy]=0.25
  [creativity]=0.30 [decision]=0.30 [knowledge]=0.40
  [danger]=0.30 [responsibility]=0.35
  [dirt]=0.15 [confined]=0.15 [outdoor]=0.10 [mobility]=0.25
  [emotional]=0.30 [hv400]=0.45 )
PSTR=""; for a in "${!P[@]}"; do PSTR+="$a=${P[$a]} "; done

# ── Виды работ: название и оси, которые работа называет ──────────────────────
declare -A KT KA
kind(){ KT[$1]="$2"; KA[$1]="$3"; }
kind strizhka   "стрижка"                      "precision creativity people"
kind plate      "пошив платья"                 "precision creativity endurance"
kind provodka   "проводка в доме, 220 В"       "knowledge precision danger energy"
kind shchit     "щит под 400 В"                "knowledge precision danger energy hv400"
kind vodoprovod "ремонт водопровода"           "physical precision dirt confined"
kind obed       "обед на артель"               "physical pace responsibility"
kind ukhod      "уход за больным"              "responsibility emotional people"
kind urok       "урок математики"              "knowledge people emotional"
kind perevozka  "перевозка груза"              "mobility responsibility endurance"
kind otchet     "квартальный отчёт"            "knowledge precision responsibility"
kind sayt       "сайт для мастерской"          "knowledge creativity info"
kind maket      "макет вывески"                "creativity info precision"
kind pech       "кладка печи"                  "physical precision dirt outdoor knowledge"
kind hleb       "выпечка хлеба"                "physical pace precision"
kind pokos      "покос"                        "physical nature outdoor endurance"
kind skot       "уход за скотом"               "nature endurance dirt responsibility"
kind poezdka    "поездка в город за деталями"  "mobility endurance decision"
# задуманная цена часа вида работ: сумма задуманных цен его осей
law(){ awk -v axes="$1" -v ps="$PSTR" 'BEGIN{n=split(ps,a," ");for(i=1;i<=n;i++){split(a[i],kv,"=");p[kv[1]]=kv[2]}
        m=split(axes,x," ");s=0;for(i=1;i<=m;i++)s+=p[x[i]];printf "%.4f",s}'; }

# ── Люди: ключ | имя | где | слаг каталога | вид работы | заявленный разряд ─────
PEOPLE=(
  "galina|Галина|Заречье|prof.hairdresser|strizhka|4"
  "inna|Инна|Заречье|prof.hairdresser|strizhka|3"
  "liza|Лиза|Заречье|prof.tailor|plate|5"
  "boris|Борис|Заречье|prof.electrician|provodka|4"
  "viktor|Виктор|Заречье|prof.electrician|provodka|3"
  "gleb|Глеб|Заречье|prof.electrician|provodka|5"
  "arseniy|Арсений|Заречье|prof.electrician|shchit|6"
  "grisha|Гриша|Заречье|prof.electrician|shchit|6"
  "trofim|Трофим|Заречье|prof.plumber|vodoprovod|5"
  "anna|Анна|Заречье|prof.cook|obed|4"
  "semen|Семён|Заречье|prof.cook|obed|3"
  "pelageya|Пелагея|Заречье|prof.nurse|ukhod|5"
  "olga|Ольга|Заречье|prof.nurse|ukhod|4"
  "raisa|Раиса|Заречье|prof.teacher|urok|5"
  "timur|Тимур|Заречье|prof.teacher|urok|4"
  "fedor|Фёдор|Заречье|prof.driver|perevozka|4"
  "yuriy|Юрий|Заречье|prof.driver|perevozka|3"
  "zoya|Зоя|Заречье|prof.accountant|otchet|5"
  "tatyana|Татьяна|Заречье|prof.accountant|otchet|4"
  "leva|Лёва|Заречье|prof.programmer|sayt|5"
  "mila|Мила|Заречье|prof.designer|maket|4"
  "dmitriy|Дмитрий|Заречье|prof.builder|pech|5"
  "vera|Вера|Заречье|prof.cook|hleb|4"
  "vektor|Контора «Вектор»|Заречье|||"
  "kuzmich|Кузьмич|Верхняя|prof.electrician|provodka|5"
  "prokhor|Прохор|Верхняя|prof.farmer|pokos|4"
  "marfa|Марфа|Верхняя|prof.farmer|skot|4"
  "efim|Ефим|Верхняя|prof.farmer|pokos|3"
  "darya|Дарья|Верхняя|prof.farmer|skot|4"
  "stepan|Степан|Нижняя|prof.electrician|provodka|5"
  "aksinya|Аксинья|Нижняя|prof.farmer|skot|4"
  "kondrat|Кондрат|Нижняя|prof.farmer|pokos|4"
  "lukerya|Лукерья|Нижняя|prof.farmer|skot|3"
  "savva|Савва|Нижняя|prof.farmer|pokos|4"
)
declare -A NAME PLACE SLUG KIND GRADE CID
KEYS=(); CITY=(); UPPER=(); LOWER=()
for row in "${PEOPLE[@]}"; do
    IFS='|' read -r k n pl sl kd gr <<<"$row"
    KEYS+=("$k"); NAME[$k]="$n"; PLACE[$k]="$pl"; SLUG[$k]="$sl"; KIND[$k]="$kd"; GRADE[$k]="$gr"
    case "$pl" in Заречье) CITY+=("$k");; Верхняя) UPPER+=("$k");; Нижняя) LOWER+=("$k");; esac
done
# заказчики щитов: у Арсения и у Гриши — свои, по шесть
ARS_CLIENTS=(vera vektor dmitriy anna trofim fedor)
GRI_CLIENTS=(semen olga timur yuriy tatyana galina)

# случайный из массива $1, кроме перечисленных дальше
pick(){ local -n L="$1"; shift; local c
        while :; do c="${L[$(rnd ${#L[@]})]}"; [[ " $* " == *" $c "* ]] || { echo "$c"; return; }; done; }

# ═════════════════════════════════════════════════════════════════════════════
log "▶ агрегатор на $VIA"
"$AGG" --port "$PORT" --db "$work/agg" --catalog "$CATALOGS" >"$work/agg.log" 2>&1 &
agg_pid=$!
for _ in $(seq 1 60); do curl -sf -o /dev/null "$VIA/catalog" && break || sleep 0.25; done

log "▶ люди: ${#KEYS[@]} цепей и профили (разряд — лишь заявление, по нему ничего не считается)"
for k in "${KEYS[@]}"; do
    ftbc "$k" identity create >/dev/null 2>&1
    CID[$k]="$("$BC" --data-dir "$work/$k" identity show | after 'User ID: ')"
    [[ -z "${CID[$k]}" ]] && { echo "не создалась цепь $k" >&2; exit 1; }
    [[ -n "${SLUG[$k]}" ]] && ftbc "$k" concept add "${NAME[$k]}: ${KT[${KIND[$k]}]}" \
        --tag kind:skill --tag "cat:${SLUG[$k]}" --tag "grade:${GRADE[$k]}" --via "$VIA" >/dev/null 2>&1
done

log "▶ оси: пилотный словарь (seed-axes.sh) и своя ось Арсения"
AXREFS="$work/axes.tsv"
OUT="$AXREFS" BC="$BC" bash "$here/seed-axes.sh" "$work/axes" --via "$VIA" >/dev/null 2>&1 || true
[[ -s "$AXREFS" ]] || { echo "не удалось завести оси" >&2; exit 1; }
HV="$(ftbc arseniy axis-define --slug hv400 --ru "Под напряжением 400 В" \
      --description "Щиты и линии 380–400 В, где напряжение не снять. Отдельно от опасности: это умение, которому учатся годами, а не риск как таковой." \
      --via "$VIA" | after 'axis ref: ')"
[[ -z "$HV" ]] && { echo "Арсений не смог завести ось" >&2; exit 1; }
printf 'hv400\t%s\n' "$HV" >> "$AXREFS"
axis(){ awk -F'\t' -v s="$1" '$1==s{print $2}' "$AXREFS"; }

log "▶ печь Веры: 315 ч оценки, ресурс 8000 ч"
TOOL="$(ftbc vera tool add "Печь подовая" --cost 315 --life 8000 --serial PP-2019-0417 \
        --note "120000руб / 380руб-ч ≈ 315ч" --via "$VIA" | after 'tool ref: ')"
TOOL_H="${TOOL#*/}"; MAT_H=""

# ── Цена по умолчанию: часы × Σ сетевых цен осей прошлого месяца ─────────────
# Так считает `bc accept` без --labor-units (economy.md §2а); здесь цены берутся из
# превью прошлого месяца, потому что календарь симуляции — в прошлом.
default_rate(){
    local f="$OUT/prices-$((M-1)).json" paths=() a
    for a in ${KA[$1]}; do paths+=("$(axis "$a")"); done
    python3 - "$f" "$(law "${KA[$1]}")" "${paths[@]}" <<'PY'
import json, sys
f, fallback, paths = sys.argv[1], sys.argv[2], sys.argv[3:]
try:
    pr = {a['axis']: a['price'] for a in json.load(open(f)).get('axes', [])}
except Exception:
    pr = {}
print(f"{sum(pr[p] for p in paths):.4f}" if paths and all(p in pr for p in paths) else fallback)
PY
}

# ── Одна сделка ──────────────────────────────────────────────────────────────
# deal <плательщик> <работник> <вид> <часы> <множитель> <сюжет> [law|default|hidden]
#   law     — задуманная цена вида × шум торга ±6% × множитель сюжета;
#   default — цена по умолчанию из сетевых цен прошлого месяца;
#   hidden  — цена щита, а разбивка — по осям проводки (надбавка не названа).
declare -A SAMPLE SHARES     # ключ образца → приёмка-образец; → «ось=доля …»
ok=0; fail=0; hours_total=0
printf 'month\tpayer\tworker\tkind\tsample\thours\trate\tmult\ttag\tunits\tcarried\tshares\n' > "$OUT/deals.tsv"
deal(){
    local c="$1" w="$2" kd="$3" h="$4" mult="$5" tag="$6" mode="${7:-law}"
    [[ "$c" == "$w" ]] && return
    local sk="$kd" axes="${KA[$kd]}" base noise rate units wh out acc xf carried extra=() args=() shares=""
    base="$(law "${KA[$kd]}")"
    case "$mode" in
        hidden)  sk="${kd}~hidden"; axes="${KA[provodka]}" ;;
        default) base="$(default_rate "$kd")" ;;
    esac
    noise=$(( $(rnd 13) - 6 ))
    rate="$(awk -v b="$base" -v n="$noise" -v m="$mult" 'BEGIN{printf "%.4f", b*(1+n/100)*m}')"
    units="$(awk -v r="$rate" -v h="$h" 'BEGIN{printf "%.4f", r*h}')"
    [[ "$kd" == hleb ]] && extra=(--tool "${TOOL_H:0:12}:$h" --material "${MAT_H:0:12}:12")
    wh="$(ftbc "$w" work log --title "${KT[$kd]}" --action "${KT[$kd]}" --hours "$h" \
          "${extra[@]}" --via "$VIA" | after 'hash: ')"
    [[ -z "$wh" ]] && { ((fail++)); return; }
    ftbc "$c" fetch "${CID[$w]}/$wh" --via "$VIA" >/dev/null 2>&1 || { ((fail++)); return; }
    if [[ -n "${SAMPLE[$sk]:-}" ]]; then
        # «такая же работа, как та»: доли берутся у образца
        args=(--like "${SAMPLE[$sk]}" --labor-units "$units"); shares="${SHARES[$sk]}"
    else
        # образец: цена разложена пропорционально задуманным ценам названных осей;
        # последней оси — остаток, чтобы разбивка сошлась с ценой в точности
        local tot a u acc_u=0 last
        tot="$(law "$axes")"; last="${axes##* }"
        for a in $axes; do
            if [[ "$a" == "$last" ]]; then
                u="$(awk -v U="$units" -v s="$acc_u" 'BEGIN{printf "%.6f", U-s}')"
            else
                u="$(awk -v U="$units" -v p="${P[$a]}" -v t="$tot" 'BEGIN{printf "%.6f", U*p/t}')"
                acc_u="$(awk -v s="$acc_u" -v x="$u" 'BEGIN{printf "%.6f", s+x}')"
            fi
            args+=(--axis "$(axis "$a")=$u")
            shares+="$a=$(awk -v p="${P[$a]}" -v t="$tot" 'BEGIN{printf "%.6f", p/t}') "
        done
    fi
    out="$(ftbc "$c" accept --work "${CID[$w]}/$wh" --quality "принято" "${args[@]}" \
           --via "$VIA" 2>"$work/acc.err")"
    acc="$(printf '%s\n' "$out" | after 'acceptance ref: ')"
    [[ -z "$acc" ]] && { ((fail++)); sed 's/^/     accept: /' "$work/acc.err" >&2; return; }
    [[ -z "${SAMPLE[$sk]:-}" ]] && { SAMPLE[$sk]="$acc"; SHARES[$sk]="$shares"; }
    carried="$(sed -n 's/.*перенос средств производства: \([0-9.e-]*\)ч.*/\1/p' "$work/acc.err" | head -1)"
    xf="$(ftbc "$c" pay --acceptance "$acc" --via "$VIA" | after 'transfer ref: ')"
    [[ -z "$xf" ]] && { ((fail++)); return; }
    ftbc "$w" transfer recv "$xf" --via "$VIA" >/dev/null 2>&1
    ((ok++)); hours_total=$(( hours_total + h ))
    printf '%d\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' "$M" "$c" "$w" "$kd" "$sk" "$h" \
        "$rate" "$mult" "$tag" "$units" "${carried:-0}" "${shares% }" >> "$OUT/deals.tsv"
}

# ═════════════════════════════════════════════════════════════════════════════
for ((M=0; M<MONTHS; M++)); do
    set_month "$M"
    MAT="$(ftbc vera material add "Мука пшеничная" --unit кг --cost 4 --qty 50 \
           --note "партия месяца $((M+1)): 2100руб / 525руб-ч = 4ч" --via "$VIA" | after 'material ref: ')"
    MAT_H="${MAT#*/}"

    # Заречье: у каждого мастера 2–3 заказа от случайных горожан
    for w in "${CITY[@]}"; do
        case "$w" in vektor|arseniy|grisha|trofim|zoya|leva|mila) continue ;; esac
        n=$(( 2 + $(rnd 2) ))
        for ((q=0; q<n; q++)); do
            mult=1; tag=honest
            # Лиза: на пятом месяце срочное свадебное платье за двойную цену
            [[ "$w" == liza && $M -eq 4 && $q -eq 0 ]] && { mult=2.0; tag=spike; }
            deal "$(pick CITY "$w")" "$w" "${KIND[$w]}" "$(( 2 + $(rnd 6) ))" "$mult" "$tag"
        done
    done
    # Арсений: щиты у своих заказчиков — ось «Под напряжением 400 В» названа
    for q in 0 1; do deal "${ARS_CLIENTS[$(( (2*M+q) % 6 ))]}" arseniy shchit "$(( 3 + $(rnd 4) ))" 1 named; done
    # Гриша: те же щиты по той же цене, но надбавка размазана по общим осям
    for q in 0 1; do deal "${GRI_CLIENTS[$(( (2*M+q) % 6 ))]}" grisha shchit "$(( 3 + $(rnd 4) ))" 1 hidden hidden; done
    # Трофим — единственный сантехник: на 40% дороже платят все
    for q in 0 1 2; do deal "$(pick CITY trofim)" trofim vodoprovod "$(( 2 + $(rnd 5) ))" 1.40 scarcity; done
    # Зоя: два обычных заказа — и «Вектор», который переплачивает 60%
    for q in 0 1; do deal "$(pick CITY zoya vektor)" zoya otchet "$(( 3 + $(rnd 5) ))" 1 honest; done
    deal vektor zoya otchet "$(( 4 + $(rnd 4) ))" 1.60 channel
    # Лёва и Мила: по заказу со стороны — и друг другу на 30% дороже
    deal "$(pick CITY leva mila)" leva sayt "$(( 3 + $(rnd 5) ))" 1 honest
    deal "$(pick CITY mila leva)" mila maket "$(( 3 + $(rnd 5) ))" 1 honest
    deal mila leva sayt 6 1.30 collusion
    deal leva mila maket 6 1.30 collusion

    # Верхняя: Кузьмич один на деревню и прячет надбавку 35% в общих осях
    for q in 0 1; do deal "$(pick UPPER kuzmich)" kuzmich provodka "$(( 2 + $(rnd 4) ))" 1.35 village-hidden; done
    v="$(pick UPPER kuzmich)"; deal kuzmich "$v" "${KIND[$v]}" "$(( 3 + $(rnd 5) ))" 1 honest
    for q in 0 1; do v="$(pick UPPER kuzmich)"; deal "$(pick UPPER kuzmich "$v")" "$v" "${KIND[$v]}" "$(( 3 + $(rnd 5) ))" 1 honest; done
    # Нижняя: Степан берёт цену по умолчанию, а поездку за деталями оформляет работой
    for q in 0 1; do
        c="$(pick LOWER stepan)"
        deal "$c" stepan provodka "$(( 2 + $(rnd 4) ))" 1 village-default default
        deal "$c" stepan poezdka "$(( 2 + $(rnd 2) ))" 1 trip default
    done
    v="$(pick LOWER stepan)"; deal stepan "$v" "${KIND[$v]}" "$(( 3 + $(rnd 5) ))" 1 honest
    for q in 0 1; do v="$(pick LOWER stepan)"; deal "$(pick LOWER stepan "$v")" "$v" "${KIND[$v]}" "$(( 3 + $(rnd 5) ))" 1 honest; done

    # цены осей месяца — превью агрегатора по сделкам этого дня, с весом ИР-021
    curl -s "$VIA/economy/rates?day=$TS" > "$OUT/prices-$M.json"
    log "▶ месяц $((M+1))/$MONTHS ($(date -u -d "@$TS" +%Y-%m-%d)): сделок ok=$ok fail=$fail"
done

# ═════════════════════════════════════════════════════════════════════════════
log "▶ книга осей, карта ренты, облако, каталог электриков"
curl -s "$VIA/economy/axis-ledger?rent=1"              > "$OUT/ledger.json"
curl -s "$VIA/economy/cloud?coords=1"                  > "$OUT/cloud.json"
curl -s "$VIA/directory/skills/prof.electrician"       > "$OUT/electricians.json"

# состав мира — для презентации и сверки
{
  printf 'key\tname\tplace\tslug\tkind\tgrade\tchain\n'
  for k in "${KEYS[@]}"; do
      printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\n' "$k" "${NAME[$k]}" "${PLACE[$k]}" "${SLUG[$k]}" \
          "${KIND[$k]}" "${GRADE[$k]}" "${CID[$k]}"
  done
} > "$work/people.tsv"
{ for s in "${!SAMPLE[@]}"; do printf '%s\t%s\n' "$s" "${SAMPLE[$s]}"; done; } > "$work/samples.tsv"
{ for kd in "${!KT[@]}"; do printf '%s\t%s\t%s\n' "$kd" "${KT[$kd]}" "${KA[$kd]}"; done; } > "$work/kinds.tsv"
python3 - "$OUT" "$work" "$PSTR" "$MONTHS" "$SEED" "$ok" "$fail" "$hours_total" \
          "$(month_ts 0)" <<'PY'
import csv, json, os, sys
out, work, pstr, months, seed, ok, fail, hours, ts0 = sys.argv[1:10]
rows = lambda f: list(csv.reader(open(os.path.join(work, f)), delimiter='\t'))
planted = {kv.split('=')[0]: float(kv.split('=')[1]) for kv in pstr.split()}
axes = {s: {'path': p, 'planted': planted.get(s)} for s, p in rows('axes.tsv')}
people = [dict(zip(r[0], x)) for r in [rows('people.tsv')] for x in r[1:]]
kinds = {k: {'title': t, 'axes': a.split()} for k, t, a in rows('kinds.tsv')}
samples = {s: ref for s, ref in rows('samples.tsv')}
json.dump({'months': int(months), 'seed': int(seed), 'month0_ts': int(ts0),
           'deals_ok': int(ok), 'deals_fail': int(fail), 'hours': int(hours),
           'people': people, 'kinds': kinds, 'axes': axes, 'samples': samples},
          open(os.path.join(out, 'world.json'), 'w'), ensure_ascii=False, indent=1)
PY

# ── Сводка в терминал: задумано и найдено, рента сюжетов, облако ─────────────
python3 - "$OUT" <<'PY'
import json, os, sys
out = sys.argv[1]
w = json.load(open(os.path.join(out, 'world.json')))
by_path = {v['path']: k for k, v in w['axes'].items()}
found = {}
for m in range(w['months']):
    try: d = json.load(open(os.path.join(out, f'prices-{m}.json')))
    except Exception: continue
    for a in d.get('axes', []):
        s = by_path.get(a['axis'])
        if s:
            f = found.setdefault(s, [0.0, 0.0, a.get('label', s)])
            f[0] += a['price'] * a['hours']; f[1] += a['hours']
print("═" * 70)
print(f"  ЗАРЕЧЬЕ И ДВЕ ДЕРЕВНИ — {len(w['people'])} цепей, {w['months']} мес., "
      f"{w['deals_ok']} сделок (сбоев {w['deals_fail']}), {w['hours']} ч")
print("═" * 70)
print("  ЦЕНЫ ОСЕЙ: задумано → найдено сетью (среднее за прогон, по часам)")
for s, (u, h, label) in sorted(found.items(), key=lambda kv: -(kv[1][0] / kv[1][1])):
    pl = w['axes'][s]['planted']
    print(f"    {label:<26} {pl:5.2f} → {u / h:5.3f}  ({100 * (u / h / pl - 1):+5.1f}%)")
name = {p['chain']: p['name'] for p in w['people']}
led = json.load(open(os.path.join(out, 'ledger.json')))
print("\n  КАРТА РЕНТЫ (сюжеты): факт / ожидание / остаток, стч за час · кто платит превышение")
for r in sorted(led.get('rent', []), key=lambda r: -r['resid']):
    who = name.get(r['worker'], r['worker'][:8])
    if abs(r['resid']) < 0.03 and who not in ('Арсений', 'Степан'): continue
    rp = 'не судим' if r['rent_payers'] is None else f"платят {r['rent_payers']:.1f} из {r['payers']}"
    print(f"    {who:<9} {r['title'][:24]:<24} {r['fact']:.2f} / {r['expected']:.2f} / "
          f"{r['resid']:+.2f}  мес. сверх {r['periods_over']}/{r['periods_seen']} · {rp}")
cl = json.load(open(os.path.join(out, 'cloud.json')))
print(f"\n  ОБЛАКО: {len(cl.get('points', []))} образцов работ; ближайшие соседи:")
for p in cl.get('points', [])[:40]:
    nb = ', '.join(f"{n['title']} {n['w']:.2f}" for n in p['neighbors'][:2])
    print(f"    {p['title'][:28]:<28} {p['deals']:>3} сд. → {nb}")
print("═" * 70)
PY

(( fail == 0 )) || { echo "сбоев сделок: $fail" >&2; exit 1; }
log "▶ готово: $OUT"
