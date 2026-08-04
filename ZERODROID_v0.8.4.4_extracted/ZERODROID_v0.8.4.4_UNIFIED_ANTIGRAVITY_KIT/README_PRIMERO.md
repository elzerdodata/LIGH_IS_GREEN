# ZERODROID v0.8.4.4 — Kit unificado para Antigravity

Este paquete contiene **una sola base de código completa y consolidada**. No es
una colección de parches que deban aplicarse en orden. La carpeta que debe
compilarse es:

```text
ZERODROID_v0.8.4.4/
```

Incluye todas las funciones de:

- v0.8.3: catálogo completo, Mis juegos/Favoritos/Recientes/Catálogo/Instalar,
  paginación protegida y mejoras de recuperación H.264.
- v0.8.4: 720p/1080p/1440p experimental, perfiles gráficos, cuadrícula de
  cuatro columnas, fuentes compactas, títulos en dos líneas y arte sin recorte.
- v0.8.4.1: hotfix para el congelamiento `waiting for a clean IDR`.
- v0.8.4.2: corrección de joystick analógico y mouse virtual.
- v0.8.4.3: envelope de mouse/teclado, ALT+TAB y reconexión local experimental.
- v0.8.4.4: centro de control grande al tocar la X lila, métricas de sesión,
  confirmación de reconexión y captura de entradas mientras el overlay está abierto.

## Acción recomendada para Antigravity

Entrega el ZIP completo y pega literalmente el contenido de
`ANTIGRAVITY_MASTER_PROMPT.md` como instrucción principal.

## Compilación rápida

Linux/macOS con Docker:

```bash
bash COMPILE_WITH_DOCKER.sh
```

Windows con Docker Desktop:

```powershell
.\COMPILE_WITH_DOCKER.ps1
```

Resultado esperado:

```text
ZERODROID_v0.8.4.4/switch/ZERODROID_v0.8.4.4.nro
ZERODROID_v0.8.4.4/switch/ZERODROID_v0.8.4.4.nro.sha256
build-v0.8.4.4.log
```

## Orden de lectura obligatorio

1. `ANTIGRAVITY_MASTER_PROMPT.md`
2. `DOCS/DO_NOT_REVERT.md`
3. `DOCS/BUILD_EXACT.md`
4. `DOCS/ARCHITECTURE_MAP.md`
5. `DOCS/FEATURE_MATRIX.md`
6. `DOCS/KNOWN_RISKS.md`
7. `DOCS/HARDWARE_TEST_PLAN.md`
8. `DOCS/DELIVERABLE_CHECKLIST.md`

## Advertencias honestas

- El código fue consolidado y revisado estáticamente, pero este entorno no
  dispone de devkitA64 para generar el NRO final.
- El mouse y la reconexión dependen de comportamiento no documentado del
  servicio Boosteroid y deben validarse en una Switch real.
- El botón `KEYBOARD INFO` del nuevo overlay es informativo. No debe convertirse
  en inyección de texto improvisada durante la compilación.
- 1440p es experimental: la Switch lo decodifica y reduce a la resolución de
  salida; no es una salida nativa 1440p de la consola.
