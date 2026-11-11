# Delayed Destruction And Same-Address Snapshots

## Follow-Up Audit

See [the implementation audit](delayed_destruction_implementation_audit.md) for
fresh destructor-boundary, collision-generation, transient-collision rebuild and
GPU-context detach/disposal evidence. It also corrects historical shot-node link
offsets and identifies both current headless destructor paths that must join the
retirement adapter. This remains research, not an implemented replacement.

## Implementation Status: 2026-11-03

Follow-up implementation: `rollback_unit.cpp` now has an enabled, bounded
exact-uGameEffect quarantine experiment for empty and supported populated CPU
billboard graphs. It postpones physical deletion
while observational snapshot references exist and drains at epoch close. It does
not restore active shot/effect simulation. A guarded
follow-up restores audited cUnit lifecycle fields and list positions for already-
retiring supported effects only. See `rollback_unit.md`; the full replacement below remains
unimplemented.

The old shot/effect recipe reconstruction subsystem has been removed, including
its capture types, resource pins, class-specific factory repairs, lifecycle
detours and effect-reference sanitizers. The delayed-destruction replacement is
NOT implemented. Native dynamic objects are not rewound; character/input/RNG
rollback still operates, so gameplay involving projectiles/effects is not
rollback-correct in this intermediate build. Shot manager IDs and slot flags
remain live rather than being rewound independently of the objects.

The source references below describe the removed implementation. Retain the
native evidence and design requirements as research, not current behavior.

Research date: 2026-10-31. Target: Ghidra port 8192,
`ichneumon_sub / umvc3.exe.unpacked.exe`, image base `0x140000000`.

This is an implementation design supported by fresh native-body inspection and
current source reading, not an implemented or runtime-validated feature. Existing
Ghidra names/comments are not independent evidence. Representative concrete
destructors were inspected, not every shot/generator subtype. No performance
numbers or complete safe hook coverage are claimed.

## Decision

Use **deferred destruction of intact allocation graphs**, with native-timed
logical retirement, and restore audited CPU state into the same retained
addresses. Deferring only allocator frees, or calling destructors with deleting
flag zero, cannot work: owned state has already been destroyed.

Retaining only the outer shot/effect also cannot work. Collision geometry,
generator graphs, model bindings and auxiliary storage can change independently
of their outer owner's lifetime. Track allocation/graph generations separately.

The intended restore path is a prevalidated list of memory copies and pointer
stores plus ordered registry restoration, not factories, setup callbacks,
particle replay or destructor calls. Whether this is the fastest practical
layout must be measured against its increased capture bandwidth and retention.

## Native Evidence

| Address | Fresh body-level finding | Consequence |
|---|---|---|
| `14051B4C0` | `sUnit` changes state 3 to 4 and removes group memberships; a later eligible state-4 visit changes it to 0, unlinks primary membership, and calls deleting virtual slot zero. | Keep retirement timing; intercept before destruction. These are scheduler visits, not necessarily consecutive frames. |
| `14051BBB0` | `sUnit` reset also dispatches deleting destructors. | Normal move-loop interception alone is incomplete. |
| `1401CCBB0` | Base shot kill retires alive callback effects, stops a sound cookie, then requests state 3. | Do not replace concrete kill behavior with a no-op. |
| `1401C9FD0` | Base shot destructor unlinks the shot, clears its ID, deletes collision state, releases resources and destroys actor/model ownership. | Base-destructor entry is too late for derived members; allocator free is much too late. |
| `1401CA0B0` | Base scalar destructor always invokes the in-place destructor; flags bit 0 only controls final allocation release. | Flags zero is not object retention. |
| `1401BEEE0`, `1401BF700` | Spiral/Storm Sword destructors destroy embedded constraints before chaining to the base shot destructor. | Protect the most-derived destructor boundary. |
| `140447F50`, `1404483F0` | Constraint destruction unlinks joint/neighbor registrations and clears its parent/list pointers. | Quarantine must detach external registrations without destructing the embedded object. |
| `1401BF190`, `1401BE9F0`, `1401BF960` | Spiral/Raid/Storm kill overrides clear Vergil reservation bytes; Raid ID 6 also clears an owner flag. | Execute native kill on the simulated timeline, never replay it during physical GC. |
| `1401BDF50` | Reservation clear is an unconditional indexed byte store at owner `+68F0`. | Delayed kill could clear a newer sword's reservation. |
| `140151490` | ID release writes zero to `sShot+38+4*id`, with no allocation identity check. | Physical GC must not release an ID already logically released and reused. |
| `140151420`, `1401513B0` | Shot-list removal rewrites both neighbors but leaves the removed embedded node's links intact. | A second unlink at GC can corrupt a newer list. |
| `1401C6870` | `releaseAtkInfo` removes geometry references then deleting-destructs primitives; optional full release also deletes wrappers. | Preserve primitives/wrappers before destruction, not just shot allocation. |
| `140161B30`, `1401B48E0` | Active shot phase callbacks call `releaseAtkInfo(slot0, false)` and create replacement hit geometry. | Collision allocation generations change while the shot is alive. |
| `1405A3B20`, `1405A3BC0` | Collision-node destruction deletes array elements/backing storage; object variant can delete user data when its ownership byte is set. | Collision retention needs ownership-aware graph traversal. |
| `1405E6C40` | Effect resource replacement releases the graph, replaces resource/parameters, then conditionally creates another graph. | Track effect graph versions independently of effect identity. |
| `1405E27C0` | Graph construction has multiple stages; failure calls graph release. | Account for partial graphs and failed construction. |
| `1405E6890` | Graph release deleting-destructs linked nodes, frees joint auxiliaries and combined storage, and clears roots/counts/readiness. | Retain the graph before any of those destructive operations. |
| `1405E4CD0`, `1405EF250` | Clear releases graph, then releases the EFL resource and resets state. | Historical graph versions need explicit resource lifetime protection. |
| `1405E4DB0`, `14083F5A0` | Restart reuses storage; ordinary-generator restart closes active particles. | Storage retention does not preserve historical contents; snapshot them. |
| `140827680`, `140827720`, `1409727E0` | Particle close moves records from active to free lists and requests child virtual `+78` retirement for alive children. | Snapshot pool topology, payload and separately registered child units together. |
| `14097A950`, `140544BE0` | Adhesion child destructor conditionally decrements the global adhesion capacity count. | Deferring the destructor without logical counter release changes allocation eligibility. |
| `14051A6B0`, `14051BDE0` | Unit insertion appends to primary membership; group registration removes old membership, invokes virtual policy, and can grow arrays. | Native add/register is not a pure historical topology restore. |
| `1401517A0` | Pending-shot flush registers queued units, clears count and activity records. | Queue state is part of a snapshot unless capture guarantees it is empty. |

The base shot destructor inspected here does not establish a write to the
game-config slot-active flags. Preserve their snapshot state, but do not invent
an additional destructor-side clear from historical prose.

### Effect Storage Extents

`uEffect+210` is NOT a generally valid contiguous copy size for `uEffect+220`.

- `1405E0950`: combined allocation initially contains unique joint records
  (`count * 0x150`), generator-requested pool slices, and an optional child tail.
  Aggregate mode instead allocates `0x150`. Record the actual extent at this
  allocation boundary.
- `1405E54C0` / `1405E55F0`: later initialization adds separately allocated
  auxiliary bytes to the accounting value at effect `+210`.
- `1405E2860`: ordinary generator allocations are commonly `0x240`,
  PrimModel/Adhesion `0x250`, Node/Infinite `0x280`; the list is heterogeneous.
- `140827930`: pool sizing includes aligned position/motion records, life
  storage and optional culling entries. Generator `+34` records its pool extent;
  just copying particle count times position stride is insufficient.
- `14080B5A0`: joint auxiliary allocation pointer `+130`, requested size `+128`,
  interior views `+138/+140`.
- `140830C60`: ordinary-generator auxiliary allocation pointer `+210`, requested
  extent `+C4`; interior pointers refer into this separate allocation.
- `140816670` / `14080B4D0`: generator destruction also handles particles,
  auxiliary memory, sound/vibration and a GPU context. GPU-context destruction
  at `1406A1190` mutates global context topology.

Do not snapshot locks, allocator metadata, COM/GPU internals or temporary draw
buffers as simulation state. The inspected CPU draw consumers do not prove that
CPU snapshot restoration alone makes all retained render contexts correct.

## Lifetime Mechanism

Maintain a plugin-side registry, separate from rewindable native bytes:

```text
Allocation record:
  monotonically unique allocation ID, address, extent, concrete type
  native destructor/deallocator identity
  ownership/dependency edges and graph-generation membership
  active / quarantined / physically destroyed status
  completed logical-cleanup flags
  snapshot leases and outstanding native consumer protection
```

Never rewind the allocation-ID counter. A timeline spawn sequence is not an
allocation identity: different prediction branches can use the same frame/spawn
ordinal. A vtable match also cannot detect address reuse.

### Normal Simulated Retirement

1. Allow native concrete kill, callback pruning and state transitions to run.
2. At the original destructor boundary, finish audited logical removal once:
   shot membership/ID, constraints, capacity counters, collision/render
   registrations, and any class-specific external ownership effects.
3. Keep the most-derived object and all required owned allocations intact.
   A quarantined unit must be absent from active scheduling/collision/drawing,
   even though its memory still exists.
4. Retain each graph version and the resources its borrowed views require.
   Graph replacement must expose native release postconditions to the caller
   while preserving the old graph out of band.

Generic whole-function suppression of graph release is not enough: leaving its
old roots published would make replacement construction operate on stale state.
Conversely, running destructors and suppressing only nested `free()` calls loses
the state needed for resurrection.

### Rewinding Away From A Prediction

Removing a future-only object during restore is NOT a native kill event.
Quarantine and detach it without replaying kill callbacks against restored owner
state. Restore the target snapshot's owner flags, counters, registrations and
object membership as one transaction. Current factory-era retirement policy must
not run afterward and delete or sanitize resurrected objects.

New spawns during resimulation can initially use native factories with fresh
allocation IDs; objects present in the target snapshot need no factories.
Reusing abandoned future spawns is an optional later optimization requiring an
exact spawn-state/side-effect contract, not merely a matching DTI or resource.

### Physical Collection

Collect only when absent from the current world, unreferenced by every retained
snapshot/dependency closure, and unreachable by jobs, render submissions or other
native consumers. Snapshot leases must include saved weak pointer targets when
their identity is restored, not only active object roots. Handle cycles using
graph reachability or owned cohorts, not naive reference counting alone.

Drop leases when ring slots are overwritten, invalidated, or reset, including
discarded prediction branches. A fixed delay of `ROLLBACK_WINDOW` simulation
ticks is not a proof of safety, especially with repeated resimulation.

Call the saved concrete destruction path once under allocation-specific cleanup
state that suppresses logical work already performed. Do not run old ID release,
stale shot/constraint unlink, child kill, capacity decrement or owner sound
cleanup against newer live state. Retain required owner context until dependent
destruction finishes. Never use an out-of-range fake shot ID to skip release.

Collection must also work when the outer effect now owns a different graph:
calling native graph release against its current roots would destroy the wrong
generation. A version-specific disposal adapter and audited external effects
are required before enabling that graph family.

## Snapshot Representation

Replace value-based pointer guessing with per-type and per-allocation-layout
descriptors. Classify each saved field/range as:

| Kind | Save/restore rule |
|---|---|
| Mutable CPU scalars/POD | Exact byte copies, regardless of whether values resemble addresses. |
| Retained allocation link | Save pointer, allocation identity and interior offset; lease target. Restore exact pointer after preflight. |
| Stable external/resource link | Establish a lifetime contract; pin resource where needed. Do not rewind global reference-count internals. |
| Intrusive/global membership | Snapshot ordered topology and restore all participating endpoints consistently. |
| Derived transient/render state | Exclude or rebuild using a proven non-simulating adapter. |

Tagged unions need an explicit discriminator rule; an address-range heuristic
is not an adequate substitute. Unclassified pointer-bearing types should not
silently enter the retained fast path.

Suggested slot payload:

```text
object/graph generation manifest and deduplicated allocation leases
packed mutable CPU bytes
precomputed copy operations {destination, payload offset, length}
retained link records {destination field, saved pointer, target ID, interior offset}
ordered scheduler/shot/group/tracker/constraint membership state
manager scalar and pending-queue state
```

For fully retained CPU-only spans, pointer words can eventually be coalesced
with adjacent scalar spans into the same memcpy. Keep semantic link metadata
for lifetime validation; do not pay a lookup for every pointer on every restore.
Shared suballocations are captured once, not once per incoming link.

Capture complete mutable particle pools (active AND free records), history,
emission state, generator/joint state, auxiliary data and child references.
Preserve exact active/free ordering. Free records cannot be omitted until every
next-use initialization/read path has proved that their prior contents are dead.
Apply the same rule to collision arrays, model/joint/motion work storage and
concrete embedded objects. Retention alone does not rewind mutable contents.

Include pending/setup and retiring states, not just alive units. A retiring shot
can still have a future owner-release callback. Native inactive-but-nonnull weak
links must not be automatically normalized to null during capture.

## Restore Transaction

1. Reach a real simulation/resource/render-safe boundary. Serial category
   dispatch alone does not stop async resource writes or pending renderer reads.
2. Preflight the complete target manifest: every retained allocation/generation,
   resource, destination range and required topology buffer must still exist.
   Reject a broken manifest before modifying the world.
3. Detach/quarantine current-only objects and graph versions without kill replay.
4. Copy saved mutable CPU spans into retained addresses; restore saved links.
   No native callbacks may observe the partially restored graph.
5. Restore manager scalars and ordered topology coherently: `sUnit` primary
   head/tail and node links, group array contents/counts, shot sentinels and
   links, ID state, effect trackers, constraint lists and relevant collision
   registration. Preserve unrelated mixed-category members and their order;
   category 13 is not exclusively ordinary effects.
6. Restore pending queue contents/counts, or assert the chosen capture fence
   makes them empty. Preserve lifecycle states 1/2/3/4 exactly; do not rerun setup
   on saved active objects or pre-advance retiring objects.
7. Reconcile sidecar active/quarantine state, refresh only audited derived draw
   state, then publish and resume. GC runs outside the latency-sensitive copy
   path under the same lifetime/synchronization rules.

Manager topology arrays can restore their contents into adequately sized current
storage rather than restoring historical array backing pointers. In either
case, preserve capacity/ownership consistency and never restore critical sections.
Restoring list order matters: appending shots in spawn-sequence order is not a
general substitute for their captured scheduler/group order.

## Current Source Changes Required

| Source anchor | Current behavior | Required retained-path change |
|---|---|---|
| `rollback_restore.cpp:470` | `RestoreShotAndEffectRecipes` pins, destroys, respawns, registers and overlays. | Replace for covered allocation graphs with the transaction above. |
| `rollback_restore.cpp:571-591` | Explicitly destroys current shots. | Quarantine current-only identities; keep target identities intact. |
| `rollback_effect.cpp:1193-1260` | `Effect_RetireCurrent` synchronously deletes effects. | Do not run this against retained graph cohorts. |
| `rollback_tick.cpp:319-324,392-397` | Headless gameplay and omitted-lane cleanup directly dispatch destructors. | Route both through the same lifetime interception as native scheduler paths. |
| `rollback_shot.cpp:504-565` | Captures tracked linked recipes, filters terminal shots, sorts by spawn sequence. | Capture exact retained world/dependency membership and scheduler order, including pending/retiring units. |
| `concrete_snapshot.cpp:371-382,1005-1064` | Allocates an overlay and preserves qwords if either value looks mapped; scans embedded objects. | Precompile explicit scalar/link/copy descriptors outside restore. |
| `concrete_snapshot.cpp:878-927` | Captures selected effect controls and restores onto compatible graphs. | Add full mutable graph/pool/auxiliary state and graph identity. |
| `effect_generator_state.h:8-22,56-77` | Compact emission state; can recreate a single particle through a phase adjustment. | Not an exact particle snapshot. Bypass this heuristic for retained graphs. |
| `rollback_snapshot.h:280-304` | Slot has recipe/state vectors and region storage. | Add retained manifest, copy/link operations, topology payload and leases. |
| `rollback_snapshot.cpp:1676-1707` | Releases snapshots/resources and resets tracking. | Drop every allocation lease and safely drain quarantine before manager teardown. |

Old reconstruction-only repairs (model setup retries, factory RNG compensation,
character pointer remapping, callback recreation) must not run on the retained
path. Do not remove them globally while uncovered families still reconstruct.
Mixing retained and reconstructed objects is safe only with explicit boundary
link handling; otherwise migrate a connected ownership cohort together.

## Implementation And Verification Gates

1. Add shadow lifetime/allocation tracing first. Record destructor entries,
   independent graph/geometry replacement, sizes, dependencies and native
   registration side effects without suppressing destruction. Report unknown
   owned allocations and direct in-place destructor paths.
2. Enable an audited shot/effect/child cohort, including CPU graph snapshots and
   exact manager membership. Keep unsupported cohorts explicitly excluded from
   the new path rather than claiming universal coverage.
3. Test capture A, advance, restore A, compare every authoritative range/link and
   ordered membership, then replay identical inputs. Cover spawn, pending setup,
   kill, state 3/4, particle reuse, resource replacement and allocation failure.
4. Exercise repeated rollback across ID reuse, Vergil reservations, adhesion
   capacity, Kikosho geometry replacement, graph clear/restart, model effects,
   async model completion, training reset and shutdown. Force GC after ID reuse
   and prove it leaves current manager state unchanged.
5. Assert no duplicate registry membership, reciprocal intrusive links,
   active/free disjointness, valid allocation generations and no destruction
   while leased. Include nested graph ownership and cyclic weak links.
6. Measure warmed save/restore/resimulation and GC separately: median/p95/p99,
   bytes copied, copy/link counts, native allocations, pool pressure and retained
   high-water bytes. First target: zero factory/setup/destructor calls for objects
   already in the target snapshot, no pointer scans, and no temporary overlay
   allocation on the retained restore path.

Retention increases native heap pressure and can itself change factory success.
Apply a consistent retention policy on both peers, budget worst-case abandoned
branches, and never reclaim leased allocations to satisfy a memory cap. Decide
an explicit failure policy rather than silently falling back after partial
restore. Renderer context disposal, async lifetime boundaries, all concrete
destructor side effects and complete model suballocation coverage remain release
blockers, not details proved by this research.
