#!/usr/bin/env python3
"""Слияние JSONL-транскриптов Claude Code между живым ~/.claude и архивом.

Слияние идёт по uuid записи (для служебных записей без uuid — по хешу строки),
поэтому операция идемпотентна и никогда не укорачивает файл: за основу берётся
самый длинный вариант, недостающие записи из остальных копий дописываются в конец.
Claude Code восстанавливает порядок по цепочке parentUuid, так что дописывание
в конец безопасно.
"""
import argparse
import hashlib
import json
import shutil
from pathlib import Path


def key_of(line: str, rec):
    if isinstance(rec, dict):
        u = rec.get("uuid")
        if u:
            return ("uuid", u)
    return ("hash", hashlib.sha1(line.encode("utf-8")).hexdigest())


def read_records(path: Path):
    out = []
    with path.open(encoding="utf-8", errors="replace") as f:
        for line in f:
            line = line.rstrip("\n")
            if not line.strip():
                continue
            try:
                rec = json.loads(line)
            except json.JSONDecodeError:
                rec = None
            out.append((key_of(line, rec), line))
    return out


def merge_files(paths, dst: Path):
    """paths — все известные копии сессии (включая dst). Возвращает (всего, добавлено)."""
    variants = [(p, read_records(p)) for p in paths if p.exists()]
    if not variants:
        return 0, 0
    variants.sort(key=lambda v: len(v[1]), reverse=True)
    seen = set()
    merged = []
    added_from_others = 0
    for i, (p, recs) in enumerate(variants):
        for k, line in recs:
            if k in seen:
                continue
            seen.add(k)
            merged.append(line)
            if i > 0:
                added_from_others += 1
    dst.parent.mkdir(parents=True, exist_ok=True)
    before = len(read_records(dst)) if dst.exists() else 0
    tmp = dst.with_name(dst.name + ".tmp")
    tmp.write_text("\n".join(merged) + "\n", encoding="utf-8")
    tmp.replace(dst)
    return len(merged), len(merged) - before


def copy_tree_newer(src: Path, dst: Path):
    """Копирует файлы из src в dst, если в dst их нет или они старше."""
    n = 0
    if not src.is_dir():
        return 0
    for s in src.rglob("*"):
        if not s.is_file():
            continue
        d = dst / s.relative_to(src)
        if d.exists() and d.stat().st_mtime >= s.stat().st_mtime and d.stat().st_size == s.stat().st_size:
            continue
        d.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(s, d)
        n += 1
    return n


def collect(dirs):
    """{session_id: [пути к .jsonl]} по списку каталогов."""
    found = {}
    for d in dirs:
        d = Path(d)
        if not d.is_dir():
            continue
        for p in sorted(d.glob("*.jsonl")):
            found.setdefault(p.stem, []).append(p)
    return found


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--archive", required=True, help="каталог raw/ архива")
    ap.add_argument("--source", action="append", default=[], help="каталог-источник (можно несколько)")
    ap.add_argument("--direction", choices=["to-archive", "to-live"], default="to-archive")
    ap.add_argument("--live", help="живой каталог ~/.claude/projects/-workspace (для --direction to-live)")
    args = ap.parse_args()

    archive = Path(args.archive)
    archive.mkdir(parents=True, exist_ok=True)

    if args.direction == "to-archive":
        found = collect([archive] + args.source)
        total_new = 0
        for sid, paths in sorted(found.items()):
            dst = archive / f"{sid}.jsonl"
            n, new = merge_files(set(paths) | {dst}, dst)
            total_new += max(new, 0)
            flag = f"  +{new}" if new else ""
            print(f"  {sid}  {n:>6} записей{flag}")
        # вспомогательные каталоги: memory/, tool-results/
        extra = 0
        for s in args.source:
            s = Path(s)
            extra += copy_tree_newer(s / "memory", archive / "memory")
            for sub in s.iterdir() if s.is_dir() else []:
                if sub.is_dir() and sub.name not in ("memory",):
                    extra += copy_tree_newer(sub, archive / sub.name)
        print(f"\nСессий в архиве: {len(found)}; новых записей: {total_new}; доп. файлов: {extra}")
    else:
        live = Path(args.live)
        live.mkdir(parents=True, exist_ok=True)
        restored = 0
        for p in sorted(archive.glob("*.jsonl")):
            dst = live / p.name
            n, new = merge_files({p, dst}, dst)
            if new:
                restored += 1
            print(f"  {p.stem}  {n:>6} записей  {'+' + str(new) if new else 'без изменений'}")
        copy_tree_newer(archive / "memory", live / "memory")
        print(f"\nВосстановлено/дополнено сессий: {restored} из {len(list(archive.glob('*.jsonl')))}")


if __name__ == "__main__":
    main()
