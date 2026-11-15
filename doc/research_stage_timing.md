# Stage Advancement During Resim, and Native Frame-Timing State

Ghidra research, 2026-11-14, port 8192 `umvc3.exe.unpacked.exe`. Static only:
no runtime trace and no stage layout data was parsed. "Verified" means read in
the decompiler. "Hypothesis" means inferred and not yet checked.

## 1. Stage Units and Their Lanes

| Unit | Registration | Resim lane? | Clock |
|---|---|---|---|
| `uStage` (and derived `uStageNNN`) | `sStage__createStage @ 0x1401F07D0` -> `sUnit__addBottom(4)` | **No** (lane 4 omitted) | `cUnit` delta / time scale; `mSchedulerSpeed`; `uStage__applyPlaybackSpeedWithPause @ 0x1401F53F0` |
| Stage `uScheduler` (`uStage+0x1498`) | `uStage_vfn120` (around `0x1401F4119`) -> `0x14051A6B0`, line 0 | No | `uScheduler__move @ 0x14050F190`: `mFrame += mSpeed * cUnit.mDeltaTime` |
| Stage objects (`uStageObject`, DTI `0x140D4EDE0`, and about 20 `uStageObjectBaseNNN` families) | `uStage__spawnLayoutObjects @ 0x1401F4290` (renamed from `uStage_vfn75`): lane = `rStgObjLayout` record `+0x24` (record stride `0x280`) | **Data-driven**; can be a resim lane | `uStageObject__tickWithStageSpeedScale @ 0x1401ECC90`: `mAnmRate * mDeltaTimeLocal * parentStage(+0x110)` or `sStage+0x3C` |
| Stage-object effects (`uGameEffect` 300/0x65) | `uStageObject100__ensureLane13Effect @ 0x1401E3060` (renamed): owner `+0x280` = stage object, `sUnit__addBottom(0xD)` | **Yes** (lane 13) | effect delta (owner time scale) |
| Character stage effect, stages 006/105 (`uGameEffect` 300/100) | `sStage__spawnCharacterStageEffect @ 0x1401F1020`, called only from `uCharacter__resetForMatch`; owner = character, aux slot 10, lane 13 | Yes, but spawned once per match | effect delta |

Links: stage-object parent is `uStageObject+0x1470` (`mpParentStage`). The stage
lists its objects in `uStage+0x14E0` (`mStageObjectArray`).
`uGameEffect__setOwner @ 0x1400604E0` writes `+0x280`;
`uGameEffect__setRequestUnit @ 0x140132140` writes `+0x288`.

### Why stage effects can run ahead (hypothesis, consistent with the code above)

1. A stage-owned effect in lane 13 runs during every resimulated tick. Its owner
   (the stage, lane 4) and the stage scheduler (line 0) do not.
2. If the effect transaction does not capture and rewind that effect, the
   restore leaves it at its live state. Resim then advances it N more ticks. A
   restore that rejects the effect has the same result. In the effect lanes
   (14-16) the same applies to any stage-spawned effect that the transaction
   rejects.
3. A stage object whose layout lane is a resim lane is not restored, because it
   is outside the character/shot/effect domains. It therefore advances N extra
   ticks per rollback.

### Gameplay relevance (verified)

- **Global RNG.** The stage-object FSM commands `OBJ_SetRandF`
  (`uStageObject__fsmExec_SetRandF_0 @ 0x1401EA5C0`) and `OBJ_SetRandI`
  (`0x1401EA640`), plus `OBJ_SetAnm_RandI`, draw from the **global**
  `MtXorShift` (`MtXorShift__floatInRange @ 0x140455550` and `__intInRange @
  0x140455590` call `MtXorShift__getGlobalState`). Registration is in
  `uStageObject__registerFsmCommands @ 0x1401F1C00`. `uStage` shares these
  commands through `uStage__initializeFsmCommands @ 0x1401F1BE0`.
  Consequence (hypothesis): if a stage FSM draws RNG on live ticks but not on
  resimulated ticks, the global RNG stream diverges between peers. That applies
  to the uStage in lane 4 and to stage objects in omitted lanes. The opposite
  case, stage objects drawing RNG in resim without restored FSM state, also
  diverges. This is a desync risk, not just a visual one. Next step: trace RNG
  draws by caller on a stage with animated crowds or gimmicks.
- **Hit solver.** `uStageObject__filterAndAddHitNode @ 0x1401EA9A0` adds stage
  object vs character/shot pairs to `sHitSolver` (callbacks
  `StageHitCallbacks__dispatchLiveStageObject{Defender,Shot}Pair`). The node
  order in the hit solver can therefore depend on stage objects.
- `uStage__moveAfter @ 0x1401F4510` only reads characters
  (`sCharacter__forEachChrInTeams`) to set stage reaction flags. No
  character-side write was found.

### Proposed fixes

1. **Presentation partition (smallest change).** In the headless lane filter,
   skip lane-13/14-16 `uGameEffect`s whose owner `+0x280` (or request unit
   `+0x288`) is kind-of `uStage` or `uStageObject`. Exclude them from the effect
   snapshot/restore domain as well. They then stay on the live timeline,
   matching their owner, which also does not run in resim. Test with
   `MtObject::isKindOf`, using the `uStageObject` DTI `0x140D4EDE0` and
   `uStage__DTI` (resolve by name).
2. **Stage objects in resim lanes.** Apply the same owner-kind test to the units
   themselves: skip `uStageObject` instances in headless dispatch. Drive this
   from DTI ancestry, not from lane numbers.
3. **RNG.** Determine whether stage FSM RNG draws happen during battle. If they
   do, the drawing units must either run in both live and resim ticks with their
   FSM and animation state captured, or never touch the gameplay RNG. A patch
   for the second option: redirect `OBJ_SetRandF`/`OBJ_SetRandI` for stage
   receivers to a private `MtXorShift`. Both peers must patch identically.
4. **Character stage effect (006/105).** Created once at round reset, so it only
   advances like any other lane-13 effect. Covered by fix 1 if it is routed by
   resource 300/100 together with the owner test. Its owner is the character,
   so the owner test alone does not catch it.

Open question: which lanes the layout records (`record+0x24`) actually use on
each stage. Checking this needs `rStgObjLayout` data, which this pass did not
inspect.

## 2. Frame Advantage / Native Timing

- `sReplay__processPerFrame @ 0x1401CEFD0` increments `sReplay+0x54` once per
  tick. It is called from `uCharacterPost__move`, lane 18, so it also runs
  during resim. **In game mode 5 (training)** it also **resets `+0x54` to 0**
  when Back+Up/Down (`0x600000`) is pressed and the count is greater than
  `0x3B`. Any netplay clock derived from `sReplay+0x54` will jump on that
  hotkey in dual-training netplay. Also verify that the counter is rewound on
  restore.
- No native frame-advantage or frame-data display exists: there are no
  "advantage" strings, and `mFrameCount` appears only in shot properties. A
  training overlay would have to compute advantage itself from character timers.
  Candidates read in `uCharacter__updateCollisionBoxes @ 0x140038540` (an
  action-wait predicate, misnamed) are listed below.
  - `mAnmWait`: `+0x1334`, wait-mode switch 1..0x11
  - `mStiffDmg`: `+0x4154`
  - `mStiffGuard`: `+0x4158`
  - Ukemi and down stiff: next floats in the same block
  - Wait-related speeds and accelerations: `+0x40E8` and `+0x40F0`

  Frame advantage = the first tick the defender becomes actionable minus the
  first tick the attacker does. The actionable tick is when this predicate (or
  `cChr__isInActiveStateAndUnflagged`) first returns true, so per-tick sampling
  on both points is enough.
- Pacing accumulators (`sMain+0x40010..`) are wall-clock pacing only. No
  gameplay consumer was identified in this pass.

## Ghidra Edits

- Renamed `0x1401F4290` to `uStage__spawnLayoutObjects`.
- Renamed `0x1401E3060` to `uStageObject100__ensureLane13Effect`.
- Added comments at `0x1401F4290` and `0x1401EA5C0`.
