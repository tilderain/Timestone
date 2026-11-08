# Delayed Destruction: Boundary Follow-Up

Ghidra research, 2026-11-06, port 8192, `umvc3.exe.unpacked.exe`, preferred base
`0x140000000`. Extends `delayed_destruction_implementation_audit.md`. Findings
are static evidence and proposed adapters, not runtime verification or benchmarks.
No runtime code or binary bytes were changed for this pass.

## Three Separate Lifetimes

| Lifetime | End condition |
| --- | --- |
| Gameplay participation and quota | Native logical destruction point |
| CPU draw consumers / scarce render reservation | Completion of last consuming render epoch |
| Intact object and owned CPU allocations | No snapshot, dependency or native-consumer obligations |

Simulation frame numbers are not render epochs or allocation identities. Physical
GC must not replay logical releases. The recommended ordinary restore boundary
is between completed main ticks, before the next native render begin, with all
affected producers stopped and CPU playback completed. Forced-inline rendering
provides a concrete place to record completion at ordinary main-tick return.
An early sync call or renderer `+0x38 == 0` alone is not a completion certificate.

## Borrowed Draw Payloads

Draw packets do not universally deep-copy their data:

1. Material state allocation `0x1405DF570` stores constants in material-owned
   storage and relocates payload pointers into it.
2. `0x1405DFD60` installs descriptor pointer/value pairs into `cDraw`.
3. `cDraw::setupDraw`, `0x14063F9B0`, copies selected pointers into a shader packet.
4. Playback `sRender::setShaderState`, `0x14053FD00`, dereferences those pointers
   when calling D3D constant setters. Texture wrappers are dereferenced too.

Retaining an allocation prevents dangling pointers but does not make overwriting
it safe while a queued frame still reads it. Animation-constant duplication
`0x1405DCD90` is selective. Material release `0x1405DD810` directly frees state.

Other concrete dependencies:

- `sScene::setEnvelope`, `0x14077E1C0`, publishes direct current/previous CPU
  palette pointers in draw slots `+0x23C0/+0x23D0/+0x23E0` for small palettes.
- `CommandCache::drawEntry`, `0x1407C8F30`, submits tags into cache-owned per-arena
  buffers. Destructor `0x1407C7F70` frees both playback buffers.
- Geometry playback `0x14053EF80` reads native handles through VB/IB wrappers.

Particle paths are more favorable: ordinary `0x1406A42C0`, line `0x1406A3090`,
and point `0x1406A5A70` copy CPU scratch into mapped pool buffers before submitting.
Packed `0x1406A1220` uploads staging and copies context constants into the arena.
Examined packets retain wrappers/state, not the original particle pool/context
body. This is not proof for every generator's virtual setup path.

## Batched CPU Completion

Verified inline main sequence:

```text
beginFrame 140537080
gameplay and draw production
renderer move 14053CAF0
endFrame 14053A250
  unlock buffers, scene end
  sync 140541030
    playback 14053A660
    optional GPU fence (native SLI can skip it)
  Present, endProtect
particle/primitive post-frame callbacks
main tick returns
```

Sort/merge jobs finish through `sMain::executeJob` (`0x140521230`). Playback
consumes commands through D3D calls and unbinds/finalizes state before return.
Unbinding is not a GPU fence.

Record one monotonic CPU-render completion epoch per completed rendered tick and
batch retirements against it. No per-shot GPU query is needed. Current ordinary
save/restore runs near `hkMvc3MainMove` entry, before native main; that is a
plausible integration point after the previous tick, not proof for reentrant
UI/reset paths or arbitrary callbacks.

Keep native wrapper protection: `Resource::release` (`0x14076F760`) queues zero-ref
resources while their frame tag is not older than the renderer frame;
`endProtect` (`0x14053A570`) deletes eligible wrappers. Bind paths use frame plus
lead (1 inline, 2 threaded). Native VB/IB/texture destruction uses COM Release.
Do not rewind renderer serials, protection tags/queues, refcounts or mapped
pointers. CPU completion permits mutation of completed CPU payloads, not
unsynchronized GPU-memory reuse. Packed-context disposal can release wrappers
through their native path rather than waiting for the GPU per context.

## Publication And Destructor Coverage

Shot DTI allocator `0x14029F7F0` creates the concrete allocation. Standard/ex
factories `0x140150E00` / `0x140150F40` bind it and publish through list insertion
`0x140150AD0` at `0x140150EF8` / `0x140151041`. Successful return is already
published even without `sUnit`. Owner change also inserts; it is not a new birth.

Exact `uGameEffect` creator `0x14013AB60` allocates `0x2C0` bytes. It covers common
outer factories and generated children: `cEffectUnitGenerator::move`
(`0x140842E20`) calls owner virtual `+0x190`, resolving to this creator for exact
`uGameEffect`, then registers and finishes additional setup. Requested-DTI
subclasses require separate admission.

Outer factories `0x140131310`, `0x140131410`, `0x140131530`, `0x140131660`,
`0x140131730`, `0x140131DA0` do not universally register units. Rocket helper
`0x1400C5970` stores an effect in character `+0x6930` before scheduler insertion.
Track allocations before escape; never snapshot midway through a birth.

| Destruction path | Entry / callsite |
| --- | --- |
| Normal sUnit most-derived dispatch | `0x14051B61C` |
| Exact uGameEffect deleting destructor | `0x140139E80` |
| Base shot deleting destructor | `0x1401CA0B0` |
| Hadouken deleting destructor | `0x1401A91A0` |
| sUnit reset main deleting call | `0x14051BCF8` |
| Reset conditional group auto-delete | `0x14051BC8F` |
| Generic array conditional deleting call | `0x14001161D` |

Hadouken body `0x1401A9110` and Vulcan base `0x1401535E0` rewrite vptrs before
the base shot destructor. A base-only hook cannot preserve concrete state.
Supported concrete deleting-destructor interception can cover bypasses, with the
normal scheduler site providing already-unlinked postconditions. The mod's two
headless deleting calls must join the same adapter. Do not normalize twice or
suppress unrelated child cleanup under a global GC flag. Group arrays normally
have auto-delete false; their deleting branch is a conditional hazard.

No top-level immediate failed-init deletion was found in the inspected ordinary
shot/exact-effect factories. Missing resource/DTI/allocation returns null. Pending
shot queue failure registers and requests kill rather than deleting immediately.

Graph construction failure does destroy allocations: `0x1405E27C0` invokes
release at `0x1405E280D`. Nodes are linked before initialization; successful count
increments afterward. Traverse actual topology, not just that count. Allocations
created and discarded wholly between snapshots, with no retained dependency,
can remain native immediate frees. Absence from `sUnit` does not prove this.

## Early Epoch Close

Draining only at `sUnit::reset` is too late. Main reset (`0x140259990`) orders:

| Callsite | Reset |
| --- | --- |
| `0x1402599B0` | GPU particle |
| `0x1402599FE` | Render |
| `0x140259A4C` | Effect |
| `0x140259A59` | Collision |
| `0x140259A66` | Resource |
| `0x140259ADC` | Shot |
| `0x140259B03` | Unit |

Shot reset discards heads/IDs/queues without destroying those shots. Proposed
idempotent epoch-close entry points: `aAction::final` (`0x140202DE0`), main reset,
manager destructor core (`0x14025A070`), main destructor (`0x140257D80`). Close
before their resets/archive releases/singleton clearing. Synchronize consumers,
stop admission, release history and drain quarantine while managers exist;
subsequent native teardown still owns live units. If entered during production,
the hook needs a proper quiescence handshake, not immediate unsafe GC.

Unsupported mutation is NOT permission to silently discard frames required by
active rollback. Cohort admission/failure must be explicit and consistent.

## Palette Reservations

Model `+0x116C` is a render reservation, not allocation identity. Allocator
`0x140779FF0` uses 64 rows, each with 28 units of 12 joints, contiguous per row:

```text
k = ceil(resourceJointCount / 12)
handle = row | (start << 12) | (k << 24)
invalid = 0xFFFFFFFF
```

An otherwise empty pool fits only 128 models with 128 joints each. Invisible
quarantine reservations also increase rows mapped/blended. Holding every handle
until snapshot GC is therefore not a robust default.

| Scene offset | Meaning |
| --- | --- |
| `+0xA8/+0xB0/+0xB8` | Three rotating textures |
| `+0xC0` | 64 upload-claim words |
| `+0x1C8` | 64 occupancy words |
| `+0x2C8/+0x2D0` | Current/previous CPU palette-address values |
| `+0x2D8` | Row pitch |

Release `0x14077D210` clears occupancy, not upload claims or the model field.
Same-epoch reuse can reject the new model's upload and draw old matrices. Set
retired handles invalid; release/reacquire at a coordinated render boundary.
Drawing does not retry allocation; same-resource `setModel` is not a solution.

Scalable proposed adapter:

1. Keep CPU joints until snapshot GC; release reservation after last consuming
   CPU-render epoch.
2. Reacquire for resurrected models before scene begin maps occupied rows, using
   original requirements or retained resource joint count.
3. Rebuild model and borrower bindings, including `uISC` target draw `0x1407514E0`.
   Invalidate affected cached commands only after their consumers finish.
4. Initialize temporal history without overwriting other live ranges.

Step 4 is still an implementation requirement: seed rotating ranges from retained
pose history for exact visuals, or deliberately reset motion history to the
restored pose as a documented visual compromise. Setting `mMotionBlurSetup=2`
is not a proved history reset. Batch uploads by texture; no per-model readback
or mid-frame global upload-claim clear. Allocation failure must not expose an
invalid handle to draw; `setEnvelope` does not provide an unskinned fallback.
Traced numeric-handle readers are rendering, not hit/gameplay identity, supporting
remapping but not proving absence of every indirect reader.

## Adhesion Quota

| Field / routine | Meaning |
| --- | --- |
| `sEffect+0x408` | Maximum, default 32 |
| `sEffect+0x40C` | Current count |
| `sEffect+0x410` | Divide maximum, default 16 |
| Child `+0x130` | Nonzero means destructor owes decrement |
| `0x140542080` / `0x140544BE0` | Increment / nonzero-guarded decrement |
| `0x140543C90` | Admission: current < maximum |

Admission failure in `0x14087D590` closes/recycles the particle, so holding the
charge changes pool evolution. Admission and increment are separately locked;
constructors themselves do not check the limit. Do not invent a stricter cap.

At suppressed native destruction, release the charge and clear child `+0x130`,
retaining eligibility in side metadata. On resurrection restore its contribution
without admission/constructor calls. GC sees flag zero. Release must not move
earlier to initial kill: native retiring states retain the charge until destruction.

Choose delta accounting OR one snapshot/rebuild accounting pass, never both.
Reconcile the object flag after copying. Restore particle `+0x60`, child state
and parent links before generator execution, preventing duplicate child creation.

Adhesion's four buffers are per-object heap storage, not the quota: records
`capacity*0xD0`, record free pointers `capacity*8`, triangles `capacity*0x70`,
triangle free pointers `capacity*8`. Default divide 16 gives 7,896 or 11,136
payload bytes by mode, plus `0x140` child. Retain these independently of quota.
Record resource links `+0x60/+0x68/+0x70` are borrowed and require owner pins.

## Async Resource Admission

Examined async queue `0x1405189A0` -> loader `0x140519040` stores `cResource*`,
not owner/callback pairs. Archive decode `0x140517F40` similarly targets resources.
Inspected loaders/finalizers populate resource payloads; owner binding separately
creates joints/material clones/collision/effect graphs. No owner-write completion
job was established for these paths; arbitrary subclasses and generic job queues
remain outside that proof.

Resource `+0x54`: ready `1`, failure `0x10`, cancelled/invalidated `0x40`.
Ready publication precedes serial-tag writes and worker-reference release at
`0x1405193B5..0x1405193DD` (assembly verified). Never restore resource headers or
assume ready means all worker header writes have stopped.

Cache lookup `0x140516D70` can return pending entries. Cache-only flag `0x80`
does not mean ready-only; synchronous-on-miss flag `1` does not force an existing
pending entry ready. Parent readiness does not establish dependency readiness.

Initial bounded admission: concrete types known, transitive resources ready and
pinned before binding, owner construction complete, topology stable or versioned.
Cache admission per generation rather than traverse dependencies every frame.
Do not wait for a resource on only one peer and silently shift a deterministic
spawn. Preload/admit consistently or explicitly reject the unsupported scenario.

Effect graph-ready bit 24 at `+0x130` certifies construction only. Shot resource
setter `0x1401CDAE0`, effect setter `0x1405E6C40`, model binding `0x140568670`,
and material replacement `0x140561C40` can still replace owned storage synchronously.
Resource pinning is not allocation versioning. Keep loader/refcount/cache/job
state live. Restore ownership links through balanced version adapters, not blind
copies; plain memcpy is reserved for audited mutable contents.

## Particle Snapshot Bandwidth

Further native inspection did not justify active-particles-only capture:

- `openParticle` (`0x14083F430`) resets selected fields, preserves index `+0x10`
  and uses masked flag writes at `+0x14`, rather than clearing the whole record.
- Close/all-close (`0x140827680` / `0x140827720`) append to the free list and
  retire children, not reset all payload.
- Initialization `0x14082B870` addresses movement/life slices by preserved index;
  position setup can fail before remaining initialization.
- Billboard initialization `0x14084C800` has conditional writes; common movement
  initialization `0x14082FFA0` also uses masked updates.

This does not prove every retained byte affects a later result, but defeats the
easy claim that reuse overwrites everything. Start with complete mutable pools
and exact active/free topology. Exclude bytes only after a per-layout
read-before-write proof and differential validation. Never normalize pool order:
it determines future slot identity and child behavior.

## Remaining Gates

The new evidence supports batched CPU-render epochs, early reset barriers,
publication tracking and finite-resource adapters. Remaining work:

- Per-type mutable/ownership descriptors and direct-destruction coverage.
- Transactional collision persistent closure and pre/post-begin semantics.
- Render cache/history rebuilding, including palette allocation-failure policy.
- Resource/admission consistency for dynamically spawned dependencies.
- Nested graph/child disposal ordering without replayed logical cleanup.
- Equality/topology tests and GC isolation from newer owners.
- Runtime timing/memory measurements; static complexity is not a benchmark.
