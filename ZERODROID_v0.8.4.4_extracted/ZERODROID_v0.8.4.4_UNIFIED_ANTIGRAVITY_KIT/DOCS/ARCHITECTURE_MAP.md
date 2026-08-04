# Mapa de arquitectura

## Entrada principal y UI

- `switch/src/switch/main.cpp`
  - autenticación y bucle principal;
  - biblioteca/catálogo;
  - configuración persistente;
  - selección de resolución;
  - input Joy-Con/Pro Controller;
  - mouse táctil y modificador Minus;
  - apertura/cierre del centro de control;
  - confirmación y flujo de reconexión.

## API Boosteroid

- `switch/src/core/boosteroid_api.cpp/.hpp`
  - sesión de cuenta;
  - biblioteca y catálogo paginados;
  - preferencias de región;
  - creación y consulta de sesiones de streaming.

## Motor de streaming

- `switch/src/switch/stream/engine.cpp/.hpp`
  - WebSocket de control;
  - transporte nativo UDP;
  - agrupación, FEC, decrypt y acceso H.264;
  - audio;
  - joystick/mouse/teclado;
  - recuperación de video;
  - contadores expuestos al overlay;
  - desconexión local para reconnect.

## Decoder y presentación

- `video_decoder.*`: FFmpeg/NVDEC.
- `video_jitter.*`: jitter/reordenamiento WebRTC.
- `dk_video_renderer.*`: deko3d, escalado, color, presets, HUD y overlays.
- `quick_menu.hpp`: geometría compartida, estados, resoluciones y presets.

## Biblioteca visual

- `gfx.*`: SDL UI fuera del stream.
- `covers.*`: caché y descarga de arte.
- `main.cpp`: layout de tarjetas, texto y navegación.

## Regla de ownership del display

SDL posee la ventana durante biblioteca/login. Al comenzar el stream se suspende
SDL y deko3d toma la ventana. Al cerrar/reconectar se apaga deko3d antes de
reanudar SDL. No abrir Swkbd de forma ingenua mientras deko3d posee la ventana.
