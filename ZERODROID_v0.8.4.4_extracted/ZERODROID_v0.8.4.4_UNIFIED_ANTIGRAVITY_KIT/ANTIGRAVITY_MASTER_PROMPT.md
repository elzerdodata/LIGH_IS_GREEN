# INSTRUCCIÓN MAESTRA PARA ANTIGRAVITY

Trabaja exclusivamente con el proyecto completo ubicado en:

```text
ZERODROID_v0.8.4.4/
```

Tu misión principal es **compilar, corregir únicamente incompatibilidades reales
de compilación/enlace y entregar un NRO verificable**. No rediseñes el proyecto,
no sustituyas subsistemas y no reviertas las funciones consolidadas.

## Entregables obligatorios

Debes producir y devolver exactamente:

```text
ZERODROID_v0.8.4.4/switch/ZERODROID_v0.8.4.4.nro
ZERODROID_v0.8.4.4/switch/ZERODROID_v0.8.4.4.nro.sha256
build-v0.8.4.4.log
ANTIGRAVITY_CHANGES.patch
ANTIGRAVITY_BUILD_REPORT.md
```

`ANTIGRAVITY_CHANGES.patch` debe estar vacío o documentar toda modificación que
hayas hecho. No entregues un NRO anterior renombrado.

## Regla de oro

Antes de editar cualquier archivo, lee:

- `DOCS/DO_NOT_REVERT.md`
- `DOCS/BUILD_EXACT.md`
- `DOCS/ARCHITECTURE_MAP.md`
- `DOCS/KNOWN_RISKS.md`

Si el código compila sin cambios, **no modifiques nada**.

## Base funcional que debe conservarse completa

### v0.8.3

1. Paginación completa de biblioteca y catálogo, sin límite fijo de 50 juegos.
2. Protección contra páginas duplicadas y bucles de paginación.
3. Secciones Mis juegos, Favoritos, Recientes, Catálogo e Instalar.
4. Acción oficial de Boosteroid para añadir juegos a Mis juegos.
5. Estados de tarjetas: ya instalado, instalar, gratuito y requiere licencia.
6. Ordenamiento de grupos UDP y descarte de unidades H.264 incompletas/corruptas.

### v0.8.4

1. Resoluciones Auto, 720p, 1080p y 1440p experimental.
2. Auto = 720p portátil y 1080p dock.
3. Presets Natural, Nítido/Sharp, Vivo, Cine, Suave y Personalizado.
4. Brillo, contraste, saturación, gamma y nitidez persistentes.
5. Cuadrícula de cuatro columnas, títulos en dos líneas y fuentes compactas.
6. Prioridad de banner horizontal y render `contain`, sin cortar el arte.

### Hotfixes acumulados

1. Ventana UDP nativa 350 ms y 24 grupos.
2. Recuperación suave antes de la espera IDR estricta.
3. Probe de recuperación y prevención de congelamiento permanente.
4. Comparación del joystick contra el último eje realmente transmitido.
5. Deadzone radial, expansión del rango externo y refresh cada 120 ms.
6. Mouse táctil y Minus + stick derecho.
7. Minus + ZR/ZL = clic izquierdo/arrastre; Minus + R3 = clic derecho.
8. Mouse/teclado con `id_cmd` y `from_udp` en el WebSocket de control.
9. Minus + X = ALT+TAB.
10. Reconexión local experimental sin enviar `terminating`, hangup ni dequeue.

### v0.8.4.4 Control Center

1. La X lila táctil superior derecha abre el centro de control.
2. El centro muestra juego, gateway, resoluciones, ping, tiempo de sesión,
   grupos UDP descartados/recuperados, recuperaciones y contadores de input.
3. Acciones táctiles: Xbox Guide, ALT+TAB, Mouse ON/OFF, Graphics, Reconnect y
   Return to Game.
4. Reconnect requiere confirmación explícita.
5. B cierra el overlay.
6. Plus+Minus es acceso alternativo; la X lila es el acceso principal.
7. Con un overlay abierto, el mando enviado al juego debe quedar neutral.
8. `KEYBOARD INFO` es informativo; no implementar teclado remoto sin un diseño
   probado de suspensión de deko3d y traducción de texto a eventos remotos.

## Procedimiento de compilación exacto

1. Ejecuta desde la raíz del kit:

```bash
bash VERIFY_KIT.sh
bash COMPILE_WITH_DOCKER.sh
```

2. El contenedor obligatorio es:

```text
devkitpro/devkita64:latest
```

3. El comando interno esperado es:

```bash
cd /workspace/switch
make clean || true
make -j"$(nproc)"
```

4. No reconstruyas WebRTC/libpeer salvo que una biblioteca esté realmente
   ausente o incompatible. Ya se incluyen:

```text
switch/deps/switch/lib/libpeer.a
switch/deps/switch/lib/libsrtp2.a
switch/deps/switch/lib/libusrsctp.a
switch/deps/switch/lib/libmbedtls.a
switch/deps/switch/lib/libmbedcrypto.a
switch/deps/switch/lib/libmbedx509.a
```

5. No reemplaces FFmpeg/NVDEC/deko3d con SDL video, OpenGL ni software decoding.

## Qué cambios sí están permitidos

Solo si el compilador lo exige:

- incluir un header estándar faltante;
- corregir una firma por cambio real de libnx/devkitA64;
- corregir un cast inequívoco;
- corregir nombre/ruta de una biblioteca realmente distinta;
- corregir un warning tratado como error únicamente cuando bloquea el build;
- ajustar el workflow o script de build sin cambiar el comportamiento runtime.

Cada cambio debe aparecer en `ANTIGRAVITY_CHANGES.patch` y explicarse en
`ANTIGRAVITY_BUILD_REPORT.md`.

## Qué cambios están prohibidos

- Eliminar 1080p o convertir Auto en 720p fijo.
- Hacer 1440p obligatorio o llamarlo salida nativa de Switch.
- Volver a cinco columnas o al recorte `cover` de carátulas.
- Volver a comparar el joystick contra la muestra física anterior.
- Eliminar el refresh de ejes no neutrales.
- Volver al gate IDR permanente de v0.8.4.
- Reducir la ventana UDP a 140 ms/8 grupos.
- Enviar mouse/teclado como simple paquete Android TV sin envelope.
- Enviar `terminating`, hangup o dequeue durante Reconnect.
- Hacer que la X lila envíe Guide directamente sin abrir el centro de control.
- Permitir que el juego reciba sticks/botones detrás de un overlay abierto.
- Inventar una API oficial de Boosteroid que no existe en el código.
- Descargar dependencias aleatorias o actualizar versiones sin necesidad.
- Reutilizar `ZERODROID_v0.8.3.nro` o cualquier NRO previo.

## Verificaciones obligatorias antes de entregar

Ejecuta:

```bash
bash VERIFY_KIT.sh
cd ZERODROID_v0.8.4.4/switch
make clean
make -j"$(nproc)"
test -f ZERODROID_v0.8.4.4.nro
sha256sum ZERODROID_v0.8.4.4.nro
```

Confirma en el reporte:

- versión NACP 0.8.4.4;
- nombre real del output;
- tamaño del NRO;
- SHA-256;
- toolchain usado;
- resultado de `VERIFY_KIT.sh`;
- lista exacta de archivos modificados;
- si la compilación fue limpia o necesitó correcciones.

## Validación estática mínima de cadenas

Deben seguir presentes:

```text
APP_VERSION := 0.8.4.4
Resolution1440p
PresetCinema
kNativeHoldMs = 350
native video recovered after
lastSentAxes_
MouseNormal
send_mouse_position
browser envelope on control WSS
send_alt_tab
disconnect_for_reconnect
ZERODROID CONTROL CENTER
kReconnectConfirmRect
mouseModeEnabled
overlayWasOpenAtFrameStart
```

## Prueba de hardware solicitada, si tienes acceso a una Switch

Sigue `DOCS/HARDWARE_TEST_PLAN.md`. No declares que mouse o reconexión funcionan
solo porque el código compiló. Compilar y validar contra Boosteroid son etapas
diferentes.
