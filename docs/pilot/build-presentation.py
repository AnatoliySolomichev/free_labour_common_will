#!/usr/bin/env python3
"""
Собирает презентацию по результатам sim-year.sh во ВСЕХ форматах разом.

Вход:  docs/pilot/sim-out/sim-dataset.json  (создаёт sim-year.sh)
Выход (туда же, в sim-out/):
  • presentation.html      — веб-страница с графиками (самодостаточна, для публикации)
  • presentation-data.json — компактная выжимка под графики
  • sim-presentation.md    — слайды в Markdown (Marp/reveal: слайды разделены '---')
  • sim-summary.md         — короткая выжимка на одну страницу
  • economy.csv / professions.csv / rates-index.csv — для таблиц

Запуск:  python3 docs/pilot/build-presentation.py
         python3 docs/pilot/build-presentation.py --out ДРУГАЯ_ПАПКА
"""
import json, os, sys, argparse, statistics as st
from datetime import datetime

HERE = os.path.dirname(os.path.abspath(__file__))

ap = argparse.ArgumentParser()
ap.add_argument('--out', default=os.path.join(HERE, 'sim-out'))
args = ap.parse_args()
OUT = args.out
DS = os.path.join(OUT, 'sim-dataset.json')
if not os.path.exists(DS):
    sys.exit(f"нет {DS} — сначала прогоните docs/pilot/sim-year.sh")

d = json.load(open(DS))
meta = d['meta']; econ = d['economy']
dist = d['distributions']; by_prof = dist['by_profession']
rates_by_month = d.get('rates_by_month') or {}
rates_index = d.get('rates_index') or {}
match = d.get('match') or {}

# ── агрегаты ────────────────────────────────────────────────────────────────
issued_tot = sum(e['issued'] for e in econ)
redeemed_tot = sum(e['redeemed'] for e in econ)
received_tot = sum(e['received'] for e in econ)
redeem_pct = 100 * redeemed_tot / issued_tot if issued_tot else 0
with_redemption = sum(1 for e in econ if e['redeemed'] > 0)

# ставки: свод по разряду за весь год (взвешенно по часам)
grade_rates = {}
for mo, blob in rates_by_month.items():
    for r in ((blob or {}).get('rates') or []):
        g = grade_rates.setdefault(r['level'], {'w': 0.0, 'wh': 0.0, 'vals': []})
        g['w'] += r['rate'] * r['hours']; g['wh'] += r['hours']; g['vals'].append(r['rate'])
grades = []
for lvl in sorted(grade_rates):
    g = grade_rates[lvl]
    grades.append({'level': lvl,
                   'mean': round(g['w'] / g['wh'], 4) if g['wh'] else 0,
                   'min': round(min(g['vals']), 3), 'max': round(max(g['vals']), 3),
                   'n': len(g['vals'])})

# профессии: сортируем по средней принятой бумаге
profs = sorted(
    ({'prof': k.replace('prof.', ''), 'n': v['n'],
      'avg_issued': v['avg_issued'], 'avg_received': v['avg_received']}
     for k, v in by_prof.items()),
    key=lambda x: -x['avg_received'])

# гистограммы с человеческими подписями
def hist_labels(edges, first_is_zero=False):
    if first_is_zero:
        return ['0'] + [f'≤{e:g}' for e in edges[1:]] + [f'>{edges[-1]:g}']
    return [f'≤{edges[0]:g}'] + [f'{edges[i-1]:g}–{edges[i]:g}' for i in range(1, len(edges))] + [f'>{edges[-1]:g}']

issued_hist = {'labels': hist_labels(dist['issued_hist_edges']), 'counts': dist['issued_hist']}
redeem_hist = {'labels': hist_labels(dist['redemption_ratio_hist_edges'], True),
               'counts': dist['redemption_ratio_hist']}

# индекс трудочаса по месяцам
idx_months = sorted(rates_index)
index_series = [{'month': m, 'value': rates_index[m]} for m in idx_months]
idx_vals = [rates_index[m] for m in idx_months]

# стыковка
needs = match.get('needs') or []
closed = sum(1 for n in needs if n.get('candidates'))
deficits = sorted({x.get('need', '') for x in (match.get('deficits') or [])})

# показательные следы: эмитировал И погасил И его труд принимали
rich = sorted((e for e in econ if e['issued'] > 0 and e['redeemed'] > 0 and e['received'] > 0),
              key=lambda e: -(e['issued'] + e['received'] + 5 * e['redeemed']))[:6]

data = {
    'meta': meta,
    'generated': datetime.now().strftime('%Y-%m-%d'),
    'totals': {
        'issued': round(issued_tot, 1), 'redeemed': round(redeemed_tot, 1),
        'received': round(received_tot, 1), 'redeem_pct': round(redeem_pct, 1),
        'with_redemption': with_redemption,
        'median_debt': round(st.median([e['debt'] for e in econ]), 2),
        'median_received': round(st.median([e['received'] for e in econ]), 2),
    },
    'issued_hist': issued_hist, 'redeem_hist': redeem_hist,
    'grades': grades, 'professions': profs,
    'index': index_series,
    'index_stats': {'mean': round(sum(idx_vals) / len(idx_vals), 4) if idx_vals else 0,
                    'min': round(min(idx_vals), 4) if idx_vals else 0,
                    'max': round(max(idx_vals), 4) if idx_vals else 0},
    'match': {'closed': closed, 'total': len(needs),
              'rings': match.get('rings_total', 0), 'deficits': deficits},
    'footprints': rich,
}
json.dump(data, open(os.path.join(OUT, 'presentation-data.json'), 'w'),
          ensure_ascii=False, indent=1)

# ── CSV для таблиц ──────────────────────────────────────────────────────────
import csv
with open(os.path.join(OUT, 'economy.csv'), 'w', newline='') as f:
    w = csv.writer(f)
    cols = ['idx', 'name', 'prof', 'grade', 'geo', 'issued', 'redeemed', 'debt',
            'received', 'spent', 'works_accepted', 'labor_appraised', 'redemption_ratio']
    w.writerow(cols)
    for e in econ:
        w.writerow([e.get(c, '') for c in cols])
with open(os.path.join(OUT, 'professions.csv'), 'w', newline='') as f:
    w = csv.writer(f); w.writerow(['profession', 'people', 'avg_issued', 'avg_received'])
    for p in profs:
        w.writerow([p['prof'], p['n'], p['avg_issued'], p['avg_received']])
with open(os.path.join(OUT, 'rates-index.csv'), 'w', newline='') as f:
    w = csv.writer(f); w.writerow(['month', 'labour_hour_index'])
    for r in index_series:
        w.writerow([r['month'], r['value']])

# ── Markdown-дек (слайды через '---') ───────────────────────────────────────
m = data['meta']; t = data['totals']; ix = data['index_stats']
top_grades = ' · '.join(f"разряд {g['level']}: {g['mean']:.2f}" for g in grades)
deck = f"""---
marp: true
paginate: true
title: Год жизни трудовой экономики
---

# Год жизни трудовой экономики

Симуляция: **{m['population']} человек**, у каждого свой блокчейн, **{m['months']} месяцев**.

Труд, расчёт трудочасами, взаимное заверение — без единого центра.

<small>Данные: `docs/pilot/sim-out/sim-dataset.json` · прогон {data['generated']}</small>

---

## Что произошло за год

| | |
|---|---|
| Сделок (труд → приёмка → расчёт) | **{m['deals_ok']}**, сбоев {m['deals_fail']} |
| Трудочасов проведено | **{m['labor_hours_transacted']}** |
| Взаимных заверений (DAG) | {m['merges']} |
| Пломб · обещаний · отзывов ключей | {m['seals']} · {m['pledges']} · {m['revocations']} |

Экономически активны: **{m['active']} из {m['population']}**.

---

## Деньги здесь — это долг за труд

Каждый сам эмитирует свою «бумагу», когда заказывает труд. Гасит — когда работает сам.

- Эмитировано за год: **{t['issued']} ч**
- Погашено трудом: **{t['redeemed']} ч** ({t['redeem_pct']}%)
- Вернули хоть часть доверенного: **{t['with_redemption']} из {m['population']}** человек
- Медианный долг: {t['median_debt']} ч · медиана принятой бумаги: {t['median_received']} ч

---

## Индекс среднего трудочаса

Взвешенное по часам среднее ставок. **1.000 = за час труда создаётся ровно час бумаги.**

- Среднее за год: **{ix['mean']}**
- Размах: {ix['min']} … {ix['max']}

Выше единицы — инфляция трудочаса, ниже — дефляция. Величина считается из публичных
сделок, поэтому её может перепроверить кто угодно.

---

## Ставки растут с разрядом

{top_grades}

Ставка — сколько часов бумаги платят за час труда. Разряд 3 в среднем ниже единицы,
разряд 6 — выше: разница в квалификации оплачивается, но общий баланс сохраняется.

---

## Стыковка потребностей и навыков

- Закрыто **{data['match']['closed']} из {data['match']['total']}** потребностей
- Замкнутых цепочек помощи (колец обмена): **{data['match']['rings']}**
- Структурный дефицит: {', '.join(data['match']['deficits']) or '—'}

Дефицит виден сразу — и виден список тех, кто готов переучиться, чтобы его закрыть.

---

## Экономический след человека

Не оценка и не рейтинг — проверяемые факты:

{chr(10).join(f"- **{e['name']}** ({e['prof'].replace('prof.','')}): выпустил {e['issued']:.1f} ч, погасил {e['redeemed']:.1f} ч, его труд приняли на {e['received']:.1f} ч" for e in rich[:4])}

Каждое число подкреплено записями в цепях контрагентов — подделать можно, только
уговорив живых людей.

---

## Как воспроизвести

```bash
apt-get install -y faketime
cmake --build build
docs/pilot/sim-year.sh                     # ~4 мин
python3 docs/pilot/build-presentation.py   # эта презентация
```

Всё самодостаточно: свой агрегатор, временная папка, полная уборка.
"""
open(os.path.join(OUT, 'sim-presentation.md'), 'w').write(deck)

# ── короткая выжимка ────────────────────────────────────────────────────────
summary = f"""# Год жизни трудовой экономики — выжимка

**{m['population']} человек · {m['months']} месяцев · {m['deals_ok']} сделок без сбоев · {m['labor_hours_transacted']} трудочасов**

- Эмитировано {t['issued']} ч бумаги, погашено трудом {t['redeemed']} ч ({t['redeem_pct']}%);
  вернули хоть часть — {t['with_redemption']} из {m['population']}.
- Индекс среднего трудочаса: {ix['mean']} (размах {ix['min']}…{ix['max']}) —
  эмиссия идёт в ногу с реально отработанными часами.
- Ставки растут с разрядом: {top_grades}.
- Стыковка закрыла {data['match']['closed']} из {data['match']['total']} потребностей;
  дефицит: {', '.join(data['match']['deficits']) or '—'}.
- Взаимных заверений в графе: {m['merges']}; пломб {m['seals']}; отзывов ключей {m['revocations']}.

Данные: `sim-dataset.json` · таблицы: `economy.csv`, `professions.csv`, `rates-index.csv`
"""
open(os.path.join(OUT, 'sim-summary.md'), 'w').write(summary)

# ── HTML: подставляем данные в шаблон ───────────────────────────────────────
tpl_path = os.path.join(HERE, 'presentation-template.html')
if os.path.exists(tpl_path):
    tpl = open(tpl_path).read()
    html = tpl.replace('/*__DATA__*/null',
                       json.dumps(data, ensure_ascii=False, separators=(',', ':')))
    open(os.path.join(OUT, 'presentation.html'), 'w').write(html)
    print('presentation.html   ✓')
else:
    print('presentation-template.html не найден — HTML пропущен')

print(f"""presentation-data.json ✓
sim-presentation.md    ✓ (слайды)
sim-summary.md         ✓
economy.csv / professions.csv / rates-index.csv ✓
всё в: {OUT}""")
