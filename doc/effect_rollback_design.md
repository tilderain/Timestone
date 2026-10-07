# Proposed effect rollback architecture

Status: design proposal, not implemented or benchmarked. Based on the current
source and the lifecycle audits through round six. This document proposes a
replacement for partial recipe restoration, not another generator-phase repair.

## Current-source qualification (2026-10-01)

The rb10 ten-agent native audit and coordinator source inspection qualify the
historical gap table below. This is static evidence, not a reproduced desync or
an implemented replacement snapshot system.

- `concrete_snapshot.h:32-36` now makes `CompletedIndependent()` depend on
  captured inactivity (states other than 1/2), not emission completion. The
  historical table's description of this predicate is superseded.
- `effect_generator_state.h:8-77` still captures selected ordinary controls and
  `hadParticles`, not pool payload, list order, or history. Its phase-5 to phase-3
  path creates another particle; it does not recover the historical record.
- `concrete_snapshot.cpp:897-997` validates graph compatibility but restores only
  ordinary controls. Reconstructed graphs retain fresh generation/census fields.
  The class exceptions at `1110-1138` retain fresh timelines to avoid known
  invalid-history and pool-corruption paths. Removing those exceptions alone is
  not a safe fix.
- `rollback_tick.cpp:75-118` still excludes categories 14-16 from headless move
  and sync. Native cleanup progressing does not prove that skipped live objects
  perform equivalent finish, owner-validation, or late-phase work.
- `rollback_snapshot.cpp:782` labels sEffect RNG presentation-only, and
  `1396-1400` explicitly omits effect-category heap state. These require a
  dependency-specific correctness argument, not a blanket visual classification.

The native audit established these priority type/ownership gaps:

1. Ordinary particles need pool topology, runtime stride, appended history,
   separate life/movement storage, and child-reference fixups. Node/Infinite
   instead use one borrowed 0x80 core record plus mode-selected life storage;
   their matrix fields must never be interpreted as ordinary pool links.
2. The plain 0x150 shared record corresponds to MPR `cEffectJoint`, not the
   polymorphic 0xe0 `cParticleManager`. Effect +0x210 includes auxiliary allocation
   accounting and is not the copy extent of the contiguous block at +0x220.
3. Aggregate generator counters are +0x1f4/+0x1f8; generator +0x1fc is a float
   frame offset. Owner uEffect +0x1fc is a separate two-byte child census.
4. Live uShot metadata overlaps two native floats at +0x2078/+0x207c with
   `mpCollisionSlot3`, and does not represent the full 13 callback pointers at
   +0x1f78. These are target-derived findings: no MPR uShot counterpart was found.
5. Model playback consumes mutable sampler/keyframe work, not just frame numbers.
   Motion slots alias one keyframe allocation (12-byte records); per-slot joint
   work has 0x68 stride. Model particles own inline animation/UV state and refer
   to separately registered synchronized children.
6. Live uGameEffectModel still has an opaque 0x8b0 tail after its 0x1450 base.
   Native evidence includes ten embedded constraints, actor-command/lifecycle
   state, and parent/resource references. No MPR counterpart was found for this
   game-specific class; names must not be invented as donor emissions.

The audit submitted 10 actual narrow signature corrections and six corroborating
no-change contracts under `rb10-` in the separate function ledger. None were
approved or applied during this audit. Larger record layouts remain proposals;
this work does not certify complete Ghidra typing or runtime rollback equivalence.

## 1. Recommendation and correctness contract

Use **identity-preserving, versioned CPU-state rollback for effects and their
gameplay dependency graph**, with separately managed resource residency and GPU
presentation. Keep native lifetime semantics. Retain physical storage while a
snapshot needs it without keeping logically dead units alive.

At the existing pre-frame/main-move snapshot boundary, restoring frame F must
produce the same future-observable state as uninterrupted execution at F:

- Same effect/shot/child identities and graph configurations.
- Same live, pending, sleeping, finishing, retiring and detached states.
- Same callback/reference values, including nulls and not-yet-pruned dead references.
- Same particle history, pool order, node clocks and continuation results.
- Same authoritative RNG, event caches, native counters and scheduling order.
- No constructor, finish, command, collision-event or RNG side effects caused
  merely by installing the snapshot.

The result need not have the same GPU allocation handles. Native CPU pointers
must be valid, and any future simulation reads must observe equivalent data.
Cross-peer checksums compare canonical identities/values, not raw addresses.

## 2. Why the current implementation cannot meet that contract

These are source-level mechanisms, not a claim that every reported incident has
already been reproduced with a matching trace.

| Current source | Gap |
|---|---|
| `rollback_restore.cpp:722-765`, `rollback_effect.cpp:1068-1139` | Most current effects/shots are destroyed and recreated, losing their historical owned graphs. |
| `effect_generator_state.h:8-77` | Captures selected ordinary control fields and a hadParticles boolean, not the pool/history. The phase5-to3 repair emits a new particle rather than restoring the old one. |
| `concrete_snapshot.h:32-35` | CompletedIndependent treats finished emission on an awake effect as completion; native finished emitters may still have live particles. |
| `concrete_snapshot.cpp:897-997` | Partial graph validation, ordinary-only restoration and fallback clocks cannot restore omitted Node/Infinite/aggregate or particle state. Reconstructed graphs keep fresh generation/census values. |
| `concrete_snapshot.cpp:1098-1172` | Shared cleanup-state detection retires recreated callbacks; class exceptions instead retain fresh timelines. Neither is a general historical restoration contract. |
| `rollback_tick.cpp:75-118,507-522` | Categories14-16 omit normal move/sync during headless replay; cleanup still runs. Required effect/child work cannot be classified by category alone. |
| `rollback_snapshot.cpp:781-782,999-1001,1395-1400` | Effect RNG is considered presentation-only, and effect manager/heap state is omitted. That needs revision for gameplay-coupled effects. |
| `rollback_effect.cpp:489-519,754-872` | One factory recipe path plus explicit exceptions does not cover all native creation/ownership paths. |
| `rollback_rng.cpp:24-57,108-135` | Address/callsite presentation classification includes former particle-system addresses now identified as uChain code; sharing a stream with visual work is not proof of gameplay independence. |

Removing one heuristic in isolation is insufficient: retaining phase5 while
recreating an empty pool makes the visual disappear; restarting the pool resets
history and can extend lifetime; retaining an advanced pool with old clocks mixes
two timelines. Replace those policies only when the complete replacement path is ready.

## 3. Define rollback ownership by dependency, not by appearance

Start conservatively. An effect belongs to authoritative simulation if any of its
state, callbacks, RNG draws, allocation admission, collision results, transforms,
resource availability or completion can influence gameplay or another authoritative
object. Include transitive dependencies and shared manager state.

This includes shot callbacks, model effects, aggregate children, relevant synchronized
children, linked effects, command/Motion state and collision-event caches. Black Hole
is a direct example where empty callback slots can end active projectile lifetime.

An independently presented effect is eligible for a cheaper presentation path only
after proving no such feedback, including shared RNG, counters and allocator-capacity
competition. DTI alone is insufficient: graph descriptors, owner role, configuration,
Custom overrides and native virtual targets can change the contract.

Maintain a versioned policy manifest keyed by concrete type, graph configuration,
resource content/version and role. Unknown configurations use a verified exact
capture path; if none exists, report missing coverage during development. Do not
silently call restart or label an unknown callback cosmetic.

## 4. Identity and allocation ownership

Introduce a sidecar registry, preserving native object layouts:

```text
EffectId / UnitId = deterministic spawn identity
AllocationId     = local slot plus non-reused physical generation
GraphVersion     = resource/configuration generation
ObjectRecord     = type, lifecycle, graph version, allocation set, typed references
```

Logical spawn identity derives from a match epoch, owner identity and rollback-owned
spawn sequence/cause. Presentation deduplication also uses command/callback site and
ordinal. Frame number or heap pointer alone is not an identity. A changed spawn
configuration invalidates cached birth state even when a command site matches.

Snapshot the logical sequence state. Keep physical generation tokens monotonic
outside rewind so recycled storage never accidentally validates an old reference.
Register all birth paths: direct constructors, factory variants, aggregate children,
model effects and synchronized particle children, with pending registration represented.

Track owned CPU allocations at their allocator/constructor boundaries, including
generator nodes, shared pools, manager subbuffers, track history, model/material
runtime and child buffers. In-place graph reset and same-resource rebinding can
mutate or replace these; graph versions cannot be inferred from EFL identity alone.

## 5. Logical destruction must remain on schedule

Maintain two independent lifetimes:

1. **Native logical lifetime:** finish requests, particle drain, sleep, state3/4,
   callback pruning, manager removal, collision-capacity release and resource-owner
   changes occur at their verified native points in forward and replay execution.
2. **Physical lifetime:** storage and required external resources remain available
   while retained history, pending jobs or GPU work can reference them.

Do not defer entire destructors blindly. Adhesion capacity is decremented in its
destructor; postponing that change alters later child admission. For supported
families, retain storage by intercepting relevant physical frees while logical
cleanup executes, or implement an audited split between logical cleanup and final
reclamation. Global effects of cleanup belong to rollback state. At collection,
release physical storage only; never replay a second native logical teardown.

Rollback installation discards an abandoned future by reconciling the registry and
restoring shared state, not by issuing fresh native finish/kill gameplay events.
Any necessary external handle cleanup must use the audited physical-release path.

Pin immutable resource dependencies once per retained graph/version. Snapshot leases
are separate from native owner references; reconcile owner references on restore
instead of copying resource refcounts or AddRef-ing every snapshot. Resident resource
policy must be consistent for both peers; physical retention must not change logical
capacity/admission decisions. Audit any gameplay observer of physical refcounts.

Reclaim only when there are no current-world, snapshot-version, pending-job or GPU
references. Age alone is not sufficient for sleeping/looping graphs. Abandoned
branches lose their history references when superseded; do not accumulate every
prediction branch forever. Return reclaimable blocks to type/size pools in batches.

## 6. Exact CPU snapshots first, compact codecs second

Build per-family codecs with explicit byte ranges, allocation ownership and pointer
fixups. Establish a full supported-CPU-state reference implementation before removing
fields. DTI size is not a map of external ownership. Do not restore locks, OS/GPU
handles, allocator headers or arbitrary pointer-looking qwords as scalar state.

| Family | Required historical state |
|---|---|
| uEffect/uGameEffect | Native state/flags, all timers and timing coefficients, owner/watch/request/subparent links, graph version, restart/hold/finish state, generation token and phase-local counts. |
| Ordinary generator | Emission phases/counters/RNG/serials, transient work state when observable, active/free topology and order, complete future-read particle fields and track/history buffers. |
| Plain manager record | Mutable record fields, seed/history selectors, optional allocation contents and internal-view relationships. Preserve its distinction from polymorphic cParticleManager. |
| Node/Infinite | Borrowed track state, node-specific phase/cursor/repeat/modulators and continuation counts; no ordinary-pool interpretation. |
| Aggregate | Child identities and tokens, configuration, scheduled census state and spawn/admission results. |
| Model effect/child | Actor command position/time, pending/current animation, Motion flags/current/prior masks and caches, relevant joint/constraint/material state and resources. |
| Light/Adhesion/Force/Custom | Concrete future-read state, record resources, rebuild flags, child controls/culling state, callback-specific buffers and side effects. |
| Shared managers | Primary/list order, masks, pending work, fixed and dynamic tracking arrays, IDs, capacities and logical counters, relevant RNG and camera/collision state. |

Initially capture the complete supported mutable CPU pool. Free entries can contain
state read by partial initialization; omit their payload only after proving write-before-
read for every field on reuse. Active count alone does not reproduce free-list order.
Never derive ordinary completion from active-head null or phase alone.

Retained same-address allocations permit bulk copies of validated ranges and stable
internal pointers. Typed external links use IDs; relocation uses explicit recorded
offset fixups. Self/owner pointers, model joints and child records are not recovered
by an address-plausibility scan.

Snapshots must be immutable. Use separate restore-resolution tables rather than
mutating saved spawn arguments or stored links. Pending, sleeping, retiring and
detached-but-reachable units need representation even if absent from an active list.

## 7. Restore transaction

```text
Restore(F):
  fence simulation jobs; separate live GPU resources from CPU restore writes
  validate snapshot, codec/configuration versions, and all retained dependencies
  enter restore-install mode (no simulation events)
  reconcile target identities, allocation versions and logically absent future units
  restore supported CPU allocation contents and object control state
  fix typed owner/parent/request/child/callback/self references
  restore global logical counters, RNG and ordered scheduler/manager membership
  restore phase-local censuses and Motion/collision consumer caches
  validate topology and references; publish the world atomically
  replay F..N using the normal deterministic simulation phase contract
  build presentation for N once; commit visual/audio event reconciliation
```

All referenced objects/allocations are materialized before any links are published,
so cycles are handled by two-pass resolution. Preserve saved callback nullness and
pending-prune values; do not replace them with an active-count inference.

The safe checkpoint is the existing defined pre-frame boundary. Do not add an
extra mutating census, Motion evaluation or collision-event query during restore.
Rebuild only proven pure derived state. Record category membership order, including
unaffected native units needed to place restored units correctly; a sorted ID list
is not automatically the native order.

A missing required generation is a restore failure, not permission for partial
clock overlay. An exact fallback must restore a complete earlier world checkpoint
and replay all dependencies/inputs, or have a certified full object-state codec.
A birth recipe without historical state is not that fallback.

## 8. Forward/replay parity and RNG

Replace the coarse category14-16 skip with object/graph policy dispatch. Required
objects run setup/move/sync/census and retirement at the same relative phases on
both peers and in forward/headless execution. Category13 is heterogeneous too.
The owner shot/model path must migrate with its effects; otherwise reconstructing
the owner continues to create duplicate callbacks and advance command/model state.

Initially preserve native RNG streams and ordering for authoritative effects,
including sEffect and manager seeds. Do not treat sEffect as cosmetic merely
because it has an effect label. Review the current caller-address isolation rules,
especially renamed uChain functions. Replaying genuine simulation births consumes
RNG normally; restore installation consumes none.

Keep gameplay-relevant camera/frustum inputs deterministic and restored, distinct
from local presentation interpolation. All-culled, zero-selected-camera and boundary
policies can affect lifetime. Local display quality or GPU capacity must not select
different authoritative effect/child completion paths.

For certified presentation-only work, isolate RNG by logical effect/event identity
and prevent writes to authoritative managers or capacity pools. This is a separate
optimization, not a wholesale replacement of native authoritative random behavior.

Do not replay old native factories merely to restore surviving effects. For genuine
resimulated births, use retained/pool storage but execute verified initialization and
birth side effects. Returning a cached pointer skips RNG/registration/owner changes;
that optimization requires its own equivalence proof and is not phase-one design.

## 9. Presentation and infinite-effect prevention

Keep rollback CPU state and rendered artifacts linked by logical identity. Suppress
GPU submission during replay, not arbitrary native callbacks named draw/prepare:
some update children, bounds, counters or callbacks. Regenerate GPU-only vertex,
index and transient command buffers once from the final CPU state when proven safe.
If simulation reads a buffer, retain its CPU shadow or classify it as authoritative.
Never restore/release a handle still in flight; use leases/fences or replacement buffers.

At final presentation, reconcile IDs: reuse matching live visuals, create newly
required ones, and remove artifacts from the abandoned branch. Native pending finish,
particle tails, sleep and explicit loops still control their lifetimes. An artifact
must not restart its age on each rollback. Event-keyed reconciliation also prevents
duplicate audio and gives loop sounds an explicit owner/stop lifecycle.

For proven independent long-lived visuals, checkpoint their CPU presentation state
and record needed transforms/controls; replay only from the nearest checkpoint to
the final displayed frame. Never resimulate an old effect from birth on every
rollback. This requires its full visual input history, not just elapsed age.

No global TTL, forced slot clear, or permanent keep-alive is a correctness fix.
Diagnostic watchdogs should report unexpected lifetime/progress, not alter gameplay.

## 10. Performance strategy

Prioritize elimination of teardown/recreation, repeated history regeneration,
pointer classification and frame-path allocation. Keep validated type/range/fixup
plans cached from birth/configuration discovery. Use pooled native storage, flat
snapshot arenas, reserved metadata and O(1) identity lookup instead of repeated
linear searches and per-generator vector allocation.

Start with contiguous copies of supported mutable ranges. Then benchmark immutable
chunk sharing, sparse dirty ranges, XOR/delta storage or compare-before-copy for large
buffers. Dirty tracking is sound only with complete writer coverage, including child
and callback writes; otherwise copy conservatively. Hash equality alone is not an
exact-state guarantee. OS page-fault COW is not the default: small frequent writes,
exception costs and mixed native allocations can make it slower.

Share immutable descriptors/resources/graph topology versions across the ring.
Do not copy GPU data or regenerate rendering for each replay frame when it has no
feedback. Keep variable delta chains bounded for random restore. A warm surviving-
graph restore should require no native construction, no driver allocation and no
OS heap allocation. Genuine new births and graph configuration changes remain work.

Memory model: retained snapshots + unique quarantined native blocks + shared graph
versions/resource leases + staging/GPU in-flight storage. With 16 retained frames,
4 MiB of mutable payload per frame would cost 64 MiB for raw snapshot payload alone;
this is an illustration, not measured effect size. Budget against maximum supported
live particles and branch history, not just average battles. Reclamation must plateau.
Under pressure, compress/share history or reduce prediction before exhausting the
supported window; never evict still-required authoritative state or kill effects.

Measure total forward-frame overhead plus save/restore/resimulation/final-render/GC
cost, P50/P95/P99, allocator calls, bytes copied and peak retained bytes. The old
September11 workload measured roughly1.62-1.72 ms per replayed frame, with native
tick around67-70% and shot/effect restoration around5-7% of total. Those numbers
are historical, not a benchmark of this proposal; retaining objects does not remove
all native work, and exact missing effect simulation can initially increase cost.

## 11. Implementation sequence and integration

1. **Reference and diagnostics.** Add canonical effect/child/resource IDs, coverage
   manifest, graph/state hashes and phase event traces. Capture actual lifecycle
   divergence before changing behavior. Add exact full-state reference codecs.
2. **Allocation/resource lifetimes.** Implement registry, owned-allocation tracking,
   graph versions, logical cleanup versus physical quarantine, resource leases and
   history reference collection. Test death/restore and address reuse first.
3. **First complete vertical slice.** Ordinary Billboard/single-shot with its owner,
   managers, full particle state, callbacks and ordered registration. Prove equivalence
   through finish/drain/death before expanding. No scalar/fresh-pool hybrid fallback.
4. **Full family coverage.** Tracks/strips/trails; Node/Infinite/aggregate; model and
   synchronized children; Adhesion/collision; Custom and concrete gameplay effects.
   Cover initialization failures, sleep/restart, replacement and mid-pool reuse.
5. **Restore and simulation integration.** Replace destructive recipe reconstruction
   for certified dependency graphs, restore manager state and RNG, migrate owner
   shot/model paths, and make forward/headless scheduling equivalent.
6. **Presentation and optimization.** Final-frame rendering, event reconciliation,
   immutable versions/dirty-copy experiments, certified cosmetic separation, and
   measured allocation/birth reuse. Remove obsolete heuristics only after parity.

Suggested responsibilities (new files are proposed):

| File | Responsibility |
|---|---|
| `rollback_effect_world.{h,cpp}` | IDs, graph versions, ownership and coverage registry. |
| `rollback_effect_state.{h,cpp}` | Family codecs, snapshot ranges and typed fixups. |
| `rollback_effect_lifetime.{h,cpp}` | Logical cleanup, retained storage/resources, GC. |
| `rollback_effect_presentation.{h,cpp}` | Final-frame rendering and visual/audio reconciliation. |
| Existing `rollback_effect.cpp` | Native boundary hooks feeding the registry; retire recipe-only policy for certified families. |
| `rollback_snapshot.{h,cpp}` | Immutable effect snapshots, shared versions and manager state. |
| `rollback_restore.cpp` / shot/model policy | One coordinated restore transaction across the dependency graph. |
| `rollback_tick.cpp` | Matched authoritative forward/headless scheduling. |
| `rollback_rng.cpp` | Correct stream ownership and isolated cosmetic execution. |
| `concrete_snapshot.*`, `effect_generator_state.h` | Replace heuristics after complete codecs pass reference comparisons. |

## 12. Tests required before calling the solution complete

Use the existing autotest/profiling/dump tools, extended with semantic effect hashes.
The current byte-array generator tests validate selected writes, not native lifetime
equivalence; their finished-emission assertion is not a suitable completion oracle.

- **Restore identity:** save F, run, restore F; compare canonical state before any
  simulation update. Repeat restore twice with no tick; both results identical.
- **Same-input replay:** uninterrupted trajectory versus rollback/replay at every
  supported depth, comparing each frame and selected phase boundaries.
- **Corrected-input replay:** compare against a fresh uninterrupted run using the
  corrected inputs, not the discarded prediction. Add repeated overlapping windows.
- **Lifetime boundaries:** pending setup, phase3 single spawn, phase5 live tail,
  hold/repeat/restart, sleep/wake, finite expiry, forced cull, state3/4 destruction,
  owner loss, callback replacement and callbacks already null/inactive.
- **Allocation ownership:** reused addresses/types, graph generation changes,
  parent death, child buffer failure, strong EFL clear, independent resource retain,
  same-pointer replacement preconditions, quarantine reclamation and GPU fences.
- **Families/assets:** Doom missiles, Black Hole, Soul Fist/Hadouken, Jam Session,
  Inferno, Attack Wire, Ground Oil/Dark Phoenix, model effects, Kirigakure/Ragnarok,
  Rocket effects, ExAtkSpace, Adhesion/Model/Light/Force, Node/Infinite and Custom.
- **Scheduling:** extra event-query probes must not be used as repair; validate
  integer/fractional cache cases, pause/hitstop, camera masks and no-selected-camera
  policies, category order and deterministic job completion.
- **Soak/performance:** repeated worst-case rollback, simultaneous spawn/retire
  pressure and long paused/looped effects. Memory/counters plateau after history
  expiry; no address leaks, duplicate callbacks, hidden logical capacity consumption
  or burst of accumulated native teardown.

Hash separately: gameplay, effect control, particle/history, references/topology,
resources/capacity, RNG and presentation. Canonicalize pointers to IDs and omit
uninitialized padding; compare declared state, not arbitrary object-byte hashes.
Visual captures at final frames check tail age and placement in addition to gameplay
hashes. Record dropped repair/sanitation paths as correctness failures in these tests.

Acceptance: supported configurations match the reference through all above paths;
warm restore has zero native reconstruction/driver allocation; bounded memory and
measured tail latency beat the reference or justify each remaining cost. Do not
promise a millisecond target or full roster coverage before these results exist.

## 13. Remaining native work

Research established the failure mechanisms, not every codec field or ownership
hook. Outstanding requirements include complete Custom/derived graph coverage,
Adhesion after strong clear, material and shared binding runtimes, logical destructor
side effects, selective BVH root validity, complete status/RNG writers, and real
event-cache scheduling. These are implementation work items with explicit coverage
gates, rather than reasons to continue mixing fresh and historical state.
