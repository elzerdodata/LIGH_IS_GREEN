#!/usr/bin/env bash
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT="$HERE/ZERODROID_v0.8.4.4"
LOG="$HERE/build-v0.8.4.4.log"
OUT="$PROJECT/switch/ZERODROID_v0.8.4.4.nro"
PATCH="$HERE/ANTIGRAVITY_CHANGES.patch"
REPORT="$HERE/ANTIGRAVITY_BUILD_REPORT.md"

if ! command -v docker >/dev/null 2>&1; then
  echo "ERROR: Docker no está instalado o no está disponible." >&2
  echo "Alternativa: usa .github/workflows/build-v0.8.4.4.yml." >&2
  exit 1
fi

bash "$HERE/VERIFY_KIT.sh"
rm -f "$LOG" "$OUT" "$OUT.sha256"

{
  echo "ZERODROID v0.8.4.4 clean build"
  echo "UTC: $(date -u +%Y-%m-%dT%H:%M:%SZ)"
  echo "Image: devkitpro/devkita64:latest"
} | tee "$LOG"

docker run --rm \
  -v "$PROJECT:/workspace" \
  -w /workspace/switch \
  devkitpro/devkita64:latest \
  bash -lc 'set -euo pipefail; make clean || true; make -j"$(nproc)"' \
  2>&1 | tee -a "$LOG"

test -f "$OUT"
sha256sum "$OUT" | tee "$OUT.sha256"

if [[ ! -f "$PATCH" ]]; then
  : > "$PATCH"
fi

cat > "$REPORT" <<REPORT
# Antigravity build report

- Target: ZERODROID v0.8.4.4
- Container: devkitpro/devkita64:latest
- Build result: SUCCESS
- Output: ZERODROID_v0.8.4.4/switch/ZERODROID_v0.8.4.4.nro
- SHA-256: $(cut -d' ' -f1 "$OUT.sha256")
- Build log: build-v0.8.4.4.log
- Additional source changes: see ANTIGRAVITY_CHANGES.patch
- Hardware validation: NOT PERFORMED BY THIS SCRIPT
REPORT

echo
echo "OK: $OUT"
echo "SHA-256: $OUT.sha256"
echo "Log: $LOG"
echo "Report: $REPORT"
