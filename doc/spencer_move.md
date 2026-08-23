# uCharacter and uSpencer Move Investigation

## Scope

This investigation correlates the current rollback profile with Ghidra analysis
of `ichneumon_sub/umvc3.exe.unpacked.exe`. Addresses use the preferred image
base `0x140000000`.

The measured category-8 callback includes the complete concrete character move
callback, not only character-specific code. For Spencer that callback is
`uSpencer__tick @ 0x1400D1A00`.

## Profile Evidence

Profile:

```text
rollback_profile_runs/20260823T073954.381616Z_candidate_command_region_metadata
```

| Side | Calls | Total | Average | Maximum |
|---|---:|---:|---:|---:|
| Client | 621 | 207.5400 ms | 334.203 us | 884.3 us |
| Host | 726 | 286.8493 ms | 395.109 us | 45.6769 ms |

Spencer accounts for 37.1% of client and 34.0% of host category-8 movement. It
accounts for approximately 31.5% and 30.2% of the complete headless unit-move
phase. The six roster characters all receive one category-8 callback per
replayed frame, but only about 2.8 characters per frame reach
`cChrInput__processFrameInput`. The others normally take the cheap reserve path.

Spencer and Viewtiful Joe are the two consistently expensive entries in this
roster, which is consistent with point/active-character execution rather than a
Spencer-only callback problem. Reserve characters still appear with the same
callback count, but usually skip the full simulation body.

## uSpencer Callback

Ghidra recovers `uSpencer__tick` as:

```text
uCharacter__move_base(this)
if (!(flags_at_1514 & 0x10)):
    uSpencer__updateControlBitset(this)
    uSpencer__updateActionTrackLatches(this)
    uSpencer__updateWireUserFlag(this)
```

The Spencer-specific tail is bounded and contains no collection traversal,
allocation, model evaluation, collision registration, shot creation, or effect
creation.

- `uSpencer__updateControlBitset @ 0x1400D22A0` performs five to seven bit
  assignments in the cChr bitset beginning at `+0x12C`.
- `uSpencer__updateActionTrackLatches @ 0x1400D2360` checks three `mUsrF` bits,
  action groups `0xBE..0xC0` and `0xAC..0xAE`, and one-shot latches at
  `+0x68F0..+0x68F2`. On a first transition it advances the action parameter
  track at `+0x1348` by 30 or 35 frames.
- `uSpencer__updateWireUserFlag @ 0x1400D25F0` sets `mUsrF.0x2000` when
  `uSpencer::mIsWire` at `+0x68F3` is true.

These functions are authoritative action state. They are not responsible for
the normal hundreds-of-microseconds callback cost and are not safe skip targets.

## Shared Full Move

`uCharacter__move_base @ 0x14004BD30` performs the expensive work. Its major
control flow is:

1. Update pre-input vectors, frame flags, and action-time state.
2. Evaluate the active/reserve gate.
3. Process CPU/netplay input through `cChrInput__processFrameInput`.
4. Run two `cChrCmd__updateMatcherAndSuccessDelay` command matchers.
5. Dispatch action transitions and animation command blocks.
6. Advance action, motion, position, gravity, and throw state.
7. Run `uModel__move @ 0x140566580`.
8. Rebuild push, attack, and hurt collision spheres.
9. Finalize references and dispatch post-move callbacks.

The full-path condition is:

```text
cChr__isMoveActive(character) ||
(!cChr__checkBusyState_6380(character) &&
 cChr__canPerformTeamChange(character))
```

This permits intentional assist, tag, DHC, and team-transition updates in
addition to the current point characters.

### Command matching

Each of the two embedded command matchers walks every configured 0x20-byte
command-line object and invokes its virtual update. Cost therefore depends on
the character's data-driven command count and current charge/matcher state.

### Action scripts

Action changes dispatch `uSpencer__onActionTransition`, then bind and execute
animation command blocks. `anmchrParser__parse3Level @ 0x1401038E0` is a large
70-case interpreter. Its cases synchronously mutate character state and can
create effects, cameras, attack spaces, and shots. This work is attributed to
the enclosing concrete character callback.

### Model and joint evaluation

`uModel__move` performs:

```text
uModel__updateCameraVisibilityMask
uModel__updateAuxRenderTexture
uModel__evaluateMotionFrame
virtual model callback +0x90
virtual model callback +0x98
```

`uModel__evaluateMotionFrame` scans active motion slots, blends active layers,
evaluates simple or layered joint transforms, advances every active motion, and
updates completion/event masks. Its inner work scales with motion-layer and
joint count. These transforms drive root motion, weapon/shot attachments, and
collision geometry; the model block cannot be removed as render-only work.

### Collision rebuilding

The late collision passes are fixed scans over embedded primitive banks:

- Up to 16 push spheres.
- Up to 16 attack and 16 hurt spheres.

Each active primitive evaluates world geometry from current model joints and is
queued into `sCollision`. This cost depends on the current action's active
hitbox/hurtbox set and cannot be skipped without changing gameplay.

### Finalization

`uCharacter__finalizeMoveAndDispatchPostCallbacks` validates 21 auxiliary
`MtObject` references, updates command/animation helpers, and invokes final
virtual callbacks. This also runs for some cheap-path states and is simulation
bookkeeping rather than draw submission.

## The 45 Millisecond Spike

The host's maximum is not a long Spencer state update. In rollback window
`367-373`:

```text
uSpencer move maximum: 45.6769 ms
sShot__spawnShot:       45.1350 ms, one call
```

The call originated at `anmchrParser__parse3Level + 0x2A96` (`0x140106376`).
The session log identifies a standard shot created at native frame 368. The
client executed the same frame-368 command with the same owner RNG transition
and a corresponding shot spawn, but did not exhibit a timing spike.

`sShot__spawnShot @ 0x140150E00` synchronously performs:

1. Shot-class lookup by DTI hash and virtual factory allocation.
2. Owner, request-unit, team, and target setup.
3. Shot-resource refcounting and data binding.
4. Attack-info and collision-primitive construction.
5. Motion-controller and matrix initialization.
6. Locked insertion into the per-slot shot list.
7. Slot-active flag publication.

The host had shot sequence 3 while the client had sequence 4, so allocator or
resource-pool warmth may differ slightly. The peer-local nature and single-call
shape are also consistent with scheduling, page faults, heap growth, or cold
resource/collision allocation. It is not evidence that Spencer's normal move
algorithm takes 45 ms.

Removing this one outlier from the host gives approximately:

```text
(286.8493 - 45.1350) / 726 = 0.3330 ms per Spencer callback
```

That matches the client's `0.3342 ms` average almost exactly.

## Conclusion

Spencer's normal measured cost is approximately `0.333 ms` per full callback.
Almost all of it belongs to required shared character simulation. The
Spencer-specific tail is small and constant-time. The apparent host excess is
entirely explained by one synchronous shot-creation stall.

No safe broad skip was identified. The next useful measurement is scoped timing
inside `uCharacter__move_base` for command matching, action-script dispatch,
model/joint evaluation, collision rebuilding, and finalization. Shot creation
should separately time DTI factory allocation, resource binding, collision
construction, motion initialization, and list insertion. That will distinguish
a repeatable native bottleneck from a one-time allocator or operating-system
stall.
