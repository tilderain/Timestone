# Allocator rewind vs selected-object restore: deep native audit

**Program:** `umvc3.exe.unpacked.exe`, Ghidra port 8192.
**Date:** 2026-09-10. Preferred-base addresses below require runtime relocation.
**Method:** Native decompilation/disassembly, allocator-vtable reads, DTI
registration and routing analysis, and comparison with current mod source.
**Not performed:** Runtime heap census, dirty-page measurement, allocator
replacement, copy benchmark or end-to-end rewind correctness test.

## Verdict

* **Rewind the existing 16 MiB Unit pool alone:** insufficient for correct
  rollback. Allocation metadata, collision children, manager state, arrays,
  resources, and globals extend outside it.
* **Rewind all relevant existing pools wholesale:** not a self-contained game
  checkpoint. Those pools mix GUI/effects, resource ownership, background sound
  and resource-loading state, and live synchronization objects.
* **Redirect selected gameplay allocations into a new rewindable arena:**
  technically plausible in a DLL, with central interception points and a native
  external-buffer initializer. Requires ownership-aware allocation routing,
  external reference accounting, lifecycle integration, and checkpoint fences.
  This is a substantial hybrid rollback design, not a local dirty-page change.
* **Selected objects/fields plus selective reconstruction:** the better-supported
  choice for this mod. The implementation exists and its expensive paths are
  identifiable. It still needs coverage and performance improvements; it is not
  automatically correct for every native object.

No static finding establishes that either fully optimized implementation is
faster. The allocator-controlled design has a real opportunity to remove
reconstruction, but no basis for a 60--70% performance-win probability or a
sub-millisecond guarantee.

## 1. The default pools are mixed ownership domains

`MtMemoryAllocator__setDefaultAppAllocator` (`0x1402563e0`) registers a class-name
map and calls `MtMemoryAllocator__commitAllocatorTable` (`0x14045d7a0`).
`MtMemoryAllocator__resolveAllocatorInfoRecursive` (`0x14045d590`) searches the
DTI name, ancestor names, and enclosing namespace names; assignments propagate
to descendants through `0x14045dbb0`.

`MtMemoryAllocator__getAllocator` (`0x14045d4f0`) dispatches through:

```cpp
allocatorTable[(dti->packedFlagsAt30 >> 23) & 0x3f];
```

`MtMemoryAllocator__createNamedPools` (`0x140256050`) gives the following default
route composition. This is static initialization evidence; runtime overrides and
actual allocation occupancy have not been measured.

| Registered class families | Logical allocator | Backing pool |
|---|---:|---|
| cUnit, cChrCmd, cChrInput, cAtkCtrl, cArm, cChrNode, numerous character helpers | 12 | Unit, 16 MiB |
| nAI, cAIObject | 17 | Alias of Unit |
| cParticleManager, uBaseEffect, uEffect2D, uSimpleEffect, lights and other effect classes | 18 | Alias of Unit |
| nGUI, uGUI, cGUIObject, cGUIInstance and other GUI classes | 19 | Alias of Unit |
| MtArray, MtTree, MtMap, property-list containers | 3 | Array/String, 32 MiB |
| nCollision, cDynamicBVHCollision, MtGeometry and collision helpers | 4 | Collision, 4 MiB |
| cSystem, sCollision, cChrTeam and many managers | 11 | System, 64 MiB |
| cResource, nDraw, resource data | 10 | Resource, 128 MiB |
| sSound, sGSound | 14 | Alias of Resource |
| MtNetObject and network families | 16 | Alias of System |
| MtStream, MtThread and other generic infrastructure | 1 | Global, 16 MiB |

Consequences:

1. A dirty Unit page is not necessarily a gameplay page.
2. Preserving live GUI/effect payloads while rewinding their shared free lists
   cannot be done by simply excluding some payload pages.
3. A Resource-pool rewind includes more than immutable asset bytes.
4. A general container allocation identified only as MtArray does not reveal
   whether its owner is a fighter, collision manager, GUI, or other subsystem.

### Shared-free-list counterexample

At checkpoint T, block B is free. After T, a live GUI allocates B. Restoring the
shared allocator to T while keeping that GUI live marks B free again. A subsequent
gameplay allocation can overwrite the GUI. Conversely, retaining the live
allocator state does not resurrect a historical object whose allocation has
already been reused.

This is a design implication of the verified shared pool, not a reproduced crash.
Solutions require coherent restoration of all affected allocations, prior pool
separation, or a deliberate retained-allocation/lifetime scheme.

## 2. The allocator itself is tractable, but not just a byte buffer

The pool factory at `0x1402562e0` allocates a separate `0x660`-byte allocator.
The historically named `MtHeapAllocator__ctorBase` at `0x140255f80` installs
`MtScalableAllocator` vtable `0x140b08620`.

Relevant native virtual entries:

| Vtable offset | Target | Observed purpose |
|---|---|---|
| `+0x30` | `0x1404cafd0` | Choose subheap and allocate; try other subheaps on failure |
| `+0x38` | `0x1404df8f0` | Allocation wrapper dispatching through +0x30 |
| `+0x40` | `0x1404cafb0` | Allocation wrapper dispatching through +0x30 |
| `+0x60` | `0x1404cb350` | Free, account and coalesce |
| `+0x68` | `0x1402563a0` | Read requested size from allocation header |
| `+0x70` | `0x1404cad00` | Initialize using caller-provided backing buffer |
| `+0x78` | `0x1404caeb0` | Obtain backing storage and initialize |

`0x1404caba0` initializes the backing range and global lists. Relevant allocator
object offsets, separate from the backing range:

| Offset | Evidence-backed role |
|---|---|
| `+0x90`, `+0x98` | Backing start/end |
| `+0xb0`, `+0xb8` | Initial capacity/free-unit accounting |
| `+0xc0..+0xd4` | Global free-list roots/counts |
| `+0xd8 + i*0xa8` | Eight subheap records |
| `+0x158 + i*0xa8` | Subheap critical sections |
| `+0x618` | Atomic subheap-selection counter (selection helper) |
| `+0x620` | Global block-management critical section |
| `+0x648` | Configured active subheap count; used by selection and fallback allocation |
| `+0x658` | Allocation-padding value initialized to four or eight; consumed by the allocation worker |

`0x1404cb260` locks subheap+0x80, obtains a block, stores requested size at
block+0x40, stores the displacement back to its header at userPointer-8, and
zeroes the allocation payload. `0x1404cb350` uses the header to find its subheap,
updates lists/counts, and coalesces through `0x1404cb480`.

The global-block acquisition at `0x1404ca3b0` searches/splits the existing free
list under allocator+0x620; the inspected normal path returns null on exhaustion.
Free/coalesce at `0x1404ca110` returns blocks to that in-memory list. No per-block
VirtualAlloc/VirtualFree occurs in those traced normal paths. Backing allocation
is `0x14045fe90`, which calls VirtualAlloc with `0x1000`/`4` flags/protection.

**Positive finding:** finite, address-stable memory and accessible allocator
metadata make coherent heap rewind mechanically plausible. It does not require
reverse-engineering the Windows heap implementation.

**Requirement:** logical metadata must rewind with blocks; lock objects must
remain live and quiescent. Suppressing just the final free or rewinding just a
cursor is not equivalent to this native allocator's semantics.

Changing DTI routing after objects already exist also requires compatible free
routing: native deleting destructors resolve the allocator from their DTI.
The header/memSize contract and +0x38/+0x40 allocation entry paths matter to any
replacement. Allocation hooks alone do not account for ordinary memory writes.

## 3. Concrete cross-pool gameplay ownership

### Shot collision children

`uShot__uShot` (`0x1401c9b00`) allocates four `0xe0`-byte child objects through
`0x1405bbd00`, historically called cChrEffectSlot__alloc. That wrapper selects
DTI `0x140e184e8`.

The DTI registration at `0x140a3f630` supplies name **sCollision::Node** (string
`0x140bac830`), parent **nCollision::cCollisionNodeObject** (DTI `0x140e18050`),
and size `0xe0`. Do not infer an effect allocation from the historical helper
name. The default namespace resolution predicts the sCollision/System mapping
for this nested class; final allocator membership needs a runtime read if the
initialization map is overridden.

Its deleting destructor at `0x1405bbd40` resolves that same DTI's allocator and
calls +0x60. The core destructor at `0x1405a3bc0` can destroy an owned child
through its virtual destructor, then destroys the collision-node base.

### Dynamic collision storage

`cDynamicBVH__resizeNodePool` (`0x1407d4b70`) creates an array-cookie header plus
`0x40`-byte nodes, and a separately allocated free-node pointer array.
`cDynamicBVH__popNodeOrAlloc` (`0x1407d3a50`) can allocate an individual node
after its reserved/free collections are exhausted. Those are additional native
allocation lifetimes, not just fields inside a fighter or shot.

`MtArray__extendBuffer` (`0x140011660`) allocates, copies, frees the old buffer,
and changes the owner pointer/capacity. Its allocator getter (`0x1400116f0`)
selects the shared MtArray DTI. Whole-pool routing cannot distinguish owners.

`cChrCmd__allocAndResetLineBuffer` (`0x1400280c0`) uses the cChrCmd::Line factory
for nonempty arrays, an example of child allocation that can follow the Unit
class map. Not every child necessarily escapes the Unit pool; coverage must be
traced rather than assumed in either direction.

### Manager state and globals

`sUnit__removeUnit` (`0x14051b9f0`) changes neighbors and manager head/tail.
`sUnit__removeFromMultiMask` (`0x14051baf0`) changes separate MtArray collections.
`uShot__dtor` (`0x1401c9fd0`) additionally removes the sShot entry and releases its
ID. Restoring only object payloads cannot restore these relationships.

The mod's `RestorePreSpawnRng` also shows a static global RNG at `0x140d765d8`,
alongside effect-manager and character RNG domains. A heap snapshot does not
automatically include .data globals or DLL-owned tracking maps.

## 4. Resource pinning is necessary but not sufficient

`cResource__releaseRef` (`0x14050d5a0`) dispatches into `sResource__release`
(`0x140519bb0`). The latter decrements resource+0x4c; on zero it removes the
resource from a manager hash table, unlocks, and invokes the deleting destructor.
`sResource__addRef_impl` (`0x140516300`) increments the same count under the
manager's synchronization.

`uActorGameModel__releaseOwnedResources` releases resources and several external
arrays. `uModel__dtor_4760` also releases joints, subcontrollers, motion resources,
and a scene-managed allocation token. `uBaseModel__dtor_B760` releases model and
material resources, instances and command caches.

Keeping a resource alive with one extra pin does not undo the logical release:
restore the object's bytes without restoring its resource ownership, destroy it
again, and the count is decremented again. A correct arena design must restore
or reconcile the simulation's contribution to reference counts while preserving
other live owners. Merely restoring Resource-pool bytes has concurrency and
external-state problems described below.

`uBaseModel__resetMaterials` allocates mutable material instances; its fallback
uses nDraw::MaterialNull DTI `0x140e1ccd8` (allocator wrapper `0x1407c7de0`). The
default nDraw mapping is Resource. Thus Resource is not an immutable-assets-only
domain. Rendering also writes bookkeeping inside uBaseModel: the submit path
`0x14055da10` updates +0x192/+0x194 and uses command-cache acquisition.

## 5. Gameplay serialization helps, but does not freeze the process

`sUnit__moveCategory` (`0x14051b4c0`) either calls unit move directly or queues
jobs. It also performs native lifecycle transitions and actual destruction.
`sMain__executeJob` submits work and waits for active workers' completion events
before resetting the job count. This provides an encouraging gameplay fence.

The current mod explicitly disables category, collision, hit-solver and related
gameplay parallelism in `Rollback_DisableGameplayParallelism`
(`rollback_runtime.cpp:1164`).

Independent native activity remains:

* `sResource__loaderThreadProc` (`0x140519040`) waits on a semaphore, removes
  queued resources, performs loading and modifies resource-manager state.
* `sSound__sSound` (`0x1405bc910`) constructs embedded **sSound::UpdateThread**
  and **sSound::LoadThread** objects and initializes numerous critical sections.
* The resource constructor (`0x140515b30`) creates load synchronization and
  shared manager state; this is not captured by rewinding an allocation buffer.

These functions establish separate threaded lifetimes, not their measured
activity during a particular fight. The existing gameplay fence is not proof
that all writers to broader System/Resource pools are quiescent. Replaying lock,
thread or handle bytes does not rewind their operating-system state.

## 6. What the current implementation actually does

Current `rollback_restore.cpp`, rather than an idealized traditional design:

* `RestoreShotAndEffectRecipes` pins shot/model/callback resources.
* It retires effects and destroys **every tracked current shot** (lines 735--768).
* It respawns saved recipes through `Shot_SpawnOriginal` (lines 889--918).
* It rebuilds callback/collision topology and registration, restores concrete
  state and IDs, and remaps references.
* It reapplies character/collision state that reconstruction can disturb
  (lines 2154 onward).

**Real allocator upside:** a complete same-address graph rewind could remove
factories, reconstruction RNG handling, saved-to-new-address remapping, and some
post-reconstruction repairs. This is a substantial opportunity, not just a
memcpy micro-optimization.

**Real traditional upside:** current snapshot discovery already separates owned
buffers and scalar fields. For example, `rollback_snapshot.cpp:525--609` captures
BVH payloads, free-node order, other pointer arrays and individual fallback nodes.
The project can improve coverage and targeted object reuse without first
partitioning every native pool. Safe reuse still requires restoring the full
relevant child state; matching the shot's pointer or DTI alone is insufficient.

Current DLL maps such as g_liveShotsBySequence also need restoration/rebuilding
if native objects rewind without passing through the lifecycle hooks that
normally update those maps.

## 7. Feasibility of three concrete alternatives

| Alternative | Assessment | Principal missing work |
|---|---|---|
| Existing Unit-buffer dirty-page rewind | Incomplete as a rollback mechanism | External allocation graph, shared allocator logical state, managers, resources |
| Existing Unit/System/Array/Collision/etc. whole-pool rewind | Not a clean process checkpoint | GUI/effects, sound and loader activity, external state, locks, resource ownership |
| Dedicated gameplay arena plus explicit external-state restore | Plausible hybrid architecture | Class and owner-aware routing, early/compatible adoption, child lifetimes, refcount accounting, presentation bridge, writer fences |

Four named pool capacities alone (Unit/System/Array/Collision) total 116 MiB.
A hypothetical full copy at effective 20 GiB/s takes about 5.66 ms. This is NOT
a minimum necessary snapshot size or a benchmark: occupied ranges and dirty
sets can be much smaller. It illustrates why expanding whole-pool coverage is
not automatically compatible with the original 16 MiB/1 ms argument.

The dedicated-arena alternative must choose an explicit ownership boundary.
Routing a fighter's main allocation is easy to identify; routing a shared
container allocation and its later resize requires ownership context. Class
names alone cannot express all those relationships.

## 8. Decision and remaining measurements

The evidence favors continuing selected-object restoration for this DLL. It
does not prove a dedicated arena impossible or inherently slower. The initial
claim that an allocator hook would provide a small, closed rollback domain is
not supported; the default routing directly contradicts that simplification.

Before approving a redesign, require a runtime ownership/allocation census and
a limited native prototype that survives creation, destruction, address reuse,
and repeated rollback with resource counts and collision relationships intact.
Then measure dirty pages at save, union pages over actual rollback depths,
tracking overhead inside simulation, external repair cost and p95/p99 latency.
No percentage odds or 1 ms guarantee can be derived from these static functions.

## Ghidra annotations made in this audit

* Corrected the `this` parameter of `0x1404cafd0`, `0x1404cb260` and `0x1404caab0`
  to the existing MtScalableAllocator type, with explicit size/alignment
  arguments where used. Re-decompiled allocation and selection to confirm
  removal of the incorrect `this[1]` extent and the actual counter/count offsets.
* Added audit comments at pool/class routing `0x1402563e0`, allocation worker
  `0x1404cb260`, and backing metadata initialization `0x1404caba0`.
* The initial audit retained historical function names. The subsequent requested
  rename pass applied the corrections below and verified them with live function
  queries and a refreshed allocation-path decompilation.

### Applied function-name corrections

| Address | Previous name | Current name |
|---|---|---|
| `0x140255f80` | MtHeapAllocator__ctorBase | MtScalableAllocator__ctorBase |
| `0x1402562e0` | MtMemoryAllocator__createHeapPool | MtMemoryAllocator__createScalablePool |
| `0x1405bbd00` | cChrEffectSlot__alloc | sCollision__Node__alloc |
| `0x1404ca110` | MtHeapAllocator__freeAndCoalesceGlobalBlock | MtScalableAllocator__freeAndCoalesceGlobalBlock |
| `0x1404ca220` | MtHeapAllocator__pushFreeBlockFront | MtScalableAllocator__pushFreeBlockFront |
| `0x1404ca260` | MtHeapAllocator__insertGlobalFreeBlockBySize | MtScalableAllocator__insertGlobalFreeBlockBySize |
| `0x1404ca360` | MtHeapAllocator__pushFreeBlockBack | MtScalableAllocator__pushFreeBlockBack |
| `0x1404ca650` | MtHeapAllocator__carveBlock | MtScalableAllocator__carveBlock |
| `0x1404caa60` | MtHeapAllocator__unlinkFreeBlock | MtScalableAllocator__unlinkFreeBlock |
| `0x1404caab0` | MtHeapAllocator__selectSubHeapByCounter | MtScalableAllocator__selectSubHeapByCounter |
| `0x1404cafd0` | MtHeapAllocator__alloc | MtScalableAllocator__alloc |
| `0x1404cb260` | MtHeapAllocator__allocFromSubHeap | MtScalableAllocator__allocFromSubHeap |
| `0x1404cb480` | MtHeapAllocator__coalesceAndInsertFreeBlock | MtScalableAllocator__coalesceAndInsertFreeBlock |
| `0x1404cb700` | MtHeapAllocator__unlinkGlobalFreeBlock | MtScalableAllocator__unlinkGlobalFreeBlock |
| `0x1404caba0` | FUN_1404caba0 | MtScalableAllocator__initializeBackingBlocks |
| `0x1404ca3b0` | FUN_1404ca3b0 | MtScalableAllocator__acquireGlobalBlock |
| `0x14055b8d0` | sScene__releaseLayerSlotsAndInit | uBaseModel__releaseAndResetCommandCache |

Earlier text records names seen during investigation. The rename pass changed
only function names; legacy parameter types in other helpers may still need a
separate typing pass. No executable bytes or object layouts changed.
