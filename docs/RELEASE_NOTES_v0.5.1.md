# Light is Green v0.5.1-pre

English | [Español](#español)

## Release summary

Version 0.5.1 is a corrective prerelease. It removes the nullable catalog
metadata introduced in v0.5.0-pre, restores the proven v0.4 loading path, and
brings the native Switch library much closer to the approved cinematic design.

### Fixes and changes

- Fixed `json.exception.type_error.302: type must be string, but is null` while
  loading the library.
- Restored the v0.4 game-cache format and catalog parser.
- The app once again reads only the existing game name and cover-art fields.
- Added a compact branded header, search surface, filter pills, a large
  selected-game hero, and an eight-game carousel.
- The hero uses existing local data only: title, xCloud source, quality, and
  favorite state.
- Added the supplied Joy-Con/Xbox artwork as the NRO icon and in-app brand icon.
- Preserved the animated selected tile and compact in-stream controls.

### Upgrade notes

This build can reuse the same v0.4 `games.json` cache. No account, favorite,
history, language, Gamma, or image-control setting needs to be reset.

---

# Español

## Resumen de la versión

La versión 0.5.1 es una prerelease correctiva. Elimina los metadatos anulables
introducidos en v0.5.0-pre, restaura la carga comprobada de v0.4 y acerca la
biblioteca nativa de Switch al diseño cinematográfico aprobado.

### Correcciones y cambios

- Se corrigió `json.exception.type_error.302: type must be string, but is null`
  al cargar la biblioteca.
- Se restauraron el formato de caché y el analizador de catálogo de v0.4.
- La aplicación vuelve a leer únicamente el nombre y la carátula existentes.
- Se agregaron cabecera compacta, buscador, filtros, un hero grande para el
  juego seleccionado y un carrusel de ocho juegos.
- El hero utiliza solo datos locales ya existentes: título, fuente xCloud,
  calidad y estado de favorito.
- Se agregó el arte Joy-Con/Xbox suministrado como icono del NRO y de la app.
- Se conservaron la animación de selección y los controles compactos durante
  la partida.

### Notas de actualización

Este build puede reutilizar el mismo caché `games.json` de v0.4. No es necesario
restablecer cuentas, favoritos, historial, idioma, Gamma ni controles de imagen.
