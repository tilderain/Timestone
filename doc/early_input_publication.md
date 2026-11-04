# Pre-Render Input Publication

## Boundary

The first early-send experiment publishes the finalized local input history at
entry to `sRender::move` (`0x14053CAF0`), before calling the original renderer.
Ghidra inspection of `sMvc3Main::move` (`0x1402594A0`) places this dispatch after
unit/character processing and before draw construction, playback, and Present.

This is **not immediate hardware-sample transmission**. Prediction-budget waits,
correction/snapshot work, native pacing, sampling, and gameplay input processing
still precede this boundary. It removes the rendering tail from the normal
sample-to-publication path without transmitting a partially accumulated mask.

`g_localPending` still comes from existing gameplay remapping, including client
keyboard merging, SOCD/native processing, and synthetic/CPU input paths. The
hardware sampling hooks are not changed.

## Scheduling

`Controller_OnFrameStart` disarms early publication before any possible return
or wait, and arms it only after successful frame-start processing. The first
live pre-render publication consumes that eligibility. Resimulation cannot
publish or consume it; unsynchronized/restarting battles are excluded. Buffer
reset and frame end disarm it too.

Both phases use `PublishFrameInput` and the original history format:

- Early phase schedules `g_netFrameCount + g_delayFrames` without overwriting an
  already scheduled value. Delay-decrease precedence remains unchanged.
- Early packets have no new checksum. The full consecutive input history and
  frame-advantage field are retained.
- Frame end still records executed local input, attaches zero-delay snapshot
  input, updates legacy history, computes/advertises checksums, and handles
  rollback-setting transitions and desync checks.
- Frame end publishes again, also providing a fallback if rendering was skipped.
  There can be two input-history packets per frame; the existing latest-packet
  mailbox can coalesce them if the service thread has not consumed the first.
- A checksum-less publication preserves an unsent mailbox checksum from the same
  battle generation. It never carries a checksum across battle generations.

## Network Wakeup

`Netplay_SendPacket` still only updates the mailbox under its critical section.
After unlocking, it signals an auto-reset outbound event. All ENet send/flush
operations remain on the service thread.

The service thread uses nonblocking `enet_host_service` and waits on both the
outbound event and a Winsock `FD_READ` event for its UDP socket. Socket events
are acknowledged before draining incoming packets. A one-millisecond timeout
retains maintenance/control polling and independent 16 ms history retransmits.
Event setup failure falls back to the original one-millisecond ENet service
polling. Wake handles are process-lifetime and reused across sessions. Shutdown
signals the outbound event before the existing service-thread join.

Steam transport's separate bridge is unchanged; it can still add its own
scheduling/polling delay. Event notification does not guarantee immediate OS
scheduling or packet delivery.

## Validation

`tools/test_early_input_send.py` exercises production packet construction and
mailbox code: delayed and zero-delay input, delay-decrease immutability, neutral
input, frame-end fallback, checksum separation, repeat calls, excluded phases,
and checksum carry/wakeup after unlocking. Static checks cover hook ordering
and receive/outbound wait registration. These mocks do not validate Winsock
event behavior under Wine or actual controller-to-wire latency.

For runtime verification, run both peers with this build and check startup for
`Netplay: input/socket wakeups enabled`. Existing `INPUT_PROVENANCE_TRACE` can
show `IPTX` phases `pre-render` and `frame-end`, but enabling it adds logging
overhead. The movement port does not include lag diagnostics; use external
profiling to compare latency and frame pacing. Test restart, disconnect, delay changes and zero-delay
play; do not infer a measured latency improvement from successful compilation.
