# Light is Green v0.4.0-pre

English | [Español](#español)

## Release summary

Version 0.4 modernizes the interface and makes the in-stream image controls
more complete without changing the existing sign-in, library, or game-language
flows.

### Changes

- New dark charcoal and emerald visual system.
- New original 16:9 background inspired by playful mushroom landscapes and a
  futuristic ringworld, without using game characters or copied artwork.
- Replaced the oversized Xbox `HOME` tile with a compact symbol-only button.
- Placed the Xbox button eight design pixels from the **••** quick-menu button.
- Added Gamma to Settings and the in-stream image panel.
- Gamma range: `0.50–2.00`; default: `1.00`; step: `0.05`.
- Gamma is applied live in the deko3d fragment shader and is included in
  **Reset image**.
- Updated application metadata and CI artifact naming to v0.4.

### Upgrade notes

Existing settings remain compatible. If `gamma` is absent from an older
`settings.json`, Light is Green uses the neutral value `1.00`.

---

# Español

## Resumen de la versión

La versión 0.4 moderniza la interfaz y completa los controles de imagen durante
la partida sin cambiar los flujos existentes de inicio de sesión, biblioteca o
idioma del juego.

### Cambios

- Nuevo sistema visual en carbón oscuro y verde esmeralda.
- Nuevo fondo original 16:9 inspirado en paisajes de hongos y un mundo-anillo
  futurista, sin utilizar personajes ni arte copiado de videojuegos.
- El recuadro Xbox `HOME` grande fue reemplazado por un botón compacto que solo
  muestra el símbolo.
- El botón Xbox queda a ocho píxeles de diseño del botón rápido **••**.
- Gamma fue agregado a Ajustes y al panel de imagen durante la partida.
- Rango de Gamma: `0.50–2.00`; valor inicial: `1.00`; incremento: `0.05`.
- Gamma se aplica en vivo mediante el fragment shader de deko3d y forma parte
  de **Restablecer imagen**.
- Metadatos de la aplicación y nombre del artefacto de CI actualizados a v0.4.

### Notas de actualización

Los ajustes existentes siguen siendo compatibles. Si un `settings.json`
anterior no contiene `gamma`, Light is Green utiliza el valor neutro `1.00`.
