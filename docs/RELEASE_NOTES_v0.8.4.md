# Light is Green v0.8.4 — Reconstructed from Working v0.8.2 / Reconstruido desde v0.8.2 Funcional

## [ES] Español

### 🌐 Corrección de Remote Play WAN (Fuera de Casa)

Esta versión **v0.8.4** fue construida exclusivamente desde la base **v0.8.2 funcional** que preserva la acción directa del botón A sobre `Your Xbox`.

#### ✨ Cambios Incluidos

1. **Recolección y Aceptación de Candidatos ICE WAN (`engine.cpp`):**
   * Eliminado el filtro restrictivo de prioridad (`candidate_priority > 1000`) que descartaba candidatos IPv4 públicos válidos.
   * Aceptación sintáctica válida de todos los candidatos para evaluación de alcanzabilidad en `libpeer`.

2. **Jerarquía y Deduplicación en libpeer:**
   * Clasificación por alcance real (`agent_remote_rank`): la IP pública alcanzable se intenta antes que la IP privada de otra red.
   * Deduplicación por endpoint (`IP + puerto`) y presupuesto por par optimizado a ~3 segundos con 3 vueltas completas.

3. **Interfaz Intacta v0.8.2:**
   * `src/switch/main.cpp` y todos los componentes protegidos permanecen 100% idénticos byte por byte a la versión v0.8.2 probada.

---

## [EN] English

### 🌐 WAN Remote Play Fix (Outside Home Network)

This **v0.8.4** release is built exclusively on top of the **working v0.8.2 base**, restoring full button A responsiveness on `Your Xbox`.

#### ✨ Key Features

1. **ICE Candidate Gathering & Validation (`engine.cpp`):**
   * Removed arbitrary priority threshold filtering (`candidate_priority > 1000`) that dropped valid public IPv4 endpoints.
   * Validated candidate syntax and passed all candidates to `libpeer` for reachability ranking.

2. **libpeer Deduplication & Ranking:**
   * Reachability ranking (`agent_remote_rank`): routable public IPv4 endpoints are tested before unroutable private LAN addresses.
   * Endpoint deduplication (`IP + port`) and per-pair budget limit.

3. **Untouched v0.8.2 UI Integrity:**
   * `src/switch/main.cpp` and all protected components remain 100% byte-for-byte identical to the working v0.8.2 base.
