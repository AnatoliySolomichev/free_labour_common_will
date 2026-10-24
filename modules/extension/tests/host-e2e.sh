#!/usr/bin/env bash
# Хост расширения вживую (extension.md §3): свой агрегатор, личность, bc
# native-install в отдельный профиль и проверка протокола host_test.py; то же
# через сценарий для хоста bc native-install --docker (extension.md §5.1); если в
# системе есть Node.js и Playwright — ещё и настоящий Chromium с расширением
# (browser_test.cjs), а если Firefox и geckodriver — настоящий Firefox с bc.xpi
# (firefox_test.py).
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
# У Firefox манифест хоста не в профиле, а в ~/.mozilla: --profile ему — отказ.
if "$BC" --data-dir "$work/me" native-install --via "$VIA" --browser firefox --profile "$work/profile" 2>/dev/null; then
    echo "   ✗ --profile у Firefox: манифест лёг туда, где Firefox его не ищет"
    exit 1
fi
echo "   ✓ --profile у Firefox — отказ: манифест хоста он ищет в ~/.mozilla"

echo "── протокол хоста ──"
python3 "$here/host_test.py" "$BC" "$work/me" "$VIA"

# Браузер на хосте, bc в контейнере (extension.md §5.1). Хост и контейнер здесь —
# один компьютер: docker подменён, у «хоста» свой дом, у bc — своя личность.
echo "── браузер на хосте, bc в контейнере ──"
mkdir -p "$work/fake-bin" "$work/host-home"
cat >"$work/fake-bin/docker" <<'EOF'
#!/bin/sh
# Подмена docker: «контейнер» — этот же компьютер.
case "$1" in
    container) exit 0 ;;                                        # container inspect ИМЯ
    exec) shift; [ "$1" = -i ] && shift; shift; exec "$@" ;;    # exec -i ИМЯ КОМАНДА…
esac
exit 1
EOF
chmod +x "$work/fake-bin/docker"
"$BC" --data-dir "$work/me2" identity create >/dev/null 2>&1
"$BC" --data-dir "$work/me2" native-install --via "$VIA" --docker dev-1 >"$work/install-host.sh" 2>/dev/null
HOME="$work/host-home" PATH="$work/fake-bin:$PATH" sh "$work/install-host.sh" | sed 's/^/   /'
# Хост — как его запускает Firefox на хосте участника (extension.md §5.1).
PATH="$work/fake-bin:$PATH" python3 "$here/host_test.py" "$BC" "$work/me2" "$VIA" \
    "$work/host-home/.config/chromium/NativeMessagingHosts/org.free_labour.bc.json" \
    "$work/host-home/.mozilla/native-messaging-hosts/org.free_labour.bc.json"

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

# Firefox ищет манифест хоста только в ~/.mozilla — у него в проверке свой дом.
if [[ "${BROWSER:-auto}" != "no" ]] && command -v firefox >/dev/null && command -v geckodriver >/dev/null; then
    echo "── расширение в Firefox ──"
    xpi="$BUILD/modules/extension/bc.xpi"
    [[ -f "$xpi" ]] || { echo "нет $xpi — собери проект: cmake --build build" >&2; exit 1; }
    "$BC" --data-dir "$work/me3" identity create >/dev/null 2>&1
    HOME="$work/ff-home" "$BC" --data-dir "$work/me3" native-install --via "$VIA" --browser firefox \
        | sed 's/^/   /'
    python3 "$here/firefox_test.py" "$xpi" "$work/ff-home" "$BC" "$work/me3"
else
    echo "── расширение в Firefox: пропущено (нет Firefox или geckodriver) ──"
fi
