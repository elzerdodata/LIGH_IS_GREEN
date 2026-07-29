# Light is Green v0.6.0

English | [Español](#español)

## Release summary

Version 0.6.0 focuses on xCloud allocation and queue control. Region bypass
and server selection are now independent settings, and the normal 1080p
high-bitrate mode no longer identifies the Switch as a Samsung/Tizen TV.

### New server controls

- **Region bypass** remains available for unsupported countries: Off, United
  States, Brazil, Japan, Korea, and Poland.
- **Server region** is a separate setting with Auto, Chile Central, Brazil
  South, and every additional datacenter returned by Xbox during login.
- The datacenter list is refreshed from Xbox and cached for the next launch.
- A selected server is matched by Xbox's stable region name. If it is no longer
  offered, the app safely falls back to Xbox's default server.
- The stream log now records the selected region and GSSV host to make queue
  and routing problems easier to diagnose.

### Stream profiles

- **1080p HQ · Windows** is now the default high-bitrate profile. It requests
  1080p at up to 30 Mbps while using the more compatible Windows allocation
  pool.
- **1080p HQ · Tizen test** preserves the former TV fingerprint as an explicit
  experimental option for side-by-side testing.
- Existing 720p/Android and 1080p/Windows profiles remain available.

### Upgrade notes

Existing accounts, favorites, history, catalog cache, image controls, and
language settings are preserved. Users who previously selected high bitrate
will automatically use the new Windows HQ profile. Start with **Server region:
Auto**; try **Chile Central** or **Brazil South** when the automatic region has
an unusually long queue.

---

# Español

## Resumen de la versión

La versión 0.6.0 se concentra en la asignación de servidores y las colas de
xCloud. El bypass geográfico y la selección del datacenter ahora son ajustes
independientes, y el modo normal de 1080p con alta tasa ya no identifica a la
Switch como un televisor Samsung/Tizen.

### Nuevos controles de servidor

- **Región de bypass** continúa disponible para países no compatibles: Off,
  Estados Unidos, Brasil, Japón, Corea y Polonia.
- **Región del servidor** es un ajuste separado con Auto, Chile Central,
  Brazil South y todos los datacenters adicionales que Xbox devuelva al iniciar
  sesión.
- La lista de datacenters se actualiza desde Xbox y queda guardada para el
  siguiente inicio.
- El servidor se selecciona mediante el nombre estable de región de Xbox. Si
  deja de estar disponible, la app vuelve de forma segura al servidor
  predeterminado de Xbox.
- El registro de streaming ahora muestra la región seleccionada y el host GSSV
  para facilitar el diagnóstico de colas y rutas.

### Perfiles de streaming

- **1080p HQ · Windows** es ahora el perfil predeterminado de alta tasa. Pide
  1080p y hasta 30 Mbps utilizando el pool de asignación Windows, que es más
  compatible.
- **1080p HQ · Tizen test** conserva el identificador anterior de TV como una
  opción experimental explícita para poder compararlos.
- Los perfiles 720p/Android y 1080p/Windows continúan disponibles.

### Notas de actualización

Se conservan cuentas, favoritos, historial, caché del catálogo, controles de
imagen e idioma. Quienes ya tenían seleccionada la alta tasa pasarán
automáticamente al nuevo perfil HQ Windows. Conviene comenzar con **Región del
servidor: Auto** y probar **Chile Central** o **Brazil South** cuando la región
automática tenga una cola inusualmente larga.
