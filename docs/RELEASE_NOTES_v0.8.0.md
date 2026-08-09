# Light is Green v0.8.0 — Stable / Estable

## English

This is the first stable release after the v0.7 preview series. It focuses on
a complete playable library, faster xCloud recovery, predictable memory use
and honest network diagnostics.

### Complete playable library

- Combines entitled Xbox Game Pass titles with eligible **Stream Your Own Game
  (BYOG)** and **free-to-play** cloud titles.
- Includes only entries for which Xbox reports an entitlement; this is not a
  mirror of the full Xbox store.
- Adds an **Owned & Free** library tab, deduplicates titles available through
  more than one catalog and preserves the correct Xbox offering for launch.
- Supports free-only accounts when Xbox grants an eligible F2P or owned cloud
  entitlement, even when Game Pass catalog authentication is unavailable.

Availability is controlled by Xbox and can vary by account, subscription and
region. Owning a game does not automatically mean Xbox has enabled it for BYOG
cloud streaming.

### Faster and more reliable connections

- Corrects libpeer's DTLS receive behavior: an idle UDP receive window is a
  recoverable timeout rather than a false connection EOF.
- Retires a silent xCloud DTLS route after roughly 6 seconds in practice,
  instead of spending roughly 12 seconds on an endpoint that never responds.
- Surfaces terminal DTLS protocol, certificate and fingerprint failures
  immediately.
- Starts the next cloud allocation as soon as the previous session teardown
  completes; the redundant fixed 3-second cloud delay is gone.
- Keeps Remote Play's 25-second DTLS allowance and progressive xHome backoff,
  which are needed for slower consoles and Internet NAT/Teredo routes.

Xbox capacity queues remain server-side and cannot be bypassed by the client.

### Regions, quality and Remote Play

- Keeps **Region bypass** separate from **Server region** selection.
- Loads the live Xbox datacenter list, including Chile Central and Brazil South
  when Xbox returns them, with Auto and optional strict server forcing.
- Limits the bypass address header to the relevant Xbox service hosts instead
  of leaking it into OAuth, Store or artwork requests.
- Includes configurable bitrate and 720p/1080p quality profiles.
- Keeps Remote Play over Xbox's xHome service available outside the local
  network when the console, account, NAT/Teredo and UDP route permit it.

### Switch stability and diagnostics

- Caps resident cover textures and pending/completed artwork jobs to prevent an
  ever-growing library from exhausting Switch memory.
- Recovers from video allocation pressure by clearing stale compressed data,
  resetting jitter state and requesting a fresh H.264 IDR frame.
- Replaces the old audio queue-depth `BUF` value with true live **PING**: STUN
  round-trip time measured on the active WebRTC media route.
- Retains Steady, Smooth and Motion pacing. **Motion remains experimental**;
  use Smooth if it produces green flashing or visual instability.

Do not publish `stream-log.txt` without reviewing it first: diagnostics may
contain network address candidates.

## Español

Esta es la primera versión estable después de la serie preliminar v0.7. Se
centra en una biblioteca jugable completa, recuperación más rápida de xCloud,
uso predecible de memoria y métricas de red honestas.

### Biblioteca jugable completa

- Combina los títulos habilitados de Xbox Game Pass con los juegos compatibles
  de **Stream Your Own Game (BYOG)** y **free-to-play** en la nube.
- Incluye únicamente entradas para las que Xbox informa acceso; no es una copia
  de toda la tienda Xbox.
- Agrega la pestaña **Propios y gratis**, combina los títulos repetidos entre
  catálogos y conserva la oferta Xbox correcta al iniciar cada juego.
- Admite cuentas sin Game Pass cuando Xbox concede acceso a un título propio o
  gratuito compatible, aunque la autenticación del catálogo Game Pass no esté
  disponible.

La disponibilidad la controla Xbox y puede variar según cuenta, suscripción y
región. Ser propietario de un juego no significa automáticamente que Xbox lo
haya habilitado para BYOG en la nube.

### Conexiones más rápidas y confiables

- Corrige la recepción DTLS de libpeer: una ventana UDP sin datos ahora produce
  un timeout recuperable y no un falso cierre de conexión.
- Descarta una ruta DTLS silenciosa de xCloud después de unos 6 segundos en la
  práctica, en lugar de perder unos 12 segundos con un endpoint que no responde.
- Informa inmediatamente los errores terminales de protocolo DTLS, certificado
  o huella.
- Inicia la siguiente asignación cloud apenas termina el cierre de la sesión
  anterior; se elimina la espera fija redundante de 3 segundos.
- Conserva 25 segundos para DTLS y la espera progresiva de xHome en Remote Play,
  necesarios para consolas lentas y rutas NAT/Teredo por Internet.

Las colas de capacidad de Xbox pertenecen al servidor y el cliente no puede
saltarlas.

### Regiones, calidad y Remote Play

- Mantiene **Bypass de región** separado de la selección de **Región del
  servidor**.
- Carga la lista real de datacenters de Xbox, incluidos Chile Central y Brazil
  South cuando Xbox los devuelve, con Auto y forzado estricto opcional.
- Envía la cabecera del bypass solamente a los servicios Xbox pertinentes, no
  a OAuth, Store ni servidores de carátulas.
- Incluye bitrate configurable y perfiles de calidad 720p/1080p.
- Mantiene Remote Play fuera de la red local mediante xHome cuando la consola,
  cuenta, NAT/Teredo y ruta UDP lo permiten.

### Estabilidad de Switch y diagnóstico

- Limita las texturas residentes de carátulas y los trabajos de imágenes
  pendientes/completados para evitar que una biblioteca creciente agote la
  memoria de Switch.
- Se recupera de presión de memoria de video eliminando datos comprimidos
  antiguos, reiniciando el estado de jitter y solicitando una imagen H.264 IDR.
- Reemplaza `BUF`, que medía la cola de audio, por **PING** real en vivo: tiempo
  de ida y vuelta STUN medido sobre la ruta multimedia WebRTC activa.
- Conserva los modos Steady, Smooth y Motion. **Motion sigue siendo
  experimental**; usa Smooth si produce destellos verdes o inestabilidad visual.

No publiques `stream-log.txt` sin revisarlo primero: el diagnóstico puede
contener candidatos de direcciones de red.

## Credits / Créditos

These historical v0.x notes are retained for release continuity. Current
licensing and distribution information is provided in `LICENSE` and `NOTICE`.
