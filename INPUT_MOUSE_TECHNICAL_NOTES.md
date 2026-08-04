# ZERODROID v0.8.4.2 input and mouse notes

## Analog-stick correction

The previous sender compared each physical stick sample with the immediately
previous physical sample. Small progressive movements could remain below the
change threshold forever even though the accumulated movement was large. The
remote game could therefore keep an older partial value and interpret a fully
held stick as walking.

v0.8.4.2 instead:

- compares each axis with the last value actually transmitted;
- uses a radial dead zone of 3200;
- expands a practical outer magnitude of about 30000 to the full 32767 range;
- sends changes of 700 or more immediately;
- always sends neutral transitions and outer-range transitions;
- refreshes held non-neutral axes every 120 ms;
- emits a rate-limited `gamepad TX` diagnostic every two seconds while a stick
  is active.

## Mouse protocol

Mouse events use the same Boosteroid event shape used by the browser client:

```json
{"type":"mouse","action":"move","X":0.5,"Y":0.5,
 "offsetX":0,"offsetY":0,"isVisible":true}
```

```json
{"type":"mouse","action":"button","btn":0,"isPressed":true}
```

Button `0` is left click and button `2` is right click.

## Touchscreen behavior

- Touch down outside ZERODROID overlays moves the cursor and presses left click.
- Drag updates the absolute cursor position.
- Touch release releases left click.
- The Guide and two-dot controls retain priority.
- When the quick menu is open, a touch outside closes it without clicking the
  remote game.

## Minus modifier

- Hold Minus and move the right stick to move the mouse.
- Hold Minus and press R3 to press/release right click.
- The right stick and R3 are suppressed from controller packets during mouse
  mode, preventing camera movement or an accidental stick click.
- A Minus press that never uses mouse mode is sent as a short View/Back press on
  release.
- L + R + Minus remains the stream-exit chord.

## Mouse speed

The two-dot panel stores one of three persistent cursor speeds:

- Precise: 0.38 screen widths per second at full stick
- Normal: 0.78 screen widths per second
- Fast: 1.28 screen widths per second

The stick response is nonlinear for fine control near the center.
