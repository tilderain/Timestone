# Projectile Ownership And Rollback

**Status: Current focused reference. Verified against Ghidra and source on
2026-07-23.**

This replaces the former fixed-array proposal. That proposal treated projectiles
as uniform byte blobs and did not account for polymorphic DTI allocation,
resource ownership, collision arrays, inline objects, effects, or scalar
deleting destructors.

## Native Manager

| Address | Meaning |
|---|---|
| `0x140D47F40` | Storage for the `sShot*` singleton |
| `0x140D47F90` | Global array of 10 list sentinels, stride `0x28` |
| `0x140D48120` | Global array of 10 `CRITICAL_SECTION` objects, stride `0x28` |

The sentinel and lock arrays are globals adjacent to the singleton-pointer
storage. They must not be described as fields at `sShot+0x50` or as overlapping
objects. `sShot__addShotToList` and `sShot__removeShotFromList` index the two
arrays independently.

Verified manager fields used by current source include:

| `sShot` offset | Meaning |
|---|---|
| `+0x38` | Shot-ID/manager state included in the current `0x50`-byte manager snapshot |
| `+0x438` | Kill-order/state scalar region |
| `+0x444` | Delta-time coefficient |
| `+0x448` | Pending-unit array, 512 entries of `0x10` bytes |
| `+0x2448` | Pending-unit count |
| `+0x2450` | Activity records |

The exact semantics of every manager-header word are not fully recovered. Do not
infer a 1024-object capacity from the size of the ID state.

## Intrusive List Layout

Each concrete `uShot` contains a list node at `shot+0x1450`:

```text
+0x1450  previous node
+0x1458  next node
+0x1460  owning uShot pointer
```

Each list sentinel contains the same node shape beginning at `sentinel+0x08`.
The list is circular when initialized. Current source traverses it as:

```cpp
uint64_t sentinel = _addr(0x140D47F90) + list * 0x28;
uint64_t end = sentinel + 0x08;
uint64_t node = *(uint64_t*)(sentinel + 0x10);

while (node && node != end) {
    void* shot = (void*)(node - 0x1450);
    node = *(uint64_t*)(node + 0x08);
}
```

Relevant native functions:

| Address | Function | Role |
|---|---|---|
| `0x140150AD0` | `sShot__addShotToList` | Lock and insert embedded node |
| `0x140151420` | `sShot__removeShotFromList` | Lock and unlink embedded node |
| `0x140151160` | `sShot__getFirstShotInSlot` | Return the first node/entry representation |
| `0x140150E00` | `sShot__spawnShot` | Standard native factory path |
| `0x140150F40` | `sShot__spawnShotEx` | Extended native factory path |
| `0x14029F7F0` | `sShot__createUShot` | Resolve concrete DTI and invoke its factory |
| `0x1401C9FD0` | `uShot__dtor` | Unlink and release owned state |
| `0x1401CA090` | `uShot__operatorDelete` | Return memory to the MT allocator |
| `0x1401C5F20` | `uShot__onOwnerChange` | Move a shot between team/list slots |

Direct list access is narrow but not isolated behind one interface. Several
gameplay routines call `sShot__getFirstShotInSlot` and continue through
`cChrNode__getNext`. Other generic engine systems retain ordinary `uShot*` or
`cUnit*` pointers.

## Object Allocation

Shots are not one fixed-size class. `sShot__createUShot`:

1. Reads the concrete class hash from the `rShot` resource.
2. Resolves an `MtDTI` with `MtDTI__findByHash`.
3. Calls the DTI virtual factory.
4. Returns a variable-sized polymorphic object.

Runtime size is decoded from DTI metadata. Concrete classes can extend beyond
the common body beginning around `0x2140`; the current snapshot code accepts
sizes up to `0x40000` but does not assume all of those bytes are flat gameplay
state.

Known ownership-bearing contents include:

- `cUnit` lifecycle and category links.
- The `sShot` intrusive list node.
- Inline `cAtkCtrl`, `cArm`, model, animation, and other `MtObject` records.
- Resource references and attack data.
- Four collision-slot pointers near `+0x2058`.
- Owner, target, parent-shot, reflected-owner, last-hit, and requested-unit
  references.
- Concrete subclass state and, for some classes, inline or external effects.

Native destruction must run to release those owners correctly. Calling the
scalar deleting destructor with flag 1 also calls `uShot__operatorDelete`.

## Current Rollback Strategy

The native allocator and native lists remain authoritative.

### Capture

Hooks on `sShot__spawnShot` and `sShot__spawnShotEx` record a `ShotRecipe`:

- Spawn function and arguments.
- Resource and owner/reference identities.
- Slot ID and spawn order.
- Pre-spawn RNG states.
- Verified common flight/gameplay ranges.
- The pointer-free `uEffect` timing scalars at `+0x110..+0x11F`, including
  `mBaseFps`, `mDeltaTimeRate`, and `mTimeInterpolationRate`.
- The four common shot matrices at `+0x1E50..+0x1F4F` (`mBaseMatrix`,
  `mOldWorldMatrix`, `mAtkMatrix`, and `mAtkBaseMatrix`).
- A runtime-DTI-sized `ConcreteShotSnapshot` used for an ownership-filtered
  overlay of the common body and concrete tail.
- Collision primitive gameplay state.
- Collision primitive parent identity when the parent is the shot itself or a
  stable roster character. Reconstructed self references are remapped to the
  fresh shot address.

Only recipes for shots still linked in `sShot` are copied into a frame snapshot.
Terminal/deferred-cleanup cases are filtered so rollback does not resurrect a
shot that was already dying.

Reconstructed shots also validate character hit callbacks against the current
category-8 `sUnit` character list. The `20260805_233025` Dante mirror dump had
matching `uShotVulcanAirPlay` state through frame 3409, then the client accepted
a contact with `0x0F4E4FD0`, an address absent from every captured roster,
helper, shot, and snapshot region. That contact set `uShot+0x1FE0` to
`0x08200020`, moved the shot from step 1 to step 2 at frame 3411, and removed it
at frame 3412; the host continued until its later contact with a roster Dante.
Only reconstructed-shot contacts whose target is no longer a registered
character are rejected. Live shots and registered roster, assist, proxy, and
helper characters retain native hit processing.

Stage body references are not effect ownership. `sStage+0x2050` is the active
`uStage` body used by `uShot__dispatchStageHitContact`, and `sStage+0x2058` is
the previous body. Both can also appear in effect-category lists, but effect
retirement must exclude them and leave their manager references intact. Clearing
the references made reconstructed Okami Shuffle projectiles detect the floor but
reject the queued stage contact, while destroying the bodies left `sStage`'s
active array stale. Either failure prevented normal stage-contact processing.

### Restore

`RestoreShotAndEffectRecipes` performs these steps:

1. Pin saved `rShot` resources.
2. Sanitize invalid unit/category or collision records that would make native
   destruction unsafe.
3. Mark transient effect-category units for native deferred retirement because
   they may hold weak references into the discarded prediction graph.
4. Remove current shots from `sUnit` and call their native scalar deleting
   destructors.
5. Clear stale pending-shot entries.
6. Recreate each saved shot through the original native spawn function.
7. Re-register it through the native pending-unit/category path.
8. Overlay compatible saved body state while retaining fresh pointer-bearing
   words, inline objects, model allocation fields, and native owner regions.
9. Restore attack-target and collision-primitive state through dedicated rules.
10. Restore RNG streams consumed by reconstruction and release pinned resources.

Doctor Doom's missile emplacement can restore an active saved phase before its
fresh asynchronous model has rebuilt the eight joint records referenced through
`uModel+0x538`. Native movement otherwise computes a small non-null value such
as `0x300` from the null table and dispatches through it. Reconstructed missiles
defer movement until the complete fresh joint table is readable and each joint
exposes a callable DTI virtual; ordinary live missiles retain native behavior.
The emplacement setup is retried at its move boundary while the asynchronous
model or motion resource is incomplete. The retry preserves the verified scalar
tail at `+0x2120..+0x216f`, including the launch trigger at `+0x212c`, and runs
before phase logic. Once ready, native `uModel__LoadResource` creates fresh joint
and motion allocations; historical model pointers are never copied. Setup can
finish synchronously on the retry and immediately continue into launch logic,
so preserve the same pointer-free motion playback ranges across that call. If
setup resets them, the first child derives a different joint Y position while
later children can still agree.

Hidden Missiles also consumes pointer-free `uModel::Motion` playback fields to
derive joint-driven child launch positions. Restore those fields with the same
ownership-safe per-slot ranges used by King Servbot while preserving the fresh
allocations at motion offsets `+0x160/+0x178`. The packed emplacement phase at
`uShot+0x2144` can resemble an accessible pointer, so the verified concrete tail
must be reapplied explicitly after the generic pointer filter. Without that
reapply, rollback repeatedly restores phase zero and launches extra children,
advancing Doom's RNG.

Frank's `uShotVulcanWeaponCart` also carries gameplay-facing model playback
state. In the Frank frame-83 capture, both peers created the same `0x2300`-byte
cart shot, but the host had an active motion header at `uShot+0x550` and
populated motion-slot playback while the reconstructed client retained the
fresh model's zero/default state. Restore the same pointer-free motion ranges
used by King Servbot and Hidden Missiles while retaining the reconstructed
joint, motion-resource, and model allocations. A reconstructed cart can be
replaced every forced-resimulation frame before its asynchronous model finishes,
leaving every saved client snapshot with the same inactive motion header. At the
cart move boundary, retry native setup while that header remains inactive and
defer movement if setup is still incomplete. Preserve the verified concrete
gameplay tail across the retry, but allow setup to initialize fresh motion state;
ordinary live carts retain native behavior. The primary cart effect separately
raises the concrete transition gate at `uShot+0x221c` when the authoritative
shot timer at `+0x1f58` reaches 67. Forced reconstruction can replace that effect
every frame even after motion setup succeeds, leaving the gate at zero and the
cart frozen. The same effect performs the end-of-tick transition to gate `2`
when the timer reaches 122; that gate drives the cart's subsequent linear phase.
For reconstructed carts only, derive gate `1` from timers 67 through 121 before
native movement, then advance it to gate `2` after the timer-122 model tick. The
gate-3 transition at timer 141 also resets the first `uModel::Motion` playback
record. Reapply the verified header/cursor values at `+0x558`, `+0x568`, and
`+0x570` if fresh reconstruction leaves the record in its invalid factory state;
otherwise the return phase decrements its timer but never updates cart position.

Weapon Cart's slot-0 type-5 primitive uses metadata type 0 and therefore the
`cHitPrimSphere` layout. Sphere world/local centers are at `+0x20`/`+0x50` and
the object is only `0x70` bytes; capsule fields at `+0x80` are out of bounds.
Capture and restore metadata-type-0 primitives through the sphere layout rather
than applying capsule ownership and endpoint offsets.

During the model-driven phase, a reconstructed cart can briefly fall through to
gate `2` and leave linear speed `20.0` at `uShot+0x2080` even after gate `1` is
restored. Native attack-matrix generation adds that stale speed to the prior cart
position, shifting the sphere hitbox by exactly 20 units from timer 69 onward.
Keep speed zero with gate `1` through timer 121, then set speed 20 together with
the authoritative gate-2 transition at timer 122.

Weapon Cart reconstruction also exposes a heap-lifetime failure independent of
the saved motion scalars. Native setup allocates 13 `uGameEffectModel` owners at
`uShot+0x2288..+0x22e8` on every factory reconstruction. They use DTI
`0x140d46ed0` and are not subclasses of `uEffect`, so the generic rollback effect
retirement pass does not reclaim them. Restore-stage measurements showed roughly
135 KB and 118 live allocator-slot-12 blocks accumulating per restored frame. At
the frame-221 transition the host had 7,465,776 live bytes, 4,584 blocks, and a
47,392-byte largest free block; the client had 16,716,288 live bytes, 13,133
blocks, and a largest free block of only 752 bytes. All 12 debris resources and
their DTI (`uShotShieldWeaponCartSpread`, `0x140d4a098`) were valid, but each
`0x2150`-byte class allocation failed in `MtHeapAllocator::alloc`. The same heap
pressure explains why the reconstructed cart loses its model and 16-joint graph
before the explosion while its inline collision primitive can still move.

`uGameEffectModel` cannot be synchronously scalar-deleted like `uEffect`; native
`sGameEffect__purgeStaleEffectsViaSUnit` first invokes virtual `+0x78` and relies
on category state transitions. Rollback cleanup mirrors those transitions for
the 13 cart-owned models: finalize through `+0x78`, advance state `3 -> 4` while
removing multi-mask links, then advance `4 -> 0`, remove the primary `sUnit`
link, and invoke the scalar destructor. Pin their resources until fresh native
setup completes. The debris attachment source is separately cached in
`cChrNode(owner+0x6490)+0x08`; remap it to the fresh primary model at
`cart+0x2288` after reconstruction and reinforce that binding at each
reconstructed move boundary so attachment indices 5 through 18 cannot dispatch
through the retired model's base `MtObject` vtable. With this cleanup,
the reconstructed cart retains its model and 16 joints through gate 3, creates
all debris, resumes return sliding, and remains checksum-synchronized.
Each debris recipe also stores its parent cart pointer in `initParam`, outside
the two validated object-reference fields. Remap that value from the captured
cart recipe's `savedPtr` to its `restoredPtr` before running the debris factory;
otherwise attachment dispatch receives the cart destroyed by the prior restore.

Arthur's `uShotVulcanAxe` similarly requires an explicit scalar reapply at
`uShot+0x21d0`. Its main callback advances this model/collision rotation
accumulator by 18 degrees each frame. A saved `18.0f` forms qword `0x41900000`
with the zero padding beside it, which can look like mapped memory to the generic
ownership filter and leave the reconstructed factory value at zero. Reapplying
the verified four-byte float after pointer filtering preserves the axe arc and
the rotating hitbox without importing model pointers.

Arthur's Cross Sword availability detector is the boolean `mIsCrossSword` at
`uArthur+0x68f9`, not a shot pointer. Setup writes one and both the concrete kill
callback and its target-loss callback write zero. Generic cleanup-step filtering
can omit a terminal Cross Sword recipe before native kill clears the detector;
restoring the character byte then leaves Arthur permanently unable to throw
another Cross Sword because no reconstructed object remains to clear it. After
shot reconstruction, character-shot repair derives the detector from successful
`uShotVulcanCrossSword` recipes owned by that Arthur. This retains the lockout
while a reconstructed sword exists and clears stale state when a terminal sword
was omitted or its factory failed.

Haggar's pipe is `uShotVulcanIronPipeRolling` (`0x140d4a4e8`, size `0x2230`).
Its constructor at `0x14017e4a0` creates a pointer-bearing model/effect owner at
`+0x2140`; setup at `0x14017eaa0` asynchronously loads the Haggar pipe MOD into
the shot model and binds that embedded owner back to the shot. The movement and
ground-bounce callback at `0x14017e670` advances the model transform, explicitly
updates its world/child matrices, and copies the resulting pose into the shot's
transform state. This matches the existing Hand Grenade model lifecycle, so the
restore policy pins `+0x100` and `+0x11b0`, restores pointer-free model playback,
and retries setup when a reconstructed pipe's model is not ready. The embedded
owner itself remains fresh and is never copied from the retired pipe.

Chris's `uShotVulcanLandMine` creates its armed-grenade `uGameEffect` only when
virtual `+0x318` handles stage contact. A frame-72 snapshot can therefore contain
`mIsTakeOn=true` and an occupied callback slot at `uShot+0x1f78`, while its fresh
factory reconstruction has neither experienced contact nor created that owned
effect graph. Restoring the boolean alone suppresses the native initializer and
the client never creates `uShotVulcanLandMineExplode` on frame 73. When the saved
Land Mine records both the armed flag and occupied first callback but the fresh
shot remains unarmed with an empty slot, reconstruction invokes the concrete
native contact initializer once before applying the captured callback timeline.
The intervention is DTI-gated and runs only during snapshot reconstruction; it
does not copy the historical effect pointer or alter live shot contact handling.

Doom specializes inherited character tail `+0x68f0` as the active missile
emplacement pointer. `uDoctorDoom__tick` validates this object every frame, and
the combo-matcher callback `uDoctorDoom__checkShotTimerNegative` reads its
`uShot+0x2078` lifetime to accept Hidden Missiles mash extensions. Generic
character metadata incorrectly describes `+0x68f0` as a float, so restoring its
scalar bytes clears the client reference after shot reconstruction. After all
generic tail restoration, bind this slot to the unique fresh emplacement recipe
owned by Doom; never copy the historical shot address.

The matrix restore is required even though later movement paths regenerate some
of them. Generic pre-move collision and pose code can consume the saved attack
and prior-world matrices first. Leaving the factory matrices in a reconstructed
Ryu Hadouken delayed its step transition and changed its lifetime after rollback.

The timing scalars receive an explicit restore after the ownership-filtered body
overlay. Their packed float values can resemble readable addresses when viewed
as a qword, causing the generic pointer-preservation heuristic to retain factory
timing. In the 2026-07-24 Ryu capture, `mTimeInterpolationRate` remained at `1`
instead of the saved `4`, delaying the Hadouken phase transition by three ticks.

Collision capsule parents require explicit identity remapping. Ghidra's
`cHitPrimCapsule_vfn58` reads parent references at `+0x58/+0x60`, parent/joint
state at `+0x68`, and local endpoints at `+0x80/+0x90` before writing world
endpoints at `+0x20/+0x30`. Saved references to the old shot or an inline object
within it are mapped by relative offset into the reconstructed shot; references
to stable roster characters are restored directly, while other effect-owned
references remain native.

The normal shot collision path has an additional authoritative source. Ghidra's
`uShot__updateHitboxes` calls `uShot__forEachExtraSlot_dispatchCollision`, whose
capsule branch at `0x1401C92E0` copies attack-info metadata `+0x20/+0x40` into
the primitive's local endpoints `+0x80/+0x90` on every tick before dispatching
virtual `+0x1C8` to calculate world endpoints. Restoring only the primitive is
therefore temporary. Reconstructed shots reapply the saved data-derived source
after the complete native hitbox update. Primitive `+0x08` cannot be used as an
activity discriminator: `MtGeometry__ctor` stores capsule geometry type `6`
there for every capsule.
The `20260724_232510` client capture retained width `220` on restored frame
`1080`, then collapsed at frame `1081`, showing the prior all-source candidate
count still fell through after native collision setup replaced the wrapper.
The Tron swarm has another representation: its saved primitive can have correct
world endpoints at `+0x20/+0x30` while the local cache at `+0x80/+0x90` is zero
or contains only sub-centimeter floating-point residue. Capture treats local
endpoint-axis pairs below `0.01` as empty and reconstructs each missing axis as
`saved world endpoint - saved shot position`. This per-axis fallback matters
when Y remains `30` but both X endpoints contain only residue. Inactive setup
primitives derive zero and stay on the native path. Active swarm capsules derive the native
`-110,+30` and `+110,+30` endpoints without hard-coding those values or relying
on a concrete DTI that can differ between the super's controller and child shots.
For reconstructed centered capsules with a nonzero recovered source, the final
repair writes the local endpoints and computes each world endpoint as
`shot position + source endpoint`. This bypasses reconstructed parent transforms
that otherwise cancel both X offsets inside virtual `+0x1C8`. It does not modify
shared `rShot` metadata or collision-entry ownership.
The repair is limited to capsules whose native post-update world endpoints have
actually collapsed to a zero-length span. The `20260806_002744/002745` Dante
pair showed why valid capsules must remain native: each Acid Rain drop uses a
swept capsule from its current position to its previous position. Shot position,
matrices, movement, timers, and phase matched, but unconditional source
reapplication froze the reconstructed client's trailing offset at `76.2` while
the host advanced through `77.472`, `78.82`, and `80.25` as the drop accelerated.
The split repeated three frames after every five-drop batch. Preserving a valid
native span retains this shot-specific sweep while the zero-span Tron repair
continues to handle genuinely collapsed geometry.

The AnmChr return slot at `cChr+0x90` is a union, not an unconditional object
pointer. Shot reconstruction remaps it when it equals a captured shot, but must
not clear non-pointer packed values merely because they lack a callable DTI. In
the `20260802_144544` Chun-Li assist capture, both peers held
`0x3F80000080000000` before frame 6249. Restore cleared that value only on the
re-simulating client, transiently changing the animation command path so the
host created `uShotVulcanKikouken` shot 16 while the client did not. The host's
saved owner pre-spawn RNG exactly matched the client's unchanged character RNG,
confirming that the later RNG mismatch was consumption by the missing shot.
The confirmed shot-reference ring head at `cChr+0x6428` retains strict invalid
object clearing.

Character indexed slots 14/15 at `cChr+0x67B8/+0x67C0` commonly own persistent
presentation effects. Unconditionally clearing those slots during every restore
leaves their live effects orphaned and causes characters to allocate replacements
on each replay. Trish Maximum Voltage exposed the gameplay consequence: stacked
weapon effects temporarily exhausted the shared allocator, so otherwise valid
`uShotVulcanEnergyBolt` factory calls returned null. The focused rerun logged a
non-null `rShot` at AnmChr caller `0x14010637B`, successful construction at frame
152, null construction at frames 156/160, and another success at frame 164.
Preserve valid slot 14/15 owners globally and rely on the all-slot DTI/vtable
validation pass to clear stale entries; slot 1 retains unconditional clearing.

`sShot` owns a 256-entry projectile-ID pool at `+0x38..+0x437` and the next-ID
cursor at `+0x438`. The old `0x50`-byte manager capture included only the first
six entries. Reconstructing shots therefore advanced the client's allocator
while the host retained its original IDs. In the `20260802_185718` Kikosho
capture, `uShotShieldKikousyou::mId` was `29` on the host and `128`, `129`, then
`130` on the client. The client then lost P2 assist 2 from the shot's
`cAtkCtrl::TargetInfo` consumed-target entry, accepted an extra Kikosho hit, and
acquired a Y-speed error that propagated into position differences of `1.5`,
`3.75`, and `6.0` units. Snapshot the complete ID pool separately and reapply it
after abandoned-shot destruction and after recipe factories finish, so temporary
construction cannot alter the restored namespace.

The ten projectile slot-active flags at `sGameConfig+0x4A89C` have the same
restore-order requirement. The broad region pass restores them before current
shots are destroyed, but native destruction clears the retired shot's slot.
Reapply the saved flags after shot teardown and after recipe factories,
alongside the shot ID allocator state.

The follow-up `20260802_202847` capture disproved slot flags as the cause of the
host-only Chun-Li shot. Both peers retained identical `01 01 00...` flags, and
both executed one standard `sShot__spawnShot` call while re-simulating frame
4052. Only the host returned `uShotVulcanKikouken`; only then did P2 assist 2's
RNG advance through setup. Ghidra shows that the standard AnmChr case obtains
the `rShot*` by name from the owner's resource table at `cChr+0x34F0` before
calling `sShot__spawnShot`. Inside the spawn function, the only failures before
setup are a null `rShot*` or failure to instantiate its DTI. The successful host
recipe cannot record the failed client's call arguments, so the existing spawn
hooks emit `SHOTSPAWNTRACE` during rollback with the element pointer and DTI,
return value, caller, and owner RNG transition. This distinguishes missing
resource-table state from a native factory failure on the next reproduction.

The `20260802_220647` capture isolated the remaining Kikosho ID failure. Both
peers spawned IDs `16/17` at frame 4701 with allocator cursor `18`. Restoring
frame 4701 reconstructed the client's shots with factory IDs `18/19`; the
manager pool was correctly rewound to cursor `18`, but the broad fresh-owner
preservation range at `uShot+0x1470..+0x2077` also retained `uShot::mId` at
`+0x1F50`. At frame 4703 the mismatched target-consumption identity produced an
extra 168 damage and changed P2 assist 2's `mDSpd/mDAcl` from `0/0` to `-1/2`.
At frame 4704 that propagated to the reported one-unit X-position difference.
Reapply the saved four-byte `mId` after filtering fresh ownership fields; do not
restore the position, which is only downstream evidence.

The `20260802_225123` Kikosho snapdump found the next recipe omission. At frame
370 both peers entered Kikosho phase 5, but only the host added the second
sphere primitive that expands the explosion. `uShotShieldKikousyou_vfn118` at
`0x140161B30` resolves attack-info through `cAtkCtrl+0x20`, then enumerates the
resource hit-data entries and allocates one primitive per entry. The broad
fresh-owner preservation had retained that map at `uShot+0x1490`; restore this
resource-map binding together with the already-restored `cAtkCtrl+0x28`
`mAtkInfoId`. Do not restore collision-slot allocations directly.

The `20260803_000601/000602` pair showed a distinct Kikosho-owned-effect
lifetime failure. At frame 4358 the client cleared P2 assist 2 from
`cAtkCtrl::mTInfo` and `mTInfoEft`; at frame 4359 it accepted a duplicate hit,
set the assist's `mDSpd/mDAcl` to `-1/2`, and produced the reported 1.5-unit Y
position difference at frame 4360. The shot factory had correctly created fresh
effects in `uShot+0x1F78/+0x1F80`, but retired effects still held the old shot
through `uEffect+0x40` and `uGameEffect+0x280`. Because effect retirement is
deferred while shot destruction is synchronous, the shot allocator could reuse
that address and delayed old-effect cleanup could mutate the fresh callback
slots. Retirement now detaches shot-valued parent, owner, and request-unit
references before the old shot is destroyed. Saved effect addresses are never
copied or remapped; the callback array remains exactly as native setup created
it.

The `20260803_010332` verification run exposed a regression in the attempted
post-factory callback validation. Kikosho first exists at frame 2537. The host
has fresh effects at `uShot+0x1F78/+0x1F80` and P2 assist 2 in both
`cAtkCtrl+0x58` and `cAtkCtrl+0x658`; the reconstructed client has all four
pointers cleared. At frame 2538 the client's `mTotalCombo` is already 2 while
the host remains at 1, and the client continues incrementing on later frames.
Native effect ownership is not necessarily finalized until pending-unit
processing, so validating parent/owner/request-unit fields immediately after
the shot factory can reject valid fresh effects. Since neither concrete restore
nor `Shot_ApplyFlight` writes the callback array, every pointer there is already
factory-created. Preserve it without post-factory validation while retaining
the retiring-effect detachment described above.

The `20260803_012706/012707` pair shows that preserving fresh callback pointers
alone is insufficient. Both peers agree through frame 4403. The host's two
Kikosho callbacks and its target pointers clear at frame 4404, then its
`mTotalCombo` increments at frame 4405. The reconstructed client callbacks stay
alive through frame 4405, clear at frame 4406, and its next hit occurs at frame
4407. The shot ID, collision primitive, combo state, and target records matched
before this two-frame lifecycle shift. Native reconstruction had restarted the
callback effects at the beginning of their EFL timelines. Capture callback
presence, DTI, and the pointer-free `uEffect+0x100..+0x107` flags and
`+0x110..+0x137` timing state. Restore those scalars onto the corresponding
factory-created effects while retaining their fresh resources, generators, and
ownership links. If a callback was already null in the saved shot, retire the
fresh replacement and keep the slot null rather than restarting a completed
timeline.

The `20260803_015146` verification pair shows that the outer `uEffect` clock is
not the complete callback timeline. Both peers create Kikosho with matching
`mId=6`, `mTotalCombo=2`, target entries, collision topology, and two callbacks
at frame 1908. At frame 1913 the reconstructed client clears both callbacks and
P2 assist 2 from `mTInfo` while the host retains them. The client consequently
accepts hits at frames 1917, 1918, and 1919, reaching `mTotalCombo=7`; the host
remains at 4. P2 assist 2 first receives the extra `mDSpd/mDAcl=-1/2` pulse at
frame 1917 and its X position diverges at frame 1919. Ghidra confirms that
`uEffect__moveAfter` derives callback completion from effect-local state beyond
`+0x137` and invokes each fresh `cEffectUnitGenerator` through virtual `+0x88`;
the generator's state determines whether `uEffect+0x206` remains active. Rewind
the pointer-free effect completion fields and matching generator scalar fields
onto the fresh graph. Preserve generator list links, owners, resource pointers,
particle-manager arrays, buffers, and all other native allocations.

`cParticleManager` entries in `uEffect::mJointArray` are plain embedded records,
not MtObjects. Never validate their first qword as a vtable. Doing so marks every
valid manager array malformed, zeros `mJointNum`, and causes
`uEffect::releaseGenerator` to skip all per-manager `releaseSubBuffer` calls,
leaking particle buffers while freeing only the containing array.

The `20260803_024045/024046` pair exposes a separate capture-state ambiguity.
At frame 4508 both peers have Kikosho `mId=17`, phase 1, timer 1, one collision
primitive, and `mTotalCombo=1`, but the reconstructed client's callback and
target slots are already null. The host retains both callbacks through frame
4628. The next Kikosho appears at frame 4613; the client accepts repeated hits
from frame 4614 while the host remains at one hit, producing the confirmed
frame-4613 checksum mismatch. A callback pointer can be non-null while its
pending effect is not yet sufficiently initialized for `IsEffectObject` and DTI
state capture. Treat callback occupancy separately from successful scalar-state
capture. A truly null saved slot retires a fresh replacement; an occupied but
not-yet-queryable saved slot preserves the fresh callback without applying a
partial timeline. A DTI mismatch likewise preserves the fresh native callback
rather than clearing it.

The `20260803_025957` pair initially suggested that callback state was being
overwritten after factory creation, but the follow-up `20260803_032307` pair and
Ghidra vtable resolution disproved that interpretation. At frame 4509 both peers
have Kikosho `mId=20`, two callbacks, target occupancy `[1,0,1]`, and
`mTotalCombo=1`. At frame 4510 only the reconstructed client has lost both
callbacks and all target entries; it accepts another hit at frame 4511 and its
target position diverges by frame 4513. `sGameEffect__flushPendingArray` at
`0x140130D00` invokes virtual `+0x78`; for `uGameEffect`, that slot resolves to
`uEffect__kill` at `0x140016F20`, which changes live unit state to retiring state
3. It does not finalize generators. Kikosho setup already binds each callback to
the shot, starts its EFL stream, and directly registers it with `sUnit` category
13. Therefore, drain the pending kill queue only before abandoned-timeline
teardown. Never flush it after shot reconstruction. Keep each factory-created
callback alive and apply the saved pointer-free effect and generator state while
retaining fresh resources, ownership, allocations, and topology.

Character target-processing tables must remain synchronized with the character
body. `uCharacter__processTarget1` indexes seven heap-backed tables through
`+0x1408..+0x1438` using packed counts at `+0x1440`, then calls virtual `+0x40`
on each active entry. Restoring a count without its corresponding table caused
the `2026-07-24 23:04:41` null-vtable crash at `0x140201007`. Active table ranges
are now captured separately and restored only while the character's owner slot
still points to the captured allocation.
Effect retirement must also detach the current `uExAtkSpace` child effects.
Ghidra's `uExAtkSpace__setModel` at `0x140137A10` walks four owned `uEffect*`
fields at `+0x1D18..+0x1D30` and releases each live child through virtual
`+0x78`. The `2026-07-24 23:31:48` crash at `0x140137A6A` occurred after a
retired child was independently destroyed and its former vtable word was reused
as packed `1.0f` values. Rollback now clears those four owner references before
marking effect categories for native deferred retirement; it does not destroy
the children directly or alter the `sAction+0x148` ExAtkSpace owner pointer.
Zero setup-phase endpoints are not reapplied. A shot reconstructed before its
attack activates must be allowed to obtain its later geometry from native
metadata; otherwise the stale snapshot zeros collapse every future capsule to
the shot origin. The `20260724_220943` capture demonstrated this case: shot
movement and lifetime matched, while the host capsules remained degenerate and
the client used the expected `-110,+30` to `+110,+30` local endpoints.
Leaving the modified vectors in shared `rShot` storage corrupted allocator state
during later effect-generator allocation in the `20260724_220644` crash. It must
not replace the metadata pointer at collision entry
`+0x48`: the `20260724_212647` capture showed that doing so made all four
reconstructed swarm capsules disappear on the next resim tick, even though the
primitive arrays and shots remained present.

This is reconstruction, not heap rewind. A restored shot may have a different
address from the shot captured in the snapshot. Code that stores historical
shot pointers must therefore use an explicit saved-to-restored mapping or retain
fresh native references.

## Why A Flat Array Is Not The Current Solution

### Dense array of `uShot*`

Replacing the ten linked lists with dense pointer arrays is possible with a
moderate hook surface, but it only changes enumeration. Every entry still points
to a variable-sized native object with external ownership. It does not make
snapshot/restore materially safer.

It would also require patching or emulating every consumer that expects a
`cChrNode`, including owner-change and destruction paths. Fake nodes preserve
the pointer topology and remove most of the intended benefit.

### Stable slab of concrete objects

A stable-address slab is substantially harder. It would require:

- Reserving aligned slots large enough for every concrete DTI size.
- Intercepting DTI factory allocation.
- Running native constructors and destructors.
- Suppressing only the final MT allocator free for slab-backed objects.
- Preserving `sUnit`, collision, resource, animation, effect, and parent-shot
  relationships.
- Proving that every concrete subclass is compatible with placement into the
  slab.

A whole-slab `memcpy` would still restore stale external pointers and allocator
state, so it would not replace the current ownership-aware restore logic.

### Pointer-free handles

Replacing `uShot*` throughout the game with indices is not a local list change.
Generic engine APIs, virtual dispatch, collision, stage logic, effects, and
character fields all consume real object addresses. This would be a partial
engine rewrite and is not practical for the plugin.

## Invariants

- Never flat-copy intrusive sentinel topology from an old frame.
- Never free a shot body without running its native destructor path.
- Never restore historical resource, collision, effect, or inline-object
  pointers merely because the address is readable.
- Preserve native list/category registration order unless a test demonstrates
  that order is irrelevant.
- Keep the pending count at or below `0x200`; the native enqueue path can
  increment before reporting overflow.
- Test projectile-heavy and character-specific cases on both peers. Static
  layout analysis cannot prove effect and collision ownership correctness.
