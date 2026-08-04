# ZERODROID v0.8.4.4 desktop input and session notes

## Mouse failure found in v0.8.4.2

The virtual mouse generated the expected `mouse/move` and `mouse/button` JSON,
but the native `/native` media path sent it as plain Android-TV control JSON.
Boosteroid's browser desktop-input path uses an `id_cmd` sequence and
`from_udp` marker. v0.8.4.4 keeps controller traffic unchanged and applies that
browser envelope to mouse and keyboard packets over the control WebSocket.

Expected first-use log entries:

```text
mouse input enabled with browser envelope on control WSS
keyboard input enabled with browser envelope on control WSS
```

## ALT+TAB

The sequence is Alt down, Tab down, Tab up, Alt up, with a short delay between
states. It is available from `Minus + X` and the touch session-actions panel.

## Reconnect Same Session

Normal Stop sends controller disconnect, `settings/terminating`, gateway
hangup and the Boosteroid API stop/dequeue operation. Reconnect closes the local
control/media/decoder resources but intentionally skips terminating, hangup and
API stop. A fresh Engine then launches the same app ID; the existing API code
polls the last/active-session endpoints while waiting for the session signal.

This path is experimental because the service can still decide that the old
session is no longer attachable. On failure, ZERODROID returns to the library
with the API or gateway error rather than silently claiming success.

## Controls

- Touch or drag the Switch screen: absolute pointer + left click/drag.
- Hold Minus + right stick: virtual pointer.
- Hold Minus + ZR or ZL: left click/drag.
- Hold Minus + R3: right click.
- Hold Minus + X: ALT+TAB.
- Touch Xbox/X icon: open Guide, ALT+TAB, Reconnect and Close actions.
