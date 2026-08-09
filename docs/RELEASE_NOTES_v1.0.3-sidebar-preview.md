# Light is Green v1.0.3 Sidebar Preview

## Español

Esta compilación experimental mantiene la identidad visual verde y el concept
art de Light is Green, pero reorganiza la navegación principal en una barra
lateral persistente pensada para mando y pantalla táctil.

- Biblioteca compacta de 6 x 3: 18 juegos visibles, tarjetas *full-bleed* y
  separación uniforme sin letterboxing negro.
- Navegación lateral para Todos, Propios y gratis, Favoritos, Historial,
  Consolas, Búsqueda y Ajustes.
- Foco visible, objetivos táctiles de 44 px físicos y geometría compartida entre
  render, mando y toque.
- Pantallas de carga y Ajustes integradas en la misma estructura lateral.
- Los textos de conexión, ruta y ayuda quedan recortados dentro del panel tanto
  en xCloud como en Remote Play.
- Remote Play consulta de forma opcional la ruta oficial `/configuration` y
  aprovecha los endpoints y servidores STUN publicados por Xbox antes de usar
  los valores anteriores como respaldo.

La mejora WAN amplía las rutas ICE disponibles, pero no promete atravesar todos
los NAT/CGNAT. Esta versión no simula TURN: el motor actual todavía necesita una
ruta UDP alcanzable y debe validarse en una Nintendo Switch fuera de la red
local.

## English

This experimental build keeps Light is Green's emerald identity and concept
art while moving primary navigation into a persistent controller- and
touch-friendly sidebar.

- Compact 6 x 3 library: 18 visible games, full-bleed artwork, and even spacing
  without black letterboxing.
- Sidebar access to All Games, Owned & Free, Favorites, History, Consoles,
  Search, and Settings.
- Visible focus, 44 px physical touch targets, and shared render/input geometry.
- Loading and Settings screens use the same persistent shell.
- Connection status, route, and help copy stay inside the launch panel for both
  xCloud and Remote Play.
- Remote Play can best-effort fetch Xbox's official `/configuration` data and
  consume the advertised endpoints and STUN servers before falling back to the
  previous route.

The WAN change increases the number of useful ICE routes but cannot guarantee
connectivity through every NAT/CGNAT. This build does not pretend to provide
TURN: the current engine still needs a reachable UDP path and requires an
off-LAN Nintendo Switch test.
