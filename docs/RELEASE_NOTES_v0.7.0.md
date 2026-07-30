# Light is Green v0.7.0

This is a preview release. v0.6.0 remains the recommended stable version.

## Video correctness and safety

- Fixes the thin green/magenta corruption stripe visible along the bottom or
  right edge of streamed video. The deko3d shader now samples the visible texel
  centres of the NV12 luma and chroma planes independently, avoiding NVDEC's
  uninitialized alignment padding.
- Motion's second NVDEC surface must have completed a normal primary render and
  pass strict layout/backing-map validation before the shader can sample it.
  Rejected surfaces produce an ordinary Smooth refresh instead.
- **Motion remains 100% experimental and may cause rapid green flashing. Stop
  using it immediately and select Smooth or Steady if flashing is observed.**
- Motion is a per-session opt-in and is never restored automatically when the
  app starts; a previously saved Motion setting safely migrates to Smooth.

## In-stream controls

- The two-dot touch panel now includes Pacing: Steady / Smooth / Motion.
- Modes change live without restarting the WebRTC stream.
- A mode change releases queued and interpolation surfaces from the previous
  mode before the next frame is presented.
- The Motion warning remains visible inside the touch panel.

## Connection information

- The preload/connection screen displays the requested server region while
  signing in and updates to the actual Xbox-selected region after login.
- Xbox Remote Play explicitly displays the xHome route instead of an unrelated
  xCloud datacenter.
- Includes the v0.6.1-beta.2 xHome/Teredo negotiation and retry improvements.

## Español

- Corrige la franja verde/magenta del borde inferior o derecho evitando el
  relleno sin inicializar de las superficies NVDEC.
- Los dos puntos permiten cambiar Steady / Smooth / Motion durante el juego sin
  reiniciar WebRTC y liberan los cuadros retenidos por el modo anterior.
- La pantalla de conexión muestra la región solicitada y luego la región real
  elegida por Xbox.
- **Motion continúa siendo 100% experimental y podría provocar pantalla verde
  parpadeante. Si ocurre, debe cambiarse inmediatamente a Smooth o Steady.**
- Motion requiere activación en cada sesión; una selección Motion guardada se
  migra automáticamente a Smooth al iniciar la aplicación.

## Credits

Light is Green is a GPL-3.0 community fork of
[green-nx](https://github.com/rmrf404/green-nx), originally created by
**rmrf404**. Light is Green development is credited to **elzerdodata**.
