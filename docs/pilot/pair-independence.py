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
ap.add_argument('--ref', choices=('deals', 'edges'), default='edges',
                help='опора аномалии: по сделкам или схлопнутая по рёбрам')
ap.add_argument('--full-cadence', action='store_true',
                help='сговор торгует каждый месяц (объём не уравнивается)')
ap.add_argument('--max-len', type=int, default=4,
                help='наибольшая длина гасимого цикла в кольцевой метрике')
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
ATTACK_DEALS = 24        # объём внедряемого сговора, одинаковый у всех топологий
VSPEC = st.mode([d['spec'] for d in BASE])
VLEVEL = st.mode([d['level'] for d in BASE if d['spec'] == VSPEC])
HONEST_K = [d['coef'] for d in BASE]


def build(k, pattern, ring=2):
    """Копия мира с внедрённым сговором из `ring` участников.

    ring=2 — пара; pattern='both' гоняет оба направления каждый месяц (наивно),
    pattern='alternate' чередует роли, маскируясь под честную деревню.
    ring≥3 — кольцо A→B→C→…→A, всегда в одну сторону: между каждой парой поток
    строго односторонний, поэтому парная метрика слепа к нему по построению.
    """
    deals = [dict(d) for d in BASE]
    names = [f'ATK-{chr(ord("A") + i)}' for i in range(ring)]

    def add(payer, worker, m):
        deals.append({'month': m, 'payer': payer, 'worker': worker,
                      'spec': VSPEC, 'level': VLEVEL,
                      'hours': args.attack_hours, 'coef': k,
                      'units': args.attack_hours * k, 'attack': True})

    # Объём атаки уравнен по топологиям (≈ATTACK_DEALS сделок), иначе кольцо
    # из четверых просто заливает корзину числом, и сравнивается размер, а не
    # форма. Заодно сговор остаётся меньшинством корзины — см. предел ниже.
    active = MONTHS if args.full_cadence \
        else MONTHS[:max(1, round(ATTACK_DEALS / ring))]
    for m in active:
        if ring == 2:
            A, B = names
            edges = ((A, B), (B, A)) if pattern == 'both' else \
                    (((A, B),) if m % 2 == 0 else ((B, A),))
        else:
            edges = tuple((names[i], names[(i + 1) % ring])
                          for i in range(ring))
        for payer, worker in edges:
            add(payer, worker, m)
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


def circulation(flow, max_len):
    """Доля потока каждого ребра, возвращающаяся по циклу длины ≤ max_len.

    Обобщение взаимности с пар на кольца (ИР-021 B5): пара — это цикл длины 2,
    кольцо A→B→C→A — длины 3, и парная метрика его не видит вовсе (между каждой
    парой поток односторонний, R=0).

    Жадное гашение циклов, короткие раньше длинных. Порядок обхода канонический
    (рёбра и соседи отсортированы), иначе разложение неоднозначно и свидетели
    не сойдутся в пересчёте.
    """
    resid = dict(flow)
    circ = {e: 0.0 for e in flow}
    out = defaultdict(list)
    for (a, b) in sorted(flow):
        out[a].append(b)

    def find_cycle(a, b, length):
        """Цикл a→b→…→a ровно из `length` рёбер; возвращает список рёбер."""
        def dfs(node, path, visited):
            if len(path) == length - 1:
                return path + [(node, a)] if resid.get((node, a), 0) > 1e-12 else None
            for nxt in out[node]:
                if nxt == a or nxt in visited:
                    continue
                if resid.get((node, nxt), 0) <= 1e-12:
                    continue
                got = dfs(nxt, path + [(node, nxt)], visited | {nxt})
                if got:
                    return got
            return None
        if resid.get((a, b), 0) <= 1e-12:
            return None
        if length == 2:
            return [(a, b), (b, a)] if resid.get((b, a), 0) > 1e-12 else None
        return dfs(b, [(a, b)], {a, b})

    for length in range(2, max_len + 1):
        for (a, b) in sorted(resid):
            while resid.get((a, b), 0) > 1e-12:
                cyc = find_cycle(a, b, length)
                if not cyc:
                    break
                m = min(resid[e] for e in cyc)
                for e in cyc:
                    resid[e] -= m
                    circ[e] += m
    return circ


def ring_R(deals, window, max_len):
    """R по кольцам: на сделку — доля её ребра, ушедшая в циркуляцию."""
    per = [0.0] * len(deals)
    for start in range(0, max(MONTHS) + 1, window):
        stop = start + window
        flow = defaultdict(float)
        idx = defaultdict(list)
        for i, d in enumerate(deals):
            if start <= d['month'] < stop:
                flow[(d['payer'], d['worker'])] += d['units']
                idx[(d['payer'], d['worker'])].append(i)
        circ = circulation(dict(flow), max_len)
        for e, ii in idx.items():
            r = circ[e] / flow[e] if flow[e] > 0 else 0.0
            for i in ii:
                per[i] = min(1.0, r)
    return per


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


def anomalies(deals, ref='deals'):
    """Во сколько раз цена часа сделки выше медианы-опоры своей корзины.

    Опора считается по ВСЕМ сделкам, включая сговорные: в жизни пометки
    «это сговор» нет. Отсюда слом: у медианы точка отказа 50%, и сговор,
    набравший больше половины сделок корзины, становится сам себе опорой.

    ref='edges' лечит это: сделки одного ребра (плательщик→работник)
    схлопываются в одну точку со средней ценой. Кольцо из троих даёт три
    точки, сколько бы сделок оно ни провело, — чтобы утопить опору, нужно
    большинство КОНТРАГЕНТОВ, а не большинство сделок.
    """
    basket = defaultdict(list)
    if ref == 'edges':
        agg = defaultdict(lambda: [0.0, 0.0])   # ребро → [Σ units, Σ hours]
        for d in deals:
            a = agg[(d['spec'], d['level'], d['payer'], d['worker'])]
            a[0] += d['units']; a[1] += d['hours']
        for (spec, lvl, _p, _w), (u, h) in agg.items():
            if h > 0:
                basket[(spec, lvl)].append(u / h)
    else:
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
hdr = (f'{"сговор":>18} {"ущерб":>8} │ {"парная":>14} │ '
       f'{"кольцевая L≤" + str(args.max_len):>14}')
print(hdr)
print(f'{"":>18} {"ставке":>8} │ {"пойм/задето":>14} │ {"пойм/задето":>14}')
print('─' * len(hdr))

SCEN = [(5.0, 'both', 2), (5.0, 'alternate', 2), (1.5, 'alternate', 2),
        (5.0, '-', 3), (2.0, '-', 3), (1.5, '-', 3),
        (5.0, '-', 4), (5.0, '-', 5), (5.0, '-', 6)]
for k, pattern, ring in SCEN:
    deals = build(k, pattern, ring)
    anom = anomalies(deals, args.ref)
    dmg = 100 * (rate_of(deals) - rate_clean) / rate_clean
    cells = []
    for kind in ('pair', 'ring'):
        r = reciprocity(deals, 12) if kind == 'pair' else ring_R(deals, 12, args.max_len)
        w = [1.0 - x * min(1.0, max(0.0, (a - 1.0) / (args.kappa - 1.0)))
             for x, a in zip(r, anom)]
        caught, hurt, resid = evaluate(deals, w)
        cells.append(f'{caught:>5.0f}%/{hurt:>5.1f}%')
        report.setdefault('scenarios', []).append(
            {'k': k, 'pattern': pattern, 'ring': ring, 'metric': kind,
             'damage_pct': round(dmg, 1), 'caught_pct': caught,
             'false_pct': hurt,
             'residual_damage_pct': round(
                 100 * (resid - rate_clean) / rate_clean, 1)})
    label = (f'пара, {pattern}' if ring == 2 else f'кольцо из {ring}')
    print(f'{label:>12} k={k:<4.1f} {dmg:>+7.1f}% │ {cells[0]:>14} │ '
          f'{cells[1]:>14}')

path = os.path.join(OUT, 'pair-independence.json')
json.dump(report, open(path, 'w'), ensure_ascii=False, indent=1)
print(f'\nпойм = поймано объёма сговора · задето = честного объёма потеряло вес')
print(f'→ {path}')
