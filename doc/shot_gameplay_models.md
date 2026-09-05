# Shot Models That Affect Gameplay

**Status: Ghidra executable audit, 2026-08-24.**

**2026-09-05 qualification:** The SomersaultGrenade entry below is not sufficient
evidence of model-to-gameplay feedback. A focused
[global-RNG/constraint/resource audit](deadpool_grenade_rng.md) found its random
spin applied to a model-joint uCnsTSR constraint, while the installed grenade
colliders use mode-0 shot matrices with zero local endpoints. The cited terminal
callback does not read model completion. Treat that historical inventory entry
and the counts including it as qualified, not proof of spin-dependent collision.

## Scope

This inventory covers shot models whose model state is an input to gameplay. A
model qualifies when its motion completion, evaluated joint pose, or attached
model callback controls any of the following:

- Attack or collision geometry.
- Child-shot spawn position.
- Shot phase, activation, or retirement.
- Landing, bounce, or movement state.

Loading a MOD, motion list, or `uGameEffectModel` for presentation alone does
not qualify. Updating a visual model from authoritative shot state also does not
make the model authoritative; data must flow from model state back into
gameplay.

## Engine Paths

Every `uShot` inherits `uActorGameModel`, but most shots do not use evaluated
model state for gameplay. The executable exposes four relevant paths.

| Address | Function | Gameplay dependency |
|---:|---|---|
| `0x1401C92E0` | `uShot__dispatchCapsuleCollision` | `rAtkInfo::mTransformMode == 1` reads the shot model's joints; mode 3 reads an indexed attachment model's joints. |
| `0x1401C9670` | `uShot__dispatchSphereCollision` | The sphere equivalent of the model-joint collision path. |
| `0x14004CAB0` | `uModel__getMotionSlot` | Concrete shot controllers read `Motion[0]` state. |
| `0x14004BC40` | `uModelMotion__getCompletionFlags` | Completion bit 4 gates concrete gameplay transitions. |

`uShot__preMove @ 0x1401CCBE0` evaluates model state before
`uShot__updateHitboxes`, so model-driven primitives consume the current pose in
the same shot tick.

## Confirmed Inventory

The following classes have an executable-confirmed model-to-gameplay path or a
previously reproduced model-dependent collision/phase failure whose native path
is visible in Ghidra.

| DTI | Shot | Model-to-gameplay edge | Principal evidence |
|---:|---|---|---|
| `0x140D49988` | `uShotLaserDrDoomJS` | `Motion[0]` completion advances beam phase 0 to 1 and selects animation 1. | `vfn91 @ 0x14016D770`; `vfn121 @ 0x14016D870`; setup `0x14016D8E0` |
| `0x140D499C8` | `uShotLaserPlasmaBeam` | `Motion[0]` completion advances beam phase 0 to 1 and changes shot/attachment animation. | `vfn91 @ 0x14016DE70`; `vfn121 @ 0x14016DFA0` |
| `0x140D49A60` | `uShotVulcanMissileEmplacement` | Evaluated model joints choose the eight Hidden Missiles child origins. | `launchNextMissile @ 0x14016ED10`; move `0x14016F220`; setup `0x14016F5F0` |
| `0x140D4A1A8` | `uShotVulcanWeaponCart` | Model playback and cart-owned `uGameEffectModel` callbacks drive phase gates, collision motion, and the debris transition. | `updateState @ 0x14017A440`; move `0x14017A9D0`; setup `0x14017AB00` |
| `0x140D4D2B0` | `uShotVulcanKingKobun` | `Motion[0]` completion advances phases 0, 1, and 2 and changes movement/attack state. | `vfn91 @ 0x1401BA2D0` |
| `0x140D4D2F0` | `uShotVulcanKobunLauncher` | `Motion[0]` completion advances launch and recovery phases. | `vfn91 @ 0x1401BAFA0`; setup `0x1401BB540` |
| `0x140D4D3D0` | `uShotVulcanShikiDanKobun` | `Motion[0]` completion in phase 3 selects the next animation and advances to phase 4 or 5. | `vfn91 @ 0x1401BC530` |
| `0x140D4ADA0` | `uShotShieldRepulsarBlastSP` | Attack collision is generated from the shot model pose; an unbound resimulation model produces invalid geometry. | setup `0x140189C00`; generic capsule/sphere dispatch; rollback resim repair |
| `0x140D4ABB8` | `uShotVulcanLegionTiger` | Collision follows evaluated model joints; fresh reconstructed models require setup/evaluation before native hitbox generation. | setup `0x140187010`; generic capsule/sphere dispatch; reproduced rollback collision failure |
| `0x140D4C4C8` | `uShotVulcanPendulumLog` | The model resource ready transition changes world capsule endpoints for one gameplay frame, and later animation drives the capsule. | setup `0x1401A66A0`; generic collision dispatch; reproduced frame-15 transition |
| `0x140D4C528` | `uShotVulcanPrickleBoard` | Opening model animation moves the capsule and its completion activates phase 2 and live speed. | activation `0x1401A6B90`; setup `0x1401A6C40`; generic collision dispatch |
| `0x140D4B888` | `uShotVulcanDogMissile` | Model animation and actor-command binding determine the evaluated pose used by gameplay collision. | `vfn118 @ 0x140198D10`; setup `0x140198DB0`; generic collision dispatch |
| `0x140D4B980` | `uShotVulcanItemThrow` | Model-backed pose is advanced with rotation and is consumed by landing, bounce, and collision state. | movement `0x1401996E0`; setup `0x140199B00` |
| `0x140D4A4E8` | `uShotVulcanIronPipeRolling` | Rotating model pose and shot transform feed collision and ground-bounce behavior. | movement `0x14017E670`; setup `0x14017EAA0` |
| `0x140D48BB0` | `uShotVulcanFireGrenadeBomb` | Model playback/pose participates in the rotating grenade collision lifecycle. | `vfn118 @ 0x14015D3F0`; setup `0x14015D530` |
| `0x140D48CD0` | `uShotVulcanHandGrenadeBomb` | Model playback/pose participates in the rotating grenade collision lifecycle. | `vfn118 @ 0x14015E720`; setup `0x14015E860` |
| `0x140D49520` | `uShotVulcanSomersaultGrenade` | Model playback/pose participates in the thrown grenade's collision and terminal transition. | `vfn110 @ 0x1401687F0`; setup `0x140168A30` |

The inventory contains 17 confirmed model consumers. Thirteen are represented
by `SHOT_MODEL_POLICIES` in `rollback_model_policy.cpp`, while Repulsor Blast
uses a separate resimulation-time binding repair. Three direct consumers do not
have an explicit model policy:

- `uShotLaserDrDoomJS`
- `uShotLaserPlasmaBeam`
- `uShotVulcanShikiDanKobun`

This does not establish a reproduced rollback defect. The generic concrete-body
overlay already restores most pointer-free `Motion[]` scalar state for every
shot. A model policy adds stronger guarantees by explicitly reapplying playback
ranges after the pointer heuristic and pinning model/motion resources across
reconstruction. DrDoomJS and PlasmaBeam commonly use already-cached Doom
resources, so fresh setup can also reproduce usable bindings immediately.

A practical failure for either beam requires a narrow combination: rollback
must reconstruct the shot near a `Motion[0]` completion edge, and generic
overlay or fresh setup must leave the reconstructed playback on a different
side of that edge. No such desync has been reproduced. Treat these two classes
as low-priority potential hardening candidates, not confirmed coverage bugs.
ShikiDanKobun has the same explicit-policy omission, but its longer multi-phase
controller gives model playback more opportunity to affect later gameplay and
should be tested independently.

## Reviewed Exclusions

The following nearby cases load or manipulate models but do not currently have
a proven model-to-gameplay edge in the executable.

| Shot | Reason excluded |
|---|---|
| `uShotVulcanAxe` | The authoritative `+0x21D0` rotation scalar independently updates shot/collision state. The model is an output of that scalar, not the source of gameplay. |
| `uShotVulcanThrowWeapon` / `ThrowWeaponB` | `move @ 0x140178550/0x140179400` changes `Motion[0]` playback flags, but no concrete method reads model completion or joint output back into gameplay. |
| Generic beam attachment models | Several beam methods scale or animate indexed attachment models, but beam length/phase remains authoritative in shot scalars unless a concrete method also reads model state. DrDoomJS and PlasmaBeam qualify because they separately read `Motion[0]` completion. |
| Pure callback `uGameEffect` models | Visual callback ownership and lifetime can indirectly cause allocator failures, but that is not model pose driving gameplay and is outside this inventory. Weapon Cart is the exception because its model callback explicitly drives shot phase gates. |

## Independent Ghidra Audit

An independent Ghidra-only audit reviewed all 18 callers of
`uModelMotion__getCompletionFlags @ 0x14004BC40`, concrete attachment-model
users, likely model-bearing shot families, and specialized `uGameEffectModel`
callbacks. It found no additional executable-explicit model-to-gameplay shot.

The shot-specific completion-flag callers are limited to the already inventoried
DrDoomJS, Plasma Beam, King Kobun, Kobun Launcher, and Shiki Dan Kobun classes.
The remaining callers belong to characters, stage objects, or generic model
infrastructure.

### Resource-Dependent Candidates

These shots carry models and attacks, but Ghidra shows no concrete model output
flowing back into gameplay. They qualify only if external attack metadata selects
transform mode 1 or 3 in the generic capsule or sphere dispatcher.

| DTI | Shot | Static evidence and limitation |
|---:|---|---|
| `0x140D4A9F0` | `uShotVulcanFormationATiger` | Setup `0x140184BF0` loads MOD 1010 and initializes an attack slot; no motion or joint output is read by concrete code. |
| `0x140D4AA28` | `uShotVulcanFormationBHawk` | Setup `0x140185030` loads MOD 1011 and initializes an attack slot; no concrete model feedback was found. |
| `0x140D4AB70` | `uShotVulcanLegionHawk` | Setup `0x140186B80` loads the hawk model; the class has no custom move or model-state consumer. |
| `0x140D4A120` | `uShotVulcanThrowWeapon` | Model-bearing and attack-capable, but no joint-derived collision is statically proven. |
| `0x140D4A160` | `uShotVulcanThrowWeaponB` | Movement remains scalar/timer-driven; no joint-derived collision is statically proven. |
| `0x140D4C760` | `uShotVulcanMiniSentinel` | Child attacks are spawned by timers and scalar fields in `0x1401AA250`, not by model pose. |
| `0x140D4C570` | `uShotVulcanTrapLandMine` | Attack setup is independent of its callback effect; no model-joint read was found. |
| `0x140D4AF80` | `uShotVulcanAnkihouKobun` | No class-specific model-completion or joint consumer was found. |
| `0x140D4BB08` | `uShotLaserGMBeam` | Concrete code proves only model scaling and effect alignment; attack transform mode remains external resource data. |

### Additional Exclusions

- `uShotVulcanThrowWeapon__move @ 0x140178550` obtains Motion slot 0 only
  to set control flags `0x8100`. It does not read motion position or completion.
- `uShotVulcanThrowWeaponB__move @ 0x140179400` controls phases from scalar
  timers and variant fields.
- Formation C bomb/spread functions at `0x140185330`, `0x140185520`,
  `0x140185D40`, and `0x140185F10` spawn children from flags, elapsed time,
  and fixed positions rather than model pose.
- `uShotVulcanGreatSorcery_vfn93 @ 0x140159960` measures attachment joints 3,
  7, and 11 against a target, but the result only selects visual game-effect
  state `0x0B` or `0x1B`.
- `uShotVulcanJammingBombExplode__move @ 0x1401936F0` reacts to callback-slot
  disappearance, but setup `0x140193760` creates a plain EFL `uGameEffect`, not
  a model owner. Attack initialization is separate.
- `uShotVulcanAnkihouBomb_vfn91 @ 0x14018B2E0` checks an attachment slot while
  trajectory and phase remain scalar-driven; it does not consume model output.
- `uShotLaserGMBeam_vfn126 @ 0x14019C810` reads an effect-model joint direction
  but only uses it to update the visual effect. No value is written into shot
  gameplay state.
- Satellite Laser, Optic Blast, Like a Megaman, Proton Cannon, Uni Beam, Sinkuu
  Hadouken, Messatsu Gou Hadou, and Healing Field use attachment models for
  visual positioning, animation, or scale only.
- `uShotVulcanMagicMagmaSpread_vfn91 @ 0x14016C820` computes trajectory from
  shot state and copies it to the visual attachment. The flow is
  gameplay-to-model, not model-to-gameplay.
- `uShotLaserStrangeReflector @ 0x1401713D0/0x140171580` uses attachment state
  to control nested visual effects rather than shot collision or lifetime.
- `uShotLaserFlamesOfFaltine_vfn77 @ 0x140173740` supplies an owner joint matrix
  only to position an effect callback.
- `uShotLaserMysticBlast_vfn91 @ 0x1401ABCA0` drives attachment scale from shot
  lifetime rather than reading model state into gameplay.

### Effect-Model Callbacks

No specialized `uGameEffectModel` outside the confirmed inventory was found to
mutate owning-shot gameplay state.

- Hawk and Tiger effect models configure only their own model flags.
- Kobun Head configures model resource, scale, offset, and animation presentation.
- Net Trap selects visual scale from owner identity.
- Zombie advances its own position and time.
- Ragnarok Cipher respawns a child presentation effect.
- Web Ball Bind reads owner/target state to play a voice or transition the effect
  itself; it does not write shot gameplay state.

## Completeness Boundary

The executable alone cannot produce a mathematically complete per-class list of
model-joint collision users. `uShot__dispatchCapsuleCollision` and
`uShot__dispatchSphereCollision` choose their transform source from
`rAtkInfo::mTransformMode`, `mAttachmentModelIndex`, and joint IDs stored in
external `rShot/rAtkInfo` resources. Those values are not encoded in each
concrete setup function and therefore have no class-specific Ghidra xref.

This audit is exhaustive for explicit executable code paths and incorporates all
currently reproduced model-driven collision cases. Certifying that no additional
resource-only class uses transform mode 1 or 3 requires either:

1. Extracting and enumerating every `rShot/rAtkInfo` resource, or
2. Runtime instrumentation in the two dispatch functions that records concrete
   shot DTI, transform mode, attachment index, and joint IDs while executing a
   complete move corpus.

Until that resource pass is performed, the 17-class table is the complete
confirmed inventory, not proof that the external attack resources contain no
additional model-driven shots.
