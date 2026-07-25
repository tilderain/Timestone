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
