#!/usr/bin/env bash
# ═════════════════════════════════════════════════════════════════════════════
# БОЛЬШОЕ ДЕМО: городок «Заречье» — все функции системы вживую.
#
# demo.sh проигрывает один узкий сюжет (амортизация + один расчёт). Этот скрипт —
# его расширение: целый городок из ~14 разнообразных людей (у каждого свой
# блокчейн), и через них проходит ВЕСЬ набор возможностей CLI `bc` и агрегатора:
#
#   1.  identity            — перепись: у каждого своя цепь
#   2.  concept / apply     — профили (навыки, потребности, стремления), в т.ч.
#                             через делегированного писаря (apply --draft)
#   3.  catalog / directory — каталоги и поиск людей
#   4.  match               — СТЫКОВКА: кольца обмена, дефицит, переобучение
#   5.  tool / material     — средства производства и амортизация (ИР-011)
#   6.  work / accept / pay — труд, приёмка, расчёт трудочасами; перенос стоимости
#   7.  deal                — полный цикл сделки (берусь → найм → работа → расчёт)
#   8.  transfer            — движение бумаги: своя эмиссия, оборот, погашение
#   9.  wallet / rates      — кошельки, долг, сетевые ставки
#  10.  merge / discover    — взаимное заверение, растущий граф (DAG), кеш участников
#  11.  seal                — пломбы (слепые и открытые)
#  12.  pledge / ideas top  — финансирование идей трудочасами
#  13.  trust / chain info  — экономический след (слои 1–3, ИР-010)
#  14.  concept link / copy — граф знаний: связи, композиты, копии, реакции
#  15.  revoke              — компрометация ключа: отзыв, замена, сертификат
#  16.  fraud / cache       — инструменты обвинений и сырой набор участников
#  17.  branch / block stub — ветки из любого узла (фирма/отдел), якорь времени
#
# Самодостаточно: поднимает свой агрегатор на отдельном порту, работает во
# временной папке, всё за собой убирает. Ничего в системе не трогает.
#
# Запуск:   docs/pilot/grand-demo.sh          (собери проект: cmake --build build)
#           BUILD=/path/to/build PORT=19191 docs/pilot/grand-demo.sh
#
# В конце проверяет ключевые числа и падает с ненулевым кодом при расхождении —
# годится как сквозной дымовой тест.
# ═════════════════════════════════════════════════════════════════════════════
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
PORT="${PORT:-19191}"
VIA="http://localhost:$PORT"

# ── Временная песочница и уборка ─────────────────────────────────────────────
work="$(mktemp -d)"
agg_pid=""
cleanup() {
    [[ -n "$agg_pid" ]] && kill "$agg_pid" 2>/dev/null || true
    rm -rf "$work"
}
trap cleanup EXIT

# ── Оформление вывода ────────────────────────────────────────────────────────
act()  { printf '\n\033[1;36m╔══ %s\033[0m\n' "$1"; }
hr()   { printf '\n\033[1m── %s ──\033[0m\n' "$1"; }
say()  { printf '   %s\n' "$*"; }

# bc от лица человека: bcp <кто> <аргументы...>
bcp()  { local who="$1"; shift; "$BC" --data-dir "$work/$who" "$@"; }
# Вырезать «...МЕТКА<то, что после>» из stdin.
after(){ sed -n "s/.*$1//p" | head -1; }
uid()  { bcp "$1" identity show | after 'User ID: '; }
short(){ printf '%s…' "${1:0:12}"; }

# ═════════════════════════════════════════════════════════════════════════════
act "0. Агрегатор — общая доска, релей, раздатчик каталогов ($VIA)"
"$AGG" --port "$PORT" --db "$work/agg" --catalog "$CATALOGS" >"$work/agg.log" 2>&1 &
agg_pid=$!
for _ in $(seq 1 40); do curl -sf -o /dev/null "$VIA/catalog" && break || sleep 0.25; done
say "поднят (pid $agg_pid), каталоги профессий и потребностей загружены"

# ═════════════════════════════════════════════════════════════════════════════
act "1. Перепись Заречья — 14 человек, у каждого свой блокчейн (identity create)"
# who  русское имя               роль
people="anna boris vera dmitry irina petr raisa semen tatyana fedor galina oleg pelageya ustin"
declare -A NAME=(
  [anna]="Анна" [boris]="Борис" [vera]="Вера" [dmitry]="Дмитрий" [irina]="Ирина"
  [petr]="Пётр" [raisa]="Раиса" [semen]="Семён" [tatyana]="Татьяна" [fedor]="Фёдор"
  [galina]="Галина" [oleg]="Олег" [pelageya]="Пелагея" [ustin]="Устин")
declare -A ROLE=(
  [anna]="повар" [boris]="электрик" [vera]="швея" [dmitry]="столяр" [irina]="юрист"
  [petr]="врач" [raisa]="преподаватель" [semen]="автослесарь" [tatyana]="бухгалтер"
  [fedor]="водитель" [galina]="парикмахер" [oleg]="программист" [pelageya]="медсестра"
  [ustin]="сварщик (в Твери, 150 км)")
declare -A CID
for p in $people; do
    bcp "$p" identity create >/dev/null
    CID[$p]="$(uid "$p")"
    printf '   %-9s %-14s %-26s %s…\n' "$p" "${NAME[$p]}" "${ROLE[$p]}" "${CID[$p]:0:10}"
done

# ═════════════════════════════════════════════════════════════════════════════
act "2. Профили: навыки, потребности, стремления (concept add с тегами)"
say "geo/r — ручка приватности и досягаемости; grade — разряд; remote:yes — удалённо"

# Хелпер: объявить факт профиля, вернуть REF (chain/hash).
# profile <кто> <kind> <текст> <cat-слаг> [доп.теги...]
profile() {
    local who="$1" kind="$2" text="$3" cat="$4"; shift 4
    local out h
    out="$(bcp "$who" concept add "$text" --tag "kind:$kind" --tag "cat:$cat" "$@" --via "$VIA")"
    h="$(printf '%s' "$out" | after 'hash: ')"
    printf '%s/%s' "${CID[$who]}" "$h"
}

hr "навыки (skill) — что человек умеет"
SK_anna=$(  profile anna   skill "Домашняя пекарня и кухня, 15 лет"        prof.cook          --tag geo:55.751,37.618 --tag r:25 --tag grade:5)
SK_boris=$( profile boris  skill "Электромонтаж, допуск до 1000В"          prof.electrician   --tag geo:55.760,37.610 --tag r:30 --tag grade:4)
SK_vera=$(  profile vera   skill "Пошив и ремонт одежды, закрой"           prof.tailor        --tag geo:55.745,37.630 --tag r:20 --tag grade:5)
SK_dmitry=$(profile dmitry skill "Столярка, мебель на заказ, реставрация"  prof.carpenter     --tag geo:55.740,37.640 --tag r:30 --tag grade:6)
SK_irina=$( profile irina  skill "Гражданское право, договоры, споры"      prof.lawyer        --tag remote:yes --tag r:global --tag grade:5)
SK_petr=$(  profile petr   skill "Терапевт, 20 лет практики"               prof.doctor        --tag geo:55.758,37.615 --tag r:25 --tag grade:6)
SK_raisa=$( profile raisa  skill "Математика и физика, репетитор"          prof.teacher       --tag geo:55.748,37.605 --tag r:20 --tag grade:5)
SK_semen=$( profile semen  skill "Ремонт двигателей, ходовая, диагностика" prof.auto-mechanic --tag geo:55.762,37.625 --tag r:30 --tag grade:4)
SK_galina=$(profile galina skill "Стрижки, окрашивание, укладки"           prof.hairdresser   --tag geo:55.755,37.600 --tag r:25 --tag grade:4)
SK_oleg=$(  profile oleg   skill "Бэкенд, базы данных, автоматизация"       prof.programmer    --tag remote:yes --tag r:global --tag grade:5)
SK_pelageya=$(profile pelageya skill "Процедурная сестра, уколы, перевязки" prof.nurse        --tag geo:55.753,37.612 --tag r:20 --tag grade:4)
SK_ustin=$( profile ustin  skill "Сварка аргоном, металлоконструкции"      prof.welder        --tag geo:56.850,35.900 --tag r:20 --tag grade:5)
say "объявлено 12 навыков в 10 профессиональных группах"

hr "потребности (need) — что человеку нужно; формируют стыковки и кольца"
ND_anna=$(  profile anna   need "Искрит проводка на кухне — срочно"        need.electrical      --tag geo:55.751,37.618 --tag r:25 --tag urgency:срочно)
ND_boris=$( profile boris  need "Порвалась рабочая роба, нужен пошив"      need.clothing        --tag geo:55.760,37.610 --tag r:30)
ND_vera=$(  profile vera   need "Просела дверь шкафа, нужен столяр"        need.housing-repair  --tag geo:55.745,37.630 --tag r:20)
ND_dmitry=$(profile dmitry need "Обеды на бригаду в цех"                   need.food            --tag geo:55.740,37.640 --tag r:30)
ND_irina=$( profile irina  need "Плановый осмотр у терапевта"             need.health          --tag remote:yes --tag r:global)
ND_petr=$(  profile petr   need "Подтянуть сына по физике перед ЕГЭ"       need.education       --tag geo:55.758,37.615 --tag r:25)
ND_raisa=$( profile raisa  need "Составить договор аренды кабинета"        need.legal           --tag geo:55.748,37.605 --tag r:20)
ND_semen=$( profile semen  need "Квартальная отчётность ИП"               need.accounting      --tag geo:55.762,37.625 --tag r:30)
ND_tatyana=$(profile tatyana need "Стучит подвеска, нужен автослесарь"    need.vehicle-repair  --tag remote:yes --tag r:global)
ND_fedor=$( profile fedor  need "Не крутит барабан у стиралки"            need.appliance-repair --tag geo:55.750,37.590 --tag r:40 --tag urgency:срочно)
ND_galina=$(profile galina need "Перевезти кресло и мойку в салон"        need.transport       --tag geo:55.755,37.600 --tag r:25)
ND_oleg=$(  profile oleg   need "Профосмотр для справки"                  need.health          --tag geo:55.759,37.616 --tag r:15)
ND_pelageya=$(profile pelageya need "Готовые обеды на смену"              need.food            --tag geo:55.753,37.612 --tag r:20)
ND_ustin=$( profile ustin  need "Нужен электрик на подключение станка"    need.electrical      --tag geo:56.850,35.900 --tag r:20)
say "объявлено 14 потребностей"

hr "стремления (aspiration): кто готов переучиться — закрыть дефицит"
profile fedor aspiration "Готов освоить ремонт бытовой техники" prof.appliance-repair --tag retrain:yes >/dev/null
profile oleg  aspiration "Могу переучиться на ремонт техники"    prof.appliance-repair --tag retrain:yes >/dev/null
say "Фёдор и Олег готовы переучиться на prof.appliance-repair"

hr "делегированное авторство: писарь готовит черновик, ХОЗЯИН подписывает (bc apply)"
say "черновик не несёт ключей и полномочий — до подписи это просто текст"
cat > "$work/semen-draft.json" <<'JSON'
{"records":[
 {"t":"concept","text":"Шиномонтаж и сезонное хранение резины",
  "tags":["kind:skill","cat:prof.auto-mechanic","geo:55.762,37.625","r:30","grade:4"]},
 {"t":"concept","text":"Нужен репетитор сыну по математике",
  "tags":["kind:need","cat:need.education","geo:55.762,37.625","r:15"]}
]}
JSON
say "1) Семён смотрит, что подсунули, ничего не подписывая (--dry-run):"
bcp semen apply --draft "$work/semen-draft.json" --dry-run | sed 's/^/     /'
say "2) и только теперь подписывает своим ключом — по блоку на запись:"
bcp semen apply --draft "$work/semen-draft.json" --yes --via "$VIA" | grep -E 'block #|Подписано' | sed 's/^/     /'

# ═════════════════════════════════════════════════════════════════════════════
act "3. Каталог и поиск — как люди находят друг друга"
hr "поиск слага в каталоге (bc catalog --search)"
bcp anna catalog --via "$VIA" --search столяр
hr "кто умеет чинить электрику (bc directory)"
bcp irina directory --skill prof.electrician --via "$VIA" \
    | python3 -c 'import sys,json; d=json.load(sys.stdin); [print("   •",c["text"],"—",",".join(t for t in c["tags"] if t.startswith(("geo","r:","grade")))) for c in d["chains"]]'
hr "открытые потребности по здоровью (bc needs list --need)"
bcp irina needs list --need need.health --via "$VIA" \
    | python3 -c 'import sys,json; d=json.load(sys.stdin); [print("   •",c["text"]) for c in d["chains"]]' 2>/dev/null || true

# ═════════════════════════════════════════════════════════════════════════════
act "4. СТЫКОВКА (bc match) — ради чего всё: кольца, дефицит, переобучение"
say "агрегатор сверяет потребности с навыками и проверяет ДОСЯГАЕМОСТЬ (geo+r)"
say "Устин в Твери (150 км) — по географии выпадет из локальных стыковок"
echo
bcp anna match --via "$VIA"

# ═════════════════════════════════════════════════════════════════════════════
act "5. Средства производства и амортизация (ИР-011): печь и мука"
say "Анне-повару нужны разряд и средства производства — они переносят стоимость"
SPEC_anna=$( bcp anna specialty add prof.cook --via "$VIA" | after 'hash: ')
GRADE_anna=$(bcp anna grade add "${CID[anna]}/$SPEC_anna" 5 | after 'hash: ')
say "специальность prof.cook, разряд 5"

TOOL=$(bcp anna tool add "Печь подовая" --cost 315 --life 8000 --serial PP-2019-0417 \
       --note "120000руб / 380руб-ч ≈ 315ч" --via "$VIA" | after 'tool ref: ')
TOOL_H="${TOOL#*/}"
MAT=$(bcp anna material add "Мука пшеничная" --unit кг --cost 4 --qty 50 \
      --note "2100руб / 525руб-ч = 4ч" --via "$VIA" | after 'material ref: ')
MAT_H="${MAT#*/}"
say "печь: 315ч / ресурс 8000ч   ($(short "$TOOL_H"))"
say "мука:  4ч / партия 50 кг     ($(short "$MAT_H"))"

# ═════════════════════════════════════════════════════════════════════════════
act "6. Анна печёт для Дмитрия (need.food) — труд + перенос стоимости"
say "6ч работы; печь 6 моточасов и мука 12 кг переносят свою стоимость в хлеб"
WORK_anna=$(bcp anna work log --agent "${CID[anna]}/$GRADE_anna" \
            --action "выпечка обедов для бригады" --hours 6 --output "хлеб:24:шт" \
            --tool "${TOOL_H:0:12}:6" --material "${MAT_H:0:12}:12" --via "$VIA" | after 'hash: ')
say "печь → 6/8000 × 315 = 0.23625ч ;  мука → 12/50 × 4 = 0.96ч"

hr "аудит нити переноса печи (bc tool show)"
bcp anna tool show "${TOOL_H:0:12}"

hr "Дмитрий принимает работу — приёмка сама суммирует труд и перенос (bc accept)"
bcp dmitry fetch "${CID[anna]}/$WORK_anna" --via "$VIA" >/dev/null
bcp dmitry fetch "$TOOL" --via "$VIA" >/dev/null
bcp dmitry fetch "$MAT"  --via "$VIA" >/dev/null
ACC_anna=$(bcp dmitry accept --work "${CID[anna]}/$WORK_anna" --quality "пройдено" --coef 1.0 --via "$VIA" | after 'acceptance ref: ')

hr "Дмитрий расплачивается трудочасами (bc pay) — потолок = 6 + 0.23625 + 0.96"
PAY1="$(bcp dmitry pay --acceptance "$ACC_anna" --via "$VIA" 2>&1)"
echo "$PAY1" | sed 's/^/   /'
XFER1="$(printf '%s' "$PAY1" | after 'transfer ref: ')"
paid1="$(printf '%s' "$PAY1" | grep -o 'paid [0-9.]*' | head -1 | awk '{print $2}')"
# Анна принимает бумагу Дмитрия к себе в кошелёк (bc transfer recv)
bcp anna transfer recv "$XFER1" --via "$VIA" >/dev/null
say "Анна держит бумагу Дмитрия на $paid1ч (bc transfer recv)"

# ═════════════════════════════════════════════════════════════════════════════
act "7. Сделка полного цикла (bc deal): Анна ← Борис закрывает need.electrical"
say "берусь → найм → работа → приёмка → расчёт; ссылки подставляются сами"
SPEC_boris=$( bcp boris specialty add prof.electrician --via "$VIA" | after 'hash: ')
GRADE_boris=$(bcp boris grade add "${CID[boris]}/$SPEC_boris" 4 | after 'hash: ')

hr "Борис берётся (bc deal take)"
bcp boris deal take "$ND_anna" --via "$VIA" | sed 's/^/   /'
hr "Анна нанимает Бориса на 3ч (bc deal hire)"
bcp anna deal hire "$ND_anna" --executor "${CID[boris]}" --units 3 --via "$VIA" | sed 's/^/   /'
hr "Борис делает работу и привязывает её к потребности (bc deal work)"
WORK_boris=$(bcp boris deal work "$ND_anna" --hours 3 --action "Замена проводки на кухне" \
             --agent "${CID[boris]}/$GRADE_boris" --via "$VIA" | after 'работа: ' | awk '{print $1}')
hr "Анна принимает и платит — бумагой Дмитрия, которую держит (оборот!)"
bcp anna fetch "$WORK_boris" --via "$VIA" >/dev/null
ACC_boris=$(bcp anna accept --work "$WORK_boris" --quality "аккуратно" --coef 1.0 --via "$VIA" | after 'acceptance ref: ')
PAY2="$(bcp anna pay --acceptance "$ACC_boris" --via "$VIA" 2>&1)"
echo "$PAY2" | sed 's/^/   /'
XFER2="$(printf '%s' "$PAY2" | after 'transfer ref: ')"
bcp boris transfer recv "$XFER2" --via "$VIA" >/dev/null
say "→ Анна расплатилась бумагой Дмитрия: она пошла по кругу дальше, к Борису"

# ═════════════════════════════════════════════════════════════════════════════
act "8. Кольцо замыкается: Борис → Вера → Дмитрий, бумага возвращается эмитенту"
SPEC_vera=$( bcp vera specialty add prof.tailor --via "$VIA" | after 'hash: ')
GRADE_vera=$(bcp vera grade add "${CID[vera]}/$SPEC_vera" 5 | after 'hash: ')
SPEC_dmitry=$( bcp dmitry specialty add prof.carpenter --via "$VIA" | after 'hash: ')
GRADE_dmitry=$(bcp dmitry grade add "${CID[dmitry]}/$SPEC_dmitry" 6 | after 'hash: ')

hr "Вера чинит робу Бориса (need.clothing) — сделка, оплата бумагой Дмитрия"
bcp vera deal take "$ND_boris" --via "$VIA" >/dev/null
bcp boris deal hire "$ND_boris" --executor "${CID[vera]}" --units 2 --via "$VIA" >/dev/null
WORK_vera=$(bcp vera deal work "$ND_boris" --hours 2 --action "Пошив рабочей робы" \
            --agent "${CID[vera]}/$GRADE_vera" --via "$VIA" | after 'работа: ' | awk '{print $1}')
bcp boris fetch "$WORK_vera" --via "$VIA" >/dev/null
ACC_vera=$(bcp boris accept --work "$WORK_vera" --quality "по фигуре" --coef 1.0 --via "$VIA" | after 'acceptance ref: ')
PAY3="$(bcp boris pay --acceptance "$ACC_vera" --via "$VIA" 2>&1)"
echo "$PAY3" | sed 's/^/   /'
XFER3="$(printf '%s' "$PAY3" | after 'transfer ref: ')"
bcp vera transfer recv "$XFER3" --via "$VIA" >/dev/null

hr "Дмитрий-столяр чинит шкаф Веры (need.housing-repair) — со своими станком и досками"
TOOL2=$(bcp dmitry tool add "Верстак с оснасткой" --cost 200 --life 5000 --serial VS-2021 \
        --note "оценка владельца" --via "$VIA" | after 'tool ref: ')
TOOL2_H="${TOOL2#*/}"
MAT2=$(bcp dmitry material add "Доска мебельная" --unit шт --cost 3 --qty 30 \
       --note "оценка партии" --via "$VIA" | after 'material ref: ')
MAT2_H="${MAT2#*/}"
WORK_dmitry=$(bcp dmitry work log --agent "${CID[dmitry]}/$GRADE_dmitry" \
              --action "Ремонт дверцы шкафа" --hours 5 --output "шкаф:1:шт" \
              --tool "${TOOL2_H:0:12}:5" --material "${MAT2_H:0:12}:10" --via "$VIA" | after 'hash: ')
# привязать работу к потребности Веры
bcp dmitry concept link "${CID[dmitry]}/$WORK_dmitry" "$ND_vera" --kind исполняет --via "$VIA" >/dev/null 2>&1 || true
bcp vera fetch "${CID[dmitry]}/$WORK_dmitry" --via "$VIA" >/dev/null
bcp vera fetch "$TOOL2" --via "$VIA" >/dev/null
bcp vera fetch "$MAT2"  --via "$VIA" >/dev/null
ACC_dmitry=$(bcp vera accept --work "${CID[dmitry]}/$WORK_dmitry" --quality "как новый" --coef 1.0 --via "$VIA" | after 'acceptance ref: ')
hr "Вера платит Дмитрию — его же бумагой: эмитент ГАСИТ свой долг (bc pay)"
PAY4="$(bcp vera pay --acceptance "$ACC_dmitry" --via "$VIA" 2>&1)"
echo "$PAY4" | sed 's/^/   /'
XFER4="$(printf '%s' "$PAY4" | after 'transfer ref: ')"
bcp dmitry transfer recv "$XFER4" --via "$VIA" >/dev/null
say "→ бумага Дмитрия прошла Анна→Борис→Вера→Дмитрий и частично погашена у эмитента"

# ═════════════════════════════════════════════════════════════════════════════
act "9. Отдельное кольцо через полный bc deal settle: Ирина ← Пётр (need.health)"
SPEC_petr=$( bcp petr specialty add prof.doctor --via "$VIA" | after 'hash: ')
GRADE_petr=$(bcp petr grade add "${CID[petr]}/$SPEC_petr" 6 | after 'hash: ')
bcp petr  deal take "$ND_irina" --via "$VIA" >/dev/null
bcp irina deal hire "$ND_irina" --executor "${CID[petr]}" --units 1 --via "$VIA" >/dev/null
WORK_petr=$(bcp petr deal work "$ND_irina" --hours 1 --action "Плановый осмотр" \
            --agent "${CID[petr]}/$GRADE_petr" --via "$VIA" | after 'работа: ' | awk '{print $1}')
bcp irina fetch "$WORK_petr" --via "$VIA" >/dev/null
bcp irina accept --work "$WORK_petr" --quality "здорова" --coef 1.0 --via "$VIA" >/dev/null
hr "bc deal settle — платит по приёмке и СПРАШИВАЕТ, закрыта ли потребность"
bcp irina deal settle "$ND_irina" --yes --via "$VIA" | sed 's/^/   /'

# ═════════════════════════════════════════════════════════════════════════════
act "10. Явный перевод и кошельки (bc transfer send / wallet)"
say "Пелагея-медсестра принимает работу Галины и платит прямым переводом"
SPEC_galina=$( bcp galina specialty add prof.hairdresser --via "$VIA" | after 'hash: ')
GRADE_galina=$(bcp galina grade add "${CID[galina]}/$SPEC_galina" 4 | after 'hash: ')
WORK_galina=$(bcp galina work log --agent "${CID[galina]}/$GRADE_galina" \
              --action "Стрижка перед сменой" --hours 1 --via "$VIA" | after 'hash: ')
bcp pelageya fetch "${CID[galina]}/$WORK_galina" --via "$VIA" >/dev/null
ACC_galina=$(bcp pelageya accept --work "${CID[galina]}/$WORK_galina" --quality "аккуратно" --coef 1.0 --via "$VIA" | after 'acceptance ref: ')
hr "bc transfer send --reason (часы движутся только против принятого труда)"
bcp pelageya transfer send --to "${CID[galina]}" --units 1 --reason "$ACC_galina" --via "$VIA" | sed 's/^/   /'
hr "кошелёк Анны (держит чужую бумагу) и Дмитрия (свой долг в обороте)"
say "Анна:";   bcp anna   wallet | sed 's/^/     /'
say "Дмитрий:"; bcp dmitry wallet | sed 's/^/     /'

hr "сетевые ставки по специальностям (bc rates — подписанный DailyAggregate)"
bcp anna rates --via "$VIA" | python3 -m json.tool 2>/dev/null | head -20 || bcp anna rates --via "$VIA"

# ═════════════════════════════════════════════════════════════════════════════
act "11. Взаимное заверение — растущий граф синхронизации, DAG (bc merge)"
say "две ветки сливаются попарно; каждый merge несёт снимок охваченных участников"
hr "кому сливаться в первую очередь (bc discover — по графу обмена)"
bcp anna discover --via "$VIA" \
  | python3 -c 'import sys,json; d=json.load(sys.stdin); [print("   •",c["chain"][:12]+"…","score %.1f"%c["score"],"объём обмена %.0fч"%c["econ_volume"]) for c in d[:5]]' 2>/dev/null || true

# merge_pair <A> <B>: B слушает один раз, A инициирует
merge_pair() {
    local a="$1" b="$2"
    bcp "$b" merge serve --via "$VIA" --once --timeout 30 >"$work/serve_$b.log" 2>&1 &
    local sv=$!
    sleep 0.6
    bcp "$a" merge run --peer "${CID[$b]}" --via "$VIA" --depth 1 --timeout 30 >"$work/run_$a.log" 2>&1 || true
    wait "$sv" 2>/dev/null || true
    local root
    root="$(after 'union root: ' <"$work/run_$a.log")"
    printf '   ✓ %-8s ⟷ %-8s   union root: %s…\n' "${NAME[$a]}" "${NAME[$b]}" "${root:0:16}"
}
hr "рост графа: 6 попарных слияний (порядок растёт вдоль цепочки)"
merge_pair anna  boris
merge_pair boris vera
merge_pair vera  dmitry
merge_pair dmitry petr
merge_pair petr  irina
merge_pair anna  galina

hr "сырой набор участников в кеше (bc cache list — из него строятся доказательства)"
bcp anna cache list | sed 's/^/   /' | head -18

# ═════════════════════════════════════════════════════════════════════════════
act "12. Пломбы (bc seal) — «я видел это состояние»"
hr "слепая пломба на корневой блок Анны (подтверждает существование, не содержимое)"
ANNA_ROOT="${SK_anna#*/}"
bcp boris seal add "$ANNA_ROOT" --via "$VIA" | sed 's/^/   /'
bcp vera  seal add "$ANNA_ROOT" --via "$VIA" >/dev/null
say "Вера тоже поставила пломбу"
hr "список пломб на блок (bc seal list — подтягивает чужие и проверяет подписи)"
bcp anna seal list "$ANNA_ROOT" --via "$VIA" | sed 's/^/   /'

# ═════════════════════════════════════════════════════════════════════════════
act "13. Финансирование идей трудочасами (bc pledge / ideas top)"
IDEA=$(bcp dmitry concept add "Общая мастерская Заречья: станки в складчину" --tag kind:idea --via "$VIA" | after 'hash: ')
IDEA_REF="${CID[dmitry]}/$IDEA"
say "Дмитрий объявил идею общей мастерской"
hr "жители обещают труд (bc pledge add)"
bcp anna  pledge add --target "$IDEA_REF" --units 5 --via "$VIA" | sed 's/^/   /'
bcp boris pledge add --target "$IDEA_REF" --units 4 --via "$VIA" >/dev/null
PL_vera_ref=$(bcp vera pledge add --target "$IDEA_REF" --units 3 --via "$VIA" | after 'pledge ref: ')
say "Борис +4ч, Вера +3ч"
hr "обещания Веры и отзыв неисполненного остатка (bc pledge list / revoke)"
bcp vera pledge list | sed 's/^/   /'
bcp vera pledge revoke --pledge "$PL_vera_ref" | sed 's/^/   /'
hr "доска финансирования (bc ideas top — сколько труда обещано под идею)"
bcp anna ideas top --via "$VIA" \
  | python3 -c 'import sys,json; d=json.load(sys.stdin); [print("   •",i["text"][:40],"— обещано %.0fч от %d чел."%(i["pledged_active"],i["pledgers"])) for i in d]' 2>/dev/null || true

# ═════════════════════════════════════════════════════════════════════════════
act "14. Экономический след (bc trust) — слои 1–3 (ИР-010)"
say "Анна смотрит на Дмитрия: она держала его бумагу и сливалась с ним — видит нить"
echo
bcp anna trust "${CID[dmitry]}" --via "$VIA"
hr "экономическое досье цепи (bc chain info — сырые числа)"
bcp anna chain info "${CID[dmitry]}" --via "$VIA" | python3 -m json.tool 2>/dev/null | sed 's/^/   /' \
  || bcp anna chain info "${CID[dmitry]}" --via "$VIA"

# ═════════════════════════════════════════════════════════════════════════════
act "15. Граф знаний (bc concept link / composite / copy / react)"
hr "композит: Дмитрий группирует свою оснастку (bc composite add)"
bcp dmitry composite add "Оснастка мастерской" --part "$TOOL2" --part "$MAT2" | sed 's/^/   /'
hr "копия чужой записи к себе (bc copy — Галина сохраняет навык Анны)"
bcp galina copy "${CID[anna]}/$ANNA_ROOT" --via "$VIA" 2>&1 | sed 's/^/   /' | head -3 || true
hr "реакция на работу (+100) от врача Петра (bc react)"
bcp petr react "$WORK_anna" --value 100 --chain "${CID[anna]}" --via "$VIA" 2>&1 | sed 's/^/   /' | head -3

# ═════════════════════════════════════════════════════════════════════════════
act "16. Компрометация ключа: отзыв и замена (bc revoke)"
say "У Олега украли рабочий ключ глубокой ветки. Ключ-предок (холоднее) его отзывает."
hr "отзыв со стоп-краном и заменой + самопроверяемый сертификат (bc revoke create)"
bcp oleg revoke create --node 2147483647 --replace --out "$work/oleg.cert" --via "$VIA" | sed 's/^/   /'
hr "сертификат проверяется автономно кем угодно (bc revoke verify)"
bcp anna revoke verify --cert "$work/oleg.cert" | sed 's/^/   /'
hr "статус ветки и зоны блоков (bc revoke status)"
bcp oleg revoke status --node 2147483647 | sed 's/^/   /'
hr "другой человек тянет сертификаты Олега со склада (bc revoke fetch)"
bcp anna revoke fetch "${CID[oleg]}" --via "$VIA" 2>&1 | sed 's/^/   /' | head -4 || true

# ═════════════════════════════════════════════════════════════════════════════
act "17. Инструменты обвинений и ветки на любых узлах"
hr "проверка обвинения самодостаточна — на подделанном proof она отвергает (bc fraud verify)"
bcp anna fraud verify --kind bad_sig \
    --proof 00 --merkle-root 0000000000000000000000000000000000000000000000000000000000000000 \
    2>&1 | sed 's/^/   /' | head -4 || true
hr "ветка из высокого узла = уровень «фирма/отдел» (bc branch init + block stub)"
say "Дмитрий заводит служебную ветку узла 2 и ставит якорь времени"
bcp dmitry branch init 2 2>&1 | sed 's/^/   /' | head -1 || true
bcp dmitry block stub --leaf 2 | sed 's/^/   /'
say "низкие индексы — служебные ветки высокого уровня, глубокие — личные/канальные"

# ═════════════════════════════════════════════════════════════════════════════
act "ПРОВЕРКИ (сквозной дымовой тест)"
fail=0
check() { if [[ "$2" == "$3" ]]; then printf '   ✓ %s\n' "$1"
          else printf '   ✗ %s: получили «%s», ждали «%s»\n' "$1" "$2" "$3"; fail=1; fi; }
checkn(){ if awk "BEGIN{exit !($2 $3)}"; then printf '   ✓ %s (%s)\n' "$1" "$2"
          else printf '   ✗ %s: %s не %s\n' "$1" "$2" "$3"; fail=1; fi; }

# перепись
check "заведено 14 цепей" "$(echo "$people" | wc -w | tr -d ' ')" "14"
# амортизация: оплата ровно по потолку labor + перенос печи + перенос муки
check "оплата = 6 + 0.23625 + 0.96" "$paid1" "7.19625"
# кольца обмена нашлись
MATCH="$(bcp anna match --via "$VIA")"
if grep -q "КОЛЬЦА ОБМЕНА" <<<"$MATCH"; then echo "   ✓ стыковка нашла кольца обмена"; else echo "   ✗ кольца не найдены"; fail=1; fi
# дефицит appliance-repair виден и есть кому переучиться
if grep -q "appliance-repair" <<<"$MATCH"; then echo "   ✓ дефицит appliance-repair виден в стыковке"; else echo "   ✗ дефицит не показан"; fail=1; fi
# оборот бумаги Дмитрия: эмитировал и хоть что-то погасил (кольцо замкнулось)
DINFO="$(bcp anna chain info "${CID[dmitry]}" --via "$VIA")"
issued="$(python3 -c "import json,sys;print(json.load(sys.stdin)['issued'])" <<<"$DINFO")"
redeemed="$(python3 -c "import json,sys;print(json.load(sys.stdin)['redeemed'])" <<<"$DINFO")"
checkn "Дмитрий эмитировал бумагу" "$issued" "> 0"
checkn "бумага Дмитрия частично вернулась к нему" "$redeemed" "> 0"
# отзыв ключа сработал
if bcp oleg revoke status --node 2147483647 | grep -q "REPLACED"; then
    echo "   ✓ ветка Олега помечена REPLACED после отзыва"; else echo "   ✗ отзыв не отражён"; fail=1; fi
# повторная оплата обязана отклониться (потолок исчерпан)
if bcp dmitry pay --acceptance "$ACC_anna" --via "$VIA" >/dev/null 2>&1; then
    echo "   ✗ повторная оплата прошла, а должна была отклониться"; fail=1
else echo "   ✓ повторная оплата отклонена (потолок §12.8)"; fi
# переоценка печи ВВЕРХ обязана отклониться (перевыпуск только вниз)
if bcp anna tool add "Печь подовая" --cost 400 --life 8000 --origin "$TOOL" --note "хочу дороже" >/dev/null 2>&1; then
    echo "   ✗ переоценка вверх прошла, а должна была отклониться"; fail=1
else echo "   ✓ переоценка вверх отклонена (перевыпуск только вниз, §10.2)"; fi

act "ИТОГ"
if [[ "$fail" == 0 ]]; then echo "   всё сошлось — весь круг возможностей отработал."
else echo "   ЕСТЬ РАСХОЖДЕНИЯ (см. выше)"; exit 1; fi
