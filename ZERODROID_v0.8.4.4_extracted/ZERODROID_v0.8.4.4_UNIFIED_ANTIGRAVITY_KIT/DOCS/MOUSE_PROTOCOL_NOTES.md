# Notas de diagnóstico del mouse

Funciones relevantes:

```text
Engine::send_input_json
Engine::send_mouse_position
Engine::send_mouse_button
Engine::send_keyboard_button
Engine::send_alt_tab
```

La implementación transmite desktop input por el WebSocket de control con:

```text
id_cmd
from_udp=false
```

En modo WebRTC también conserva el duplicado por data channel con
`from_udp=true`. En `/native`, el WebSocket es el camino autoritativo.

El centro de control muestra:

- movimientos de mouse intentados;
- clics presionados;
- eventos de teclado.

Estos contadores son diagnóstico local, no ACK del servidor.
