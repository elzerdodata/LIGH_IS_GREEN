# Light is Green v0.7.1

This is a smoothness-focused preview. v0.6.0 remains the recommended stable
release, and v0.7.0 remains available for comparison.

## Smoother pacing

- Source cadence now comes from H.264's 90 kHz RTP timestamps. Network arrival
  bursts no longer make the client mistake 30 fps for 60 fps or vice versa.
- Smooth still retains one decoded frame to absorb ordinary jitter. When a
  burst leaves more than that target reserve, one stale presentation frame is
  dropped to return to low latency instead of carrying the backlog forever.
- The deko3d renderer now uses its three framebuffer slots asynchronously.
  Command memory and uniform data are isolated per slot, and every primary or
  Motion NVDEC surface remains referenced until that slot is acquired again.
  The GPU is no longer forced completely idle after every present.

## Diagnostics

- `stream-log.txt` now writes a `video|` line once per second with RTP packets,
  assembled frames, unrecoverable drops, NACKs, keyframe resyncs, last access
  unit size, and the decoder queue depth.
- Existing `pace|`, packet-loss and audio-buffer metrics remain available. Do
  not publish the complete log because ICE diagnostics can contain IP addresses.

## Recommended test

Use **Smooth** from the in-game two-dot panel and test both a camera pan and a
fast game. If stutter remains, compare the `video|`, `pace|`, loss and buffer
lines from the log. Motion remains 100% experimental and may cause rapid green
flashing; leave it immediately if that occurs.

## Español

- La cadencia usa el timestamp RTP real del video y ya no hereda las ráfagas de
  llegada de la red.
- Smooth vuelve automáticamente a una sola trama de reserva después de una
  ráfaga para evitar que la latencia quede acumulada.
- El render usa los tres framebuffers de forma asíncrona y conserva referencias
  seguras a las superficies NVDEC hasta que la GPU termina de usarlas.
- El log agrega una línea `video|` para distinguir problemas de red, pérdida,
  resincronización y atasco del decodificador.

## Credits

Light is Green is a GPL-3.0 community fork of
[green-nx](https://github.com/rmrf404/green-nx), originally created by
**rmrf404**. Light is Green development is credited to **elzerdodata**.
