#!/usr/bin/env bash
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$HERE"

if [[ -f MANIFEST_SHA256.txt && "${ALLOW_MODIFIED_KIT:-0}" != "1" ]]; then
  sha256sum -c MANIFEST_SHA256.txt
elif [[ "${ALLOW_MODIFIED_KIT:-0}" == "1" ]]; then
  echo "ADVERTENCIA: se omite la comparación de hashes porque ALLOW_MODIFIED_KIT=1."
  echo "Las verificaciones estructurales siguen activas."
fi

ROOT="ZERODROID_v0.8.4.4"
test -f "$ROOT/switch/deps/switch/lib/libpeer.a"
test -f "$ROOT/switch/deps/switch/lib/libsrtp2.a"
test -f "$ROOT/switch/deps/switch/lib/libusrsctp.a"
test -f "$ROOT/switch/deps/switch/lib/libmbedtls.a"
test -f "$ROOT/switch/romfs/shaders/video_vsh.dksh"
test -f "$ROOT/switch/romfs/shaders/video_fsh.dksh"
test -f "$ROOT/switch/romfs/shaders/hud_fsh.dksh"

grep -q 'APP_VERSION := 0.8.4.4' "$ROOT/switch/Makefile"
grep -q 'TARGET   := ZERODROID_v0.8.4.4' "$ROOT/switch/Makefile"
grep -q 'Resolution1440p' "$ROOT/switch/src/switch/stream/quick_menu.hpp"
grep -q 'PresetCinema' "$ROOT/switch/src/switch/stream/quick_menu.hpp"
grep -q 'kSessionPanelRect' "$ROOT/switch/src/switch/stream/quick_menu.hpp"
grep -q 'kReconnectConfirmRect' "$ROOT/switch/src/switch/stream/quick_menu.hpp"
grep -q 'mouseModeEnabled' "$ROOT/switch/src/switch/stream/quick_menu.hpp"
grep -q 'ZERODROID CONTROL CENTER' "$ROOT/switch/src/switch/stream/dk_video_renderer.cpp"
grep -q 'UDP DROP / RECOVER' "$ROOT/switch/src/switch/stream/dk_video_renderer.cpp"
grep -q 'overlayWasOpenAtFrameStart' "$ROOT/switch/src/switch/main.cpp"
grep -q 'lastSentAxes_' "$ROOT/switch/src/switch/stream/engine.hpp"
grep -q 'browser envelope on control WSS' "$ROOT/switch/src/switch/stream/engine.cpp"
grep -q 'void Engine::send_alt_tab' "$ROOT/switch/src/switch/stream/engine.cpp"
grep -q 'void Engine::disconnect_for_reconnect' "$ROOT/switch/src/switch/stream/engine.cpp"
grep -q 'kNativeHoldMs = 350' "$ROOT/switch/src/switch/stream/engine.cpp"
grep -q 'native video recovered after' "$ROOT/switch/src/switch/stream/engine.cpp"

if grep -R --include='*.cpp' --include='*.hpp' -n 'APP_VERSION := 0.8.3' "$ROOT" >/dev/null; then
  echo "ERROR: se encontró una versión objetivo vieja en el código." >&2
  exit 1
fi

echo "Kit ZERODROID v0.8.4.4 verificado correctamente."
