# Delayed Destruction Implementation Audit

Research: 2026-11-04, Ghidra port 8192, `umvc3.exe.unpacked.exe`, preferred base
`0x140000000`. Native bodies, disassembly, concrete vtables and callers were
examined. This is a proposed implementation, not a runtime-tested feature or a
measured performance result. No game binary or runtime lifecycle code was changed.
See `delayed_free_snapshot_design.md` for the earlier design and implementation
status. This follow-up resolves several previously open adapter questions.

Further findings on CPU-render fences, publication/reset coverage, palette
reservations, adhesion quotas and resource admission are in
[the boundary follow-up](delayed_destruction_boundaries.md).

## Recommended Model

Use **same-address snapshots with intact-object quarantine and versioned owned
graphs**. Logical death occurs at the original native time; physical disposal
occurs only after snapshot and native-consumer obligations expire.

The fast path is packed CPU copies and coherent ordered-topology restoration.
No factory, setup, kill, particle replay, or native destruction is needed to
restore objects already represented by a retained snapshot. New spawns on a
corrected timeline may still use native factories. Do not promise zero allocation
for the entire resimulation or reuse abandoned spawns solely by frame/type.

Keep lifetime interception at ownership transitions rather than hooking every
allocator free or searching object memory for values that resemble pointers.
Construction/replacement records exact extents and compiled per-type descriptors.
Normal capture iterates dense records; retirement uses cached descriptors.

This is not one generic RET patch. It needs a small lifetime core plus explicit
adapters for shot registration, mutable collision storage, effect graphs, render
contexts, and external side effects. The adapters prevent delayed destruction
from changing gameplay capacity or destroying a newer allocation's registration.

## Intact Shot Retirement

`sUnit::moveLine` (`0x14051B4C0`) provides a precise ordinary boundary:

| Instruction | State |
| --- | --- |
| `0x14051B5F4` | State-3 pass removes group membership; unit becomes state 4 |
| `0x14051B609` | Subsequent eligible state-4 pass clears lifecycle state |
| `0x14051B60C` | Removes primary scheduler membership |
| `0x14051B61C` | Calls most-derived deleting destructor with flag 1 |

Intercept the last call for supported registered objects. At that point primary
links `+0x18/+0x20` are null and category bits are detached, but the dynamic vptr
and all derived members are intact. Preserve the preceding native state-3 pass;
these are scheduler visits, not a fixed two-frame timer.

Perform only logical teardown at this boundary: release shot membership/ID,
detach audited external registrations and account for logical capacity. Quarantine
the intact allocation graph. Native concrete kill, callback cleanup, sound stop
and Vergil reservation clearing must already have run on the simulated timeline.
Never replay kill during GC or when discarding a future-only prediction object.

Base destructor `0x1401C9FD0` is too late: derived destructors already destroyed
members. Deleting flags zero still runs those destructors; intercepting free is
later still. Neither preserves a resurrectable object.

### List Correction And Duplicate Release

Assembly of `0x1401513B0` and its four accessors establishes this correction to
older notes:

| Address within shot | Meaning |
| --- | --- |
| `+0x1450` | Embedded list-node start / vptr |
| `+0x1458` | Next node |
| `+0x1460` | Previous node, NOT owning-shot pointer |

Sentinel node is global slot head `+8`; the list is circular and doubly linked.
Unlink changes neighbors but leaves the removed node's links stale. A second
unlink at GC can corrupt a newer list. Accessors `0x140011720`/`0x14068BA70`
read node `+8/+0x10`; setters `0x140003D50`/`0x140057030` write them.

ID release `0x140151490` blindly clears `sShot+0x38+4*id`. A logically released
ID may already belong to another shot by GC. Never call this again for the old
allocation, and never use an out-of-range fake ID.

Preferred physical-disposal adapter: suppress only the already-completed base
registration block `0x1401C9FE3..0x1401CA00C` for the exact collecting allocation;
continue physical teardown at `0x1401CA011`. Use nested/scoped identity, not a
global GC boolean that suppresses cleanup of unrelated children. Keep the saved
most-derived deleting function and call it once. A neutralized-node/ID-zero
variant has fewer interception points but more concrete-destructor assumptions.

### Constraints And Other Finite Handles

Constraint unlink `0x1404483F0` / `0x1405EC190` is non-destructive. For an embedded
`uCnsTSR`, it repairs the parent joint/interface chain, clears parent/neighbor
links and sets order to -1. Later native destruction sees a null parent and does
not unlink twice. Restore reciprocal topology, not just the constraint body;
normal rebind can reorder the chain.

Spiral/Storm constraints begin at shot `+0x2140` and bind to the shot's own joint
0 in the inspected setup paths (`0x1401BF410`, `0x1401BFE80`). Other shots have
one or multiple constraints at different offsets. Use per-type descriptors,
not a sword-only special case. Joint-head setter `0x14056F900` can also mark
active model/joint ancestry dirty; those effects require coherent restoration.

Model destruction releases scene palette handle `+0x116C` through `0x14077D210`.
Holding this handle until GC is simpler but increases occupancy. Early release
requires allocator ownership restoration and suppression of the later release;
the valid native no-handle sentinel is `0xFFFFFFFF`. Do not silently treat finite
logical handles like unlimited retained heap memory.

## Mutable Collision Generations

`uShot::releaseAtkInfo` (`0x1401C6870`) is the principal examined replacement
boundary. It is called during active phase changes as well as destruction.

Partial release deletes wrappers/primitives but retains the collision node and
array length/capacity. It leaves holes, which native add fills before appending.
Full release additionally destroys the root and clears the shot's slot.

Retain the old child array, wrappers and primitives as a generation. For partial
release, keep the root address stable and install a new array with the native
postimage and unchanged length/capacity. For full release, retain the root graph
and clear the published slot. Do not retain an old graph while letting its active
replacement mutate the same wrappers.

| Allocation / field | Verified size or offset |
| --- | --- |
| Collision object base | `0xD0`, factory `0x1405AD2A0` |
| Actual shot collision root (`sCollision::Node`) | `0xE0`, factory `0x1405BBE20`, ctor `0x1405BBC60` |
| Geometry wrapper | `0x60`, factory `0x1405AD3A0` |
| Shot sphere / capsule | `0x70` / `0xB0` |
| Root array length / capacity / data | `+0x20 / +0x24 / +0x30` |
| Root array ownership | `+0x28` |
| Root bounds / setup latch | `+0x50 / +0x74` |

Normal shot wrappers borrow their primitives; root destruction alone will not
free those primitives. Final disposal must reproduce the shot's explicit
primitive-deletion contract. Null-primitive wrappers survive partial release;
exclude or separately version that exceptional case instead of assuming all
postimage entries are null.

2026-11-08 native recheck: shot ctor `0x1401C9B00` allocates four `0xE0`
roots. The derived root ctor writes at `+0xD0`; capturing only the `0xD0`
base would omit part of the actual allocation. Wrappers are `0x60`, sphere
primitives `0x70`, and capsule primitives `0xB0`. Shot ID/team are dwords at
`+0x1F50/+0x1F54`. Base destruction calls team unlink `0x140151420` and ID
clear `0x140151490` before collision/resource release. These helpers can be
detoured instead of patching the base-destructor instruction range, but ID-clear
suppression must be scoped to the exact currently collecting shot's base
destructor, including nested destruction. `rollback_unit.cpp` now implements
this scoped suppression for terminal shot/model retention, with concrete
deleting-target detours registered after factory return. This does not implement
the collision/model replacement adapters described elsewhere in this audit.

Pin the old `rShot` before `setShotResource` releases its reference: that release
precedes the collision-release call. Wrapper hit metadata borrows resource bytes.
Intercept tracked array growth at `0x140011660` too, or use adequately sized live
topology buffers without resurrecting historical backing pointers. Native clone
helpers may shallow-copy borrowed primitives/userdata and are not generic deep
snapshots.

## Collision Work That Can Be Rebuilt

The useful candidate fence is immediately after `sCollision::beginFrame`
(`0x1405BB710`) and before unit simulation. The ordinary Collider starts at
`sCollision+0x7368`.

Frame-start helper `0x1405B7F20` zeroes the pass cursor, clears transient passive
counts, and recycles active callback records into FREE-record pools. These pools
are not pending gameplay callbacks. `0x1405B5BE0` pops a free record and enqueue
`0x1405B5FF0` rewrites its payload before use.

Ordinary shot hitbox registration (`0x1401C9000`) writes transient banks each
eligible tick. Native passive-group build (`0x1405B88C0`) recycles groups and
rebuilds their BVH from current registrations. The inspected immediate-query
path requires requested pass < pass cursor, so cursor zero blocks those queries
before rebuilding. This supports avoiding historical transient-bank/BVH copies
under an explicit admission condition, not clearing the entire collision system.

**Fast-path condition:** both target and abandoned live state have zero committed
continuous registrations and zero pending continuous operations. Preserve live
allocator/cache descriptors, restore authoritative owners/nodes and relevant
control flags, and let normal simulation rebuild transient registrations in
native order. Do not call contact processing as a restore fixup; callbacks create
hit-solver work and modify character state.

Each NodeList has 41 thread/job banks (not 41 categories). Continuous state is
outside the transient rebuild:

| NodeList descriptor | Meaning |
| --- | --- |
| `+0x1028 + thread*0x20` | Pending continuous-active additions |
| `+0x1548 + thread*0x20` | Pending continuous-passive additions |
| `+0x1A68 + thread*0x20` | Pending continuous-active removals |
| `+0x1F88 + thread*0x20` | Pending continuous-passive removals |
| `+0x24A8` / `+0x24C8` | Committed continuous active / passive |
| `+0x24E8` | Continuous-passive BVH |

Descriptors have count `+8`, capacity `+0xC`, ownership `+0x10`, data `+0x18`.
There are 166 relevant counts per NodeList. Continuous record sizes are `0x48`
active and `0x28` passive. If present, retain/snapshot their ordered sparse
closure and pending aliases, including BVH leaf associations/topology unless an
order-equivalent rebuild is proved. Checking only historical emptiness misses
registrations introduced by an abandoned future branch.

SBC at `sCollision+0x43B8`, skin mesh at `+0x6860`, and deferred operation banks
are separate persistent state. Begin-frame drains can destroy their registration
records. Neither ordinary-shot rebuilding nor character clones covers them.
They require independent closure or a validated invariant for the admitted cohort.

Current snapshots are saved before `Rollback_HeadlessTick`, which itself calls
collision begin. Do NOT silently switch to post-begin semantics: split both live
and resim execution consistently, including earlier input/action work, or retain
pre-begin semantics and prove begin runs exactly once against the correct restored
persistent state. This remains an integration requirement.

## Effect Graph Versions

Intercept graph release `0x1405E6890` before node destruction; retain nodes,
combined storage, auxiliaries and a resource pin. Replace/clear must observe the
native release postconditions, not the old roots left published.

| Owner field | Teardown view |
| --- | --- |
| `+0x218` | Node-list root |
| `+0x220` | Combined allocation |
| `+0x228` | Child/aggregate tail view |
| `+0x204..+0x20B` | Counts including joint count at `+0x208` |
| `+0x130` bit `0x01000000` | Graph-ready |
| `+0x1E0`, `+0x12C` | Draw accounting and kill number |
| `+0x108` | Resource pinned separately for the version |

Actual combined extent must be recorded at allocation `0x1405E0950`; owner
`+0x210` includes separate allocations and is not a contiguous copy length.
Nodes retain owner `+0x10`, next `+0x18`, joint `+0x28`, pool `+0x38`, resource
views and context `+0x48`. Joint records also retain their owner. A surrogate
owner copy does not redirect those backreferences.

At GC, an audited graph can be disposed using a temporary teardown view at the
original retained owner's address, under quiescence: save its live fields,
install the old version's view, call native graph release with retention bypass,
then restore the live view. Keep the owner leased until all versions are disposed.
Do not call full owner destruction or doClear against an owner with a live graph.
Concrete node callbacks can read more owner fields; classify that footprint per
supported graph family, not just the direct release function's root fields.

Clearing graph-ready is NOT retirement: `uEffect::move` can rebuild the graph.
Quarantined owners must also be removed from active update/draw reachability.
Restart reuses and overwrites pools, so ordinary per-frame snapshots remain needed.

## GPU Context Adapter

The follow-up resolves a useful non-destructive detach/dispose path for common
contexts. Contexts remain globally registered independently of effect roots.

| Field | Meaning |
| --- | --- |
| `sGpuParticle+0x58 / +0x60` | Head / tail |
| `sGpuParticle+0x138` | Registered-context count |
| Context `+8 / +0x10` | Next / previous |
| Context `+0x1C / +0x20` | Capacity / pending capacity |
| Generator `+0x48` | Context pointer |

Logical retirement can splice neighbors, repair head/tail, decrement count once,
and null context links in O(1). Track membership explicitly: null links also
describe a registered singleton. Use native locking policy (system `+0x30` or
global `0x140E178F0`, lock at system `+8`) and a real update/render-safe boundary.
The registry lock alone does not stop direct consumers through generator pointers.

At GC, null the retiring generator's context pointer and call the saved context
deleting virtual `+0x20` directly. Do NOT call manager destroy `0x1406A1190` on
an already detached context: it would change live global accounting or skip
disposal when the count is zero.

Ordinary/point/line/polyline use `0x14069FBA0`, which only repairs nonnull neighbor
links and frees the allocation; null links make detached disposal safe for that
chain. Packed-quad uses `0x14069FB60` -> `0x14069F7D0`, additionally releasing its
owned buffers/resource. Preserve native renderer retirement contracts for those
resources; CPU playback completion is not synonymous with GPU completion.

### Restore Render Scratch, Not Old GPU Pointers

Common contexts contain sixteen `0x98`-byte slots beginning at `+0x28`. Their
mapped VB/IB pointers and scratch slices belong to a shared pool, not the context.
Do not snapshot/restore those pointers as authoritative simulation state.

Native context reset `0x1406A9480` clears transient slot state without advancing
particles, RNG or owner logic. It also applies pending capacity, so choose the
render-epoch boundary deliberately. The single-slot reset `0x1406A9430` does not
apply capacity; it is an alternative when that transition must remain pending.

Ordinary billboard draw `0x14084ABC0` walks restored CPU particles, fills fresh
context batches, then draws. Ordinary/line/point consumers `0x1406A42C0`,
`0x1406A3090`, `0x1406A5A70` use the current pool. Reactivate/reset contexts and
rebuild once on final normal drawing, not on every resim tick. Do not rewind or
rotate the global pool as a local fix; unrelated effects use it too.

Packed-quad/infinite contexts have persistent phase and owned geometry. Phase
`+0x9C8` is advanced by node update `0x1408161C0`; restore it rather than running
update as a visual fixup. Keep buffer content by content epoch. Crossing an
epoch requires retained generated bytes or captured build-time inputs for fill
`0x140813660`/`0x140813CD0`; these indexed-seed builders do not advance gameplay
but later node/joint parameters need not equal original build inputs. CPU staging
is freed after upload, so retaining the context alone does not retain staging.

## External Effects And Capacity

Native `cParticleManager::stopRequest` (`0x14080EC10`) stops request-related sound
and vibration without freeing graph allocations. It mutates request bits/IDs;
keep external presentation-request ownership separate from rewindable CPU bytes.
Sound cleanup uses the node pointer as a key and old owner policy. Do it at the
native logical time, not after that owner has acquired a replacement graph.

Child-particle close mutates active/free pools and requests child `+0x78` kill.
It is not a non-destructive graph detach. Retain/version the entire child-unit
dependency closure; prevent terminal GC from issuing kill against a reactivated
or reused child. Initial cohorts can exclude child-bearing generators explicitly.

Adhesion children hold a finite count until destructor `0x14097A950` invokes
`0x140544BE0`. Logical retirement must release this charge, while sidecar state
prevents a second decrement at GC. Reactivation restores logical capacity
consistently with the snapshot. A zero-count guard does not protect newer owners.

## Fast Lifetime Bookkeeping

Proposed implementation choices, not native facts:

1. Dense sidecar records with monotonic allocation IDs, actual extents, cached
   type descriptors, graph-version identity, logical-cleanup state and epoch.
   Never rewind allocation IDs or use shot IDs as allocation identity.
2. Per-snapshot deduplicated allocation manifests plus packed CPU byte payloads
   and precompiled copy spans. Classify links at construction/replacement, not
   through address-looking-value scans during restore.
3. The current rollback ring has 16 slots. A per-allocation slot bitmask can track
   leases cheaply; each slot keeps the IDs it leased. Drop that slot's bits before
   reuse, including same-frame rewritten predictions. Use separate active-world,
   render/job and dependency obligations, not just the mask.
4. Compute the complete dependency closure of snapshot roots, including restored
   weak pointer identities, with generation-stamped traversal/deduplication.
   Cache stable ownership cohorts. Cycles are handled by reachability/cohorts,
   not naive reference counting; changing edges invalidate cached closure.
5. Append newly eligible allocations/cohorts to a GC queue when their last lease
   disappears. Avoid scanning the entire quarantine every frame. Keep GC outside
   the restore-critical section; bound started work by measured cohort costs.
   A native destructor is not preemptible, so a time budget is not a hard bound.
6. Snapshot ordered scheduler/shot/constraint/tracker topology and logical
   counters. Restore all endpoints transactionally; normal add/rebind may call
   policy callbacks, reorder elements, or grow storage.

For B captured mutable bytes and K copy spans, capture/restore is O(B + K), plus
manifest/dependency and topology work. Retire/detach is O(number of owned
registrations), commonly constant per simple object. Destruction cost is deferred,
not eliminated. Retention trades allocation churn for live memory and snapshot
bandwidth; it is not inherently faster without measurement.

Approximate storage budget:

```text
snapshot payload ~= 16 * mutable bytes captured per frame
retained heap    = union of active + snapshot + dependency + native-consumer graphs
```

Abandoned prediction branches can increase that union. Fixed N-frame delayed
free is not sufficient with resimulation or manually retained snapshots. Never
free a leased allocation to meet a cap; define an explicit failure policy.

## Integration And Validation

Current `rollback_snapshot.h` has no retained object/graph manifests. Shot/effect
rewind remains absent. `rollback_snapshot.cpp` disables collision-manager external
discovery; existing collision clones concern the current character-oriented path.
They do not establish the persistent closure required above.

Both `HeadlessUnitCategoryMove` and `HeadlessUnitCategoryCleanup` in
`rollback_tick.cpp` directly invoke deleting destructors. They MUST use the same
retirement adapter as the native `0x14051B61C` call. Hooking only native sUnit
would still destroy retained objects during rollback.

`sUnit::reset` (`0x14051BBB0`) has different dispatches and postconditions;
`sShot::reset` (`0x1401515E0`) resets lists/IDs/queues. Treat match/reset/subsystem
teardown as epoch barriers: release history and drain retained cohorts while
managers and resources are still valid, then allow teardown. Detached GPU
contexts are absent from the manager destructor's traversal and need explicit
drain. Direct destructors and failure cleanup need trace coverage too.

Remaining release gates:

- Exhaustive destructor/allocation replacement coverage for each admitted cohort.
- Complete mutable CPU descriptors, including free-pool records until proven dead.
- Model replacement (`0x140568670`), blend-count changes (`0x1405291E0`), motion
  resources and material/cache ownership. Equal-size buffers may be overwritten
  in place; retention alone does not preserve their contents.
- A proved frame/render/resource safe point. Forced inline rendering and jobs
  simplify synchronization but do not freeze async resource loaders or establish
  completion of every native consumer.
- Collision persistent closure and consistently defined pre/post-begin snapshots.
- Logical capacity, child-unit, external-request and scene-palette accounting.
- Packed-content epoch capture for graph types that use persistent GPU geometry.

### Model Ownership Recheck, 2026-11-08

`uModel::setModel` (`0x140568670`) and `setBlendNum` (`0x1405291E0`)
were rechecked after Ghidra access was restored. The following are actual shot
inherited-model allocations, not a complete shot capture descriptor:

| Pointer | Extent / ownership |
| --- | --- |
| `+538` | Joint payload, `N*0xC0`, with `0x10`-byte allocation cookie before the pointer |
| `+548` | Joint-depth table, `N` bytes |
| `+6C0+i*180` | Per-motion joint work buffer, `N*0x68` |
| `+6D8` | Shared slot-zero keyframe allocation, `K*B*0xC`; later slots hold interior slices |
| `+11B0` | Joint interpolation buffer, `N*0x40` |

Use the allocation-cookie count for `N`: `setModel` allocates using the full
resource joint count, but stores a truncated count at `+530`. Although
`setBlendNum` clamps to 16, constructor/destructor evidence establishes eight
inline motion records; the clamp is not permission to traverse sixteen records.
The joint vector destructor at `0x140564900` rewrites joint vptrs and clears model
backpointers before freeing. Retention at allocator free alone is too late.

Animation raw buffers use allocator selector `0x14045D4F0` with descriptor
`0x140E17D88`, allocation virtual `+40` and free virtual `+60`. Joint allocation
uses `0x14056AB90`; freeing its allocation base uses `0x14056AAD0`.

Model replacement also releases/rebuilds materials, command caches and auxiliary
textures through base setter `0x140561CE0` and `initRCN` `0x140566380`. Actor
material-job arrays at `+1408..+1438` contain resource/string references and
self-pointers, and are replaced by `0x1401FFCA0`, not only destroyed at teardown.
Joint constraints retain direct aliases into the joint array. Consequently a raw
DTI-sized shot body copy plus collision bytes is not a safe general restore, and
blindly cloning model buffers is not a valid ownership adapter.

The palette handle at `+116C` represents a scene reservation, not plain scalar
state. It cannot be restored after releasing/reusing that reservation. Native
reserve/release/acquire functions are `0x140779FF0`, `0x14077D210`, and
`0x14077CDD0`; renderer-cycle synchronization and material ownership still need
implementation evidence before enabling generic shot restoration.

Validate capture A -> advance -> restore A byte/topology equality, then identical
input replay. Stress ID reuse before GC, repeated correction branches, partial
geometry replacement, effect restart/resource replacement, child retirement,
adhesion saturation, constraint order, allocation failure, reset and shutdown.
GC after ID reuse must leave all newer registrations unchanged.

Measure warmed save/restore/resim/GC separately: median/p95/p99, bytes copied,
native allocations, factories/destructors during restore, retained high-water
bytes, logical-slot pressure and final-frame draw rebuild. The initial target is
zero native construction/destruction on the retained restore path, not an
unsubstantiated microsecond target.
