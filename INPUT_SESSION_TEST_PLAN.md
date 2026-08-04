# ZERODROID v0.8.4.4 input/session test plan

Test with title takeover, not Album/applet mode. Keep `stream.log` after each
failed test.

## 1. Mouse transport

1. Launch a game that exposes a launcher or Windows desktop.
2. Touch four corners of the screen and verify the pointer follows.
3. Tap a button, then drag a window or slider.
4. Hold Minus and move the right stick.
5. Verify `Minus + ZR` left click/drag and `Minus + R3` right click.
6. Confirm `stream.log` contains the browser-envelope mouse entry.

## 2. ALT+TAB

1. Hold Minus and press X once.
2. Verify the Windows task switcher appears and no X gamepad press reaches the
   game.
3. Open the touch Xbox/X panel and select ALT+TAB.
4. Confirm the log contains `sending ALT+TAB` and the keyboard-envelope entry.

## 3. Reconnect Same Session

1. Play until the remote VM is fully active.
2. Touch Xbox/X, choose Reconnect Same Session.
3. Verify ZERODROID returns briefly to its connecting panel.
4. Confirm the same remote desktop/game state resumes rather than starting a
   fresh game.
5. Inspect the log for `local transport closed; preserving remote session`.
6. If reconnection fails, preserve the complete new `stream.log`.

## 4. Regression

- Plain Minus still sends View/Back.
- L + R + Minus still closes streaming normally.
- Both stick clicks still send Guide/Home.
- Gamepad right stick, X, ZL/ZR and R3 work normally when Minus is not held.
- Video remains stable for at least 20 minutes at 720p and 1080p.
