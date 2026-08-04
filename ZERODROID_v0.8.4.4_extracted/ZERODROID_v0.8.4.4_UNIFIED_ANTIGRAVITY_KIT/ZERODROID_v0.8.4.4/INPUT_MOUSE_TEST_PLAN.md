# ZERODROID v0.8.4.2 input and mouse test plan

## 1. Analog movement regression

1. Launch Final Fantasy VII at 720p or 1080p.
2. From neutral, push the left stick slowly to its physical edge.
3. Confirm the character reaches running speed and does not remain walking.
4. Release slowly and confirm the character returns to neutral.
5. Repeat in all eight main directions and with a circular movement.
6. Repeat with a Pro Controller if available.

Expected log while moving:

```text
gamepad TX LX=... LY=... RX=... RY=...
```

At a physical edge, the radial magnitude should approach 32767.

## 2. Minus compatibility

1. Tap Minus without touching the right stick.
2. Confirm the game receives its normal View/Back action once.
3. Hold Minus for two seconds without moving the stick, then release.
4. Confirm it still produces one View/Back action.
5. Confirm L + R + Minus still exits streaming.

## 3. Stick mouse

1. Hold Minus and move the right stick.
2. Confirm the cursor moves while the game camera does not.
3. Test Precise, Normal and Fast in the two-dot menu.
4. Release Minus and confirm the right stick immediately controls the game
   normally again.

## 4. Right click

1. Hold Minus.
2. Press and hold R3 over an item that has a context menu.
3. Confirm right-click press is received.
4. Release R3 and confirm the button is released.
5. Confirm the game does not also receive an R3 controller click.

## 5. Touch mouse

1. Tap a launcher or text field on the Switch screen.
2. Confirm the cursor moves to the touched position and performs left click.
3. Drag a window, scrollbar or slider and confirm left click stays held until
   the finger is lifted.
4. Tap the Guide icon and two-dot icon; confirm neither tap reaches the game.
5. Open the two-dot menu, then tap outside; confirm the panel closes without an
   accidental game click.

## 6. Video regression

Run at least 15 minutes at 1080p and confirm the v0.8.4.1 native-video recovery
remains active and the image does not stay frozen after a sequence gap.
