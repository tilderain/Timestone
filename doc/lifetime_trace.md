# Shot And Effect Lifetime Trace

## Status And Control

`LIFETIME_TRACE_ENABLED = true` in `UMVC3Hook/code/lifetime_trace.h` enables the
development instrumentation. Set it false and rebuild to remove trace hook
installation. It is independent of `lagTest` and backend timing; neither needs
to be enabled. All hooks belong to the existing gameplay-hook lifecycle, so
Normal Launch disables them with the other gameplay hooks.

`EMPTY_EFFECT_QUARANTINE_ENABLED = true` now enables the first narrow physical
retention experiment described below. Set it false to return to trace-only
behavior. `RETIRING_EMPTY_EFFECT_RESTORE_ENABLED = true` additionally enables the
guarded retiring-only restore described below. `RETIRING_CPU_BILLBOARD_ENABLED`
extends it to populated CPU billboard graphs. Active effect simulation and shots
are not rewound; rollback is still incomplete.

## Populated CPU Billboard Retention

The empty-graph baseline below is now supplemented by intact retention of exact
`uGameEffect` objects containing only exact `cParticleGeneratorBillboard` nodes
(vtable `0x140BC4CB0`, size `0x240`). This preserves populated pools and auxiliary
allocations at their original addresses. It is not an active-particle snapshot:
only objects already retiring at the saved frame can be restored to that stage.

Admission checks the complete bounded node list, ordinary completed graph mode,
owner/joint/pool relationships, GPU descriptor mode and null GPU context, no
pending audio/vibration requests, and no child-enabled generators or active
particle child pointers. Aggregate tails, setup errors, other generator classes,
and invalid/cyclic topology are rejected. Maximums are 32 nodes, 32 joints, and
256 KiB of described owner/node/combined/auxiliary storage per retained object.
The existing 128-object cap bounds described retained storage to 32 MiB, excluding
allocator overhead and resource memory retained by existing references.

The combined extent is reconstructed from `jointCount * 0x150` and node pool sizes,
not effect `+210`. Native destructor evidence: `0x140816670` frees generator
auxiliaries and may retire particle children; `0x14080B543` destroys GPU contexts
and services external cleanup requests. Those external obligations are excluded,
not silently deferred. `0x140843730` establishes the billboard GPU descriptor gate.

Resource binding and graph create/release revisions must still match the snapshot.
The graph is retained unchanged, not copied back or reconstructed. Restoring state
1/2, replacing graph generations, abandoned future effects, and owner/observer
registration restoration remain unsupported. Holding a resource reference also
delays resource reclamation; this is not an externally invisible lifetime policy.

`quarantine_hold.c` is now the retained generator count (zero for empty effects).
`quarantine_skip.c` explains failure of the original empty predicate as a bitmask:
1=class, 2=counts, 4=roots, 8=ready, 16=lifecycle/category, 32=links, 64=groups.
This mask does not individually identify the populated-billboard admission failure.
Up to 64 `quarantine_graph` node samples report a=node vtable, b=GPU context,
c=node status, d=node address. They are read before native destruction only.

Use `python3 -B tools/lifetime_trace_summary.py <host-log> <client-log>` to check
hold/restore/re-retire/collect accounting, sequence gaps, and base teardown counts.
Results from truncated logs are explicitly marked incomplete.

The automated frame-limit close now requests `LifetimeTrace_FinishCapture()`.
Actual draining happens at the next main-tick flush, outside rendering and the
recorder lock; recording then stops. Close waits for queued trace output to drain,
so the run can exceed its requested frame minimum by a few ticks. This terminal
test path does not prove that every native reset/destruction path is exercised.

### Populated Capture Results

Run `20261108T042810.373332Z_populated_billboard_terminal_drain` completed both
peers with a 900-frame minimum (901 actual frames after the terminal drain).

| Observation | Host | Client |
| --- | ---: | ---: |
| Populated hold events | 94 | 93 |
| Unique retained identities | 88 | 92 |
| Restored from quarantine to saved retirement state | 6 | 1 |
| Unique physically collected identities | 88 | 92 |
| Topology-rejected restore batches | 2 | 1 |
| Peak held identities | 14 | 11 |
| Remaining held at capture finish | 0 | 0 |

Both complete-prefix traces have zero sequence gaps and zero accounting errors.
Every collected identity has exactly one observed base destructor. Drops, registry
misses, and live-address reuse counters are zero. The final client clear actually
drained populated identity 422 before `capture_finish`, rather than relying on
process exit to reclaim it. Earlier 700/900-frame runs also exercised successful
populated restoration but exposed the missing terminal-drain observation.

These are lifetime/retirement results, not active effect rewind or determinism
results. Desync artifacts remain expected; the runner reports them as
`correctness_regression`. Timing profiles were not produced, so no performance
improvement is claimed.

Startup logs `LifetimeTrace: queued=1 ...` after successful creation and enable
queueing of all 13 trace detours plus four retention/epoch detours when enabled.
Actual application uses the existing queued-hook
commit. If any creation fails, none of these trace hooks is queued. Enable-queue
failure also uses the existing gameplay install failure handling.

## Empty-Graph Retention Experiment

This is a bounded collector test, not general shot/effect rewind. It delays the
most-derived deleting destructor at `0x140139E80` only when ALL conditions hold:

- Exact `uGameEffect` vtable `0x140A8E180`, not a derived class or UI effect.
- Known live trace identity with at least one current snapshot-observation bit.
- Native deleting flag exactly 1.
- Native state 0 and detached category sentinel, primary links null, group mask 0.
- Generator/joint counts zero, roots `+218/+220/+228` null, graph-ready bit clear.
- Recording/retention admission open and fewer than 128 quarantined allocations.

Native kill, group removal and primary unlink have already happened. The wrapper
returns the original object address without running any destructor body. It keeps
the outer allocation and its existing resource reference intact; it does not
acquire a new reference or leave the unit active. Native ordinary and headless
deleting dispatch both reach this concrete entry. In-place calls, linked objects,
unsupported populated graphs, unknown objects, capacity overflow and other types
pass through. Supported populated graphs use the separate admission above.

The native audit supporting this limited split is `uGameEffect` destructor
`0x140139DF0` -> `uEffect` destructor `0x1405E07F0` -> graph release
`0x1405E6890` -> `uBaseEffect` destructor `0x1405EE550`. With the required empty
roots/counts, graph release has no nodes/buffers/contexts/child requests to destroy.
Base effect only releases resource `+108`; coordinate teardown `0x140547450`
rewrites the vtable and jumps to the trivial destructor `0x1409BFE90`. The scalar
wrapper then frees the allocation. This reasoning does NOT extend to populated
graphs, adhesion children, derived classes or shots.

Quarantined identities occupy their registry slots but are excluded from new
snapshot censuses. Dropping/overwriting saved slots clears their observation bits.
At the next main-tick entry, objects with no bits are collected through the saved
original deleting destructor, outside the recorder lock. Nested base hooks still
observe physical teardown. No normal add/register or kill replay is performed.
The optional retiring-only restore can return supported objects to their saved
retirement stage. Duplicate deleting calls on a quarantined object are reported and
do not destroy it twice.

History clearing, main reset, `aAction::final` (`0x140202DE0`), main destructor
(`0x140257D80`), manager destructor (`0x14025A070`) and gameplay-hook disable drain
quarantine before relevant native teardown. Recording-limit exhaustion also closes
admission and drains at the next main-tick entry; cleanup does not depend on the
logger continuing to accept events. A subsequent successfully saved world can
reopen admission while recording remains enabled. Epoch close/capture exhaustion
ends this experiment's observational leases, not a fully implemented rollback
ownership contract.

| Event | Interpretation |
| --- | --- |
| `quarantine_hold` | Native deletion postponed; a=slot mask, b=current held count |
| `quarantine_skip` | Native deletion continues; a=1 eligibility rejected, a=2 capacity full |
| `quarantine_collect` | Last observation expired or epoch closed; original destructor follows |
| `quarantine_duplicate_delete` | Repeated request for an already-held object; a=requested flags |
| `quarantine_restore_pending` | a=restore frame, b=retained identities found before the guarded restore attempt |

An identity can now be present at check time solely because it is quarantined.
`quarantine_restore_pending` alone does not establish successful restoration.
The experiment neither preserves old graph versions nor reconstructs a graph that
existed before the object became empty. Missing snapshots, unknown dependencies,
trace overflow and unsupported objects still prevent general correctness claims.

Use a fresh short gameplay session with failed/empty effects and rollback, followed
by battle reset and normal exit. Look for paired hold/collect events and pending
restore reports, without repeated collection or post-reset holds from the old
epoch. Unsupported visual effects and all shots still show native teardown.
This is not a performance or desync-fix build.

## Retiring-Only Restore

This step restores exact empty or supported CPU-billboard `uGameEffect` objects
that were already in native state **3 or 4 at snapshot time**, with no group bits.
It does not restore state-1 setup or state-2 active effects. Native scheduler
inspection at `0x14051B4C0` establishes that these retirement states progress
through group removal and destruction, without invoking effect simulation.

Capture records up to 128 candidates per snapshot, their identity/revision,
resource identity, category, ordered previous/next identities, cUnit control bytes
`+08..+17`, and delta time `+28`. Only these audited lifecycle fields are copied
back, not the entire effect, resource headers, allocator metadata, graph roots,
borrowed owner pointers or render state. Non-null neighbours must be tracked live
identities with reciprocal links in the same category; otherwise capture marks
the retiring batch unavailable. The full observational census is still recorded.

Native `sUnit::addBottom` (`0x14051A6B0`) and `remove` (`0x14051B9F0`) confirm the
head/tail and reciprocal link layout. AddBottom also registers groups and appends
at the tail, so it is NOT used to restore historical order. Restricting group bits
to zero avoids rebuilding secondary group arrays or invoking registration policy.
The cSystem constructor `0x140531AA0` initializes its critical section at `+8`
even when mThreadSafe is false. Capture/restore acquire that native unit lock
before the recorder lock, matching native callback lock ordering.

The restore attempt runs at the end of an accepted ordinary snapshot restore,
at the existing between-ticks boundary. It preflights the entire admitted batch:

- Snapshot/frame, unit-manager identity, category bounds and capture thread match.
- Each target generation still exists, is live-retiring or quarantined, still has
  this snapshot bit, and has the captured resource identity and revision.
- Targets remain exact-type, empty-graph and ungrouped. Live targets must still
  be in state 3/4; quarantined targets must be completely detached.
- Saved neighbour generations survive. Current live links are reciprocal.
- Removing admitted targets conceptually from the current lists leaves the same
  external anchors around each saved segment. Unknown, missing, reused, cyclic or
  newly occupied gaps reject the entire retiring batch without writing it.

After preflight, the commit only performs memory copies, list-link stores and
sidecar occupancy updates. It removes still-linked targets, restores the saved
segments in their original positions, and changes held targets back to tracked
live-retiring objects. It does not invoke native callbacks, constructors,
destructors, allocation, group registration or kill. Slot ownership/revision
counters stay live. An object may subsequently retire and enter quarantine again
under the same identity; repeated hold cycles after successful restoration are
expected, not double deletion.

Events:

| Event | Meaning |
| --- | --- |
| `retiring_state_restored` | Per identity: a=frame, b=was quarantined, c=category, d=saved state (3/4) |
| `retiring_restore_applied` | a=frame, b=restored target count, c=reactivated-from-quarantine count |
| `retiring_restore_skip` | a=frame, b=reason, c=target count when available, d=held count encountered |

Skip reasons: 1=missing/unavailable retiring capture, 2=manager/thread/category
context, 3=target identity/revision/resource/state, 4=neighbour/list topology.
A valid empty candidate set needs no work and emits no applied event. Rejection
does not block the pre-existing character restore and is not a full-game rollback
success. Missing active objects, future-only objects, manager observers, populated
effect graphs and shots remain outside this feature. A successful retiring restore
must not be advertised as active-effect or whole-world state equivalence.

Use the same short rollback/reset capture. Look for applied records followed by
native retirement or another hold of the same ID, no duplicate links/collections,
and explicit skips rather than unsafe approximate reinsertion. Tests exercise
adjacent targets, singleton head/tail, already-linked replay, unknown/reused
anchors, resource/revision changes and all-or-nothing failure. In-game validation
is still pending.

## Collection

Records use a fixed 8,192-event ring and a 4,096-entry identity table with a
32-probe lookup limit. There are no trace heap allocations, formatting, file IO,
or native callbacks under the recorder lock. Shared kill/unlink hooks ignore
untracked objects without reading their storage. Other untracked lifecycle calls
increment a coverage counter without populating the registry speculatively.
The first 256 such calls per process also emit bounded `untracked=1` samples with
their boundary, object vtable/flags, arguments and caller. Further calls still
increment the counter. Samples consume the ordinary event-ring/capture budget;
`unknown_samples` counts sample attempts, so ring drops can lose their output.
The verified `uUiEffectBase` vtable at preferred address `0x140B213D0` is excluded
from unknown events and counters: UI effects are outside the intended gameplay
scope. This is an exact-type filter, not a blanket exclusion of other unknown
subclasses or all shared base-destructor phases whose concrete vtable is gone.

Recording stops after 65,536 event attempts, including ring drops. Native calls
continue unchanged. Restart the process for another capture. Identities are
monotonically assigned to observed births and never rewound with snapshots.
Address reuse after observed teardown receives a new identity.

At main-tick entry, at most 256 queued records are formatted through the existing
HookLog path. Output appears in the ordinary Timestone log (including host/client
logs). This is synchronous bounded draining, not an asynchronous file writer.
It can perturb timing, particularly with console output enabled: do not use this
build to claim performance gains. Queued startup records can precede frame labels;
the initial frame is -1. The final undrained tail may be lost on exit/crash, and
the existing HookLog retention/rotation rules still apply.

## Record Format

`LIFE` records contain:

| Field | Meaning |
| --- | --- |
| `seq` | Total recorder sequence; gaps can indicate overflow or missing log retention |
| `ms` | GetTickCount64 timestamp at recording, not CPU time or call duration |
| `thread` | Native calling thread |
| `frame`, `resim` | Published enclosing live/resimulation frame context |
| `epoch` | Main-reset entry counter, NOT a complete battle/resource epoch |
| `event` | Boundary name |
| `id` | Observed allocation identity; zero for markers or untracked birth overflow |
| `kind` | Shot/effect when identity is known; otherwise none |
| `obj` | Native object address |
| `birthvt` | Vtable at shot factory return or uGameEffect constructor return |
| `vt` | Vtable at this boundary; zero on post-teardown events |
| `flags` | Raw unit word at +0x10; low three bits are native lifecycle state |
| `a..d` | Event-specific values below, printed in hex |
| `untracked` | 1 for a sampled typed boundary with no known birth identity |
| `caller` | Hook-entry return address, or zero for ordinary plugin markers |

The frame context is one atomic publication shared with worker events. It labels
the enclosing trace phase, not an independent proof of a worker's simulation
ownership. Resim explicitly publishes each replayed frame; IDs/sequence are not
rewound. Snapshot/restore markers are requests, not success acknowledgements.
Subtract the logged runtime image base and add `0x140000000` to map executable
vtable addresses into the preferred-address Ghidra image.
Caller attribution is captured at hook entry and preserved across nested hooks
using thread-local scope. It is a return address, not the beginning of a CALL
instruction; resolve the preceding instruction in Ghidra. Native tail calls can
preserve an earlier caller's return address. A native call executed inside a
MinHook trampoline may also expose a trampoline return address instead of an
image address; do not interpret every caller value as executable-image code.

## Boundaries

| Event | Native entry | Values / interpretation |
| --- | --- | --- |
| `birth` / `birth_null` | Shot factory `0x14029F7F0`, effect constructor `0x140139D30` | a=kind (0 shot, 1 effect); result observed after original returns |
| `shot_publish_begin/end` | `0x140150AD0` | a=team/slot; owner-change reinsertion retains the same identity |
| `kill_begin/end` | `0x140016F20` | Shared cUnit kill; flags show whether state changed; end a=begin identity |
| `unlink_begin/end` | `0x14051B9F0` | Primary unlink, not necessarily destruction; end a=native bool result |
| `shot_base_begin/end` | `0x1401C9FD0` | Base in-place teardown, not most-derived entry or allocator free |
| `game_effect_base_begin/end` | `0x140139DF0` | uGameEffect in-place teardown, same limitation |
| `shot_resource_begin/end` | `0x1401CDAE0` | begin a=old resource, b=requested; end a=requested |
| `collision_release_begin/end` | `0x1401C6870` | a=slot selector (FFFFFFFF=all), b=full-root-release byte |
| `collision_roots` | Same release, before original | a..d=the four root pointers at shot +2058..+2070 |
| `effect_resource_begin/end` | `0x1405E6C40` | begin a=old resource, b=requested; end a=requested |
| `graph_release_begin/end` | `0x1405E6890` | begin a=node root, b=combined allocation, c=resource, d=packed counts at +204 |
| `graph_create_begin/end` | `0x1405E27C0` | begin a=resource, b=resource state at +54 (0 if null), c=effect status at +130, d=node root; end a=bool result, b=node root, c=combined allocation, d=packed counts |
| `main_reset_begin/end` | `0x140259990` | begin increments epoch, a=main; registry is not cleared |
| `snapshot_request` / `restore_request` | Plugin snapshot entry points | a=target frame as uint32, independent of enclosing frame |

Native signatures were checked in Ghidra before hooking. In particular, the shot
DTI helper takes a resource and returns the constructed pointer, rather than the
historical misleading manager/void signature. `sUnit::remove` and graph creation
return bool, which their wrappers preserve. The effect constructor takes its
object in RCX and returns that object. Its factory at `0x14013AB60` is no longer
hooked, avoiding duplicate identities. This covers direct allocation+construction
in MachineGun/OneSpark shot setup as well as ordinary factory paths.

For derived effects that call this base constructor, `birthvt` is the base-stage
vtable, not proof of the final concrete type or allocation size. Later `vt` fields
show the type at each observed boundary. Allocation failure before construction
is no longer an observed effect `birth_null`; shot factory failures remain visible.

Teardown completion uses only the saved identity and pointer value, never object
storage. Nested collision/graph releases still resolve the identity during the
base destructor. The registry removes it afterward. This intentionally ends
tracking at base-body completion; it does not claim the outer deleting wrapper
has already returned or freed the allocation.

## Snapshot Lifetime Observations

The trace now keeps a bounded observational manifest alongside each of the
`ROLLBACK_WINDOW` native rollback slots (currently 16). A successful snapshot save
records every identity still marked live by the tracer, including pending and
retiring objects. This is a conservative census, NOT the snapshot's complete
reachable allocation graph. The separate retiring-only payload contains only the
audited lifecycle fields described above, not full shot/effect simulation state.

Observations are invalidated before a save mutates its ring slot; successful saves
commit a replacement manifest at the end. Same-frame resim saves and modulo-ring
overwrites replace the previous observation. Failed saves leave that observation
absent rather than claiming a valid census. FreeSnapshots and main reset clear all
observations and drain the optional quarantine. Native snapshot payloads are not
extended with effect state. Snapdump's longer
diagnostic history does not acquire these observational slot bits.

Each identity has a slot mask and a monotonic revision counter. Observed base
teardown, resource binding, collision release, graph release and graph creation
advance its revision. These counters are not rewound. They mark calls that can
invalidate owned storage, not proof that a particular call actually freed memory:
an empty graph release or failed creation can advance the revision too. Ordinary
movement and uninstrumented model/child mutations are not covered by this counter.

New records:

| Event | a | b | c | d |
| --- | --- | --- | --- | --- |
| `snapshot_manifest` | Frame (uint32) | Ring slot | Observed identity count | Any prior trace coverage/loss counter nonzero |
| `snapshot_drop` | Previous frame | Ring slot | Previous count | 0 |
| `snapshot_hazard` | Boundary reason | Referencing slot mask | Revision before boundary | 0 |
| `snapshot_manifest_check` | Requested frame | Observed count | Missing/reused identities | Surviving identities with changed revisions |
| `snapshot_identity_missing/changed` | Requested frame | Current ID, or 0 | Saved revision | Current revision, or 0 |
| `snapshot_manifest_absent` | Requested frame | Ring slot | 0 | 0 |
| `snapshot_manifests_clear` | 0 | 0 | 0 | 0 |

Hazard reasons: 1=base teardown, 2=graph release, 3=collision release,
4=resource bind, 5=graph creation. Nested boundaries can report more than one
hazard for the same operation; these are not distinct destroyed allocations.
All LIFE rows additionally carry `snapshots` (slot bitmask) and `revision`.
Missing/changed details use the historical identity metadata; interpret them with
their requested frame rather than as a current-live registry entry.

The restore check runs only after native snapshot availability validation, before
restoring regions. It looks up pointer values plus generation IDs without reading
historical object memory. Address reuse is reported as missing, even if the new
allocation has the same type. It reports full aggregate counts but at most 16
detail rows per check. Restore is neither blocked nor modified by these findings.

This version uses fixed storage for at most 4,096 identities per slot and bounded
registry scans on save/drop. It is a correctness experiment, not the final
incremental lease implementation or a performance benchmark. History-bearing
pointer targets, generator allocation versions, render epochs, external resources
and unobserved destruction still require additional ownership coverage.

**Expected result:** `snapshot_hazard` followed by missing/changed identities on
restore demonstrates where native lifecycle progresses beyond a saved census.
It does not fix the known shot/effect rollback gap. Only the explicitly enabled
empty-graph cohort delays destruction; restoration is limited to the retiring
states and validated list positions described above.

## Coverage Counters

`LIFESTATS` is emitted when counters change:

- `dropped`: ring full; ordering remains sequenced but events are missing.
- `registry_misses`: bounded birth insertion could not find a slot; later lifetime
  events for that allocation cannot be correlated.
- `untracked`: a non-shared lifecycle boundary had no observed identity. This can
  reflect unsupported effect classes, calls before constructor return, missed
  births, or registry loss; it is not by itself evidence of a native bug.
- `unknown_samples`: attempted samples of those calls, capped at 256. Samples
  retain `id=0`; they do not claim an allocation lifetime or a new birth.
- `reused_live`: a birth returned an address still marked live. Investigate missed
  teardown coverage or duplicate factory observation. The new birth gets a fresh ID.
- `stopped`: capture event limit reached, not game shutdown.
- `quarantined`: currently held empty-graph allocations; must return to zero on
  history/epoch close or recording-limit drain, including when event recording stopped.

Nonzero dropped/misses or unexplained reused-live counts invalidate conclusions
that require a complete history. Snapshot markers alone do not enumerate all
reachable dependencies or prove that an allocation is safe to retain/free.

## Initial Capture Procedure

1. Start a fresh mod session and verify trace queueing plus gameplay-hook apply.
2. In training, spawn simple projectiles, let them expire or collide, and repeat
   enough to exercise native address and shot-ID reuse.
3. Exercise an actual rollback correction or the existing save/restore test.
   Look for `resim=1` lifecycle events interleaved with restore requests.
4. Reset the battle and compare normal teardown versus reset teardown.
5. Review sequence gaps and LIFESTATS before interpreting missing events.

This version does not trace every concrete deleting destructor, generator/node
allocation, allocator free, model/cache replacement, child-unit birth, quota or
palette update. Graph root addresses are observations, not generation IDs or
allocation extents. Effects passing through the uGameEffect constructor are covered;
types or inlined construction paths bypassing it are not. Direct shared cUnit kill is
covered, not every overriding kill implementation. Primary unlink can be a
recategorization, and reset can destroy without unlink. These distinctions are
why this trace precedes retention rather than silently acting as a lifetime manager.

## Tests

`tools.test_lifetime_trace` compiles production recorder/wrappers against mocked
Windows/native APIs with AddressSanitizer and UndefinedBehaviorSanitizer. It
checks passthrough, nested teardown, no post-free reads, address generations,
bounded overflow/probes, concurrent producers, capture stop, installation failure,
and disabled mode. It does not validate real MinHook patches or in-game coverage.
