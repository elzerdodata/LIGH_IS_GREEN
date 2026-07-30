# Light is Green v0.8.0 (Stable Release)

## 🇪🇸 Notas de Lanzamiento en Español

### 🌟 Novedades Principales
* **Frame Generation sin Parpadeo Verde (Modo Motion):**
  * Se corrigió la falla de croma YUV en el renderizador deko3d mediante interpolación **50% Luma-Only** y aislamiento de descriptores por ranura de memoria GPU (`0x500`, `0x600`, `0x700`).
  * Generación de cuadros de 30 Hz a 60 Hz suave y fluida con cero destellos verdes.
* **Modo `Force region` (Forzar Servidor Seleccionado sin Fallback):**
  * Nueva opción en el menú de configuración que desactiva los servidores de respaldo de Microsoft.
  * Garantiza que las conexiones a **Brasil (São Paulo)** o **Chile** no sean transferidas silenciosamente a servidores de EE.UU., manteniendo pings bajos (40-50 ms en LATAM).
* **Control de Bitrate Máximo (Max Bitrate):**
  * Selector de ancho de banda WebRTC configurable entre **Auto**, **7 Mbps**, **14 Mbps**, **20 Mbps** y **30 Mbps (Ultra HQ)**.
* **Interfaz de Ajustes Reorganizada:**
  * Opciones de servidor (`Server region`), bypass de IP (`Region bypass`) y forzado de servidor (`Force region`) agrupadas secuencialmente al principio del menú.
* **Correcciones en Tiempos de Espera y Catálogo:**
  * Soporte expandido para múltiples formatos de claves de tiempo estimado de espera en colas xCloud.
  * Filtro de catálogo ajustado para incluir nuevos juegos sin depender de etiquetas rígidas de programa.

---

## 🇬🇧 Release Notes in English

### 🌟 Key Highlights
* **Green Flashing Fix in Motion Profile (Frame Generation):**
  * Resolved YUV chroma zeroing in deko3d renderer via **50% Luma-Only** frame interpolation and per-slot GPU descriptor set memory isolation (`0x500`, `0x600`, `0x700`).
  * Smooth 30 Hz to 60 Hz motion blending with zero green flickering.
* **`Force region` Mode (Strict Datacenter Selection without US Fallback):**
  * New settings toggle that disables Microsoft fallback region lists.
  * Ensures connection requests to **Brazil South** or **Chile Central** remain locked to local datacenters instead of being silently transferred to US servers, preserving low latencies (40-50 ms in South America).
* **Maximum Bitrate Selector (Max Bitrate):**
  * Configurable WebRTC video bitrate cap options: **Auto**, **7 Mbps (Low)**, **14 Mbps (Medium)**, **20 Mbps (High)**, and **30 Mbps (Ultra HQ)**.
* **Reordered Settings UI:**
  * `Server region`, `Region bypass`, and `Force region` are sequentially grouped together at the top of the settings menu for intuitive navigation.
* **Queue Wait-Time & Catalog Fixes:**
  * Expanded wait-time parsing supporting both numeric and string payload formats across xCloud response types.
  * Relaxed entitlement filtering to correctly render newly released games.
