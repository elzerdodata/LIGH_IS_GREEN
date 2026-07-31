# Light is Green v0.7.2-beta (Green Screen Fix Release)

> ⚠️ **Nota Importante / Important Note:**
> **[ES]** La versión preliminar v0.8.0 fue retirada del repositorio debido a que requería pruebas adicionales de estabilidad. La v0.7.2-beta es la versión oficial actual recomendada para su uso.
> **[EN]** The preliminary v0.8.0 release was withdrawn from the repository as it required additional stability testing. v0.7.2-beta is the current official recommended release.

---

## 🇪🇸 Notas de Lanzamiento en Español

### 💚 Corrección del Parpadeo Verde (Perfil Motion)
* **Solución a la falla de croma YUV / deko3d:**
  * Se corrigió la colisión de memoria GPU (*descriptor set race condition*) en deko3d asignando búferes de descriptores aislados por ranura de swapchain (`0x500`, `0x600`, `0x700`).
  * Se aplicó la mezcla de fotogramas al 50% en el plano Luma (*Luma-Only Blending*), eliminando permanentemente los destellos y parpadeos verdes al activar la interpolación a 60 Hz.

---

## 🇬🇧 Release Notes in English

### 💚 Green Screen / Flashing Fix (Motion Profile)
* **Resolved YUV Chroma / deko3d Memory Race Condition:**
  * Fixed GPU memory race condition in deko3d by assigning isolated descriptor set buffers per swapchain slot (`0x500`, `0x600`, `0x700`).
  * Implemented 50% Luma-Only frame interpolation, permanently eliminating rapid green flashing when using the 60 Hz Motion profile.
