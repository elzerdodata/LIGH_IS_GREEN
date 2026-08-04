# Riesgos conocidos y límites

## Mouse

El usuario reportó que el mouse de la primera implementación no funcionaba. La
base posterior cambió el envelope de eventos, pero todavía requiere prueba con
Boosteroid real. Los contadores del centro de control demuestran que ZERODROID
intentó transmitir; no demuestran que el servidor aceptó el evento.

Diagnóstico:

- contador no aumenta: fallo de captura local;
- contador aumenta y no hay cursor/acción remota: formato/canal no aceptado;
- movimiento funciona y clic no: revisar `btn/isPressed`;
- touch funciona pero stick no: revisar modificador Minus/deadzone;
- stick funciona pero touch no: revisar coordenadas normalizadas/captura overlay.

## Reconexión

El código evita terminar explícitamente la VM y vuelve a iniciar el mismo appId.
El servicio puede decidir adjuntar a la sesión existente o crear/consultar otra.
No describirlo como garantía. La confirmación es obligatoria.

## 1440p

Puede aumentar ancho de banda, paquetes UDP, carga de decode y memoria. Probar
primero 720p y 1080p. No usar 1440p para diagnosticar estabilidad.

## Keyboard Info

El tile existe para documentar que el teclado es una necesidad pendiente. No
abre Swkbd durante deko3d ni inyecta texto. ALT+TAB sí usa eventos de teclado.

## Overlay

La textura del centro de control reutiliza la textura existente de 672x928 para
limitar memoria. En portátil se reescala junto con el espacio lógico 1920x1080;
verificar legibilidad y hitboxes en hardware.
