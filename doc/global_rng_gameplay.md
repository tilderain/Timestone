# Global RNG versus gameplay RNG

Ghidra recheck, 2026-09-05, PC `umvc3.exe.unpacked.exe`.

## Verification status — 2026-10-26

Read-only native recheck on **Ghidra port 8193**,
program `umvc3.exe.unpacked.exe` (preferred image base `0x140000000`). Existing September
consumer/resource evidence below is retained, not represented as a fresh full
consumer census or archive audit.

Fresh bodies: owner dispatchers `0x14014EB80` / `0x14014EAB0`, plus audio
allocator `0x1402A21F0`, queue processor `0x1402B0E00`, request wrapper
`0x1402A2A10`, and variant selector `0x1402A9110`. The key distinction is
**stream selection versus synchronization**: a non-null non-character owner
still selects global RNG, but under the wrapper's critical section; a null owner
selects global RNG without entering that wrapper lock. Neither wrapper proves
the entire global stream cosmetic or all its downstream users synchronized.

Current-source checks are limited to `rollback_rng.cpp`, `rollback_hooks.cpp`,
and the relevant region-builder portion of `rollback_snapshot.cpp`. The latter
actively adds the 16-byte global state at `0x140D765D8`; its nearby broad comment
about hit rolls/damage variance is not independent native evidence for those
uses. No runtime test, patch, build, or Ghidra layout/name mutation was performed.

## Conclusion and limits

The global stream is **not exclusively cosmetic**: native offline CPU action
selection and script wait timing consume it. It must not be discarded as
presentation-only state. However, this audit does **not** establish that changing
the global stream changes ordinary human-versus-human hitbox overlap results.
Several prominent combat random choices instead use character-local RNG.

The [focused hitbox follow-up](global_rng_hitbox_followup.md) traces the generic
cArm spawn-error draws all the way into projectile collision matrices, audits
regular fighter primitive transforms, and checks additional installed collision
resources. It establishes an end-to-end **conditional** global-RNG/hitbox
dependency, but not an ordinary two-human reproduction of the global fallback.

## Verified routing

- Global state: `0x140D765D8`, four 32-bit words; getter `0x1400046D0`.
- `MtXorShift__dispatcherNext @ 0x14014EB80` and
  `MtXorShift__lockedAdvanceForCharOrGlobal @ 0x14014EAB0` select the supplied
  object's `+0x2D80` state if it is kind-of the character DTI at `0x140D44A30`.
  Null or non-character objects fall back to the global stream.
- Both wrappers enter/leave the critical section only when the owner is non-null,
  including the non-character/global fallback. The null/global route has no
  wrapper-level lock. `0x14014EB80` returns an integer next value;
  `0x14014EAB0` calls nextFloat01. Existing function comments that simply say
  "CS-protected" omit this null-route distinction.
- Inclusive integer helper `0x14014EB30` calls that owner-aware float dispatcher.
- `MtXorShift__floatInRange @ 0x140455550` and
  `MtXorShift__intInRange @ 0x140455590` use the global stream directly.

## Concrete consumers

| Consumer | Verified effect / RNG domain |
|---|---|
| `rChrCpu__randomMod @ 0x14010FE50` | Offline calls global getter and next, then returns modulo. Native live-online branch uses the associated object and owner-aware dispatcher instead. |
| `rChrCpu__updateRandomActionSelection` (calls at `0x140115B82`, `0x140115C02`, `0x140115C19`) | Uses randomMod results to choose stored action-selection state and output bits. Gameplay-relevant CPU decisions, not sound. |
| `rChrCpu__processRandomWaitOpcode` (call `0x140118864`) | Uses randomMod to initialize a countdown; advances the script when the countdown expires. Gameplay timing. Corrected its receiver type from sMain to rChrCpu in Ghidra. |
| `uHsienKo__selectReservedAnkiVariant @ 0x1400A1260` | When no reserved variant exists, draws 0..15 with `this` as RNG owner: character-local. |
| `uPhoenixWright__randomizeEvidenceSlot` (calls `0x1400B1E35`, `0x1400B1ECE`) | Chooses and writes evidence slots using `this` as owner: character-local. |
| `cChr__populateHitReactionResult` (call `0x140048CCC`) | Offline random-counter-hit branch rolls modulo 100 through dispatcher with `this`: character-local for a normal fighter. Result bit 4 selects attack data +0x58 instead of +0x54. Native online skips this roll. Not evidence of global RNG affecting counter-hits. |
| `uShotVulcanMysticStare__selectUnusedVariant` (call `0x1401AD68C`) | Dispatches through the owning object obtained at +0x1308; ordinarily character-local, not directly global. |
| `sEarthAtk__selectRandomRoundEvent @ 0x140275EC0` | Global draws select campaign/round event paths. Not proof of ordinary versus collision effects. |

## Collision check

Inspected `nCollision__testNodeGeometryPairs @ 0x1405AB9A0`. Its body checks
collision ID inequality, mask overlap, bounding boxes, and primitive intersection
dispatch, then invokes callbacks. There is no direct RNG draw in this function.
This is not an exhaustive proof about every callback, primitive implementation,
or upstream producer of geometry.

RNG can influence gameplay by selecting actions or timing even if the overlap
math itself is deterministic. Conversely, finding RNG in a projectile method
does not establish a trajectory or collision effect: follow its output.

### Misleading names encountered

`uShot__lookupAttackInputSlot @ 0x1401CD5A0` forwards to `0x1401CD630`, which
reads rShot slots, calls `sMvc3Manager__getGSound` / `FUN_1402AE7F0`, and stores
sound references. Thus the random 0/1 arguments in Formation A Tiger setup
`0x140184BF0`, Formation B Hawk setup `0x140185030`, and FallRock vfn100 are
**not evidence of random attack hitbox selection**. The existing name is
misleading; this audit did not rename it globally.

Deadpool SomersaultGrenade has global draws at setup `0x140168A30` and contact
callback `0x140168550`. The examined output feeds a rotation-rate/rotation path
used by `0x1401688D0`, rather than proving random translational bounce velocity.
The follow-up [grenade RNG audit](deadpool_grenade_rng.md) traces the destination
to an embedded uCnsTSR model-joint constraint and checks the installed SHT:
mode-0 capsules with zero local endpoints, independent of the constrained joint.
The inspected movement and terminal callbacks also do not use this random spin
to choose trajectory or lifetime. These draws are not a demonstrated collision
change for this resource; see the follow-up for evidence and scope.

## Follow-up: additional combat-looking consumers (2026-09-05)

Continued through global getter and global integer/float-range callers in the
character, projectile, and script code. **No additional confirmed ordinary
human-versus-human gameplay dependency was established in this pass.** This is
a bounded static audit, not an exhaustive proof of absence or permission to
discard the global state.

### Contact orientation is not knockback

`cAtkCtrl__computeShotDirection_14000cc40` draws a global float at
`0x14000CE2D` when the attack entry's angle range at +0x170 is nontrivial.
Despite its name, the checked call chain is:

```text
0x14000CC40 randomizes an output direction
  -> uZeroSh__buildShotAimVector @ 0x14000CAA0
  -> cChr__hitResolveCallback / uShot__processHitCollision
  -> cChr__resolveAttackContact(contactRotation)
  -> cChr__commitAttackContactResult
  -> sGameEffect__stateUpdate_FA0
```

In that normal-hit chain, contactRotation is forwarded to effect generation,
not the damage/knockback computations. Also checked the LightWall, mutual-hit
versus character, and mutual-shot-priority callers: their output likewise goes
to effect spawning. This does not audit every effect's internal callbacks, but
it rules out citing the helper name as direct random trajectory evidence.

Corrected `0x14000CAA0`'s receiver type to `cAtkCtrl*` and source argument to
`uModel*` in Ghidra; verified the applied signature. An attempted RNG callsite
comment failed, so the evidence is recorded here.

### Other checked consumers

| Function | Observed use of the global random result |
|---|---|
| GreatSorcery `0x140159BB0` | Chooses sound variant, randomized sound position around the camera, and the next sound timer. Not a child projectile spawn. |
| Kobun controller `0x1401B9760` | Chooses voice ID `0x5A/0x5B`. |
| KobunLauncher setup `0x1401BB540` | Chooses voice ID `0x5E/0x5F`. |
| ShikiDanKobun move `0x1401BC530` | Chooses voice ID `0x5C/0x5D`. |
| ShikiDanKobun setup `0x1401BCDA0` | Chooses 0/30/60 timer value; the move routine uses it to gate the voice request, not the model-completion phase transition. |
| Crystal setup `0x1401631B0` | Chooses an effect resource bit mask `1 << random(0,2)` on a newly created uGameEffect. No trajectory write identified from this result. |
| Countdown `0x140009670` | Chooses an announcer/voice queue variant modulo 5. |

Crystal's effect-lifetime feedback and other resource-dependent effects are not
fully proven cosmetic by this table. MolecularShield setup also selects an
effect variant and remains a resource/callback-dependent boundary, not a new
verified gameplay result.

### Character streams do not draw their round seed from the advancing global state

Rechecked `sReplay__onRoundStart @ 0x1401CFAD0`: global RNG, sEffect RNG, and
each character are seeded separately from the same stored replay/round seed.
It does not call global-next repeatedly to derive character seeds. Therefore
the shared initialization seed is not evidence that an extra sound draw changes
the character streams during the round.

The verified positive evidence remains native offline CPU action selection and
random-wait timing, plus mode-specific campaign/selection behavior. Additional
work would require auditing remaining owner-dispatch fallbacks, effect callbacks,
and inlined/aliased state accesses, then runtime traces in two-human matches.

## Owner-aware gameplay callers and conditional global fallback

Further pass on 2026-09-05: enumerated all **22 direct call references** to
`MtXorShift__randomIntRangeInclusive @ 0x14014EB30` in the current database and
inspected their RNG-owner arguments. None explicitly passes a literal null owner.
That is not a guarantee that the dynamically obtained owner is non-null.

Additional concrete gameplay consumers:

| Function | Random output and supplied owner |
|---|---|
| AcidRain child spawn `0x140166150` | Draws 0..15, writes a float to child +0x1F5C and enables the associated state through 0x140107960. Supplies shot +0x2030 as RNG owner. |
| SpreadArrow child spawn `0x1401821C0` | Draws 155..160 for an angle, -350..-100 for a position component, and 0..15 for child state. Supplies shot +0x2030. |
| IronPipeRolling landing `0x14017E670` | Selects a sign and 30..45 degree Y rotation, multiplies the shot orientation, writes shot quaternion +0x60, and refreshes attack matrices. Supplies shot +0x1308. |
| AnkihouRolling landing `0x14018CB60` | Similar owner-aware orientation change, not merely the separate model-spin constraint inspected for Deadpool. Supplies shot +0x1308. |
| ItemThrow landing `0x1401996E0` | Owner-aware randomized landing orientation. Supplies shot +0x1308. |
| ShockingPinkBomb `0x1401C2780` | Random sign on a near-zero averaged-direction branch. Supplies shot +0x2030. |

The normal character-associated route uses character-local RNG. **However, if
one of these supplied pointers is null or fails the character DTI check, the
same gameplay calculation consumes global RNG.** This is a verified conditional
code dependency, not a reproduced ordinary-match global-RNG dependency.

The condition is worth investigating rather than dismissing: `uShot__preMove`
checks linked objects for engine alive state and clears invalid references;
when its request-unit field is zero it explicitly clears shot +0x1308. Whether
a relevant rolling shot survives that cleanup and subsequently executes a
randomized landing has not been established. Character KO is not automatically
equivalent to failing the engine object-alive check.

Disassembly corrections used for this audit:

- The misleadingly named `cChrInput__getInputBuffer3 @ 0x1401CC5D0` returns
  **shot +0x2030**.
- `MtObject__getField_4872 @ 0x1400803B0` returns **object +0x1308**.
- `uShot__setRequestUnit @ 0x140151770` actually writes **+0x2028**. Do not
  infer that it initializes +0x2030 from its current name or struct member name.

Also checked random recovery, advanced guard, throw escape, and variable-counter
transition branches at 0x14003EF30, 0x14003F1E0, 0x140040000, 0x1400408A0,
0x140040E20, and 0x140041790. All supply the character itself as RNG owner;
the inspected random-setting branches are additionally offline-gated. They are
not new evidence for global RNG affecting normal fighter reactions.

The database has zero xrefs to the interior global-state words +4/+8/+12
(0x140D765DC/E0/E4). This narrows obvious direct inlined accesses but does not
rule out aliases, computed pointers, or missing analysis.

### Owner lifetime follow-up (2026-09-05)

Resolved the initialization ambiguity: `uShot__setOwnerCharacter @ 0x1401CD970`
mirrors the supplied pointer to both **+0x1308 and +0x2030**. This is distinct
from `uShot__setRequestUnit @ 0x140151770` (+0x2028) and
`uShot__setOwner @ 0x140151760` (+0x2000). Existing member/function names are
inconsistent; use offsets when tracing these relationships.

`uShot__onOwnerChange @ 0x1401C5ED0` replaces +0x2000, updates +0x2008/team
membership for a character, and resynchronizes collision IDs. It does not
replace +0x1308/+0x2030. Thus this inspected reflection/owner-change handler is
not evidence of switching the RNG stream to the reflector or to global RNG.

`MtObject__isAliveState` tests an engine lifecycle tag (low three bits equal to
1 or 2), not health. The inspected KO processor
`uCharacterPost__processDefeatTransitions @ 0x14005CF20` works with fighter
flags at +0x14F0/+0x1500/+0x1504, team state and defeat effects; it does not
establish that KO clears this engine lifecycle tag. Do not equate KO and object
retirement.

The generic preMove cleanup can clear +0x2030 and +0x1308, but the explicit
owner-invalid destruction branch separately tests +0x2000 and
`shotFlags(+0x1FE8) & 0xA817F`. `uShot__setShotResource` obtains +0x1FE8 from
SHT common data +0x10. Read-only installed-resource inspection found this value
is **zero** for:

- `0019_param.arc`: `chr\\Haggar\\shot\\IronPipeRolling`.
- `0023_param.arc`: all twelve `chr\\Naruhodo\\shot\\ItemThrow*` resources
  (Photo, Bonsai, Bottle, Clock, Envelope, Glasses, Knife, Kobun, MobilePhone,
  RubberCup, Tonosaman, Vase).

So that particular generic branch does not itself guarantee these shots are
destroyed when their combat-owner object becomes invalid. Other cleanup,
resource-flag changes, active-state checks, or round transitions could still
remove them first; the installed archives were not clean-depot authenticated.

The inspected IronPipe, AnkihouRolling, and ItemThrow landing bodies have no
explicit null-owner early return immediately guarding their RNG calls. Their
landing gates use height, count, and variant/bounds instead. Together these
findings make the fallback a concrete conditional lifecycle path worth testing,
but **do not demonstrate that ordinary KO/tag/reflection reaches it**.

The next discriminating runtime observation is the lifecycle tag and actual
RNG-owner pointer at the landing draw, not just whether the fighter has zero
health. No runtime probe or patch was installed during this audit.

## Caller census follow-up — 2026-11-13

Read-only pass on Ghidra port 8192 (`umvc3.exe.unpacked.exe`). Enumerated all
53 data refs to `0x140D765D8`, all 74 calls to getter `0x1400046D0`, all 37
calls to `floatInRange 0x140455550` and 20 to `intInRange 0x140455590`, then
inspected every caller in the fighter/shot/script range not already covered
above. No Ghidra mutations.

### Round seed chaining (most important new edge)

`sReplay__onRoundStart @ 0x1401CFA90` reseeds global, sEffect, and every
character stream from `sReplay+0x23424`. Writers of that field (all through
`sReplay__setField_144420_locked @ 0x14000B350`):

| Writer | Source of seed |
|---|---|
| `sAction__executeMatchPhase` call `0x14000604A` | **Global next**, gated by `g__isTrainingMode` (game mode 5) after clearing replay flags. |
| Char-select/battle-start code at `0x140369D42` (no Ghidra function) | **Global next** (offline match start). |
| `cNetChrSel__applyCharacterSelectionMessage` `0x140263065`, and `0x140262DB4` | Network message field (+0x28 / +0x40): online seed is peer-synchronized, not local global state. |

So offline/training, the global stream at match/phase start decides the next
round's **character-local** seeds too, so any drift in the global stream
(e.g. an extra audio draw) carries into character RNG on the next reseed.
Online, the seed comes from the network, which breaks that chain at round
boundaries.

### New consumers classified

| Consumer | Use of global result | Class |
|---|---|---|
| `uCharacter__selectProfileVariant @ 0x140055B50` → `0x14005BF70` (percent roll vs rChrProfile row byte +1) and `intInRange` at `0x140055D5B` | Picks intro (kind 1) / win (kind 2) profile rows. Callers: `sAction__enterFightingPhaseSetup`, `executeMatchPhase`, `anmchrCmd1_interpreter`. | Pre/post-round presentation. Intro choice is not shown to alter in-round state. |
| `anmchrParser__parse3Level` opcode at `0x1401049ED` (6 × floatInRange) | Random position offset + rotation (degrees × `0x140A6A6C4`) → `0x140103720` transform → `sAnmchrParser__spawnLevel3CommandEffect @ 0x140120FE0`. Dispatchers only create sGameEffect/uGameEffectModel/sound. | Effect placement (cosmetic). |
| `anmchrCmd4_6_stageInterpreter` case 9 (`0x14010840A`) | Random float → stage-object motion slot float +0x5C. Only on uStage/uStageObject. | Stage cosmetic. |
| `uRyu__tick` `0x1400C86A7..E3` | Random offset of a sound position (`0x1402AAB10`). | Sound. |
| `uThor__tick` `0x1400E0102` | One-shot `&1` into +0x6910, choosing voice 0x4D vs 0x6F. | Voice. |
| `0x1400D7984`, `0x1400806DB`, `0x140080A28` | `&1` voice-ID variant → `sGSound__playPersistentVoice`. | Voice. |
| `0x140187750` (vtable `0x140ABC9D0`) | Random Y rotation for effect 0x3E8 spawn position; then 0/1 into `uShot__lookupAttackInputSlot` (sound, see misleading-names note). | Effect + sound. |
| `0x14018DC50` | `&1` → `uBaseModel__setPartsDisp`. | Model part visibility. |
| `uShotVulcanDirectAttackBomb` init `0x1401A99F0` (`0x1401A9B58/7A`) | Random values into embedded uCnsTSR (+0x2140) spin at +0x21D0/+0x21D8. Same pattern as Deadpool grenade; this SHT has zero hit records (see hitbox follow-up). | Model constraint. |
| Doom effect init `0x14016FBEA` | `1 << random(0,2)` effect mask on new uGameEffect (`chr\DrDoom\effect\efl\`). | Same as MolecularShield EFL variant. |
| `0x1401BD3CF` | `random(0,2)*30` into +0x2170. ShikiDanKobun voice-gate timer already listed above. | Voice timing. |
| `uChain__simulateGroup`, `cpChain__allUpdate`, rCloud*, uTVNoise*, uGrassWind, uShadow, `sEffect__cloudJitterStep` | Direct reads of the state words. | Cloth/hair, stage, post-FX. |
| `sEffect__getRandomU32 @ 0x140543B70` | Uses sEffect's own +0x3C state when arg≠0, global when arg==0. | Effect RNG (cosmetic consumers). |
| `MtNetSession__getNonce @ 0x14050B900`, `nNetwork__ProtocolStandard__encodeFrame` `0x14094040C` | Session IDs and packet encoding draw from global. | **Non-simulation advance** during native networking; desyncs the stream between peers if native net runs. |

### Owner-aware callers that pass a non-character owner

Checked the owner argument at every call to `lerpRandom 0x14014EA70` (45),
`lockedAdvanceForCharOrGlobal 0x14014EAB0` (14 direct), `dispatcherNext
0x14014EB80` (8), and `randomIntRangeInclusive 0x14014EB30` (22). Almost
all of them pass shot +0x1308, shot +0x2030, or the fighter `this`, so they
are character-local in normal play. These ones pass a non-character owner and
**always** draw from the global stream:

| Caller | Owner | Output |
|---|---|---|
| `uShotVulcanJigenZan__move @ 0x1401C02E0` (6 draws) | the shot (R14 = this) | Random Euler → `sGameEffect__spawnFromQuaternionPosition`: effect only (already known). |
| `uShotVulcanHyoran__setup @ 0x140154420` (`0x140154633`) | the shot | `random(0,360)` → embedded uCnsTSR(+0x2140)+0x98, i.e. model-joint spin. Hyoran capsules use mAtkMatrix, not the joint, so this is cosmetic. |
| **`anmchrCmd0_interpreter` opcode 0x46 (`0x14010308A`)** | interpreter host `mpOwner` (+0x38) | `randomIntRangeInclusive(a,b)` → **`mCmpS32`** (`cActorGameSpriteAnm__setCommandValueAt7C`). |
| **`anmchrCmd0_interpreter` opcode 0x47 (`0x1401030BB`)** | interpreter host | `lerpRandom(a,b)` → **`mCmpF32`** (+0x80, via folded setter `0x1406D9AB0`). |

Jump table `0x140103188`, 75 entries: index 70 → `0x10308A`, index 71 →
`0x1030BB`. The interpreter host is the `uActorGameModel` that owns the
cActorGameModelAnm. Its command blocks run from `uShot__preMove`,
`uGameEffectModel__restart`, `uStageObject__move`, and `uZeroSh__tick` (via
`cActorGameModelAnm__bindActionCommandState`), and the ctor is shared by uShot,
uKobun, uLinlin, uChrWeapon, uGameEffectModel, and stage objects. **When a shot (or other
non-character actor) script executes 0_46 / 0_47, the draw comes from global
RNG and lands in the compare register used by the script's conditional jumps**
(`FUN_140100C70` int compare / `FUN_140100B90` float compare). That is a
direct global → shot-behaviour branch path, with no owner fallback needed.
The same opcodes on a fighter are character-local.

`anmchrCmd1_interpreter`'s random opcode (`0x1400FEB98`) casts the host to
uCharacter first and writes fighter fields (+0x4110/+0x40E8), so it is
fighter-only and character-local.

Still open: which installed shot/actor anmchr resources actually contain
0_46/0_47. That needs an archive scan of `chr/archive/*_param.arc` (not done:
the game install is outside the sandbox for this pass). Morrigan's
FinishingShower code itself (`setup 0x1401961D0`,
`computeSteeredNextPositionAndDelta 0x140196690`) has no direct RNG; its
spread comes from cArm spawn error with the owner's +0x2D80 (or the
Astral Vision `uMorriganSh`, whose DTI parent is uCharacter, so also local).
Any FinishingShower global dependency would therefore have to come from
its anmchr script using 0_46/0_47.

### Updated bottom line

- The new unconditional gameplay path is anmchr command 0_46/0_47 run by a
  shot or other non-character actor: a global draw into the script compare
  registers. Which resources use it still needs an archive scan.
- Otherwise the gameplay-relevant global consumers are offline CPU AI
  (`rChrCpu__randomMod`), the conditional owner-fallback paths, and
  campaign/menu selection.
- Global drift matters indirectly in offline and training modes, through the
  round reseed above.
- Many cosmetic draws (sound, effects, chains, stage, native network) advance
  the shared stream. That is why the mod snapshots/restores the 16 bytes at
  `0x140D765D8` (`rollback_snapshot.cpp` BuildRegionsUncached,
  `rollback_restore.cpp` restore). Discarding the state would leave the
  conditional gameplay consumers exposed to presentation-dependent drift.

## Relevance to pain sounds and current hooks

Both `sGSound__selectRandomVoiceVariant @ 0x1402A9110` and bank-side
`sSound__pickVoiceVariant @ 0x1407F93B0` consume global RNG. The latter explicitly
calls next on `g_MtXorShift__globalState` for weighted selection.

`UMVC3Hook/code/rollback_rng.cpp::hkSoundRandomVariant` saves/restores global
state only while Netplay_IsActive. Its existence does not isolate every bank
selection: restoring the previously rejected group path reaches the lower bank
selector without necessarily passing through this wrapper. The listed
presentation callsites do not establish coverage of the bank picker itself.

The current CPU hook also differs from native: during netplay it returns
`rand() % modulus`; the save/restore code below that return is unreachable.
Therefore native Ghidra findings and current hooked runtime behavior must be
distinguished. No hooks were changed here.

An extra audio draw can shift subsequent global consumers, including offline CPU
decisions. It does not directly advance Phoenix Wright or Hsien-Ko's separate
character streams. For a sound-only fix, preserve/isolate audio RNG and test
offline/replay as well as online. A universal human-versus-human desync claim is
not established by this static audit.
