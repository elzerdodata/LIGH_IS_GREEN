# Light is Green v0.6.1-beta.2

This is an experimental prerelease focused on Remote Play connection recovery.
v0.6.0 remains the stable release.

## Remote Play resilience

- Console wake/registration and WebRTC media negotiation now have independent
  retry budgets. A slow Xbox startup no longer consumes the attempts reserved
  for fresh ICE/DTLS sessions.
- xHome can retry console readiness up to 10 times and rebuild the media path
  up to 8 times before showing a final diagnostic.
- Fresh media sessions use progressive 8–20 second backoff so Xbox has time to
  release the previous server-side reservation.
- Remote ICE gathering receives up to 25 seconds, DTLS/SCTP receives up to 25
  seconds after ICE connects, and total xHome negotiation receives up to 75
  seconds per fresh session.
- Teredo candidates now accept compressed IPv6 notation, retain a usable ICE
  priority after IPv4 conversion, and are checked before the known xHome
  placeholder route. This specifically targets Remote Play outside the LAN.
- A failed experimental 1080p negotiation switches every remaining attempt to
  stable 720p. A connected 1080p session gets 25 seconds for first video;
  stable 720p gets 60 seconds before another fresh session is requested.
- The xHome endpoint and credentials refresh after every third transport
  failure, while safely retaining the last route if refresh is unavailable.
- Retry cleanup now resets peer, handshake, end-of-session, jitter, queued
  video, audio and telemetry state so stale data cannot poison the next route.

## Existing beta features

- Pacing: Steady / Smooth / Motion.
- Experimental 1080p Xbox Remote Play with automatic 720p fallback.
- Performance HUD with source, output and generated FPS.
- Away-from-home Remote Play through Xbox xHome, subject to Remote Features,
  NAT, IPv6/Teredo and UDP connectivity.

## Important limitation

More retries cannot create a route that the network permanently blocks. If all
fresh sessions fail, leave the Xbox powered on for a minute, verify Remote
Features, NAT/IPv6 and UDP, then restart Remote Features or the console.

## Credits

Light is Green is a GPL-3.0 community fork of
[green-nx](https://github.com/rmrf404/green-nx), originally created by
**rmrf404**. Light is Green development is credited to **elzerdodata**.
