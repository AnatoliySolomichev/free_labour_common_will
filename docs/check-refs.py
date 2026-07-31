#!/usr/bin/env python3
"""Проверка ссылок вида `файл.md §N` по всему проекту.

Конвенция (CLAUDE.md, раздел «Ссылки на разделы документов»):
  каждая ссылка называет документ явно и стоит вплотную к номеру:
      records.md §11.2        blockchain.md §6.7 rule 9
  голая `§N` — нарушение: по ней нельзя понять, о каком документе речь.

Исключения: ссылки на внешние стандарты (RFC 8949 §4.2.1).

Запуск:  python3 docs/check-refs.py        (из корня проекта)
Код возврата: 0 — всё чисто, 1 — есть нарушения.
"""

import os
import re
import sys
import glob

DOC_GLOB = 'docs/**/*.md'
SRC_EXT = ('.cpp', '.h', '.hpp', '.cc')
SRC_GLOB = 'modules/**/*'

# Внешние стандарты: их §-нумерация к нашим документам отношения не имеет.
EXTERNAL = re.compile(r'\b(RFC|ISO|IEEE|IETF)\s*[0-9-]+')

HEADING = re.compile(r'^#+\s+([0-9]+[А-Яа-яA-Za-z]?(?:\.[0-9]+)*)\.')
REF = re.compile(r'§\s*([0-9]+[А-Яа-яA-Za-z]?(?:\.[0-9]+)*)')
QUALIFIED = re.compile(r'([a-z][a-z-]*\.md)\s*§')


def index_sections(path):
    """Номера разделов документа: только заголовки, не пункты списков."""
    return {m.group(1) for m in map(HEADING.match, open(path, encoding='utf-8')) if m}


def main():
    docs = {os.path.basename(p): index_sections(p)
            for p in sorted(glob.glob(DOC_GLOB, recursive=True))}
    files = sorted(glob.glob(DOC_GLOB, recursive=True)) + \
        sorted(p for p in glob.glob(SRC_GLOB, recursive=True) if p.endswith(SRC_EXT))

    bare, stale, total = [], [], 0
    for path in files:
        prev_ends_with_doc = False
        for lineno, line in enumerate(open(path, encoding='utf-8', errors='replace'), 1):
            line = line.rstrip('\n')
            if EXTERNAL.search(line):
                prev_ends_with_doc = False
                continue
            # позиции «имя.md» — ссылка обязана стоять сразу за одной из них
            anchors = [(m.start(1), m.end(1), m.group(1)) for m in QUALIFIED.finditer(line)]
            for m in REF.finditer(line):
                total += 1
                num, pos = m.group(1), m.start()
                owner = next((f for s, e, f in anchors if e <= pos and not line[e:pos].strip()), None)
                if owner is None:
                    # ссылка могла быть перенесена на новую строку комментария
                    if prev_ends_with_doc and not anchors:
                        continue
                    bare.append((path, lineno, num, line.strip()[:90]))
                    continue
                if owner not in docs:
                    stale.append((path, lineno, f'{owner} §{num}', 'нет такого документа'))
                elif num not in docs[owner]:
                    stale.append((path, lineno, f'{owner} §{num}', 'нет такого раздела'))
            tail = re.search(r'([a-z][a-z-]*\.md)`?\s*$', line)
            prev_ends_with_doc = bool(tail and tail.group(1) in docs)

    for path, lineno, num, ctx in bare:
        print(f'{path}:{lineno}: голая ссылка §{num} — не указан документ | {ctx}')
    for path, lineno, ref, why in stale:
        print(f'{path}:{lineno}: {ref} — {why}')

    bad = len(bare) + len(stale)
    print(f'\nпроверено ссылок: {total}; голых: {len(bare)}; битых: {len(stale)}')
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
