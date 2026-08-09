#!/usr/bin/env bash
# Rebuild only patched libpeer against the already-vendored Switch dependencies.
# This intentionally installs into this preview tree and touches no older release.
set -Eeuo pipefail

export DEVKITPRO=/opt/devkitpro
export DEVKITA64=/opt/devkitpro/devkitA64
export PORTLIBS=/opt/devkitpro/portlibs/switch
export LIBNX=/opt/devkitpro/libnx
export PATH="$PORTLIBS/bin:$DEVKITA64/bin:$DEVKITPRO/tools/bin:$PATH"

project="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
workspace="$(cd "$project/../../.." && pwd)"
src="${LIBPEER_SRC:-$workspace/green-nx-ZD-motion-flow-lite/deps/src/libpeer}"
prefix="$project/deps/switch"
shim="$project/deps/shim/include"
expected_commit='9319aa434cb9e893faed0293ba9d2a21eca59c8b'

if [[ ! -d "$src/.git" ]]; then
  echo "Patched libpeer source not found: $src" >&2
  echo "Set LIBPEER_SRC to its absolute MSYS path and retry." >&2
  exit 2
fi
if [[ "$(git -C "$src" rev-parse HEAD)" != "$expected_commit" ]]; then
  echo "libpeer base commit does not match the reviewed Switch patch." >&2
  exit 3
fi
if ! grep -q 'peer_connection_get_rtt_ms' "$src/src/peer_connection.c"; then
  echo "libpeer source is missing the consent-path RTT patch." >&2
  exit 4
fi

build="$(mktemp -d "$project/deps/.build-libpeer-rtt.XXXXXX")"
compiler_tmp="$(mktemp -d "$project/deps/.compiler-tmp-libpeer-rtt.XXXXXX")"
cleanup() {
  [[ "$build" == "$project/deps/.build-libpeer-rtt."* ]] && rm -rf -- "$build"
  [[ "$compiler_tmp" == "$project/deps/.compiler-tmp-libpeer-rtt."* ]] &&
    rm -rf -- "$compiler_tmp"
}
trap cleanup EXIT

export TMPDIR="$compiler_tmp"
export TMP="$compiler_tmp"
export TEMP="$compiler_tmp"

CFLAGS="-I\"$prefix/include\" -I\"$shim\" -include switch_missing.h -DLOG_REDIRECT=1 -DLOG_LEVEL=2" \
  aarch64-none-elf-cmake -S "$src" -B "$build" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="$prefix" \
    -DDISABLE_PEER_SIGNALING=ON \
    -DCMAKE_PREFIX_PATH="$prefix"

cmake --build "$build" -j2
cmake --install "$build"

if ! aarch64-none-elf-nm -g --defined-only "$prefix/lib/libpeer.a" |
     grep -q 'peer_connection_get_rtt_ms'; then
  echo "RTT getter missing from rebuilt libpeer.a" >&2
  exit 5
fi
echo "libpeer RTT rebuild verified: $prefix/lib/libpeer.a"
