# Light is Green v0.6.1-beta.1

This is an experimental prerelease. v0.6.0 remains the stable release.

## Highlights

- **Pacing: Steady / Smooth / Motion**
  - Steady keeps the lowest-latency latest-frame behavior.
  - Smooth holds one decoded frame in reserve to absorb network jitter.
  - Motion adds a 50/50 GPU midpoint between consecutive frames when a 30 fps
    source cadence is detected, producing a 60 Hz presentation sequence.
- **Experimental 1080p Xbox Remote Play**
  - Requests 1920x1080, H.264 level 4.2 and a conservative 20 Mbps profile.
  - If the session connects but produces no video for 15 seconds, Light is
    Green discards it and retries automatically with stable 720p capabilities.
- **Expanded performance HUD** with source FPS, display-output FPS and Motion
  generated FPS.
- **Away-from-home diagnostics** now point to Xbox Remote Features, NAT,
  IPv6/Teredo and UDP when no viable xHome route is found.

## Important limitations

- Motion is simple cross-frame blending. It does not estimate motion vectors,
  reconstruct occluded detail or provide DLSS/FSR-style optical-flow frame
  generation. Fast camera movement can show double images.
- Xbox controls the final Remote Play allocation. Announcing 1080p capability
  does not guarantee that every console, account or network will receive it.
- Remote Play outside the home is supported by the existing Xbox xHome route,
  but depends on the console's Remote Features configuration and network path.
- Logs are useful for diagnosis but may contain IP candidates. Do not attach
  `stream-log.txt` to a public issue without reviewing and redacting it.

## Compatibility and rollback

- Existing v0.6 `smooth` settings migrate to the new `pacing` setting.
- The legacy `smooth` key is still written so returning to v0.6 keeps a safe
  pacing choice.
- The default remains Steady pacing and stable 720p console Remote Play.

## Credits

These historical v0.x notes are retained for release continuity. Current
licensing and distribution information is provided in `LICENSE` and `NOTICE`.
