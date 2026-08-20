# Post-restart desync false positive: per-peer frame-epoch phase offset

Status: hypothesis, code-confirmed, not yet validated against live logs.

## Observation

Session `nplive_20260802_224438` (forced FrameStepper match restarts, not autotest —
generations 7..12, kind=0, seed=0; no re-simulation ran):

- Host logged a desync mismatch at **confirmed frame 61**.
- Client logged a desync mismatch at **confirmed frame 68**.
- Both peers logged a mismatch at **confirmed frame 451**.

61 vs 68 = **7-frame gap**. The f76/f83 snapdump pairs show **zero divergence** in
every checksum-covered field (`+0x50`, `+0x54`, `+0x1550`, `+0x1310`, `+0x2D80`,
global RNG) across frames 0..83. The f83 client alert therefore has no in-dump
state counterpart — it is not a state desync.

## Conclusion

The detector is comparing **different timeline positions on the two peers**. Host
frame 61 and client frame 68 are the *same real divergence point*, reached at a
different logical-frame label on each machine because the two peers' logical frame
epochs are phase-offset by ~7 frames.

## Mechanism (code)

1. **Epoch is set per-peer at intro-barrier release.** On mutual
   READY/RELEASE, each peer runs `Controller_ResetFrameEpoch()`
   (`UMVC3Hook/code/controller.cpp:391-397`):

   ```
   g_gameFrameBase = *(int32_t*)(sReplay + 0x54);   // THIS peer's native counter
   g_netFrameCount = 0;
   ```

   Logical frame 0 is anchored to each peer's **own** native replay counter at its
   **own** release moment. The READY/RELEASE handshake
   (`UMVC3Hook/code/framestepper.cpp:503-512`) is real-time/network paced, so the
   two machines can observe the barrier release at slightly different native
    counters (different `step==2 && pendingStep<0` observation points, latency,
   per-machine pacing). That difference becomes a phase offset in logical numbering.

2. **The write-back rebase locks the offset in.** Every frame,
   `Controller_OnFrameStart()` (`UMVC3Hook/code/controller.cpp:1216-1228`) does:

   ```
   g_netFrameCount = GetGameFrameCount();            // native - g_gameFrameBase
   if (sReplay) { *(int32_t*)(sReplay + 0x54) = g_netFrameCount; g_gameFrameBase = 0; }
   ```

   Each peer's native counter is forced to track its local logical count, so the
   offset never self-corrects — both peers keep their own shifted numbering.

3. **Detection compares by logical frame index.**
   `UpdateConfirmedChecksums()` / `Controller_OnFrameEnd()`
   (`UMVC3Hook/code/controller.cpp:1540-1573, 1687-1729`) compare
   `g_localChecksum[frame]` vs `g_remoteChecksum[frame]` for the **same** logical
   `frame`. With a 7-frame phase gap, host's frame 61 and client's frame 68 hold
   the same timeline position, so each peer flags its own numbered frame for the
   same divergence.

4. `ApplyScheduledDelayIfReady()` (`UMVC3Hook/code/controller.cpp:891-903`) changes
   *when* the first compare runs but does not rebase frame labels, so it does not
   create the offset — it only affects which frame is checked first.

## Why this is not a real desync

A GGPO-style confirmed frontier should be shared: `g_confirmedChecksumThrough`
advances only on frames both peers have input for. A 7-frame gap in `newestChecked`
is too large to be normal prediction lead and indicates the confirmed input exchange
itself is phase-offset. That is a frame-accounting bug, not gameplay-state
divergence — consistent with the f83 pair's zero in-dump divergence.

## Proposed fix

Establish the logical frame epoch **symmetrically** instead of from each peer's
independent native counter. Options:

- Anchor logical frame 0 to a shared value exchanged in the barrier handshake (e.g.
  piggyback a start-frame/epoch value alongside the round seed, host-authoritative),
  so both peers rebase onto the same logical 0.
- Or, after mutual release, have both peers wait for and adopt a common native base
  broadcast by the host before any input/checksum exchange begins.

Validation: after the fix, a forced-restart session should log matching mismatch
frames on both peers (or none) rather than a 61-vs-68 split, and f83 should no
longer fire.

## Caveat

The f466 pair (both peers, frame 451, mirrored checksums) may still be a genuine
state desync and should be re-examined separately after the phase issue is resolved,
since phase offset can also mask/emulate a real divergence there.
