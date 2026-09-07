# uActionSideCamera

`uActionSideCamera` is the slot-0 battle camera unit used for ordinary side
tracking, throw tracking, scripted camera commands, and Maya-motion cinematic
cameras. The object has DTI size `0x590` and is referenced by
`sCamera + 0x58` (viewport slot 0 at `sCamera + 0x50`, camera pointer at slot
`+0x08`). Restoring `sCamera` does not by itself restore this unit's internal
camera animation state.

## Update order

The main update is `uActionSideCamera__move @ 0x14001AA20`, dispatched through
the category-3 `sUnit` move pass. Its relevant call sequence is:

```text
uActionSideCamera__move
  -> g_update @ 0x140019EF0
  -> uActionSideCamera__updateMayaMotionOrTracks @ 0x14001C0B0
       -> cMayaMotionWork__advance @ 0x140015EC0
```

`g_update` selects the characters tracked by the camera. During throw/capture
state it follows `cChr__getThrowChrInfo()` from the thrower to the captured
fighter, making the normal side-camera unit responsible for throw tracking.
There is no separate throw-camera unit.

After the category-3 move, the phase-3 callback
`uCamera__getBoundary @ 0x140545F70` refreshes the matching `sCamera` viewport
and frustum. Later, `sCamera__move @ 0x140532D80` derives render matrices from
the camera unit. The late callbacks cannot compensate for skipped or incorrectly
terminated motion in `uActionSideCamera__move`.

## Vibration leakage into gameplay bounds

The slot-0 boundary planes at `sCamera+0x90..+0xCF` are not derived solely from
the logical `uActionSideCamera` position and target. Their producer chain is:

```text
sCamera__move @ 0x140532D80
  -> uActionSideCamera__getViewMatrix @ 0x14001A700
  -> when sVibration+0x840 is active:
       sVibration__applyCameraShakeToView @ 0x140524220
       reads slot-0 offsets at sVibration+0x820..+0x838
  -> caches the resulting view matrix at viewport+0xA0

uCamera__getBoundary @ 0x140545F70
  -> sCamera__refreshViewsForBoundary @ 0x140535C80
  -> sCameraViewport__rebuildFrustumPlanes @ 0x1405351D0
       reads viewport+0xA0..+0xD8, +0xE0, and +0xF4
       writes viewport+0x40..+0x9F
```

For viewport 0, `viewport+0x40` is `sCamera+0x90` and `viewport+0xA0` is
`sCamera+0xF0`. `uCharacter__classifyCameraBounds @ 0x14004ECA0` later consumes
these planes as gameplay state.

This caused the Vergil ground Dimension Slash rollback desync. The logical
camera body and projection inputs were byte-identical, but the cached view
matrix contained different horizontal shake on each peer. At the failing
radius-50 comparison, the host plane dot was `47.5856` while the client was
`51.1809`. The resulting camera-boundary flag changed Vergil's direction and
gameplay RNG consumption.

The full `sCamera` snapshot does not solve this because each peer captures its
own presentation-derived matrix. `sVibration` is not snapshotted, and the
headless tick intentionally skips `sVibration::move`. Replacing the headless
tick with the original native main tick also reproduces the desync because it
starts from unrestored prediction-timeline vibration state.

The deterministic correction point is the boundary refresh, not gameplay RNG:
build its slot-0 frustum from `uActionSideCamera__getViewMatrix` without the
vibration transform. The later normal `sCamera__move` can still apply vibration
to the render view, preserving presentation while preventing camera shake from
affecting gameplay classification.

## Cinematic dispatcher

`uActionSideCamera__updateMayaMotionOrTracks @ 0x14001C0B0` implements the
scripted/throw cinematic state machine:

1. `mReqMayaMotion` at `+0x589` consumes the request block at `+0x3C0`.
2. It initializes the embedded `cMayaMotionWork` at `+0x230` and sets
   `mIsMayaMotion` at `+0x588`.
3. While active, `cMayaMotionWork__advance` advances the cinematic camera and
   the dispatcher returns true.
4. A true return suppresses ordinary side-camera tracking for that frame.
5. Completion at camera `+0x28C` clears `mIsMayaMotion`, resets the embedded
   work, and allows ordinary tracking to resume.

Animation command handlers install non-Maya tracking commands through
`uActionSideCamera__func_4020 @ 0x140014020` and
`uActionSideCamera__func_4150 @ 0x140014150`. These handlers resolve slot 0
from `sCamera`, configure the camera's embedded tracks, and mark the command
state active.

## Embedded cMayaMotionWork

The embedded object begins at camera `+0x230` and has size `0x190`.

| Camera offset | Work offset | Field or role |
|---|---|---|
| `+0x240` | `+0x10` | `mOffsetPos` |
| `+0x250` | `+0x20` | `mTrackPos` |
| `+0x260` | `+0x30` | `mTrackTarget` |
| `+0x270` | `+0x40` | `mZoom` |
| `+0x274` | `+0x44` | `mRoll` |
| `+0x278` | `+0x48` | `mCut` |
| `+0x27C` | `+0x4C` | `mFrame` |
| `+0x280` | `+0x50` | `mSpeed` |
| `+0x284` | `+0x54` | `mFovType` / evaluated state |
| `+0x288` | `+0x58` | tracking and repeat flags |
| `+0x28C` | `+0x5C` | completion flag |
| `+0x290` | `+0x60` | reference-counted camera-list resource |
| `+0x298` | `+0x68` | parent character pointer |
| `+0x2A0` | `+0x70` | parent node/index |
| `+0x360` | `+0x130` | interpolation duration |
| `+0x364` | `+0x134` | tracking option |
| `+0x370` | `+0x140` | cached parent matrix |
| `+0x3B0` | `+0x180` | hit-stop behavior |

## Pending request block

The request consumed when `mReqMayaMotion` is set occupies `+0x3C0..+0x3F4`.

| Offset | Role |
|---|---|
| `+0x3C0` | reference-counted camera-list resource |
| `+0x3C8` | camera cut/index |
| `+0x3D0` | parent character pointer |
| `+0x3D8` | parent node/index |
| `+0x3E0` | tracking offset |
| `+0x3F0` | interpolation duration |
| `+0x3F4` | request option copied to camera `+0x364` |

Activation releases and clears the pending request resource after transferring
it into the embedded work. Consequently, restoring `mReqMayaMotion` without
the request block can replay an already-consumed request.

## Rollback requirements

Ordinary throws do not necessarily use Maya motion. In the 20260802_055706
snapdump, `mIsMayaMotion` and `mReqMayaMotion` remain zero through the frame-4750
camera split. The throw instead uses the normal tracking path described above.

The camera also embeds `cAimInfo` at `+0xA0` with size `0x190`, ending at
`+0x22F`. Its initialized tail ends at `+0x224`; `+0x225..+0x22F` is padding.
The peer-specific values observed at `+0x228` in the 20260802_055706 dump are
therefore not camera state. The control bytes at `+0x220..+0x224` diverge at
frame 4737 before the normal throw-tracking update turns the mismatch into
different position and target values.

`cAimInfo::mMode` is at camera `+0xB0`. The original scalar ranges ended one
range at `+0xAF` and resumed at `+0xB8`, so this field was captured but not
restored. In the same dump it first differs at frame 4747 (`1` host, `0`
client), before `cAimInfo::mState` at `+0xA8` differs at frame 4750 and the
client camera stops advancing.

The cinematic mode flags and their controlled state form one atomic rollback
unit. Restoring `+0x588/+0x589` without the embedded work can produce either:

```text
mIsMayaMotion = 1, camera+0x28C = 1
```

which immediately terminates the restored throw camera, or:

```text
mReqMayaMotion = 1, request block already consumed
```

which initializes motion from future or cleared request data.

Rollback therefore restores `cAimInfo::mMode`, the initialized `cAimInfo` tail
through `+0x224`, followed by the ranges `+0x230..+0x28F`,
`+0x2A0..+0x3BF`, `+0x3C8..+0x3CF`, and `+0x3D8..+0x3F7` together with the
existing camera scalars and mode flags. The resources at `+0x290` and `+0x3C0`
are pinned per snapshot with native reference counting. Parent pointers at
`+0x298` and `+0x3D0` are restored only when null, unchanged, or still present
in the live team roster.

## Additional Restore Gaps: Static Audit

The September 6 audit distinguishes a reachable rollback failure from a
runtime-confirmed desync. No new reproduction was run for the gaps below.

### Position Attachment Joint

Camera `+0xE8` is `cAimInfo::mPosJoint`, a signed 32-bit joint selector.
`anmchrCmd0_interpreter @ 0x140100DE0`, opcode `0x33`, configures the position
track, writes its parent at camera `+0xE0`, then writes the joint through
`MtObject__setField_72 @ 0x1400146E0` (call site `0x14010259D`). The receiver
is the embedded `cAimInfo` at camera `+0xA0`.

The position-track evaluator `0x14001BB60` consumes that joint on subsequent
camera moves. A negative index selects the parent's local transform; a
nonnegative index calls its joint-matrix accessor at vtable `+0xA0`. The joint
also determines the mirrored axis. The evaluator does not reset this field.

The previous restore copied the parent and track but omitted the joint: the range
`+0xB8/0x30` ends immediately before `+0xE8`. Rolling back from a later joint
assignment to an already-active earlier track therefore mixes old track state
with the future joint. Replaying a command is not sufficient when that command
preceded the restored frame. The resulting logical camera feeds camera-relative
facing (`0x140051040`) and gameplay boundary classification. This is a concrete
static missing-state failure path; no affected move has yet been reproduced.
Restore now explicitly includes `+0xE8/4`, without copying adjacent padding.

### View Attachments

`uActionSideCamera__getViewMatrix @ 0x14001A700` directly reads position
attachment `+0x410` / joint `+0x418` and target attachment `+0x420` / joint
`+0x428` on the non-Maya path. Both joint indices initialize to `-1`, both
references to null. This entire block is omitted from explicit restore.

Follow-up writer research found no non-constructor writer established as
operating on this camera block. Direct-displacement instruction searches in the
unpacked executable were checked against Ghidra: similarly-offset stores in
`cChrTrace` copying (`0x140039FF0`), finish-screen objects (`0x14031A7A0`,
`0x14031BFB0`), character-select UI (`0x140375AC0`), vibration
(`0x1405229E0`), and stack-local resource tables are not camera writes.
This is not an exhaustive proof against computed-address or generic writes.

The known animation attachment commands do not populate this block. Opcode
`0x33` writes position parent/joint at `+0xE0/+0xE8`; opcode `0x39` writes
target parent/joint at `+0xD0/+0xD8`, all inside the embedded `cAimInfo`.
Those are consumed during track evaluation, whereas `+0x410..+0x42F` is an
additional transform applied when building the view matrix.

If only `+0x410` is non-null, its selected joint matrix transforms both camera
position and look-at target. If `+0x420` is also non-null, that second matrix
transforms the target independently. Maya-motion mode bypasses both. Each
joint field is four bytes; `+0x41C..+0x41F` and `+0x42C..+0x42F` appear to be
alignment padding. The reader dispatches directly through the attachment's
joint accessor with no liveness check. Neither the reader nor the camera
destructor (`0x140017800`) adds/releases these references, consistent with
borrowed coordinate/model pointers rather than owned resources. No concrete
attachment type or active lifetime is established without an activation writer.

Conclusion: apparently dormant in the reviewed battle paths, not a demonstrated
additional desync source. Omitted indices do not matter while both references
remain null. Leave restore unchanged unless a reachable activation path is
identified; do not add speculative pointer restoration for this block.

### Retained Tracking Character

`uActionSideCamera__selectTrackedCharacters @ 0x140019EF0` has another omitted
reference at `+0x4D0`. Under action flag `0x10000`, a non-null airborne-hitstun
candidate is stored there. If a later lookup yields no candidate, the retained
pointer is reused before throw-target and ordinary fallback selection. Thus a
prediction-only candidate can survive restore and select the wrong character.
Restore now reads the saved pointer from the camera body and accepts it only
when present in the current team roster. Null or invalid references clear the
slot instead of retaining a prediction-only selection. The native candidate
finder `cChrTeam__findChrInAirborneHitstun` traverses that same roster, so this
validation covers its candidates without accepting unrelated live units.
The audit does not establish how often this branch is exercised; this repair
still needs an in-game rerun.

By contrast, tracked-character output slots `+0x488/+0x490` are assigned on
each selector call through `0x14001BA50`; their omission alone is not proof of
a stale-state failure on that update path. `cAimInfo` references `+0xB8`,
`+0xD0`, and `+0xE0` are already raw-restored, so their outstanding issue is
reference validity rather than missing capture/restore.
