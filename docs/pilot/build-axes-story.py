#!/usr/bin/env python3
"""
Собирает презентацию «Оси труда» — для обывателя, из двух прогонов:

  • сюжетного — docs/pilot/axes-town.sh → sim-out/axes-town/ (город и две
    деревни, у каждого сюжет: новая ось, спрятанная надбавка, мастер в деревне,
    сговор, канал, дефицит, материалы, образцы и облако);
  • годового — docs/pilot/sim-year.sh → sim-out/ (200 человек, масштаб) и
    замер справедливости к мастерам aggregator_measure_indep по его журналу.

Все числа — из настоящего кода: агрегатор считал цены осей, вес ИР-021, книгу
осей, карту ренты и облако; здесь они только собраны и подписаны.

Выход: sim-out/axes-story.html (шаблон axes-story-template.html, плейсхолдер
/*__DATA__*/null) и sim-out/axes-story-data.json.

Запуск:  docs/pilot/sim-year.sh && docs/pilot/axes-town.sh
         python3 docs/pilot/build-axes-story.py [--out sim-out]
"""
import argparse, csv, json, os, re, subprocess, sys
from datetime import datetime, timezone

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))

ap = argparse.ArgumentParser()
ap.add_argument('--out', default=os.path.join(HERE, 'sim-out'))
args = ap.parse_args()
OUT = args.out
TOWN = os.path.join(OUT, 'axes-town')
for need in (os.path.join(TOWN, 'world.json'), os.path.join(OUT, 'rates_by_month.json')):
    if not os.path.exists(need):
        sys.exit(f"нет {need} — сначала docs/pilot/sim-year.sh и docs/pilot/axes-town.sh")


def load(path, default=None):
    try:
        return json.load(open(path, encoding='utf-8'))
    except Exception:
        return default


# ── Словарь осей: группы и аргументы — из пилотной цепи (seed-axes.sh) ──────
dictionary, group = [], None
for line in open(os.path.join(HERE, 'seed-axes.sh'), encoding='utf-8'):
    m = re.match(r'echo "── (.+?) ──"', line)
    if m:
        group = {'name': m.group(1), 'axes': []}
        dictionary.append(group)
        continue
    m = re.match(r'define (\S+)\s+"([^"]+)"\s+"([^"]+)"', line)
    if m and group is not None:
        group['axes'].append({'slug': m.group(1), 'ru': m.group(2), 'desc': m.group(3)})
m = re.search(r'--slug hv400 --ru "([^"]+)"\s*\\\s*\n\s*--description "([^"]+)"',
              open(os.path.join(HERE, 'axes-town.sh'), encoding='utf-8').read())
own_axis = {'slug': 'hv400', 'ru': m.group(1) if m else 'Под напряжением 400 В',
            'desc': m.group(2) if m else ''}
group_of = {a['slug']: g['name'] for g in dictionary for a in g['axes']}
group_of['hv400'] = 'своя ось'
ru_of = {a['slug']: a['ru'] for g in dictionary for a in g['axes']}
ru_of['hv400'] = own_axis['ru']

# ── Сюжетный прогон ─────────────────────────────────────────────────────────
world = load(os.path.join(TOWN, 'world.json'))
months = world['months']
t0 = world['month0_ts']
month_ts = [t0 + m * 30 * 86400 for m in range(months)]
RU_MONTH = ['янв', 'фев', 'мар', 'апр', 'май', 'июн', 'июл', 'авг', 'сен', 'окт', 'ноя', 'дек']
month_lbl = [f"{RU_MONTH[datetime.fromtimestamp(t, timezone.utc).month - 1]} "
             f"{datetime.fromtimestamp(t, timezone.utc).strftime('%y')}" for t in month_ts]
people = {p['key']: p for p in world['people']}
by_chain = {p['chain']: p['key'] for p in world['people']}
path_slug = {v['path']: s for s, v in world['axes'].items()}
planted = {s: v['planted'] for s, v in world['axes'].items() if v['planted'] is not None}

deals = []
for r in csv.DictReader(open(os.path.join(TOWN, 'deals.tsv'), encoding='utf-8'), delimiter='\t'):
    sh = {kv.split('=')[0]: float(kv.split('=')[1]) for kv in r['shares'].split()}
    deals.append({'m': int(r['month']), 'payer': r['payer'], 'worker': r['worker'],
                  'kind': r['kind'], 'sample': r['sample'], 'hours': float(r['hours']),
                  'rate': float(r['rate']), 'mult': float(r['mult']), 'tag': r['tag'],
                  'units': float(r['units']), 'carried': float(r['carried'] or 0), 'shares': sh})

# цены осей по месяцам: агрегатор (с весом ИР-021) и «без защиты» из журнала
prices = [load(os.path.join(TOWN, f'prices-{m}.json'), {}) for m in range(months)]
W_town = [round(p.get('W', 0), 4) if p else None for p in prices]
axes = {}
for m, p in enumerate(prices):
    for a in (p or {}).get('axes', []):
        s = path_slug.get(a['axis'])
        if not s:
            continue
        x = axes.setdefault(s, {'slug': s, 'ru': a.get('label') or ru_of.get(s, s),
                                'group': group_of.get(s, ''), 'planted': planted.get(s),
                                'monthly': [None] * months, 'naive_monthly': [None] * months,
                                'hours': 0.0, 'whours': 0.0, '_pu': 0.0})
        if a['hours'] > 0:
            x['monthly'][m] = round(a['price'], 4)
            x['hours'] += a['hours']; x['whours'] += a.get('weighted_hours', a['hours'])
            x['_pu'] += a['price'] * a['hours']
naive_u, naive_h = {}, {}
naive_mu, naive_mh = {}, {}
for d in deals:
    for s, share in d['shares'].items():
        naive_u[s] = naive_u.get(s, 0) + d['units'] * share
        naive_h[s] = naive_h.get(s, 0) + d['hours']
        k = (s, d['m'])
        naive_mu[k] = naive_mu.get(k, 0) + d['units'] * share
        naive_mh[k] = naive_mh.get(k, 0) + d['hours']
for s, x in axes.items():
    x['found'] = round(x['_pu'] / x['hours'], 4) if x['hours'] else None
    x['naive'] = round(naive_u[s] / naive_h[s], 4) if naive_h.get(s) else None
    for m in range(months):
        if naive_mh.get((s, m)):
            x['naive_monthly'][m] = round(naive_mu[(s, m)] / naive_mh[(s, m)], 4)
    x['survived'] = round(x['whours'] / x['hours'], 3) if x['hours'] else None
    x['payers'] = len({d['payer'] for d in deals if s in d['shares']})
    x['kinds'] = sorted({world['kinds'][d['kind']]['title'] for d in deals if s in d['shares']})
    x['hours'] = round(x['hours'], 1); x['whours'] = round(x['whours'], 1)
    del x['_pu']
found_price = {s: x['found'] for s, x in axes.items()}


def receipt(d, who_note=''):
    """Чек сделки: разбивка цены по осям, перенос материалов, итог."""
    lines = [{'slug': s, 'ru': ru_of.get(s, s), 'group': group_of.get(s, ''),
              'units': round(d['units'] * sh, 4)} for s, sh in d['shares'].items()]
    return {'title': world['kinds'][d['kind']]['title'], 'worker': people[d['worker']]['name'],
            'payer': people[d['payer']]['name'], 'month': month_lbl[d['m']],
            'hours': d['hours'], 'units': round(d['units'], 4), 'rate': round(d['rate'], 4),
            'carried': round(d['carried'], 5), 'lines': lines, 'tag': d['tag'], 'note': who_note}


def first(pred):
    return next((d for d in deals if pred(d)), None)


receipts = {}
for key, pred in {
    'arseniy': lambda d: d['worker'] == 'arseniy',
    'grisha': lambda d: d['worker'] == 'grisha',
    'boris': lambda d: d['worker'] == 'boris',
    'nurse': lambda d: d['kind'] == 'ukhod',
    'hair': lambda d: d['kind'] == 'strizhka',
    'stove': lambda d: d['kind'] == 'pech',
    'bread': lambda d: d['kind'] == 'hleb' and d['carried'] > 0,
    'kuzmich': lambda d: d['worker'] == 'kuzmich' and d['kind'] == 'provodka' and d['m'] >= 6,
    'stepan': lambda d: d['worker'] == 'stepan' and d['kind'] == 'provodka' and d['m'] >= 6,
    'trip': lambda d: d['worker'] == 'stepan' and d['kind'] == 'poezdka' and d['m'] >= 6,
    'collusion': lambda d: d['tag'] == 'collusion' and d['worker'] == 'mila',
}.items():
    d = first(pred)
    if d:
        receipts[key] = receipt(d)

# виды работ: задуманная цена и средняя фактическая
kinds = {}
for k, v in world['kinds'].items():
    ds = [d for d in deals if d['kind'] == k and d['tag'] == 'honest']
    h = sum(d['hours'] for d in ds)
    kinds[k] = {'title': v['title'], 'axes': v['axes'],
                'planted': round(sum(planted.get(a, 0) for a in v['axes']), 4),
                'network': round(sum(found_price.get(a) or 0 for a in v['axes']), 4),
                'fact': round(sum(d['units'] for d in ds) / h, 4) if h else None,
                'deals': len([d for d in deals if d['kind'] == k])}

# карта ренты
ledger = load(os.path.join(TOWN, 'ledger.json'), {})
STORY = {'arseniy': 'назвал свою ось', 'grisha': 'спрятал надбавку', 'kuzmich': 'деревня, спрятал',
         'stepan': 'деревня, по умолчанию', 'trofim': 'один на город', 'zoya': 'переплата «Вектора»',
         'leva': 'сговор', 'mila': 'сговор', 'liza': 'срочный заказ'}
rent = []
for r in ledger.get('rent', []):
    key = by_chain.get(r['worker'])
    if not key:
        continue
    rent.append({'key': key, 'name': people[key]['name'], 'place': people[key]['place'],
                 'title': r['title'], 'fact': round(r['fact'], 4), 'expected': round(r['expected'], 4),
                 'resid': round(r['resid'], 4), 'hours': round(r['hours'], 1), 'payers': r['payers'],
                 'rent_payers': None if r['rent_payers'] is None else round(r['rent_payers'], 2),
                 'over': r['periods_over'], 'seen': r['periods_seen'], 'story': STORY.get(key, '')})
rent_of = {r['key']: r for r in rent}

# новая ось: цена по месяцам и сколько разных плательщиков её уже назвали
hv = axes.get('hv400', {})
seen, payers_cum = set(), []
for m in range(months):
    seen |= {d['payer'] for d in deals if d['m'] == m and 'hv400' in d['shares']}
    payers_cum.append(len(seen))
known_from = next((m for m, n in enumerate(payers_cum) if n >= 4), None)

# деревня: цена одного выезда
def per_job(worker, extra_kind=None):
    wd = [d for d in deals if d['worker'] == worker and d['kind'] == 'provodka']
    td = [d for d in deals if d['worker'] == worker and d['kind'] == extra_kind] if extra_kind else []
    n = len(wd) or 1
    return {'jobs': len(wd), 'hours': round(sum(d['hours'] for d in wd) / n, 2),
            'units': round(sum(d['units'] for d in wd) / n, 3),
            'rate': round(sum(d['units'] for d in wd) / (sum(d['hours'] for d in wd) or 1), 4),
            'trip_hours': round(sum(d['hours'] for d in td) / n, 2) if td else 0,
            'trip_units': round(sum(d['units'] for d in td) / n, 3) if td else 0}

village = {'kuzmich': per_job('kuzmich'), 'stepan': per_job('stepan', 'poezdka'),
           'network_rate': kinds['provodka']['network'], 'planted_rate': kinds['provodka']['planted']}

# сговор: оси, которые называют работы пары
pair_axes = sorted({s for d in deals if d['tag'] == 'collusion' for s in d['shares']},
                   key=lambda s: -(axes.get(s, {}).get('naive') or 0) / (planted.get(s) or 1))
collusion = {'deals': len([d for d in deals if d['tag'] == 'collusion']),
             'hours': sum(d['hours'] for d in deals if d['tag'] == 'collusion'),
             'axes': pair_axes}

# облако образцов
cloud = load(os.path.join(TOWN, 'cloud.json'), {})
sample_key = {ref: sk for sk, ref in world['samples'].items()}
first_worker = {}
for d in deals:
    first_worker.setdefault(d['sample'], d['worker'])
points = []
for p in cloud.get('points', []):
    sk = sample_key.get(p['sample'], '')
    kd = sk.split('~')[0]
    comp = {}
    for d in deals:
        if d['sample'] == sk:
            for s, sh in d['shares'].items():
                comp[s] = comp.get(s, 0) + d['units'] * sh
    tot = sum(comp.values()) or 1
    points.append({'id': p['sample'], 'title': p['title'], 'sample': sk,
                   'who': people.get(first_worker.get(sk, ''), {}).get('name', ''),
                   'deals': p['deals'], 'xy': p.get('coord'),
                   'comp': [{'slug': s, 'ru': ru_of.get(s, s), 'group': group_of.get(s, ''),
                             'share': round(u / tot, 3)} for s, u in
                            sorted(comp.items(), key=lambda kv: -kv[1])],
                   'nb': [{'id': n['sample'], 'title': n['title'], 'w': round(n['w'], 3)}
                          for n in p['neighbors']]})

# электрики в каталоге: заявленный разряд рядом с принятой работой
electricians = []
for c in (load(os.path.join(TOWN, 'electricians.json'), {}) or {}).get('chains', []):
    grade = next((t.split(':', 1)[1] for t in c.get('tags', []) if t.startswith('grade:')), '')
    at = c.get('attested') or {}
    electricians.append({'name': (c.get('text') or '').split(':')[0], 'text': c.get('text', ''),
                         'grade': grade, 'chains': at.get('chains', 0),
                         'hours': round(at.get('hours', 0), 1),
                         'axes': [{'ru': a.get('label', ''), 'group': group_of.get(
                             path_slug.get(a.get('axis', ''), ''), ''),
                             'chains': a.get('chains'), 'hours': round(a.get('hours', 0), 1)}
                             for a in at.get('axes', [])]})
electricians.sort(key=lambda e: -e['hours'])

town = {
    'meta': {'people': len(world['people']), 'months': months, 'deals': world['deals_ok'],
             'fail': world['deals_fail'], 'hours': world['hours'], 'seed': world['seed'],
             'from': month_lbl[0], 'to': month_lbl[-1],
             'places': {pl: sum(1 for p in world['people'] if p['place'] == pl)
                        for pl in ('Заречье', 'Верхняя', 'Нижняя')}},
    'months': month_lbl, 'W': W_town,
    'axes': sorted(axes.values(), key=lambda x: -(x['found'] or 0)),
    'kinds': kinds, 'receipts': receipts, 'rent': rent,
    'hv': {'monthly': hv.get('monthly'), 'payers': payers_cum, 'known_from': known_from,
           'planted': planted.get('hv400'), 'found': hv.get('found')},
    'village': village, 'collusion': collusion, 'cloud': points,
    'electricians': electricians,
    'people': [{'key': k, 'name': p['name'], 'place': p['place'],
                'kind': world['kinds'].get(p['kind'], {}).get('title', ''), 'grade': p['grade']}
               for k, p in people.items()],
}

# ── Годовой прогон и замер справедливости ───────────────────────────────────
ds = load(os.path.join(OUT, 'sim-dataset.json'), {}) or {}
rbm = load(os.path.join(OUT, 'rates_by_month.json'), {}) or {}
ymonths = sorted(rbm)
yaxes = {}
for i, mo in enumerate(ymonths):
    for a in (rbm[mo] or {}).get('axes', []):
        x = yaxes.setdefault(a['label'], {'ru': a['label'], 'monthly': [None] * len(ymonths),
                                          'hours': 0.0, '_pu': 0.0})
        x['monthly'][i] = round(a['price'], 4)
        x['hours'] += a['hours']; x['_pu'] += a['price'] * a['hours']
for x in yaxes.values():
    x['mean'] = round(x['_pu'] / x['hours'], 4) if x['hours'] else None
    x['hours'] = round(x['hours']); del x['_pu']
year = {'meta': ds.get('meta', {}), 'months': [f"{RU_MONTH[int(mo[5:]) - 1]} {mo[2:4]}" for mo in ymonths],
        'W': [round((rbm[mo] or {}).get('W', 0), 4) for mo in ymonths],
        'axes': sorted(yaxes.values(), key=lambda x: -(x['mean'] or 0))}

measure_path = os.path.join(OUT, 'measure-indep.txt')
tool = os.path.join(ROOT, 'build', 'modules', 'aggregator', 'aggregator_measure_indep')
if not os.path.exists(measure_path) and os.path.exists(tool):
    print('замер справедливости: aggregator_measure_indep (≈1.5 мин)…')
    open(measure_path, 'w').write(subprocess.run(
        [tool, os.path.join(OUT, 'deals.tsv'), '1.3'], capture_output=True, text=True).stdout)
measure = {'modes': [], 'deals': None}
if os.path.exists(measure_path):
    txt = open(measure_path, encoding='utf-8').read()
    m = re.search(r'журнале прогона года: (\d+) сделок', txt)
    measure['deals'] = int(m.group(1)) if m else None
    names = ['спрятана в общих осях', 'одна ось на всё мастерство', 'своя ось на каждую ступень']
    blocks = re.findall(r'честный объём под дисконтом:\s+р\.3 ([\d.]+)%\s+р\.4 ([\d.]+)%\s+р\.5 ([\d.]+)%'
                        r'\s+р\.6 ([\d.]+)%\s+· всего ([\d.]+)%\s*\n\s*нормировщик W:.*?\(сдвиг ([-\d.]+)%\)'
                        r'\s*\n\s*сговор ×([\d.]+) уцелел:\s+в общих осях ([\d.]+)%\s+на оси мастерства ([\d.]+)%'
                        r'\s+на своей оси ([\d.]+)%', txt)
    for name, b in zip(names, blocks):
        measure['modes'].append({'name': name, 'grades': [float(v) for v in b[:4]], 'total': float(b[4]),
                                 'wshift': float(b[5]), 'k': float(b[6]),
                                 'collusion': [float(v) for v in b[7:10]]})
year['measure'] = measure

data = {'generated': datetime.now().strftime('%Y-%m-%d'),
        'dict': {'groups': dictionary, 'own': own_axis}, 'town': town, 'year': year}
json.dump(data, open(os.path.join(OUT, 'axes-story-data.json'), 'w', encoding='utf-8'),
          ensure_ascii=False, indent=1)
tpl = open(os.path.join(HERE, 'axes-story-template.html'), encoding='utf-8').read()
html = tpl.replace('/*__DATA__*/null', json.dumps(data, ensure_ascii=False, separators=(',', ':')))
open(os.path.join(OUT, 'axes-story.html'), 'w', encoding='utf-8').write(html)
print(f"axes-story.html ✓  ({len(html) // 1024} КБ)\naxes-story-data.json ✓\nвсё в: {OUT}")
