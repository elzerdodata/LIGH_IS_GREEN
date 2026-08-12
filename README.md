# Light is Green

<p align="center">
  <img src="icon.jpg" width="180" alt="Light is Green Icon" />
</p>

> **Official Release v1.0.6 / Versión Oficial v1.0.6**

[English](#english) · [Español](#español)

---

## Showcase / Galería de Capturas

![v1.0.6 Clean 5x2 Grid](docs/showcase_v1.0.6.png)

<p floating="left">
  <img src="docs/showcase_1.jpg" width="48%" />
  <img src="docs/showcase_2.jpg" width="48%" />
  <img src="docs/showcase_3.jpg" width="48%" />
  <img src="docs/showcase_4.jpg" width="48%" />
</p>

---

## English

Light is Green is an independently maintained, open-source Cloud Play and Remote Play client for Nintendo Switch homebrew. Authentication, catalogue, native WebRTC streaming, hardware H.264 decoding, GPU presentation, audio, and controller input run natively on the console—no companion PC required.

### Key Features (v1.0.6)

- **Clean 5 × 2 Library Grid**: Redesigned library layout displaying larger, high-resolution square Xbox BoxArt covers without text clutter under cards. Full game titles are displayed in the top status panel when focused.
- **Pure Edge-to-Edge Artwork**: Removed dark padding and ambient borders around covers for a sleek, modern, edge-to-edge aesthetic.
- **Real-Time Picture & Color Controls**: Fine-tuned Quick Menu ("••") picture controls (Contrast, Saturation, Gamma, Brightness, and Temperature) with responsive step sizes for real-time video adjustments while streaming.
- **Native WebRTC & Zero-Copy Rendering**: High-performance streaming pipeline utilizing `libpeer`, hardware H.264 video decoding via NVDEC, and `deko3d` GPU presentation.
- **Flexible Stream Quality**: Supports 720p, 1080p, 1080p HQ Windows, and experimental HQ Tizen profiles with customizable bitrates and pacing.
- **Full Input & Guide Support**: Native Switch controller mapping, vibration/rumble feedback, and Xbox Guide menu access (**L3 + R3** or touch overlay).
- **Diagnostics & Statistics**: In-stream HUD overlay showing live video bitrate, frame rate, packet loss, and WebRTC ping latency.
- **Custom Theme & Icon**: Featuring custom app icon and Aurora background artwork created by **@Djihads80**.

### Catalogue Scope

The library intentionally displays cloud-playable games returned for the signed-in Microsoft account: Game Pass titles, eligible Stream Your Own Game / BYOG titles, and free-to-play offerings. Duplicate entitlements are unified and launched through the appropriate granting license.

### Controls Reference

| Context | Controls |
| --- | --- |
| **Library** | Left Stick / D-Pad: Move focus · **A**: Details / Launch · **Y**: Search · **X**: Favorite · **ZR**: Refresh · **ZL**: Settings · **+**: Exit |
| **Settings** | Left / Right: Adjust value · **A**: Open / Confirm · **B**: Back |
| **In Stream** | Switch Controls: Xbox Controller mapped · **Touch ••**: Picture & Performance Quick Menu · **Touch Xbox Logo** or **L3 + R3**: Xbox Guide · Hold **− + +**: Exit Stream |

### Requirements and Installation

1. Use a Nintendo Switch console with [Atmosphère](https://github.com/Atmosphere-NX/Atmosphere) custom firmware. A 5 GHz Wi-Fi network or docked Ethernet connection is strongly recommended.
2. Copy `Light_is_Green-v1.0.6.nro` to `sdmc:/switch/`.
3. Launch the app in **Title Mode** by holding **R** while launching any installed game. *(Applet mode lacks sufficient RAM for hardware video decoding)*.
4. Enter the displayed device code at [microsoft.com/link](https://microsoft.com/link). Account tokens and cached data are stored safely in `sdmc:/switch/light-is-green/`.

### Build Instructions

Build inside the `devkitpro/devkita64` Docker container or MSYS2 environment:

```sh
# Build WebRTC dependencies
bash deps/build-switch.sh

# Build the Switch application
docker run --rm -v "$PWD":/src -w /src devkitpro/devkita64 make
```

The resulting binary is `Light_is_Green-v1.0.6.nro`.

### Source Code Architecture

```text
src/core/            Authentication, catalogue, HTTP engine, and xCloud protocol
src/switch/          SDL2 user interface, cover art manager, and input handling
src/switch/stream/   WebRTC pipeline, RTP, NVDEC video decoder, deko3d renderer, and Opus audio
shaders/             deko3d GLSL shaders (compiled to .dksh)
deps/                Dependency build scripts and Switch compatibility shims
romfs/ui/            User interface assets, icons, and theme textures
```

### Third-Party Software & Libraries

| Library / Component | License | Purpose |
| --- | --- | --- |
| [libpeer](https://github.com/sepfy/libpeer) (patched) | MIT | WebRTC, ICE, DTLS-SRTP, SCTP data channels |
| [libsrtp](https://github.com/cisco/libsrtp) | BSD-3-Clause | SRTP media encryption |
| [usrsctp](https://github.com/sctplab/usrsctp) | BSD-3-Clause | SCTP protocol for data channels |
| [mbedTLS](https://github.com/Mbed-TLS/mbedtls) | Apache-2.0 | TLS & DTLS cryptography |
| [FFmpeg](https://ffmpeg.org) / [NVTEGRA](https://github.com/averne/FFmpeg) | LGPL-2.1+ | Hardware H.264 video decoding |
| [deko3d](https://github.com/devkitPro/deko3d) | zlib | Low-level GPU video rendering |
| [SDL2](https://libsdl.org) / SDL2_ttf / SDL2_image | zlib | UI rendering, font handling, and input |
| [Opus](https://opus-codec.org) | BSD-3-Clause | High-fidelity audio decoding |
| [libcurl](https://curl.se) | curl | HTTPS networking |
| [nlohmann/json](https://github.com/nlohmann/json) | MIT | JSON parsing |
| [devkitPro / libnx](https://devkitpro.org) | Various | Switch toolchain & system APIs |

---

## Español

Light is Green es un cliente de Cloud Play y Remote Play para homebrew de Nintendo Switch, mantenido de forma independiente y de código abierto. La autenticación, catálogo, streaming WebRTC nativo, decodificación H.264 por hardware, presentación en GPU, audio y controles funcionan directamente en la consola: no requiere PC auxiliar.

### Novedades y Funciones Destacadas (v1.0.6)

- **Nueva Grilla Limpia 5 × 2**: Biblioteca rediseñada con carátulas cuadradas nativas de Xbox en alta resolución, más grandes y sin nombres debajo de las tarjetas. El título completo aparece en el panel superior al seleccionar cada juego.
- **Arte Puro sin Bordes**: Se eliminaron los bordes oscuros y el relleno ambiental alrededor de las carátulas para una experiencia visual limpia y de aspecto premium.
- **Controles de Imagen al Instante**: Menú rápido ("••") optimizado con saltos ágiles para Contraste, Saturación, Gamma, Brillo y Temperatura con vista previa en tiempo real durante el juego.
- **Streaming WebRTC Nativo**: Decodificación H.264 por hardware vía NVDEC y presentación GPU mediante `deko3d` sin copias adicionales de memoria.
- **Calidad de Video Configurable**: Soporte para 720p, 1080p, 1080p HQ Windows y perfil experimental HQ Tizen, con control de bitrate y suavizado (pacing).
- **Controles y Menú Guide**: Mapeo completo de controles de Switch, vibración e integración del menú Xbox Guide (**L3 + R3** o botón táctil).
- **HUD de Diagnóstico**: Diagnóstico en vivo sobre el stream con información de bitrate, cuadros por segundo (FPS), pérdida de paquetes y ping WebRTC.
- **Icono y Tema Visual**: Incluye el nuevo icono oficial de la app y el fondo arte creados por **@Djihads80**.

### Alcance del Catálogo

La biblioteca muestra intencionalmente sólo juegos cloud jugables que Xbox devuelve para la cuenta iniciada: títulos de Game Pass, títulos elegibles de Stream Your Own Game / BYOG y juegos free-to-play. Las licencias duplicadas se unifican y cada juego se abre con la oferta correspondiente.

### Tabla de Controles

| Contexto | Controles |
| --- | --- |
| **Biblioteca** | Stick izquierdo / Cruceta: Mover foco · **A**: Detalles / Jugar · **Y**: Buscar · **X**: Favorito · **ZR**: Actualizar · **ZL**: Ajustes · **+**: Salir |
| **Ajustes** | Izquierda / Derecha: Cambiar valor · **A**: Abrir / Confirmar · **B**: Volver |
| **En Stream** | Controles Switch: Mapeados a Xbox · **Tocar ••**: Menú rápido de imagen y rendimiento · **Tocar símbolo Xbox** o **L3 + R3**: Menú Guide · Mantener **− + +**: Salir del stream |

### Requisitos e Instalación

1. Usar una consola Nintendo Switch con firmware personalizado [Atmosphère](https://github.com/Atmosphere-NX/Atmosphere). Se recomienda red Wi-Fi de 5 GHz o Ethernet en dock.
2. Copiar `Light_is_Green-v1.0.6.nro` a `sdmc:/switch/`.
3. Abrir en **Modo Título** manteniendo **R** al iniciar cualquier juego instalado. *(El modo applet no dispone de suficiente memoria RAM para decodificación de video por hardware)*.
4. Ingresar el código mostrado en [microsoft.com/link](https://microsoft.com/link). Los tokens y datos se guardan en `sdmc:/switch/light-is-green/`.

### Instrucciones de Compilación

Compila en el contenedor Docker `devkitpro/devkita64` o en entorno MSYS2:

```sh
# Compilar dependencias WebRTC
bash deps/build-switch.sh

# Compilar la aplicación para Switch
docker run --rm -v "$PWD":/src -w /src devkitpro/devkita64 make
```

El resultado es `Light_is_Green-v1.0.6.nro`.

---

## Authors & Collaborators / Autores y Colaboradores

- **elzerdodata** — Main Developer, Project Maintainer & Lead Designer (v0.2 – v1.0.6).
- **rmrf404** — Original Project Creator & Base Architecture ([green-nx](https://github.com/rmrf404/green-nx)).
- **@Djihads80** — Visual Theme Designer, Official App Icon & Aurora Background Art.

---

## Disclaimer / Aviso Legal

Light is Green is an open-source, non-commercial community project for personal use. It is not affiliated with, endorsed by, or supported by Microsoft Corporation or Nintendo Co., Ltd. Xbox, Xbox Cloud Gaming, Game Pass, and Nintendo Switch are trademarks of their respective owners. Users must provide their own valid account and subscriptions.

---

## License / Licencia

Distributed under the [GNU General Public License v3.0](LICENSE). See [NOTICE](NOTICE) for third-party license disclosures.
