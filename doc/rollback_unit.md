# Shot And Effect Rollback (RollbackUnit)

## Status And Control

`ROLLBACK_UNIT_ENABLED = true` in `UMVC3Hook/code/rollback_unit.h` enables the
development instrumentation. Set it false and rebuild to remove trace hook
installation. It is independent of `lagTest` and backend timing; neither needs
to be enabled. All hooks belong to the existing gameplay-hook lifecycle, so
Normal Launch disables them with the other gameplay hooks.

`ROLLBACK_UNIT_LOG_ENABLED` in the same header (currently `false`) is the whole
tracer. Off, the build has no event ring, no per-identity revisions, no caller
attribution, no `LIFE` / `LIFESTATS` lines and no `LIFE_PENDING` crash section,
and six detours that only record are not installed (unit kill `0x140016F20`,
unlink `0x14051B9F0`, scheduler add `0x14051A6B0`, effect owner bind
`0x1400604E0`, effect resource bind `0x1405E6C40`, graph restart `0x1405E4DB0`),
leaving 30. Identities, snapshots, retention and restore are unaffected. The
revision field in snapdump shot records and `native_caller` in
`lifetime_failure.log` read 0. Every section below that describes events,
counters or record formats applies only with the flag on. The unit tests build
with it on; `test_tracer_off_keeps_lifetime_tracking` covers the shipped
setting. The startup `RollbackUnit:` line reports it as `trace_log=`.

`EMPTY_EFFECT_QUARANTINE_ENABLED = true` now enables the first narrow physical
retention experiment described below. Set it false to return to trace-only
behavior. `EFFECT_RESTORE_ENABLED = true` enables the shared restoration transaction.
`ORDINARY_CPU_EFFECT_GRAPHS_ENABLED` enables the audited owned-allocation descriptor;
`EFFECT_SIMULATION_SNAPSHOTS_ENABLED` adds active graph payload snapshots. These
remain partial effect coverage, not complete world or shot rewind.

## Snapshot Ownership

### Current Integration

#### Shared Snapshot Storage

Lifetime captures are owned by `RollbackSnapshotSlot::lifetime`, not a separate
tracer history ring. Each capture contains the saved identity census, ordered
primary/group membership, effect roots, graph descriptors, and CPU graph bytes.
Snapdump shares that exact capture alongside the frame's regular region backing,
so effect history covers all 255 recorded frames without another native capture
or a second diagnostic byte copy. Storage vectors retain their capacity for bank
reuse; memory usage depends on actual captured high-water sizes.

`g_liveSnapshots` is only a non-owning index for the current 16 rollback leases.
Drop/clear removes those index entries and registry/generation lease bits but
does not modify history-owned snapshot bytes. Shared snapshot destruction releases
CPU storage only; it never destroys native owners, releases resources, or extends
their leases. The dump exporter reads the frame-owned capture even after native
GC has run. See [desync_snapdump.md](desync_snapdump.md) for the v5 region layout
and the separate, non-historical dump-time shot/model observations.

#### Borrowed Owner Lifetime And 0x14004F0BC

The supplied `nplive_20261108_015137_client` crash reaches
`uGameEffect::updateDeltaTimeCoefficient` (`0x14013AC40`) and then
`MtObject::isKindOf` (`0x14004F0B0`). At `0x14013AC4F`, native code loads the
effect's owner from `+280`. The crash owner is `0xFA29230`; its first qword is
`0x1728`, so the virtual `+20` call at `0x14004F0BC` reads `0x1748`.
The preceding native alive test only checks lifecycle bits, not the vptr.
This establishes an invalid owner reference, not the owner's original class or
whether freeing versus an unrelated overwrite damaged it.

Captured root pointers at `+40/+138/+1F0/+278/+280/+288` now lease their
referenced `cUnit` allocations. Already tracked identities reuse their slot
leases. Newly discovered live units are classified by native DTI ancestry and
registered with the same most-derived deletion interception as shots. This
includes model-backed effects and other unit owners outside the old shot/effect
census, without a character/type whitelist. Adopted shots recover actual native
team-list membership instead of trusting stale own links. Non-cUnit borrowed
allocations still need their owning-allocation adapter.

Dependency leases follow snapshot-slot replacement/drop/clear, including saved
references to quarantined owners. They preserve original allocations and native
resource ownership; they do not add snapshot resource refs or copy a model to
another address. Eligible effects are physically collected before borrowed outer
owners at the same collection fence. Generic-owner immediate disposal also
tombstones its identity, since it has no shot/effect base hook to do that.
Snapshots publish only after dependency acquisition completes. This is lifetime
closure for these direct typed links, not full saved-state restoration of all
borrowed owners or their recursive external dependencies.

One detail in the supplied trace remains unexplained: effect 76 (`0xFAE04C0`)
was quarantined at frame 320, yet the crash stack reaches its native move path
at main frame 324. The inspected native factories, restart, weak-slot pruning,
and `addBottom` do not demonstrate constructor-free resurrection. The last
printed restore is frame 312. Do not claim the dependency fix identifies the
writer that made this object active/reachable again.

Cold-path diagnostics now report registration of quarantined units and owner
rebinding on quarantined effects, without changing native behavior or skipping
restoration. The crash report also snapshots the last 64 pending trace records as
`LIFE_PENDING`, because this crash's new Mystic Blast shot appears on the stack
but its birth/resource events had not reached the normal log flush. This crash
snapshot uses a nonblocking trace-lock attempt, leaves the queue intact, never
runs collection, and never dereferences historical object pointers.

Regression tests cover generic borrowed-owner retirement, old/new owner rebinding,
multiple snapshot leases and aliased links, saved-pointer restoration, effect-first
disposal, generic identity tombstoning, adopted-shot list/ID cleanup, and crash
queue output after object storage has been freed or while the trace lock is busy.
These tests do not establish that the supplied native crash is fully resolved.

Validation: `make -j8`, all 38 tests, and `git diff --check` pass. Run
`20261108T092958.058808Z_effect_borrowed_owner_complete_trace` completed both
900-frame peers without crash artifacts. Both unit rollback traces are complete from
sequence 1, with zero sequence gaps, dropped events, registry misses, remaining
holds, accounting errors, or restore skips. Host/client collected all 331/325
distinct held allocations: 316/311 effects, 14/13 shots, and one borrowed unit
each. That borrowed unit was a `uGameEffectModel` (vtable `0x140A8E510`), retired
at frame 2 and collected at frame 19. Payload restores totalled 1337/1187.
Dependency diagnostics log new dependency periods, not every slot lease; the
complete run emitted only 36/37 such events.

The preceding 900-frame run `20261108T092716.488648Z_effect_borrowed_owner_retention`
also completed without crashes, but repetitive lease logging had discarded its
early log prefix. Run `20261108T093248.337084Z_effect_borrowed_owner_mystic_long`
completed both 2000-frame peers without crash artifacts; its log prefixes were
also discarded, so missing-hold warnings in its suffix summary do not establish
complete-run lifetime accounting. No retired-unit registration/owner-rebind
diagnostic was observed in the retained logs. The complete 900-frame trace does
not show a Mystic Blast birth, and coverage of that exact attack in the longer
run is unconfirmed. The original post-quarantine move remains unproven, not
declared fixed merely because these smoke tests survived.

Desync artifacts remain in all three runs, and no timing-profile windows were
produced. The runner's `correctness_regression` classification reflects detected
desync dumps, not a comparison proving this patch introduced a new regression.
These runs establish exercised retention/collection, not complete determinism or
a measured performance improvement. Summaries are saved in each run directory.

#### Shot And Model Terminal Retention

The concrete shot factory now records each completed object's deleting target.
The first instance of each vtable redirects that vtable's slot 0 to a detour
(`GameplayHooks_PatchPointer`); vtables sharing a deleting target share one of
the 256 target entries. This is a single pointer store, restored by the
gameplay-hook disable path. It replaced an inline MinHook detour, whose
installation suspends every thread and stalled the match for about 60 ms the
first time each class appeared. Only virtual dispatch through a registered
vtable is intercepted. The detour
intercepts before any most-derived destructor body runs. A snapshot-leased shot
is quarantined intact, including its inherited model, joint/animation buffers,
materials, model resources, collision allocations, and concrete owned members.
There is no per-character/type whitelist, surrogate model, byte-pointer scan,
per-buffer destructor patch, or snapshot resource `addRef` in this terminal path.

Logical retirement removes primary/group membership if still present, unlinks
the native shot list once, and releases its ID once. It does not replay kill.
Physical collection waits for the final snapshot lease, then calls the saved
most-derived deleting trampoline exactly once. Scoped base-destructor context
suppresses only the already-retired shot's duplicate list/ID releases. An
unrelated shot destroyed by that destructor still performs its own releases.
The model's scene-palette reservation remains owned until physical teardown;
this trades retained native capacity for same-address lifetime safety.

Static helper hooks are `sShot::removeShotFromList` (`0x140151420`) and
`clearShotIdEntry` (`0x140151490`). A direct `sUnit::reset` (`0x14051BBB0`)
closes snapshot history before its destructor dispatches, complementing the
existing earlier main/action/manager epoch barriers. There are now 36 static
lifetime hooks, plus one lazy vtable-slot redirect per shot/adopted-unit vtable. Normal and
headless virtual deleting calls use the same concrete detour. Unleased objects
retain native destruction. A leased in-place/non-deleting destructor invocation
is an ownership invariant failure, not permission to destroy retained state.

This implements **terminal shot/model lifetime retention**, not shot-state
restoration. Concrete shot body and audited joint/depth/motion/interpolation/key
bytes can be captured into the snapshot-owned history (see Shot Payload Capture
below). Completed-construction
DTI/size is cached at registration; the same typed span walker serves historical
capture and dump-time observations. Historical export never dereferences their
saved native addresses, including after an active buffer replacement or native GC.
Active model pose/work/key retirement and normal attack-info release now use
coarse ownership generations, described in `shot_restore_ownership_audit.md`.
Only generation IDs/slot masks are attached to snapshots, not per-leaf leases.
Generation storage staging is tested but not connected to frame restoration.
Actor material/controller rebuilds, standalone geometry replacement, full shot
membership/ID rewind and the complete world restore transaction still require
integration. Terminal retention alone does not
claim those mutations are safe to rewind. It also does not establish complete
logical-capacity/dependency closure for every concrete shot's derived teardown.

### Shot Payload Capture

Every save captures the whole-shot bytes: body, model joint/depth/interpolation/
motion work/keyframe spans, collision root/entry/wrapper/primitive spans, and
constraint node images. Saved-shot restore and the snapdump recorder read them.

An earlier `RollbackUnitShotPayload::Omit` mode skipped those copies while shots
were only withdrawn on rollback, never restored. It became unreachable once
saved-shot restore shipped and was removed, with the per-stage checks and the
`diagnostic.lifetime.shot_payload_omitted` dump record that existed for it.

### Registry And Generation Indexes

Registry loops visit only active slots (Live, Quarantined, Collecting) through the
`g_activeSlots` bitmap, in ascending slot order with the mask re-read at each step,
so they match a full index loop over the entries they accept. `Birth` is the only
way into the set and `TombstoneIdentity` the only way out; tombstoning also clears
the slot's lease bits, which nothing reads afterwards.

Shot generations are indexed per owner in `g_ownerGenerations` as `{slot, id}`,
appended in `NewShotGeneration` (the only place `ownerId` is set). An entry counts
only while the slot still holds that id, so collected or reused slots need no
bookkeeping; pruning only bounds list length. Model images are heap buffers held
by Pose/Work/Keys generations only.

Test builds define `LIFETIME_VALIDATE_INDEXES`, which checks both indexes against a
full scan at every lifetime entry point. Any transition that bypasses them fails
there instead of silently dropping an object from a snapshot.

### Future-Shot Withdrawal

`RollbackUnit_RetireFutureShots` now runs immediately before effect restoration.
Shots born after the target manifest's birth watermark are withdrawn from native
execution without kill callbacks: sUnit groups/primary membership, sShot list and
ID occupancy, and pending `{unit, category}` queue entries. The saved ID cursor
and four cSPCData shot-spawn flag bytes are restored. Occupancy is released per
exact abandoned allocation, not overwritten with a historical occupancy table.
Already-quarantined future shots are not logically retired twice.

The collision boundary uses native transient-record recycling (`1405B7F20`),
passive-group/BVH clearing (`1405B5B60`), and pooled-group reset (`1405A45C0`) for
every allocated category/pass, including unused passes. Continuous world
registrations/configuration remain intact; a future root in a live or pending
continuous registration fails preflight before the transient reset. Native
postMove rebuilds the transient registrations before collision testing.

Base constraint construction/destruction hooks (`1401091C0/140109240`) identify
embedded constraints without a shot-class whitelist. Future-shot withdrawal
unlinks those constraints non-destructively and removes their execution membership.
Physical disposal remains deferred behind snapshot leases and known effect,
common-shot and character shot-reference obligations. Collection repeats the
collision fence; it does not assume one elapsed tick cleared unused passes.

The native ASan/UBSan regression verifies one replayed shot, native ID reuse,
pending-record preservation, unused-pass cleanup, constraint detachment, no kill,
and no duplicate ID release at physical GC. Continuous registration rejection is
tested separately. This fixes the demonstrated abandoned-future duplicate-spawn
path, **not** saved-shot body/dependency resurrection. `StageShotGenerationStorage`
remains unwired; full shot rewind and unsupported effect-graph closure are still
unfinished. There is no claim of live gameplay validation for this patch.

### Fatal Invariant Diagnostics

Lifetime invariant failures now produce an immediate `lifetime_failure.log` beside
the ASI before calling abort. The record includes source file/line/function, the
failed requirement or a specific reason, process/thread/tick, current
frame/resimulation state, target snapshot frame, operation phase, object/birth ID
and native caller. Ownership checks also include existing local values such as
field offset, expected/actual pointer, generation ID, lease mask or hook status.

Plain guards use `LIFE_REQUIRE(condition)`, which evaluates the condition once and
reports `requirement failed: <condition>` from the stringified source, so the
message cannot drift from the check. `LIFE_FAIL(reason, LIFE_VALUE(...)...)` is
kept where a human-readable reason or expected/actual dump adds information. Both
use the same sink. The conversion found no pasted predicate that had drifted.

The independent file uses append/write-through and `FlushFileBuffers` before
termination. Normal HookLog output is attempted with its nonblocking lock only.
The failure path does not acquire the recorder lock, query object DTIs, inspect
arbitrary pointees, resume simulation or run rollback teardown. The SIGABRT bridge
then attempts the full crash report separately. Startup advertises
`lifetime_failure_log=1`; this instrumentation does not imply live shot rewind.

The regression checks failure output while the recorder lock is already held,
including a real expected/actual ownership mismatch. Windows I/O mocks verify
partial-write handling and that independent flush/close precedes normal logging,
even when that logger is busy. Source checks prohibit bare aborts outside the
single reporting sink. Fifteen focused tests, `make -j8` and whitespace checks
passed for this diagnostic change.

### Saved-Shot Restore Stages

`rollback_unit_shot_cpu.inl` now supplies the common typed CPU-field stage. Its native
mock regression restores timer `11` to the saved `3`, then replays once to `4`,
together with pose/position/life/attack clock/phase. It preserves live materials,
sound references, topology and unreviewed concrete fields. The stage returns
`ReviewedSubset` and remains unwired pending reference/lifecycle publication;
see `shot_cpu_restore.md` for exact ranges, native evidence and exclusions.

The common kernel also directly restores audited borrowed unit references,
callback slots, complete target/count/time history payloads and command bindings
and cursors. Capture leases the referenced unit identities using the existing
dependency mechanism; staging rejects expired/reused identities before writing.
There is no pointer remapping, snapshot addRef or liveness-based omission.
See `shot_reference_restore.md` for assignment/destruction evidence and the
distinction between retaining callback storage and restoring its simulation state.

Motion staging now restores retained sampler bindings/cursors, sequence history
and key descriptors alongside the already-captured external work/key bytes.
`StageShotExecutionState` supplies tested ordered sUnit membership and lifecycle
publication, including resurrection and re-quarantine. Shot-manager staging owns
historical ID/team restoration as well as list/occupancy state. These stages are
still unpublished pending concrete suffix, constraint and dependent-state closure.
The verified conditional render-worker wait is active at restore entry; see
`shot_execution_restore.md` for native call ordering and what that wait does not
guarantee. Startup explicitly reports `saved_shot_restore=0`.

Common constraint initialization/destruction provenance now covers the observed
base-ctor/dtor bypasses, and snapshots capture complete discovered neighboring
chains. The tested graph stage restores exact reciprocal links, joint heads and
saved dependency caches rather than relinking or evaluating. Unknown interfaces
are reported without dereferencing them and invalidate the entire graph restore.
Common constraint execution state now joins the same scheduler publication loop,
including logical withdrawal of proven future inline children. Foreign-model pose
retention and derived constraint payloads remain unfinished. These stages still
require enclosing-transaction integration. See `shot_constraint_restore.md`.

Snapshot capture now also retains the complete shot ID table, manager controls,
ordered identities in all ten global shot lists, and pending `{unit, category}`
records. Capture holds the native unit fence, shot-manager critical section and
global shot-list locks in that order, before the recorder lock.

`StageShotManagerState` is an isolated registration stage, **not called by frame
restore**. It validates snapshot leases and restored body IDs/teams, rejects
duplicate ID claims or surviving foreign shot-list nodes, then installs the saved
ordered lists, occupancy/cursor, controls and spawn flags without native spawn or
kill callbacks. Saved pending shots are restored with the entire category word;
historical foreign queue addresses are never revived. Only current foreign queue
records are retained, and queue overflow fails before commit. The stage does not
restore bodies, constraints or scheduler membership, or mark retained shots live.

### Saved-Shot Restore Transaction

2026-11-10. `RollbackUnit_RestoreShots` (`rollback_unit_shot_restore.inl`) is called by
`Rollback_RestoreSnapshot` after future-shot withdrawal and effect restore, and
startup reports `saved_shot_restore=1`. Production saves always capture the shot payload. A
target frame that held no shots returns before any work.

For each shot in the target snapshot it runs, in order: the storage stage, the
common CPU kernel, a raw copy of the class-specific tail (`2140..extent`), and the
joint-dependency byte `123D`. It then copies every saved constraint's common
fields, clears the links of inline constraints initialized after the snapshot,
and publishes the manager and execution stages. Any failed requirement is a fatal
lifetime failure; there is no skip path.

The tail copy rests on a census of all 306 `uShot` classes in the unpacked
executable (destructors and methods disassembled, shared helpers decompiled on
Ghidra port 8192):

- 146 classes have no tail. In the rest, destructors release only inline TSR
  constraints (`140447F50`, 54 classes), resource references (`14050D5A0`, 9
  classes) and sound cRefs (`1402A8C60`/`1402AA590`, 10 classes).
- Shot methods allocate only effects (`140139E00`) and effect models
  (`14013B4E0`), both units. No tail owns a raw allocation.
- The tail is therefore data, unit pointers and inline constraints. The copy
  leaves live the resource and sound fields listed in `ShotTailLiveFields`, and
  the unit header (`10..28`, `30..38`) of each saved inline constraint, which the
  execution stage owns. Capture leases every registered unit a tail word names.

The prepared constraint-graph stage is not used. Constraint links are bytes among
retained objects, and the saved nodes, the shots' joints (pose spans) and
character joints (`uCharacter.jointStates`/`jointIndices` regions) all return to
the same frame. A chain with a member that is not captured is left inconsistent.

Collision layout changes mid-life only in Kikosho, Body Beam, Super Burning Skull
and Elemental Summon Shot; `releaseAtkInfo` (`1401C6870`) is otherwise reached
only from spawn and the deferred destructor. Model rebinding outside setup occurs
in about 25 classes. The generation layer covers both.

Not covered: effect models are not registry units unless adopted, so a tail
pointer to one is not retained; unsupported effect graphs are retained but not
rewound. The regression exercises rewind, replay, resurrection of a shot that
died after the snapshot, a later inline constraint and pre-write rejection. It is
a native-mock test, not live-game validation.

### Shot-Driven Models

2026-11-10. A shot can drive a separate `uGameEffectModel` unit (DTI
`0x140D46ED0`): Frank West's cart is `uShotVulcanWeaponCart` plus a cart model at
shot `+1F78` / `+2288`. The model is a full actor in scheduler lane 13. Its
animation gates the shot (`updateState @ 0x14017A440` leaves phase 0 only while the
model pointer is non-null) and `uGameEffectModel::moveAfter @ 0x14013C510` retires
it when that animation ends. A lease kept it allocated but not rewound, so every
replay advanced it again; it ended early, `uShotVulcanWeaponCart::move` cleared the
dead pointer, and the cart stayed in phase 0 on the peer that had rolled back.

`rollback_unit_shot_models.inl` rewinds these models inside the effect transaction:

- Capture: every live model named by a shot's typed references or tail. Body
  image, joint / depth / interpolation / motion-work / keyframe buffers, control
  words, delta time and scheduler groups.
- Restore: the actor prefix through the `ActorShot` column of the shared model-prefix
  table in `rollback_character_restore.h` (the ranges the shot kernel uses below `+1450`), buffer contents, and the lifecycle
  words `+1CC0..+1D00`. Scheduler membership and revival of a model that retired
  since the frame follow the attack-space path.
- A model whose buffer pointers or counts changed since the snapshot keeps its
  animation live; only lifecycle, control and membership rewind
  (`model_restored c=0`).
- A restored unit link is kept only if the snapshot retained its target.
- `MarkAbandonedModels` runs at the start of future-shot withdrawal: a model that
  a current shot names through a typed reference and that was constructed after
  the restored frame is withdrawn with the abandoned branch. Construction order
  comes from hooks on `uGameEffectModel::ctor @ 0x14013B330` and its base
  destructor `0x14013B480`, stamped on the identity clock. Adoption order is not
  used: a shot can name a model that an effect or character owns and that
  existed all along; withdrawing that one frees a model its owner still drives.

Not covered: models named only by concrete tail words that have never been in a
snapshot (the cart's twelve debris links at `+2290`), and
`uGameEffectModel` bytes `+1450..+1CC0`, which stay live.

### Live Presentation Policy

Shot storage staging leaves material sources/tables, material objects and their
state/playback/access buffers, per-part draw modes, and the seven material-vjob
arrays live. It neither follows these pointers during capture nor reinstalls them
from a saved body. The raw shot-body diagnostic still contains their opaque root
values; it is not permission to restore presentation ownership. There are no deep
material/vjob generations, draw-resource/string hooks, or speculative render/cache
preparation for this removed restore path. The existing character material repair
and controller restore behavior is unchanged. Startup reports
`shot_presentation_live=1`.

Native UV/color/pattern and material animation paths have temporal state, but the
audit established presentation consumers, not a demonstrated shot-gameplay
dependency requiring deep material rewind. Native mutability alone does not
justify snapshotting the graph. A future controller-state adapter needs concrete
consumer evidence and should preserve live ownership, as the character path does.

The sixteen motion-list and four actor command-resource occurrences remain
simulation ownership domains. Their live interpreters can borrow old bindings
after owned-slot replacement; displaced references are retained through native
owner destruction, without snapshot addRefs. Their setter hooks and collection
policy are independent of material presentation.

ASan/UBSan regressions cover registration staging for both live and retained
shots, reflection/list ordering, pending-record preservation, expired leases,
duplicate ID claims, queue overflow, and command/motion resource borrowers.
They explicitly verify that registration staging alone does **not** rewind a
shot timer or revive its execution state. Full saved-shot rollback remains
disabled pending coherent shot CPU/dependency and reciprocal constraint closure,
not deep material rewind. A poisoned-pointer regression verifies that shot capture
does not inspect material/vjob pointees and storage staging preserves the live
presentation fields byte-for-byte.

ASan/UBSan tests exercise intact joints and backpointers, animation/material and
concrete-owned allocations, unchanged native resource ownership, multiple slot
leases, trace exhaustion, reused IDs/list slots before GC, direct-delete
scheduler detachment, shared deleting targets, nested unrelated destruction,
immediate unleased deletion, and reset draining. The summary tool accounts for
both effect and shot base teardowns and reports held/collected totals by kind.

Validation: `make -j8`, the 36-test suite, and `git diff --check` pass. Game run
`20261108T083942.434874Z_shot_model_terminal_retention` exited with
`game_exit_code=-31` (SIGSYS) before either peer completed, with no captured
profiling windows. This is not in-game lifetime or performance validation.

#### Effect State And Membership

Active and retiring effects now use the same ordinary-generator allocation
descriptor. Concrete classes are discovered through native DTI ancestry under
`cParticleGenerator` (`0x140E1E120`), excluding the separate child-unit-generator
ownership family (`0x140E1E168`). Object extent is decoded from DTI `+30`; pool
stride comes from generator `+116`. The fixed billboard vtable/size/stride test is
gone. This includes ordinary subclasses with a `0x250` object extent.

Numeric movement/life-mode filters and per-character dependency admission were
removed. The captured root and its retained allocations are copied directly.
Typed borrowed unit targets are now leased as described above. Shot-state rewind,
non-unit borrowed allocations, GPU contexts, aggregate child units, and full manager/observer restoration
remain incomplete. The ownership descriptor still rejects external branches it
cannot yet retain. This is not complete-world rollback.

Primary lists and group arrays are captured in native order. Restoration rebuilds
the retained effect cohort in that order, including category changes and
resurrection. It does not dereference saved foreign neighbours: only foreign
entries still present in the live lists survive reconciliation. Newly registered
foreign entries follow the saved cohort in their current relative order.
Captured group masks and ordering are restored into the native `sUnit` group
arrays instead of requiring unchanged masks. Group arrays
are at manager `+C40`, stride `30`, with length/capacity/data at `+18/+1C/+28`.
The implementation preserves current storage and non-participating entries;
native `MtArray::extendBuffer` (`0x140011660`) grows capacity outside the recorder
lock if necessary. Registration callbacks are not replayed.

The saved-neighbour admission algorithm, `effectsValid`, and
`retiring_restore_skip` production paths have been removed. Snapshot storage or
ownership invariant failures terminate explicitly; they do not silently continue
with an unrestored effect batch. This does not eliminate the ownership
descriptor's unsupported graph families.

Empty owners, setup-state owners, and unregistered callback owners use the same
generation/root payload path as populated active/retiring owners. Future-only
effects in this ownership domain are removed from primary and group membership,
and quarantined without replaying kill callbacks. Other history slots can still
lease and restore them. The empty-target snapshot case also performs this
reconciliation. Non-effect managers, GPU context membership, and shot ownership
are not restored by the effect transaction.

The membership change passes the 34-test suite, including ASan/UBSan production
extraction tests for reordered lists, category relocation, missing foreign
neighbours, future-only removal, empty targets, newer-frame resurrection, and
unregistered owners. `make -j8` passes. Runtime attempts
`20261108T075119.123944Z_ordered_effect_membership` and
`20261108T075416.922977Z_ordered_effect_membership_retry` both exited with
`game_exit_code=-31` (SIGSYS), without completed peers. They are not runtime
validation or performance measurements of this change. A third attempt with
`WINEESYNC=0 WINEFSYNC=0`,
`20261108T081454.671475Z_ordered_effect_membership_no_esync`, also completed
neither peer. Ghidra port 8192 became unresponsive during the shot ownership
audit and was subsequently restored. Shot collision/model ownership adapters
remain unimplemented; restored tool access does not change that status.

Before this membership change, run
`20261108T070531.755669Z_ordinary_generator_world_restore` completed both peers
without reported crash artifacts. Host/client recorded 1,259/977 active-state
restores, including 46/61 restorations from quarantine back to active state.
All 332/313 distinct quarantined effects were physically collected, with zero
remaining holds, accounting errors, sequence gaps, or trace drops. There were
still 12/15 rejected transactions, all primary-topology reason 4. Desync artifacts
remain; these counts establish exercised restoration/lifetime paths, not full
determinism or universal generator support.

Supported snapshots lease graph generations, rather than hashing current state or
hoping its allocation addresses survive. The same `EffectState`, payload arena,
and `RollbackUnit_RestoreEffects` transaction serve linked and quarantined units.
Each snapshot slot holds a generation-reference bit. Graph release detaches its
owned allocations, retaining the original owner address; replacement creates a
different generation. Restore selects the saved generation and copies its saved
bytes, without comparing current contents or rejecting an ordinary restart.

Snapshot capture and restore do **not** increment resource reference counts. The
native object already owns a reference. The base resource setter at `0x1405F0980`
is intercepted only to transfer an existing old reference into retention when its
graph has been detached. Native acquisition of the new resource remains unchanged.
Restore transfers reference ownership back to the object without addRef/release.
Graph rebuilds that do not rebind the resource share the existing reference record;
disposing one generation cannot release the reference needed by another. The last
detached generation releases that reference once. If it is still on the native
object, its normal destructor performs the release instead.

Collection invokes original graph teardown outside the recorder lock, using a
temporary root view at the original retained owner's address. It restores the
owner's current view afterward. An owner cannot be freed while any retained graph
still needs that address. Trace budget exhaustion stops logging, not ownership:
leases remain until snapshot-slot expiry or explicit history/session close.

The fixed limits are 512 retained generation records, 128 captured effects per
snapshot, 2048 payload spans and a 1 MiB payload arena per history slot. Capacity
failure refuses a new capture; it never frees an already-leased generation.
Identity/generation violations are fatal ownership invariants, not normal content
mismatches. Whole-world membership outside the retained effect domain, shot
ownership, and unsupported generator dependencies remain incomplete.

`graph_owned`, `graph_detached`, `graph_collect`, and `graph_resource_transfer`
describe ownership transitions. `effect_payload_restored` records frame, copied
byte count, whether the object was quarantined, and completion (a/b/c/d).
The older `retiring_*` transaction event names remain, with state 2 now possible in
`retiring_state_restored.d`. `effect_capture` records active candidates, admitted
candidates, admission-reason bitmask, and payload bytes.

The production-extraction tests cover live/quarantined restoration, resource
replacement, switching between retained generations, multiple snapshot leases,
same-resource rebuilds, exact reference-release counts, and logger exhaustion.
The attempted ownership capture
`20261108T055724.880768Z_effect_generation_ownership` failed before launch with
Wine `SIGSYS` (`-31`); the runtime results below are historical retirement-path
results, not validation of these new generation transitions.

## Populated CPU Billboard Retention

This section records the earlier retirement-only experiment. The shared ownership
and payload transaction above supersedes its retirement-only and trace-stop limits.

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

Use `python3 -B tools/rollback_unit_summary.py <host-log> <client-log>` to check
hold/restore/re-retire/collect accounting, sequence gaps, and base teardown counts.
Results from truncated logs are explicitly marked incomplete.

The automated frame-limit close now requests `RollbackUnit_FinishCapture()`.
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

Startup logs `RollbackUnit: queued=1 ...` after successful creation and enable
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

`tools.test_rollback_unit` compiles production recorder/wrappers against mocked
Windows/native APIs with AddressSanitizer and UndefinedBehaviorSanitizer. It
checks passthrough, nested teardown, no post-free reads, address generations,
bounded overflow/probes, concurrent producers, capture stop, installation failure,
and disabled mode. It does not validate real MinHook patches or in-game coverage.
