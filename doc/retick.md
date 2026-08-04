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

## Expected Gain From The Two Model Skips

The likely steady-state gain from suppressing the two presentation-oriented
model helpers is modest:

| Candidate | Estimated saving |
|---|---:|
| Camera visibility mask | 0.002-0.008 ms/frame total |
| Auxiliary render texture | 0.005-0.050 ms/frame normally |
| Combined normal estimate | 0.02-0.08 ms/frame |
| Render-heavy optimistic case | 0.10-0.15 ms/frame |

For a normal headless tick of `1.31 ms/frame`, the likely improvement is about
2-6%, with an optimistic state-dependent result around 8-11%.

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
