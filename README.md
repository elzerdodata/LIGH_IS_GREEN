# Light is Green

> **Preliminary v0.5.0-pre / Versión preliminar v0.5.0-pre**

English | [Español](#español)

Light is Green is a community fork of
[green-nx](https://github.com/rmrf404/green-nx), originally created by
**rmrf404** and distributed under GPL-3.0.

It is a standalone, open-source **Xbox Cloud Gaming (xCloud) client for
Nintendo Switch** homebrew. Authentication, WebRTC, hardware H.264 decoding,
GPU rendering, audio, and controller input all run on the console; no companion
PC is required.

## What's new in v0.5

- Frameless, symbol-only **••** and Xbox Guide controls in the extreme top safe
  strip. Their visible glyphs are smaller while their full 48×48 physical-pixel
  touch targets remain easy to tap.
- The controls no longer use bright green square outlines and are positioned
  above the Switch status clock instead of covering it.
- The selected game now grows with a short 220 ms spring animation instead of
  snapping instantly between states.
- A persistent selection rail shows the game's title, developer/publisher,
  genre, cloud source, stream quality, and favorite state when available.
- The game detail view now includes Microsoft Store metadata and a short
  description. Existing v0.4 Gamma and live image controls remain available.

## Features

- Microsoft **device-code sign-in**; no password is typed on the console.
- Game library with cover art, favorites, history, search, and console remote
  play.
- Live xCloud queue estimate while a server is allocated.
- Native WebRTC streaming with hardware H.264 decoding and zero-copy deko3d
  rendering.
- Opus audio, 60 fps video, and 720p / 1080p / 1080p high-bitrate modes.
- Configurable game language, region bypass, button mapping, vibration, video
  pacing, volume, and image controls.

## Requirements

- A Nintendo Switch running
  [Atmosphère](https://github.com/Atmosphere-NX/Atmosphere) custom firmware.
- An active Xbox Game Pass plan that includes Cloud Gaming.
- A 5 GHz Wi-Fi connection or docked Ethernet is recommended.

## Installation

1. Copy `Light_is_Green.nro` to `sdmc:/switch/` on the SD card.
2. Launch it in **title mode** by holding **R** while opening an installed game.
   Applet mode does not provide enough memory for hardware video decoding.
3. Enter the device code shown by the app at `microsoft.com/link`.
   Tokens and cached data are stored in `sdmc:/switch/green-nx/`.

### Controls

| Context | Controls |
| --- | --- |
| Library | Left stick / d-pad: move · **A**: open/play · **Y**: search · **X**: refresh · **ZL**: settings · **+**: exit |
| Settings | Left/right: change · **A**: open/confirm · **B**: back |
| In stream | Switch pad: Xbox controls · tap **••**: image/stats panel · tap the small **Xbox symbol** or press **L3 + R3**: Guide · hold **-** + **+**: quit |

## Build

Build inside the `devkitpro/devkita64` Docker image:

```sh
# Build WebRTC dependencies once
bash deps/build-switch.sh

# Build the application
docker run --rm -v "$PWD":/src -w /src devkitpro/devkita64 make
```

The output is `Light_is_Green.nro`. The desktop core-development harness can
be built with `make -f Makefile.pc`.

### Source layout

```text
src/core/            authentication, catalog, HTTP, xCloud protocol
src/switch/          SDL2 interface, covers, and input
src/switch/stream/   WebRTC, RTP, NVDEC, deko3d, and Opus streaming engine
shaders/             deko3d video shaders
deps/                Switch dependency build scripts and patches
romfs/ui/            interface artwork
```

## Third-party software

| Project | License | Purpose |
| --- | --- | --- |
| [libpeer](https://github.com/sepfy/libpeer) (patched) | MIT | WebRTC, ICE, DTLS-SRTP, SCTP |
| [libsrtp](https://github.com/cisco/libsrtp) | BSD-3 | SRTP encryption |
| [usrsctp](https://github.com/sctplab/usrsctp) | BSD-3 | SCTP data channels |
| [mbedTLS](https://github.com/Mbed-TLS/mbedtls) | Apache-2.0 | TLS and DTLS cryptography |
| [FFmpeg](https://ffmpeg.org) with [NVTEGRA](https://github.com/averne/FFmpeg) | LGPL-2.1+ | Hardware H.264 decoding |
| [deko3d](https://github.com/devkitPro/deko3d) | zlib | GPU rendering |
| [SDL2](https://libsdl.org), SDL2_ttf, SDL2_image | zlib | Interface, input, and images |
| [Opus](https://opus-codec.org) | BSD-3 | Audio decoding |
| [libcurl](https://curl.se) | curl | HTTPS |
| [nlohmann/json](https://github.com/nlohmann/json) | MIT | JSON |
| [devkitPro / libnx](https://devkitpro.org) | various | Switch toolchain and OS APIs |

## Disclaimer

Light is Green is an experimental, non-commercial hobby project provided as-is
for personal use. It is not affiliated with, endorsed by, or supported by
Microsoft or Nintendo. Xbox, Xbox Cloud Gaming, and Game Pass are Microsoft
trademarks. Nintendo Switch is a Nintendo trademark. You must provide your own
valid subscription and hardware.

## License and credits

- License: [GPL-3.0](LICENSE)
- Original project and core implementation: **rmrf404** —
  [green-nx](https://github.com/rmrf404/green-nx)
- Light is Green fork and v0.2–v0.5 work: **elzerdodata**

---

# Español

Light is Green es un fork comunitario de
[green-nx](https://github.com/rmrf404/green-nx), creado originalmente por
**rmrf404** y distribuido bajo la licencia GPL-3.0.

Es un cliente homebrew independiente y de código abierto de **Xbox Cloud
Gaming (xCloud) para Nintendo Switch**. La autenticación, WebRTC, decodificación
H.264 por hardware, renderizado en GPU, audio y controles funcionan en la
consola; no requiere una PC auxiliar.

## Novedades de v0.5

- Controles **••** y Xbox Guide sin recuadros, ubicados en la franja segura del
  extremo superior. Los símbolos visibles son más pequeños, pero conservan
  áreas táctiles físicas completas de 48×48 píxeles.
- Se eliminaron los bordes cuadrados verdes y los controles quedan por encima
  del reloj de estado de Switch, sin taparlo.
- El juego seleccionado ahora crece con una breve animación elástica de 220 ms
  en lugar de cambiar de tamaño instantáneamente.
- Una ficha de selección permanente muestra título, desarrollador/editor,
  género, fuente en la nube, calidad y estado de favorito cuando están
  disponibles.
- La vista de detalles ahora incluye metadatos de Microsoft Store y una
  descripción breve. Gamma y los controles de imagen de v0.4 siguen incluidos.

## Funciones

- Inicio de sesión de Microsoft mediante **código de dispositivo**; no se
  escribe la contraseña en la consola.
- Biblioteca con carátulas, favoritos, historial, búsqueda y juego remoto desde
  una consola Xbox.
- Estimación en vivo de la cola de xCloud mientras se asigna un servidor.
- Streaming WebRTC nativo con decodificación H.264 por hardware y renderizado
  deko3d sin copias adicionales.
- Audio Opus, video a 60 fps y modos 720p / 1080p / 1080p de alta tasa.
- Idioma del juego, salto de región, distribución de botones, vibración,
  suavizado de video, volumen y controles de imagen configurables.

## Requisitos

- Nintendo Switch con el firmware personalizado
  [Atmosphère](https://github.com/Atmosphere-NX/Atmosphere).
- Plan activo de Xbox Game Pass que incluya Cloud Gaming.
- Se recomienda Wi-Fi de 5 GHz o Ethernet mediante el dock.

## Instalación

1. Copia `Light_is_Green.nro` a `sdmc:/switch/` en la tarjeta SD.
2. Inícialo en **modo título** manteniendo **R** al abrir un juego instalado.
   El modo applet no ofrece suficiente memoria para decodificar video por
   hardware.
3. Introduce el código mostrado por la app en `microsoft.com/link`.
   Los tokens y datos en caché se guardan en `sdmc:/switch/green-nx/`.

### Controles

| Contexto | Controles |
| --- | --- |
| Biblioteca | Stick izquierdo / cruceta: mover · **A**: abrir/jugar · **Y**: buscar · **X**: actualizar · **ZL**: ajustes · **+**: salir |
| Ajustes | Izquierda/derecha: cambiar · **A**: abrir/confirmar · **B**: volver |
| En partida | Controles de Switch: controles Xbox · tocar **••**: panel de imagen/datos · tocar el **símbolo pequeño de Xbox** o pulsar **L3 + R3**: Guide · mantener **-** + **+**: salir |

## Compilación

Compila dentro de la imagen Docker `devkitpro/devkita64`:

```sh
# Compilar una vez las dependencias WebRTC
bash deps/build-switch.sh

# Compilar la aplicación
docker run --rm -v "$PWD":/src -w /src devkitpro/devkita64 make
```

El resultado es `Light_is_Green.nro`. El entorno de desarrollo del núcleo para
PC se compila con `make -f Makefile.pc`.

### Estructura del código

```text
src/core/            autenticación, catálogo, HTTP y protocolo xCloud
src/switch/          interfaz SDL2, carátulas y controles
src/switch/stream/   WebRTC, RTP, NVDEC, deko3d y motor de audio Opus
shaders/             shaders de video para deko3d
deps/                scripts y parches de dependencias para Switch
romfs/ui/            arte de interfaz
```

## Software de terceros

| Proyecto | Licencia | Uso |
| --- | --- | --- |
| [libpeer](https://github.com/sepfy/libpeer) (con parches) | MIT | WebRTC, ICE, DTLS-SRTP, SCTP |
| [libsrtp](https://github.com/cisco/libsrtp) | BSD-3 | Cifrado SRTP |
| [usrsctp](https://github.com/sctplab/usrsctp) | BSD-3 | Canales de datos SCTP |
| [mbedTLS](https://github.com/Mbed-TLS/mbedtls) | Apache-2.0 | Criptografía TLS y DTLS |
| [FFmpeg](https://ffmpeg.org) con [NVTEGRA](https://github.com/averne/FFmpeg) | LGPL-2.1+ | Decodificación H.264 por hardware |
| [deko3d](https://github.com/devkitPro/deko3d) | zlib | Renderizado en GPU |
| [SDL2](https://libsdl.org), SDL2_ttf, SDL2_image | zlib | Interfaz, controles e imágenes |
| [Opus](https://opus-codec.org) | BSD-3 | Decodificación de audio |
| [libcurl](https://curl.se) | curl | HTTPS |
| [nlohmann/json](https://github.com/nlohmann/json) | MIT | JSON |
| [devkitPro / libnx](https://devkitpro.org) | varias | Toolchain y APIs del sistema Switch |

## Aviso

Light is Green es un proyecto experimental, no comercial y de afición,
entregado tal cual para uso personal. No está afiliado, respaldado ni soportado
por Microsoft o Nintendo. Xbox, Xbox Cloud Gaming y Game Pass son marcas de
Microsoft. Nintendo Switch es una marca de Nintendo. Debes proporcionar tu
propia suscripción válida y tu hardware.

## Licencia y créditos

- Licencia: [GPL-3.0](LICENSE)
- Proyecto original e implementación base: **rmrf404** —
  [green-nx](https://github.com/rmrf404/green-nx)
- Fork Light is Green y trabajo de v0.2–v0.5: **elzerdodata**
