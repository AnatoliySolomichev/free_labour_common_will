#!/usr/bin/env python3
"""
ИР-020, три замера, на которых стоят ратифицированные решения (2026-09-03).

Считалка цен осей (docs/pilot/axis-prices.py) отвечает «сколько стоит ось».
Этот прототип отвечает на три вопроса, которые считалка не закрывает:

  форма  — оси складываются или перемножаются? (ратифицировано: складываются)
  рента  — как отличить дефицит от канала?     (по тому, КТО платит ренту)
  эхо    — ловится ли списанный профиль?       (нет; правило отклонено)

Каждый ответ проверяем на данных, а не на убеждении: два первых замера
воспроизведены тестами C++ (test_rent_map.cpp), третий — единственный, который
дал ОТРИЦАТЕЛЬНЫЙ результат, и потому правила из него не выросло.

Запуск:  python3 docs/pilot/form-and-rent.py [--out ПАПКА]
Зависимости: только stdlib. Нужен прогон sim-year.sh (rates_by_month.json,
deals.tsv) — без него секции «рента» и часть «формы» пропускаются.
"""
import argparse, collections, csv, importlib.util, json, math, os, random, statistics

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))

_spec = importlib.util.spec_from_file_location("ap", os.path.join(HERE, "axis-prices.py"))
ap = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(ap)


# ── 1. форма: сумма или произведение ────────────────────────────────────────

def loo_in_rate_units(xs, ys, ws, log):
    """Скользящий контроль В ЕДИНИЦАХ СТАВКИ — иначе формы несравнимы.

    Ошибку лог-подгонки нельзя мерить в логарифмах и сравнивать с линейной:
    надо вернуться в исходные единицы, а при возврате e^ε в среднем не равно 1,
    поэтому применяется поправка Дуана (smearing).
    """
    n, se, sw = len(xs), 0.0, 0.0
    for i in range(n):
        X, W = xs[:i] + xs[i+1:], ws[:i] + ws[i+1:]
        rest = ys[:i] + ys[i+1:]
        Y = [math.log(y) for y in rest] if log else rest
        b = ap.solve_wls(X, Y, W)
        p = ap.predict(b, xs[i])
        if log:
            res = [math.log(y) - ap.predict(b, x) for x, y in zip(X, rest)]
            p = math.exp(p) * (sum(w * math.exp(r) for r, w in zip(res, W)) / sum(W))
        se += ws[i] * (ys[i] - p) ** 2
        sw += ws[i]
    return (se / sw) ** 0.5


def normalize(raw, ws):
    """W = средневзвешенный час (economy.md §2б); ставки делятся на него."""
    W = sum(r * w for r, w in zip(raw, ws)) / sum(ws)
    return [r / W for r in raw], W


def section_form(out_dir):
    print("=" * 72)
    print("1. ФОРМА: складываются оси или перемножаются?")
    print("=" * 72)
    print("""
Линейная:  ставка ≈ β0 + Σ βj·xj      «за столько-то опасности — столько-то часов»
Лог-:      ставка ≈ e^β0 · Π e^(βj·xj)  «за вредность +30%» — от чего угодно
""")
    ws = [float(h) for *_, h in ap.DEMO]
    ys, _ = normalize([r for *_, r, h in ap.DEMO], ws)
    xs = [[1.0, k, d, p, m] for _, k, d, p, m, r, h in ap.DEMO]
    lin, lg = loo_in_rate_units(xs, ys, ws, False), loo_in_rate_units(xs, ys, ws, True)
    print(f"учебный мир ({len(xs)} деятельностей): "
          f"LOO линейной {lin:.4f}, лог {lg:.4f}")

    rates_path = os.path.join(out_dir, "rates_by_month.json")
    if os.path.exists(rates_path):
        obs, cat = sim_baskets(rates_path)
        ws2 = [o["hours"] for o in obs]
        ys2, _ = normalize([o["rate"] for o in obs], ws2)
        xs2 = [[1.0, cat[o["slug"]]["axes"].get("info", 0.0),
                cat[o["slug"]]["axes"].get("people", 0.0),
                cat[o["slug"]]["axes"].get("danger", 0.0),
                (o["level"] - 1) / 5.0] for o in obs]
        print(f"прогон года ({len(obs)} корзин):        "
              f"LOO линейной {loo_in_rate_units(xs2, ys2, ws2, False):.4f}, "
              f"лог {loo_in_rate_units(xs2, ys2, ws2, True):.4f}")

    print("""
Ни то, ни другое ничего не решает: оба мира синтетические и порождены почти
линейным законом. Поэтому — где проходит граница? Поднимаем одну ставку,
остальное не трогаем.
""")
    print(f"  {'ставка хирурга':>16}{'LOO линейной':>14}{'LOO лог':>10}   кто лучше")
    for factor in (1.0, 2.0, 4.0, 6.5):
        rows = [list(r) for r in ap.DEMO]
        for r in rows:
            if r[0].startswith("хирург"):
                r[5] *= factor
        w = [float(r[6]) for r in rows]
        y, _ = normalize([r[5] for r in rows], w)
        x = [[1.0, r[1], r[2], r[3], r[4]] for r in rows]
        a, b = loo_in_rate_units(x, y, w, False), loo_in_rate_units(x, y, w, True)
        i = next(i for i, r in enumerate(rows) if r[0].startswith("хирург"))
        print(f"  {y[i]:>16.2f}{a:>14.4f}{b:>10.4f}   "
              f"{'лог' if b < a else 'линейная'}")
    print("""
Перелом — примерно на четырёхкратном разрыве лучшего часа со средним. То есть
спор был не о форме, а о том, каким мы ХОТИМ видеть разброс оплаты. Выбор сделан
по принципу (ценность не должна рождаться из соседства признаков), а не по
подгонке; лог-подгонка не публикуется вовсе.""")


def sim_baskets(rates_path):
    cat_path = os.path.join(ROOT, "docs", "catalogs", "professions.json")
    cat = {e["slug"]: e for e in json.load(open(cat_path))["entries"] if "axes" in e}
    agg = {}
    for blob in json.load(open(rates_path)).values():
        for r in (blob or {}).get("rates") or []:
            if r["specialty"] not in cat or r["hours"] <= 0:
                continue
            a = agg.setdefault((r["specialty"], r["level"]), [0.0, 0.0])
            a[0] += r["rate"] * r["hours"]
            a[1] += r["hours"]
    obs = [{"slug": s, "level": l, "rate": v[0] / v[1], "hours": v[1]}
           for (s, l), v in sorted(agg.items())]
    return obs, cat


# ── 2. рента: дефицит или канал ─────────────────────────────────────────────

def edge_reference(deals):
    """Опора корзины — медиана по РЁБРАМ (ИР-021): все сделки одной пары
    плательщик→работник схлопываются в одну точку. Иначе достаточно нескольких
    сговорившихся, чтобы опора стала их собственной ценой."""
    edges = collections.defaultdict(lambda: [0.0, 0.0])
    for d in deals:
        e = edges[(d["payer"], d["worker"])]
        e[0] += d["coef"] * d["hours"]
        e[1] += d["hours"]
    return statistics.median(sorted(v[0] / v[1] for v in edges.values()))


def effective_payers(deals, ref, by_rent):
    """1/HHI — «сколько плательщиков, если бы все были равны».

    by_rent=False: доля в ЧАСАХ.   by_rent=True: доля в ИЗБЫТКЕ над опорой.
    """
    share = collections.Counter()
    for d in deals:
        if by_rent:
            over = max(0.0, d["coef"] - ref)
            if over > 0:
                share[d["payer"]] += over * d["hours"]
        else:
            share[d["payer"]] += d["hours"]
    total = sum(share.values())
    if total <= 0:
        return float("nan")
    hhi = sum((v / total) ** 2 for v in share.values())
    return 1.0 / hhi if hhi > 0 else float("nan")


def section_rent(out_dir):
    print()
    print("=" * 72)
    print("2. РЕНТА: один и тот же остаток — два разных зверя")
    print("=" * 72)
    path = os.path.join(out_dir, "deals.tsv")
    if not os.path.exists(path):
        print(f"  нет {path} — запустите sim-year.sh")
        return
    rows = list(csv.DictReader(open(path), delimiter="\t"))
    for r in rows:
        r["hours"] = float(r["hours"])
        r["coef"] = float(r["coef"])
    print("""
дефицит — электриков не хватает, дороже платят ВСЕ: рыночный сигнал.
канал   — один плательщик переплачивает одному работнику: труба.
Величина ренты их не различает. Различает — кто её платит.
""")
    tgt = ("prof.tailor", "5")
    mine = [r for r in rows if (r["specialty"], r["level"]) == tgt]
    base = sum(r["coef"] * r["hours"] for r in mine) / sum(r["hours"] for r in mine)

    channel = rows + [{"month": str(m), "payer": "999", "worker": "998",
                       "specialty": tgt[0], "level": tgt[1],
                       "hours": 8.0, "coef": base * 1.6} for m in range(12)]
    scarce = [dict(r, coef=r["coef"] * 1.6)
              if (r["specialty"], r["level"]) == tgt else r for r in rows]

    print(f"  {'корзина':<34}{'по часам':>10}{'ПО РЕНТЕ':>10}")
    for label, data, key in (("честная prof.cook р.3", rows, ("prof.cook", "3")),
                             ("честная prof.welder р.4", rows, ("prof.welder", "4")),
                             ("честная prof.tailor р.5", rows, tgt),
                             ("КАНАЛ (один платит ×1.6)", channel, tgt),
                             ("дефицит ×1.6 (платят все)", scarce, tgt)):
        d = [r for r in data if (r["specialty"], r["level"]) == key]
        ref = edge_reference(d)
        print(f"  {label:<34}{effective_payers(d, ref, False):>10.1f}"
              f"{effective_payers(d, ref, True):>10.1f}")

    vals = []
    for key in {(r["specialty"], r["level"]) for r in rows}:
        d = [r for r in rows if (r["specialty"], r["level"]) == key]
        if len(d) < 8:
            continue
        v = effective_payers(d, edge_reference(d), True)
        if v == v:
            vals.append(v)
    vals.sort()
    print(f"\n  честный фон по {len(vals)} корзинам (плательщиков за ренту):")
    for q, lab in ((0.05, "5%"), (0.5, "медиана"), (0.95, "95%")):
        print(f"    {lab:>8}: {vals[int(q * (len(vals) - 1))]:.1f}")
    print("""
По часам порога НЕ СУЩЕСТВУЕТ: канал попадает внутрь честного разброса.
По ренте канал схлопывается в одного, а дефицит не двигается вовсе — когда
дорожают все, опора уезжает вместе с ними и избытка нет ни у кого.
Отсюда три числа карты ренты и отказ считать сводный балл.""")


# ── 3. эхо: ловится ли списанный профиль ────────────────────────────────────

def exact_agreement(grid, sigma, axes, n=20000, seed=7):
    """Доля ЧЕСТНЫХ пар, чьи округлённые оценки совпали в точку по всем осям."""
    rng = random.Random(seed)
    same = 0
    for _ in range(n):
        truth = rng.uniform(0.05, 0.95)
        if all(abs(round((truth + rng.gauss(0, sigma)) / grid) * grid
                   - round((truth + rng.gauss(0, sigma)) / grid) * grid) < 1e-9
               for _ in range(axes)):
            same += 1
    return 100.0 * same / n


def section_echo():
    print()
    print("=" * 72)
    print("3. ЭХО: покупатель видит профиль продавца и может его списать")
    print("=" * 72)
    print("""
Соблазнительное правило: «совпали в точку — значит списали». Проверяем его на
честном мире, где стороны судят независимо, но пишут ОКРУГЛЁННЫЕ числа.
""")
    for axes in (1, 3):
        print(f"  совпасть надо по {axes} " + ("оси:" if axes == 1 else "осям сразу:"))
        print(f"    {'сетка':>8}" + "".join(f"{'σ=' + str(s):>10}" for s in (0.02, 0.05, 0.10)))
        for grid in (0.05, 0.10, 0.25):
            row = f"    {grid:>8.2f}"
            for s in (0.02, 0.05, 0.10):
                row += f"{exact_agreement(grid, s, axes):>9.1f}%"
            print(row)
    print("""
Правило ОТКЛОНЕНО: на сетке 0.05 при обычном разбросе восприятия 27% честных пар
совпадают в точку случайно. По трём осям строже, но зависимость от сетки слишком
крутая для фиксированного порога — а сетку задают люди, не протокол.

Единственное настоящее решение — обязательство и раскрытие (хеш профиля до
расчёта, раскрытие после). Оставлено в запасе: пока эхо дешевле показывать
прибором (распределение расхождений), чем ловить полицейским.""")


def main():
    pa = argparse.ArgumentParser()
    pa.add_argument("--out", default=os.path.join(HERE, "sim-out"))
    args = pa.parse_args()
    section_form(args.out)
    section_rent(args.out)
    section_echo()


if __name__ == "__main__":
    main()
