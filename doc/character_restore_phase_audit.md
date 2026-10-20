# Character Restore Copy Audit

Investigation, 2026-10-18. No production restore policy was changed. The measured
copy-kernel experiments are in
[`../tools/benchmark_character_restore.md`](../tools/benchmark_character_restore.md#layout-mechanism-experiments-2026-10-18).

## Decisions

- Generate fixed-offset/fixed-size copies only from an audited schema; the
  benchmark's oracle-assisted fixture masks are not a native field schema.
- Coalesce adjacent/overlapping copies only within one phase, with identical
  source provenance and no intervening reader/writer or conditional policy.
- Hoist bounds checks only for a validated immutable plan and separately checked
  source/destination capacities. Keep the existing short-buffer behavior.
- Do not add a dispatch cache solely for speed: the cheap indexed lookup tested
  here has no convincing measured penalty. Actual DTI resolution is not measured.
- Do not eliminate an early write just because a late write touches the same
  address. No cross-phase write deletion is approved by this bounded audit.

## Scalar Table Accounting

The runner now writes `scalar_range_audit.json` alongside every result. It parses
the actual extracted declarations, ignoring comments, and reports byte unions.
This is table accounting, not a complete tally of character restoration: it
excludes the broad body copy, embedded-record loops, references and native calls.

Current working-tree extraction:

| Table | Entries | Written Bytes | Unique Bytes | Repeated Bytes | Coalesced Entries |
| --- | ---: | ---: | ---: | ---: | ---: |
| `CCHR_SCALAR_RESTORE_RANGES` | 46 | 2368 | 2178 | 190 | 30 |
| `CCHR_POST_RESTORE_TAIL_RANGES` | 22 | 1213 | 1209 | 4 | 17 |

The two table masks overlap by 645 bytes across phases. That overlap is **not**
645 bytes proven removable. The early table is not a safe scalar-only schema:
it includes the shot reference at `+0x6428` and a global `+0x68F0` entry despite
concrete tails starting there. This audit does not legitimize those policies.

Examples of genuine same-phase overlap:

- Early `+0xA8/4` is contained in `+0x80/0x80`.
- Early `+0x4168/0x90` is contained in `+0x4120/0xD8`.
- Early and late `+0x6828/4` are each contained in their own phase's
  `+0x6810/0x24` entry.
- Early animation ranges `+0x1300/8` and `+0x1308/0x40` are adjacent.

Within each scalar-table loop, these are ordinary saved-byte copies with no
intervening native calls. Their union is equivalent for a full-sized buffer.
For short buffers it is not sufficient to replace adjacent entries with one
larger bounds-checked entry: that could skip a valid prefix when the larger
entry fails its bound. Use a validated full-buffer fast path and retain the
original per-entry fallback. Do not extend the same argument to calls and
conditional pointer/resource fixups between other copies.

The existing fixture oracle already produces maximal contiguous spans. The
coalescing experiment recovers that same union from intentionally fragmented
descriptors; its speedup over fragments is not an additional gain over the
existing oracle.

## Reconstruction Boundary

`Rollback_RestoreSnapshot` first copies regions and character state. Before its
late character pass it then performs collision-clone repair, allocation-pointer
repair, hit-node reset, stale-reference cleanup, shot/effect reconstruction and
material-state repair. The late pass is not immediately adjacent to the early
pass in the running game.

`RestoreShotAndEffectRecipes` includes virtual finalizers/destructors, native
factories, optional setup retries, collision construction, pending-unit flush,
model evaluation and effect factories. It also restores character shot links
before some of those operations. The second reference restore after late repair
therefore cannot be treated as a duplicate call without a separate audit.

| Repeated State | Current Purpose | Audit Decision |
| --- | --- | --- |
| `+0x1480/8` hip rate/offset | Saved action-placement parameters; status-mode application can overwrite them | Preserve both phases pending consumer proof |
| `+0x6350/0x10`, `+0x6360/0x10` | Existing combo/collision policy ranges; post-reconstruction correction | Keep; current field labels require reconciliation with the Ghidra layout, not inference from names |
| `+0x6430/0x2C` | Shot-group serials and consumed-hit latches | Keep while teardown/setup callbacks can affect owner state |
| `+0x64A0..+0x6710` payloads | Five target/attacker/damage/throw/parent records | Keep early relationships coherent with reconstruction, then restore saved payloads late |
| `+0x1518/0x18`, `+0x2AD8/0x18`, `+0x3F84/0x18` | Flags, command state and animation/EP state | No all-callback read/write proof; keep |
| `+0x3BF0`, `+0x3D60` constraint payloads and validated links | Joint constraints used by collision and native mode setup | Keep; do not merge through vtables/target/interior/callback pointers |
| Capsule payloads, `+0x4D10/0x100` history | Saved collision geometry and history after native setup | Keep; documented desync history is a reason to test, not proof of a removable phase |
| `+0x680D`, `+0x6810`, `+0x6840`, `+0x68C0`, `+0x68D0` | Throw/callback latches, matrices and late scalar repair | No complete native-consumer proof; keep |
| `+0x68F0/4` | Existing global tail write | Must become a per-DTI policy before any phase-removal decision |

The empty late sphere-bank loop at the current source's `+0x4E20/+0x5530/+0x5C40`
does not copy anything. It is not a counted write-saving opportunity and an
optimizing compiler can eliminate it.

## Direct Native Evidence

Inspected Ghidra instance: `ichneumon_sub/umvc3.exe.unpacked.exe`, image base
`0x140000000`. Types were applied where the current parameter layout was known.

- `sShot__spawnShot`, `0x140150E00`, binds owner/target/resource, initializes
  attachment state and inserts the shot. Its resource and setup calls are not
  pure allocation operations.
- `uShot__initFromShotData`, `0x1401CD9A0`, calls
  `cArm__initMotionController`, which samples attachment parents and consumes
  randomization before constructing shot matrices. Moving all character restore
  to the end could change reconstruction even if the final character bytes match.
- `cAtkCtrl__computeShootAttachmentPose`, `0x14014FB60`, is actually passed a
  `cArm*` by that initializer. After applying that type, the native code reads
  attachment-parent/target matrices or joint transforms and dispatches position
  and rotation callbacks. The current name must not imply a `cAtkCtrl` layout.
- `uShotVulcanNormalArrow__setup`, `0x140180FF0`, and
  `uShotLaserCrystal__setup`, `0x1401631B0`, read the owner's `mEtcF` to select
  particle volume. This is a concrete reconstruction-time character read, not
  proof that every late-table flag range has a consumer.
- `uShotVulcanCrossSword__setup`, `0x140159080`, writes Arthur's Cross Sword
  latch through its owner. Current `Rollback_RemapCharacterShotLinks` also
  derives this latch from reconstructed shots. A tail copy can interact with
  lifecycle callbacks; an arbitrary "all tails late" rule is invalid.
- `cChrStatus__setMode`, `0x140031810`, calls `FUN_14004D2E0` then applies hip
  rate/offset. With `uCharacter*` applied to the latter, it rebinds `mEyes`,
  `mCnsLook` and `mCnsNeck`, reads the target-character record at `+0x6490`, and
  updates constraint references. Xrefs include character setup/state callbacks.
  This demonstrates the side effects if invoked; this pass did **not** prove it
  is reached by every current reconstruction path.

These samples reject a blanket final-state-only proof. They do not establish a
complete consumer/writer map for hundreds of virtual subtype callbacks. In
particular, hip/collision/throw fields need path-specific checks, not a claim that
every known status-mode side effect occurs on every restore.

## Next Safe Optimization

For the future explicit production schema, normalize same-phase copies and use
one validated-size fast path first. Then evaluate a shared fixed-copy common
kernel with compact per-DTI tail handling. Fully duplicating the generated common
kernel for every character risks instruction-cache pressure: the eight synthetic
layout pairs measured 219,725 portable bytes of function text.

Before removing a cross-phase copy, instrument the real reconstruction boundary
to find changed fields, audit the selected subtype's intermediate readers, and
test both intermediate invariants and replay results. Observing no writes does
not prove no reads; observing a write does not prove the early saved value was
unnecessary. The standalone benchmark's empty reconstruction interval cannot
answer either question.
