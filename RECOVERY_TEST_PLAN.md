# ZERODROID v0.8.4.4 recovery regression test plan

The hotfix was prepared from the v0.8.4 source and the two uploaded session logs.
Both logs reached native 1920×1080 correctly and then entered the same sequence-gap state while controller messages continued.

## Build

```bash
cd switch
make clean
make -j"$(nproc)"
```

Expected output:

```text
switch/ZERODROID_v0.8.4.4.nro
```

## First validation

1. Start with **720p + Natural** and play for at least 15 minutes.
2. Repeat with **1080p + Natural** for at least 20 minutes.
3. Repeat the Final Fantasy VII section that froze on v0.8.4.
4. Do not use 1440p until 1080p is stable.

## Expected recovery log

A real packet-loss event should now look like this:

```text
native video recovery started: native sequence gap (soft concealment)
native recovery refresh requested: native sequence gap
native video recovered after N ms
```

If FFmpeg detects actual corruption, the log may instead show:

```text
native video recovery started: native decoder corruption (hard IDR wait)
native clean IDR received; decoder recovery can resume
native video recovered after N ms
```

If the gateway ignores the keyframe request, the client should eventually log:

```text
native IDR timeout: probing decoder with a complete access unit
```

The image must not remain frozen indefinitely. Audio and controls should continue throughout recovery.

## Files to return after testing

```text
sdmc:/switch/ZERODROID/stream.log
sdmc:/switch/ZERODROID/stream-prev.log
```

Also record the selected resolution, handheld/docked state, game and approximate time until any recovery event.
