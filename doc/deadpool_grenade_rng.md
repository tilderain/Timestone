# Deadpool Somersault Grenade: global RNG and collision

**2026-09-05:** Ghidra PC executable plus read-only installed resource audit.

## Result

For the inspected installed SomersaultGrenade resource, the two global draws
control **model-joint spin**, not the translational trajectory or the collision
geometry used by the shot. This is stronger evidence than merely observing a
rotation variable: the RNG destination, constraint consumer, resource-selected
collision transforms, and movement callbacks were checked.

No runtime A/B collision trace was performed. This conclusion is specific to
these draws and resources, not a declaration that global RNG is always cosmetic
or that every rotating projectile is collision-independent.

## RNG destination and consumer

- `uShotVulcanSomersaultGrenade__ctor @ 0x140168430` constructs an embedded
  `uCnsTSR` at shot `+0x2140`. Its vtable is `0x140B7F3A0`; the ctor helper is
  misleadingly named `MtArray__method_7E80`.
- Setup `0x140168A30`, draw call `0x140168C08`: global range `[0,360]` initializes
  constraint `+0x90`, i.e. **shot `+0x21D0`, uCnsTSR.mRot.x**.
- Contact callback `0x140168550`, draw call `0x1401686BB`: global range
  `[-4.5,4.5]` adds to the bounce-indexed spin-rate table. The setter at
  `0x140169AF0` stores this at shot `+0x2214`.
- Move `0x1401688D0` reads that rate through `0x14017EA10` and advances
  `mRot.x` by rate times time scale, wrapping over 0..360. At terminal bounce
  phase it eases the same angle toward 90 or 270 degrees.
- `uCnsTSR_vfn22 @ 0x140448030` converts mRot degrees to a rotation matrix and
  applies the constraint to its bound joint matrix.
- Setup binds the constraint to the shot's model. The binding helper currently
  named `rGameEffectModel__method_C340` resolves a joint through model `+0x540`
  and `+0x538` and links the constraint to that joint. It is not a write to the
  shot's world position or movement quaternion.

Tried applying `uCnsTSR*` as the return type of the shared +0x2140 getter;
the MCP signature setter rejected the signature. Added explanatory Ghidra
comments at the two draw callsites instead. No executable bytes changed.

## Actual collision resources

Read `nativePCx64/chr/archive/0040_param.arc` without extraction or modification.
This installed archive was not authenticated against a clean Steam depot.

`rShot__load @ 0x14029F930` copies SHT file bytes after `0x68` to its data
buffer. Header `+0x60` supplies the hit-data offset; header `+0x4C` supplies the
count. `rShot__getHitData @ 0x14011A200` resolves that offset.
`uShot__setShotResource` iterates **0x60-byte records**, creates primitives, and
retains pointers to these records as collision metadata. For these resources
the hit-data offset is `0x300` (file offset `0x368`).

| Resource | Record / slot | Shape | Transform mode | Local endpoints | Radius |
|---|---|---|---|---|---:|
| SomersaultGrenade | 0 / 0 | Capsule | 0 | Both `(0,0,0)` | 12 |
| SomersaultGrenade | 1 / 3 | Capsule | 0 | Both `(0,0,0)` | 6 |
| SomersaultGrenadeExplode | 0 / 0 | Sphere | 0 | Center `(0,0,0)` | 100 |

Joint IDs are -1. There is no mode-1 or mode-3 model-joint collision in these
records. Radii are resource values before runtime size-rate application.

`uShot__dispatchCapsuleCollision @ 0x1401C92E0` selects mAtkMatrix and
mAtkBaseMatrix for mode 0. Thus the endpoints follow their translations, not
the constrained model joint. The capsule may span different current/base
positions: **zero local endpoints do not mean the moving capsule always has
zero world length**. They do mean rotating those local zero offsets does not
change the endpoints.

`uShot__dispatchSphereCollision @ 0x1401C9670` likewise selects the shot attack
matrix for the explosion's mode-0 centered sphere.

Decompressed SHT SHA-256:

- SomersaultGrenade:
  `61b3c2f9b31b6b00ff628f93722fba15c291f5dbf961b38e6c1723f7af48dfef`
- SomersaultGrenadeExplode:
  `fc2ea6d5b9ea1a83139e533e82297ebd30ddadcdd3ff2edbc04dbd47f317d0ea`

## Movement, contact, and lifetime cross-check

Grenade vtable: `0x140AA5720`.

- While bounce phase is below 5, `0x1401688D0` calls
  `uShot__advanceStraightAndUpdatePose`, then separately updates the constraint
  angle. The straight-movement path uses the shot quaternion and position and
  copies world matrices to the attack matrices.
- Movement vtable slot `+0x3E0` resolves to
  `uShot__rotateOffsetWithGravity @ 0x140153450`: computes displacement from
  movement direction, scalar speed, gravity/acceleration, and time scale. It
  does not read the constraint angle or random spin rate.
- Contact `0x140168550` resolves penetration from the contact blob, scales the
  vertical movement component by approximately -0.65, increments bounce phase,
  and stops movement after enough bounces or low vertical speed. These tests
  do not depend on the random spin rate; the random-rate assignment comes
  afterward on the continuing-bounce path.
- Boundary callback `0x140168C50` turns the movement quaternion at X beyond
  +/-875 and clamps Y to 5 in the applicable mode. It does not read mRot.x.
- Terminal callback `0x1401687F0` changes shot phase and optionally emits an
  effect using the owner's quaternion and shot position. No model completion
  or random-angle test appears there.
- Generic bounds `0x1401CE430` gets joint index -1. The grenade's getter is
  `0x140566330`, which returns shot world matrix `+0xC0` for a negative index,
  not the constrained joint matrix.

The earlier broad model-consumer inventory cited this grenade's model lifecycle
as gameplay evidence. The focused trace above does not support using that entry
as proof that these RNG-driven spin values affect collision or retirement.

## Sound-fix implication

These draws are **not a demonstrated human-versus-human collision-desync
mechanism** for the inspected grenade. A different model pose can still produce
snapshot differences, and extra audio draws still shift other global consumers
(including native offline CPU decisions). Keep those separate from a claim that
the grenade physically bounces differently because its visual spin changed.
