# AI Mass Ghidra Documentation Specification

**Status:** Active implementation specification
**Scope:** Brute-force completion of the UMVC3 Ghidra database after automated naming
**Primary consumer:** An AI coordinator dispatching multiple Ghidra analysis subagents

## 1. Core Objective

Systematically analyze, name, type, and document the remaining unresolved
functions in the current Ghidra database for
`ichneumon_sub/umvc3.exe.unpacked.exe`.

The database has already received broad automated processing. Existing work
includes vtable method naming, class ownership inference, and naming of local
functions called exclusively by one class function. The mass-documentation
controller must preserve that work and concentrate on the residual functions
that require semantic inspection.

The remaining task is a bounded parallel workload:

1. Inventory unresolved functions.
2. Divide them into non-overlapping work shards.
3. Dispatch shards to concurrent analysis subagents.
4. Require each subagent to inspect decompilation, disassembly, callers, and
   callees as needed.
5. Apply conservative names, signatures, types, and comments in Ghidra.
6. Revisit unresolved and low-confidence functions in later passes.
7. Continue until every in-scope function is either documented or explicitly
   classified as unresolved.

The objective is not to recover nonexistent original debug symbols. The
objective is a useful and internally consistent reverse-engineering database in
which relevant functions have an owner, purpose, usable prototype, and clear
uncertainty when exact semantics cannot be established.

## 2. Existing Database Assumptions

The controller must assume the following automation has already been performed:

- Vtable functions have been associated with classes and automatically named.
- Functions called locally by only one class function have generally been
  associated with that class and named with `__local_<address>` forms.
- Constructors, destructors, DTI methods, property methods, allocators, and
  many common methods have already received generated or manually verified
  names.
- Existing structures, namespaces, comments, signatures, and function names
  may contain important manual work.
- Existing documentation under `doc/` is relevant evidence and must be checked
  when a function intersects an already investigated subsystem.

Existing names are evidence, not an invitation to rerun broad renaming
automation. A subagent may improve a generated placeholder when its behavior is
understood, but it must not perform database-wide renaming or recreate work
that is already present.

Examples of residual names that normally require analysis:

```text
FUN_1402aaec0
FUN_14070e340
uCnsChain__local_14070dbf0
sHitSolver__method_1330
uMenuOptionKbd_vfn27
unknown_libname_12
```

Not every generated-looking name is unresolved. For example,
`uCnsChain__local_14070dbf0` already records useful ownership. It should be
renamed only if inspection supports a more useful semantic name.

## 3. Campaign Outcomes

| Outcome | Meaning |
|---|---|
| `documented` | Function has an evidence-supported semantic name and sufficient signature or comment. |
| `classified` | Exact semantics remain unknown, but owner and behavioral category are documented. |
| `already_complete` | Existing name, signature, and typing are sufficient; no change required. |
| `deferred_dependency` | Analysis depends on a structure, caller, callee, or subsystem not yet resolved. |
| `ambiguous` | Multiple plausible interpretations remain after the full per-function budget. |
| `library_or_generated` | Function is compiler, runtime, library, thunk, or generated support code. |
| `bad_function_boundary` | Ghidra's function definition or control flow is invalid and requires repair. |
| `analysis_failed` | Decompilation or Ghidra access failed after retry. |
| `conflict` | Proposed change conflicts with existing verified database information. |

`deferred_dependency`, `ambiguous`, `bad_function_boundary`, `analysis_failed`,
and `conflict` are never counted as fully documented. They must remain in a
follow-up queue.

## 4. Agent Roles

### 4.1 Coordinator

Exactly one coordinator owns campaign state. It must:

1. Confirm the current Ghidra instance and program.
2. Build the unresolved function inventory.
3. Create non-overlapping shards.
4. Dispatch shards to workers.
5. Track assignment leases and completed results.
6. Detect duplicate or conflicting work.
7. Schedule dependency and low-confidence passes.
8. Publish progress and final coverage reports.

The coordinator must not spend its context deeply decompiling every function.
Its primary job is work allocation, validation, and recovery.

### 4.2 Function Worker

Function workers perform the brute-force analysis. Please provide a copy of this doc or tell the worker to read the doc.

Each worker receives a
fixed list of function entry addresses and must:

1. Analyze only its assigned root functions.
2. Read neighboring callers and callees when needed for context.
3. Improve the assigned functions in Ghidra when evidence is sufficient.
4. Avoid modifying unassigned neighboring functions except for strictly local
   type propagation that does not rename them.
5. Record a result for every assigned address.
6. Continue after individual ambiguous or failed functions.

### 4.3 Structure Worker

Structure changes are high-impact and must be centralized. A structure worker
handles requests such as:

- Applying an existing known structure to an unknown parameter.
- Adding a confirmed field to a shared structure.
- Correcting a shared base type or embedded type.
- Resolving a repeated unknown offset used by several functions.
- Establishing a missing enum or function-pointer type.

Ordinary function workers may apply an existing structure to their assigned
function's parameters and locals. They must not independently redesign shared
structures.

### 4.4 Review Worker

Review workers inspect:

- Low-confidence semantic names.
- Signature changes with broad caller impact.
- Conflicting worker conclusions.
- Functions marked `ambiguous` or `deferred_dependency`.
- Names based primarily on one decompiler interpretation.
- Suspicious changes to previously meaningful names.

Review is targeted. It must not become a complete second pass over every
high-confidence trivial function.

## 5. Work Inventory

### 5.1 Inventory Source

The authoritative inventory is the current Ghidra function table. The
coordinator must query the live database rather than relying on an old exported
address list.

At campaign start, record:

```json
{
  "project": "ichneumon_sub",
  "program": "umvc3.exe.unpacked.exe",
  "image_base": "0x140000000",
  "language": "x86:LE:64:default",
  "analysis_complete": true,
  "inventory_timestamp": "<ISO-8601>",
  "total_functions": 56702
}
```

The function count is a snapshot, not a permanent constant. Creating or fixing
function boundaries may change it.

### 5.2 In-Scope Candidate Selection

A function is a candidate when one or more conditions hold:

- Name begins with `FUN_`.
- Name contains `__local_<address>` and its behavior is not documented.
- Name contains generic placeholders such as `__method_`, `__util_`, or
  `unknown_libname_`.
- Name is an inherited `vfn` placeholder whose slot is known but purpose is not.
- Function has a meaningful name but an unusable or clearly incorrect
  signature.
- Function has unresolved parameter types despite an established owning class.
- Function has no summary comment and its generated name does not communicate
  behavior.
- Existing analysis or documentation explicitly marks it unresolved.

Candidate selection must not rely on one regular expression alone. The current
database contains a mixture of raw `FUN_` names and generated names that retain
useful ownership but still require semantic work.

### 5.3 Default Exclusions

Exclude from the first semantic pass:

- External imports already named by their libraries.
- Pure jump thunks whose destination is named.
- Padding, alignment stubs, and invalid zero-byte functions.
- Functions positively identified as standard runtime or third-party library
  code and already classified as such.
- Functions whose existing semantic name and signature are clearly complete.

Excluded functions must still receive an inventory result such as
`already_complete` or `library_or_generated`. Silently dropping them makes
coverage totals unreliable.

## 6. Sharding Strategy

### 6.1 Shard by Function List

The coordinator must shard the ordered candidate function list, not divide the
raw virtual address space into equal byte intervals. Code density varies, and a
large address interval may contain fewer functions than a much smaller one.

Each shard contains explicit function entry addresses:

```json
{
  "shard_id": "pass1-0042",
  "pass": 1,
  "addresses": [
    "0x1402aaec0",
    "0x1402aaf20",
    "0x1402aaf80",
    "0x1402ab0c0"
  ],
  "range_hint": "0x1402aaec0-0x1402ab0c0",
  "status": "pending"
}
```

The address range is a human-readable hint. The explicit address list is
authoritative.

### 6.2 Shard Size

Default shard sizes:

| Function complexity | Functions per shard |
|---|---:|
| Trivial wrappers, accessors, thunks | 50-100 |
| Typical game functions | 20-40 |
| Large state machines or unknown ownership | 5-15 |
| Review and dependency pass | 5-20 |

Start conservatively. A shard should fit within one worker context while
leaving room to inspect callers, callees, strings, and disassembly.

### 6.3 Locality

Keep nearby functions together when possible. Adjacent functions often share:

- The same translation unit.
- The same class or subsystem.
- Common strings and globals.
- Shared constructors or cleanup paths.
- Similar calling conventions and data types.

Do not move a function into another worker's shard merely because one of its
callers belongs there. Workers may read across shard boundaries; only root
ownership must remain exclusive.

### 6.4 Assignment Lease

Every active shard requires a lease:

```json
{
  "shard_id": "pass1-0042",
  "worker_id": "ghidra-worker-07",
  "assigned_at": "<ISO-8601>",
  "lease_expires_at": "<ISO-8601>",
  "status": "in_progress"
}
```

If a worker fails or times out, the coordinator may requeue the shard only
after its lease expires or the worker explicitly reports failure. This prevents
two workers from modifying the same root function simultaneously.

## 7. Parallelism and Shared Database Rules

### 7.1 Allowed Parallel Work

Workers may concurrently:

- Decompile different root functions.
- Read disassembly and memory.
- Query callers, callees, variables, strings, symbols, and structures.
- Rename their exclusively assigned functions.
- Set signatures on their exclusively assigned functions.
- Add comments to their exclusively assigned functions.
- Rename local variables in their exclusively assigned functions.

### 7.2 Serialized Work

The following changes must be routed through the coordinator or structure
worker and serialized:

- Creating, deleting, resizing, or substantially changing shared structures.
- Changing common typedefs, enums, unions, or function-pointer types.
- Renaming global data used by multiple shards.
- Renaming a function not present in the worker's explicit assignment.
- Repairing function boundaries.
- Running broad analysis passes.
- Changing a base-class signature that will retype many derived methods.

### 7.3 Conflict Rule

Before mutating an assigned function, the worker must retrieve its current
name and signature. If they differ materially from the shard's recorded
starting state, another actor has changed the function.

The worker must then:

1. Re-read the function using the current database state.
2. Preserve an improved existing change when it is compatible.
3. Avoid overwriting a meaningful new name with its own weaker proposal.
4. Report `conflict` if conclusions are incompatible.

Workers must never revert another worker's or the operator's changes merely to
restore the shard's starting state.

## 8. Per-Function Analysis Protocol

### 8.1 Fast Classification

For each assigned function:

1. Retrieve current function details and variables.
2. Decompile the function.
3. Determine whether the existing name is already sufficient.
4. Identify obvious wrappers, accessors, thunks, allocators, constructors,
   destructors, and library functions.
5. If the function is trivial, name and type it immediately.
6. If behavior is nontrivial, proceed to contextual analysis.

### 8.2 Contextual Analysis

Inspect only as much context as needed:

- Direct callers to determine why the function is invoked.
- Direct callees to determine what work it delegates.
- Strings and referenced global data.
- Existing class and structure types.
- Vtable slot and sibling implementations.
- Nearby functions from the same apparent translation unit.
- Disassembly when decompiler output hides calling convention, signedness,
  switch behavior, aliasing, or indirect calls.
- Forward or backward data flow when the value source or destination is unclear.

Default call-graph depth is one. Increase it only when direct context is
insufficient. Unbounded call-graph exploration wastes worker context and
duplicates other shards.

### 8.3 Owning Class

When a function operates on a class instance, establish ownership from at least
one strong source:

- It is present in the class vtable.
- A constructor or class method passes its `this` pointer.
- The first parameter is consistently used with a known structure.
- Existing automated naming associates the function with one class.
- The function is referenced only by a confirmed class-specific path.

Apply the appropriate existing structure to the `this` parameter when known.
For example, if analysis establishes that the first parameter is `cChrInput *`,
use that type instead of leaving `undefined8` or inventing a duplicate
structure.

**Mandatory parameter typing.** When the function's field accesses are clearly
supported by an established class structure, the worker must change the first
parameter to that class's pointer type as part of the same edit, even if the
semantic rename itself is uncertain. "Clearly supported" means the function
reads or writes class-relative fields that are already named in the structure,
or the ownership sources above identify the class and the first parameter is
used as its `this` pointer. Examples of the required outcome:

```text
void sMvc3Main__setPendingGraphicsOption(sMvc3Main *this, uint index, uint value)
void cActionTime__startHitSlow(cActionTime *this, ...)
MtGeomCapsule__getSupportPoint(MtGeomCapsule *this, ...)
```

A class-qualified name without a class-typed first parameter is incomplete:
both changes belong together. If the exact field meaning is unknown but the
class is established, still type the first parameter as `<Class> *` and record
the unresolved fields. Only leave `undefined8`/`longlong` as the first
parameter when no owning class can be established after the bounded context
pass.

Ownership recovery is a prerequisite for a useful semantic rename. A worker
must not stop at an observed field offset, primitive type, or table access when
the callers, vtable, constructor, DTI metadata, or sibling methods can identify
the owning class. If the first parameter is an object pointer, inspect its direct
callers and callees, constructor or factory paths, vtable references, and the
existing class reference before choosing a neutral subsystem name. A function
called with a known singleton or class instance should be named in that class's
namespace whenever the evidence supports it.

If ownership cannot be established after the bounded context pass, retain the
raw `FUN_<address>` name and add an evidence-based comment. Do not manufacture a
generic utility name merely to remove the `FUN_` prefix.

### 8.4 Behavioral Naming

Names must describe established behavior and its owning class, not merely
restate an implementation detail. The worker must make a deliberate attempt to
recover the real class method or subsystem function name before applying any
rename. At minimum, inspect the function's direct callers, direct callees,
vtable or constructor relationships, relevant strings or data, and the owning
class reference when one exists. For a small helper, inspect every meaningful
caller before deciding that its behavior is truly generic.

The following are not semantic names and must not be applied:

```text
lookup_table_dword_140003180
get_dword_at_0x4028c_140003180
process_data_140003180
function_140003180
helper_140003180
```

These names only expose a primitive access, offset, or placeholder. If the
function is a method of `sMain`, `uUiAnmSprite`, or another established owner,
the name must use that class and the operation recovered from its callers, for
example `sMain__getInputHistoryValue` or `uUiAnmSprite__saveInputState`, only
when the surrounding evidence actually supports that meaning. If the owner is
known but the exact operation is not, use an ownership-preserving placeholder
such as `sMain__method_140003180` and a comment describing the observed behavior;
do not promote a guessed table role to a semantic name.

Preferred examples:

```text
sGSound__stopAllVoices
uCnsChain__updateChildren
cChrInput__clearCommandHistory
sShot__findById
uModel__setMotionCalcFlag
```

Weak but acceptable when exact semantics remain unclear:

```text
sGSound__processVoiceEntry_1402AAF20
uCnsChain__updateUnknownState_14070E340
cChr__lookupTableEntry_140123450
```

Avoid unsupported certainty:

```text
uCnsChain__fixRollbackPhysics
sGSound__playIronManVictoryQuote
cChr__calculatePerfectHitboxCollision
```

An address suffix is acceptable only after the owner and a useful behavioral
category have been established. It is not a substitute for ownership recovery.
The category in an address-suffixed name must be supported by more than one
implementation detail. `lookupTableEntry` is acceptable only when callers and
data establish that the function selects an entry from a lookup table; a single
DWORD load at a fixed offset is not sufficient. A primitive operation with no
recoverable owner should remain `FUN_<address>` with a useful comment.

### 8.5 Signature Recovery

Set a signature when supported by callers, callee usage, and the function body.
Check:

- Return value use at all inspected call sites.
- Register and stack parameter use.
- Signed versus unsigned comparisons.
- Pointer dereferences and established data types.
- Whether the first parameter is a class instance.
- Whether an apparent parameter is actually unused compiler residue.
- Calling convention and hidden return-object parameters.
- Whether the first parameter must be retyped to an established class pointer
  per section 8.3. If the function's field accesses match named class fields,
  the first parameter must be changed to `<Class> *` even when the semantic name
  stays broad or address-suffixed.

Do not force exact enum, const, reference, or ownership semantics without
evidence. A correct `uint32_t` is better than an invented named enum. A correct
`<Class> *` first parameter is never optional when the class is established.

### 8.6 Structure Application

If a known structure clearly matches an unknown parameter or local variable,
apply it during analysis. This is required when doing so materially improves
the decompilation and the match is supported by field accesses.

If a structure does not yet exist or requires new fields:

1. Record all observed offsets, widths, reads, and writes.
2. Submit a structure dependency request.
3. Mark the function `deferred_dependency` only if the missing type prevents a
   responsible conclusion.
4. Otherwise use conservative pointer or primitive types and continue.

### 8.7 Comments

Add a function comment when:

- The final name is intentionally broad.
- Behavior depends on a non-obvious state or flag.
- The function is ambiguous but useful facts were established.
- The function is classified as library/generated code.
- A decompiler trap or unusual calling convention should be preserved.

A useful comment is concise and evidence-based:

```text
Walks active voice entries and stops entries whose owner matches param_2.
The exact meaning of entry+0x34 remains unresolved.
```

Do not add comments that merely repeat the function name or narrate each line.

### 8.8 Class Reference Documents

Before decompiling a class-owned function, the worker must check the class
reference directory:

```text
./doc/ghidra_classes/
```

Each class uses one Markdown file named after the Ghidra class or structure:

```text
./doc/ghidra_classes/cChr.md
./doc/ghidra_classes/cChrInput.md
./doc/ghidra_classes/sAction.md
./doc/ghidra_classes/uCnsChain.md
```

If the class reference exists, read it before analyzing the function. Treat it
as accumulated reverse-engineering evidence: use its confirmed inheritance,
size, fields, methods, ownership rules, and unresolved questions as context.
The live Ghidra database and current instructions remain authoritative when a
reference is stale or conflicts with verified code.

If no reference exists, create it when the worker establishes useful class
information. Do not create an empty file merely because a class name appears in
a generated function name.

While decompiling, actively inspect accesses to unnamed class fields. Examples
include:

```text
this->field_1234
*(uint32_t *)((char *)this + 0x1234)
*(undefined8 *)(param_1 + 0x88)
entry[index].field_20
```

For each unnamed field whose type or purpose can be responsibly identified:

1. Confirm the object type and absolute class-relative offset.
2. Determine access width, type, and whether the access is a read, write, or
   both.
3. Inspect constructor initialization, destructor handling, sibling methods,
   callers, and callees as needed.
4. Propose a conservative field name and confidence level.
5. Record the discovery in the class reference document before completing the
   shard.
6. Submit a structure dependency request if applying the field to Ghidra would
   change a shared structure.

Do not ignore an identifiable field merely because the assigned root task is
function naming. Incremental class-layout recovery is a required output of the
mass-documentation campaign.

The class reference must distinguish confirmed fields from unresolved or
hypothesized fields. Use this minimum format:

```markdown
# cChrInput

**Ghidra type:** `cChrInput`
**Known size:** `0x...`
**Base class:** `...`

## Confirmed Fields

| Offset | Size | Type | Name | Access | Evidence |
|---:|---:|---|---|---|---|
| `0x1234` | `4` | `uint32_t` | `mCommandState` | read/write | Written by `cChrInput__...`; tested as command-state flags by `...`. |

## Candidate Fields

| Offset | Size | Proposed type/name | Confidence | Evidence needed |
|---:|---:|---|---|---|
| `0x1280` | `8` | `UnknownEntry *mpEntry` | low | Confirm allocation owner and destructor behavior. |

## Relevant Methods

| Address | Name | Relationship |
|---|---|---|
| `0x140123450` | `cChrInput__...` | Writes `mCommandState`. |

## Open Questions

- Determine whether `+0x1280` is owned or borrowed.
```

Field documentation requirements:

- Use offsets relative to the complete class object, not relative to a temporary
  cast or embedded subobject. If an offset is relative to an embedded object,
  state both the embedded-relative and complete-object offsets.
- Record field size and type separately from semantic name confidence. The
  width may be certain while the meaning remains unknown.
- Record the function address and operation that support the field conclusion.
- Preserve existing unresolved entries and append new evidence instead of
  deleting uncertainty.
- Move a candidate field to `Confirmed Fields` only when the evidence reaches
  `high` or `certain` confidence.
- Do not invent names solely from one comparison constant or one decompiler
  cast.
- Do not document inherited fields as newly owned derived-class fields. Link to
  the base-class reference instead.
- When a field belongs to an embedded structure, update or create the embedded
  structure's reference and identify the containing field in the parent class.

Because multiple workers may encounter the same class, edits to one class
reference must be serialized just like shared Ghidra structure changes. A
worker may collect field findings in its result record, but the coordinator
must ensure that only one worker at a time updates a given
`doc/ghidra_classes/<Class>.md` file. Later workers must read the updated
reference and merge evidence rather than replacing it.

The per-function result record must list class-reference updates:

```json
{
  "address": "0x140123450",
  "class_references_read": ["doc/ghidra_classes/cChrInput.md"],
  "field_findings": [
    {
      "class": "cChrInput",
      "offset": "0x1234",
      "size": 4,
      "type": "uint32_t",
      "proposed_name": "mCommandState",
      "confidence": "high",
      "access": "read_write"
    }
  ],
  "class_references_updated": ["doc/ghidra_classes/cChrInput.md"]
}
```

### 8.9 Rollback-Priority Class References

The mass-documentation campaign should prioritize classes according to their
impact on deterministic simulation, snapshot/restore correctness, and native
object ownership. This list is derived from the current references in
`doc/current.md`, `doc/shot.md`, `doc/scollision_rollback_snapshot.md`,
`doc/desync_snapdump.md`, `doc/main.md`, `doc/replay.md`,
`doc/sunit_categories.md`, `doc/command_matcher_states.md`, and
`doc/sbattlesetting.md`.

This is a priority list, not a claim that every byte of every listed class must
be snapshotted. Several important classes mix authoritative gameplay scalars
with live pointers, allocators, synchronization objects, resources, or derived
presentation state. Workers must document that boundary rather than labeling
the complete object as rollback-safe.

#### Priority 0: Core Fighter and Input State

These classes directly determine character actions, movement, damage, command
recognition, team state, and rollback input consumption. Analyze their unnamed
fields before unrelated engine or presentation classes.

| Class or family | Rollback relevance | Documentation focus |
|---|---|---|
| `cChr` / `uCharacter` | Main mutable fighter body. Concrete subclasses can add rollback-relevant tail state beyond the common layout. | Program/action flags, position and velocity, animation/action state, damage and hit state, timers, target references, RNG, owned external arrays, subclass tails, and pointer ownership. |
| Concrete fighter classes (`uRyu`, `uChunLi`, etc.) | Character-specific state can escape the common `cChr` layout and cause matchup-specific divergence. | Fields beyond the common DTI/base boundary, character-specific counters, helper ownership, transformation state, and character-specific command predicates. |
| `cChrInput` | Final native input-processing object used by the custom timeline at `cChrInput__processFrameInput`. | Held/trigger/release masks, direction and button histories, edge state, charge/repeat timers, facing conversion, and fields read after timeline injection. |
| `cChrCmd`, `cChrCmdSpatk`, `cChrCmdCombo`, `cChrCmdLimit`, and `cChrCmd::Line` | Mutable command matcher state determines whether the same historical input produces the same action. | Line arrays, command cursors, timers, success state, charge state, limits, resource pointers versus mutable matcher fields, and embedded-object boundaries. |
| `cChrStatus` and character status/ability subobjects | Status, health, hitstun, blockstun, action eligibility, and character-specific abilities feed combat decisions. | Mutable scalar state, status transitions, ownership of external records, and fields consumed by command restrictions or hit resolution. |
| `cChrTeam` | Active-character selection, health/gauge aggregates, tag state, member activation, and roster identity are authoritative. | Active-character pointer semantics, stable member identity, team aggregates, tag/assist state, list topology versus rewindable payload, and defeat replacement. |
| `sCharacter` | Owns or coordinates both teams and the live character roster. | Team placement, character-node ownership, traversal order, stable roster identity, and manager fields that affect per-frame dispatch. |
| `cChrNode` | Intrusive links connect teams, characters, and shots; stale topology cannot be flat-restored. | Previous/next links, contained-object identity, owner/list semantics, and which fields are topology rather than gameplay payload. |

Create or improve class references for these types whenever their functions are
encountered, even if a worker could name the assigned function without fully
resolving an accessed field.

#### Priority 0: Combat, Action, and Projectile State

| Class or family | Rollback relevance | Documentation focus |
|---|---|---|
| `sAction` | Owns the fighting-phase state machine and authoritative defeat/team-transition state. Current investigation shows important state outside the compact known ranges. | Real `0x250` object boundary, action-step fields, time/state transitions, embedded/reference state, pointers versus scalars, and any state currently hidden by broad capture spill. |
| `sShot` | Native projectile manager, intrusive lists, projectile-ID pool, pending-unit queue, and shot lifecycle authority. | Complete ID pool/cursor, pending records, kill/activity state, list globals versus object fields, lock/allocator boundaries, and spawn/destruction ordering. |
| `uShot` and every concrete `uShot*` subclass | Variable-sized polymorphic projectile objects are destroyed and reconstructed from recipes during rollback. | Common-body gameplay scalars, concrete tails, IDs, phase/lifetime, movement, owner/target/parent identity, inline objects, collision slots, callback effects, and fields that must retain fresh native ownership. |
| `cAtkCtrl` and nested target/attack records | Attack data, consumed-target identity, combo accounting, and hit eligibility directly affect repeated or missing hits. | Attack-info bindings, target tables such as `mTInfo`/`mTInfoEft`, consumed-target entries, counts, IDs, external allocations, and callback ownership. |
| `sHitSolver` and hit-solver node/record classes | Contact production and combat resolution occur between unit phases. Current policy resets/rebuilds transient solver state rather than treating the complete body as persistent state. | Persistent versus frame-transient fields, pending contact ownership, node identity, reset boundaries, hit/guard/throw/armor state, and callback ordering. |
| `sCollision` | Owns broadphase, BVHs, queues, collision managers, cloned geometry arrays, private allocation state, and external buffers. | Inline rewindable state versus allocator/synchronization ownership, external region headers, counts/capacities, category arrays, and move-matrix queues. |
| `nCollision::cCollisionNodeObject`, collision-node subclasses, and child-node graphs | Character collision attachments are deep-cloned and restored through native copy paths. | Wrapper scalar payload, expected vtable, owner/callback/user-data fields, child topology, copy/factory behavior, and concrete subclass state. |
| `MtGeometry`, `MtGeom`, and concrete hit primitives such as `cHitPrimCapsule` | Primitive geometry and cached/local/world endpoints affect hit detection and require parent-identity handling. | Geometry type, local and world shape payload, parent and joint identity, active state, cache derivation, and virtual recomputation methods. |

Concrete shot and collision subclasses are not optional follow-up work. Their
extra fields and ownership rules are a known source of projectile-specific and
contact-specific rollback failures.

#### Priority 1: Scheduling, Lifecycle, Effects, and Determinism

| Class or family | Rollback relevance | Documentation focus |
|---|---|---|
| `cUnit` | Base lifecycle and category linkage used by fighters, shots, effects, helpers, cameras, and stage objects. | Unit state, move-enable flags, category bits, intrusive links, delta time, kill/retire transitions, and fields that must remain live. |
| `sUnit` | Executes ordered move lines and two late synchronization phases. Registration order and lifecycle processing affect combat objects. | Category-list headers, active counts, line flags, pending initialization/destruction, multi-mask arrays, and phase dispatch. |
| `uCharacterPre` and `uCharacterPost` | Character coordinators run pre- and post-simulation work, including hit-solver links, throws, targets, teams, swaps, defeat, and status finalization. | Coordinator-owned state, iteration order, delayed callbacks, team/character references, and fields that survive across frames. |
| `uShotPreProcess` | Runs in the pre-simulation lane and coordinates shot processing even when recovered late callbacks are no-ops. | Registration/lifecycle state and any mutable state consumed before projectile movement. |
| `sEffect` / `sGameEffect` | Manager state includes an independent gameplay RNG and effect lifecycle queues used by reconstructed shots. | RNG, pending arrays, category cleanup, ID/allocation state, and manager ownership boundaries. |
| `uEffect`, `uGameEffect`, and concrete effect subclasses | Effects can retain shot references, drive callback lifetime, and influence attack-space or projectile behavior. | Parent/owner/request-unit links, pointer-free timing and completion state, kill/sync behavior, resource ownership, and concrete gameplay callbacks. |
| `cEffectUnitGenerator` and related generator classes | Generator-local state can determine effect completion and whether shot callbacks remain active. | Timeline/completion scalars, virtual state transitions, list and owner links, particle/resource pointers, and fresh-graph versus restored payload. |
| `uGameEffectModel` | Gameplay effect-model lifecycle participates in categories 13-16 and can dispatch or retire effects. | Owner validation, timers, model state, bounds, retirement conditions, and derived rendering state boundaries. |
| `uExAtkSpace` and concrete subclasses | Own child effects and attack-space lifecycle used by supers and other attacks. | Four child-effect owner fields, model binding, attack-space phase/timers, native release behavior, and `sAction` ownership. |
| `uChrChain` and gameplay character-helper classes | Character chains/helpers can carry mutable auxiliary state and execute late finalization. | Owner/resource rebinding, chain state, helper identity, category registration, and whether state affects gameplay or only secondary motion. |
| `sMain` | Supplies frame timing and job execution. Rollback captures a broad body but restores only selected timing and process ranges while preserving live jobs and synchronization. | Timing scalars, delta and pause fields, deterministic job-count policy, selected restore ranges, and strict separation from job arrays, handles, queues, and workers. |
| `MtXorShift` and all RNG-owning classes | Global RNG, `sEffect` RNG, and per-character RNG must reproduce consumption order and values. | Complete state width, seed/reset paths, consumers, ownership domain, and reconstruction-time save/restore behavior. |
| ID allocator classes, including `sShot` ID state and any live `sId` domains | Allocation identity affects projectiles, effects, target records, and later object creation. | Pool entries, cursors, free/reuse rules, namespace reset, consumers, and pointer-free state suitable for restore. |

#### Priority 1: Animation and Model State Used by Gameplay

These classes contain substantial presentation and resource state, but selected
fields feed shot creation, collision transforms, action progression, and
reconstruction safety.

| Class or family | Rollback relevance | Documentation focus |
|---|---|---|
| `uActorGameModel` | Character animation serial/timing fields feed animation-command processing and shot-event identity. | Gameplay-visible animation counters/serials versus render-only model fields. |
| `cActorGameModelAnm` | Embedded animation-command state tracks command indices, cursors, stop flags, animation numbers, and times. | Mutable command progress versus owner/resource pointers and cache state. |
| `uModel` and `uModel::Motion` | Motion evaluation supplies transforms and can gate reconstructed projectile movement; much of the object is resource or derived visual state. | Motion-slot scalars, joint counts/state, model flags, motion timing, resource pointers, allocated joint arrays, and which outputs can be regenerated. |
| `uBaseModel` / `uToonModel` | Base model hierarchy contains both process-local render caches and fields inherited by gameplay actors. | Confirmed class boundaries, resource/cache pointers, render stamps, and explicit proof before treating any field as gameplay state. |
| `RollbackMotionBlendRecord` and `RollbackJointWorkRecord` native equivalents | External per-character animation allocations are captured for diagnosis but are largely derived visual work buffers. | Exact element layouts, owner pointers, hierarchy fields, constraints, and evidence of any gameplay reader before promoting them to authoritative state. |

Do not classify an entire model or animation object as authoritative merely
because it resides inside `uCharacter`. Record individual fields as gameplay,
resource ownership, process-local cache, or derived output.

#### Priority 2: Match Configuration and Frame Boundaries

| Class or family | Rollback relevance | Documentation focus |
|---|---|---|
| `sReplay` | Current rollback uses `+0x54` as the native absolute-frame source; round start also seeds RNG domains. Native replay arrays are not custom rollback history. | Frame counter, round seed/state, per-team cursors, RNG initialization, and separation from dormant/file replay state. |
| `sBattleSetting` and nested `cMainChr`, `cSubChr`, and `cOnePlay` | Match construction and mode predicates affect character creation and input routing; mutable input-history rings exist in the singleton. | Six coherent slot records, battle type, input-history rings/cursors, training/native-online mode fields, and backup/restore state. |
| `sGameConfig` and embedded `cSPCData` | Holds gameplay-authoritative combat-rule tables and projectile slot-active flags. It must not be confused with the standalone `sBattleSetting`. | `cSPCData` boundary and combat tables, slot flags, mutable runtime values versus immutable configuration/resource data. |
| `rActionParam` and `rActionParamPayload` | Immutable action parameters drive damage, meter, stun decay, DHC behavior, movement limits, and team transitions. | Stable resource identity, payload size/layout, confirmed fields, and pointer identity versus immutable content. |
| `rShot` and attack/collision resource classes | Shot factories and hitbox setup consume these immutable resources during reconstruction. | DTI/class identity, resource key, hit-data layout, factory arguments, attack-info maps, and immutable versus runtime state. |
| `aAction` | Coordinates battle loading and invokes the `sAction` state machine. It is a small area object and must not be confused with `sAction`. | Area lifecycle fields, battle initialization/finalization, deterministic setup switches, and pointers to live managers. |

#### Conditional Priority: CPU, Camera, and Stage Systems

These classes matter only under specific modes or unresolved interactions. They
should follow core combat classes unless a desync or assigned call graph leads
directly to them.

| Class or family | Rollback relevance | Documentation focus |
|---|---|---|
| `uChrCpu` and `rChrCpu` | Authoritative when CPU/training-script behavior is enabled; separate from generic `sAI`. | Controller binding, script state, action selection, RNG/input production, and category-21 lifecycle. |
| `sAI`, `cAITask`, and AI service handlers | Primarily generic stage/presentation task machinery; queued tasks remain an open rollback question. | Concrete queued-task types, handler flags, task ownership, job submission, and proof of any fighter-gameplay callback. |
| `uActionSideCamera` | Persistent smoothing/anchor state may participate in camera-relative stage-edge clamps and position behavior. | Gameplay readers, anchor/smoothing state, throw/cinematic flags, and distinction from derived `sCamera` output. |
| `sCamera` | Mostly presentation-derived viewport and matrix state; currently a candidate to remain on the live timeline. | Prove any gameplay consumer before treating fields as authoritative; separate camera-unit references and render caches. |
| `uStage`, `uStageObject`, and concrete stage classes | Current analysis found mostly presentation behavior and no selectable-stage combat hazards, but generic tasks and hit-node reactions remain contextual. | Concrete callbacks, task types, collision/hit-solver submissions, and evidence of actual fighter-state writes. |

#### Normally Low Priority for Rollback Correctness

UI, lighting, post-processing, ordinary sound playback, material, and purely
render-derived classes are low priority unless they own a reference or callback
that mutates one of the higher-priority systems. Examples include most
`uBattleUi*`, `uUi*`, light, filter, shadow, and material classes.

Do not assume a class is irrelevant solely from its name or `sUnit` category.
Categories are ordered execution lanes and can contain mixed entity types. Use
concrete DTI, actual virtual target, callers, and field provenance to decide
whether a class or field affects rollback.

#### Priority Rules for Class Documents

When choosing which files to create under `doc/ghidra_classes/`, use this order:

1. `cChr`/`uCharacter`, concrete fighters, `cChrInput`, command matchers,
   `cChrTeam`, and `sCharacter`.
2. `sAction`, `sShot`, concrete `uShot` classes, `cAtkCtrl`, `sCollision`,
   collision nodes/primitives, and `sHitSolver`.
3. `cUnit`, `sUnit`, character pre/post coordinators, effects/generators,
   `uExAtkSpace`, `sMain`, RNG, and ID allocators.
4. Gameplay-visible animation/model classes and their external work records.
5. Match configuration, replay-frame, immutable combat-resource, CPU, camera,
   and stage classes as their call paths become relevant.

If a lower-priority function reveals a write into a higher-priority object,
document that field in the higher-priority class reference immediately and add
the writer to its `Relevant Methods` table.

## 9. Evidence and Confidence

### 9.1 Evidence Sources

Evidence strength, from strongest to weakest:

1. Explicit DTI, property, resource, or format string names.
2. Confirmed vtable and constructor association.
3. Multiple typed callers and callees agreeing on behavior.
4. Stable field access patterns using established structures.
5. Sibling implementations of the same virtual slot.
6. Direct disassembly and control-flow evidence.
7. One decompiled call site.
8. General resemblance to a known algorithm.

Decompiler-generated variable names and casts are not independent evidence.

### 9.2 Confidence Levels

| Confidence | Meaning | Database action |
|---|---|---|
| `certain` | Explicit name or behavior confirmed from authoritative metadata and code. | Apply semantic name and signature. |
| `high` | Multiple independent code facts agree. | Apply semantic name and signature. |
| `medium` | Broad behavior is clear; exact semantic detail is uncertain. | Apply conservative name, usually with address suffix. |
| `low` | Plausible interpretation based on limited evidence. | Do not apply a precise semantic name; classify and queue for review. |
| `unknown` | No responsible behavioral conclusion. | Preserve or improve ownership-only placeholder and record unresolved state. |

Confidence is not a numeric decoration. It controls whether a precise name may
be written into the canonical database.

### 9.3 Minimum Naming Standard

A precise semantic rename requires `high` or `certain` confidence and a
class-qualified or otherwise evidence-supported owner. `medium` confidence
permits only a conservative owner-qualified behavioral name, usually with an
address suffix. `low` confidence permits an ownership/category placeholder or
comment, but not a guessed operation. `unknown` confidence must retain the raw
name unless an existing meaningful owner-only name is already present.

The following are mandatory rejection cases, regardless of confidence claims:

- The name only describes a primitive load/store, field offset, register, or
  inferred table representation.
- The name uses `lookup`, `process`, `data`, `value`, `helper`, or `function`
  without an established object and operation.
- The name omits an owner that is recoverable from callers, vtables,
  constructors, DTI metadata, or class references.
- The name is based only on one decompiler-generated variable or one call site
  when additional direct context is available.

Existing meaningful names must not be replaced by lower-confidence names.

## 10. Function Result Record

Every assigned function must produce one result record, including functions
that required no change:

For any proposed rename, the result must separately record `owner_evidence` and
`behavior_evidence`. These fields must cite the concrete callers, callees,
vtable or constructor relationship, DTI/class reference, strings, or repeated
data access that justify the class-qualified name. A statement such as "loads a
DWORD from offset 0x4028c" is implementation evidence, not owner or behavioral
evidence for a function name.

```json
{
  "shard_id": "pass1-0042",
  "address": "0x1402aaf20",
  "starting_name": "FUN_1402aaf20",
  "final_name": "sGSound__processVoiceEntry_1402AAF20",
  "starting_signature": "undefined8 FUN_1402aaf20(undefined8 param_1)",
  "final_signature": "bool sGSound__processVoiceEntry_1402AAF20(sGSound *self, uint32_t index)",
  "outcome": "documented",
  "confidence": "medium",
  "classification": "state_update",
  "owner": "sGSound",
  "owner_evidence": [
    "three callers pass the sGSound singleton as rcx",
    "the function is reached from the sGSound voice-management path"
  ],
  "behavior_evidence": [
    "reads the active voice entry, validates its owner, and advances or retires it"
  ],
  "summary": "Validates a voice table entry and advances or retires its state.",
  "evidence": [
    "all inspected callers pass the sGSound singleton as rcx",
    "param_2 indexes the same voice table in three field accesses",
    "returns false for inactive entries and true after successful processing"
  ],
  "dependencies": [],
  "changes_applied": [
    "function_name",
    "signature",
    "function_comment"
  ]
}
```

For an unresolved function:

```json
{
  "address": "0x14070e340",
  "starting_name": "FUN_14070e340",
  "final_name": "FUN_14070e340",
  "outcome": "deferred_dependency",
  "confidence": "unknown",
  "classification": "unknown",
  "owner": null,
  "summary": "Writes offsets 0x48 and 0x58 through an unresolved object pointer.",
  "evidence": [
    "two callers pass objects allocated by FUN_14070d6e0"
  ],
  "dependencies": [
    "analyze allocator/factory FUN_14070d6e0",
    "identify object layout used by parameter 1"
  ],
  "changes_applied": []
}
```

## 11. Worker Prompt Contract

Every function worker must receive a prompt containing:

- Exact project and program identity.
- Explicit root address list.
- Statement that existing automated names must be preserved unless improved by
  evidence.
- Permission boundaries for database changes.
- Required per-function protocol.
- Confidence and naming rules.
- Required result schema.
- A hard instruction to return a result for every root address.

Reference worker prompt:

```text
Analyze the assigned functions in the current Ghidra program
ichneumon_sub/umvc3.exe.unpacked.exe.

Assigned root addresses:
<EXPLICIT ADDRESS LIST>

The database has already had vtables, class-local functions, constructors,
DTI methods, and many properties automatically named. Preserve existing work.

For each assigned root:
1. Identify the owning class and read `doc/ghidra_classes/<Class>.md` when it
   exists.
2. Retrieve current details, variables, and decompilation.
3. Inspect direct callers, callees, strings, structures, sibling methods, vtable
   references, constructors or factories, and disassembly as needed to recover
   the owner and real operation. For object-pointer parameters, this context
   pass is mandatory before naming.
4. Determine owner and behavior conservatively. Do not name from a single field
   offset, primitive load/store, or one decompiler-generated variable name.
5. Identify unnamed class fields encountered in the decompilation and record
   evidence-supported field findings in the class reference.
6. Apply a high-confidence class-qualified semantic name when the real operation
   is established. A medium-confidence address-suffixed name is allowed only
   when it still contains an evidence-supported owner and behavioral category.
   Reject generic names such as `lookup_table_*`, `get_dword_at_*`,
   `process_data_*`, or `helper_*`.
7. Set a supported signature and apply existing structs to unknown parameters
   when justified. When field accesses clearly match an established class, retype
   the first parameter to `<Class> *` (see section 8.3) and do not leave it as
   `undefined8`/`longlong`.
8. Add a concise comment for broad, ambiguous, or non-obvious behavior.
9. Do not rename functions outside the assigned root list.
10. Do not create or redesign shared structures; report structure dependencies.
11. Continue after ambiguous functions or decompiler failures.
12. Return one structured result record for every assigned address, including
    class references read and any field findings.

Do not infer exact game semantics from appearance alone. Do not overwrite an
existing meaningful name with a weaker proposal. Before writing, verify that
the current function has not materially changed since analysis began.
```

## 12. Time and Depth Budgets

### 12.1 Per-Function Budget

Default budgets:

| Function type | Initial budget |
|---|---:|
| Trivial thunk/accessor/wrapper | 1-3 minutes |
| Typical function | 5-10 minutes |
| Large or indirect function | 15-20 minutes |
| First-pass maximum | 20 minutes |

When the first-pass maximum is reached, the worker must record current facts,
dependencies, and ambiguity, then continue. One difficult function must not
consume an entire shard.

### 12.2 Context Limits

Workers should:

- Read filtered decompilation sections for very large functions.
- Prefer direct xrefs over deep call graphs.
- Avoid recursively analyzing another shard's full call tree.
- Cache repeatedly used structure and function details within the shard.
- Stop when evidence is sufficient for the permitted confidence level.

The goal is maximum responsible database coverage, not exhaustive essays for
every helper.

## 13. Campaign Passes

### 13.1 Pass 0: Inventory and Baseline

1. Confirm the expected Ghidra program and image base.
2. Confirm analysis is complete.
3. Export the current candidate inventory.
4. Record current names and signatures.
5. Assign initial classifications where obvious.
6. Generate non-overlapping shards.

No broad renaming occurs during Pass 0.

### 13.2 Pass 1: Fast Semantic Coverage

Workers process all candidates with bounded effort:

- Recover the owning class or subsystem before attempting a semantic rename.
- Resolve obvious wrappers, getters, setters, table lookups, lifecycle helpers,
  and class-owned operations.
- Improve parameter types using existing structures.
- Apply class-qualified names when the real operation is supported by callers,
  callees, vtables, constructors, DTI metadata, or class references.
- Preserve `FUN_<address>` with a comment when only an offset, primitive access,
  or guessed table role is known.
- Defer expensive dependencies.

Pass 1 optimizes for owner-qualified semantic coverage without guessing. Removing
the `FUN_` prefix is not a completion metric by itself.

### 13.3 Pass 2: Dependency Resolution

Group dependency requests by common target:

```text
unknown structure used by 14 functions
unnamed factory called by 9 functions
global table referenced by 23 functions
unresolved callback signature shared by 6 classes
```

Resolve the highest fan-out dependencies first, apply the shared type or name,
then requeue affected functions.

### 13.4 Pass 3: Low-Confidence Review

Review:

- All `low` and `unknown` results.
- `medium` names without an address suffix.
- Signatures that changed return type or more than two parameters.
- Functions whose proposed owner conflicts with generated ownership.
- Functions with indirect calls or questionable boundaries.

### 13.5 Pass 4: Consistency

Check database-wide conventions:

- Same operation uses the same verb across related classes.
- Constructors and destructors use established project naming.
- Address suffix capitalization is consistent.
- Class names match DTI and existing namespace spelling.
- Getter/setter names agree with property names.
- Duplicate names do not hide overload distinctions.
- Function signatures agree at call sites.
- Placeholder categories are used consistently.

Consistency edits must not introduce semantic certainty absent from the
underlying analysis.

### 13.6 Pass 5: Final Residual Queue

Reprocess only:

- `deferred_dependency`
- `ambiguous`
- `bad_function_boundary`
- `analysis_failed`
- `conflict`
- Remaining raw `FUN_` names not positively classified as library/generated

At the end of this pass, unresolved functions receive a durable comment and
result record describing what is known and why exact naming failed.

## 14. Structure Dependency Protocol

### 14.1 Dependency Request

A function worker submits:

```json
{
  "request_id": "struct-0172",
  "requested_by": ["0x14070e340", "0x14070faaa"],
  "candidate_type": "uCnsChain::UnknownEntry",
  "minimum_size": "0x60",
  "observed_fields": [
    {"offset": "0x08", "width": 8, "usage": "pointer read"},
    {"offset": "0x48", "width": 4, "usage": "float write"},
    {"offset": "0x58", "width": 4, "usage": "flags read/write"}
  ],
  "blocking": true,
  "notes": "Objects appear to be allocated by FUN_14070d6e0."
}
```

### 14.2 Structure Evidence

The structure worker must verify fields against multiple accesses when
possible. It must preserve unknown gaps and must not fill them with speculative
members merely to make decompilation prettier.

Required checks:

1. Allocation or embedded-object size.
2. Constructor initialization.
3. Destructor ownership behavior.
4. Read and write widths.
5. Alignment.
6. Base-class boundary.
7. Array indexing or repeated stride.
8. Compatibility with existing data types.

### 14.3 Reanalysis

After a shared type improvement:

1. Apply it to known matching functions.
2. Re-decompile affected roots.
3. Requeue only functions whose result could materially improve.
4. Do not rerun the entire program analysis unless necessary.

## 15. Naming Conventions

Follow existing database conventions rather than imposing a new C++ symbol
style during the campaign.

Preferred forms:

```text
Class__method
Class__nestedType__method
namespace__function
Class_vfn27
Class__local_140123450
Class__broadBehavior_140123450
```

Lifecycle forms should remain consistent with existing names:

```text
Class__ctor
Class__dtor
Class__vdtor
Class__operator_new
Class__operator_delete
```

Do not rename all existing `__` separators to `::`, or vice versa, as part of
this campaign.

### 15.1 Unknown Ownership

If behavior is known but ownership is not:

```text
util__binarySearchByHash_140123450
math__normalizeQuat_140123450
resource__validateHeader_140123450
```

Use a neutral subsystem prefix only when the subsystem is supported by multiple
callers, data, or an established type. Never use a neutral prefix to conceal a
missed class-owner analysis. Otherwise retain `FUN_<address>` and add a comment.

### 15.2 Name Quality Gate

Before applying a rename, the worker must answer all of these questions in its
result record:

1. What class, namespace, or subsystem owns the function, and what evidence
   establishes that ownership?
2. What does the function do for its callers, rather than what instruction or
   field access happens to be visible in the body?
3. Which independent evidence sources support the proposed verb and object?
4. Why is the proposed name more informative than the existing generated name?

Reject the rename and preserve `FUN_<address>` when the proposed name would be
unchanged in meaning by replacing the operation with `lookup`, `process`,
`data`, `value`, `helper`, `function`, or an offset or type description. A raw
placeholder plus a precise comment is better than a misleading semantic name.

For class-owned functions, the minimum acceptable fallback is:

```text
<EstablishedClass>__method_<address>
```

with a comment containing the confirmed behavior and unresolved question. This
fallback is allowed only after the worker has inspected ownership evidence; it
must not be used as a shortcut before caller, callee, and class analysis.

### 15.3 Local Functions

An existing `Class__local_<address>` name is useful and should not be replaced
with another vague placeholder. Improve it only when behavior is understood:

```text
uCnsChain__local_14070DBF0
```

may become:

```text
uCnsChain__rebuildChildMatrices
```

but must not become:

```text
uCnsChain__processData
```

unless `processData` is genuinely more informative than ownership plus address.

## 16. Function Boundary Problems

Workers must inspect disassembly when decompilation shows:

- Impossible stack behavior.
- Jumps into the middle of another function.
- Missing return paths.
- Overlapping functions.
- Large regions treated as data inside executable code.
- Calls targeting undefined addresses.
- A function containing several unrelated entry paths.

Do not repair boundaries casually in a parallel worker. Record:

```json
{
  "address": "0x140123450",
  "outcome": "bad_function_boundary",
  "observed_problem": "conditional branch enters 0x14012348c, currently defined as a separate function",
  "recommended_action": "review both entries in disassembly before merging"
}
```

Boundary repair is serialized because it may invalidate shard inventories and
neighboring analysis.

## 17. Failure Recovery

### 17.1 Decompiler Timeout

1. Retry with a larger timeout once.
2. Request only selected line ranges if partial decompilation is available.
3. Inspect disassembly and direct xrefs.
4. If still unresolved, record `analysis_failed` or `ambiguous` and continue.

### 17.2 Ghidra MCP Failure

1. Confirm the current instance and program.
2. Retry the failed read once.
3. Do not blindly repeat a mutating call whose result is unknown.
4. Retrieve the function after a failed mutation to determine whether it was
   applied.
5. Requeue the shard if database access remains unavailable.

### 17.3 Worker Timeout

Preserve completed per-function results. Requeue only unreported addresses,
not the entire shard.

### 17.4 Conflicting Rename

If two analyses propose different names:

1. Preserve the currently applied name until review.
2. Compare evidence, not worker confidence claims alone.
3. Prefer the broader name when both behaviors are possible.
4. Use an address-suffixed classification if exact semantics remain disputed.
5. Record the rejected proposal and reason.

## 18. Progress Tracking

The coordinator must report both raw and weighted coverage.

### 18.1 Raw Metrics

```text
total functions
candidate functions
assigned functions
processed functions
documented functions
classified functions
already-complete functions
library/generated functions
deferred functions
ambiguous functions
failed functions
conflicts
remaining functions
```

### 18.2 Quality Metrics

```text
precise semantic names applied
conservative address-suffixed names applied
signatures improved
known structs applied to parameters
unnamed class fields identified
class reference documents created or updated
function comments added
shared dependencies resolved
low-confidence names awaiting review
bad function boundaries awaiting repair
```

### 18.3 Progress Report

```json
{
  "pass": 1,
  "shards_total": 240,
  "shards_complete": 96,
  "candidates_total": 6840,
  "processed": 2714,
  "documented": 1510,
  "classified": 621,
  "already_complete": 403,
  "library_or_generated": 118,
  "deferred_dependency": 42,
  "ambiguous": 17,
  "analysis_failed": 3,
  "remaining": 4126
}
```

Do not report only the number of renames. A worker that correctly confirms an
existing name or classifies library code has still completed useful inventory
work.

## 19. Acceptance Criteria

The mass-documentation controller is ready for unattended operation when it
can:

1. Verify the expected Ghidra project, program, architecture, and image base.
2. Build a complete candidate inventory without erasing existing work.
3. Produce explicit, non-overlapping function shards.
4. Maintain leases so each root function has at most one active worker.
5. Dispatch multiple workers concurrently.
6. Require one structured result for every assigned function.
7. Apply names and signatures only at the permitted confidence level.
8. Preserve existing stronger names and types.
9. Detect concurrent changes before overwriting a function.
10. Route shared structure and function-boundary changes through serialized
    review.
11. Recover partial results from worker or MCP failures.
12. Requeue dependencies and low-confidence cases without repeating completed
    high-confidence work.
13. Produce reproducible progress and final coverage reports.
14. Leave every residual function explicitly classified, even when exact
    semantics cannot be recovered.
15. Read the owning class reference before class-function analysis and preserve
    evidence-supported unnamed-field discoveries in that reference.

## 20. Completion Criteria

The campaign is complete when:

- Every candidate function has a final result record.
- No shard remains pending, leased, or partially reported.
- No raw `FUN_` function remains unexplained unless it is explicitly marked
  ambiguous, failed, boundary-invalid, or intentionally unresolved.
- All `deferred_dependency` entries have been retried after their dependency was
  resolved or formally accepted as residual unknowns.
- All `conflict` entries have been reviewed.
- All applied precise names are `high` or `certain` confidence.
- All applied medium-confidence names are conservative and address-suffixed
  where needed.
- Shared structures changed during the campaign have documented size and field
  evidence.
- Every identified unnamed class field has been recorded under
  `doc/ghidra_classes/`, including unresolved candidates that were not safe to
  apply to Ghidra.
- A final consistency pass has completed.
- The final report separates semantic coverage from library/generated code and
  irreducible unknowns.

Completion does not require assigning invented semantics to every function.
An honest unresolved classification is superior to a confident but incorrect
database name.

## 21. Key Constraints

- **Preserve prior automation:** Do not rerun broad vtable, local-function, or
  class ownership naming unless a concrete defect is found.
- **Explicit root ownership:** A worker may rename only functions listed in its
  shard.
- **Read across boundaries, write within boundaries:** Callers and callees may
  be inspected for evidence without taking ownership of them.
- **Centralize shared types:** Structure edits and broad type changes are
  serialized.
- **No unsupported precision:** Exact semantic names require multiple agreeing
  facts.
- **Use existing structs:** Apply established types such as `cChrInput`,
  `sAction`, or other suitable known structures when the function evidence
  supports them.
- **Read and update class references:** Check
  `doc/ghidra_classes/<Class>.md` before class analysis and record identifiable
  unnamed fields discovered while decompiling.
- **Do not erase uncertainty:** Broad names use address suffixes; unresolved
  offsets remain documented as unknown.
- **Do not let one function stall a shard:** Respect the first-pass time budget
  and requeue difficult work.
- **No destructive database edits:** Never delete structures, functions, or
  prior analysis merely to simplify a worker's decompilation without serialized
  review.
- **No source-code changes:** This campaign modifies and documents the Ghidra
  database. Plugin source changes are outside scope unless separately requested.
- **Current database is authoritative:** Re-read live names and signatures
  before applying changes.

## 22. Quick Reference

### Worker Loop

```text
for each assigned root function:
    identify owner and read doc/ghidra_classes/<Class>.md if present
    read current function and variables
    decompile
    if existing work is complete:
        record already_complete
        continue

    classify trivial behavior if possible
    inspect direct callers/callees, vtables, constructors, and class references
    before choosing an owner or semantic name
    inspect disassembly when decompilation is questionable
    identify owner and apply an existing struct when justified
    if field accesses match an established class, retype the first parameter
    to <Class> * even when the semantic name stays broad
    record evidence-supported unnamed fields in the class reference

    if owner and precise behavior are high confidence:
        apply class-qualified semantic name and supported signature
    elif owner and a useful broad category are medium confidence:
        apply conservative owner-qualified address-suffixed name
    else:
        preserve FUN_<address> and document known facts

    reject names based only on offsets, primitive types, table guesses,
    or generic words such as lookup/process/data/value/helper/function

    verify function did not change concurrently
    record outcome, evidence, dependencies, and applied changes
```

### Coordinator Loop

```text
verify Ghidra program
inventory unresolved functions
create explicit non-overlapping shards

while pending shards exist:
    dispatch up to worker limit
    collect per-function results
    expire failed leases
    requeue only unfinished addresses
    collect shared dependency requests

resolve high-fan-out dependencies
run low-confidence review
run consistency pass
run final residual pass
publish final coverage report
```

### Core Ghidra MCP Operations

```text
Instance validation:
  ghidra_instances_current
  ghidra_analysis_status

Function inventory and context:
  ghidra_functions_list
  ghidra_functions_get
  ghidra_functions_get_variables
  ghidra_functions_decompile
  ghidra_functions_disassemble
  ghidra_xrefs_list
  ghidra_analysis_get_callgraph
  ghidra_analysis_get_dataflow

Type context:
  ghidra_classes_list
  ghidra_structs_list
  ghidra_structs_get
  ghidra_datatypes_search

Function documentation:
  ghidra_functions_rename
  ghidra_functions_set_signature
  ghidra_functions_set_comment

Serialized shared changes:
  ghidra_structs_create
  ghidra_structs_add_field
  ghidra_structs_update_field
  ghidra_data_set_type
  ghidra_data_rename
```

### Final Principle

```text
Maximize useful, evidence-supported coverage.
Parallelize independent function roots.
Serialize shared database concepts.
Preserve existing work.
Record uncertainty instead of inventing certainty.
```
