# Character Tick Re-simulation Performance Investigation

## Status

Current investigation based on Ghidra analysis of
`ichneumon_sub/umvc3.exe.unpacked.exe` and a 49-window rollback profile with
266 replayed frames. Addresses use the preferred image base `0x140000000` and
must be relocated through `_addr()` at runtime.

The findings below identify likely presentation-only work inside character
movement, explain the large state-dependent `uRyu` timing changes, and set
realistic optimization expectations. Candidate skips are not considered proven
until live-versus-resimulation state comparison passes representative tests.

## Profile Summary

The analyzed capture reported:

| Metric | Value |
|---|---:|
| Rollback windows | 49 |
| Frames replayed | 266 |
| Average rollback depth | 5.4 frames |
| Average rollback cost | 13.64 ms |
| P95 rollback cost | 22.99 ms |
| Average restore | 1.39 ms |
| Average post-resimulation save | 1.30 ms |
| Aggregate restore share | 10.2% |
| Aggregate snapshot share | 14.4% |
| Aggregate native-tick share | 75.1% |

Restore and snapshot performance are no longer the principal issue in this
capture. Native simulation, particularly category 8 character movement,
dominates both average cost and the slow tail.

## Headless Character Dispatch

`Rollback_HeadlessTick()` performs filtered `sUnit` passes. For character
objects, the relevant callbacks are:

| Pass | Object vtable offset | Character implementation |
|---|---:|---|
| Main movement | `+0x40` | Character-specific tick, such as `uRyu__tick` |
| Sync phase 2 | `+0x50` | `uCharacter__sync_base` or override |
| Sync phase 3 | `+0x48` | `uCharacter__moveAfter` or override |

The `+0x48` use in the phase-three pass must not be confused with the native
draw traversal. `uCharacter__draw` is at `0x14004BB80`, but ordinary character
model submission is already omitted because the headless tick does not run the
native character draw traversal.

Consequently, the large `uRyu` movement measurement is not the cost of calling
`uCharacter__draw`. It is work embedded in `uRyu__tick` and its call to
`uCharacter__move_base`.

## uRyu Tick

`uRyu__tick` is at `0x1400C84F0`. It begins by calling
`uCharacter__move_base` at `0x14004BD30`, then performs a small Ryu-specific
tail.

The Ryu-specific tail contains:

1. Beam/effect lifecycle handling through `uRyu + 0x6918`.
2. `mShinkuState` handling at `uRyu + 0x6920`.
3. A sound invocation through `sGSound__invokeUnderLock_A5E0`.
4. Action 227 voice sequencing using `mRanbuVoiceTimer` at `+0x6938` and
   `mRanbuVoiceIndex` at `+0x693C`.
5. Three random positional offsets and random voice selection.

The complete Ryu-specific tail cannot be skipped safely:

- Beam and effect references participate in native lifecycle and attachment
  ownership.
- The timer and index are persistent character state.
- `MtXorShift__floatInRange` advances RNG three times when a voice event fires.
- `sGSound__selectRandomVoiceVariant` also consumes global RNG.

Sound enqueueing can be suppressed only while preserving timer, index, RNG,
effect-lifecycle, reference-cleanup, and locking behavior.

## Presentation Work Inside uCharacter Movement

Near the end of the full path in `uCharacter__move_base`, call site
`0x14004C2E4` invokes `uModel__move` at `0x140566580`. Its top-level sequence
is:

```text
uModel__updateCameraVisibilityMask
uModel__updateAuxRenderTexture
uModel__evaluateMotionFrame
virtual callback +0x90
virtual callback +0x98
```

The first two functions are presentation-oriented candidates. The remaining
model work is gameplay-relevant and must not be removed as a block.

### Camera visibility mask

`uModel__updateCameraVisibilityMask` at `0x1405641A0` computes a model's
per-camera visibility mask from model render flags and the enabled state of the
eight active camera slots. Its observed writes are visibility fields in model
render/transform state. No action, input, RNG, position, or collision output was
identified.

This is a reasonable candidate for an early return while
`g_isResimulating == true`, subject to state-delta validation.

### Auxiliary render texture

`uModel__updateAuxRenderTexture` at `0x14056A860` is the stronger candidate. It:

- Reads the active render context from `g_sRender__mpInstance + 0x67648`.
- Allocates or releases an `MtTexture` depending on a model render flag.
- Walks model joints and writes values into texture staging data.
- Updates the per-render-context texture resource.

No gameplay output was observed. It is a strong candidate for a resimulation
early return.

### Required model work

Do not skip `uModel__move` itself. `uModel__evaluateMotionFrame` dispatches:

- Active motion blending.
- Motion frame advancement.
- Completion and event-mask updates.
- Layered or simple joint-transform evaluation.
- Motion-channel sampling.

Character actions, root motion, collision geometry, hurtboxes, attack spheres,
throws, and joint-attached projectiles consume this state. Render-adjacent field
names do not prove presentation-only ownership; world matrices and model joints
are part of deterministic combat simulation.

### Aggregate model bounds

The final call inside `uModel__updateJointMatrix` was previously named
`sRender__playbackDrawListsInner`. Ghidra analysis shows that it is
`uModel__updateBoundsFromJoints` at `0x140568F30`.

This function scans the completed joint matrices and the model resource's joint
bounding spheres. It writes only derived aggregate model state:

- Center and radius at `uModel + 0x1B0..+0x1BF`.
- Derived bounds matrices at `uModel + 0x1C0..+0x1FF`.
- Half extents at `uModel + 0x200..+0x20F`.
- Cached root position at `uModel + 0x210..+0x218`.

`uModel__submitForRender` and `uModel__cullVs2FrustumCascade` consume these
fields for visibility and frustum decisions. The later character push, attack,
hurt, attachment, and projectile collision passes instead consume the world and
joint matrices that were already completed before this call. The character
camera-bound classifier also uses `uCoord` position and camera planes rather
than the aggregate model bounds.

Two additional structural checks support this classification:

- `uModel__updateBoneMatrices` already accepts bit `1` as a native request to
  skip this bounds refresh while still evaluating the required joint matrices.
- Character restore intentionally keeps `cChr + 0x80..+0x55F` on the live
  presentation timeline. That range contains the visibility and aggregate-bound
  fields rather than restoring them as deterministic character state.

This makes the bounds scan a strong presentation-only coalescing candidate. It
should not be removed permanently: defer it while resimulating, remember each
model that reached it, and execute it once from the final corrected joint state
before the first post-rollback draw. This preserves exact final render bounds
while replacing N per-joint scans with one.

### Safe coalescing shape

The three model helpers can use the same fixed-capacity deferred-model list:

1. During `g_isResimulating`, record the model and return from
   `uModel__updateCameraVisibilityMask`, `uModel__updateAuxRenderTexture`, or
   `uModel__updateBoundsFromJoints`.
2. After the final replayed frame and after `g_isResimulating` becomes false,
   discard dead/replaced model pointers.
3. Run each deferred helper once for every surviving model before rendering.

Coalescing is safer than an unconditional resimulation early return. A pure
skip can leave visibility, texture data, or model bounds on the prediction
timeline for the first corrected draw. None of these helpers has temporal
gameplay state, so final-state recomputation is sufficient.

The embedded `cChrEyes__update` is not in this group. Although eye motion is
visual, it advances smoothed gaze-controller state and mutates eye and child
joint transforms. Replaying only its final call is not equivalent to replaying
all intermediate updates. Material-mode commands are also unsafe to discard:
an intermediate command can establish the persistent material state required by
the final corrected frame.

## Embedded Sound Candidates

Character movement invokes sound services even though the outer `sSound__move`
manager update is already suppressed during resimulation.

### Voice parameter update

`FUN_1402B1280` is called from `uCharacter__move_base` at `0x14004C17F`. It
locks `sGSound`, updates voice parameters, stores sound-manager state, and has no
observed character gameplay output. It is a strong resimulation-suppression
candidate.

### Voice invocation

`FUN_1402A1990` is the lower-level voice invocation used by
`sGSound__invokeUnderLock_A5E0`. Suppressing this lower-level operation is safer
than returning from the outer wrapper because the wrapper still performs its
critical-section and `cRef` cleanup.

This target is shared by many character-specific functions and could avoid
character-by-character sound patches. It must still be tested for hidden state
that affects deterministic RNG or subsequent voice-selection behavior.

Ryu's action-227 path requires additional care: suppressing the final enqueue
does not permit skipping the preceding timer, index, positional RNG, or random
voice-selection work.

## uCharacterPost Movement

`uCharacterPost__move` at `0x14005E2E0` contains no render or presentation-only
work. It validates two cached character pointers, walks both teams, calls
`uCharacter__movePost` at `0x14004B680` for every character, and then calls
`sReplay__processPerFrame` at `0x1401CEFD0`.

`uCharacter__movePost` is collision work despite its late placement. It resets
the character collision object's move state, updates as many as 16 embedded
collision primitives, and queues active objects and contact callbacks with
`sCollision`. Skipping it would change collision registration and contact
results. `sReplay__processPerFrame` advances recording/playback counters and can
write replay input, so it is also simulation state rather than presentation.

The estimated removable presentation cost in `uCharacterPost__move` is
therefore `0.000 ms/frame`. Any measurable category-18 move cost belongs to
collision and replay processing, not rendering.

## Cause Of uRyu Timing Spikes

The profile shows two distinct character-movement regimes.

### Normal window

For rollback `75-82`:

| Measurement | Value |
|---|---:|
| Frames | 7 |
| Total headless tick | 1.31 ms/frame |
| Unit movement | 1.04 ms/frame |
| `uRyu` movement | 0.917 ms/frame |
| `uRyu` callbacks | 42, or 6/frame |
| `process_frame_input` calls | 14, or 2/frame |

All six character objects receive their `uRyu__tick` callback, but only two
reach `cChrInput__processFrameInput` and the full active-character path.

### Slow window

For rollback `29-34`:

| Measurement | Value |
|---|---:|
| Frames | 5 |
| Total headless tick | 3.53 ms/frame |
| Unit movement | 3.09 ms/frame |
| `uRyu` movement | 2.959 ms/frame |
| `uRyu` callbacks | 30, or 6/frame |
| `process_frame_input` calls | 30, or 6/frame |

In this window, all six characters reach the full path. The timing ratio closely
matches the active-character ratio:

```text
2.959 / 0.917 = 3.23x measured uRyu cost
6 / 2           = 3.00x full-path character count
```

This is strong evidence that the spike is caused by four reserve characters
entering the full movement path, not by an occasional render submission or a
single pathological Ryu-specific callback.

The approximate per-character model is:

```text
Full active Ryu movement: approximately 0.46-0.50 ms
Inactive/reserve movement: approximately 0.00-0.02 ms
```

Six full updates account for almost the complete measured `2.959 ms/frame`.

## Likely State Transition

Slow windows cluster early in the match:

```text
13-18: 16.48 ms native tick total
29-34: 17.65 ms native tick total
54-61: 19.24 ms native tick total
```

Later windows commonly fall to approximately `7.5-10.5 ms` of native tick per
rollback. A separate portion of the capture shows a similar transition around
frames `60-75`.

This pattern is consistent with battle-start or intro initialization, during
which all six team members are active enough to execute full movement. It may
also occur during team changes, snapback, DHC, cinematic, or other multi-character
states, but the current evidence most clearly identifies an opening-state
transition.

The shared predicate currently named `uDrStrangeSh__stateCheck_6290` controls a
major branch near the start of `uCharacter__move_base`. The name is misleading;
it is used by the shared character implementation. Depending on this and related
team-change/busy checks, reserve characters either initialize input and reach a
cheap finalization path or continue through the full path containing:

- Frame input processing.
- Command matcher updates.
- Action dispatch.
- Motion and joint evaluation.
- Position integration.
- Collision attachment updates.
- Push, attack, and hurt-sphere rebuilding.

The existing `process_frame_input` hook count is therefore a useful proxy for
the number of full-path characters in each replay window.

## Expected Gain From Presentation Skips

The likely steady-state gain from suppressing the two presentation-oriented
model helpers is modest:

| Candidate | Estimated saving |
|---|---:|
| Camera visibility mask | 0.002-0.008 ms/frame total |
| Auxiliary render texture | 0.0008-0.0021 ms/frame observed |
| Combined model-helper estimate | 0.003-0.010 ms/frame |
| Unsampled allocation/upload case | Unknown |

These are profile-derived static estimates rather than isolated microbenchmarks.
With the normal two-character full path, the camera estimate implies
approximately `0.001-0.004 ms` per camera-mask call. The measured
auxiliary-texture helper averaged approximately `0.0003 ms` per call. It remains
state-dependent: its cheap branch only checks or releases a context resource,
while its expensive branch can allocate a texture, walk every model joint, and
upload staging data.

The shared `sGSound` voice-parameter update is estimated at another
`0.001-0.010 ms` per call when its uncontended critical section reaches voice
lookup and parameter application. It can be cheaper when the stored parameter
already selects no update. For two full-path characters, a reasonable total is
`0.002-0.020 ms/frame`; for six full-path characters it is roughly
`0.006-0.060 ms/frame`. This range is less certain than the model estimates
because the supplied profile did not time this helper separately.

Combining all three shared presentation candidates gives the following working
budget:

| State | Estimated presentation-only time |
|---|---:|
| One full-path character | 0.002-0.015 ms/frame |
| Normal two-character full path | 0.005-0.030 ms/frame |
| Six-character full path | 0.015-0.090 ms/frame |
| `uCharacterPost__move` | 0.000 ms/frame removable |

### Current resimulation behavior

None of the three shared candidates above is currently skipped at its own call
site:

| Candidate | Skipped during resimulation? | Current path |
|---|---|---|
| `uModel__updateCameraVisibilityMask` | No | `uCharacter__move_base` calls the unhooked `0x1402021D0 -> 0x140200CB0` model thunk, which calls this helper unconditionally. |
| `uModel__updateAuxRenderTexture` | No | The same unhooked model thunk calls it immediately after visibility-mask maintenance. |
| `uModel__updateBoundsFromJoints` | No | Character vtable `+0x98` reaches `uCharacter__updateConstraints -> uActorShareModel__updateJointMatricesAndConstraints -> uModel__updateJointMatrix`, which performs this per-joint bounds scan after the required joint update. |
| `FUN_1402B1280` voice-parameter update | No | The wrapper still locks `sGSound`, resolves voice references, applies parameters, and stores its per-character value. Separate hooks suppress voice creation, audible starts, and `sSound__move`, but not this update. |
| `uCharacterPost__move` | No | Category 18 is enabled in the headless move pass, so the coordinator and its collision/replay children run. No presentation-only child work was identified. |

There is an existing, separate presentation skip at `0x1402021E0`.
`hkCharacterModelPostMove` in `rollback_runtime.cpp` returns during resimulation
instead of running the `uCharacter__moveAfter` tail that updates toon materials
and cut-in lighting. This is not the `0x1402021D0` model-movement thunk containing
the visibility and auxiliary-texture helpers; the adjacent addresses dispatch
different native functions.

The high ends cover auxiliary-texture allocation/upload and are not expected on
every frame. Direct QPC scopes around the three helpers are required for tighter
hardware-specific numbers; at these durations, profiler overhead should be
measured with an empty scope and subtracted.

### Auxiliary-texture measurement

A temporary QPC hook measured nine eight-frame rollback windows. Across all
windows, `uModel__updateAuxRenderTexture` received 248 calls and consumed
`0.0759 ms` total:

| Measurement | Observed value |
|---|---:|
| Aggregate per replayed frame | `0.00105 ms` |
| Average call | `0.000306 ms` |
| Largest observed call | `0.0007 ms` |
| Normal call count | 3 calls/frame |
| Opening/high-activity call count | 7 calls/frame |

Normal windows ranged from `0.0008-0.00115 ms/frame`; the seven-call opening
window measured `0.00204 ms/frame`. These numbers are an order of magnitude
below the earlier static estimate and are consistent with the cheap
resource-check path rather than meaningful texture allocation or joint-data
upload. QPC overhead is included, so the native cost may be lower still. The
helper is not a useful optimization target in these states; a branch counter
would be needed to prove whether a rare expensive allocation/upload path occurs
in other matchups.

### Aggregate-bounds measurement

The profiling-only `model_bounds_from_joints` hook measured the complete native
function in the 2,000-fighting-frame session
`nplive_20260823_011313_{host,client}`:

| Measurement | Host | Client |
|---|---:|---:|
| Rollback windows | 156 | 191 |
| Replayed frames | 1,127 | 1,374 |
| Bounds calls | 3,991 | 4,822 |
| Calls/replayed frame | 3.541 | 3.509 |
| Bounds total | 35.9048 ms | 43.0248 ms |
| Aggregate average/call | 8.996 us | 8.923 us |
| Window median average/call | 9.617 us | 10.063 us |
| Window p95 average/call | 13.157 us | 12.867 us |
| Bounds/replayed frame | 0.03186 ms | 0.03131 ms |
| Window median/replayed frame | 0.03181 ms | 0.03170 ms |
| Window p95/replayed frame | 0.04840 ms | 0.04646 ms |
| Share of unit movement | 1.99% | 3.02% |

The host recorded one `681.4 us` maximum in window `1832-1840`. Removing that
window leaves 3,967 calls averaging `8.803 us` with a `68.4 us` maximum, closely
matching the client. The isolated maximum is therefore consistent with a local
scheduling/preemption event rather than ordinary bounds work.

The measured normal cost is about three times the earlier static estimate and
is large enough to be a useful secondary optimization. Estimating one final
call per model per rollback window from each window's call density removes about
85.7-85.8% of calls. At the measured average this predicts savings of:

- Host: 30.78 ms over the run, or `0.0273 ms/replayed frame`.
- Client: 36.93 ms over the run, or `0.0269 ms/replayed frame`.
- Typical seven-frame correction: approximately `0.19 ms`.

The exact saving requires recording distinct model pointers because models can
appear or retire inside a rollback window. The close peer agreement is already
sufficient to prioritize a defer-and-flush implementation over the much cheaper
auxiliary-texture helper.

### Deferred-bounds candidate

The candidate implementation records each model that requests a bounds scan
during resimulation, skips duplicate intermediate scans, and refreshes each
surviving model once after the final corrected frame. A fixed 512-entry queue
avoids replay-time allocation and falls back to native execution if it fills.

Candidate profile:

```text
rollback_profile_runs/20260823T082359.701265Z_candidate_deferred_model_bounds
```

| Measurement | Baseline host | Candidate host | Baseline client | Candidate client |
|---|---:|---:|---:|---:|
| Replayed frames | 1,127 | 1,127 | 1,374 | 1,110 |
| Bounds calls/frame | 3.541 | 0.472 | 3.509 | 0.469 |
| Bounds ms/frame | 0.03186 | 0.00464 | 0.03131 | 0.00460 |
| Average executed call | 8.996 us | 9.832 us | 8.923 us | 9.804 us |

The candidate removed 86.6% of calls on both peers. Normalized bounds cost fell
by `0.02722 ms/replayed frame` on host and `0.02671 ms/replayed frame` on client,
matching the pre-implementation prediction. The remaining calls are the final
per-model refreshes, so their slightly higher average is consistent with a
different model mix rather than queue overhead.

Both peers completed the 2,000-fighting-frame run. The runner found no crash or
desync artifacts. Aggregate rollback time is not used as the acceptance signal
because the candidate and baseline followed different battle workloads; the
direct hook bucket provides the attributable comparison.

For a normal headless tick of `1.31 ms/frame`, the measured auxiliary-texture
helper accounts for approximately `0.08%`. The unmeasured camera and voice
candidates dominate the remaining working estimate.

For the seven-frame `75-82` rollback, the estimated saving is approximately
`0.14-0.56 ms`, reducing `13.19 ms` total to about `12.63-13.05 ms`.

The six-full-character state may execute these helpers three times as often,
but removing them will not eliminate the approximately `2 ms/frame` spike. The
larger opportunity is determining whether the four reserve full updates are
required or are caused by unnecessarily restored/advanced state.

## Performance Outlook

The result is not bleak for playable rollback:

- Average rollback cost is already below one 60 Hz presentation frame.
- Common seven-frame examples complete around 11-16 ms.
- Restore averages only 1.39 ms.
- Snapshot saves are around 0.3 ms per intermediate frame.
- Normal headless simulation can run near 1.3 ms per replayed frame.

The difficult target is a universal sub-2 ms replay tick across every state.
Slow multi-character states currently reach 3-4 ms per replayed frame and are
mostly executing legitimate native character work.

A realistic target without replacing native character simulation is:

| State | Expected target |
|---|---:|
| Normal replay tick | 1.0-1.3 ms/frame |
| Busy combat replay tick | 1.5-2.5 ms/frame |
| Six-full-character tick | 2.5-3.5 ms/frame unless specialized |
| Common three-frame rollback | 5-7 ms total |
| Common five-frame rollback | 8-12 ms total |
| Common seven-frame rollback | 11-16 ms total |
| Pathological rollback | 20-30 ms total |

The project is already near a practical target for common corrections. A
cumulative 15-30% native-tick reduction still appears plausible through several
focused exclusions and state-path fixes. Another easy twofold reduction does
not.

## Recommended Next Measurements

Instrument `uCharacter__move_base` rather than continuing to treat the entire
`uRyu` callback as one bucket. Record, per character and frame:

- Character pointer, team, and slot.
- Action ID at `uCharacter + 0x1310`.
- Result of the shared active-state predicate.
- Whether `cChrInput__processFrameInput` was reached.
- Time in input and command matching.
- Time in action dispatch.
- Time in `uModel__move`.
- Separate time in `uModel__updateCameraVisibilityMask`.
- Separate time in `uModel__updateAuxRenderTexture`.
- Separate time in `uModel__updateBoundsFromJoints`.
- Time in collision sphere and geometry rebuilding.
- Time in finalization callbacks.

The central correctness question is whether all six full updates during opening
resimulation are required native setup or whether reserve-character state is
being restored or advanced incorrectly. Avoiding four unnecessary full updates
would save approximately `1.8-2.0 ms` per replayed frame in the observed slow
windows, much more than presentation suppression alone.

## Validation Requirements

Enable one candidate skip at a time and compare live versus resimulated state.
At minimum, test:

- Match start and intro transitions.
- Standing, movement, normals, and command moves.
- Throws and throw capture.
- Assists, tags, DHC, snapback, and reserve activation.
- Cinematic and side-camera actions.
- Beam and joint-attached projectiles.
- Weapon visibility and weapon creation/destruction.
- Install and transformation states.
- Hitstop, blockstun, and action-time freeze.

Compare character fields, collision geometry, action IDs, positions, RNG state,
shot topology, and final ground-truth checksums. Presentation-only differences
may be acceptable during replay, but gameplay state and RNG consumption must
remain identical.

## Assist And Reserve Hot-Path Gate

Further Ghidra analysis established the exact branch that decides whether a
character executes the full `uCharacter__move_base` path.

The previously named `uDrStrangeSh__stateCheck_6290` at `0x140056290` has been
renamed in Ghidra to `cChr__isMoveActive`. It returns true only when all three
conditions hold:

```text
MtObject alive-state                         != 0
(cUnit + 0x10) lifecycle state               == 2
(cUnit + 0x10) callback/move-enable bit 0x400 != 0
(cChr + 0x14F0) mChrF bit 0                  == 0
```

Its native implementation is effectively:

```c
bool cChr__isMoveActive(cChr* chr)
{
    return MtObject__isAliveState(chr) &&
           ((chr->cUnitFlags & 7) == 2) &&
           ((chr->cUnitFlags & 0x400) != 0) &&
           ((chr->mChrF & 1) == 0);
}
```

This predicate does not mean "is the point character." A reserve can remain a
live category-8 unit with movement enabled while still being benched. The game
uses additional character and team flags to select the cheap reserve path.

At `0x14004BDE5`, `uCharacter__move_base` applies this decision:

```text
if cChr__isMoveActive(character):
    enter full path
else:
    clear stale team-change state
    busy = cChr__checkBusyState_6380(character)
    transitionEligible = cChr__canPerformTeamChange(character)

    if !transitionEligible || busy:
        initialize input buffer
        exit through finalization

    enter full path
```

The exact full-path condition is therefore:

```text
cChr__isMoveActive(character) ||
(!cChr__checkBusyState_6380(character) &&
 cChr__canPerformTeamChange(character))
```

This corrects the earlier interpretation that `cChr__isMoveActive` alone was
the active/reserve gate.

### Reserve transition exception

`cChr__canPerformTeamChange` at `0x140038AF0` is the mechanism that can
intentionally send a non-active reserve into the expensive path. It first
requires:

- Team cached active animation flags `cChrTeam::mCAnmF` bit `0x2000` set.
- The character's `cChr + 0x14F0` byte bit 7 clear.

It then chooses a reserve by current DHC state:

```text
if team.mDHCCount == 0:
    member 1 is eligible if team.mAliveMember bit 1 is set
else if team.mDHCCount == 1 and the team transition count is at least 3:
    member 2 is eligible
```

The character's current member ID is read from `cChr + 0x2DB0`. Ghidra's old
generic getter at `0x140004800` has been renamed to `cChr__getMemberId`.

This is not necessarily erroneous native behavior. The reserve is allowed to
run full input, action, animation, and collision processing so it can enter a
tag, DHC, or related team transition. The anomalous case is when this exception
remains true for characters that are no longer pending entry.

### State feeding the exception

The hot-path exception depends on state from several owners:

| Owner | Field | Role |
|---|---:|---|
| `cChr` | `+0x10` | cUnit lifecycle and movement-enable state |
| `cChr` | `+0x14F0` | `mChrF`; bit 0 suppresses `cChr__isMoveActive`, byte bit 7 affects transition eligibility |
| `cChr` | `+0x2DB0` | Current member ID, compared with 1 or 2 |
| `cChrTeam` | `+0x48` | `mpCurrentChr` active-character pointer |
| `cChrTeam` | `+0x60` | `mAliveMember` bitmask |
| `cChrTeam` | `+0xBC` | `mDHCCount` |
| `cChrTeam` | `+0xF8` | Cached active-character `mCAnmF`; bit `0x2000` opens the reserve exception |
| `cChrTeam` | `+0xA0/+0xA4` | Pending current/next member IDs |
| `sCharacter` | `+0x5C0..+0x5FF` | Two 0x20-byte delayed change-order records |

`cChrTeam__updateAggregateState` at `0x140022870` recomputes team aggregates
after character callbacks. In particular, it copies the active character's
`mAnmF` into `team.mCAnmF`, rebuilds `mAliveMember`, and computes
`mChangeEnable`, `mChangeEnable2`, and `mAssist`. This means the team fields used
by the next character tick are derived late in the previous frame.

### Swap and assist transition pipeline

Tag and assist progression is distributed across several stages:

1. `sCharacterChangeOrder__requestOrApplySwap` at `0x14001F690` either applies
   an immediate transition or fills one 0x20-byte delayed change-order record.
2. `sCharacterChangeOrder__advance` at `0x14001F860` advances its two timers,
   checks assist-entry delays and action/game flags, and eventually publishes a
   pending swap.
3. `cChrTeam__setPendingCharacterSwap` stores swap mode and current/next member
   IDs at team `+0x9C/+0xA0/+0xA4`.
4. `cChrTeam__commitPendingCharacterSwap` at `0x140021510` swaps member IDs,
   updates `mpCurrentChr`, clears the pending IDs, and resets character message
   records.
5. `cChrTeam__updateAggregateState` later rebuilds the cached team state used by
   the next tick.

The delayed change-order records are legitimately long-lived during assist
entry. Mode 5 specifically consults assist-entry and assist-unlock delays and
can keep a reserve transition pending while action flags permit it.

### Likely rollback failure modes

Current source restores all of the obvious scalar inputs:

- Character `+0x14F0..+0x1517` is in `CCHR_SCALAR_RESTORE_RANGES`.
- Character identity state `+0x2D9C..+0x2DBF` includes member ID.
- Team `+0x50..+0x10F` includes `mAliveMember`, pending swap IDs,
  `mDHCCount`, and `mCAnmF`.
- `sCharacter +0x5C0..+0x5FF` captures both change-order records.
- Character cUnit `+0x10` follows the saved frame, while intrusive links remain
  live.

Therefore the current evidence does not identify a simple missing snapshot
range. More plausible causes are:

1. **Legitimate historical state.** The restored frame is inside assist, tag,
   DHC, or opening initialization, so reserves are supposed to run fully.
2. **Mixed-frame team cache.** `team.mCAnmF`, `mAliveMember`, or pending IDs are
   restored correctly initially but are recomputed from character state that
   has already diverged or is processed in a different order during replay.
3. **Change-order replay.** A restored delayed record advances again because its
   timer, mode, or publication boundary is inconsistent with the restored team
   pending IDs.
4. **Member-ID/active-pointer disagreement.** `cChrTeam::mpCurrentChr`, character
   `mMemberId`, and pending IDs describe different transition phases. The
   restore validates the active pointer against the live roster but does not
   otherwise normalize these fields, which is correct unless the saved tuple is
   already internally inconsistent.
5. **Busy-check disagreement.** `cChr__checkBusyState_6380` also reads team
   ability aggregates, `mChrF`, and character status state. A false busy result
   combined with stale transition eligibility opens the hot path.
6. **Opening phase behavior.** `sAction__stepFightingCore` intentionally resets
   all fighters, chooses initial active members, advances both change-order
   records, commits pending swaps, and applies status to every roster member.
   The early slow windows may therefore be correct initialization behavior, not
   erroneous assist activation.

### Diagnostic needed to prove the cause

Profile each category-8 character immediately before the gate with:

```text
frame, team, character pointer
member ID at +0x2DB0
is team.mpCurrentChr
cUnit flags at +0x10
mChrF at +0x14F0
isMoveActive result
busy-check result
canPerformTeamChange result
team.mAliveMember at +0x60
team.mDHCCount at +0xBC
team.mCAnmF at +0xF8
team pending mode/IDs at +0x9C/+0xA0/+0xA4
change-order mode/timers/IDs at sCharacter+0x5C0 and +0x5E0
```

Record the same tuple on the original timeline and on resimulation. The first
field that differs identifies the responsible boundary:

- Different at restore completion: restore inconsistency.
- Same after restore but different before the first character tick: ordering or
  pre-character manager work.
- Same at gate with all six eligible on both paths: legitimate native state.
- Same active predicate but different busy/transition result: ability/status or
  team-cache divergence.

Do not optimize by checking only `character != team.mpCurrentChr`. Assists and
incoming tag/DHC characters must run the full path before they become the active
pointer. Any fast path needs to prove that no change-order record, pending swap,
assist flag, tag action, or transition eligibility references that reserve.
