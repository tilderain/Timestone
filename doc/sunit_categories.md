# sUnit Categories and Synchronization Phases

**Status: Native scheduling reference with current headless policy. Original
capture analysis: 2026-07-27; live/source reconciliation: 2026-10-26.**
Addresses refer to Marvel `umvc3.exe.unpacked.exe`, base `0x140000000`.
Read-only checks on Ghidra 8193 covered `sUnit__moveLine` (`0x14051B4C0`),
`syncLine` (`0x14051BF10`), `moveAfterLine` (`0x14051B6F0`), wrapper names,
and current `sUnit`/`sUnit__MoveLine` layouts. Source policy was checked against
`rollback_tick.cpp` and `rollback_runtime.cpp`. The historical category/DTI
population and per-concrete callback table were not exhaustively recaptured or
re-decompiled; their names are address-based earlier research labels.

No Ghidra changes, game run or new timing measurement was performed. Serial
dispatch and a matching checksum are not universal proofs about omitted units.

## Summary

The 64 `sUnit` categories are ordered **move lines**. They are scheduling lanes,
not a strict type taxonomy. A category can contain unrelated `cUnit` subclasses,
and a class can be placed on different lines by different callers. The names
returned by `sUnit__getDefaultCategoryName @ 0x14051B1C0` are static labels only:

- 0-5 have broad engine labels (`SystemFilter` through `SystemBaseModel`).
- 6-15 are named `SystemGroupN`.
- 16-63 are named `GroupN`.

Consequently, names such as `Group29` cannot be resolved to one hidden class or
manager. The useful identity of a line is the set of objects registered there.
The table below records the strongest static evidence for lines 0-31, which are
the active lines in the supplied rollback profile.

## Native Traversal

`sUnit` embeds 64 `0x30`-byte move-line **records** beginning at `+0x38`.
Each record's head is at `+0x18`, so the head array is addressed from singleton
`+0x50` with the same stride. The active line count is at `+0xC38`. Do not use
the head pointer as the record origin; the previous table mixed those two bases.

Each move-line entry contains at least:

| Offset From Record `+0x38 + line*0x30` | Meaning |
|---|---|
| `+0x00` | Embedded `MtObject` vtable |
| `+0x08` | Native line-name pointer |
| `+0x10` | Line flags; bit 0 enables job dispatch, bit 1 disables the line |
| `+0x18` | Primary-list head (`sUnit+0x50 + line*0x30`) |
| `+0x20` | Primary-list tail |
| `+0x28` | Delta-time coefficient |

Equivalently, relative to the head address, flags are at `-0x08` and delta
coefficient at `+0x10`, as used by current headless code. This distinction
changes actual field interpretation, not just table formatting.

`sUnit__addUnit @ 0x14051A6B0` appends a node to the requested line and writes
the category into bits 3-9 of `cUnit + 0x10`. It then registers the unit in any
applicable multi-mask arrays. The earlier inventory recorded 1,082 static
calls/references; that count was not recomputed for the current database. Many
pass a variable category, showing that the default line name
is not an exhaustive description of its occupants.

## Per-Frame Phases

The profile's three calls per replayed frame reproduce these native `sUnit`
passes one category at a time:

| Profile label | Fence | sUnit category function | Per-active-node virtual | Meaning |
|---|---:|---|---|---|
| `move` | 1 | `sUnit__moveLine @ 0x14051B4C0` | `+0x40` | Main update, initialization and staged destruction |
| `sync phase 2` / `sync_job` | 2 | `sUnit__syncLine @ 0x14051BF10` | `+0x50` | First late pass after collision/hit solving |
| `sync phase 3` / `sync_save` | 3 | `sUnit__moveAfterLine @ 0x14051B6F0` | `+0x48` | Final late pass before manager/tail work |

The old names `load`, `loadCategory`, `save`, and `save_jobwrap` are historical
aliases. These functions do not serialize rollback snapshots. Current
`sUnit__sync @ 0x14051BFF0` opens fence 2 and dispatches manager vtable slot 12
(`0x60`, `syncLine`) for every category. `sUnit__moveAfter @ 0x14051B850`
opens fence 3 and dispatches manager slot 10 (`0x50`, `moveAfterLine`). Manager
vtable offsets must not be confused with the per-unit offsets in the table.

Fresh bodies also show late-pass initialization. A state-1 unit is set active
and receives setup `+0x28`; eligible units can receive an initial move `+0x40`
inside `syncLine`. `moveAfterLine` can additionally run initial `+0x50` before
its ordinary `+0x48`. Liveness/move flags are rechecked; a callback can retire
the unit. These extra calls matter at spawn/reconstruction boundaries. State-3
and state-4 teardown are scheduler visits, not a guaranteed two wall-clock-frame
delay independent of pause, group masks or traversal.

The node virtuals do not have one universal semantic name. Ghidra names such as
`moveAfter`, `sync`, and `getBoundary` describe individual class implementations,
not the scheduler slots. The scheduler always calls `+0x50` in phase 2 and
`+0x48` in phase 3. Concrete vtables prove that recovered names can appear in
the opposite order from their names: ordinary fighters place
`uCharacter__sync_base` at `+0x50` and `uCharacter__moveAfter` at `+0x48`, while
`uCharacterPost` places `uCharacterPost__sync` at `+0x50` and
`uCharacterPost__moveAfter` at `+0x48`.

### What the late phases do

The two phases are ordered extension points for work that must happen after all
selected units have moved and after `sCollision` and `sHitSolver` have run. They
are not network synchronization, snapshot serialization, or generic memory
copying. Each class assigns different work to the two virtual slots.

The exact callback targets recovered for entities in the supplied profile are:

| Entity/family | Phase 2: vtable `+0x50` | Phase 3: vtable `+0x48` | Recovered effects |
|---|---|---|---|
| `uActionSideCamera` | no-op | `uCamera__getBoundary @ 0x140545F70` | Refreshes `sCamera` views and derives camera boundary/translation vectors. |
| `uCutIn` | no-op | `uCutIn__moveAfter @ 0x14044AF10` | Advances cinematic timers, lighting, animation, participant lists, fade state, and cut-in lifetime. |
| `uViewDraw` | no-op | no-op | No gameplay work in either late slot. |
| `uCharacterPre` | no-op | `uCharacterPre__moveAfter @ 0x14005EF20` | Handles a conditional global character damage/action path under game flag `0x10000`. |
| `uShotPreProcess` | no-op | no-op | No late work in the recovered vtable. |
| `uGSoundProcess` | no-op | function at `0x1402B17C0` | Invokes the gameplay sound manager's late processing helper. |
| ordinary fighters | `uCharacter__sync_base @ 0x14004C840` | `uCharacter__moveAfter @ 0x14004C360` | Validates linked subobjects, synchronizes joint-hold targets, updates status, applies delayed hit/damage effects, advances timers, and performs post-hit/action cleanup. |
| `uFeliciaF` | `uFeliciaF__sync @ 0x140081550` | inherited character post-move | Runs base character synchronization and updates draw mode from its linked object. |
| `uShot` family | `uShot__dispatchByState @ 0x1401CE120` | `uShot__moveAfter @ 0x1401CCEB0` | Dispatches projectile state, then advances transforms, attack matrices, hit results, attack transitions, life/expiration, and cleanup. |
| `uChrChain` | no-op | `uChrChain__moveAfter @ 0x140036130` | Validates/rebinds model resources and advances chain/model finalization based on runtime flags. |
| `uGameEffect` | `uEffect__sync @ 0x1405E8690` | `uMaterialVjob__tickAndCullByBounds @ 0x14013A9B0` | Validates generator/parent lifetime and reference counts, advances effect state, culls by battle bounds, and may retire the object. |
| `uGameEffectModel` | no-op | `uGameEffectModel__getBoundary @ 0x14013C510` | Validates owner links, advances timers/model state, updates bounds, and may dispatch or retire the object. |
| `uExAtkSpace` | function at `0x140138250` | `uExAtkSpace__getBoundary @ 0x140137B60` | Cleans effect links, drives stage/attack-space lifecycle, advances the model, and destroys the attack-space object once children are gone. |
| `uCharacterPost` | `uCharacterPost__sync @ 0x14005F2B0` | `uCharacterPost__moveAfter @ 0x14005EE00` | Finalizes hit-solver and character links, throws/captures, targets, team state, partner attacks, swaps, defeat transitions, and post-frame status. |
| `uChrCpu` | `uChrCpu__sync @ 0x140120E90` | `uChrCpu__moveAfter @ 0x140120AE0` | Rebinds CPU controllers to active characters, clears dead references, and runs CPU script/action processing when enabled. |

The phase-3 category-13 cost is therefore not one generic synchronization
routine. In the measured matchup it is the sum of 16 chain finalizers plus
effect/model and attack-space lifecycle callbacks. Likewise, category 18 is a
single coordinator callback that loops over both complete teams and commits
authoritative combat state.

### Native Safety Boundaries

The earlier analysis identified these conservative dependencies. They are not
an exact description of every current filter or a proof that a whole line is
safe to drop:

- **Retain categories 8, 10-13, and 18.** Their late callbacks mutate combat,
  collision-facing, team, projectile, attack-space, target-link, or lifecycle
  state.
- **Retain category 9 when character auxiliaries are active.** Such objects
  inherit character late processing even when a particular capture is cheap.
- **Category 21 is mode-dependent, not generally disposable.** It can be
  omitted only if replay deliberately excludes CPU/training-script behavior and
  its controller bindings are reconstructed elsewhere.
- **Category 3 is primarily camera/cinematic state.** `uViewDraw` is a no-op,
  but `uActionSideCamera` refreshes camera-derived state and `uCutIn` advances
  cinematic lifetime and mutates shot/effect presentation flags. Skip only if
  the rollback design explicitly freezes and reconciles those systems.
- **Category 7 should be filtered by entity rather than removed wholesale.**
  shot preprocessing has no late work in the recovered vtable, but character
  preprocessing has a conditional gameplay path and sound processing advances
  its manager.

The safest optimization is therefore callback- or DTI-level suppression of
proven no-ops/presentation work. A category-only sync filter is useful for
experiments, but categories 7 and 13 are mixed lanes and cannot be classified
correctly by category alone.

In the historical capture, gameplay line job mode was disabled, so category
durations were serial lane wall time, not sums of worker CPU durations. Current
headless code has distinct profiled/unprofiled dispatch and optional native job
branches; do not assume the instrumentation is merely the unmodified native
category function plus a stopwatch.

### Current Headless Policy — 2026-10-26

Both move and late-pass allowlists currently retain lines **3, 7, 8, 9, 10, 11,
12, 13, 18 and 21**. Other lines receive move-phase state-3/4 cleanup rather
than normal active callbacks. That cleanup preserves lifecycle work while
omitting active simulation; it is not equivalent to deleting every skipped unit.

Exact-object filtering further excludes `uChrChain` and selected ExAtkSpace
parents/linked transition effects. Thus retaining line 13 does not mean all its
objects are replayed. Category-14 retained presentation and Rocket's separate
chain policy have their own contracts; see [super presentation](super_presentation_resim.md)
and [chain gating](felicia_shuma_chain_gating.md).

The exact mode-zero `uChrCpu` post path can retain binding cleanup/mode reset
while omitting threat work, falling back to native when core references are
invalid. This is not removal of CPU/input processing in general. The generic
`sAI` service is separate and its two explicit headless callbacks are commented
out. The active policy still needs matchup/mode/lifecycle tests; the historical
table below is evidence for reasoning, not a current exhaustive runtime census.

## Categories 0-31

Confidence meanings:

- **Confirmed**: a constant-category registration and concrete class or role
  were recovered in Ghidra.
- **Mixed**: confirmed examples exist, but the line is demonstrably a general
  ordering lane.
- **Tentative**: role is inferred from neighboring lines or dynamic callers;
  no unique static identity exists.

| Cat. | Native label | Recovered role and confirmed occupants | Confidence |
|---:|---|---|---|
| 0 | `SystemFilter` | Scheduler/filter lane. Runtime DTIs: `uUiScheduler`, `uScheduler`, `uStageScheduler`, `uCmbText`, and `uBtUiSubSysMsg`. Stage environment, UI, and resource schedulers are all confirmed here. | Confirmed mixed lane |
| 1 | `SystemShadow` | Shadow-system ordering line. The label is native; no battle-specific constant registration was needed to explain the near-zero capture cost. | Tentative |
| 2 | `SystemLight` | Light-system ordering line. The label is native; the supplied battle capture has effectively no active work here. | Tentative |
| 3 | `SystemFog` | Early scene/camera line. Runtime DTIs: `uActionSideCamera`, `uCutIn`, and `uViewDraw`. Despite the default name, it is not fog-only. | Confirmed mixed lane |
| 4 | `SystemEffect` | Stage/effect support line. Runtime DTIs: `uStage000`, `uUiSdlAnimeFrame`, `uCutIn::uCutInCam`, and `uInfiniteLight`. Native training reset purges stale categories 4-6. This is distinct from gameplay effects on lines 13-16. | Confirmed mixed lane |
| 5 | `SystemBaseModel` | Stage model line. Runtime DTIs: `uStgObj000_LightBeam` and `uStageStaticModel`. | Confirmed |
| 6 | `SystemGroup6` | Final line in the early stage/model region purged by training reset. Concrete membership is data-driven in the analyzed battle path. | Tentative |
| 7 | `SystemGroup7` | Pre-simulation coordinators. Runtime DTIs: `uCharacterPre`, `uShotPreProcess`, `uGameEffectPreProcess`, and `uGSoundProcess`. This explains why line 7 runs before characters and shots. | Confirmed |
| 8 | `SystemGroup8` | Main character/fighter line. Runtime DTIs in the capture: `uRyu`, `uMorrigan`, `uFelicia`, `uFeliciaF`, and `uFeliciaC`. Character-specific helper/familiar entities can share the fighter line. | Confirmed |
| 9 | `SystemGroup9` | Character auxiliary line. Runtime DTI: `uMorriganSh`, the Morrigan shadow/duplicate helper. Other character-owned helpers may use this lane in different matchups. | Confirmed captured role |
| 10 | `SystemGroup10` | Primary projectile line. Runtime DTIs: `uShotLaserSinkuHadouken` and `uShotVulcanHadouken`. Numerous static shot paths also register category 10. Shot resources may request another category, so not every projectile must use 10. | Confirmed |
| 11 | `SystemGroup11` | Secondary projectile/gameplay ordering line selected by data-driven shot/effect category fields. | Tentative |
| 12 | `SystemGroup12` | Secondary projectile/gameplay ordering line selected by data-driven shot/effect category fields. | Tentative |
| 13 | `SystemGroup13` | First gameplay-effect/model line. Runtime DTIs: `uChrChain`, `uExAtkSpaceLv1`, `uExAtkSpaceLv5`, `uGameEffectModel`, and `uGameEffect`. `sGameEffect__purgeStaleEffectsViaSUnit` scans 13-16 for gameplay effects. | Confirmed |
| 14 | `SystemGroup14` | Second gameplay-effect/model line. Runtime DTI: `uGameEffect`. Some linked effects had no active move callback during the capture. | Confirmed |
| 15 | `SystemGroup15` | Third gameplay-effect/model line. Included in the native 13-16 effect cleanup range; exact occupant line is selected by animation/effect resource data. | Confirmed region |
| 16 | `Group16` | Fourth gameplay-effect/model line. Included in the native 13-16 effect cleanup range; exact occupant line is selected by animation/effect resource data. | Confirmed region |
| 17 | `Group17` | Gap/order line after gameplay effects. No unique constant-registration role was recovered; it is available to resource-selected units. | Tentative |
| 18 | `Group18` | Character post-simulation coordinator. Runtime DTI: singleton `uCharacterPost`. Its move performs every fighter's `uCharacter__movePost`; phase 2 performs team/event finalization; phase 3 runs character and hit-solver synchronization. | Confirmed |
| 19 | `Group19` | Lighting lane. Runtime DTIs: `uToonLight`, `uInfiniteLight`, `uHemiSphereLight`, `uAmbientLight`, and `uSynchroPointLight`. | Confirmed |
| 20 | `Group20` | Post-processing/filter lane. Runtime DTIs: `uXFactorOutlineFilter`, `uColorCorrectFilter`, `uMultiBlurFilter`, `uMultiColorCorrectFilter`, `uOutlineFilter`, `uMvc3LightSpaceShadow`, `uBloomFilter`, `uRadialBlurFilter`, and `uFishEyeFilter`. | Confirmed |
| 21 | `Group21` | Character control/CPU lane. Runtime DTI: six `uChrCpu` controllers. Static code can also register character/mission action objects on `0x15`, so the line remains an ordering lane rather than a CPU-only type bucket. | Confirmed mixed lane |
| 22 | `Group22` | Action/UI transition ordering line. Static callers frequently derive neighboring child lines from a parent unit, so this is not a single subsystem. | Tentative |
| 23 | `Group23` | Battle-UI root/transition ordering region. Native UI code creates children on `parentMoveLine + 1`, making 23-31 a hierarchy of ordered UI lanes rather than class groups. | Tentative |
| 24 | `Group24` | Early battle HUD/view line. Runtime DTIs: `uBtScore` and `uViewDrawUi`. Static registrations also place training-attack and attract-mode UI here. | Confirmed mixed lane |
| 25 | `Group25` | Pause/training UI controller line. Runtime DTI in this spectator capture: `uBattleUiPauseNetGallery`. Static registrations also place `uBattleUiTraining` and offline `uBattleUiPause` here. | Confirmed |
| 26 | `Group26` | Child UI ordering line used relative to a parent move line. No unique owner; typically one level after a menu/HUD controller. | Tentative |
| 27 | `Group27` | UI text lane. Runtime DTI: `uUiAnmSpriteFont`. Other child UI objects can use this parent-relative line in different screens. | Confirmed captured role |
| 28 | `Group28` | Battle UI root/status line. Runtime DTIs: `uBtStatusGaugeManager`, `uFade`, `uBattleUi`, and `uHemiSphereLight`. Result/training-record objects also use line 28 in other states. | Confirmed mixed lane |
| 29 | `Group29` | Main HUD widget lane. Runtime DTIs: `uBtGaugeHc`, `uUiModelBase`, `uBtChrIcon`, `uBattleUiPhase`, `uUiAnmSpriteFont`, `uBattleUiTime`, `uBtAssistChrIcon`, `uUiAnmSprite`, `uBtGaugeLife`, `uBtKeyDisp`, `uBattleUiHit`, and `uOutlineFilter`. | Confirmed mixed lane |
| 30 | `Group30` | HUD child-model lane. Runtime DTIs: `uUiModelBase`, `uUiAnmSprite`, `uHitDamageModel`, `uHitModel`, `uUiAnmSpriteFont`, `uCmbTextModel`, `uMaterialControl`, and `uUiSprite3D`. | Confirmed mixed lane |
| 31 | `Group31` | HUD material/effect child lane. Runtime DTIs: `uMaterialControl`, `uUiAnmSprite`, `uUiModelBase`, `uModel`, `uUiEffectBase`, and `uMaterialVjobCngColor`. | Confirmed mixed lane |

## Runtime Entity Capture

The entity profile instruments all three traversals. For every linked node, it
resolves the node's exact DTI via `GetObjectDti` before checking whether the node
receives that phase's callback. Records are keyed by phase, category, and DTI so
the `+0x40`, `+0x50`, and `+0x48` timings remain separate.

The counters therefore mean:

| Counter | Exact meaning |
|---|---|
| `list visits` | Linked-list nodes encountered with that category and exact DTI |
| `callbacks` | Nodes that were active (`state == 2`), move-enabled (`0x400`), and not excluded by the active multi-mask before that phase's virtual was invoked |
| `calls/frame` | Callback count divided by replayed frame count; it approximates active instance count |
| callback timing | Time around only the phase virtual (`+0x40`, `+0x50`, or `+0x48`); category traversal, DTI lookup, lifecycle work, and skipped nodes are outside it |

The UI presents `list visits` and `callbacks` as averages per rollback over the
capture set, which is why those values can be fractional. `calls/frame` uses the
total replayed-frame divisor. A DTI with visits but zero callbacks was linked but
never satisfied the active move conditions during the sampled replay frames.

The same DTI appearing in multiple categories is expected. It means instances
of the same class were assigned to different scheduling lanes. Examples in this
capture include:

- `uInfiniteLight` on categories 4 and 19;
- `uHemiSphereLight` on categories 19 and 28;
- `uOutlineFilter` on categories 20 and 29;
- `uUiAnmSpriteFont` on categories 27, 29, and 30;
- `uUiModelBase` on categories 29, 30, and 31;
- `uUiAnmSprite` on categories 29, 30, and 31;
- `uMaterialControl` on categories 30 and 31.

This directly confirms that category is execution order/context, while DTI is
entity type.

### Callback Ownership

The timed `+0x40` callback is not necessarily implemented by the exact DTI
shown in the report. If a class does not override that slot, dispatch lands in
an inherited base implementation. Confirmed named implementations include:

| DTI or family | Move implementation recovered in Ghidra |
|---|---|
| `uUiScheduler` | `uUiScheduler__move @ 0x14032DA00` |
| `uMorrigan` | `uMorrigan__move @ 0x1400AC6E0` |
| fighters without a class-specific override | inherited `uCharacter__move_base @ 0x14004BD30` |
| `uChrChain` | `uChrChain__move @ 0x140035F90` |
| `uGameEffectModel` | `uGameEffectModel__move @ 0x14013C500` |
| generic effects | `uEffect__move @ 0x1405E5720` or a subclass override |
| generic shots | `j_uShot__move @ 0x1401530F0` or a projectile override |
| `uXFactorOutlineFilter` | `uXFactorOutlineFilter__move @ 0x140139750` |
| `uColorCorrectFilter` | `uColorCorrectFilter__move @ 0x14062F560` |
| `uBtStatusGaugeManager` | `uBtStatusGaugeManager__move @ 0x14030E780` |
| `uBtGaugeHc` | `uBtGaugeHc__move @ 0x1402F2AC0` |
| `uBtChrIcon` | `uBtChrIcon__move @ 0x1402EF120` |
| `uBattleUiPhase` | `uBattleUiPhase__move @ 0x1402E4940` |
| `uUiAnmSprite` | `uUiAnmSprite__move @ 0x1402C9FB0` |
| `uHitDamageModel` | `uHitDamageModel__move @ 0x1402D7BB0` |
| `uHitModel` | `uHitModel__move @ 0x1402D7D30` |
| `uUiSprite3D` | `uUiSprite3D__move @ 0x140332D10` |
| `uModel` | `uModel__move @ 0x140566580` |
| `uMaterialVjobCngColor` | `uMaterialVjobCngColor__move @ 0x140140060` |

For future captures, recording the actual function pointer read from
`node->vtable + 0x40` alongside DTI would distinguish inherited callbacks and
allow aggregation by implementation as well as by entity type.

## Reading the Supplied Profile

The reported `2.415 calls` is an average number of category-pass invocations per
rollback, matching the average replay depth. It does not mean each category
contains 2.415 objects. Each category function call then walks all active nodes
on that line.

The highest entries have direct structural explanations:

| Profile entry | Interpretation |
|---|---|
| Category 8 `move` | Main fighter/unit updates; expected dominant simulation lane |
| Category 0 `move` | Resource scheduler/filter activity, including stage schedulers |
| Category 13 phase 3 | Final synchronization for gameplay effect/model units |
| Categories 29-31 | Layered UI/model animation and synchronization work |
| Category 18 phases 2/3 | Global post-character and character/hit-solver finalization |

The UI text `sync phase 30.088 ms / frame` is visually ambiguous: it means
**sync phase 3, 0.088 ms/frame**, not a 30.088 ms phase. The same applies to
phase 2 lines.

## Limits and Next Instrumentation

The runtime table is exhaustive for linked entities encountered in this capture,
but it is not a universal list for every stage, matchup, game mode, or UI state.
Static analysis alone cannot produce that universal list because:

- many registrations pass a category read from shot, effect, stage, or UI
  resource data;
- child UI objects commonly use `cUnit__getMoveLine(parent) + 1`;
- a node's line can therefore vary by context even when its DTI class is fixed.

The phase-aware instrumentation supplies the exact match-specific DTI breakdown.
The next useful extension is to record the actual virtual target alongside each
DTI. That would distinguish inherited callbacks from DTI-specific overrides.
