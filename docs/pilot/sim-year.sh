#!/usr/bin/env bash
# ═════════════════════════════════════════════════════════════════════════════
# ГОД ЖИЗНИ ПОПУЛЯЦИИ — статистический прогон.
#
# Моделирует ~200 человек (у каждого свой блокчейн) на протяжении года: каждый
# и заказывает труд (эмитирует свою бумагу, растит долг), и работает (принимает
# чужую бумагу). Бумага ходит по кругу и частично гасится у эмитентов — за год
# экономика балансируется, и «Экономический след» (ИР-010) у каждого набирает
# содержание. Параллельно растёт граф взаимных заверений (DAG), ставятся пломбы,
# финансируются идеи, случаются компрометации ключей.
#
# Время идёт по-настоящему: каждый месяц блоки штампуются своей датой через
# faketime (реальные монотонные часы не подменяются — таймауты слияний целы).
# Поэтому в следе «последняя эмиссия N дней назад», timestamp_witnessed и
# распределение активности по календарю — настоящие.
#
# На выходе (в $OUT, по умолчанию docs/pilot/sim-out/):
#   • sim-dataset.json — население, экономика по каждому, ставки, стыковка,
#       распределения — готовый корм для презентации;
#   • sim-stats.txt    — та же сводка, что печатается в терминал.
#
# Параметры (env):
#   POP=200  MONTHS=12  MERGES_PER_MONTH=12  SEALS_PER_MONTH=6  SEED=42
#   YEAR=2025  PORT=19300  OUT=<путь>  BUILD=<путь к build>
#
# Прогон ~200 человек — единицы−десятки минут. Для отладки: POP=12 MONTHS=2.
#
# ВНИМАНИЕ: нужен faketime (apt-get install faketime).
# ═════════════════════════════════════════════════════════════════════════════
set -uo pipefail   # без -e: единичный сбой сделки не должен рушить весь прогон

# ── Параметры ────────────────────────────────────────────────────────────────
POP="${POP:-200}"
MONTHS="${MONTHS:-12}"
MERGES_PER_MONTH="${MERGES_PER_MONTH:-12}"
SEALS_PER_MONTH="${SEALS_PER_MONTH:-6}"
NGROUPS="${NGROUPS:-16}"    # торговых групп (плотных локальных экономик)
SEED="${SEED:-42}"
YEAR="${YEAR:-2025}"
PORT="${PORT:-19300}"
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD="${BUILD:-$here/../../build}"
OUT="${OUT:-$here/sim-out}"
BC="$BUILD/modules/cli/bc"
AGG="$BUILD/modules/aggregator/aggregator_server"
CATALOGS="$here/../catalogs"
VIA="http://localhost:$PORT"

command -v faketime >/dev/null || { echo "нужен faketime: apt-get install faketime" >&2; exit 1; }
for bin in "$BC" "$AGG"; do
    [[ -x "$bin" ]] || { echo "не найден $bin — соберите: cmake --build \"$BUILD\"" >&2; exit 1; }
done
export FAKETIME_DONT_FAKE_MONOTONIC=1   # только realtime; монотонные часы (таймауты) реальны

# ── Песочница цепей (временная); OUT — постоянный ────────────────────────────
work="$(mktemp -d)"
agg_pid=""
cleanup(){ [[ -n "$agg_pid" ]] && kill "$agg_pid" 2>/dev/null; rm -rf "$work"; }
trap cleanup EXIT
mkdir -p "$OUT/econ"
rm -f "$OUT"/econ/*.json 2>/dev/null

log(){ printf '%s %s\n' "$(date +%H:%M:%S)" "$*"; }
after(){ sed -n "s/.*$1//p" | head -1; }

# ── faketime-обёртка: FT задаётся перед каждой фазой ─────────────────────────
FT=(faketime "$YEAR-01-15 12:00:00")
ftbc(){ local who="$1"; shift; "${FT[@]}" "$BC" --data-dir "$work/$who" "$@"; }
month_date(){ date -u -d "$YEAR-01-15 +$1 months" '+%Y-%m-%d %H:%M:%S'; }
set_month(){ FT=(faketime "$(month_date "$1")"); }

# ── Детерминированная перестановка 0..n-1 по сид ──────────────────────────────
perm(){ awk -v n="$1" -v s="$2" 'BEGIN{srand(s);for(i=0;i<n;i++)a[i]=i;
        for(i=n-1;i>0;i--){j=int(rand()*(i+1));t=a[i];a[i]=a[j];a[j]=t}
        for(i=0;i<n;i++)print a[i]}'; }
RANDOM=$SEED
rnd(){ echo $(( RANDOM % $1 )); }        # 0..n-1

# ── Справочники: профессии (закрывающие нужды — с весом ×2) и нужды ───────────
# appliance-repair НЕ включён в население → структурный дефицит + переобучение.
PROFPOOL=(
  prof.cook prof.cook prof.farmer prof.farmer prof.tailor prof.tailor
  prof.builder prof.builder prof.carpenter prof.carpenter prof.electrician prof.electrician
  prof.plumber prof.plumber prof.auto-mechanic prof.auto-mechanic prof.driver prof.driver
  prof.teacher prof.teacher prof.doctor prof.doctor prof.nurse prof.nurse
  prof.lawyer prof.lawyer prof.accountant prof.accountant
  prof.hairdresser prof.programmer prof.designer prof.photographer prof.sysadmin prof.welder prof.scribe )
NEEDPOOL=(
  need.food need.clothing need.housing-repair need.electrical need.plumbing
  need.vehicle-repair need.transport need.education need.health need.legal
  need.accounting need.childcare need.housing
  need.appliance-repair need.appliance-repair need.leisure )   # 2 последних — дефицит
# профессии, работающие удалённо (география игнорируется)
declare -A REMOTE=( [prof.lawyer]=1 [prof.accountant]=1 [prof.programmer]=1 [prof.designer]=1 [prof.sysadmin]=1 [prof.scribe]=1 )
# 5 районов города (кластеры досягаемости) + разброс
CLAT=(55.75 55.80 55.70 55.73 55.78); CLON=(37.60 37.55 37.65 37.50 37.62)
FIRST=(Анна Борис Вера Глеб Дарья Егор Жанна Захар Инна Кирилл Лада Максим Нина Олег Пётр Рита Семён Тая Устин Фёкла Юрий Яна Лев Мила Тимур Соня Артём Вика Денис Полина)

# ═════════════════════════════════════════════════════════════════════════════
log "▶ агрегатор (реальное время) на $VIA"
"$AGG" --port "$PORT" --db "$work/agg" --catalog "$CATALOGS" >"$work/agg.log" 2>&1 &
agg_pid=$!
for _ in $(seq 1 60); do curl -sf -o /dev/null "$VIA/catalog" && break || sleep 0.25; done

# ── Население ────────────────────────────────────────────────────────────────
log "▶ создаю население: $POP человек (identity + профиль + разряд), январь"
set_month 0
declare -a CID PROF GRADEREF SKREF GEOTXT NAME LVL CLUSTER
: > "$OUT/roster.tsv"
setup_fail=0
for ((i=0;i<POP;i++)); do
    who="p$i"
    ftbc "$who" identity create >/dev/null 2>&1
    CID[$i]="$("$BC" --data-dir "$work/$who" identity show | after 'User ID: ')"
    if [[ -z "${CID[$i]}" ]]; then ((setup_fail++)); PROF[$i]=""; continue; fi
    prof="${PROFPOOL[$(rnd ${#PROFPOOL[@]})]}"; PROF[$i]="$prof"
    lvl=$(( 3 + $(rnd 4) )); LVL[$i]=$lvl
    NAME[$i]="${FIRST[$(( i % ${#FIRST[@]} ))]}-$i"
    # география (район, досягаемость) и торговая группа — РАЗНЫЕ оси:
    #   район даёт reachability для стыковки; плотная торговая группа —
    #   повторные сделки за год → возврат бумаги эмитенту → баланс.
    gd=$(rnd 5)                       # 1 из 5 районов
    CLUSTER[$i]=$(rnd "$NGROUPS")     # торговая группа
    if [[ -n "${REMOTE[$prof]:-}" && $(rnd 2) -eq 0 ]]; then
        geo=(--tag remote:yes --tag r:global); GEOTXT[$i]="remote"
    else
        lat=$(awk -v b="${CLAT[$gd]}" -v j="$(rnd 60)" 'BEGIN{printf "%.3f", b+(j-30)/1000}')
        lon=$(awk -v b="${CLON[$gd]}" -v j="$(rnd 60)" 'BEGIN{printf "%.3f", b+(j-30)/1000}')
        r=$(( 6 + $(rnd 12) )); geo=(--tag "geo:$lat,$lon" --tag "r:$r"); GEOTXT[$i]="$lat,$lon/r$r"
    fi
    # навык (профиль)
    SKREF[$i]="${CID[$i]}/$(ftbc "$who" concept add "${prof#prof.} — опыт $((3+$(rnd 15))) лет" \
        --tag kind:skill --tag "cat:$prof" "${geo[@]}" --tag "grade:$lvl" --via "$VIA" | after 'hash: ')"
    # потребность
    need="${NEEDPOOL[$(rnd ${#NEEDPOOL[@]})]}"
    urg=(); [[ $(rnd 4) -eq 0 ]] && urg=(--tag urgency:срочно)
    ftbc "$who" concept add "нужно: ${need#need.}" --tag kind:need --tag "cat:$need" "${geo[@]}" "${urg[@]}" --via "$VIA" >/dev/null 2>&1
    # стремление переучиться на дефицитную профессию (у части)
    [[ $(rnd 3) -eq 0 ]] && ftbc "$who" concept add "готов освоить ремонт техники" \
        --tag kind:aspiration --tag cat:prof.appliance-repair --tag retrain:yes --via "$VIA" >/dev/null 2>&1
    # специальность + разряд (чтобы каждый мог работать)
    sp="$(ftbc "$who" specialty add "$prof" --via "$VIA" --force | after 'hash: ')"
    GRADEREF[$i]="${CID[$i]}/$(ftbc "$who" grade add "${CID[$i]}/$sp" "$lvl" --via "$VIA" | after 'hash: ')"
    printf '%d\t%s\t%s\t%d\t%s\t%s\n' "$i" "${CID[$i]}" "$prof" "$lvl" "${GEOTXT[$i]}" "${NAME[$i]}" >> "$OUT/roster.tsv"
    (( i % 25 == 0 )) && log "   … $i/$POP"
done
log "   население готово (сбоев setup: $setup_fail)"

# состав районов (для локальной торговли — повторяющиеся пары гасят бумагу)
declare -A CLIST
for ((i=0;i<POP;i++)); do [[ -n "${PROF[$i]}" ]] && CLIST[${CLUSTER[$i]}]+=" $i"; done

# ── Идеи для финансирования (3 штуки) ────────────────────────────────────────
declare -a IDEAREF
for j in 0 1 2; do
    o=$(( (j*37+5) % POP ))
    IDEAREF[$j]="${CID[$o]}/$(ftbc "p$o" concept add "общий проект #$j: мастерская/склад/транспорт" --tag kind:idea --via "$VIA" | after 'hash: ')"
done

# ── Одна сделка: заказчик c нанимает работника w на h часов ──────────────────
ok_deals=0; fail_deals=0; total_hours=0
do_deal(){
    local c="$1" w="$2" h="$3"
    [[ -z "${PROF[$c]}" || -z "${PROF[$w]}" || "$c" == "$w" ]] && return
    local wh acc xf coef units
    # Ставка ЦЕНТРИРОВАНА НА ЕДИНИЦЕ: премия за разряд вокруг среднего разряда 4.5
    # (разряд 3 → 0.85, 6 → 1.15) + шум. При равномерных разрядах взвешенное по
    # часам среднее ≈ 1.0, т.е. за час труда в среднем создаётся ровно час бумаги —
    # эмиссия растёт в ногу с реально отработанными часами, без встроенной инфляции.
    # Это выбор ГЕНЕРАТОРА демо-данных; нормирует ли ставки сам протокол —
    # открытый вопрос экономики (обсуждается, см. docs/economy.md).
    coef="$(awk -v l="${LVL[$w]}" -v j="$(rnd 17)" 'BEGIN{printf "%.2f", 1.0+0.10*(l-4.5)+(j-8)/100}')"
    units="$(awk -v c="$coef" -v h="$h" 'BEGIN{printf "%.4f", c*h}')"
    wh="$(ftbc "p$w" work log --agent "${GRADEREF[$w]}" --action "услуга" --hours "$h" --via "$VIA" | after 'hash: ')"
    [[ -z "$wh" ]] && { ((fail_deals++)); return; }
    ftbc "p$c" fetch "${CID[$w]}/$wh" --via "$VIA" >/dev/null 2>&1 || { ((fail_deals++)); return; }
    acc="$(ftbc "p$c" accept --work "${CID[$w]}/$wh" --quality ok --labor-units "$units" --via "$VIA" | after 'acceptance ref: ')"
    [[ -z "$acc" ]] && { ((fail_deals++)); return; }
    xf="$(ftbc "p$c" pay --acceptance "$acc" --via "$VIA" | after 'transfer ref: ')"
    [[ -z "$xf" ]] && { ((fail_deals++)); return; }
    ftbc "p$w" transfer recv "$xf" --via "$VIA" >/dev/null 2>&1
    ((ok_deals++)); total_hours=$(( total_hours + h ))
}

# ── Одно слияние: b слушает раз, a инициирует ────────────────────────────────
ok_merges=0
merge_pair(){
    local a="$1" b="$2"
    [[ -z "${CID[$a]}" || -z "${CID[$b]}" || "$a" == "$b" ]] && return
    ftbc "p$b" merge serve --via "$VIA" --once --timeout 25 >"$work/mserve.log" 2>&1 &
    local sv=$!; sleep 0.4
    ftbc "p$a" merge run --peer "${CID[$b]}" --via "$VIA" --depth 1 --timeout 25 >"$work/mrun.log" 2>&1
    wait "$sv" 2>/dev/null
    grep -q "Merge complete" "$work/mrun.log" && ((ok_merges++))
}

# ── Годовой цикл ─────────────────────────────────────────────────────────────
ok_seals=0; ok_pledges=0
for ((m=0;m<MONTHS;m++)); do
    set_month "$m"
    md="$(month_date "$m")"
    # экономика: торговля ПРЕИМУЩЕСТВЕННО внутри района — пары повторяются за год,
    # бумага возвращается эмитентам (погашение), замкнутость района растёт.
    for ((kc=0;kc<NGROUPS;kc++)); do
        mapfile -t M < <(for x in ${CLIST[$kc]:-}; do echo "$x"; done \
            | awk -v s=$(( SEED + m*131 + kc*17 )) 'BEGIN{srand(s)} {a[NR]=$0}
                END{for(i=NR;i>1;i--){j=int(rand()*i)+1;t=a[i];a[i]=a[j];a[j]=t} for(i=1;i<=NR;i++)print a[i]}')
        for ((q=0;q+1<${#M[@]};q+=2)); do
            c="${M[$q]}"; w="${M[$((q+1))]}"
            (( m % 2 == 1 )) && { t="$c"; c="$w"; w="$t"; }   # роли чередуются по месяцам
            do_deal "$c" "$w" "$(( 1 + $(rnd 8) ))"
        done
    done
    # немного межрайонных сделок — утечка «наружу», чтобы замкнутость была < 100%
    for ((s=0; s<POP/12+1; s++)); do do_deal "$(rnd "$POP")" "$(rnd "$POP")" "$(( 1 + $(rnd 8) ))"; done
    # слияния (рост DAG)
    for ((s=0;s<MERGES_PER_MONTH;s++)); do merge_pair "$(rnd "$POP")" "$(rnd "$POP")"; done
    # пломбы
    for ((s=0;s<SEALS_PER_MONTH;s++)); do
        a="$(rnd "$POP")"; b="$(rnd "$POP")"
        [[ -n "${SKREF[$b]:-}" ]] && ftbc "p$a" seal add "${SKREF[$b]#*/}" --via "$VIA" >/dev/null 2>&1 && ((ok_seals++))
    done
    # обещания под идеи
    for ((s=0;s<4;s++)); do
        a="$(rnd "$POP")"; j="$(rnd 3)"
        ftbc "p$a" pledge add --target "${IDEAREF[$j]}" --units "$(( 1 + $(rnd 5) ))" --via "$VIA" >/dev/null 2>&1 && ((ok_pledges++))
    done
    log "▶ месяц $((m+1))/$MONTHS ($md): сделок ok=$ok_deals fail=$fail_deals · слияний=$ok_merges · пломб=$ok_seals · обещаний=$ok_pledges"
done

# ── Компрометации ключей (несколько в конце года) ────────────────────────────
set_month "$((MONTHS-1))"
ok_revokes=0
for s in 0 1 2 3 4; do
    a=$(( (s*41+7) % POP ))
    [[ -z "${CID[$a]}" ]] && continue
    ftbc "p$a" revoke create --node 2147483647 --replace --via "$VIA" >/dev/null 2>&1 && ((ok_revokes++))
done
log "▶ отзывов ключей: $ok_revokes"

# ═════════════════════════════════════════════════════════════════════════════
log "▶ сбор статистики (взгляд из конца декабря — «N дней назад» настоящие)"
FT=(faketime "$YEAR-12-28 12:00:00")
# профили, стыковка — одним запросом каждый
ftbc p0 export profiles --via "$VIA" --out "$OUT/profiles.json" >/dev/null 2>&1
ftbc p0 match --via "$VIA" --json > "$OUT/match.json" 2>/dev/null
# ставки помесячно: preview-эндпоинт считает ставки за конкретный день по его
# сделкам (?day=TS). Блоки каждого месяца на одну дату → получаем ряд по году.
{
  echo "{"
  for ((m=0;m<MONTHS;m++)); do
    ts=$(date -u -d "$(month_date "$m")" +%s)
    r="$(curl -s "$VIA/economy/rates?day=$ts")"
    printf '"%s": %s' "$(date -u -d "@$ts" +%Y-%m)" "${r:-null}"
    (( m<MONTHS-1 )) && echo "," || echo ""
  done
  echo "}"
} > "$OUT/rates_by_month.json"
# экономическое досье по каждому
for ((i=0;i<POP;i++)); do
    [[ -z "${CID[$i]}" ]] && continue
    ftbc p0 chain info "${CID[$i]}" --via "$VIA" > "$OUT/econ/$i.json" 2>/dev/null
done
# размер DAG — по кешу активного участника
ftbc p0 cache list > "$OUT/cache0.txt" 2>/dev/null

# показательный САМЫЙ ПОЛНЫЙ след: эмитировал (есть нить, слой 1) И погасил
# (кольцо замкнулось) И его труд принимали — все три слоя содержательны.
TOP3="$(python3 - "$OUT" <<'PY'
import json,sys,glob,os
out=sys.argv[1]; rich=[]; any_issue=[]
for f in glob.glob(os.path.join(out,'econ','*.json')):
    try: d=json.load(open(f))
    except: continue
    idx=os.path.splitext(os.path.basename(f))[0]
    iss=d.get('issued',0); red=d.get('redeemed',0); rec=d.get('received',0)
    if iss>0: any_issue.append((iss+rec,idx))
    if iss>0 and red>0 and rec>0: rich.append((iss+rec+red*5, idx))   # погашение — в приоритете
rich.sort(reverse=True); any_issue.sort(reverse=True)
pick=[i for _,i in rich[:3]] or [i for _,i in any_issue[:3]]
print(' '.join(pick))
PY
)"

# ── Сборка датасета и сводки (python) ────────────────────────────────────────
python3 - "$OUT" "$POP" "$MONTHS" "$SEED" "$YEAR" "$ok_deals" "$fail_deals" "$total_hours" "$ok_merges" "$ok_seals" "$ok_pledges" "$ok_revokes" > "$OUT/sim-stats.txt" <<'PY'
import json,sys,glob,os,statistics as st
out,POP,MONTHS,SEED,YEAR = sys.argv[1],int(sys.argv[2]),int(sys.argv[3]),int(sys.argv[4]),int(sys.argv[5])
ok_deals,fail_deals,total_hours,ok_merges,ok_seals,ok_pledges,ok_revokes = map(int,sys.argv[6:13])

roster={}
for line in open(os.path.join(out,'roster.tsv')):
    p=line.rstrip('\n').split('\t')
    if len(p)>=6: roster[p[0]]={'chain':p[1],'prof':p[2],'grade':int(p[3]),'geo':p[4],'name':p[5]}

econ=[]
for f in glob.glob(os.path.join(out,'econ','*.json')):
    idx=os.path.splitext(os.path.basename(f))[0]
    try: d=json.load(open(f))
    except: continue
    r=roster.get(idx,{})
    rec={'idx':int(idx),'chain':r.get('chain'),'prof':r.get('prof'),'grade':r.get('grade'),
         'geo':r.get('geo'),'name':r.get('name')}
    rec.update({k:d.get(k,0) for k in ['debt','issued','redeemed','received','spent',
               'works_accepted','labor_appraised','pledges_active','pledges_settled']})
    rec['redemption_ratio']=round(rec['redeemed']/rec['issued'],3) if rec['issued'] else 0.0
    econ.append(rec)
econ.sort(key=lambda x:x['idx'])

def load(name):
    try: return json.load(open(os.path.join(out,name)))
    except: return None
rates_by_month=load('rates_by_month.json') or {}; match=load('match.json')
# кольца обмена при плотной сети комбинаторно многочисленны — храним счётчик и образец
rings_total=0
if isinstance(match,dict) and match.get('rings') is not None:
    rings_total=len(match['rings']); match['rings']=match['rings'][:100]; match['rings_total']=rings_total
# последний месяц, где ставки посчитались — как «текущие» ставки года
rates_latest=[]; rates_latest_month=None
for mo in sorted(rates_by_month):
    rr=(rates_by_month[mo] or {}).get('rates') or []
    if rr: rates_latest, rates_latest_month = rr, mo

# ИНДЕКС СРЕДНЕГО ТРУДОЧАСА: взвешенное по часам среднее ставок за месяц.
# =1.0 → за час труда создаётся ровно час бумаги (эмиссия в ногу с работой);
# >1 → инфляция трудочаса, <1 → дефляция. Считается из публичных сделок.
rates_index={}
for mo in sorted(rates_by_month):
    rr=(rates_by_month[mo] or {}).get('rates') or []
    den=sum(x.get('hours',0) for x in rr)
    if den: rates_index[mo]=round(sum(x.get('rate',0)*x.get('hours',0) for x in rr)/den,4)

# распределения
def hist(vals,edges):
    h=[0]*(len(edges)+1)
    for v in vals:
        placed=False
        for i,e in enumerate(edges):
            if v<=e: h[i]+=1; placed=True; break
        if not placed: h[-1]+=1
    return h
issued=[e['issued'] for e in econ]; received=[e['received'] for e in econ]
debt=[e['debt'] for e in econ]; rr=[e['redemption_ratio'] for e in econ]
by_prof={}
for e in econ:
    b=by_prof.setdefault(e['prof'],{'n':0,'issued':0.0,'received':0.0,'labor':0.0})
    b['n']+=1; b['issued']+=e['issued']; b['received']+=e['received']; b['labor']+=e['labor_appraised']
for b in by_prof.values():
    n=b['n'] or 1
    b['avg_issued']=round(b['issued']/n,2); b['avg_received']=round(b['received']/n,2)
    b['avg_labor']=round(b['labor']/n,2)

active=[e for e in econ if e['issued']>0 or e['received']>0]
dataset={
 'meta':{'population':POP,'active':len(active),'months':MONTHS,'seed':SEED,'year':YEAR,
         'deals_ok':ok_deals,'deals_fail':fail_deals,'labor_hours_transacted':total_hours,
         'merges':ok_merges,'seals':ok_seals,'pledges':ok_pledges,'revocations':ok_revokes},
 'population':[roster[k] for k in sorted(roster,key=int)],
 'economy':econ,
 'rates_by_month':rates_by_month,'rates_latest_month':rates_latest_month,
 'rates_index':rates_index,'match':match,
 'distributions':{
   'issued_hist_edges':[0,5,15,30,60,120],'issued_hist':hist(issued,[0,5,15,30,60,120]),
   'received_hist_edges':[0,5,15,30,60,120],'received_hist':hist(received,[0,5,15,30,60,120]),
   'redemption_ratio_hist_edges':[0,0.1,0.25,0.5,0.75,1.0],
   'redemption_ratio_hist':hist(rr,[0.0001,0.1,0.25,0.5,0.75,1.0]),
   'by_profession':by_prof,
 },
}
json.dump(dataset,open(os.path.join(out,'sim-dataset.json'),'w'),ensure_ascii=False,indent=1)

# ── человекочитаемая сводка ──
def bar(n,mx,w=32):
    return '█'*int(round(w*n/mx)) if mx else ''
P=print
P("═"*66); P(f"  ГОД ЖИЗНИ ПОПУЛЯЦИИ — сводка ({YEAR}, {POP} человек, {MONTHS} мес.)"); P("═"*66)
m=dataset['meta']
P(f"  активны экономически: {m['active']}/{POP}")
P(f"  сделок: {m['deals_ok']} (сбоев {m['deals_fail']}) · трудочасов проведено: {m['labor_hours_transacted']}")
P(f"  слияний (DAG): {m['merges']} · пломб: {m['seals']} · обещаний: {m['pledges']} · отзывов ключей: {m['revocations']}")
if issued:
    P(f"  бумаги эмитировано всего: {sum(issued):.0f}ч · погашено: {sum(e['redeemed'] for e in econ):.0f}ч "
      f"({100*sum(e['redeemed'] for e in econ)/ (sum(issued) or 1):.0f}%)")
    P(f"  медиана долга: {st.median(debt):.2f}ч · медиана принятой бумаги: {st.median(received):.2f}ч")

P("\n  РАСПРЕДЕЛЕНИЕ ЭМИССИИ (сколько своей бумаги в обороте, ч):")
edges=dataset['distributions']['issued_hist_edges']; hh=dataset['distributions']['issued_hist']
labels=[f"≤{edges[0]:g}"]+[f"{edges[i-1]:g}–{edges[i]:g}" for i in range(1,len(edges))]+[f">{edges[-1]:g}"]
mx=max(hh) or 1
for l,n in zip(labels,hh): P(f"    {l:>10} │{bar(n,mx)} {n}")

P("\n  РАСПРЕДЕЛЕНИЕ ПОГАШЕНИЯ (redeemed/issued — «сколько доверили и вернул»):")
edges=dataset['distributions']['redemption_ratio_hist_edges']; hh=dataset['distributions']['redemption_ratio_hist']
labels=["0"]+[f"≤{edges[i]:g}" for i in range(1,len(edges))]+[">"+f"{edges[-1]:g}"]
mx=max(hh) or 1
for l,n in zip(labels,hh): P(f"    {l:>10} │{bar(n,mx)} {n}")

P("\n  ПО ПРОФЕССИЯМ (средняя эмиссия / принятая бумага / оценка труда, ч):")
P(f"    {'профессия':<20}{'n':>4}{'эмис.':>9}{'принял':>9}{'труд':>9}")
for prof,b in sorted(by_prof.items(),key=lambda kv:-kv[1]['avg_received']):
    P(f"    {prof:<20}{b['n']:>4}{b['avg_issued']:>9}{b['avg_received']:>9}{b['avg_labor']:>9}")

if rates_latest:
    P(f"\n  СЕТЕВЫЕ СТАВКИ на {rates_latest_month} (ч оплаты за час труда, по специальности×разряду):")
    for r in sorted(rates_latest,key=lambda r:-r.get('rate',0))[:10]:
        P(f"    {r.get('specialty',''):<20} разряд {r.get('level','?')}  ставка {r.get('rate',0):.3f}  "
          f"(сделок {r.get('deals',0)}, ч {r.get('hours',0):.0f})")
    months_with_rates=sum(1 for mo in rates_by_month if (rates_by_month[mo] or {}).get('rates'))
    P(f"    (ставки посчитаны для {months_with_rates} мес. из {MONTHS})")
    lo=min(r.get('rate',0) for r in rates_latest); hi=max(r.get('rate',0) for r in rates_latest)
    P(f"    размах в этом месяце: {lo:.3f} … {hi:.3f} — есть и ниже, и выше единицы")

if rates_index:
    P("\n  ИНДЕКС СРЕДНЕГО ТРУДОЧАСА по месяцам (взвешенное по часам среднее ставок):")
    P("    =1.000 → за час труда создаётся ровно час бумаги; >1 инфляция, <1 дефляция")
    vals=list(rates_index.values())
    for mo,v in rates_index.items():
        dev=int(round((v-1.0)*200))          # 1 символ ≈ 0.5%
        bar=('·'*20)+'│'+('·'*20)
        pos=max(0,min(40,20+dev))
        bar=bar[:pos]+'●'+bar[pos+1:]
        P(f"    {mo}  {v:6.3f}  {bar}")
    P(f"    среднее за год: {sum(vals)/len(vals):.4f}  (размах {min(vals):.3f}…{max(vals):.3f})")

if match and isinstance(match,dict):
    needs=match.get('needs') or []
    closed=sum(1 for n in needs if n.get('candidates'))
    defs=match.get('deficits') or []
    P(f"\n  СТЫКОВКА: закрыто {closed} из {len(needs)} потребностей · дефицитов: {len(defs)}")
    P(f"    колец обмена (замкнутых цепочек помощи): {rings_total} — сеть плотно связана")
    if defs:
        P("    дефицитные профессии: "+", ".join(sorted({d.get('need','') for d in defs})[:8]))

top=sorted(econ,key=lambda e:-e['received'])[:8]
P("\n  ТОП-8 ПО ПРИНЯТОЙ БУМАГЕ (им больше всех доверили труд):")
for e in top:
    P(f"    {e['name']:<12} {e['prof']:<18} принял {e['received']:>7.2f}ч · эмитировал {e['issued']:>7.2f}ч · долг {e['debt']:>7.2f}ч")
P("═"*66)
PY
cat "$OUT/sim-stats.txt"

# ── Показать полный «Экономический след» трёх самых доверенных ────────────────
echo
echo "────────────────────────────────────────────────────────────────"
echo "  ПОКАЗАТЕЛЬНЫЙ ЭКОНОМИЧЕСКИЙ СЛЕД (bc trust) — три самых доверенных"
echo "────────────────────────────────────────────────────────────────"
for i in $TOP3; do
    [[ -z "${CID[$i]:-}" ]] && continue
    echo; echo "### ${NAME[$i]} (${PROF[$i]}) — взгляд на собственный след"
    ftbc "p$i" trust "${CID[$i]}" --via "$VIA" 2>/dev/null
done

log "▶ готово. Датасет: $OUT/sim-dataset.json ; сводка: $OUT/sim-stats.txt"
