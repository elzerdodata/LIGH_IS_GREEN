# Native recovery hotfix technical notes

## Failure in v0.8.4

`handle_native_video_packet()` used a 140 ms / 8-group reorder window. When a group was declared missing, it immediately set `nativeWaitingKeyframe_ = true`, cleared the decode queue and discarded all subsequent non-IDR access units. The native route had no guaranteed request/response path for an immediate IDR, so one loss event could hold the last good frame forever.

The loop also advanced one absent group ID at a time. During a larger discontinuity this generated the same log message repeatedly and could keep discarding newly completed groups while trying to catch up.

## v0.8.4.1 behavior

- 350 ms hold window and 24-group reorder threshold.
- Direct jump to the nearest observed newer group when the expected group does not exist.
- Soft recovery on sequence loss: complete access units continue to FFmpeg without flushing its reference surfaces.
- Hard IDR gating only after malformed data, Reed-Solomon failure, queue overflow or a decoder-reported error.
- Rate-limited visibility reassertion as a best-effort native encoder refresh.
- 2.5-second probe fallback to avoid a permanent wait when the gateway does not provide an IDR.

The native keyframe control action is not documented by Boosteroid in this source tree. It is intentionally best-effort; the soft recovery and probe fallback are the mechanisms that prevent the client from depending solely on that message.
