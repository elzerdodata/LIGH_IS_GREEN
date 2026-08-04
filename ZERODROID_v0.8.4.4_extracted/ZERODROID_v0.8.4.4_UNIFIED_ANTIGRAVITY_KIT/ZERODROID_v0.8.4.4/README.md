# ZERODROID

**Zero Data · Nintendo Switch cloud gaming client**

ZERODROID is an open-source, unofficial Boosteroid client for Nintendo Switch
homebrew. Version 0.8.4.4 provides QR login, a controller-first game library,
native H.264 video, Opus audio and complete controller input.

> [!IMPORTANT]
> ZERODROID is a beta and is not affiliated with or endorsed by Boosteroid or
> Nintendo. It requires a legitimate Boosteroid account and does not provide,
> download or bypass access to games, subscriptions, DRM or Nintendo software.

## New in v0.8.4.4

- One unified source tree containing the complete v0.8.3 catalog work, the v0.8.4 display/library redesign and every later recovery/input hotfix.
- Touching the lilac Xbox/X icon in the top-right corner opens a large session control center instead of immediately sending Guide/Home.
- The control center displays the current game, gateway, stream/output resolutions, ping, session time, UDP recovery counters and mouse/keyboard transmission counters.
- Large touch actions provide Xbox Guide/Home, ALT+TAB, mouse enable/disable, graphics settings, reconnect and return to game.
- Reconnect requires confirmation and attempts to preserve the remote Boosteroid machine while rebuilding only ZERODROID's local transport. This remains experimental and must be verified on real hardware.
- B closes the control center. Plus+Minus is an optional secondary shortcut; touching the lilac X is the primary control.
- While an overlay is open, ZERODROID sends neutral controller input so the game cannot move or react behind the menu.
- The keyboard tile is intentionally informational in this build. Antigravity must not fake remote text injection or block the deko3d streaming window with an untested software-keyboard implementation.

## Included from v0.8.4.3

- Fixes desktop mouse/keyboard packets on native `/native` streams by using the browser-style `id_cmd` and `from_udp` envelope expected by Boosteroid.
- Adds **Minus + X** for ALT+TAB, matching the browser client's keyboard event sequence.
- Adds left click/drag with **Minus + ZR** or **Minus + ZL**, while **Minus + R3** remains right click.
- Reconnect Same Session closes only the local Switch transport before attempting a new attachment to the active app.

## Included from v0.8.4.2

- Fixes gradual-stick updates that could leave the remote game at a partial movement value and make a running character fall back to walking.
- Applies a radial dead zone, expands the useful Joy-Con outer range to full scale and refreshes held axes periodically.
- Adds a virtual mouse: touch/drag the Switch screen, or hold **Minus** and move the right stick.
- While holding **Minus**, press **R3** for right click. A plain Minus tap still sends the normal View/Back button.
- Adds Precise, Normal and Fast mouse speeds to the in-stream two-dot settings panel.
- Shows the mouse controls on the main library screen and inside the quick settings panel.

## Included from v0.8.4.1

- Fixes the native 1080p freeze reported as `native sequence gap: waiting for a clean IDR`.
- Uses a larger reorder window suitable for the heavier 1080p UDP stream.
- Tries soft decoder concealment before discarding the H.264 reference chain.
- Reasserts stream visibility during recovery and retries without allowing a permanent frozen frame.
- Collapses repeated missing group IDs into one recovery event and logs the recovery duration.

## Included from v0.8.4

- Selectable Auto, 720p, 1080p and experimental 1440p stream profiles.
- Auto requests 720p in handheld mode and 1080p while docked.
- Natural, Sharp, Vivid, Cinema, Soft and Custom picture presets in the in-stream two-dot menu.
- Persistent brightness, contrast, saturation, gamma and sharpness controls.
- Four-column library, compact fonts, two-line titles and uncropped horizontal artwork.

## Included from v0.8.3

- Removed the 50-game ceiling: **My Games** and the Boosteroid catalog now
  traverse every available API page, with duplicate and loop protection.
- Added separate My Games, Favorites, Recently Played, Catalog and Install
  sections designed for Joy-Con/Pro Controller navigation.
- Catalog cards identify games already in My Games, free titles, Install and
  Play entries and titles that require a legitimate store license.
- Pressing **A** on an uninstalled catalog title uses Boosteroid's official
  add-to-library action. Game purchases remain in each authorized external
  store; ZERODROID does not sell, bundle or unlock licenses.
- Hardened native UDP and WebRTC H.264 recovery: frames are kept in sequence,
  incomplete access units are dropped, corrupt decoder output is never shown,
  and playback resumes on a clean IDR while the last good frame stays visible.

## Included from v0.8.2

- Search the library with the native Nintendo Switch keyboard.
- All, Favorites and Recently Played library tabs.
- Persistent favorites and a 30-game recent history stored locally.
- Spanish and English interface selector in Settings.

## Included from v0.8.1

- Completely new ZERODROID identity and lilac/graphite visual system.
- Original ZD application icon.
- Live gateway ping meter during streaming.
- Touch Xbox Guide/Home button. Both stick clicks also send Guide.
- Touch two-dot menu during streaming.
- In-stream performance HUD, Nintendo/Xbox face-button layout and image
  controls.
- Library Settings menu for distant regions, preferred server and controller
  layout.

## Download and install

Download the latest NRO from [GitHub Releases](../../releases/latest), then
copy it to:

```text
sdmc:/switch/ZERODROID/ZERODROID.nro
```

Launch Homebrew Menu through title takeover by holding **R** while opening an
installed game. Album/applet mode does not provide enough memory for streaming.

The console must already be capable of running legal homebrew. ZERODROID does
not include or install custom firmware.

## Controls

- **Minus in the library:** ZERODROID Settings.
- **ZL / ZR:** switch between My Games, Favorites, Recent, Catalog and Install.
- **Y in the library:** search using the system keyboard.
- **A in Catalog/Install:** add a title to My Games; press again to play.
- **X in any library section:** add or remove the selected game from Favorites.
- **R3 in the library:** refresh the Boosteroid library.
- **Touch/drag during streaming:** move the remote mouse and hold/release left click.
- **Hold Minus + right stick:** move the remote mouse without touching the screen.
- **Hold Minus + ZR or ZL:** press/hold the remote left mouse button for click and drag.
- **Hold Minus + R3:** press/release the remote right mouse button.
- **Hold Minus + X:** send ALT+TAB to the remote Windows desktop.
- **Tap Minus:** send the normal View/Back controller button.
- **Two dots during streaming:** open the touch settings panel and choose mouse speed.
- **Touch Xbox/X symbol:** open session actions: Guide/Home, ALT+TAB, reconnect or close.
- **Press both sticks:** send Xbox Guide/Home without touch.
- **L + R + Minus:** stop streaming and return to the library.

Application data is stored only under `sdmc:/switch/ZERODROID/`.

## Technical overview

- C++17 application built with devkitA64 and libnx.
- SDL2 library and login UI; deko3d video presentation while streaming.
- FFmpeg/libavcodec H.264 decoding and Opus audio playback.
- libpeer, WebRTC data channels, SRTP and SCTP for the streaming transport.
- Device-code QR authentication; the resulting session remains on the SD card.
- No analytics, advertising, referral links or telemetry.

## Build from source

The standard devkitPro Switch toolchain and Switch portlibs are required. The
WebRTC dependency bundle can be built reproducibly with Docker:

```bash
cd switch
bash deps/build-switch.sh
make -j4
```

The dependency script clones upstream sources, applies the Switch-specific
patches in `switch/deps/patches/`, and places generated libraries under the
ignored `switch/deps/switch/` directory.

## Beta status and feedback

Service-side API or streaming changes can temporarily break an unofficial
client. Please include the ZERODROID version, server region and relevant lines
from `sdmc:/switch/ZERODROID/stream.log` in reproducible bug reports. Remove
any personal information before posting logs publicly.

Licensed under Apache-2.0. Third-party components remain under their respective
licenses; see [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
