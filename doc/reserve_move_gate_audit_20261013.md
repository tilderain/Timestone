# Reserve Move Gate And Rocket Callback Audit

Static Ghidra/source investigation, 2026-10-13. Native executable:
`ichneumon_sub/umvc3.exe.unpacked.exe`, preferred base `0x140000000`.
No gameplay code was changed or game launched. This report supplements
`reservechr.md`; it does not establish runtime attribution of the supplied
`uRRaccoon` timing.

## Verdict

**No current defect bypassing the shared benched-reserve update gates was
established.** Native code intentionally schedules reserve callbacks but normally
skips their input/action/animation body. The current restore already contains the
roster `cUnit+0x10` correction documented in `reservechr.md`.

**Separate confirmed behavior:** Rocket's post-base airborne logic is not gated
by reserve activity or zero time scale. Its phase effect can be replaced on every
invocation at a fixed height in `(60,70]`, or spawned and immediately retired at
exactly `120`. This is a concrete repeated-work path, not proof that benched
reserves ran the shared full update, nor proof it caused the measured cost.

The supplied `6.00 calls/frame`, `121.40 us avg`, and `0.728 ms/frame` describe six
outer callbacks. They do not count entries into the expensive inner branches.

## Two Native Boundaries

`cChr__isMoveActive @ 0x140056290` requires:

```text
MtObject::isAliveState(character)
(cUnit.flags_at_10 & 7) == 2
(cUnit.flags_at_10 & 0x400) != 0
(uCharacter.mChrF_at_14F0 & 1) == 0
```

The scheduler uses cUnit state/move enablement, not `mChrF.bit0`. Therefore all
six roster objects can receive callbacks while four take the reserve early-out.
The profiled traversal in `rollback_tick.cpp:331` retains this distinction and
dispatches the actual `+0x40` virtual. It does not force the character gate true.

The initial gate inside `uCharacter__move_base @ 0x14004BD30` is:

```text
isMoveActive || (canPerformTeamChange && !busyOrBlocked)
```

Disassembly at `0x14004BDF0..0x14004BE2D` confirms failing this gate initializes
input and jumps directly to finalization. It does not execute command matchers,
action selection, animation, movement, or the ordinary sphere-update calls.

`cChr__canPerformTeamChange @ 0x140038AF0` requires cached team `mCAnmF & 0x2000`,
non-proxy roster membership, and specific DHC/member conditions. With DHC count
zero it selects member 1 if alive, otherwise member 2. With count one it selects
member 2 only when team `mAlive >= 3`; other counts fail. The shared accessor
named `cPlayData__getDmgThrow @ 0x1401007C0` reads team `+0x5C` here, which is
`mAlive`, not a throw statistic or the `mAliveMember` mask.

This exception admits at most one inactive reserve per team to the longer
input/matcher/action-selection path. It does not itself clear `mChrF.bit0`.

At `0x14004C187..0x14004C19E`, a second gate requires:

```text
isMoveActive && canAdvanceSimulation
```

It controls the animation-command loop, model motion/joints, and movement.
Transition-eligible inactive reserves do not automatically pass it. Both gates
test the predicate's low byte; decompiler artifacts in wider return values are
not evidence of a native boolean bug.

## Who Activates And Benches A Fighter

Being different from `team.mpCurrentChr` does not imply inactivity. Native
activation supports assists and overlapping outgoing/incoming fighters.

| Function / command | Address | Verified behavior |
|---|---|---|
| `uCharacter__resetForMatch` | `0x1400568A0` | Clears bit0 with mask `0xFDA55FC6`, then ORs `0x4102`; opening reset intentionally leaves all six fighters' bit0 clear. |
| `uCharacter__applyAttackFlagsAndAnimationRequest` | `0x1400567F0` | Request flag `2` clears bit0 at `0x140056859`; it can activate a non-current fighter. |
| `cChrMessage__dispatchToRecipients` | Call at `0x1400307F9` | Invokes recipient virtual `+0x300` with request flag `2`; message flag `4` also marks assist state through `mEtcF & 0x4000`. |
| `sCharacterChangeOrder__requestOrApplySwap` | `0x14001F690` | Incoming action application calls `+0x300` with flag `2` at `0x14001F744`. |
| `cChrTeam__commitPendingCharacterSwap` | `0x140021510` | Exchanges member IDs/current pointer; does not itself set outgoing bit0. |
| `uCharacter__clearComboState` | `0x140056FB0` | Despite its old name, `+0x280` is an inactivation callback: sets bit0 and resets command/ability state. |
| Animation command `1_13` | Call at `0x1400FC22B` | Invokes the `+0x280` inactivation callback. |
| Animation commands `1_29..1_2C` | `0x1400FC3C8..0x1400FC40F` | Assign, OR, clear, or XOR supplied `mChrF` masks. |

The inspected character pre/post pipeline does not automatically bench every
non-current fighter. Exit inactivation is action/resource-driven. A stale clear
bit0 could keep a physically offscreen fighter updating, but proving that defect
requires demonstrating that the expected exit/inactivation command was missed,
overwritten, or replayed incorrectly.

The installed action-resource command streams and runtime exit timing were not
audited in this pass. This is not an exhaustive proof against every possible
character-specific or resource-driven activation bug.

## Current Restore And Replay

Relevant current source policies:

| State | Current policy |
|---|---|
| Roster cUnit `+0x10` | Excluded from pointer-like preservation at `rollback_restore.cpp:1703` and from live lifecycle reapplication at `:1728`; follows the saved body. Helpers retain their distinct live lifecycle policy. |
| `mChrF`, `mAnmF`, `mUsrF` | Explicit pointer-free restore `{0x14F0,0x28}` at `rollback_restore.cpp:227`; overrides broad pointer heuristics. |
| Team transition gates | Team ranges at `rollback_restore.cpp:370` cover alive/member state, DHC count, pending swaps, and cached `mCAnmF`. |
| Active fighter pointer | Restored at `rollback_restore.cpp:1587`; invalid/not-in-live-roster saved references can fall back to the live pointer. This is a mixed-timeline risk, not a direct bit0 writer. |
| Activation messages | Six message payloads restored at `rollback_restore.cpp:1876`; owner pointers remain self-owned. |
| Action/motion script state | Main action and motion payloads restore; motion event bindings rewind only while their borrowed resource identity matches. |
| Rocket phase/latch | Verified concrete ranges at `concrete_snapshot.cpp:302..304`: `+0x6950/2`, `+0x6954/9`, `+0x6960/4`; the nine-byte range includes latch `+0x695C`. |
| Rocket effect owners | Captured/remapped at `rollback_effect.cpp:807`, `:935`; short recipes preserve native owner occupancy, including completed-but-referenced owners. |

Character regions can still be skipped if the expected vtable no longer matches.
Also, the late scalar-reapply list does not reapply `mChrF`/`mUsrF` after recipe
construction. These are audit boundaries, not confirmed causes: no current
post-restore path clearing all reserves' bit0 was identified.

Headless replay retains character preprocessing (category 7), fighter callbacks
(8), and postprocessing (18), including the late phases. The action helper at
`0x14014F410`, called before the move pass, performs action-owned reference
cleanup; it is not a second all-fighter reset/update loop. Active-visibility
repair changes draw flags, not `mChrF.bit0`.

## Rocket Work After The Reserve Early-Out

`uRocketRaccoon__tick @ 0x1400C4C10` calls the base update, then still validates
twelve weak references and, unless `mDbgF & 0x10`, runs airborne lifecycle,
weapon-facing visibility, part visibility, and a conditional animation fix.

The ordinary reference/visibility work is bounded. It is not evidence of a
runaway full-update loop.

`uRRaccoon__updateAirTumbleLifecycle @ 0x1400C56B0` enters airborne simulation
only when `mUsrF.bit0` is set and `+0x695C` is zero. It has no activity/time-scale
gate. Match reset initializes `mUsrF` to zero. Ordinary action change calls
Rocket's `+0x2D0` return-3 mask and clears both user bits 0/1 in the shared handler
at `0x14003B285`. Normal idle reserves therefore do not unconditionally spawn
jetpack effects.

The bench callback itself does not clear `mUsrF`. Whether a particular exit
script can bench Rocket while leaving the enable bit set remains unverified.

### Pointer-Dependent Reinitialization

`uRocketRaccoon__updateAirTumble @ 0x1400C4690` spawns owners at `+0x68F8` and
`+0x6900` when absent. A missing second owner also resets vertical speed,
acceleration, and the movement accumulator, dispatches persistent voice `0x2D`,
and sets user bit1. These side effects occur even if the spawn fails. Neither
bit1 nor time scale suppresses that missing-owner initialization.

Conversely, the larger exit cleanup (gravity/latch/phase handling) requires the
second owner to be non-null. Incorrectly clearing this owner is not merely a
cosmetic omission. Current reconstruction explicitly handles this occupancy,
but failure to reconstruct it remains a concrete failure mode to trace.

### Confirmed Phase Replacement Behavior

`uRocketRaccoon__updateAirTumbleEffect @ 0x1400C58B0` reads world Y at `+0x54`.
Disassembly and float constants confirm the finite-value branch behavior:

```text
if 60 < Y <= 120 and A == 0: replace with variant1; (A,B) = (1,0)
else if Y <= 70 and B == 0: replace with variant2; (A,B) = (0,1)
then if Y >= 120 and either phase set: retire effect; (A,B) = (0,0)
```

Here `A/B` are bytes `+0x6950/+0x6951`. At fixed `60 < Y <= 70`, the valid
states alternate and an effect is replaced on each invocation. At exactly
`120`, the first branch can create an effect that the final branch immediately
retires, resetting its recreation latch. Phase flags advance on spawn failure
too. Joint/matrix and swept sphere/plane work follows regardless of effect
presence.

This is confirmed native allocation/retirement behavior. Intended visual
semantics, actual frequency during hitstop or reserve states, and contribution
to `121.40 us`/`0.803 ms max` are unmeasured. Do not patch it by suppressing
reserves or changing thresholds without gameplay/determinism validation.

## Decisive Runtime Check

The current DTI profile cannot resolve this hypothesis. Capture each roster
instance on the original and replay timelines with:

```text
frame, character pointer, team/member, current-point identity
cUnit+0x10, mChrF, mAnmF, mUsrF, current action
initial gate outcome and later animation/movement gate outcome
activation-request and inactivation-command events
team mAlive, mAliveMember, DHC count, cached mCAnmF, pending swap state
Rocket Y, time scale, +0x6950/+0x6951/+0x695C, +0x6900 owner presence
base duration, Rocket post-base duration, short-effect creation count
```

Compare snapshot values, post-restore values, pre-category-8 values, and callback
exit. The failure signature is an unexplained activation or lost bench command,
not simply six callbacks or a non-current fighter with bit0 clear. Separating
base/post-base timing also distinguishes a real gate leak from Rocket effect
work outside that gate.

Ghidra receiver types and explanatory comments were updated. No native code
bytes, plugin behavior, or reserve-suppression policy were changed.
