#!/usr/bin/env python3
"""
ИР-021: прогон метрики независимости контрагентов на данных sim-year.sh.

Вопрос: метрика, дисконтирующая взаимные пары, обязана отличать СГОВОР от
честной деревни (развилка B4). Скрипт меряет это на живых данных симуляции,
куда внедряется синтетическая пара-сговор с настраиваемой наглостью.

Симуляция — удачный полигон: торговля идёт преимущественно внутри района, а
роли в паре чередуются по месяцам, то есть честная взаимность там massova.

Вход:  deals.tsv из sim-year.sh (month, payer, worker, specialty, level,
       hours, coef). coef — цена часа сделки (в симуляции это ставка и k разом).
Выход: консольный отчёт + <out>/pair-independence.json

Запуск: python3 docs/pilot/pair-independence.py --deals ПУТЬ/deals.tsv
Детерминировано: stdlib, никакого random.
"""
import json, os, sys, argparse, statistics as st
from collections import defaultdict

ap = argparse.ArgumentParser()
ap.add_argument('--deals', required=True)
ap.add_argument('--out')
ap.add_argument('--kappa', type=float, default=1.25,
                help='порог ценовой аномалии для конъюнктивной метрики')
ap.add_argument('--attack-hours', type=float, default=6.0)
args = ap.parse_args()
OUT = args.out or os.path.dirname(os.path.abspath(args.deals))

BASE = []
with open(args.deals) as f:
    f.readline()
    for line in f:
        p = line.rstrip('\n').split('\t')
        if len(p) >= 7:
            BASE.append({'month': int(p[0]), 'payer': p[1], 'worker': p[2],
                         'spec': p[3], 'level': int(p[4]), 'hours': float(p[5]),
                         'coef': float(p[6]), 'attack': False})
if not BASE:
    sys.exit(f'пустой журнал сделок: {args.deals}')
for d in BASE:
    d['units'] = d['hours'] * d['coef']

MONTHS = sorted({d['month'] for d in BASE})
VSPEC = st.mode([d['spec'] for d in BASE])
VLEVEL = st.mode([d['level'] for d in BASE if d['spec'] == VSPEC])
HONEST_K = [d['coef'] for d in BASE]


def build(k, pattern):
    """Копия мира с внедрённой парой-сговором.

    pattern='both'      — оба направления каждый месяц (наивный сговор);
    pattern='alternate' — роли чередуются по месяцам, как у честной деревни
                          (сговор, маскирующийся под обычную взаимность).
    """
    deals = [dict(d) for d in BASE]
    A, B = 'ATK-A', 'ATK-B'
    for m in MONTHS:
        pairs = ((A, B), (B, A)) if pattern == 'both' else \
                (((A, B),) if m % 2 == 0 else ((B, A),))
        for payer, worker in pairs:
            deals.append({'month': m, 'payer': payer, 'worker': worker,
                          'spec': VSPEC, 'level': VLEVEL,
                          'hours': args.attack_hours, 'coef': k,
                          'units': args.attack_hours * k, 'attack': True})
    return deals


def rate_of(deals, weights=None):
    num = den = 0.0
    for i, d in enumerate(deals):
        if d['spec'] != VSPEC or d['level'] != VLEVEL:
            continue
        w = 1.0 if weights is None else weights[i]
        num += d['units'] * w
        den += d['hours'] * w
    return num / den if den else 0.0


def reciprocity(deals, window):
    """R = 1 − |нетто|/валовое по паре в окне. 0 — односторонний, 1 — баланс."""
    per = [0.0] * len(deals)
    for start in range(0, max(MONTHS) + 1, window):
        stop = start + window
        flow = defaultdict(float)
        idx = defaultdict(list)
        for i, d in enumerate(deals):
            if start <= d['month'] < stop:
                flow[(d['payer'], d['worker'])] += d['units']
                idx[frozenset((d['payer'], d['worker']))].append(i)
        for pair, ii in idx.items():
            t = tuple(pair)
            a, b = (t[0], t[1]) if len(t) == 2 else (t[0], t[0])
            ab, ba = flow.get((a, b), 0.0), flow.get((b, a), 0.0)
            gross = ab + ba
            r = 1.0 - abs(ab - ba) / gross if gross else 0.0
            for i in ii:
                per[i] = r
    return per


def anomalies(deals):
    """Во сколько раз цена часа сделки выше медианы своей корзины.

    Медиана берётся по ВСЕМ сделкам корзины, включая сговорные: в жизни
    пометки «это сговор» нет. Медиана держится, пока сговор — меньше
    половины корзины по числу сделок; отсюда требование к минимальному
    размеру корзины (см. отчёт).
    """
    basket = defaultdict(list)
    for d in deals:
        basket[(d['spec'], d['level'])].append(d['coef'])
    med = {k: st.median(v) for k, v in basket.items()}
    return [d['coef'] / med.get((d['spec'], d['level']), d['coef'] or 1.0)
            for d in deals]


def evaluate(deals, weights):
    hh = sum(d['hours'] for d in deals if not d['attack'])
    ah = sum(d['hours'] for d in deals if d['attack'])
    hurt = sum(d['hours'] for d, w in zip(deals, weights)
               if not d['attack'] and w < 0.5)
    caught = sum(d['hours'] for d, w in zip(deals, weights)
                 if d['attack'] and w < 0.5)
    return (round(100 * caught / ah, 1) if ah else 0.0,
            round(100 * hurt / hh, 1) if hh else 0.0,
            rate_of(deals, weights))


rate_clean = rate_of(BASE)
report = {'victim': {'spec': VSPEC, 'level': VLEVEL,
                     'rate_clean': round(rate_clean, 4)},
          'honest_k': {'min': round(min(HONEST_K), 2),
                       'max': round(max(HONEST_K), 2),
                       'median': round(st.median(HONEST_K), 2)},
          'kappa': args.kappa, 'scenarios': []}

print('ИР-021 · метрика независимости контрагентов на данных sim-year.sh')
print(f'честных сделок {len(BASE)} · корзина-жертва {VSPEC} р.{VLEVEL} '
      f'(чистая ставка {rate_clean:.3f})')
print(f'k честных сделок: {min(HONEST_K):.2f}…{max(HONEST_K):.2f} '
      f'(медиана {st.median(HONEST_K):.2f}) — вот на каком фоне прячется сговор')
print()
hdr = (f'{"сговор k":>9} {"схема":>10} {"ущерб":>8} │ '
       f'{"наив@1":>13} {"наив@12":>13} │ {"конъ@1":>13} {"конъ@12":>13}')
print(hdr)
print(f'{"":>9} {"":>10} {"ставке":>8} │ '
      f'{"пойм/задето":>13} {"пойм/задето":>13} │ '
      f'{"пойм/задето":>13} {"пойм/задето":>13}')
print('─' * len(hdr))

for k in (5.0, 2.0, 1.5, 1.3, 1.2, 1.1):
    for pattern in ('both', 'alternate'):
        deals = build(k, pattern)
        anom = anomalies(deals)
        dmg = 100 * (rate_of(deals) - rate_clean) / rate_clean
        row = {'k': k, 'pattern': pattern, 'damage_pct': round(dmg, 1),
               'cells': {}}
        cells = []
        for metric in ('naive', 'conj'):
            for window in (1, 12):
                r = reciprocity(deals, window)
                if metric == 'naive':
                    w = [1.0 - x for x in r]
                else:
                    w = [1.0 - x * min(1.0, max(0.0, (a - 1.0) /
                                                (args.kappa - 1.0)))
                         for x, a in zip(r, anom)]
                caught, hurt, resid = evaluate(deals, w)
                row['cells'][f'{metric}@{window}'] = {
                    'caught_pct': caught, 'false_pct': hurt,
                    'residual_damage_pct': round(
                        100 * (resid - rate_clean) / rate_clean, 1)}
                cells.append(f'{caught:>5.0f}%/{hurt:>5.1f}%')
        report['scenarios'].append(row)
        print(f'{k:>9.2f} {pattern:>10} {dmg:>+7.1f}% │ '
              f'{cells[0]:>13} {cells[1]:>13} │ {cells[2]:>13} {cells[3]:>13}')

path = os.path.join(OUT, 'pair-independence.json')
json.dump(report, open(path, 'w'), ensure_ascii=False, indent=1)
print(f'\nпойм = поймано объёма сговора · задето = честного объёма потеряло вес')
print(f'→ {path}')
