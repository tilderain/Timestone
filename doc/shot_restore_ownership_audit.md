# Shot Restore Ownership Audit

2026-11-08, Ghidra port 8192, `umvc3.exe.unpacked.exe`, preferred base
`0x140000000`. Research covered native model replacement/collection, shot
collision replacement/registration, and nested material-controller ownership.
This is not a claim that shot restore is implemented or runtime validated.

## Implementation Status

`lifetime_shot_generations.inl` now replaces the unfinished per-allocation ledger.
Snapshots hold generation IDs and one rollback-slot mask per ownership domain:

- Model pose: joint vector, depth table, interpolation and palette reservation.
- Motion work: eight native motion-work pointers.
- Keyframes: the slot-zero slab and its non-owning aliases.
- The existing model-resource and shot-resource reference, each separately owned.
- Four attack-info cohorts, one for each native shot collision root.

There are no allocator-free detours or leaf-deleting hooks. Stable allocations
remain owned by the shot. Destructive `setModel`/`setBlendNum` branches detach
only displaced domains before invoking the original setter. Same-count model
replacement keeps joints/work attached. Base model null/equality control flow is
preserved; a scoped suppression of its old-resource release transfers the original
reference without snapshot `addRef`.

Old joints receive native vector logical teardown with flags `2` (no free),
clearing obsolete model backpointers. Their saved CPU bytes restore vtables and
backpointers when selected again. At physical collection, the typed first-joint
vtable is reinstated for the native vector-deleting dispatch. Other model-domain
collection uses the original owner address and `0x1401FF6B0` with unrelated owned
slots masked out, then restores the current live root image.

Partial attack release produces a new same-length/capacity pointer buffer with
holes for released primitives, retaining null-primitive wrappers at their original
addresses. Unchanged children can be shared by COW cohorts, but have no individual
snapshot masks. Native root `addChild` (`0x1405A4420`) and `MtArray::extendBuffer`
are intercepted for known shot roots so insertion/growth cannot free or overwrite
a leased array. Full release nulls the shot slot and retains the original root.
Native wrapper/primitive destructors and the matching array allocator perform
physical disposal once the last cohort drops the displaced storage.
Disposal is explicit at the collection fence; destruction of C++ bookkeeping
objects never calls native game code.

`StageShotGenerationStorage` validates generation identities and payload extents,
swaps retained domain ownership, and copies the saved CPU spans. Isolated native
tests exercise A/B/A swaps, replacement collection, same-count model reuse,
null-model transitions, native resource balance, partial release, null wrappers,
array growth and full root retirement. Keyframe allocation extent is tracked
separately from the mutable blend count to cover the native null-model early branch.

**This is a storage stage, not a published whole-shot/world restore.** It remains
unwired from frame restoration. Material/controller ownership, standalone geometry
replacement/removal, shot registration/IDs, external constraint graphs and collider
queues still need coherent adapters before simulation can resume after this stage.
Unsupported attack ownership changes fail their ownership invariant before copying
historical pointers; there is no silent restore skip or character whitelist.
Palette reservations remain held until physical pose collection, so logical
palette-capacity closure is not yet established.

2026-11-09 follow-up: `lifetime_shot_world.inl` now withdraws abandoned future
shots at the live frame-restore boundary, before effect restoration. It releases
their exact native list/ID registrations, filters pending queue pairs, rewinds
the ID cursor and shot-spawn flags, detaches constructed embedded constraints,
and fences transient collision references before withdrawal/collection. Native
continuous registrations are preserved and explicitly checked for unsupported
future-root membership. Known native borrowers can continue to retain withdrawn
storage after its snapshot leases expire. See `lifetime_trace.md` for the precise
coverage and `20261109_hyper_mystic_blast_desync.md` for the original reproduction.
This does not install historical saved-shot bodies, restore a saved quarantined
shot's dependency graph, or make the storage stage safe to publish on its own.

Generation-layer verification: 66 Python regression tests, including the native
ASan/UBSan generation scenarios, eight viewer tests, `make -j8` and
`git diff --check` passed. Run `20261109T065048.543418Z_coarse_shot_generations`
requested 900 frames but completed neither peer (`game_exit_code=-31`, SIGSYS),
with no crash/desync artifacts and no timing windows. It does not validate native
gameplay correctness or performance. The older capture-stage run below is retained
as historical evidence, not the latest implementation status.

The shared lifetime snapshot now captures concrete shot bodies and audited model
CPU spans: joints, depths, interpolation, up to eight motion work buffers, and
the shared slot-zero keyframe slab. DTI/size is cached after completed construction.
Historical export reads those saved bytes, not historical native allocations.
ASan/UBSan tests cover mutation after capture, rollback-slot turnover, shared
history backing, and export after the native shot and model allocation are freed.

No shot-body/model restore call was enabled. The bytes include ownership-bearing
pointers; copying them back before the following adapters exist would be unsafe.
The existing effect restore transaction and shot terminal retention remain in use.

Verification: `make -j8`, 65 Python regression tests (including native ASan/UBSan
extraction tests), eight viewer tests, and `git diff --check` passed. Run
`20261109T050744.356029Z_shot_model_historical_capture` requested 900 CPU frames
but completed neither peer: `game_exit_code=-31` (SIGSYS), no crash artifacts,
no desync artifacts, and no profiling windows. It supplies no runtime correctness
or performance evidence. Full ownership-domain coverage and coherent shot/world
restore are still unfinished, not merely awaiting that run's validation.

## Model Ownership Transitions

`uModel::setModel` at `0x140568670` frees joints, depth, motion work,
interpolation and scene palette before calling `uBaseModel::setModel`
(`0x140561CE0`) when incoming joint count differs or incoming model is null.
Intercepting only the base setter is too late.

Same-count replacement reuses those allocations and compares old/new joint
descriptors before the base call. Clearing the old model pointer at outer entry
changes native control flow; blanket model-root detachment is not correct.

The base setter releases the old model reference at call `0x140561D13`, return
address `0x140561D18`. A scoped transfer of that specific release can preserve
native equality/null handling without snapshot `addRef`. Nulling `+100` at base
entry when incoming is null incorrectly skips the native reset.

`setBlendNum` at `0x1405291E0` frees motion work at `0x140529294` and the
slot-zero key slab at `0x1405292C3`. It only changes the count when model/joint
count is zero. There are eight constructed/destructed inline motion records,
despite a setter clamp of sixteen; traversing sixteen is not supported by the ABI.

Other required mutation boundaries:

| Address | Operation |
| --- | --- |
| `0x140529730` | Motion-list resource assignment |
| `0x140561C40` | Material-source replacement |
| `0x140561BC0` | Individual material replacement |
| `0x14055FBC0` | Material rebuild |
| `0x14055B8D0` | Command-cache cleanup |
| `0x140566380` / `0x140564A70` | RCN initialization / release |
| `0x140200D10` | Actor command-resource assignment |
| `0x1401FFCA0` | Material-vjob array rebuild |

The actor owns **four**, not three, command-resource references at
`+13E8/+13F0/+13F8/+1400`. Seven vjob arrays are at `+1408..+1438`;
packed counts at `+1440` are overwritten before their old arrays are destroyed.
Their strides are `BB0,BB0,B90,B90,BB0,BB0,B80`; cookies are 16 bytes for
arrays 0/4/5, otherwise 8 bytes.

`uActorGameModel::releaseOwnedResources` at `0x1401FF6B0` is a non-deleting
ownership destructor whose base chain does not perform shot teardown. It is a
candidate collector for exclusively retired model ownership installed in a
masked root view at the **original owner address**. Every live/shared owned slot
must be masked, all eight work pointers considered, inline materials normalized
to original-root `+198`, and palette set to `FFFFFFFF` unless exclusively owned.
This collector is now implemented for the pose/work/key domains above. A complete
renderer/resource-consumer fence and the other ownership domains remain unproven.

## Nested Material Ownership

For each vjob `J`, 32 controllers begin at `J+50`, stride `28`, and 32 extended
controllers at `J+550`, stride `30`. Owned slots are:

- String handle `J+68+i*28` (allocation header with refcount, not character pointer).
- Texture resources `J+568/+570/+578+i*30`.

Inspected vjob move bodies change scalars and borrowed links, but configuration
and exposed setters replace those references. Name setter is `0x140141FE0`;
texture-resource setters are `0x14044F5D0/0x14044F760/0x14044F990`.
Retaining the outer vjob allocation does not protect a replaced nested reference.

Ordinary `nDraw::Material::update` (`0x1405E0530`) also replaces sampler and
texture references. State records are `18` bytes under material `+50`; types
2/3 own resource at record `+8`, whereas type 1 holds a tagged constant pointer.
Do not restore that array wholesale. `setAnimationData` (`0x1405DDC30`) also
replaces material `+98/+A0`; `setStateBuffer` (`0x1405DF570`) replaces its buffers.

## Collision Ownership

Four shot roots at `+2058/+2060/+2068/+2070` are `E0` bytes each. Root array
length/capacity/data are `+20/+24/+30`. Wrappers are `60` bytes; normal shot
wrappers borrow their primitive (`+10`) and hit metadata (`+48`). Sphere/capsule
sizes are `70/B0`, type discriminators at primitive `+8` are `5/6`.

`releaseAtkInfo` (`0x1401C6870`) partially destroys only entries with nonnull
primitives, leaving holes and preserving array length/capacity. A nonnull wrapper
with a null primitive survives partial release. Full release additionally destroys
the root. `setShotResource` (`0x1401CDAE0`) releases its resource at `+1F68`
before collision release, so retaining the resource only at collision entry is late.

Active partial rebuilds also occur at `0x140161B30` and `0x1401B48E0`.
Pointer-array growth at `0x140011660` frees old backing. Neither a resource-setter
hook nor root-pointer equality establishes ownership closure.

External collision queues hold roots/owners and contact records. Historical
cursor restoration with future queue/NodeList contents is invalid. Persistent
collision closure or a verified drained/rebuild boundary is required.

## Registration And Derived State

`sShot` is `3450` bytes. Relevant state: occupancy table `+38..+438`, cursor
`+438`, deferred-kill mode `+43C`, scalar controls `+440/+444`, 512 pending
entries at `+448`, pending count `+2448`, scratch `+2450..+3450`.
Normally issuable IDs are **1..255**; probe index 256 aliases the cursor.
Restore occupancy and cursor together, not just occupancy inferred from live shots.
Pending count above 512 is invalid: native enqueue overflow does not undo increment.

Ten global list headers start at `0x140D47F90`, stride `28`; sentinel at `+8`,
next/previous at header `+10/+18`. Shot node at `+1450` has next/previous/self
at `+1458/+1460/+1468`. Native insertion is newest-first. Unlink leaves stale
links. Reflection/owner change at `0x1401C5ED0` changes team/list/collision IDs.
The external per-team spawn flags written by `0x140151750` also need reconciliation.

There is no universal scalar suffix after `+2140`: sampled types contain child
effect links, inline constraints, and debris-model links at overlapping offsets.
Examples include ElementalSummonShot `+21E0`, MolecularShield `+2140`,
MysticStareBomb constraint `+2190`, and WeaponCart constraint `+2140` plus twelve
debris links at `+2290`. Raw suffix copying cannot substitute for dependency and
constraint topology restoration.
