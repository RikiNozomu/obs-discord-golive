#!/usr/bin/env bash
# Loads the built plugin into a headless libobs and checks registration,
# settings validation, forced encoder settings and start-failure messages.
#
# Usage: tests/run-module-test.sh <obs-sdk-prefix> [path/to/obs-discord-golive.so]
#   <obs-sdk-prefix> must contain include/obs and lib/libobs.so (OBS 31.x).
# Needs a display on Linux; under CI or SSH it uses xvfb-run if available.
set -euo pipefail

SDK="${1:?usage: $0 <obs-sdk-prefix> [plugin.so]}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SO="${2:-$ROOT/build_x86_64/obs-discord-golive.so}"
WORK="$(mktemp -d)"
trap 'rm -rf -- "$WORK"' EXIT

gcc -o "$WORK/module-test" "$ROOT/tests/module-test.c" -I"$SDK/include/obs" -L"$SDK/lib" -lobs -Wl,-rpath,"$SDK/lib"
echo "fake-token-for-tests" > "$WORK/token.txt"

RUN=()
if [[ -z "${DISPLAY:-}" ]] && command -v xvfb-run >/dev/null; then RUN=(xvfb-run -a); fi
"${RUN[@]}" "$WORK/module-test" "$SO" "$ROOT/data" "$WORK/token.txt" | grep -v '^info: \|^debug: '
