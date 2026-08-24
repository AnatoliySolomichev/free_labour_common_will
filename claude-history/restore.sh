#!/usr/bin/env bash
# Возврат переписки из архива в ~/.claude после пересборки контейнера.
# Слияние, а не перезапись: живые сессии не затираются.
#
#   ./claude-history/restore.sh
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$HERE")"
CLAUDE_HOME="${CLAUDE_CONFIG_DIR:-$HOME/.claude}"
PROJ_KEY="$(printf '%s' "$PROJECT_ROOT" | sed 's#/#-#g')"
LIVE="$CLAUDE_HOME/projects/$PROJ_KEY"

echo "Восстанавливаю в: $LIVE"
mkdir -p "$LIVE"
python3 "$HERE/archive.py" --archive "$HERE/raw" --direction to-live --live "$LIVE"

# каталоги вложений сессий (tool-results и т.п.)
for d in "$HERE"/raw/*/; do
  name="$(basename "$d")"
  case "$name" in memory|_global) continue;; esac
  mkdir -p "$LIVE/$name"
  cp -rpn "$d." "$LIVE/$name/" 2>/dev/null || true
done

echo
echo "Готово. Перезапустите Claude Code, затем: claude --resume"
