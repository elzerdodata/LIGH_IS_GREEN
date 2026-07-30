# Light is Green v0.7.2-beta

Esta versión corrige el funcionamiento del perfil **Motion** (frame generation/interpolation 30fps -> 60fps).

## Cambios y Correcciones en v0.7.2-beta
* **Corrección del perfil Motion:** Se eliminó la restricción `candidate->primary_ready` en el renderizador deko3d (`DkVideoRenderer::render`), la cual impedía la asignación de `motion_fm` y desactivaba la mezcla de fotogramas (frame blending).
* **Forzado de cadencia de 2 refrescos en modo Motion:** Ajustado el pacer en `Engine::pump_video()` para forzar un período de cadencia (`period = 2`) cuando el perfil Motion está activo, permitiendo la generación de fotogramas intermedios (50/50 blend) en pantallas de 60Hz.
* **Persistencia del perfil Motion:** Eliminado el reseteo automático de `settings.pacing` a `1` (Smooth) al iniciar la aplicación, permitiendo que la preferencia del usuario se guarde correctamente.
