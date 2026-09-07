#!/usr/bin/env python3
"""
ИР-020: прототип-считалка «цены осей» — гедонистическое разложение ставок.

Сделка остаётся одномерной (economy.md §2а: ставка × k × время); считалка
раскладывает УЖЕ СОВЕРШЁННЫЕ сделки по профилям осей:

    ставка_i ≈ β0 + Σ_j βj · x_ij

Цены осей βj никем не назначаются — выводятся из сделок (гедонистическая
регрессия). Невязка (факт − предсказание) — диагностика, не платёж:
  • устойчивая на классе работ  → в словаре не хватает оси (ИР-020);
  • временная на одной деятельности → рента дефицита (видимый индикатор).
Ставка — рельеф над картой, не ось карты (specialty-axes.md §7); здесь
считается второй рельеф — рента.

Два набора данных за один запуск:
  demo — встроенный учебный пример (11 деятельностей, оси знание/опасность/
         люди/мастерство) — числа для презентации «на пальцах»;
  sim  — реальный прогон года: ставки sim-out/rates_by_month.json
         (создаёт sim-year.sh) × профили каталога docs/catalogs/professions.json
         (оси info/people/danger; material — базовая категория, т.к. тройка
         материя+информация+люди суммируется в 1, specialty-axes.md §4.1)
         + разряд как ось мастерства.

Выход: консольный отчёт + sim-out/axis-prices.json;
       --html: сборка презентации axes-presentation-template.html
               → sim-out/axes-presentation.html (плейсхолдер __DATA__).

Запуск:  python3 docs/pilot/axis-prices.py [--html] [--out ПАПКА]
Детерминировано: stdlib, нормальные уравнения + ридж-стабилизация, без numpy.
"""
import json, os, sys, argparse
from datetime import date

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))

# ── линейная алгебра (детерминированная, stdlib) ────────────────────────────

def solve_wls(rows_x, rows_y, rows_w, ridge_rel=1e-6):
    """Взвешенный МНК через нормальные уравнения (XᵀWX + λI)β = XᵀWy."""
    n = len(rows_x[0])
    ata = [[0.0] * n for _ in range(n)]
    atb = [0.0] * n
    for x, y, w in zip(rows_x, rows_y, rows_w):
        for i in range(n):
            wi = w * x[i]
            atb[i] += wi * y
            for j in range(n):
                ata[i][j] += wi * x[j]
    lam = ridge_rel * max(ata[i][i] for i in range(n))
    for i in range(n):
        ata[i][i] += lam
    # Гаусс с выбором главного элемента
    m = [ata[i][:] + [atb[i]] for i in range(n)]
    for col in range(n):
        piv = max(range(col, n), key=lambda r: abs(m[r][col]))
        m[col], m[piv] = m[piv], m[col]
        d = m[col][col]
        for r in range(n):
            if r != col and m[r][col] != 0.0:
                f = m[r][col] / d
                for c in range(col, n + 1):
                    m[r][c] -= f * m[col][c]
    return [m[i][n] / m[i][i] for i in range(n)]


def fit(names, rows_x, rows_y, rows_w):
    beta = solve_wls(rows_x, rows_y, rows_w)
    sw = sum(rows_w)
    ybar = sum(y * w for y, w in zip(rows_y, rows_w)) / sw
    ss_tot = sum(w * (y - ybar) ** 2 for y, w in zip(rows_y, rows_w))
    preds, ss_res = [], 0.0
    for x, y, w in zip(rows_x, rows_y, rows_w):
        p = sum(b * xi for b, xi in zip(beta, x))
        preds.append(p)
        ss_res += w * (y - p) ** 2
    r2 = 1.0 - ss_res / ss_tot if ss_tot > 0 else 1.0
    return {'beta': {nm: round(b, 4) for nm, b in zip(names, beta)},
            '_beta_raw': beta, 'r2': round(r2, 4), 'preds': preds}


def predict(beta_raw, x):
    return sum(b * xi for b, xi in zip(beta_raw, x))


# Запас, который кандидат обязан отыграть, чтобы быть принятым. Строгого
# сравнения НЕ ХВАТАЕТ: замерено на 78 корзинах прогона года, 200 независимых
# пустышек — строгий критерий (loo_with < loo_base) допускает пустышку в 16-18%
# случаев, потому что скользящий контроль сам шумит. Запас 5% сбивает это до
# 0.5% (1 из 200), не мешая настоящей оси: разряд отыгрывает 87%.
GATE_MARGIN = 0.05

# Ниже стольких наблюдений НА СТОЛБЕЦ экзамен не различает и судить отказывается.
# Замер подвыборками 78 корзин прогона года (5 столбцов), доля принятых пустышек:
#   строк   10    13    20    30    40    55    78
#   пустышка 20%  16%   12%    6%    4%   1.7%  0.3%   (настоящая ось: 100% везде)
# Отказ не стоит ничего в силе — разряд принимается при любом объёме. Больше
# КОНТРОЛЕЙ не заменяет данные: три контроля при 13 строках сбивают пустышку до
# 10%, но роняют настоящую ось до 55%; пять — до 6%/36%.
MIN_ROWS_PER_COL = 10


def gate_verdict(loo, bar, n_rows, n_cols):
    if n_rows < n_cols * MIN_ROWS_PER_COL:
        return 'судить не по чему'
    return 'принять' if loo < bar else 'отклонить'


def loo_rmse(rows_x, rows_y, rows_w):
    """Взвешенный RMSE скользящего контроля (leave-one-out).

    Критерий допуска оси в базис: ось принимается, только если улучшает
    предсказание на наблюдении, которого модель НЕ видела при подгонке,
    и улучшает ЗАМЕТНО (GATE_MARGIN). Подгонка «в себя» улучшается от любой
    оси, даже выдуманной, — поэтому решает не R², а этот показатель
    (ИР-020, порог допуска оси).
    """
    n = len(rows_x)
    se = sw = 0.0
    for i in range(n):
        xs = rows_x[:i] + rows_x[i + 1:]
        ys = rows_y[:i] + rows_y[i + 1:]
        ws = rows_w[:i] + rows_w[i + 1:]
        beta = solve_wls(xs, ys, ws)
        err = rows_y[i] - predict(beta, rows_x[i])
        se += rows_w[i] * err * err
        sw += rows_w[i]
    return (se / sw) ** 0.5


def junk_axis(slug, level=0):
    """Детерминированная ось-пустышка: осмысленного сигнала не несёт.

    Нужна как контроль критерия: честный критерий обязан её ОТВЕРГНУТЬ.
    Значения воспроизводимы (FNV-1a от ключа), никакого random.

    Ключ — «<слаг>#<разряд>»: это СОГЛАШЕНИЕ ПРОТОКОЛА, а не деталь реализации.
    Два свидетеля обязаны нарисовать одну и ту же пустышку, иначе экзамен у них
    разный. Тот же ключ в `aggregator::junk_axis` (axis_prices.h).
    """
    name = f'{slug}#{level}'
    h = 2166136261
    for ch in name.encode('utf-8'):
        h = ((h ^ ch) * 16777619) & 0xFFFFFFFF
    return (h % 1000) / 1000.0

# ── учебный набор (demo): маленький модельный мир ───────────────────────────
# оси: K знание · D опасность · P люди · M мастерство (порог входа/лестница).
# Ставки этого мира порождены неявным «законом» (плюс мелкий шум сделок) —
# считалка закона не знает и должна восстановить его из «сделок». Стажёр-
# программист (K высокое, M низкое) и мастер-столяр (K умеренное, M высокое)
# расцепляют знание и мастерство — иначе оси коллинеарны и цены шумят
# (specialty-axes.md §3: оси не ортогональны — цена этого и есть ридж).

DEMO = [
    # имя                          K     D     P     M    ставка  часы
    ('уборка помещений',          0.05, 0.05, 0.10, 0.05,  0.90, 400),
    ('грузчик',                   0.05, 0.20, 0.05, 0.05,  0.95, 300),
    ('ввод данных',               0.25, 0.00, 0.10, 0.15,  1.08, 250),
    ('электрик / монтаж',         0.35, 0.10, 0.05, 0.35,  1.45, 200),
    ('электрик / под напряжением',0.50, 0.85, 0.05, 0.60,  2.60,  80),
    ('сварщик',                   0.40, 0.60, 0.02, 0.45,  2.05, 150),
    ('водитель',                  0.20, 0.35, 0.15, 0.30,  1.60, 220),
    ('преподаватель / лекция',    0.60, 0.00, 0.90, 0.55,  2.05, 180),
    ('медсестра',                 0.55, 0.25, 0.95, 0.50,  2.30, 160),
    ('бухгалтер',                 0.50, 0.00, 0.20, 0.40,  1.65, 190),
    ('хирург / операция',         0.95, 0.50, 0.70, 1.00,  3.20,  40),
    ('стажёр-программист',        0.70, 0.00, 0.05, 0.15,  1.40, 120),
    ('мастер-столяр',             0.30, 0.20, 0.05, 0.90,  2.15,  90),
]
DEFICIT_DEMO = ('электрик / под напряжением', 1.25)
DEFICIT_SIM = ('prof.electrician', 1.25)


def run_demo():
    names_a = ['базовый час', 'знание', 'опасность', 'люди']
    names_b = names_a + ['мастерство']
    xs_a = [[1.0, k, d, p] for _, k, d, p, m, r, h in DEMO]
    xs_b = [[1.0, k, d, p, m] for _, k, d, p, m, r, h in DEMO]
    ws = [float(h) for *_, h in DEMO]
    # нормировка мира на W=1 (economy.md §2б): средневзвешенный час = 1
    raw = [r for *_, r, h in DEMO]
    W = sum(r * w for r, w in zip(raw, ws)) / sum(ws)
    ys = [r / W for r in raw]

    fa, fb = fit(names_a, xs_a, ys, ws), fit(names_b, xs_b, ys, ws)
    rows = []
    for i, (nm, k, d, p, m, _r, h) in enumerate(DEMO):
        y = ys[i]
        rows.append({'name': nm, 'axes': {'знание': k, 'опасность': d,
                                          'люди': p, 'мастерство': m},
                     'hours': h, 'fact': round(y, 2),
                     'pred_a': round(fa['preds'][i], 2),
                     'resid_a': round(y - fa['preds'][i], 2),
                     'pred_b': round(fb['preds'][i], 2),
                     'resid_b': round(y - fb['preds'][i], 2)})

    slug, factor = DEFICIT_DEMO
    i = next(i for i, row in enumerate(DEMO) if row[0] == slug)
    bumped = ys[i] * factor
    pred = predict(fb['_beta_raw'], xs_b[i])
    deficit = {'name': slug, 'factor': factor,
               'fact_normal': round(ys[i], 2), 'fact_bumped': round(bumped, 2),
               'pred': round(pred, 2), 'rent': round(bumped - pred, 2),
               'rent_pct': round(100 * (bumped - pred) / pred, 1)}
    # порог допуска оси: скользящий контроль на невиданном наблюдении.
    # C = пасс A + ось-пустышка; честный критерий обязан её отвергнуть.
    xs_c = [xa + [junk_axis(row[0])] for xa, row in zip(xs_a, DEMO)]  # разряда нет → 0
    gate = {'loo_a': round(loo_rmse(xs_a, ys, ws), 4),
            'loo_b': round(loo_rmse(xs_b, ys, ws), 4),
            'loo_c': round(loo_rmse(xs_c, ys, ws), 4)}
    bar = gate['loo_a'] * (1.0 - GATE_MARGIN)
    gate['bar'] = round(bar, 4)
    gate['rows'] = len(DEMO)
    gate['cols'] = len(names_b)
    gate['verdict_b'] = gate_verdict(gate['loo_b'], bar, gate['rows'], gate['cols'])
    gate['verdict_c'] = gate_verdict(gate['loo_c'], bar, gate['rows'], gate['cols'])

    return {'axes': ['знание', 'опасность', 'люди', 'мастерство'],
            'W': round(W, 4), 'rows': rows, 'gate': gate,
            'passA': {'beta': fa['beta'], 'r2': fa['r2']},
            'passB': {'beta': fb['beta'], 'r2': fb['r2']},
            'deficit': deficit}

# ── реальный прогон: sim-out + каталог ──────────────────────────────────────

def run_sim(out_dir):
    rates_path = os.path.join(out_dir, 'rates_by_month.json')
    cat_path = os.path.join(ROOT, 'docs', 'catalogs', 'professions.json')
    if not os.path.exists(rates_path):
        return None
    months = json.load(open(rates_path))
    cat = {e['slug']: e for e in json.load(open(cat_path))['entries']
           if 'axes' in e}

    agg = {}  # (slug, level) -> [Σ rate·h, Σ h]
    for blob in months.values():
        for r in (blob or {}).get('rates') or []:
            if r['specialty'] not in cat or r['hours'] <= 0:
                continue
            a = agg.setdefault((r['specialty'], r['level']), [0.0, 0.0])
            a[0] += r['rate'] * r['hours']
            a[1] += r['hours']
    if not agg:
        return None

    obs = [{'slug': s, 'level': l, 'rate': v[0] / v[1], 'hours': v[1]}
           for (s, l), v in sorted(agg.items())]
    sw = sum(o['hours'] for o in obs)
    W = sum(o['rate'] * o['hours'] for o in obs) / sw   # economy.md §2б
    for o in obs:
        o['nrate'] = o['rate'] / W
    # Разряд нормируется ПРОТОКОЛЬНЫМ размахом 1..6 (records.md §9.2), а не тем,
    # который случайно попался в данных. Размах по данным делал бы β_разряд
    # несравнимым между свидетелями: у видевшего разряды 3..6 и у видевшего 1..6
    # это разные единицы, и медиану по агрегаторам брать не от чего.
    GRADE_MIN, GRADE_MAX = 1, 6
    span = float(GRADE_MAX - GRADE_MIN)
    gmin = min(o['level'] for o in obs)
    gmax = max(o['level'] for o in obs)

    # Свободного члена НЕТ и долей больше НЕТ (ИР-020, 2026-09-07): шесть
    # независимых степеней, ничего ни во что не суммируется. Час, у которого все
    # степени нули, — час, в котором ничего не происходило, и платить за него
    # нечего. Каждый коэффициент читается прямо: цена часа такой работы.
    # Знание и ответственность (ИР-020) — это то, чем разряд 5 отличается от 2.
    # Пока они лежат в каталоге по слагу, внутри деятельности они одинаковы, и
    # разряд экзамен проходит. Он уйдёт, когда профиль станет различаться по
    # разряду — тогда платить за разряд сверху значит платить дважды.
    names_a = ['физтруд', 'информация', 'люди', 'опасность',
               'знание', 'ответственность']
    names_b = names_a + ['разряд']

    def feats(o, with_grade):
        ax = cat[o['slug']]['axes']
        x = [ax.get('physical', 0.0), ax.get('info', 0.0),
             ax.get('people', 0.0), ax.get('danger', 0.0),
             ax.get('knowledge', 0.0), ax.get('responsibility', 0.0)]
        if with_grade:
            x.append(min(1.0, max(0.0, (o['level'] - GRADE_MIN) / span)))
        return x

    xs_a = [feats(o, False) for o in obs]
    xs_b = [feats(o, True) for o in obs]
    ys = [o['nrate'] for o in obs]
    ws = [o['hours'] for o in obs]
    fa, fb = fit(names_a, xs_a, ys, ws), fit(names_b, xs_b, ys, ws)

    rows = []
    for i, o in enumerate(obs):
        rows.append({'slug': o['slug'], 'ru': cat[o['slug']].get('ru', o['slug']),
                     'level': o['level'], 'hours': round(o['hours'], 1),
                     'fact': round(o['nrate'], 3),
                     'pred_a': round(fa['preds'][i], 3),
                     'resid_a': round(o['nrate'] - fa['preds'][i], 3),
                     'pred_b': round(fb['preds'][i], 3),
                     'resid_b': round(o['nrate'] - fb['preds'][i], 3)})

    slug, factor = DEFICIT_SIM
    deficit = None
    cand = [i for i, o in enumerate(obs) if o['slug'] == slug]
    if cand:
        i = max(cand, key=lambda i: obs[i]['hours'])
        bumped = obs[i]['nrate'] * factor
        pred = predict(fb['_beta_raw'], xs_b[i])
        deficit = {'slug': slug, 'ru': cat[slug].get('ru', slug),
                   'level': obs[i]['level'], 'factor': factor,
                   'fact_normal': round(obs[i]['nrate'], 3),
                   'fact_bumped': round(bumped, 3), 'pred': round(pred, 3),
                   'rent': round(bumped - pred, 3),
                   'rent_pct': round(100 * (bumped - pred) / pred, 1)}

    xs_c = [xa + [junk_axis(o['slug'], o['level'])]
            for xa, o in zip(xs_a, obs)]
    gate = {'loo_a': round(loo_rmse(xs_a, ys, ws), 4),
            'loo_b': round(loo_rmse(xs_b, ys, ws), 4),
            'loo_c': round(loo_rmse(xs_c, ys, ws), 4)}
    bar = gate['loo_a'] * (1.0 - GATE_MARGIN)
    gate['bar'] = round(bar, 4)
    gate['rows'] = len(obs)
    gate['cols'] = len(names_b)
    gate['verdict_b'] = gate_verdict(gate['loo_b'], bar, gate['rows'], gate['cols'])
    gate['verdict_c'] = gate_verdict(gate['loo_c'], bar, gate['rows'], gate['cols'])

    return {'W': round(W, 4), 'months': len(months), 'n_obs': len(obs),
            'gate': gate,
            'n_spec': len({o['slug'] for o in obs}),
            'grade_min': gmin, 'grade_max': gmax,
            'passA': {'beta': fa['beta'], 'r2': fa['r2']},
            'passB': {'beta': fb['beta'], 'r2': fb['r2']},
            'rows': rows, 'deficit': deficit}

# ── отчёт ───────────────────────────────────────────────────────────────────

def print_beta(tag, p):
    print(f'  {tag}: R²={p["r2"]}')
    for nm, b in p['beta'].items():
        print(f'    {nm:<12} {b:+.3f}')


def report(demo, sim):
    print('ИР-020 · цены осей: гедонистическое разложение ставок')
    print('невязка — диагностика, не платёж (specialty-axes.md §7: рельеф, не ось)')
    print()
    print(f'── учебный пример ({len(demo["rows"])} деятельностей, '
          f'W_мира={demo["W"]}) ──')
    print_beta('пасс A (без мастерства)', demo['passA'])
    print_beta('пасс B (+мастерство)   ', demo['passB'])
    worst = max(demo['rows'], key=lambda r: abs(r['resid_a']))
    print(f'  наибольшая невязка пасса A: {worst["name"]} '
          f'{worst["resid_a"]:+.2f} (факт {worst["fact"]}, '
          f'предсказание {worst["pred_a"]}) → сигнал недостающей оси')
    print(f'  она же в пассе B: {worst["resid_b"]:+.2f} '
          f'(предсказание {worst["pred_b"]})')
    g = demo['gate']
    print(f'  допуск оси (скользящий контроль, RMSE — меньше лучше;'
          f' планка {g["bar"]:.4f} = запас {GATE_MARGIN:.0%}):')
    print(f'    без мастерства        {g["loo_a"]:.4f}')
    print(f'    + мастерство          {g["loo_b"]:.4f}  → {g["verdict_b"]}')
    print(f'    + ось-пустышка        {g["loo_c"]:.4f}  → {g["verdict_c"]}')
    d = demo['deficit']
    print(f'  дефицит-эксперимент: {d["name"]} ×{d["factor"]} → '
          f'рента {d["rent"]:+.2f} ({d["rent_pct"]:+.1f}% к структуре)')
    print()
    if sim is None:
        print('── прогон года: нет sim-out/rates_by_month.json '
              '(сначала docs/pilot/sim-year.sh) ──')
        return
    print(f'── прогон года (sim-year.sh): {sim["n_spec"]} специальностей, '
          f'{sim["n_obs"]} корзин специальность×разряд, W={sim["W"]} ──')
    print_beta('пасс A (оси каталога)', sim['passA'])
    print_beta('пасс B (+разряд)     ', sim['passB'])
    g = sim['gate']
    print(f'  допуск оси: без разряда {g["loo_a"]:.4f} · '
          f'+разряд {g["loo_b"]:.4f} ({g["verdict_b"]}) · '
          f'+пустышка {g["loo_c"]:.4f} ({g["verdict_c"]})')
    d = sim.get('deficit')
    if d:
        print(f'  дефицит-эксперимент: {d["ru"]} р.{d["level"]} ×{d["factor"]} '
              f'→ рента {d["rent"]:+.3f} ({d["rent_pct"]:+.1f}%)')

# ── main ────────────────────────────────────────────────────────────────────

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--out', default=os.path.join(HERE, 'sim-out'))
    ap.add_argument('--html', action='store_true',
                    help='собрать sim-out/axes-presentation.html по шаблону')
    args = ap.parse_args()
    os.makedirs(args.out, exist_ok=True)

    demo = run_demo()
    sim = run_sim(args.out)
    report(demo, sim)

    data = {'generated': date.today().isoformat(), 'demo': demo, 'sim': sim}
    for section in (demo, (sim or {})):
        for p in (section.get('passA'), section.get('passB')):
            if p:
                p.pop('_beta_raw', None)
                p.pop('preds', None)
    out_json = os.path.join(args.out, 'axis-prices.json')
    json.dump(data, open(out_json, 'w'), ensure_ascii=False, indent=1)
    print(f'\n→ {out_json}')

    if args.html:
        tpl_path = os.path.join(HERE, 'axes-presentation-template.html')
        if not os.path.exists(tpl_path):
            sys.exit('нет axes-presentation-template.html — HTML пропущен')
        tpl = open(tpl_path, encoding='utf-8').read()
        html = tpl.replace('__DATA__', json.dumps(data, ensure_ascii=False))
        out_html = os.path.join(args.out, 'axes-presentation.html')
        open(out_html, 'w', encoding='utf-8').write(html)
        print(f'→ {out_html}')


if __name__ == '__main__':
    main()
