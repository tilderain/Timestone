# Crash Guard Removal Audit

Research date: 2026-10-20. Source baseline: `3f7ff03` (`0.3.14`). Native
analysis: Ghidra port 8192, `umvc3.exe.unpacked.exe`, image base `0x140000000`.
This is an investigation, not a runtime fix or a claim that guard-free play has
been validated. Existing unrelated worktree edits were left untouched.

## Conclusion

Most speculative memory probes can be removed after replacing their role with
explicit snapshot schemas, allocation/object identity, and ownership-correct
retirement. They cannot safely be deleted from the current implementation:

1. Some probes determine which bytes are restored, not merely whether to crash.
2. Immediate rollback destruction bypasses native observer-detachment scheduling.
3. Address/vtable equality does not establish allocation identity or ownership.
4. Several guards misdescribe the native function or its nullable contract.
5. Genuine allocation failure remains possible even with correct rollback.

The desired release implementation should have no speculative heap probing in
ordinary gameplay. It should still respect native null/state/generation checks,
validate external inputs and supported executable versions, and handle genuine
resource failure deliberately. Crash-report readers are a separate best-effort
diagnostic boundary, not evidence that normal gameplay needs the same probes.

Static analysis identifies contracts and concrete failure mechanisms. It does
not identify the first corrupting runtime write, prove every subtype's lifetime,
or establish that every historical allocation failure came from a leak.

## Current Inventory

Paths below are under `UMVC3Hook/code/`; line numbers refer to this baseline.
Text-search counts are not active-guard counts: helpers, diagnostics, comments,
and disabled discovery paths contain many matches.

| Area | Current mechanisms | Removal prerequisite |
|---|---|---|
| `rollback_runtime.cpp:235` | Global isAlive pointer/vtable validation | No observer reads after target storage dies; retain native state predicate |
| `rollback_runtime.cpp:107` | Suppress invalid Motion-buffer free | Exactly one current owner, correct allocation generation and allocator |
| `rollback_runtime.cpp:536` | Material validation, array clearing, deferred reset | Balanced material/resource references and render-epoch ownership |
| `rollback_runtime.cpp:355` | Four exception-recovery handlers | Correct clone/scheduler initialization and bounded draw-arena usage |
| `rollback_runtime.cpp:856` | Battle UI allocation-failure branch patches | Successful bounded setup, or explicit owner-level failure handling |
| `rollback_runtime.cpp:265,284,294,315,664,772` | Effect, animation, cut-in, position-source, material-joint checks | Typed owner-specific contracts; some names are incorrect (below) |
| `rollback_runtime.cpp:734` | Deferred model-bounds pointer/vtable/state check | Queue cancellation on destruction or generation-tagged queue entries |
| `rollback_hooks.cpp:368` | Sound borrowed-model validation/detachment | Detach queued and active sound bindings before target deletion |
| `rollback_hooks.cpp:415,426,465,474` | UI child and material-resource guards | Setup/teardown scheduling and retained resource ownership |
| `rollback_effect.cpp:261,313,592` | Manager/chain sanitization and draw suppression | Preserve whole ownership tuples; native polymorphic teardown |
| `rollback_effect.cpp:187,1044` | Effect restore-parent validation | Resolve typed, current-generation relationships before factory entry |
| `rollback_shot.cpp:400,412` | Indexed attachment validation | Stable parent identity and factory dependency ordering |
| `rollback_shot.cpp:811,832` | Collision-array clearing and list repair | Native-owned collision topology and complete membership removal |
| `rollback_tick.cpp:290` | Malformed-unit detachment via DTI lookup | No freed or partially destructed objects in scheduled lists |
| `rollback_cleanup.cpp:206` | Material/indexed-character-slot cleanup | Invalidate observers at destruction rather than scan after restore |
| `concrete_snapshot.cpp:434-594,1191-1285` | Pointer-shaped word and embedded-object inference | Explicit per-type scalar, owner, reference, and derived-state schemas |
| `rollback_restore.cpp:1489-1550` | Owner-address/vtable tests and region skipping | Generation and capacity compatibility before payload restoration |
| `rollback_model_policy.cpp` | Joint/resource/binding accessibility tests | Resource-ready lifecycle and model-generation invariants |
| `rollback_memory.cpp` | VirtualQuery cache and guarded DTI/type predicates | Separate trusted live-object classification from untrusted diagnostics |

Do not remove headless gameplay/presentation separation, RNG preservation,
restart barriers, or thread synchronization merely because the same files also
contain crash guards. Those implement other behavior.

### Dormant code is not a runtime blocker

`rollback_cleanup.cpp:125` defines `RemoveMtDtiUnits` without a caller;
`Rollback_RemoveStaleUiUnits` at line 198 is a no-op. The old full `sCollision`
capture and external BVH discovery invocation is commented out at
`rollback_snapshot.cpp:868-919` and beyond. `Rollback_RestoreCollisionBody`
remains as a compatibility branch for such regions but the current builder
does not emit the full-singleton region. Removing this dead machinery is a
separate cleanup, not a fix for live corruption or a gameplay speedup.

Older notes in `resnap.md` describing an active `sAction` 0x20000 capture are
not the current source. Do not use those notes to attribute this crash.

## 1. Snapshot Schemas Come First

`PreserveLivePointers` (`concrete_snapshot.cpp:475`) preserves aligned words
when either live or saved bits look like a mapped pointer. Character tail
restoration scans at four-byte boundaries and preserves adjacent bytes around
suspected pointers (`:532`). Generated character ranges are also suppressed
when they overlap a mapped-pointer candidate (`:434`). These functions are
called by active restore paths, including shot overlay at `:1207`.

Consequences of this algorithm, independent of whether a specific crash used it:

- Scalars can resemble mapped addresses and be incorrectly retained.
- A stale, no-longer-mapped pointer can escape a pointer filter.
- Two peers can classify identical scalar bits differently due to address maps.
- Adjacent count/selector/ownership state can be restored inconsistently unless
  an explicit preserve range covers the entire native ownership tuple.

The source already has exceptions for scalar false positives (Arthur axe angle,
Item Throw tail), whole material headers, model joint headers, inline owners,
and several concrete subtypes. These are incremental schemas, not proof that
unclassified fields are safe.

Replacement: for each supported DTI, classify fields as authoritative scalar,
logical object reference, retained resource binding, native-owned allocation,
intrusive membership, or derived cache. Restore only declared gameplay fields;
resolve references by stable identity; keep complete owner tuples native. Use
the existing generated and verified ranges as inputs, but do not assume every
reflected property is an ownership-free scalar. Unknown layouts require an
explicit unsupported/debug-audit decision, not a fallback to pointer guessing.

The current external-region test compares `*ownerSlot == savedAddress`
(`rollback_restore.cpp:1498-1499`). That cannot detect freeing and reallocating
at the same address, or prove capacity/resource compatibility. Track the owner
generation and allocation shape where these can change inside a rollback window.

## 2. Native Unit Lifetime

Verified native entries:

| Entry | Contract |
|---|---|
| `0x140001B80` | isAlive means `(state & 7)` is 1 or 2; it is not pointer validation |
| `0x140016F20` | cUnit kill changes active state to 3, subject to fixed-unit behavior |
| `0x14051B4C0` | moveLine performs retirement transitions during eligible traversal |
| `0x14051BAF0` | Remove group memberships using unit mask at `+0x30` |
| `0x14051B9F0` | Unlink primary list; no group removal or object destruction |
| `0x14051A6B0` | Append/register; not an idempotent re-registration operation |
| `0x14051C0C0` | Group-array traversal dispatches virtual `+0x38` without first filtering lifecycle state |

Native order in moveLine is state 3 -> 4 and group removal at `0x14051B5F1`
and `0x14051B5F4`; on a later eligible traversal, state 4 -> 0, primary unlink,
then deleting destructor at `0x14051B609`, `0x14051B60C`, `0x14051B61C`.
These are eligible traversals, not necessarily two rendered frames.

Primary membership uses category bits in `unit+0x10`, next/previous at
`+0x18/+0x20`, and list head/tail at `sUnit+0x50/+0x58+category*0x30`.
Group arrays are separate at `sUnit+0xC50+group*0x30`.

Clearing the mask before group removal loses removal information. Unlinking
only the primary list leaves other dispatchers able to call the freed object.
Calling a deleting destructor directly does not implicitly repair these lists.

Current headless cleanup reproduces the two native transitions, which should
remain. However, rollback also synchronously deletes effects and shots
(`rollback_effect.cpp:1151,1203`; `rollback_restore.cpp:755`). It removes list
membership, but that alone does not reproduce the time available for borrowed
observers to see retirement and detach.

For synchronous reclamation, establish a quiescent graph transaction: identify
the retiring set, invalidate/detach surviving observers and queued users, unlink
both memberships, then destroy each owner exactly once in dependency order.
Do not simply run arbitrary gameplay ticks twice during restore: those ticks
have gameplay/RNG side effects. Alternatively, retain retired storage through
the required observer grace period and bound its memory demand. Neither design
has been proven complete for every native observer in this audit.

## 3. Shot Parent Identity And Collision Owners

The Storm Sword producer at `0x1401BFB30` supplies its own
`uShotShieldStormSword*` as spawnShotEx's attachment argument. The factory at
`0x140150F40` initializes the shot before adding it to sShot's list.
`uShot__initFromShotData` (`0x1401CD9A0`) initializes cArm, whose pose helper
at `0x14014FB60` immediately samples the parent.

The 2026-10-20 crash was the indexed dispatch at `0x14014FC04` through
`cArm+0x78` to a `cParticleGeneratorModel`. Joint index 2 became a model
pointer in generator draw, causing `[RDX+0x88]` at `0x1406512AF` to read 0x8A.
This is wrong object identity, not render-page exhaustion.

Native later cleanup at `0x140150600` clears cArm parents `+0x78/+0x80` when
isAlive is false. `uShot__preMove` (`0x1401CCBE0`) also clears dead observers
and callback slots. All these consumers assume target storage still exists.
They cannot repair an address already recycled into another live class.

Current parent remapping (`rollback_model_policy.cpp:1139`) already supports
Storm Sword Spread, but matches raw saved addresses. `restoredPtr` is cleared
only as each recipe is visited (`rollback_restore.cpp:901`). Clear the full
mapping before reconstruction, key parents by spawn identity and match epoch,
and publish only successfully constructed parents from the current transaction.
Spawn-sequence ordering helps a genuine parent-before-child dependency but is
not itself proof against address reuse or stale mappings.

The existing attachment guard hooks `0x14014F960`, not the initialization
helper, and only validates registered reconstructed shots. It cannot protect
the failing initialization before registration. Resolve the parent before
entering the native factory; validate the native null-parent reconstruction
case for a child whose original parent no longer exists.

`uShot__dtor` (`0x1401C9FD0`) removes the sShot entry and ID, releases its shot
resource, and destroys collision/inline owners. `releaseAtkInfo`
(`0x1401C6870`) explicitly removes and deletes each geometry and then the node.
Zeroing a malformed collision-array header (`rollback_shot.cpp:826`) can
abandon owned geometries; it is not equivalent to valid native teardown.

Remove collision/attachment guards only once fresh native ownership is never
overwritten by scalar overlays and each relationship resolves before use.

## 4. Effect Graphs And Observers

`uEffect__createGenerator` (`0x1405E27C0`) builds a heterogeneous polymorphic
chain, allocates shared manager/pool storage, initializes records and nodes,
then publishes readiness. Failure after construction begins dispatches cleanup.

`uEffect__releaseGenerator` (`0x1405E6890`) performs:

1. Walk `effect+0x218`, save each next pointer at node+0x18, and call its
   deleting destructor. This traversal does not use successful-node count +0x204.
2. Release each manager-record sub-buffer using count `uint16(+0x208)` and
   record stride 0x150 through `0x14080C120`.
3. Free shared storage at +0x220 and clear related pointers/counts/readiness.

The sub-buffer owner is record+0x130; +0x138/+0x140 are cleared with it.
Effect+0x210 is aggregate accounting, not the extent of +0x220, and release
does not reset it. It must not be used as proof of allocation size or liveness.

Generator destruction still needs its owner and resource data:
`cParticleManager__dtor` (`0x14080B4D0`) can call owner-effect virtual +0x240
for sound cleanup and destroy GPU context +0x48. Ordinary generator destructor
`0x140816670` also handles active-particle children and auxiliary storage +0x210.
Other node types have different destructor contracts. Release the graph before
releasing the outer EFL resource; use virtual destruction, not uniform frees.

Borrowed observers include effect+0x40, +0x138, +0x1F0 and particle+0x60.
`uEffect__updateParentEnable` (`0x1405EA1D0`) reads parent lifecycle state;
sync (`0x1405E8690`) additionally compares generation token +0x1F8. Its linked
child count +0x1FC is a phase-local census, not a refcount authorizing free.
Some failure paths kill the child without clearing its parent pointer.
`ParticleRecord__requestChildRetirement` (`0x1409727E0`) clears an active
child after requesting kill but leaves an already inactive pointer unchanged;
the separate validator `0x140976190` clears rejected children.

Therefore immediate free requires more than detaching shot callback slots.
Retained character effects, child effects, sound requests, stage registries,
and deferred work must not continue to reference the dying allocation.

Current sanitizers can truncate a generator chain, clear the resource pointer,
or zero manager counts (`rollback_effect.cpp:261-389`). Those actions may
prevent a bad dispatch but abandon legitimate ownership in the discarded suffix
or buffers. A readable surviving prefix still is not proven to be owned by this
effect. Remove sanitization by preserving ownership at creation/restore and
detaching observers before destruction, not by adding more vtable tests.

## 5. Materials, Motion, And Draw Arenas

### Material references

`uBaseModel__resetAllMaterial` is `0x14055FBC0`; release is `0x14055FB20`.
Model resource +0x100 and optional material resource +0x108 are referenced
objects. Cache +0x110/count +0x118 describes owned material references; up to
three entries use inline storage at +0x198, larger arrays are heap-backed.
Command-cache slots at +0x278 (16 * 0x28) are separate owned objects.

Native release balances each entry and frees the array when count > 3.
`setModel` (`0x140561CE0`) and `setMaterial` (`0x140561BC0`) maintain these
references. A material reset replaces entries; it does not reconstruct a lost
array/count header. Clearing all entries when one looks invalid
(`rollback_runtime.cpp:581`) bypasses releases for valid entries too. This is a
conditional leak mechanism, not proof it caused any particular exhaustion.

`nDraw__Resource__release` (`0x14076F760`) decrements refcount +0x0C and may
queue deletion according to retirement frame +0x08. `endProtect`
(`0x14053A570`) performs eligible deferred destruction. Readable storage or an
unchanged vtable is not proof that an owned reference remains outstanding.

Material duplicate at `0x1405DCA60` obtains the source DTI, invokes its factory,
and dereferences the new result at `0x1405DCA7F` without a null check. RAX=0
there means the factory returned null. Further nested allocations can fail too.
The MaterialNull branch in reset handles a missing override lookup, not a
general clone-allocation failure. The VEH's comment claiming a general caller
fallback is not supported by this native path and must not justify its removal
or retention without tracing the actual caller.

### Motion allocation identity

Eight inline Motion slots are constructed at `model+0x560+i*0x180`.
Each buffer owner is `motion+0x160`. Destructor `0x140526000` frees it but
does not clear the pointer afterward. Model rebinding/blend allocation uses
`jointCount*0x68` buffers; replacement paths free and null old ownership.
Separate keyframe storage has interior slices, not independent owners.

Allocator free `0x1404CB350` recovers the block header via the offset at
`userPointer-8`, updates lists/pool metadata, and coalesces neighbors through
`0x1404CB480`. Testing only header+8's owner is not proof that the allocation
is current, singly owned, or has intact neighboring blocks. Allocation and
destruction select allocators through different DTI entries; runtime allocator
compatibility must be established, not assumed from static zero-initialized data.

Removal requires generation/capacity-compatible payload restore into native
allocations, never restoration of historical allocation headers or interior
pointer ownership. A separate audit lead is that setBlendNum (`0x1405291E0`)
clamps to 16 whereas the inspected model constructor/destructor covers eight
slots. Check actual callers/counts before treating that discrepancy as a defect
reachable in the current game.

### Fixed draw arena, not ordinary heap failure

Fault `0x140640011` is in cDraw setup (`0x14063F9B0`). Its cursor/end are
+0x28/+0x30, dirty flag is +0x130 bit 8, and cached emitted block is +0x258.
The code updates dirty/cache/cursor state before its unchecked write.

`sRender::allocDrawBuffer` (`0x140536EF0`) atomically reserves bytes in a fixed
arena: success requires `oldCursor + size < capacity`. Equality fails, and a
failed reservation still advances the cursor. `sRender::move`
(`0x14053CAF0`, reset at `0x14053CB85`) resets the arena and temporarily reserves
0x20000 bytes for a later phase. `beginFrame` (`0x140537080`) toggles buffers.

To remove the VEH, establish proper frame/phase reset, worker completion, and
bounded command construction. Measure the first failed request's old cursor,
size, capacity, buffer selector, frame, phase and thread. Increasing heap size
does not address this fixed-arena failure. A missing reset, repeated submission,
valid per-frame demand, and stale cDraw cursor are distinct explanations.

## 6. Scheduler, UI, Stage And Sound Corrections

### The "MtAnim" guard is a scheduler guard

The hook installed at `rollback_runtime.cpp:1052` targets `0x14050EE60`,
which is `uScheduler__getUnitIndex`, not animation-frame update. It searches
type/name descriptors through `scheduler+0x60`, count +0x68, and returns
UINT_MAX when no match exists. The existing validator checks pointer-shaped
entries but does not establish the descriptor lifetime it subsequently follows.

Rebuild `0x14050F010` first deletes old track work, then allocates count*0x68
at +0x58 and type-2 index count*8 at +0x60. Fault sites `0x14050F0B5` and
`0x14050F169` are unchecked array writes. The resource at scheduler+0x70 must
outlive borrowed track descriptors. `deleteTrackWork` (`0x14050EC40`) kills bound units through
virtual +0x78 and frees arrays; it does not immediately destroy those units.
It only resets the main count inside its nonnull main-array branch.

Returning an apparently successful empty scheduler does not meet consumers'
required named-unit contract. Result binder `0x14030A3F0` resolves required
objects and dereferences +0x248 at `0x14030A734`; this is not established as an
optional cursor. Empty compensation, lookup failure, and incomplete creation
are separate possible causes. Make setup complete or explicitly fail/defer at
the owner before dependent code runs; do not chain downstream recovery handlers.

Native UI has real unchecked allocation-result consumers. For example,
`0x1402D2A32` is a conditional branch after life-gauge allocation; native code
later calls the frame-index setter with the selected child even on null. Correct
rollback can remove abnormal allocation pressure, but cannot promise unlimited
memory. Null resource/optional binding and factory-failure handling are legitimate
contracts, not stale-pointer probes to delete indiscriminately.

### The stage registry contains effects

`0x140121FF0..0x140122081` creates a uGameEffect, registers it on move line 13,
then passes it to `0x1401F02D0`. The array at sStage+0x2060 therefore contains
stage-associated effects on this path, not the stage body (separate +0x2050).
uGameEffect virtual +0xD8 resolves to `uEffect::doFinish` (`0x1405E4D10`).
Replacement requests finish and removes the old reference; it does not directly
destroy the effect. Dead-entry reap is `0x1401F03D0`, called by stage move.

Dropping an invalid entry in `hkReplaceActiveStage` skips that finish request.
Fix registry lifetime relative to synchronous effect retirement, rather than
trying to validate an allegedly live stage body. Other completion paths may
still retire an effect, so skipped finish alone does not prove permanent leakage.

### Negative sound selectors are valid

The guarded updater at `0x1405D6CD0` is used on voice-slot addresses. Its bound
object is +0x80, selector +0x88, positioning mode +0xC0. Request producer
`0x1405CD790` calls virtual +0xA0, queues the borrowed object and selector;
extraction `0x1405C1EA0` installs them. Resource references are retained separately
from the borrowed coordinate pointer. Set-coord command `0x19` changes the
pointer without changing selector/mode (`0x1405D3C50`).

`uModel::getWorldMatrix` (`0x140566330`) and `uEffect::getWorldMatrix`
(`0x1405E5470`) both return the object world matrix for negative selectors.
Thus the unconditional `mode == 2 && joint < 0` rejection at
`rollback_hooks.cpp:384` rejects valid native cases. IsBaseModel-only validation
also needs comparison with legitimate effect bindings before being treated as
the type contract. This does not establish every possible +0xA0 implementation
accepts every selector: the bound dynamic type and selector must agree.

Native queued-binding cleanup (`0x1405D76E0`) and active-voice cleanup inspect
state before clearing dead targets. They still require readable target storage.
Detach pending and active borrowed bindings before exceptional immediate free;
retain native null/dead-state handling. Clamping a selector cannot repair a
wrong dynamic object or a recycled allocation.

## 7. Removal Sequence And Proof Gates

1. Correct the native labels/contracts in code and diagnostics. Separate actual
   allocation failure, wrong object identity, and absent required setup objects.
   Keep changes narrow; the sound sentinel rejection is an early targeted
   candidate, not justification for removing all binding protection at once.
2. Add development-only object/allocation-generation records at creation and
   destruction boundaries. Include match/restore epoch, owner, DTI, capacity,
   allocator, and event sequence where relevant. Do not use current heap contents
   to infer whether an old object is still alive.
3. Replace pointer-guessing snapshots with verified per-type schemas. Resolve
   logical references and preserve native-owned tuples atomically. Reuse the
   existing character/weapon collision-parent identity design as a precedent.
4. Make restore a graph transaction: quiesce consumers, identify retained and
   retiring sets, detach observers, remove memberships, destroy once, clear
   reconstruction mappings, rebuild dependencies, apply gameplay payloads, then
   publish complete relationships and resume scheduled work.
5. Remove shot/effect/list sanitizers only after this transaction proves no
   dangling observer or scheduled entry exists. Preserve required native cleanup
   side effects, including character sword-slot reservations and sound stops.
6. Establish material/refcount and Motion allocation invariants. Remove stale
   cache/free guards without replacing them with destructive zeroing.
7. Measure and fix allocation pressure and render-arena lifecycle separately.
   Replace VEH continuation with correct setup success/failure at the owning
   boundary where resource failure remains a supported condition.
8. Remove global isAlive probing last. Retain its original state semantics.
   Keep memory probes isolated to crash dumps/debug exploration if desired.

Before each removal, require both no invariant violations and no guard
activations across the targeted lifecycle tests. Zero activations alone is not
proof if the scenario never exercised the path. Compare deterministic state as
well as survival; a guard can hide a desync by skipping work.

Suggested test matrix:

- Restore the same snapshot repeatedly, including parent already retired and
  forced allocator address reuse; verify shot parent identity before factory entry.
- Storm/Spiral/Raid swords, nested shots, lasers, embedded effects, character
  persistent effects, and stage-associated effects.
- Character death, assists, transformations, model replacement, super/cut-in
  entry and exit, restart during transitions, and result-screen setup.
- Looping sounds and queued coordinate rebinding across shot/effect destruction.
- Thousands of restores/restarts with per-owner allocation/reference accounting;
  compare steady-state plateaus rather than only process memory snapshots.
- Render arena usage across headless windows and first visible frame, including
  threaded mode and peak particle/filter activity.
- Controlled allocator failure at scheduler first/second arrays, material factory
  and nested buffers, and UI children. Confirm no partial setup is published as
  success and no successful allocation is orphaned.
- Two-peer deterministic replay with broad character coverage; preserve RNG,
  hitstop, collision, and parent-child gameplay behavior.

## Scope And Remaining Work

This audit deeply traced the shared ownership mechanisms behind the largest
guard families. It did not prove every character-specific resource callback,
every native matrix virtual, every UI setup consumer, or every asynchronous
reader. In particular, remaining owner audits include cut-in manager observers,
character+0x6428's mode-0 position source (consumer `0x1400E3020`), uncommon
derived collision and generator types, and resource-loading transitions.

No guard was disabled and no runtime test was performed. The concrete next
implementation should be the shot/effect restore ownership transaction and
explicit reference identity, not a global "disable guards" toggle. Ghidra's
`uShot__clearDeadAuxiliaryReferences` parameter at `0x140150600` was corrected
to the existing cArm type after verifying accesses to its two parent fields.
