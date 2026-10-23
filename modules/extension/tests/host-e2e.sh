#!/usr/bin/env bash
# Хост расширения вживую (extension.md §3): свой агрегатор, личность, bc
# native-install в отдельный профиль и проверка протокола host_test.py; если в
# системе есть Node.js и Playwright — ещё и настоящий Chromium с расширением
# (browser_test.cjs).
#
# Запуск:   modules/extension/tests/host-e2e.sh      (собери проект: cmake --build build)
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD="${BUILD:-$here/../../../build}"
BC="$BUILD/modules/cli/bc"
AGG="$BUILD/modules/aggregator/aggregator_server"
PORT="${PORT:-19370}"
VIA="http://localhost:$PORT"

work="$(mktemp -d)"
agg_pid=""
cleanup() {
    [[ -n "$agg_pid" ]] && kill "$agg_pid" 2>/dev/null || true
    rm -rf "$work"
}
trap cleanup EXIT

"$AGG" --port "$PORT" --db "$work/agg" >"$work/agg.log" 2>&1 &
agg_pid=$!
for _ in $(seq 1 40); do curl -sf -o /dev/null "$VIA/stats" && break || sleep 0.25; done

"$BC" --data-dir "$work/me" identity create >/dev/null 2>&1
"$BC" --data-dir "$work/me" native-install --via "$VIA" --browser chromium --profile "$work/profile" \
    | sed 's/^/   /'

echo "── протокол хоста ──"
python3 "$here/host_test.py" "$BC" "$work/me" "$VIA"

if [[ "${BROWSER:-auto}" != "no" ]] && command -v node >/dev/null && \
   NODE_PATH="${NODE_PATH:-$(npm root -g 2>/dev/null)}" node -e "require('playwright')" 2>/dev/null; then
    echo "── расширение в Chromium ──"
    run=(node)
    command -v xvfb-run >/dev/null && run=(xvfb-run -a node)
    NODE_PATH="${NODE_PATH:-$(npm root -g)}" "${run[@]}" "$here/browser_test.cjs" \
        "$here/.." "$work/profile" "$BC" "$work/me" "$VIA"
else
    echo "── расширение в Chromium: пропущено (нет Node.js или Playwright) ──"
fi
