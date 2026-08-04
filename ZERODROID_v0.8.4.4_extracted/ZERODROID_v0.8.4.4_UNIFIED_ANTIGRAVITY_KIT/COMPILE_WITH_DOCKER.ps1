$ErrorActionPreference = "Stop"
$Here = Split-Path -Parent $MyInvocation.MyCommand.Path
$Project = Join-Path $Here "ZERODROID_v0.8.4.4"
$SwitchDir = Join-Path $Project "switch"
$Output = Join-Path $SwitchDir "ZERODROID_v0.8.4.4.nro"
$Log = Join-Path $Here "build-v0.8.4.4.log"
$Report = Join-Path $Here "ANTIGRAVITY_BUILD_REPORT.md"
$Patch = Join-Path $Here "ANTIGRAVITY_CHANGES.patch"

if (-not (Get-Command docker -ErrorAction SilentlyContinue)) {
    throw "Docker Desktop no está instalado o no está disponible."
}

& (Join-Path $Here "VERIFY_KIT.ps1")
if (Test-Path $Output) { Remove-Item $Output -Force }
if (Test-Path "$Output.sha256") { Remove-Item "$Output.sha256" -Force }
if (Test-Path $Log) { Remove-Item $Log -Force }

$MountPath = $Project.Replace('\','/')
& docker run --rm `
    -v "${MountPath}:/workspace" `
    -w /workspace/switch `
    devkitpro/devkita64:latest `
    bash -lc 'set -euo pipefail; make clean || true; make -j"$(nproc)"' 2>&1 |
    Tee-Object -FilePath $Log

if ($LASTEXITCODE -ne 0) { throw "La compilación falló. Revisa build-v0.8.4.4.log" }
if (-not (Test-Path $Output)) { throw "No se generó ZERODROID_v0.8.4.4.nro" }

$Hash = (Get-FileHash $Output -Algorithm SHA256).Hash.ToLower()
"$Hash  ZERODROID_v0.8.4.4.nro" | Set-Content "$Output.sha256" -Encoding ascii
if (-not (Test-Path $Patch)) { New-Item $Patch -ItemType File | Out-Null }
@"
# Antigravity build report

- Target: ZERODROID v0.8.4.4
- Container: devkitpro/devkita64:latest
- Build result: SUCCESS
- Output: ZERODROID_v0.8.4.4/switch/ZERODROID_v0.8.4.4.nro
- SHA-256: $Hash
- Build log: build-v0.8.4.4.log
- Additional source changes: see ANTIGRAVITY_CHANGES.patch
- Hardware validation: NOT PERFORMED BY THIS SCRIPT
"@ | Set-Content $Report -Encoding utf8
Write-Host "OK: $Output"
Write-Host "SHA-256: $Hash"
