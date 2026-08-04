# Building ZERODROID v0.8.4.4

The source archive includes the prebuilt Switch WebRTC dependency bundle under
`switch/deps/switch/` and the generated deko3d shaders under
`switch/romfs/shaders/`.

## devkitPro / devkita64

```bash
cd switch
make clean
make -j4
```

The expected output is:

```text
switch/ZERODROID_v0.8.4.4.nro
```

## GitHub Actions

The included `.github/workflows/build-v0.8.4.4.yml` builds the same NRO inside the
official `devkitpro/devkita64` container and uploads it as a workflow artifact.
It can be started manually with **Actions → Build ZERODROID v0.8.4.4 → Run
workflow**.

## Runtime notes

- Auto resolution requests 1280×720 handheld and 1920×1080 docked.
- Explicit 720p and 1080p profiles work in either output mode.
- Experimental 1440p requests 2560×1440 from the service and renders it down to
  the Switch output framebuffer. Availability and sustained decode performance
  depend on the negotiated Boosteroid session.
- Resolution changes apply on the next game launch; picture presets apply during
  the current stream.
