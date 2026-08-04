$ErrorActionPreference = "Stop"
$Here = Split-Path -Parent $MyInvocation.MyCommand.Path
$Root = Join-Path $Here "ZERODROID_v0.8.4.4"
$Required = @(
  "switch/deps/switch/lib/libpeer.a",
  "switch/deps/switch/lib/libsrtp2.a",
  "switch/deps/switch/lib/libusrsctp.a",
  "switch/deps/switch/lib/libmbedtls.a",
  "switch/romfs/shaders/video_vsh.dksh",
  "switch/romfs/shaders/video_fsh.dksh",
  "switch/romfs/shaders/hud_fsh.dksh"
)
foreach ($Relative in $Required) {
  $Path = Join-Path $Root $Relative
  if (-not (Test-Path $Path)) { throw "Falta archivo obligatorio: $Relative" }
}
$Makefile = Get-Content (Join-Path $Root "switch/Makefile") -Raw
if ($Makefile -notmatch 'APP_VERSION := 0\.8\.4\.4') { throw "Versión incorrecta" }
$Engine = Get-Content (Join-Path $Root "switch/src/switch/stream/engine.cpp") -Raw
if ($Engine -notmatch 'browser envelope on control WSS') { throw "Falta desktop input envelope" }
if ($Engine -notmatch 'kNativeHoldMs = 350') { throw "Falta ventana UDP hotfix" }
$Renderer = Get-Content (Join-Path $Root "switch/src/switch/stream/dk_video_renderer.cpp") -Raw
if ($Renderer -notmatch 'ZERODROID CONTROL CENTER') { throw "Falta control center" }
Write-Host "Kit ZERODROID v0.8.4.4 verificado correctamente."
