# ZERODROID changelog

## v0.8.4.4 - Unified control center

- Consolidates every feature introduced in v0.8.3, v0.8.4, v0.8.4.1, v0.8.4.2 and v0.8.4.3 into one clean source tree and one reproducible build target.
- Replaces the compact session popup with a large lilac/graphite session control center opened from the top-right lilac Xbox/X touch icon.
- Adds live session diagnostics: game, negotiated stream resolution, Switch output resolution, ping, UDP drop/recovery counters, recovery requests, gateway, session time and desktop-input transmission counters.
- Adds large touch actions for Xbox Guide, ALT+TAB, mouse enable/disable, graphics settings, reconnect and return to game.
- Adds a mandatory confirmation step before reconnecting so an accidental touch cannot tear down the local transport.
- Captures controller input while either in-stream overlay is visible, preventing the game from moving or accepting buttons behind the menu.
- Adds B to close the overlay and keeps Plus+Minus as an optional controller shortcut; the preferred action remains touching the lilac X.
- Keeps the keyboard action informational until a safe deko3d suspend/resume and remote text-injection path is proven.

## v0.8.4.3 - Desktop input and session recovery

- Sends mouse and keyboard events with Boosteroid's browser-style `id_cmd` / `from_udp` control envelope even while the media path is native UDP.
- Adds ALT+TAB through **Minus + X** and through the touch session-actions panel.
- Adds controller left click/drag through **Minus + ZR** or **Minus + ZL**; **Minus + R3** remains right click.
- Changes the always-visible Xbox/X touch icon into a session-actions launcher while preserving Guide/Home as an explicit action.
- Adds an experimental Reconnect Same Session path that closes local media/control resources without terminating or dequeuing the remote VM, then starts a fresh attachment to the active Boosteroid session.
- Adds visible bilingual mouse, ALT+TAB and session-action instructions.

## v0.8.4.2 - Analog input and virtual mouse

- Compares analog input against the last value actually transmitted instead of the previous physical sample, so gradual stick movement accumulates correctly.
- Applies a radial dead zone and maps the practical Joy-Con outer range to the full Android controller range.
- Refreshes non-neutral axes every 120 ms and adds throttled full-axis diagnostics to `stream.log`.
- Adds absolute Boosteroid mouse events for Switch touchscreen movement, left-click press/release and drag.
- Adds a delayed Minus modifier: hold Minus and use the right stick as a virtual mouse; Minus + R3 sends right click.
- Preserves a normal Minus/View press by synthesizing a short controller press when Minus is released without using mouse mode.
- Suppresses the right stick and R3 controller events while the Minus mouse modifier is active, preventing simultaneous camera movement.
- Adds Precise, Normal and Fast mouse-speed presets to the in-stream two-dot panel.
- Adds bilingual mouse-control instructions to the main library screen.

## v0.8.4.1 - Native video recovery hotfix

- Increased the native UDP reorder window from 140 ms / 8 groups to 350 ms / 24 groups for 1080p sessions.
- Replaced the permanent IDR gate after an isolated sequence gap with two-stage recovery: soft FFmpeg concealment first, hard IDR recovery only after a real decoder error.
- Added rate-limited native visibility refresh requests while recovery is active.
- Added a 2.5-second controlled decoder probe so an ignored keyframe request cannot leave the last frame frozen forever.
- Jumps directly to the nearest observed native group when several group IDs are absent, preventing the repeated `native sequence gap` log storm.
- Added recovery lifecycle logging: start, clean IDR, decoder probe and successful recovery duration.
- Increased the native decode queue safety limit from 16 to 24 complete access units.

## v0.8.4 - Display quality and library redesign

- Added Auto, 720p, 1080p and experimental 1440p streaming profiles.
- Auto requests 720p handheld and 1080p docked; 1440p is decoded and downscaled to the active Switch output.
- Added Natural, Sharp, Vivid, Cinema, Soft and Custom picture presets to the in-stream two-dot menu.
- Persisted resolution and image controls across launches.
- Loaded true small/body/title/note fonts instead of falling back to the oversized body font.
- Changed the game grid to four columns with two-line titles and clearer metadata.
- Preferred higher-resolution horizontal artwork and changed card rendering from crop/cover to contain.

## v0.8.3 - Complete catalog and clean video recovery

- Removed the fixed 50-game request and added guarded pagination for every page
  of both the installed library and complete Boosteroid catalog.
- Added separate My Games, Favorites, Recent, Catalog and Install sections.
- Added Boosteroid's official add-to-library action for catalog entries.
- Added catalog state badges for My Game, Install, Free and License Required.
- Ordered native UDP video groups before decoding and detected stalled/missing
  Reed-Solomon groups instead of feeding later predictive frames out of order.
- Kept the last good video frame on loss/corruption and resumed only from a
  clean H.264 IDR; corrupt FFmpeg frames are no longer presented.
- Reduced the video decode queue safety bound and treats overflow as a resync
  event instead of silently continuing with broken references.

## v0.8.2 - Library organization

- Added native game search through the Nintendo Switch software keyboard.
- Added All, Favorites and Recently Played tabs.
- Added persistent favorites and a 30-game recent history.
- Added a persistent Spanish/English interface selector.
- Moved sign out into Settings so X can toggle favorites.

## v0.8.1 - Local test beta

- New ZERODROID name, visual identity, data directory and application icon.
- Added WebSocket round-trip ping measurement and an always-visible ping badge.
- Activated the touch Xbox Guide/Home button.
- Activated the touch two-dot stream settings panel.
- Added Xbox Guide through simultaneous left/right stick clicks.
- Added live Nintendo/Xbox A/B/X/Y switching from Settings and the touch panel.
- Preserved preferred server and distant-region configuration.
