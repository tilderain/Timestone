# Effect lifecycle, generators, and shot lifetime

Native-code research of `umvc3.exe.unpacked.exe`, Ghidra image base `140000000`.
Addresses below identify verified implementations; names are descriptive research
labels. Existing decompiler types/comments can be stale because this campaign
changed function names only.

## Main result

**Emission finish, particle drain, effect retirement, and clearing a shot callback
slot are separate operations.** A generator can finish emission on the same
substep that creates its one visible particle. Neither that finish nor deferred
effect kill synchronously frees the entire graph.

Typical graceful ordinary-effect path:

```text
shot setup / animation effect command
  -> construct and register uGameEffect
  -> bind EFL resource, parent/owner/watch state, optional shot callback slot
  -> construct heterogeneous generator graph and shared runtime storage
  -> scheduled generator updates emit, animate and recycle particles
  -> finish requested: stop emission, retain existing particles
  -> generator-specific continuation becomes false
  -> outer active-generator census becomes zero
  -> sleep or request deferred effect retirement
  -> shot preMove clears references that are no longer alive
  -> shot-specific cleanup condition may complete the shot
  -> sUnit eventually unlinks and destroys retired registered units
```

This is one path, not a mandatory sequence for every effect. Forced lifetime-end
mode, invalid required parent/watch, construction failure, or explicit kill can
bypass ordinary draining. A shot's finite vanish timer can also expire first.

## 1. Creation and the relevant object types

Animation level-3 commands route through
`sAnmchrParser__spawnLevel3CommandEffect` to:

- `140121120` — common effect-command branch;
- `140124630` — Capcom-character effect-command dispatcher;
- `140129BE0` — Marvel-character effect-command dispatcher.

Concrete shot setup may construct effects directly instead. Soul Fist
(`1401973A0`) and Hadouken (`1401A9220`) create ordinary `uGameEffect` objects,
assign callback slots and register effect-category units.

| Object | Role and ownership distinction |
|---|---|
| `uGameEffect` | Gameplay-aware particle effect built on `uEffect`; adds owner/request/subparent controls. |
| `uGameEffectModel` | Separate actor/model-backed effect, with model resources, animation and ten embedded constraints. Its graceful finish is animation-based. |
| `uEffectEmitter` | Unit that creates/updates a referenced effect and propagates configured attachment/timing; it is not the ordinary particle generator itself. |
| `cParticleManager` | Polymorphic `0xE0` base used by generator/node implementations. |
| `EffectParticleManagerRecord` | Descriptive name for a plain `0x150` runtime record in the effect-owned shared allocation. It is not a polymorphic manager object. |
| `cParticleGenerator` | Ordinary `0x240` base with active/free particle lists and emission state. |
| `cParticleNode` / `cParticleNodeInfinite` | Different runtime layouts and borrowed-track continuation logic. |
| `cEffectUnitGenerator` | Aggregate generator that spawns whole child effects and uses a linked-child census. |

The old name `sParticleSystem__simulateBatch @ 140550CB0` was misleading: it is
now `uChain__simulateGroup`, a separate joint-chain solver rather than this particle
generator system.

## 2. Graph construction and storage

`uEffect__createGenerator @ 1405E27C0` performs:

1. `buildGeneratorObjectList @ 1405E2860`: choose concrete generator/node objects
   from resource descriptors, or construct an aggregate effect-unit generator.
2. `allocateParticleManagerAndGeneratorBuffers @ 1405E0950`: allocate shared plain
   manager records and particle-pool slices after the object list exists.
3. `initializeParticleManagers @ 1405E54C0`: initialize records and optional buffers.
4. `initializeGenerators @ 1405E55F0`: initialize polymorphic nodes and their
   independently owned auxiliary storage.
5. Refresh disable flags and set graph-ready bit `uEffect+130 bit 24`.

If the construction pipeline fails after graph setup begins, it dispatches graph
cleanup. Missing/empty-resource early rejection does not dispatch that cleanup;
the ordinary move path can also leave a resource-less effect pending. Successful
earlier initialization steps may already have consumed RNG or updated accounting
and flags, so cleanup is not complete side-effect rollback. This cleanup is distinct
from later unit retirement and destruction. The effect's
`+210` allocation-accounting field is not the contiguous size of the block at
`+220`: it can include separate auxiliary allocations.

Ordinary particle pool entries are borrowed by the generator from the effect-owned
storage. The generator manages their intrusive active/free topology. Separately
allocated auxiliary/GPU blocks and registered child units have different lifetimes.

Even missing/empty-resource rejection follows an initial reset dispatch. Manager
initialization can consume external RNG before later pipeline failures. Successful
allocation can overwrite accounting on retry; preserved accounting after release
is not evidence that every retry monotonically accumulates it.

## 3. Scheduled updates and emission

`uEffect__moveAfter @ 1405E57A0` drives the outer timeline. On nonzero substeps it
prepares manager records, invokes generator updates, finalizes manager activity and
runs generator post-update callbacks. Zero-substep transform/render refreshes do
not necessarily rebuild the continuation census.

`uEffect__updateGeneratorsAndCountActive @ 1405E67C0` is **mutating**, not a count
query. It clears `uEffect+206`, invokes virtual `+88` on eligible graph nodes, and
counts nonzero continuation results. These calls can spawn/recycle particles and
consume RNG.

Ordinary generator `+88`, `cParticleGenerator__updateAndSpawnParticles @ 140834F30`:

1. Selects a spawn count through the multi-spawn (`140841A10`) or single-spawn
   (`1408426D0`) state machine.
2. Takes entries from the free list and invokes concrete particle initialization.
3. Invokes concrete `+B8` to update/recycle active particles.
4. Reports whether the generator still has continuing work.

The single-spawn path's phase 3 invokes `finish(false)` and returns **one spawn**.
Phase 5 subsequently returns zero spawns while that particle can remain visible.

### Fields that must not be conflated

| Object/offset | Meaning |
|---|---|
| Ordinary generator `+20` | Status flags |
| Ordinary generator `+77` | Emission phase |
| Ordinary generator `+122` | Phase countdown |
| Ordinary generator `+206` | Repeat counter |
| Ordinary generator `+112` | Transient concrete-pass work count; not final active-list length |
| `uEffect+204` | Total generator count |
| `uEffect+206` | Active-generator continuation census |
| `uEffect+1FC` | Phase-rebuilt linked-child effect census |
| `uEffect+200` | Configured child lifetime, not a live-child count |
| `uEffect+208/+20A` | Total/active plain manager-record counts |
| `sEffect+40C` | Synchronized-adhesion collision-user count; unrelated to effect completion |

Ordinary `+88` returns false when its post-pass leaves `+112 == 0` and status bits
31 and 2 are set. It does not directly test the active-list head. A pass that
recycles the final particle can still have visited an entry, affecting when the
next zero-work result becomes visible.

## 4. Different generator completion contracts

| Family | What continues after emission finishes? |
|---|---|
| Ordinary particle generators | Concrete particles, histories and optional child units continue until update/recycle and status conditions permit false. |
| Node / Infinite | Node-specific phase and borrowed-track work; their tail is not an ordinary active/free pool. |
| Aggregate effect-unit generator | Linked child `uEffect` units; zero-spawn completion tests owner `+1FC` and retiring status. |
| Custom generator | Owner-defined initialization/update/preparation/draw callbacks, within the ordinary generator's list-drain wrapper. |

Custom initialization ignores the owner callback's return value; its update
callback's false result does recycle the particle. Rejected records still count
as visited work in that concrete pass. Similarly, removal during the ordinary
motion/track stage can yield zero work immediately, whereas removal inside the
Billboard concrete loop still increments its work count. There is no universal
extra-frame drain delay after the last particle disappears.

`uEffect__rebuildLinkedChildCensus @ 1405E8690` clears an aggregate root's census.
Valid linked children compare parent liveness, generation token `+1F8` and parent
graph count before contributing to it. This is a scheduler-phase census, not a
destructor-balanced reference count.

### Child units

- Adhesion can create `uSynchroAdhesion` (`0x140` bytes).
- Model can create `uSynchroModel` (`0x5B0` bytes), a specialized base-model-derived
  unit, not the full skeletal `uModel` (`0x12C0`).
- Light, LightShaft and Force can create registered specialized light/shaft/wind units.
- A particle's `+60` slot records its child. Recycling requests virtual `+78`
  retirement for active children; this does not immediately delete them.
- The retirement helper and the separate active-child validator have different
  stale-pointer clearing behavior. Final unlink/destruction remains scheduled.

Some slot-19 generator callbacks update real child units. They must not all be
interpreted as pure render-buffer preparation.

### Resource survival and child recreation

Model independently retains rModel; LightShaft and grass-wind children also retain
their bound resources until base destruction. Adhesion's inspected record slots
instead borrow converted EFL-entry resources at entry+08/+10/+18. Graph-only release
leaves the outer EFL reference intact. Strong graph-plus-EFL clear removes that
protection; child scheduling and other references must be considered separately.

`rEffectList__releaseConvertedStorageAndResources @1405EDD10` releases entry-owned
references and packed/converted storage. `rEffectList__writePackedEffectListToStream
@1405EE1B0` serializes a temporary EFL header/payload; despite its old clear name, it
does not clear those live resources.

Adhesion free-stack reset preserves record pointer bytes; record acquisition clears
selected borrowed slots without release. Null child creation recycles the particle,
whereas failed buffer allocation on an existing child keeps the particle and child.
Retries depend on the rebuild byte at particle+10A. Stale-child replacement alone
does not re-arm that byte; normal activation clears particle+60 and normal particle
initialization re-arms rebuilding. Conditional empty replacement is a native branch
possibility, not an observed runtime defect.

## 5. Finish, hold, restart, sleep, kill and destruction

| Operation | Entry | Effect |
|---|---|---|
| Request finish | `1405E4D10` | Calls generator virtual `+50(1)`. Suppresses the immediate repeat path and enters finishing state; does not drain pools or prohibit later explicit restart. |
| Request hold release | `1405E4D60` | Clears sustained-hold mode bit28 and sets transient status bit30 on graph nodes; the verified track consumer can advance toward fall/end. Universal emission suppression is not established. |
| Restart graph | `1405E4DB0` | Reuses allocated nodes/records, resets their runtime state and active counts. |
| Clear graph/resource | `1405E4CD0` | Releases the graph, then retained effect-list resource. |
| Sleep or kill | `1405EF2B0` | Completion disposition: sleep preserves object/graph; kill requests deferred retirement. |
| Unit kill request | `140016F20` | Shared implementation changes active states 1/2 to 3 unless fixed. |
| Graph release | `1405E6890` | Deleting-destructs graph nodes, releases plain-record sub-buffers and shared storage, and clears graph readiness/counts. |

Lifetime end mode 0 requests finish; mode 1 routes to completion without waiting
for ordinary drain; mode 2 requests hold release. Dead required links and failed
graph creation can also route to completion. Visibility result3 provides another
early completion route before outer generator updates; result2 requests finish.

Visibility polarity is verified: `uEffect__isCulledByCamera @1405EF510` returns
true for rejection. After its resource/mode gates, the outer policy takes the
configured action when all selected existing cameras are culled, including when
no cameras are tested. Passing either enabled sphere or AABB frustum test prevents
that camera's volume rejection, subject to the preceding distance condition.

Automatic repeat and explicit restart are different: ordinary phase6 advances to
another emission cycle without recycling existing particles, allowing overlap.
Explicit ordinary-generator restart recycles the active pool and resets emission
state. Waking via the sleep setter merely toggles a bit and is not graph restart.
Inactive child pointers can remain in recycled records until later validation or
activation; active-only retirement helpers do not normalize every slot.

Synchronized-child draw filtering is separate from that outer completion policy.
`SynchroUnitOwnerRecord__applyChildDrawStateAndCameraMask @1408F1550` updates flags
and camera masks without retiring the child. Its caller requests retirement for
an invalid owner before invoking it. An owner in setup state1 can satisfy liveness
while failing this helper's state2 drawing requirement. A zero camera mask alone
does not cause retirement in the inspected child visibility helpers.

`cParticleGenerator__buildCameraCullRecord @140834B70` copies the owner's visibility
boundary-policy nibble into nested mode+3B. Value2 selects finish-on-cull and bypasses
nested child filtering, while owner drawing and independent outer distance gates
remain. This nibble is separate from lifetime end-mode2 (hold release). The builder
selectively sets payload/enables; callers clear enable bytes before conditional copy.
False return does not imply the old payload was erased.

Visibility processing precedes pending graph restart: result3 can postpone restart
while sleeping, whereas result2 can request finish and then restart the graph in
the same invocation. Waking, graph restart and resource replacement remain distinct.

`sUnit__moveCategory @ 14051B4C0` performs deferred stages `3 -> 4 -> 0`, removes
membership links and invokes deleting destruction. These are traversal stages,
not a guaranteed fixed number of wall-clock frames. Global/category pause and
unit-mask eligibility gate these transitions. Primary-list counts include retiring
units until unlink. Manager destruction is a separate immediate teardown path.

Collision-enabled synchronized Adhesion children keep their collision-user count
until their destructor decrements it. `sEffect__hasSynchroAdhesionCollisionCapacity
@140543C90` is only a query; reservation/increment happens separately. A particle
clearing its child pointer does not immediately return this capacity.

Emitter cleanup differs from shot callback cleanup: `1407BCA20` clears its child
reference even when that child is already inactive. At `1407BCE10`, a fixed or
inactive emitter skips its own retirement-state write but still performs child
and retained-resource cleanup.

The normal `sGameEffect` tracking array is **non-owning**: its constructor clears
the array ownership byte. Dead-entry removal compacts references. The generic
removal helper can delete entries only when that ownership byte is true; normal
registered-effect destruction belongs to `sUnit`, not unconditional tracker deletion.

Follow-up native review clarified the shared array helpers:

- `MtArray__removeAtWithOwnership @14004C9D0` conditionally deletes the selected
  element according to array ownership, then compacts the pointer array.
- `MtArray__clearEntriesAndOptionallyFreeStorage @1400112D0` clears count and
  optionally frees backing storage; it never deletes pointed-to elements.
- `MtArray__deleteElementsAndClear @140011570` explicitly deletes every non-null
  element before clearing storage; the ownership decision belongs to its caller.
- `UnitPointerArray__pruneNullOrInactiveEntries @1401F2AA0` is a shared helper,
  taking the array in its second argument, rather than an effect-manager method.

## 6. How effects are tied to shot lifetime

### Distinct links

For ordinary `uGameEffect`:

| Link | Offset | Role |
|---|---:|---|
| Transform parent | `+40` | Placement and parent-validity policy |
| Required watch object | `+138` | Outer lifecycle validity gate |
| Linked aggregate parent | `+1F0` | Child-effect census/generation relationship |
| Subparent | `+278` | Alternative attachment transform |
| Owner | `+280` | Gameplay action/motion/status watching |
| Request unit | `+288` | Initiator identity and related controls |

These pointers are not aliases and their loss does not have one universal outcome.
Owner-watch controls can detach, request finish or kill. A required outer link can
invalidate the effect directly. Motion-event watches can trigger when a selected
bit disappears from owner Motion[0].

### Shot callback slots

Shots store thirteen callback pointers, indexed 1..13. Relevant entries:

- `1401C9AA0` — raw callback-slot assignment;
- `1401C9910` — graceful ordinary-effect finish request;
- `1401C9890` — model-effect finish;
- `1401C9A20` — kill/finalize selected callbacks and clear live slots;
- `1401C9AB0` — pre-move pruning of pointers whose objects are no longer alive;
- `1401C99F0` — **pointer emptiness** test, not a generator/particle-count test.

The kill/clear helper at `1401C9A20` only clears selected pointers whose objects
are still in active states1/2. Already inactive non-null slots are left for the
separate pruning helper. A null slot does not establish completed destruction of
the effect allocation or generator graph.

The checked Doom Missile, Soul Fist and Hadouken tables use the shared cleanup
callback at `1401A0050`. That callback first invokes `uShot__tickVanishState`, which
can mark completion when a positive finite vanish timer expires, and then marks
completion if the callback slots are empty. Its condition is therefore:

```text
finite vanish timer expires OR all callback slots are null
```

When the timer does not expire first, effects can keep this cleanup path alive
through their own continuation and retirement. After an effect enters a non-alive
state, slot pruning can occur before its allocation is actually freed. Other shot
classes use different gates; keeping an allocation alive does not keep its callback
slot alive after retirement begins.

Terminal `uShot__killAndCleanup @ 1401CCBB0` kills/clears remaining callbacks before
requesting the shot's own deferred unit retirement.

Its middle helper `uShot__stopSoundCookieSlot @1401CE040` stops an indexed sound
cookie, not attack commands. The ordinary terminal path supplies index0; GMPulse's
finish handler supplies index3. The helper's four-iteration tail checks the same
selected reference repeatedly.

Concrete secondary predicates do not necessarily gate finite-timer completion.
SatelliteLaserLock additionally waits for its child pointer only on its secondary
branch; DrDoomJS tests Motion endpoint only on that branch. Ouroboros and
SoulFistAbsorb can be marked terminal by the timer without executing the owner-flag
clear guarded by callback emptiness. EnergySpearSpread can still dispatch its
one-shot attack after the timer tick returns. These are local ordering findings;
asset timer values and alternative owner-flag cleanup remain unproven.

Ouroboros's concrete kill handler at `140183F80` does supply alternate owner-flag
clearing and ability-type0x20 reset. SoulFistAbsorb's setup can set its owner latch
even when effect spawning returns null. Neither observation establishes a global
flag leak or recovery guarantee for every shot.

### Model effects

`uGameEffectModel__finish @ 14013C330` can select a finish animation and finishing
step, or immediately dispatch finalization if no different finish animation is
available. Model/command completion then drives finalization. This is distinct
from particle-generator drain even when both kinds occupy shot callback slots.

The animation selection is a pending request, not immediate Motion rebinding.
Repeated finish before request commit can requeue; after the finish animation is
current, another finish call dispatches retirement. In the inspected model-effect
moveAfter, owner-watch precedes request commit, so an owner-triggered finish can
commit in that same call. Command processing and its completion query precede the
actor timer increment and model advance. Completion uses actor command time, not
Motion endpoint bit4 or blend completion.

End-type1 expiry exits moveAfter early, but end-type0 finish can request retirement
and continue processing. Command-completion retirement also falls through to later
actor/model work. The exact base `uGameEffectModel__restart` dispatch resets actor
command time, then conditionally binds/seeks command animation; the root does not
directly reset lifecycle, step, remaining lifetime or elapsed time.

Kirigakure normally requests finish on live child effects and immediately drops
their pointers. Its next step checks pointer occupancy, not particle drain. The
double-liveness disagreement branch can skip clearing; occurrence is unproven.
Kirigakure advances past setup even on spawn failure, while RagnarokCipher retries
a null child on later calls. A non-null factory return proves object construction,
not success of setup operations whose return values are ignored.

Material animation has its own update path: `nDraw__Material__advanceAnimation
@1405E0530` samples then advances four selected tracks. Actual material destruction
and runtime-binding release are separate functions. Model-resource rebinding can
reuse equal-sized joint allocations; matching descriptors can also avoid joint
reinitialization. Rebinding does not always mean a fresh skeletal allocation.

## 7. Motion event timing

`uModel__Motion__getPlaybackStateFlags` returns a mixed state word, including
selection-dirty, blend and endpoint flags. It is not a completion-only boolean.
Endpoint bit `4` behaves differently for clamped and looping playback.

Forward completion compares integer cursor-plus-epsilon against endpoint; reverse
completion requires cursor strictly below zero. Zero delta is not a blanket no-op:
event/flag/channel work can still occur. Attr `0x200000` suppresses the optional
endpoint-lookahead event scan, not the actual forward-wrap head scan. Attr10's
cursor/event hold does not suppress all blend/endpoint processing.

Current Motion event masks are at `+140..+14C`; prior masks at `+150..+15C`.
The verified event-stream scan excludes the lower edge but evaluates an entry
before checking whether its cumulative time exceeds the upper bound. It must not
be casually replaced with an assumed conventional interval scan.

Concrete shot callbacks run before that shot's model evaluation/advance in the
inspected pre-move path. Separate owner/effect category ordering still matters;
extra model advancement can change which flags a later watch observes. This is a
native ordering finding, not a confirmed diagnosis of a particular runtime defect.

Repeated collision-event evaluation is not necessarily stable without a model
update. MotionSequenceSupport combines current mask with a scan from its mutable
float-frame cache; GeometryArray switches between rescanning and current-mask
selection using an integer cache compared against a float cursor. An extra call
can change the selected bits. Fractional stationary cursors can keep rescanning
equal integer bounds. Round-six E records explicit native-derived examples;
actual assets and repeated-call scheduling remain unverified.

## Evidence and limits

See `shot_effect_research_round4_summary.md` and the A–J TSV/Markdown checkpoints
for per-function evidence, ownership reservations and audit exceptions.
The subsequent `shot_effect_lifecycle_followup_20260914.md` and matching TSV record
collection-helper naming corrections and callback-clear conditions.
`shot_effect_research_round5_summary.md` and A-J/parent ledgers record the latest
hold/repeat, command-clock, scheduling, material and synchronized-child refinements.
`shot_effect_lifecycle_visibility_followup.md` and matching TSV resolve camera-cull
polarity, embedded child visibility records and Collider slot-dispatch ownership.
`shot_effect_research_round6_summary.md` and A-J/parent ledgers add resource survival,
callback3 ownership, concrete retry policies and repeated-event evaluation evidence.

This establishes native mechanisms and selected concrete shot paths, not the
complete generator mix of every EFL asset at runtime. Five grass-wind/shared virtual
interfaces retain structural unresolved names. Some large rendering/adapter helpers
remain unclassified, and known spurious function boundaries were not repaired.
