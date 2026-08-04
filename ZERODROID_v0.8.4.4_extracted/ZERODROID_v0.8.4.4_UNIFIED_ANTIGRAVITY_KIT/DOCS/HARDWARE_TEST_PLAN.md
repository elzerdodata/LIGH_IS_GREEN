# Plan de prueba en Nintendo Switch

Guardar después de cada sesión:

```text
sdmc:/switch/ZERODROID/stream.log
sdmc:/switch/ZERODROID/stream-prev.log
sdmc:/switch/ZERODROID/zerodroid.log
```

## Fase 1 — Smoke test

1. Arrancar por title takeover, no Album/applet mode.
2. Abrir biblioteca y verificar las cinco secciones.
3. Confirmar cuatro columnas, títulos legibles y arte sin recorte.
4. Abrir un juego en 720p Natural durante 10 minutos.
5. Repetir en 1080p Natural durante 15 minutos.

## Fase 2 — Recuperación de video

1. Repetir la zona de Final Fantasy VII que congeló la imagen.
2. Confirmar que no aparece un freeze permanente.
3. Abrir X lila y observar DROP/RECOVER y RECOVERY EVENTS.
4. Verificar logs por `native video recovered after`.

## Fase 3 — Joystick

1. Empujar stick izquierdo lentamente hasta el extremo.
2. Confirmar que el personaje pasa de caminar a correr.
3. Repetir con empuje brusco.
4. Mantener extremo 30 segundos y confirmar que no cae a un valor parcial.

## Fase 4 — Centro de control

1. Tocar X lila.
2. Confirmar que el juego no se mueve detrás del panel.
3. Confirmar métricas y botones.
4. Cerrar con B.
5. Abrir con Plus+Minus y cerrar.
6. Abrir Graphics y volver.

## Fase 5 — Mouse

1. Activar MOUSE: ON.
2. Tocar y arrastrar la pantalla.
3. Mantener Minus y mover stick derecho.
4. Minus+ZR: clic izquierdo.
5. Minus+R3: clic derecho.
6. Verificar que los contadores TX aumenten.
7. Registrar si el servidor responde o solo aumenta el contador local.

## Fase 6 — ALT+TAB

1. Usar Minus+X.
2. Usar botón ALT+TAB del overlay.
3. Confirmar secuencia remota y revisar contador keyboard.

## Fase 7 — Reconnect

1. Solo después de completar las fases anteriores.
2. Tocar Reconnect y confirmar que aparece la segunda pantalla.
3. Cancelar una vez.
4. Reabrir y confirmar.
5. Observar si vuelve al mismo escritorio/juego.
6. Verificar que no se haya terminado la máquina.
7. Guardar ambos logs.

## Criterio de aprobación

No aprobar por compilación. Aprobar por separado:

- build válido;
- biblioteca/UI;
- estabilidad de video;
- joystick;
- mouse;
- ALT+TAB;
- reconnect.
