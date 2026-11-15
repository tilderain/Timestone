# Visual-State Rollback Research: Maya, Cut-Ins, Veil Of Mist

Ghidra 8192 `umvc3.exe.unpacked.exe`, 2026-11-14. Static analysis only; no
runtime trace. "Verified" = read in native code; "Hypothesis" = inferred.

## Shared Draw Gate (verified)

`sUnit__draw @ 0x14051AD90` draws a unit only if `+0x10` state bits `== 2`,
`(flags & 0x4800) == 0x4800`, the current view bit is set in `(flags >> 16) & 0x3FF`,
`+0x14` (draw mode, `cUnit__setDrawMode @ 0x1401461E0`) matches the pass, and the
`+0x30` group mask passes `sUnit::mGroupDrawFlag`. `uBaseModel__draw` also
early-outs on `mpModel == 0` or `mTransparency (+0x180) == 0` (`0x14055DA44..5C`).

`uBaseModel::mViewDraw/+0x190` is **not** a draw gate. Its only readers are
`uBaseModel__updateViewDraw @ 0x1405641A0` and a property getter
(`0x14055FA30`). The `mViewDrawControl` re-arm in `uBaseModel__draw` is harmless.

## 1. Cut-In Disappearing

### Native lifecycle (verified)

- Spawn: the `anmchrCmd1_interpreter` cut-in opcode at `0x1400FD305` runs
  `uCutIn__operator_new(0x7C0)` and `ctor @ 0x140448C10`. It calls `addCharacter`
  for the user and for every partner whose owner is the user (gate: user
  `+0x150C & 0x100000`), then `setVariantPropagateToCam`, then
  `sUnit::addPending(cat 3) @ 0x14051A6B0`. Nothing checks for an existing cut-in.
- Registry: the ctor and dtor call `sGameUI__registerCutin @ 0x1402BFCF0`. It
  maintains the MtArray at **`sGameUI+0x5670`** (count `+0x5678`, data `+0x5688`).
  `sGameUi__updateCutinActivity` sets `sGameUI+0x59B0` when any registered cut-in
  is alive. `sGameUi__killRegisteredCutins` kills them all.
- Tick: `uCutIn__moveAfter @ 0x14044AF10`, cat 3.
  - Kill (`+0x78`) if the first participant's slot-0 motion number differs from
    the cache `+0x7A8`.
  - Timer `+0x750` += speed·dt. Fade timers `+0x79C/+0x7A0`.
  - End when the timer exceeds duration `+0x754`, or the cam `+0x670` anim frame
    reaches its end.
- `uCutIn__setCutinRenderMode @ 0x14044B310`, for each participant (array
  `+0x40`: count `+0x48`, data `+0x58`):
  - Mode 1 (fade-in reached): save the `0x4000` visible bit into slot word
    `+0x758 + 4i`, clear `0x4000`, and set draw layer `0x100000 << n` on the
    character and its weapons. Then `+0x7A4 = 1`.
  - Mode 0 (from `uCutIn__destroy @ 0x14044A900`): restore `0x4000` from the slot
    (char id `0x3A` with a parent is forced visible), set layer bit 1, and
    stencil 3.

### Defects

1. **Invalid cut-in source (verified).** `sMvc3Manager` is a 392-byte holder
   (`sMvc3Manager__ctor`, writes `g_psMvc3Manager @ 0x140D510A0`). No native code
   writes `[mgr+0xB50]`; an unfiltered scalar scan found no cut-in/anmchr/sMvc3
   writer. `Rollback_SyncActiveCharacterVisibility` and
   `Rollback_RepairOrphanedCutinRenderState` therefore read heap memory past the
   object, so `validCutin` is effectively always false. The consequences:
   - After every resim, the point character and every team member with cut-in
     layer bits are forced back to the normal layer, made visible and given
     stencil 3. A cut-in in progress loses its participants: they vanish from the
     cut-in and reappear in the world.
   - `RepairOrphaned` also writes `0` to `mgr+0xB50`, which is out-of-bounds heap
     corruption, whenever the garbage is nonzero. The autotest `HasActiveCutin`
     uses the same offset.
2. **Cut-in timeline not rewound, and double-aged (verified mechanism).** `uCutIn`
   is not snapshotted, but cat 3 runs in headless resim. A rollback inside a
   cut-in replays N frames on an un-rewound timer, so it ends N frames early. The
   cam (cat 4) is excluded from resim, so the two end conditions diverge.
3. **Hypothesis.** A rollback to before the cut-in start lets the old cut-in kill
   itself by motion mismatch, and the replay respawns it. That part is correct.
   But the respawned cut-in ages during resim, while its cam does not.

### Proposed fix

- Replace every `mgr+0xB50` use with the `sGameUI+0x5670` array: alive check via
  `(flags & 7) - 1 < 2`, vtable/DTI `0x140D6BCC8`. Treat a character as a cut-in
  participant if any alive registered cut-in's `+0x58[0..+0x48)` contains it.
  Delete the `mgr+0xB50 = 0` store and fix autotest's `HasActiveCutin`.
- Do not tick `uCutIn` (exact DTI) in non-final resim frames, like `uChrChain`.
  Alternatively, snapshot per registered cut-in: `+0x750`, `+0x754`, `+0x79C`,
  `+0x7A0`, `+0x7A4`, `+0x7A8`, slot words `+0x758..+0x797`, and the cam
  (`+0x670`) anim frame/duration (`uCutInCam__setAnimFrame @ 0x140027F50`,
  `getAnimFrame @ 0x1400046C0`). Restore them when the identity is still alive.

## 2. Invisible Maya

Verified facts:

- `uMayoi` (DTI `0x140D456C0`, vtable `0x140A7A830`) is a team-chain
  `uCharacter`, and roster restore copies her `+0x10` (state, `0x800`, `0x4000`,
  view bits). `0x140D457C0` is **uNaruhodo** (Wright), not Mayoi; `+0x6910` is
  Wright's Maya slot.
- Her visibility is script-driven by the `anmchrCmd0` display command at
  `0x14010133E` (`MtObject__setFlag_0x4000_at_16 @ 0x140037700`). Her tick
  (`0x1400A8830`) only toggles weapon parts. Neither her code nor the anmchr
  interpreters write `+0x180`.
- She qualifies as a cut-in participant of Wright's hypers (partner owned by the
  user), so `setCutinRenderMode` saves, clears and restores her `0x4000` and layer.
  Char id `0x3A` with a parent is forced visible in mode 0; that this id is Maya is
  **unverified**.

Ranked hypotheses:

1. **(Medium) Cut-in interaction.** Her visibility is owned temporarily by a
   live, un-rewound `uCutIn`. Snapshot restore writes her raw mid-cut-in `0x4000`
   and layer bits, while repair uses the bogus `mgr+0xB50` source and only special-
   cases the point character. A rollback that crosses the cut-in fade-in or end
   can leave her `0x4000` cleared, or her layer on cut-in bits, with no surviving
   cut-in to restore it. Fix: the cut-in fixes above, plus a post-restore
   reconcile that, for every team-chain character (not just the point), clears
   cut-in layer bits and restores `0x4000` from the **saved** render state when
   it is not a participant of an alive registered cut-in.
2. **(Low) Transparency.** Restored raw `+0x180 == 0` would hide her. No native
   character writer was found, so this is unlikely unless a stale snapshot was
   taken during a 0 value.
3. Ruled out: `mViewDraw` (not a draw gate) and draw mode `+0x14` (Mayoi never
   changes it after `setup_base`).

Verification to collect: log Maya `+0x10`, `+0x11C`, `+0x180`, `+0x14` and the
`sGameUI+0x5670` contents before and after restore and at the first invisible frame.

## 3. Veil Of Mist (Amaterasu "Kirigakure")

Verified:

- `uAmaterasu__beginKirigakure @ 0x140060D30` sets `mIsKirigakure +0x6910 = 1`
  and slows the enemy team (`sCharacter__setTeamDeltaTimeOrder`).
  `tickAndResolveWeapon @ 0x1400600B0` clears it when ability prim 0x16 ends.
  `applyKirigakureEnemyEffect @ 0x140060440` spawns command effect 1004 per enemy
  (`uKirigakureEffect`, vtable `0x140A880A0`, uGameEffect base).
- `uKirigakureScreenEffect` (vtable `0x140A88350`, uGameEffectModel; ctor
  `0x1401325C0`) has a state machine at `mLocalStep +0x1D10`, run by
  `updateChildEffectLifecycle @ 0x1401327D0`:
  - Step 0 spawns two screen-mist `uGameEffect` children into `+0x1D00/+0x1D08`
    (control `0x240C0`, constant-update, cat 14, owner = the screen effect).
  - Step 1 waits for owner `+0x6910 == 0`, then `doFinish (+0xD8)` both children
    and nulls the slots. Step 2 waits for the slots to empty; step 3 kills itself.
- `uGameEffect__updateOwnerState @ 0x140139F50`: without control bit `0x800`, a
  dead owner only clears `mpOwner` and never kills. The children loop forever
  unless step 1 finishes them.

Root cause (high confidence, mechanism verified; RollbackUnit coverage of this
standalone `uGameEffectModel` inferred):

- The children are ordinary `uGameEffect`s, rewound and resurrected by the
  effect transaction.
- The screen effect's step and child slots are not rewound. It is at most leased
  as a borrowed owner, and it keeps ticking in resim (cat 14 runs).
- A rollback across the Kirigakure end, after the live screen effect has already
  passed step 1 (slots nulled, or self-killed), resurrects the children with no
  controller left to finish them. The mist lasts the rest of the round.

Fixes, in order of preference:

1. Include `uKirigakureScreenEffect` in the effect transaction: capture/restore
   `+0x1D10`, lease and restore `+0x1D00/+0x1D08`, and its lifecycle.
2. A post-restore sweep: for each live `uGameEffect` whose `+0x280` owner is a
   screen effect, kill or `doFinish` it if the owner is dead, at step ≥ 2, or no
   longer references it in `+0x1D00/+0x1D08`. Generalize to any owner-controlled
   child lacking `0x800`.
3. Verify that `uAmaterasu+0x6910` is in Amaterasu's concrete restore tail.
   Otherwise step 1 sees a stale flag.
