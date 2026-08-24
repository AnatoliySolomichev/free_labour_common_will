#!/usr/bin/env bash
# Снимок переписки Claude Code в каталог проекта.
# Идемпотентен: можно запускать сколько угодно раз, ничего не теряется.
#
#   ./claude-history/backup.sh [дополнительный_каталог_с_jsonl ...]
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$HERE")"
CLAUDE_HOME="${CLAUDE_CONFIG_DIR:-$HOME/.claude}"
PROJ_KEY="$(printf '%s' "$PROJECT_ROOT" | sed 's#/#-#g')"   # /workspace → -workspace

LIVE="$CLAUDE_HOME/projects/$PROJ_KEY"
LEGACY="$PROJECT_ROOT/.claude-container-backup/projects/$PROJ_KEY"

args=()
[ -d "$LIVE" ]   && args+=(--source "$LIVE")
[ -d "$LEGACY" ] && args+=(--source "$LEGACY")
for extra in "$@"; do
  [ -d "$extra" ] && args+=(--source "$extra")
done

if [ ${#args[@]} -eq 0 ]; then
  echo "Не найдено ни одного источника (искал: $LIVE)" >&2
  exit 1
fi

echo "── Слияние транскриптов ────────────────────────────────"
python3 "$HERE/archive.py" --archive "$HERE/raw" "${args[@]}"

# Глобальные мелочи. .credentials.json НЕ копируем — это секрет.
mkdir -p "$HERE/raw/_global"
for f in settings.json history.jsonl; do
  [ -f "$CLAUDE_HOME/$f" ] && cp -p "$CLAUDE_HOME/$f" "$HERE/raw/_global/$f"
done

echo
echo "── Рендер в Markdown и HTML ────────────────────────────"
python3 "$HERE/render.py" --root "$HERE"

echo
echo "Готово. Читать: $HERE/html/index.html   (или $HERE/INDEX.md)"
du -sh "$HERE" | sed 's/^/Объём архива: /'
