# Light is Green v0.8.3 — Release Notes / Notas de la Versión

## [ES] Español

### 🌐 Corrección de Remote Play WAN (Fuera de Casa)

Esta versión v0.8.3 resuelve la conexión en **Xbox Remote Play desde redes externas a la LAN** (redes Wi-Fi secundarias o hotspot móvil).

#### 🛠️ Cambios Principales

1. **Recolección y Aceptación de Candidatos ICE WAN (`engine.cpp`):**
   - Eliminado el filtro que rechazaba candidatos IPv4 públicos si su prioridad ICE era $\le 1000$.
   - Aceptación sintáctica válida de todos los candidatos para evaluación de alcanzabilidad en libpeer.
   - Eliminado el reordenamiento estricto por prioridad en C++ para preservar el orden de señalización.

2. **Deduplicación y Clasificación en libpeer:**
   - Clasificación por alcance real (`agent_remote_rank`): la IP pública alcanzable se intenta antes que la IP privada de otra red.
   - Deduplicación por endpoint (`IP + puerto`) y presupuesto optimizado.

3. **Mejoras Visuales de la Biblioteca (incorporadas desde v0.8.1):**
   - Escalado de portadas en relación de aspecto `contain` 16:9 sin recortes ni deformaciones.
   - Cuadrícula 6×3 (18 juegos por página) con títulos ajustados en 2 líneas.
   - Marquesina animada para títulos largos seleccionados.

---

## [EN] English

### 🌐 WAN Remote Play Connection Fix (Outside Home Network)

This v0.8.3 release resolves **Xbox Remote Play connectivity from networks outside the home LAN** (such as secondary Wi-Fi access points or mobile data hotspots).

#### 🛠️ Key Improvements

1. **ICE Candidate Gathering & Validation (`engine.cpp`):**
   - Removed arbitrary priority threshold filtering (`candidate_priority > 1000`) that dropped valid public IPv4 endpoints.
   - Validated candidate syntax and passed all candidates to libpeer for reachability ranking.
   - Removed forced C++ priority sorting to preserve signaling order.

2. **libpeer Deduplication & Ranking:**
   - Reachability ranking (`agent_remote_rank`): routable public IPv4 endpoints are tested before unroutable private LAN addresses.
   - Endpoint deduplication (`IP + port`) and per-pair budget limit.

3. **Library UI Readability (Carried from v0.8.1):**
   - 16:9 `contain` aspect ratio cover scaling (no cropping or stretching).
   - 6×3 grid (18 games per page) with 2-line measured title wrapping.
   - Animated marquee header for selected long game titles.
