#!/usr/bin/env bash
# Builds a public macOS release (Universal: Apple Silicon + Intel).
#
#   scripts/release-macos.sh
#
# Output in release/:
#   obs-discord-golive-<version>-macos-universal.pkg   installer
#   obs-discord-golive-<version>-macos-universal.zip   the .plugin, for manual install
#
# Steps:
#   1. Stage the helper's production dependencies (no dev deps) with native
#      modules for both arm64 and x64, so the plugin works on every Mac.
#   2. Configure + build the plugin in Release with that staged node_modules.
#   3. cmake --install, which also builds the .pkg (cmake/macos/resources).
#
# Signing: if CODESIGN_IDENT is set (a "Developer ID Application" identity) the
# preset signs with it; otherwise the build is ad-hoc signed and users must
# allow it in System Settings → Privacy & Security on first launch.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

VERSION="$(node -p "require('./buildspec.json').version")"
NAME="obs-discord-golive-${VERSION}-macos-universal"
STAGE="$ROOT/build_release/helper-darwin"
X64="$ROOT/build_release/helper-darwin-x64"
OUT="$ROOT/release"

echo "==> Staging helper dependencies (arm64 + x64)"
rm -rf "$STAGE" "$X64"
mkdir -p "$STAGE" "$X64"
for dir in "$STAGE" "$X64"; do cp helper/package.json helper/package-lock.json "$dir/"; done
(cd "$STAGE" && npm ci --omit=dev --ignore-scripts --no-audit --no-fund --cpu=arm64 --os=darwin)
(cd "$X64" && npm ci --omit=dev --ignore-scripts --no-audit --no-fund --cpu=x64 --os=darwin)

# Native modules ship as one package per platform (e.g. @snazzah/davey-darwin-x64);
# copy the x64 ones next to the arm64 ones. Node picks the right one at runtime.
(cd "$X64/node_modules" && find . -maxdepth 2 -type d -name '*darwin-x64*') | while read -r pkg; do
	mkdir -p "$STAGE/node_modules/$(dirname "$pkg")"
	rm -rf "$STAGE/node_modules/$pkg"
	cp -R "$X64/node_modules/$pkg" "$STAGE/node_modules/$pkg"
	echo "    + ${pkg#./}"
done

echo "==> Building the plugin (Release)"
cmake --preset macos -DHELPER_NODE_MODULES="$STAGE/node_modules"
cmake --build --preset macos --config Release

echo "==> Packaging"
rm -rf "$OUT"
cmake --install build_macos --config Release --prefix "$OUT"
mv "$OUT/obs-discord-golive.pkg" "$OUT/$NAME.pkg"
(cd "$OUT" && ditto -c -k --keepParent obs-discord-golive.plugin "$NAME.zip")
rm -rf "$OUT/obs-discord-golive.plugin" "$OUT/obs-discord-golive.plugin.dSYM"

# Put the developer build back on the normal (dev) node_modules.
cmake --preset macos -DHELPER_NODE_MODULES="$ROOT/helper/node_modules" >/dev/null

echo "==> Done"
ls -lh "$OUT"
