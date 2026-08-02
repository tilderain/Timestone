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
