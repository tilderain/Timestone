# Snapshot region reuse

`Rollback_SaveSnapshot` used to copy every region (about 1.8 MB) on every save.
`snapshot_reuse.cpp` skips regions whose bytes are already in the destination
buffer. The buffer layout does not change, so restore and the dump recorder are
unaffected.

## Mechanism

- Each reusable region has a version. It is bumped at a save whenever a signal
  says the region may have changed since the previous save.
- Each destination buffer (ring slot or snapdump history frame) remembers the
  address, offset, size and version of every region last written to it.
- A save copies a region unless the buffer already holds it at the current
  version.
- A restore puts live memory back to what the restored buffer holds. Each region
  the restore copied back byte for byte takes the version recorded in that
  buffer, so the replayed frames still match buffers saved at that version;
  any other region is bumped (`SnapshotReuse_BeginRestore` / `RegionRestored` /
  `EndRestore`). Bumping everything on a restore, as the first version did, left
  nothing reusable in a match with frequent rollbacks (0-11% measured).
- Every version is bumped when the region list is rebuilt
  (`SnapshotReuse_SetPlan`). A buffer that is freed or written by anything else
  is forgotten (`SnapshotReuse_ForgetBuffer`).

## Signals

Owners are character and weapon bodies; the "prefix" is the first 0x1450 bytes
(the `uActorGameModel` part), compared with a copy kept from the previous save.

| Region | Treated as changed when |
|---|---|
| Motion blend buffer of slot *m* | that slot's record (`+0x560 + m*0x180`, 0x180 bytes) changed |
| Joint states, joint indices, blended joints, motion channels | any byte of the owner's prefix changed, or, for a weapon, of its character's prefix |
| Material job table (`+0x1408..+0x1438`) | a job is active now (bit 0 of `+0xB64`), was active at the previous save, or a job's control block (`+0xB40` to the end of the entry) changed. This signal is incomplete, so a skip is always confirmed by comparing the bytes, in every mode |
| Everything else (bodies, command lines, collision, camera, ...) | always |

## Evidence and limits

The signals are observed correlations, not proofs. Replaying the rules over 18
host desync dumps (4,319 frames, 5.96 GB of region data) gave no case where a
signal said "unchanged" and the bytes differed, and 52% of all snapshot bytes
reused with the 16-slot ring.

Found in play on 2026-11-10 (validate mode, about 100,000 saves per side), and
the reason for the two qualifications in the table above:

- A 13-joint weapon's joint states changed (joint 1, `+0x20`) with its own
  prefix unchanged: the weapon follows the character's hand.
- Material job tables changed at entry `+0x40` with no job active and the
  control block unchanged. What writes it has not been identified.

Known gaps:

- A material job that starts and finishes inside one frame, or is reconfigured
  only outside its control block while inactive, is not seen.
- With `EnableSnapDumpWrites=1` snapshots are stored in the 255-frame dump
  history, so a region is reused only if it has not changed for 255 frames.
- The dumps are windows around desyncs, not a random sample of play.

## Modes

`SNAPSHOT_REUSE_MODE` in `snapshot_reuse.h`:

- `Off`: copy everything.
- `Validate` (default): compare every region a signal calls unchanged; on a
  difference log `SnapshotReuse: MISMATCH` and copy it. Saved bytes are always
  correct in this mode; it only saves the write.
- `Trust`: skip without comparing. Switch to this once `Validate` has stayed
  free of mismatches.

A summary line (`SnapshotReuse: saves=... reused=...% mismatches=...`) is logged
every 4096 saves. Tests: `tools/test_snapshot_reuse.py`.
