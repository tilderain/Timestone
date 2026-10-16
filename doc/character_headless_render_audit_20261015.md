# Character tick headless presentation audit

Research: 2026-10-15, Ghidra instance 8192, `umvc3.exe.unpacked.exe`, image
base `0x140000000`, reconciled with current plugin hooks. No runtime
optimization or performance measurement was performed in this audit.

Follow-up: `material_headless_savings_20261015.md` traces material clocks,
controller application, wildcard job costs and historical timing bounds.

## Result

Character ticks are NOT free of rendering/resource work during resimulation.
The clearest confirmed remaining GPU path is
`uModel__updateShaderAttributes @ 0x14056A860`: it can create, map, populate and
unmap a GPU texture from inside character/model movement. The current
character-only material-post skip does not cover it.

Other remaining work includes weapon/helper material animation, character
material jobs and queued model-effect construction. These have narrower
presentation opportunities, but their clocks, ownership and callbacks prevent
classifying their complete callers as disposable rendering work.

This is a static reachability audit, not proof that every branch executes in
every match, a runtime invocation count, or an exhaustive proof over all
animation scripts, resources and indirect callbacks. No percentage or
millisecond saving is established.

## Current suppression boundaries

| Path | Current behavior |
|---|---|
| Character-only post thunk `0x1402021E0` | `hkCharacterModelPostMove` returns during resim after native character combat/status processing. |
| Joint-derived bounds `0x140568F30` | Deferred per model and flushed after resim, rather than recalculated every historical update. |
| Material reset `0x14055FBC0` | Deferred only when `IsLiveCharacterModel` recognizes the model as a team-list character or its weapon. Other models still execute native reset. |
| Base render setup `0x14055FDC0`, material update `0x140564110` | Cache repair/sanitization hooks, NOT unconditional resim skips. |
| Particle Model `0x140898730`, Filter `0x1408D61C0` | Existing resim skips; not character model movement hooks. |
| Dr. Strange persistent spell effects `0x14007B9E0` | Existing `hkDrStrangeUpdateSummons` skips this helper during resim. Summon timer handling is separate. |
| Hit-mark queue consumer `0x140131070` | Existing hook sanitizes references, then still invokes native dispatch during resim. |

The headless scheduler still processes gameplay categories 7-13, 18 and 21
(plus camera category 3), subject to existing object exclusions. Character
move, sync and post-move are separate native phases. Skipping ordinary draw
passes or presentation does not eliminate work originating in those phases.

## 1. Confirmed GPU texture work inside model movement

```text
uCharacter__move_base                         0x14004BD30
  -> model-move thunk                        0x1402021D0
  -> aliases                                0x140200CB0 / 0x140451360
  -> uModel__move                           0x140566580
       -> uBaseModel__updateViewDraw         0x1405641A0
       -> uModel__updateShaderAttributes     0x14056A860
       -> uModel__moveMotion
       -> model virtual +0x90 / +0x98
```

`0x1402021D0` is a model-move thunk, despite its older recovered setup name.
Do not confuse it with the already-hooked post thunk at `0x1402021E0`.
Weapons and helpers also reach this shared model-move path.

The shader helper reads render-context index `sRender+0x67648`, render flag
bit 29, and per-context texture slots at model `+0x1250/+0x1258`:

- With bit 29 enabled, allocates a texture object if missing, creates a
  256-by-1 texture, maps it, converts each joint's packed shader attributes to
  float4 values, writes them to the mapped buffer and unmaps.
- With bit 29 disabled, releases and clears an existing selected-context
  texture, or returns cheaply if the slot is already null.
- No motion advance, combat update or RNG call occurs in the helper body.

Scalar-reference inspection scoped to `uModel` found slot `+0x1250` accesses
in this helper, the model destructor and `uModel__setCommonState`'s render
binding path. This supports presentation isolation; it is not a complete
whole-program proof that no other code aliases those slots.

**Best next intervention:** coalesce only shader-attribute texture work per
surviving model/render context, then refresh from corrected state before the
first draw that consumes it. Keep motion, joint matrices, constraints and
model callbacks native. A bare early return without a final refresh can leave
missing/stale textures when no subsequent live movement occurs before drawing.
Validate model retirement/replacement, resource ownership, both render contexts
and renderer synchronization. Measure the enabled upload path separately from
the cheap disabled/null path; current flag frequency is unknown.

`updateViewDraw` is another CPU presentation candidate, but maintains current,
pending and control-mask state and is also explicitly used by visibility repair.
It is not a demonstrated GPU allocation/submission hotspot.

## 2. Weapon/helper material post bypasses the character thunk

```text
uChrWeapon__moveAfter                         0x140037340
  -> uActorGameModel__updateMaterialsAndCutinLight 0x140200CC0
       -> uToonModel__moveAfter              0x140451370
       -> virtual +0x230, ordinary target    0x140200B60
```

Kobun and Linlin use `uActorGameModel__moveAfterIfActive @ 0x1400A30A0`,
which also reaches `0x140200CC0`; their inspected `+0x248` callback is a no-op.
These entries bypass the existing hook at `0x1402021E0`.

This is remaining material-animation and stage/cut-in-light propagation, not
a newly confirmed direct GPU submit. Inspected material `+0x38` targets reach
`nDraw__Material__update @ 0x1405E0530`, which evaluates animation and advances,
wraps or clamps clocks. Reducing repeated material evaluation needs equivalent
clock/event behavior and correct final bindings. Do not replace all historical
updates with one ordinary delta-time update or skip weapon movement/animation.

## 3. Material jobs still execute in character movement

`uCharacter__finalizeMoveAndDispatchPostCallbacks @ 0x14004C6D0` dispatches
virtual `+0x218`; the inspected Tron vtable resolves it to
`uActorGameModel__updateMaterialVjobs @ 0x140200F50`. Weapon movement also
dispatches its material-job callback before shared model movement.

The updater traverses seven material-job tables, up to ten entries per table.
Color job `0x140140060` calls base job update, advances its clock, evaluates
channels and applies material values; repeat mode resets time and swaps
endpoints, while completion can deactivate/reset/retire a job.

`uMaterialControl__trans @ 0x14054A430` applies animation controls through
32 controller records. Controller application `0x14054A960` can search material
animation sequences, select a slot and write a wrapped animation frame.
`cFollowEyes__updateMaterialControl @ 0x140034F00` dispatches this embedded
material controller separately from the skipped character post thunk.

There is removable presentation evaluation to investigate, but whole-job skips
would omit clocks, completion callbacks and reference changes. The shared
`uCharacter__updateTeamMaterialEffect @ 0x1400419D0` also changes character
flags, a transition latch and a team callback; its name is not a safe-skip proof.

## 4. Character commands can create model/material resources

Derived ticks enqueue CPU effect commands at `0x140130FA0`:

| Producer | Command | Tick callsite |
|---|---:|---|
| Ghost Rider | `0x3EB` | `0x14008953E` |
| Iron Man | `0x3EC` | `0x14009A4A4` |
| Spider-Man | `0x3ED` | `0x1400D4C15` |
| Sentinel | `0x3ED` | `0x1400CA525` |
| Vergil | `0x3F0` | `0x1400E833D` |

These calls acquire a lock and append to a bounded CPU command queue, not a
GPU command queue. `uGameEffectPreProcess__moveAfter @ 0x140132230`, on
headless-enabled category 7, consumes it through `0x140131070`. The current
sanitizer hook does not suppress this dispatch.

The Spider-Man `0x3ED` branch provides a concrete resource/cache path:

```text
Marvel character effect dispatcher           0x140129BE0
  -> spawnModelWithoutTransform              0x140131CE0
       resource scope 150, resource number 11
  -> uGameEffectModel::setupResource          0x14013C970
  -> model-resource binding                  0x1401CD8F0
  -> uModel::setModel                        0x140568670
  -> uBaseModel::setModel                    0x140561CE0
  -> resetAllMaterial                        0x14055FBC0
  -> clearCommandCache                       0x14055B8D0
```

The new effect model is not a team-list fighter/weapon, so the existing
`IsLiveCharacterModel` reset deferral does not automatically cover it.
Native reset releases/recreates material instances, supports allocated null
fallback materials, updates lighting flags and clears command caches. This
establishes allocation/resource churn, not a measured texture-creation spike.

Command-specific deferral might avoid work, but these branches register units,
store auxiliary references and install owner-motion watches. Preserve their
occupancy and lifetime contracts. Do not discard the entire effect queue or
return null from factories merely because the spawned unit is visually named.

## 5. Smaller presentation writes

- Ghost Rider post-move updates selected weapon material colors from stage
  data through `0x140089F50` and `0x140089940`.
- Zero's tick calls saber-alpha material update `0x1400F8190` at
  `0x1400F6A6D`, enumerating weapon materials and changing their base alpha.

Inspected leaf setters search/copy existing CPU material state, without a
confirmed clone, texture allocation or upload. These are lower-priority than
the shader upload path. Preserve Zero's preceding alpha integration and weapon
motion events even if final material writes are deferred.

## Coverage and exclusions

Reviewed shared character move `0x14004BD30`, sync `0x14004C840`, post-move
`0x14004C360`, finalization `0x14004C6D0`, weapon move/post, model-move aliases,
material jobs and suspicious derived helpers. The derived-root review covered
52 named ticks in `0x140060000..0x1400F2FFF`, plus Dante, Doctor Strange,
Kobun, Linlin, Morrigan and Vergil move aliases and Zero/ZeroSh/Zombie ticks.
Additional post roots included Ghost Rider, Strange shadow, Zero and the
generic helper-model wrapper. Transitive-callee coverage was targeted, not
exhaustive; unnamed functions and resource-selected overrides remain a limit.

No additional character-local GPU filter, hair/cloth GPU submission or direct
scene/decal submission was confirmed in those expanded derived paths. This
does not prove their global absence. CPU model/joint work, collision spheres,
root motion, look/neck constraints, weapon animation and effect lifetime are
not blanket rendering skips. Several apparently visual voice paths also consume
global RNG or advance character latches/timers.

## Priority and verification

1. Measure shader helper enabled/disabled calls and texture create/map/unmap
   time; verify final-state coalescing against native visible output.
2. Measure weapon/helper material-post and material-job evaluation separately;
   identify clock/apply boundaries before omitting presentation writes.
3. Attribute model-effect creation by command/resource before considering
   command-specific lifecycle-preserving deferral.
4. Leave small material color/alpha writes until their costs justify a hook.

Compare ordinary gameplay checksums, RNG, animation cursors, joints/collision,
child liveness and first-visible material/shader state across rollbacks, tags,
assists, transformations, hyper transitions and effect retirement. Include
final refresh cost and forward-frame overhead in any performance result.

Ghidra parameter types for the eyes wrapper and common team-material helper
were corrected to existing `cFollowEyes` / `uCharacter` structs. No runtime
hooks, skip policies or profiler instrumentation were added by this audit.
