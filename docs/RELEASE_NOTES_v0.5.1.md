# Light is Green v0.5.1

English | [Español](#español)

## Release summary

Version 0.5.1 is a corrective release. It removes the nullable catalog
metadata introduced in v0.5.0-pre, restores the proven v0.4 loading path, and
uses the available screen space for a content-first game library.

### Fixes and changes

- Fixed `json.exception.type_error.302: type must be string, but is null` while
  loading the library.
- Restored the v0.4 game-cache format and catalog parser.
- The app once again reads only the existing game name and cover-art fields.
- Added a compact branded header, search surface, filter pills, and an 8×3
  library grid showing up to 24 games per page.
- Removed the empty selected-game hero and the non-functional **In-stream**
  header tab.
- Added the supplied Joy-Con/Xbox artwork as the NRO icon and in-app brand icon.
- Preserved the animated selected tile and compact in-stream controls.

### Upgrade notes

This build can reuse the same v0.4 `games.json` cache. No account, favorite,
history, language, Gamma, or image-control setting needs to be reset.

---

# Español

## Resumen de la versión

La versión 0.5.1 es una versión correctiva. Elimina los metadatos anulables
introducidos en v0.5.0-pre, restaura la carga comprobada de v0.4 y acerca la
biblioteca nativa de Switch a un diseño centrado en los juegos.

### Correcciones y cambios

- Se corrigió `json.exception.type_error.302: type must be string, but is null`
  al cargar la biblioteca.
- Se restauraron el formato de caché y el analizador de catálogo de v0.4.
- La aplicación vuelve a leer únicamente el nombre y la carátula existentes.
- Se agregaron cabecera compacta, buscador, filtros y una cuadrícula 8×3 que
  muestra hasta 24 juegos por página.
- Se eliminaron el hero vacío del juego seleccionado y la pestaña
  **In-stream** del encabezado, que no tenía función.
- Se agregó el arte Joy-Con/Xbox suministrado como icono del NRO y de la app.
- Se conservaron la animación de selección y los controles compactos durante
  la partida.

### Notas de actualización

Este build puede reutilizar el mismo caché `games.json` de v0.4. No es necesario
restablecer cuentas, favoritos, historial, idioma, Gamma ni controles de imagen.
