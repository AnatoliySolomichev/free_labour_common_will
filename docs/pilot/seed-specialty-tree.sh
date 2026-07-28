#!/usr/bin/env bash
# ─────────────────────────────────────────────────────────────────────────────
# Служебная цепь с ДЕРЕВОМ СПЕЦИАЛЬНОСТЕЙ в блоках (прототип ИР-017).
#
# Заводит блокчейн «служебного аккаунта» и записывает в него каталог профессий
# как дерево: каждая запись — обычный Concept с тегами
#     kind:catalog-entry · cat:<слаг> · parent:<слаг-родителя> · alias:<синоним>…
# Корень `prof` → категории `prof.*` → специальности. Синонимы (`alias:`) — та
# самая связка «синонимы → канон»: разные имена ведут к одному слагу.
#
# Источник дерева — docs/catalogs/professions.json (пока это bootstrap-форма,
# ИР-007). Скрипт показывает КАНОНИЧЕСКУЮ форму — тот же каталог, но в цепи,
# подписанный ключом служебного аккаунта. Это и есть «единая точка входа в
# дерево» из графа записей, которую можно синхронизировать между блокчейнами.
#
# Запуск:
#   docs/pilot/seed-specialty-tree.sh                 # демо: temp-цепь, показать, убрать
#   docs/pilot/seed-specialty-tree.sh --keep DIR      # завести НАСТОЯЩИЙ аккаунт в DIR
#                                                       (ключи остаются у вас — как всегда)
#   docs/pilot/seed-specialty-tree.sh --keep DIR --via URL   # ещё и выгрузить на агрегатор
#
# ВНИМАНИЕ: ключи служебного аккаунта — как любые ключи, их никто не отдаёт.
# Кто держит ключ этой цепи, тот и ведёт канонический каталог этого источника.
# ─────────────────────────────────────────────────────────────────────────────
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD="${BUILD:-$here/../../build}"
BC="$BUILD/modules/cli/bc"
CATALOG_JSON="$here/../catalogs/professions.json"
[[ -x "$BC" ]] || { echo "не найден $BC — соберите: cmake --build \"$BUILD\"" >&2; exit 1; }
[[ -f "$CATALOG_JSON" ]] || { echo "не найден $CATALOG_JSON" >&2; exit 1; }

# ── Разбор аргументов ────────────────────────────────────────────────────────
KEEP=""; VIA=""
while [[ $# -gt 0 ]]; do
    case "$1" in
        --keep) KEEP="${2:?--keep требует путь}"; shift 2;;
        --via)  VIA="${2:?--via требует URL}"; shift 2;;
        *) echo "неизвестный аргумент: $1" >&2; exit 1;;
    esac
done

if [[ -n "$KEEP" ]]; then DIR="$KEEP"; else DIR="$(mktemp -d)"; trap 'rm -rf "$DIR"' EXIT; fi
mkdir -p "$DIR"

hr(){ printf '\n\033[1m── %s ──\033[0m\n' "$1"; }

# ── Служебный аккаунт ────────────────────────────────────────────────────────
if [[ ! -d "$DIR/keys" ]]; then
    hr "создаю служебный аккаунт в $DIR"
    "$BC" --data-dir "$DIR" identity create >/dev/null
fi
SID="$("$BC" --data-dir "$DIR" identity show | sed -n 's/.*User ID: //p')"
echo "   служебная цепь: ${SID:0:16}…"

# ── Записываю дерево из JSON в блоки цепи ─────────────────────────────────────
hr "пишу дерево специальностей в блоки (kind:catalog-entry)"
count=0
# Каждая строка: слаг \t parent \t ru \t alias,alias,… \t axis=val,axis=val,…
while IFS=$'\t' read -r slug parent ru aliases axes; do
    [[ -z "$slug" ]] && continue
    args=( --data-dir "$DIR" concept add "$ru" --tag kind:catalog-entry --tag "cat:$slug" )
    [[ -n "$parent" && "$parent" != "-" ]] && args+=( --tag "parent:$parent" )
    if [[ -n "$aliases" && "$aliases" != "-" ]]; then
        IFS=',' read -ra AL <<< "$aliases"
        for a in "${AL[@]}"; do [[ -n "$a" ]] && args+=( --tag "alias:$a" ); done
    fi
    # координаты облака: axis:material/info/people/danger (ИР-018)
    if [[ -n "$axes" && "$axes" != "-" ]]; then
        IFS=',' read -ra AX <<< "$axes"
        for kv in "${AX[@]}"; do [[ -n "$kv" ]] && args+=( --tag "axis:$kv" ); done
    fi
    [[ -n "$VIA" ]] && args+=( --via "$VIA" )
    "$BC" "${args[@]}" >/dev/null
    count=$((count+1))
done < <(python3 -c '
import json,sys
d=json.load(open(sys.argv[1],encoding="utf-8"))
for e in d["entries"]:
    ax=e.get("axes")
    axs=",".join(f"{k}={v}" for k,v in ax.items()) if ax else "-"
    print("\t".join([e["slug"], e.get("parent","-"), e["ru"], ",".join(e.get("aliases",[])) or "-", axs]))
' "$CATALOG_JSON")
echo "   записано узлов дерева: $count"

# ── Показываю дерево, восстановленное ИЗ ЦЕПИ ────────────────────────────────
hr "дерево, прочитанное обратно из блоков цепи (bc list → реконструкция)"
"$BC" --data-dir "$DIR" list | python3 -c '
import sys,re
# из строк bc list достаём cat:/parent: тегов каждого catalog-entry
nodes={}; kids={}
for ln in sys.stdin:
    if "catalog-entry" not in ln: continue
    tags=re.search(r"tags:\[([^\]]*)\]", ln)
    if not tags: continue
    t=tags.group(1).split(",")
    slug=parent=None; ru=re.search(r"\"([^\"]*)\"", ln); ru=ru.group(1) if ru else ""
    al=[]
    for x in t:
        if x.startswith("cat:"): slug=x[4:]
        elif x.startswith("parent:"): parent=x[7:]
        elif x.startswith("alias:"): al.append(x[6:])
    if slug:
        nodes[slug]=(ru,al); kids.setdefault(parent,[]).append(slug)
def show(s,ind=0):
    ru,al=nodes.get(s,("",[])); tail=("  ["+", ".join(al)+"]") if al else ""
    print("   "+"  "*ind+"• "+s+"  "+ru+tail)
    for c in sorted(kids.get(s,[])): show(c,ind+1)
roots=[s for s in nodes if nodes and s not in sum(kids.values(),[])]
for r in sorted(set(kids.get(None,[])) or roots): show(r)
'

hr "синонимы → канон (проверка одной ветки)"
"$BC" --data-dir "$DIR" list | python3 -c '
import sys,re
for ln in sys.stdin:
    if "catalog-entry" not in ln: continue
    if "alias:электромонтёр" in ln or "cat:prof.electrician" in ln:
        tags=re.findall(r"(cat:[^,\]]+|alias:[^,\]]+)", ln)
        print("   электрик:", " · ".join(tags)); break
'
echo "   → «электромонтёр», «электромонтажник» ведут к канону cat:prof.electrician"

# ── Облако: читаю КООРДИНАТЫ из блоков и считаю ближайших соседей ─────────────
hr "облако профессий из блоков цепи: координаты + ближайший сосед (ИР-018)"
"$BC" --data-dir "$DIR" list | python3 -c '
import sys,re,math
pts={}
for ln in sys.stdin:
    if "catalog-entry" not in ln: continue
    tags=re.search(r"tags:\[([^\]]*)\]", ln)
    if not tags: continue
    slug=None; ax={}
    for x in tags.group(1).split(","):
        if x.startswith("cat:"): slug=x[4:]
        elif x.startswith("axis:") and "=" in x:
            k,v=x[5:].split("=",1)
            try: ax[k]=float(v)
            except: pass
    if slug and ax: pts[slug]=ax
keys=["material","info","people","danger"]
def vec(a): return [a.get(k,0.0) for k in keys]
def dist(a,b): return math.sqrt(sum((x-y)**2 for x,y in zip(vec(a),vec(b))))
print("   деятельность            материя инфо люди опас   ближайший сосед")
for s in sorted(pts):
    a=pts[s]
    others=[(dist(a,pts[o]),o) for o in pts if o!=s]
    nn=min(others)[1] if others else "-"
    print("   %-22s  %.2f  %.2f  %.2f  %.2f   %s" % (
        s.replace("prof.",""), a.get("material",0),a.get("info",0),a.get("people",0),a.get("danger",0),
        nn.replace("prof.","")))
print()
print("   → врач рядом с преподавателем (люди+информация), программист — с бухгалтером/писарем")
print("     (чистая информация), сварщик — с электриком (материя+опасность). Всё из блоков цепи.")
'

if [[ -n "$KEEP" ]]; then
    echo
    echo "Готово. Служебный аккаунт сохранён в: $DIR"
    echo "  идентификатор: $SID"
    [[ -n "$VIA" ]] && echo "  дерево выгружено на $VIA (bc fetch ... доступно другим)"
    echo "  ключи в $DIR/keys — НИКОМУ не отдавать."
else
    echo
    echo "(демо-прогон: цепь во временной папке, будет убрана. Для своего аккаунта: --keep DIR)"
fi
