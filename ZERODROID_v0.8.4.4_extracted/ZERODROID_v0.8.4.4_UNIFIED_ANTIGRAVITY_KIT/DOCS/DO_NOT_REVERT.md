# Elementos que Antigravity no debe revertir

Este documento funciona como contrato de conservación. Si una corrección de
compilación entra en conflicto con uno de estos puntos, detente y documenta el
bloqueo en vez de eliminar la función.

## Transporte y video

- Mantener ruta nativa UDP, WebSocket de control, H.264/NVDEC y deko3d.
- Mantener `kNativeHoldMs = 350` y la ventana de 24 grupos.
- Mantener recuperación suave, solicitud de refresh y probe temporizado.
- Mantener el último frame válido mientras se recupera.
- No alimentar unidades incompletas al decoder.
- No presentar frames que FFmpeg marque como corruptos.

## Resolución y calidad

- Auto: 720p portátil, 1080p dock.
- 720p y 1080p seleccionables.
- 1440p experimental, recibido y reescalado; nunca anunciarlo como output nativo.
- Presets y controles manuales persistentes.
- Render de arte `contain`, no `cover`.
- Cuatro columnas, títulos de dos líneas y fuentes compactas reales.

## Joystick

- Comparar contra `lastSentAxes_`, no contra la última lectura física.
- Deadzone radial.
- Rango externo escalado a ±32767.
- Refresh periódico de ejes sostenidos.
- Neutral y extremos enviados de forma inmediata.

## Mouse y teclado

- Touch y Minus+stick derecho.
- Minus+ZR/ZL clic izquierdo/arrastre.
- Minus+R3 clic derecho.
- Minus+X ALT+TAB.
- Envelope `id_cmd/from_udp` para desktop input por control WSS.
- No afirmar que el protocolo está oficialmente documentado.

## Sesión y overlay

- X lila abre el centro de control, no Guide directo.
- Guide se conserva como acción del panel.
- Reconnect requiere confirmación.
- Reconnect no debe emitir `terminating`, hangup ni stop/dequeue.
- Overlay abierto = mando neutral enviado al juego.
- B cierra el overlay.
- Teclado remoto queda informativo hasta una implementación probada.
