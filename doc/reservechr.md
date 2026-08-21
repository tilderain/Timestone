# Reserve Character Full-Update And Restore Audit

## Scope

Static analysis of `uCharacter__move_base`, the team transition pipeline, and
character snapshot restoration. Addresses refer to
`ichneumon_sub/umvc3.exe.unpacked.exe` at preferred image base `0x140000000`.
No restore behavior was changed during this investigation.

## Native Full-Update Gate

The native character implementation already has a cheap reserve path. The full
movement body runs only when:

```text
cChr__isMoveActive(character) ||
(!cChr__checkBusyState_6380(character) &&
 cChr__canPerformTeamChange(character))
```

`cChr__isMoveActive @ 0x140056290` requires all three conditions:

```text
(cChr+0x10 & 7) == 2
(cChr+0x10 & 0x400) != 0
(cChr+0x14F0 & 1) == 0
```

Characters that fail the full-path expression initialize their input buffer and
jump directly to `uCharacter__finalizeMoveAndDispatchPostCallbacks`. Plain
benched characters therefore already avoid input, command, action, model,
position, and collision-sphere evaluation.

## Legitimate Full Reserve Updates

A non-point character can run fully for two reasons:

1. It is move-active. Assists, incoming or outgoing tag/DHC characters, and
   intro/setup actors can have `mChrF.bit0` clear while not equal to
   `cChrTeam::mpCurrentChr`. `cChr__isLocalControlled_62F0` also accepts a
   non-point move-active character when `mChrF & 0x40000` is set.
2. It is the selected inactive transition reserve. When the active character's
   cached `mCAnmF & 0x2000` is set, `cChr__canPerformTeamChange @ 0x140038AF0`
   selects member 1 when `mDHCCount == 0`, or member 2 when `mDHCCount == 1` and
   the transition count is at least 3. Input and special-command processing use
   the same predicate so the reserve can begin its entry action.

The inactive transition exception selects at most one reserve per team. It can
explain at most four full updates including the two point characters. Six full
updates require additional roster members to satisfy `cChr__isMoveActive`.

## Opening Six-Character Updates

`sAction__stepFightingCore @ 0x140006370` calls
`character_updateAllFighters_maybe @ 0x14005A210`, which dispatches every
fighter's vtable slot `+0x2F0`. Ordinary fighters reach
`uCharacter__resetForMatch @ 0x1400568A0`.

The reset rewrites `mChrF` with `0x4102`, leaving bit 0 clear for all six roster
members. It also resets action, motion, ability, status, input, command,
collision, and character-specific state. Initial point selection and pending
swap commitment occur afterward. Consequently, all six characters are
intentionally move-active during part of native opening/setup processing.

Blanket suppression based on `character != team.mpCurrentChr` would break
intro, assist, tag, and DHC behavior. Full updates produce authoritative action,
motion, joint, root-motion, position, collision, RNG, helper, and team aggregate
state. `uCharacterPost__move` can subsequently register collision primitives
from those results.

## Fixed Restore Gap

The character snapshot contains the complete cUnit flag word at `cChr+0x10`.
The restore previously discarded that saved value for roster characters.

In `rollback_restore.cpp`:

1. `CUNIT_LIVE_STATE_OFFSETS` contains `{0x10, 0x18, 0x20, 0x38}`.
2. Restore saves those qwords from the prediction timeline.
3. It copies the snapshot character body.
4. It then writes the preserved live qwords back.
5. The old loop excluded only intrusive-link offsets `+0x18`, `+0x20`, and
   `+0x38`. It wrote the live prediction-time qword back at `+0x10`.

The restore now leaves the snapshot's `+0x10` qword in place for the six roster
characters. Helper cChr objects retain the old live-lifecycle policy so rewind
cannot re-run their setup/createProperty lifecycle. Intrusive links at `+0x18`,
`+0x20`, and `+0x38` remain live for every object. Roster `+0x10` is also
excluded from the broad pointer-preservation scan so an aligned inactive flag
combination cannot be misclassified and restored from the live timeline later.

The old gap could create an erroneous full reserve update under the following
state:

```text
saved mChrF.bit0 == 0
saved cUnit state was inactive or move-disabled
live prediction cUnit state is active and move-enabled
```

The old restore combined historical `mChrF` with live lifecycle/move flags,
causing the character to pass `cChr__isMoveActive`. The headless category-8
traversal uses the same `+0x10` state to decide whether to dispatch the callback.
Four reserve objects could independently inherit this mismatch.

## Explanatory Limit

The former `+0x10` gap was a real possible mechanism, but static analysis does
not prove that it caused the measured opening windows.

In ordinary bench frames, profiling showed six category-8 callbacks but only two
full input paths. This proves all six cUnits were already active and
move-enabled; `mChrF.bit0` selected the cheap path for four reserves. The
character restore explicitly reapplies `+0x14F0..+0x1517` from the snapshot.
During native opening reset, all six saved `mChrF.bit0` values are intentionally
clear.

The fix matters specifically at a lifecycle boundary where saved and live
`+0x10` eligibility differ. It does not change ordinary bench frames where the
saved and live cUnit flags already agree.

## Other Restore Asymmetries

- `cChrTeam::mpCurrentChr` normally follows the snapshot, but a saved null or a
  saved pointer not found in the current team list falls back to the live point
  character. This can create mixed-frame role/control decisions but does not
  independently clear `mChrF.bit0`.
- If a live roster object's vtable differs from the snapshot's expected vtable,
  the complete character region is skipped, leaving prediction-time `mChrF` and
  other state. Ordinary roster objects are expected to retain stable identities,
  so four simultaneous mismatches have no identified static cause.
- `mChrF` is restored before shot/effect recipe reconstruction and is not in the
  late character reapply table. Native factories and model repair run in
  between, but no generic post-restore writer that clears all reserve
  `mChrF.bit0` values was identified.
- Team gameplay aggregates, pending swap IDs, character member IDs, and both
  `sCharacter` change-order records are covered. A defect in these values could
  open the inactive transition exception, but that exception selects at most one
  reserve per team and cannot make all four reserves move-active.

## Validation Needed

For every roster character, record these values from the snapshot, immediately
before restore, immediately after restore, and before the first replayed
character callback:

```text
cChr pointer, team, member ID
cChr+0x10 complete qword
cChr+0x14F0 mChrF
is cChrTeam::mpCurrentChr
team pending mode/current/next IDs at +0x9C/+0xA0/+0xA4
team mAliveMember at +0x60
team mDHCCount at +0xBC
team mCAnmF at +0xF8
sCharacter change-order records at +0x5C0 and +0x5E0
```

Interpretation:

- Saved and restored `+0x10` differ while restored `mChrF.bit0` is clear: the
  confirmed cUnit preservation gap can explain the extra full update.
- Saved and restored gate values match and all six are active on both original
  and replay timelines: the updates are intentional native state.
- `mChrF` differs immediately after restore: the character body was skipped or
  overwritten during restore.
- Gate values match after restore but differ before the first character tick:
  pre-character ordering or manager work is responsible rather than snapshot
  coverage.

## Verdict

Live preservation of roster `cChr+0x10` was the only confirmed restore gap
directly inside the three-term `cChr__isMoveActive` predicate. Roster characters
now restore this qword from the snapshot, while helper lifecycle and all
intrusive links remain live. The old defect is not proven to explain the
observed six-character opening state, which has a complete native explanation
through `uCharacter__resetForMatch`.


Restoring cUnit+0x10
Advantages:
- Character callback eligibility matches the snapshot.
- Prevents prediction-time state == 2 or Move == 1 from making historical reserves update.
- Restores characters that were active historically but became inactive on the prediction timeline.
- Keeps cChr__isMoveActive internally consistent with restored mChrF.
Risks:
- Restoring lifecycle states 1, 3, or 4 can make native sUnit setup, unlink, or destroy an already-live roster object.
- Restoring mMoveLine can disagree with the live intrusive list containing the character.
- Restoring draw, visibility, fix, and draw-view bits rewinds presentation state unnecessarily.
- The current qword restoration also rewinds mDrawMode at +0x14.
- Live resource ownership and scheduler links may no longer agree with the restored lifecycle flags.
Not restoring it
Advantages:
- Lifecycle flags remain consistent with the live sUnit lists and allocations.
- Avoids repeating setup or destruction.
- Preserves current renderer visibility and draw state.
- This is safer for dynamically created helper characters.
Risks:
- Produces mixed-frame character state: historical mChrF, actions, and status combined with prediction-time callback eligibility.
- A reserve can receive an extra full update.
- A historically active character can incorrectly miss its update.
- TAC, tag, assist, or defeat transitions can observe the wrong alive/move state.
- The first replayed frame can already diverge before character logic has a chance to correct it.
