# Light is Green

> **Stable v0.6.0 · Preview v0.7.0 / Estable v0.6.0 · Preliminar v0.7.0**

English | [Español](#español)

Light is Green is a community fork of
[green-nx](https://github.com/rmrf404/green-nx), originally created by
**rmrf404** and distributed under GPL-3.0.

It is a standalone, open-source **Xbox Cloud Gaming (xCloud) client for
Nintendo Switch** homebrew. Authentication, WebRTC, hardware H.264 decoding,
GPU rendering, audio, and controller input all run on the console; no companion
PC is required.

## v0.7.0 preview

- Fixes the thin green/magenta corruption stripe at the bottom/right video
  edge by sampling the visible luma and chroma texel centres independently,
  never NVDEC's uninitialized alignment padding.
- Adds live **Pacing: Steady / Smooth / Motion** controls to the in-stream
  two-dot touch panel. Changing mode releases old queued/interpolation surfaces
  without restarting WebRTC.
- **Motion is 100% experimental and may cause rapid green flashing.** It now
  rejects unproven or incompatible secondary decoder surfaces and uses a normal
  Smooth refresh instead, but users should immediately switch back if flashing
  is observed. Motion is never restored automatically after restarting the app.
- Shows the selected server region throughout the xCloud sign-in and connection
  screen, updating to Xbox's actual region as soon as login completes.
- Adds an experimental **1080p Console quality** option for Remote Play. If
  negotiation or first video fails, the app starts a fresh session
  automatically with the stable 720p profile.
- Beta 2 separates console wake/registration retries from WebRTC media retries,
  adds progressive teardown backoff, refreshes the xHome route periodically,
  gives remote ICE/DTLS negotiation more time, and fixes compressed Teredo
  candidate conversion for Remote Play outside the LAN.
- Keeps Remote Play available away from home through Xbox's xHome service.
  Success depends on Remote Features, NAT, IPv6/Teredo and UDP connectivity.
- Expands the performance overlay with source, output and generated FPS.

The v0.7 preview is published separately. **v0.6.0 remains the recommended stable
release.** Do not publish `stream-log.txt`: connection diagnostics can include
IP candidates.

## What's new in v0.6.0

- Separates **Region bypass** from the new **Server region** selector.
- Adds Auto, Chile Central, Brazil South, and every additional xCloud
  datacenter returned by Xbox during login.
- Refreshes and caches the live datacenter list, with a safe fallback to the
  Xbox default when a selected region is unavailable.
- Changes the normal 1080p high-bitrate preset to the Windows allocation pool
  while preserving the former Tizen/TV fingerprint as an experimental option.
- Records the selected region and host in `stream-log.txt` for queue diagnosis.

## Features

- Microsoft **device-code sign-in**; no password is typed on the console.
- Game library with cover art, favorites, history, search, and console remote
  play.
- Live xCloud queue estimate while a server is allocated.
- Native WebRTC streaming with hardware H.264 decoding and zero-copy deko3d
  rendering.
- Opus audio, 60 fps video, and 720p / 1080p / 1080p HQ Windows modes, plus an
  experimental Tizen HQ profile.
- Independent region bypass and xCloud server selection, configurable game
  language, button mapping, vibration, video pacing, volume, and image controls.

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
| Library | Left stick / d-pad: move · **A**: open/play · **Y**: search · **X**: favorite · **ZR**: refresh · **ZL**: settings · **+**: exit |
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
- Light is Green fork and v0.2–v0.7 work: **elzerdodata**

---

# Español

Light is Green es un fork comunitario de
[green-nx](https://github.com/rmrf404/green-nx), creado originalmente por
**rmrf404** y distribuido bajo la licencia GPL-3.0.

Es un cliente homebrew independiente y de código abierto de **Xbox Cloud
Gaming (xCloud) para Nintendo Switch**. La autenticación, WebRTC, decodificación
H.264 por hardware, renderizado en GPU, audio y controles funcionan en la
consola; no requiere una PC auxiliar.

## Avance de v0.7.0

- Corrige la franja fina verde/magenta del borde inferior/derecho del video:
  luma y croma ahora se muestrean desde sus texeles visibles y nunca desde el
  relleno de alineación sin inicializar de NVDEC.
- Agrega **Pacing: Steady / Smooth / Motion** al panel táctil de los dos puntos
  dentro del juego. El cambio se aplica en caliente, libera las superficies del
  modo anterior y no reinicia WebRTC.
- **Motion es 100% experimental y podría causar pantalla verde parpadeante.**
  Ahora rechaza superficies secundarias no comprobadas o incompatibles y usa
  un refresco Smooth normal, pero se debe volver inmediatamente a Smooth o
  Steady si aparece parpadeo. Motion nunca se restaura automáticamente al
  reiniciar la aplicación.
- Muestra la región del servidor durante el inicio de sesión y la conexión a
  xCloud, y cambia a la región real elegida por Xbox al terminar el login.
- Agrega **Calidad de consola 1080p** experimental para Remote Play. Si falla
  la negociación o no llega el primer video, la aplicación abre una sesión
  nueva con el perfil estable de 720p.
- Beta 2 separa los reintentos de encendido/registro de los reintentos WebRTC,
  agrega espera progresiva entre sesiones, renueva periódicamente la ruta
  xHome, concede más tiempo a la negociación ICE/DTLS remota y corrige la
  conversión de candidatos Teredo comprimidos para jugar fuera de la red local.
- Mantiene Remote Play fuera de casa mediante el servicio xHome de Xbox. Su
  funcionamiento depende de Remote Features, NAT, IPv6/Teredo y conectividad
  UDP.
- Amplía el overlay de rendimiento con FPS de origen, salida y generados.

La versión preliminar v0.7 se publica por separado. **v0.6.0 sigue siendo la
versión estable recomendada.** No publiques `stream-log.txt`: el diagnóstico puede incluir
candidatos de direcciones IP.

## Novedades de v0.6.0

- Separa **Región de bypass** del nuevo selector **Región del servidor**.
- Agrega Auto, Chile Central, Brazil South y todos los datacenters adicionales
  de xCloud que Xbox devuelva al iniciar sesión.
- Actualiza y guarda la lista real de datacenters, con retorno seguro al
  servidor predeterminado de Xbox cuando una región no esté disponible.
- Cambia el modo normal de 1080p con alta tasa al pool Windows y conserva el
  perfil anterior Tizen/TV como opción experimental.
- Registra la región y el host seleccionados en `stream-log.txt` para facilitar
  el diagnóstico de colas.

## Funciones

- Inicio de sesión de Microsoft mediante **código de dispositivo**; no se
  escribe la contraseña en la consola.
- Biblioteca con carátulas, favoritos, historial, búsqueda y juego remoto desde
  una consola Xbox.
- Estimación en vivo de la cola de xCloud mientras se asigna un servidor.
- Streaming WebRTC nativo con decodificación H.264 por hardware y renderizado
  deko3d sin copias adicionales.
- Audio Opus, video a 60 fps y modos 720p / 1080p / 1080p HQ Windows, además de
  un perfil HQ Tizen experimental.
- Bypass geográfico y servidor de xCloud independientes, además de idioma,
  distribución de botones, vibración, suavizado, volumen y controles de imagen.

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
| Biblioteca | Stick izquierdo / cruceta: mover · **A**: abrir/jugar · **Y**: buscar · **X**: favorito · **ZR**: actualizar · **ZL**: ajustes · **+**: salir |
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
- Fork Light is Green y trabajo de v0.2–v0.7: **elzerdodata**
