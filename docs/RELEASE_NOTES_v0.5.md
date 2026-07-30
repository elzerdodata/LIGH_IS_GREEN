# Light is Green v0.5.0-pre

English | [Español](#español)

## Release summary

Version 0.5 makes selection feel responsive and keeps the in-stream controls
clear of the Nintendo Switch status clock. It also exposes useful catalog
information before and after opening a game.

### Changes

- Removed the bright green square tiles from the in-stream **••** and Xbox
  Guide controls.
- Moved both controls to the extreme top safe strip, above the status clock.
- Reduced the visible glyph size while preserving separate 72×72 design-space
  (48×48 physical-pixel) touch targets with an eight-pixel gap.
- Added an interruptible 220 ms spring animation to the selected library card.
- Added a persistent selected-game information rail to the library.
- Added best-effort developer, publisher, genre, and short-description metadata
  from the Microsoft Store display catalog.
- Expanded the game detail screen with the available catalog metadata.
- Updated application metadata, cache format, and CI artifact naming to v0.5.

### Upgrade notes

The game catalog cache is refreshed once after upgrading so the new metadata
can be populated. Account tokens, favorites, history, and settings remain
compatible, including the v0.4 Gamma control.

---

# Español

## Resumen de la versión

La versión 0.5 hace que la selección se sienta más fluida, mantiene los
controles durante la partida fuera del reloj de estado de Nintendo Switch y
muestra información útil antes y después de abrir un juego.

### Cambios

- Se eliminaron los recuadros cuadrados verdes de los controles **••** y Xbox
  Guide durante la partida.
- Ambos controles fueron movidos a la franja segura del extremo superior, por
  encima del reloj de estado.
- Se redujo el tamaño visual de los símbolos conservando áreas táctiles
  separadas de 72×72 en el espacio de diseño (48×48 píxeles físicos), con ocho
  píxeles de separación.
- Se agregó una animación elástica e interrumpible de 220 ms a la tarjeta
  seleccionada de la biblioteca.
- Se agregó una ficha permanente con información del juego seleccionado.
- Se añadieron, cuando están disponibles, desarrollador, editor, género y
  descripción breve desde el catálogo de Microsoft Store.
- La pantalla de detalles ahora muestra los metadatos disponibles.
- Se actualizaron metadatos de la aplicación, formato de caché y nombre del
  artefacto de CI a v0.5.

### Notas de actualización

La caché del catálogo se actualiza una vez después de instalar esta versión
para completar los nuevos metadatos. Las cuentas, favoritos, historial y
ajustes siguen siendo compatibles, incluido el control Gamma de v0.4.
