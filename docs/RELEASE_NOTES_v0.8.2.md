# Light is Green v0.8.2

## WAN Remote Play Fix / Corrección de Remote Play WAN (Fuera de Casa)

- Fixes Xbox Remote Play connection when Nintendo Switch is on a different Wi-Fi network or mobile hotspot.
- Removes priority threshold filtering (`candidate_priority > 1000`) in `engine.cpp` that discarded valid public IPv4 ICE candidates.
- Relies on libpeer's deduplication and routability ranking (`agent_remote_rank`) to prioritize reachable public endpoints over non-routable private LAN IPs when streaming over WAN.
- Preserves full compatibility with same-LAN Remote Play, xCloud streaming, Microsoft Sign-In, picture profiles, and custom bitrates.
