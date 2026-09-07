# Global RNG and hitbox construction: focused follow-up

Ghidra PC audit, 2026-09-05. Native executable behavior, not a runtime
reproduction. Game archives were inspected read-only, in memory.

## Result

There is an **end-to-end conditional global-RNG-to-projectile-hitbox path** in
the generic projectile initializer. It is not restricted to rolling-shot landing
orientation. With a valid character RNG owner the same path uses character RNG;
ordinary versus play reaching its global fallback remains unproven.

The inspected regular character primitive transforms and capsule distance tests
do not draw RNG. This means the checked overlap math does not randomly decide
hits; it does not mean every upstream producer of its inputs is RNG-independent.

## Generic projectile spawn randomization

### Exact flow

1. `sShot__spawnShot @ 0x140150E00` reads the SHT common-data spawn parameters.
   Position-error vector is at common data +0x90; direction-error vector at
   +0xB0. It passes them to `uShot__initFromShotData @ 0x1401CD9A0`.
2. The latter invokes `uShot__initMotionController @ 0x140150390` on the
   embedded **cArm**, not on a cAtkCtrl or complete uShot.
3. cArm +0x70 (`mpSelfShot`) supplies the shot, whose +0x1308 supplies the
   RNG-owner object. The initializer makes six calls to
   `MtXorShift__lerpRandom @ 0x14014EA70`:

   ```text
   positionError.xyz = random(-authoredError.xyz, +authoredError.xyz)
   directionError.xyz = random(-authoredError.xyz, +authoredError.xyz)
   ```

   The errors are stored at cArm +0x50/+0x60 and applied to its spawn
   offset/orientation. The position and direction attachment parents at
   +0x78/+0x80 are separate from the RNG owner; a projectile attachment parent
   does not itself imply a non-character RNG owner.
4. `MtXorShift__lerpRandom` always calls the owner-aware advance helper, even
   when the two bounds are equal. Zero error still consumes a draw but does not
   introduce positional error.
5. `uShot__initFromShotData` copies the resulting position and quaternion to
   the shot, refreshes its transforms, and copies the world matrix to
   **mBaseMatrix, mOldWorldMatrix, mAtkMatrix, and mAtkBaseMatrix**.
6. `uShot__dispatchCollisionGeometryForAllSlots @ 0x1401C91C0` dispatches
   attack metadata to `uShot__dispatchCapsuleCollision @ 0x1401C92E0` or
   `uShot__dispatchSphereCollision @ 0x1401C9670`.
7. Mode-0 capsule geometry transforms the authored endpoints with mAtkMatrix
   and mAtkBaseMatrix; mode-0 sphere geometry uses mAtkMatrix. Thus randomized
   spawn translation can change actual collision centers/endpoints, not just a
   displayed model. Rotation can change nonzero local endpoints and subsequent
   movement; a centered sphere's shape is not changed by rotation alone.

The secondary initializer `cAtkCtrl__initializeChildAttackTransform @
0x140150120` is also a **cArm** method with the same six owner-aware error
draws. `uShot__initializeChildShotAttackTransform @ 0x1401CDF00` copies its
output into the same shot/attack matrices.

Applied existing `cArm*` typing and float-vector parameter types to both
0x140150120 and 0x140150390 in Ghidra, then verified decompilation. Existing
function names were retained for cross-reference stability. The partial cArm
struct still has imperfect field definitions; raw offsets remain authoritative.

### Installed resources actually use these error parameters

Read every installed `chr/archive/*_param.arc` through `read_arc`, without
extracting files. For SHT type 0x10BE43D4, common-data start is
`0x68 + header_u32(0x50)`. Found **47 SHT resources with a nonzero position or
direction error vector**. This count includes presentation/non-attacking shots,
not 47 proven gameplay/global-RNG consumers.

Examples (raw resource units; direction errors are radians):

| Shot | Position error XYZ | Direction error XYZ |
|---|---|---|
| Morrigan FinishingShower | 30, 50, 0 | ~0.698132, 0, 0 |
| Dante MultiLock | 0, 0, 0 | ~0.959931, 0, 0 |
| Magneto MagneticTempest | 0, 0, 0 | ~0.523599, 0, 0 |
| Doom MolecularShieldW/M/S and Front variants | 0, 0, 0 | ~0.069813, 0, 0 |
| Hsien-Ko Tenraiha | 50, 0, 100 | 0, 0, 0 |
| Rocket Raccoon GatlingGun | 15, 15, 0 | 0, 0, 0 |
| Trish MagicMissiles / JMagicMissiles | 100, 0, 0 | 0, 0, 0 |

These values establish authored randomness in the generic path, **not that the
normal owner reference is null**. Ex-spawn callers can also supply overrides.
Archives were not authenticated against a clean Steam depot.

### Owner routing cross-check

The checked level-3 script spawn at 0x140106376 explicitly validates the
character DTI before supplying that character as the RNG owner. MODOK's spawn
at 0x1400AB7CC and Wright's item spawn at 0x1400B1504 supply the fighter itself.
Generic child spawn at 0x1401CBE0A and Sphere Flame's child spawn at
0x14017085E inherit the parent's +0x1308 reference rather than supplying the
parent projectile itself. None of these inspected paths establishes an ordinary
non-character RNG owner.

The global condition remains null/non-character at the dispatcher. The earlier
owner-retirement/cleanup investigation remains relevant, but has not supplied
an ordinary-match reproduction. This distinction must remain in any conclusion.

## Regular fighter geometry and narrow-phase math

Inspected:

- `uCharacter__updateAttackAndHurtSpheres @ 0x14004B7E0`: active-state and
  flag gating, primitive virtual updates, collision-node registration.
- `uCharacter__updatePushSpheresAndQueueCorrection @ 0x14004B9D0`: updates
  push primitives and queues the push correction callback.
- `cHitPrimCapsule__updateWorldEndpointsFromParents @ 0x14000FFD0`: reads
  parent world/joint matrices, transforms local endpoint offsets, optionally
  flattens Z. No random draw.
- `cHitPrimSphere__updateWorldFromOwnerWithBase @ 0x140010210` and
  `cHitPrimSphere__updateWorldFromOwner @ 0x140010380`: analogous world/joint
  transforms and optional Z flattening. No random draw.
- Capsule overlap implementation `0x140469320` and its capsule/sphere helper
  currently named `MtCollision__planeSphereVsSphere`: segment lengths,
  closest-distance calculations and summed-radius comparisons, not RNG rolls.
  Several math helper names/signatures are misleading; decompiled SIMD copies
  also have artifacts, so do not treat the pseudocode as a clean reimplementation.
- `nCollision__testNodeGeometryPairs @ 0x1405AB9A0`: rechecked ID/mask,
  bounds and primitive dispatch; callbacks remain a separate boundary.

World/joint poses, active attack state, and callbacks are inputs to this math.
Their entire transitive dependency graph has not been proven RNG-independent.
Some functions previously named `updateHurtboxes`/`setHitboxParams` actually
lead into draw/model handling; names alone are not evidence of collision work.

Projectile dispatch also has a flag-selected camera-origin projection path.
It reads camera slot 0's **uCoord position**; simply observing global RNG in
the separately named `sCamera__jitterStep` is not proof that it changes that
position. No shake-to-collision edge was established in this pass.

## More global draws separated from hitbox inputs

Judgment Cut `uShotVulcanJigenZan__move` passes **the shot itself** to the
owner-aware helper. Thus these draws use global RNG even with a live character
at shot +0x2030. The output goes to slash-effect orientation, not shot position
or quaternion. All eight installed JigenZan/DevilJigenZan/assist SHTs use a
mode-0 centered sphere: radius 80 normally, 120 for Devil variants. They do not
read the random slash-effect pose to construct that sphere.

Additional read-only collision metadata checks:

| Family | Installed hit geometry |
|---|---|
| Dante Crystal1..4 and AssistCrystal1..4 | Mode-0 capsule, radius 40. First three centered; fourth has both local offsets (0,10,0). No effect/model-joint mode. |
| Doom MolecularShield W/M/S and Front | Two mode-0 centered capsules, radii 30 and 20. Pre variants have a mode-0 centered capsule, radius 150. |
| Amaterasu Hyoran | Two mode-0 capsules, endpoints (0,0,50) and (0,0,-40), radius 20. |
| Sentinel DirectAttackBomb / Explode | Zero hit-data records in these two SHTs. Does not rule out children or scripted attack creation. |

Crystal and Molecular Shield's global setup draws choose EFL effect masks.
Their installed hit geometry does not directly read those randomly selected
effects. Effect lifetime/callback behavior is not fully cleared by that finding.
In particular, Molecular Shield's separate **cArm direction error** must not be
confused with its **global EFL variant draw**.

## Bottom line

- Global RNG is confirmed gameplay-relevant for native offline CPUs.
- Generic projectile hitbox inputs are conditionally global-RNG-dependent;
  the complete initializer-to-collider connection is now traced.
- No direct RNG draw was found in the inspected regular primitive transforms
  or capsule overlap math.
- Ordinary two-human global-RNG-caused hit divergence is still not established.
  This is not a proof that it cannot occur, nor authorization to discard shared
  RNG state. No runtime instrumentation, executable patch, or game-file change
  was made during this pass.
