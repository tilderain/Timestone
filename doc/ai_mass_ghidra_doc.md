# AI Mass Ghidra Documentation Specification

**Status:** Active implementation specification
**Scope:** Brute-force completion of the UMVC3 Ghidra database after automated naming
**Primary consumer:** An AI coordinator dispatching multiple Ghidra analysis subagents

**Lightweight reporting:** Keep the analysis, typing, class-reference, and safety
requirements below. Do not build a JSON reporting framework to execute them.
Use explicit address assignments and compact per-worker TSV checkpoints. Keep
rename reasoning in the analysis; log detail only for exceptions or type changes, and
return counts plus the log path rather than copying every record into chat.
User-specified scope takes precedence: an explicitly function-name-only assignment
does not authorize signature or type changes. Otherwise, ordinary mass-renaming
work includes evidence-supported parameter naming and typing, especially when a
class or debug-symbol reference is available. Do not narrow a general request to
function-name-only work by default. Shared-structure changes remain serialized.

**Enforcement:** Every dispatch must include the worker contract in section 11,
not just a link to this document. Every approval must pass the coordinator gate
in section 4.1.1. Exact original names and complete typing are not prerequisites
for a supported rename; verified class ownership and honest provenance are.
The contract is an acceptance standard, not a per-function tool-call checklist.
Use the fast path in section 4.1.2; do not turn enforcement into duplicate research.

## 1. Core Objective

Systematically analyze, name, type, and document the remaining unresolved
functions in the current Ghidra database for
`umvc3.exe.unpacked.exe`.

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
| `classified` | Exact semantics remain unknown, but behavioral category and any proven owner are documented. |
| `already_complete` | Existing name, signature, and typing are sufficient; no change required. |
| `deferred_dependency` | Analysis depends on a structure, caller, callee, or subsystem not yet resolved. |
| `ambiguous` | Multiple plausible interpretations remain after bounded relevant inspection. |
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
5. Track exclusive assignments and completed results.
6. Detect duplicate or conflicting work.
7. Schedule dependency and low-confidence passes.
8. Personally review worker changes before accepting them as verified.
9. Publish progress and final coverage reports.

The coordinator must not spend its context deeply decompiling every function.
Its primary job is work allocation, validation, and recovery.

**Mandatory orchestrator review:** The coordinator must review every worker's
completed batch itself; a worker's success report or "live-verified" claim is
not independent validation. Read the actual name and signature deltas, reconcile
them with live Ghidra state, and check that the changes remain within scope.
Personally inspect code and relevant callers for ownership changes, newly precise
semantic names, signature/type changes, and contradictions between workers or
existing references. Spot-check straightforward remaining changes and expand the
review when a defect appears. Review effort should follow risk, not require a
complete second investigation of every trivial function.

Check getters against the best available property/consumer evidence; inspect
setters and initialization paths to resolve uncertainty, not as mandatory complete
reconstruction before naming. Use native offsets;
existing structure field labels and generated names can be wrong. Verify base,
derived, embedded, and shared receiver identities rather than trusting a
decompiler cast. Report unsupported changes and review gaps explicitly; do not
count unreviewed worker claims as independently verified. Review agents may
assist, but do not replace the orchestrator's own review. Keep review notes
compact and focused on findings, without reinstating per-function evidence essays.

Reuse existing proof in one coordinator decision pass. Name-only work requires
neither a second full investigation, a per-function pointer census, nor manual
function-code/evidence-file hashes. Preserve target identity, reviewed-cell integrity,
live state/body-range guards and transactional readback in the application tooling.
An old naming hold may be reconsidered on existing facts under the relaxed policy
only with an explicit tracked decision identifying the superseded requirement and
remaining holds. Stale old-name/amendment paperwork is not a new-research gate;
preserve history and use existing guarded transitions. Do not automatically reopen
or approve old holds, and do not treat this policy as resolving factual conflicts.

### 4.1.1 Approval Gate

Before approving each exact destination, the coordinator must be able to answer:

> What proves this class belongs before this method name, and what proves the
> method describes this function's behavior?

An agent's confidence claim, generated name, decompiler cast, or the class of an
object merely used by the function does not answer that question.

1. Read every exact proposed name and its evidence. Reject invented owners,
   generic replacement prefixes, unsupported class assignments and overstated
   semantics. Personally inspect risk-bearing code/callers as required above;
   review-agent approval is assistance, not a substitute for this check.
2. Distinguish emitted MPR matches, declaration/layout-supported descriptions,
   and native-derived descriptions. Never advertise the latter two as exact
   original symbols. Preserve overload and target-ABI uncertainties separately.
3. Resolve withdrawals before approval. A correction buried in notes does not
   neutralize an incorrect stored request. Use a history-preserving amendment,
   or explicitly hold that operation. Do not approve the withdrawn payload.
4. Approve only the reviewed explicit address set, using the existing bulk CLI.
   Names, signatures, parameter names and shared layouts remain independent
   decisions; do not wait for unrelated type holds or another identical review.
5. For heterogeneous shared entries, require an explicit representation decision
   before any primary-name change. Per-use names are not exclusive declaring
   identities. Workers must not choose a representative class themselves. If a
   nonexclusive display name is expressly authorized, preserve the per-class
   mapping and state the primary-name/decompiler limitations. A comment is not
   an installed secondary symbol; verify and report each separately.
6. Reconcile proposal, approval, committed live application and project save as
   four distinct states. Count applied changes from actual committed readback,
   not worker totals, selected operations or successful submissions.
7. Do not silently substitute rollback for correction. First correct ownership
   while retaining supported behavior. An explicitly authorized restoration must
   preserve history, identify any unverified old owner, and remain unfinished
   semantic work. Never report a restored placeholder as a recovered method.

These checks belong in the existing review/ledger evidence, not a new reporting
framework or a mandatory second full investigation of every trivial function.

### 4.1.2 Throughput Fast Path

Use the smallest evidence set that establishes the claim and resolves any known
contradiction. More tool calls, longer notes and repeated reviews do not by
themselves improve confidence. Class-qualified naming, independent approval and
application safeguards remain required; redundant work does not.

| Case | Sufficient action | Do not require by default |
|---|---|---|
| Straightforward getter, setter or wrapper | Body plus a decisive native ownership/behavior anchor, with no known contradictory use; submit immediately | A fresh caller, setter, constructor, DTI and full lifecycle investigation for every root |
| Coherent class/method family | Establish shared constructor/property/dispatch proof once, then check each body's distinguishing behavior | Rediscover or duplicate the same class evidence for every method |
| Incomplete type or ABI | Submit the supported name and separately record the exact type blocker | Solve missing arguments, enum definitions, whole-class layouts or pointer census before naming |
| Ambiguous owner or shared body | Inspect the most promising missing route; if it does not resolve the decision, record the concrete gap and continue | Repeated full passes over unchanged evidence or an exhaustive search for nonexistent symbols |
| Ready reviewed batch | Coordinator makes one risk-based decision and bulk-approves the accepted explicit addresses | A routine researcher -> reviewer -> second reviewer -> coordinator rediscovery chain |

- A decisive native property callback or constructor-backed receiver route can
  establish ownership and behavior together. Do not impose a universal minimum
  number of independent references. Expand inspection when sharing, conflicting
  evidence, precise claims or ABI risk makes it necessary.
- Read the specification and workflow once per agent context. Reuse unchanged
  instructions, class references, donor records and history within that context;
  revisit them for actual changes or a specific unresolved question, not every
  checkpoint. Full class-layout recovery rules apply to layout work, not incidental
  fields encountered during function naming.
- Self-review is a brief challenge of conclusions already reached, not a second
  round of the same tool calls. The coordinator still reads every exact proposal
  and its evidence, but risk-based spot checks cover straightforward family members.
  A discovered defect expands the relevant review, not automatically the whole wave.
- Store evidence once in the existing ledger or reference notes and cite it.
  Routine TSV rows remain four columns. Keep exception/type notes only when needed;
  do not reproduce full proposal tables in chat when the ledger already contains
  them. Counts and checkpoint paths are sufficient for routine worker closeout.
- Successful unambiguous CLI receipts normally establish submission/approval state.
  Do not add per-address `show` loops to reconfirm them. Actual application still
  needs its required live preflight and committed readback; ambiguous failures,
  interruptions and concurrent changes require reconciliation.
- Mark an unsupported signature operation held and deliver the supported name now.
  Do not wait for another agent, an entire wave, or unrelated type infrastructure.
  Never silently truncate a type proposal to make it pass.
- Every additional investigation step should answer a concrete question that could
  change the decision. Otherwise checkpoint the uncertainty and move to another
  assigned root. Prioritize fresh or actionable work over recycling difficult holds.

### 4.2 Function Worker

Function workers perform the brute-force analysis. Please provide a copy of this doc or tell the worker to read the doc.

Each worker receives a
fixed list of function entry addresses and must:

1. Analyze only its assigned root functions.
2. Read neighboring callers and callees when needed for context.
3. Submit supported improvements; apply only independently authorized operations.
4. Treat unassigned neighboring functions as read-only context. Route changes
   with cross-function propagation or shared-type impact through the coordinator.
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

Record project, program, image base, language, analysis status, inventory time,
and live function count once in the campaign log. No separate JSON is required.

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

Use a plain address list in the worker assignment, with a shard identifier and
pass number when needed. A separate JSON shard file is unnecessary.

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

### 6.4 Assignment Ownership

Track which worker owns each active shard. For a manually coordinated campaign,
the explicit assignment is sufficient; no lease JSON or expiry framework is
required. If a worker fails or times out, confirm that it has stopped writing
before reassigning its roots. Expiration alone does not prevent an old worker
from continuing to modify the database.

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

This is a concurrency boundary, not approval to apply fresh research. Names,
signatures and parameter edits require their respective authorization. A worker
must not self-approve a proposal merely because the root is exclusively assigned.

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
5. If the function is trivial, submit its supported name and types independently
   for review; apply only authorized operations.
6. If behavior is nontrivial, proceed to contextual analysis.

### 8.2 Contextual Analysis

Inspect only as much context as needed:

Start with the body and best available reference or direct use. Add context only
when it can resolve the naming decision or a concrete contradiction; the sources
below are alternatives, not a required per-function checklist.

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
- Independently verified automated ownership associates it with one class;
  the generated name alone is not proof, since prior scans crossed table boundaries.
- The function is referenced only by a confirmed class-specific path.

Separate the incoming receiver/declaring class, embedded subobject, explicit
input/output object, and collaborator singleton or resource. A wrapper's output
object or queried singleton does not establish its owner. Conversely, an unused
optimized receiver does not prove a free function: inspect caller-side receiver
provenance before discarding ownership evidence.

Apply the appropriate existing structure to the `this` parameter when known.
For example, if analysis establishes that the first parameter is `cChrInput *`,
use that type instead of leaving `undefined8` or inventing a duplicate
structure.

**Independent parameter typing.** Record a supported receiver type during the same
inspection and submit it independently when its ABI and target mapping are proven.
Named fields or a generated cast alone do not prove the receiver. A type hold or
pending type approval must not delay a supported rename. Examples of supported
name/type combinations, not mandatory same-edit outcomes:

```text
void sMvc3Main__setPendingGraphicsOption(sMvc3Main *this, uint index, uint value)
void cActionTime__startHitSlow(cActionTime *this, ...)
MtGeomCapsule__getSupportPoint(MtGeomCapsule *this, ...)
```

A class-qualified rename can be complete as a naming operation while receiver
typing remains held. Record unresolved fields/types separately. Use a proven
class or base owner when appropriate, without claiming an unproven derived owner.
For folded/shared code, do not force one caller's class onto the implementation.

Attempt ownership recovery with bounded relevant context, not every constructor,
vtable and caller path. Every new semantic name requires a proven real declaring
class prefix `Class__method`, with exact supported spelling. If ownership remains
unknown, preserve the existing name and comment the known behavior and ownership
hold. Do not use global names or invented effects/stage/networkSession/sound/ui/
character/math/Helpers/NativeFighter buckets. For heterogeneous folded code, do not
invent a common base; a base-qualified name requires proven declaring ownership.

### 8.4 Behavioral Naming

Names must describe established behavior and a proven real declaring class, not
merely restate an implementation detail. Attempt bounded recovery of a useful
class-qualified behavioral name, without requiring the original method name.
Inspect direct callers, callees, vtable or constructor relationships,
relevant strings or data, and the owning class reference as needed to establish
ownership and behavior. This is not a mandatory exhaustive checklist when the
evidence is already sufficient. For a shared helper, inspect enough distinct
uses to avoid assigning one caller's class-specific semantics to generic code.

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
known but the exact operation is not, use a supported broad behavioral category
and document uncertainty. Use `sMain__method_140003180` only if no meaningful
category is supported; do not promote a guessed table role to a semantic name.

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

Exact MPR correspondence is not required. Explicitly label native-derived names
as such rather than donor-recovered. A verified owner plus a supported behavior
category is enough; property metadata or consumer behavior may establish broad
getter semantics without complete field or lifecycle recovery. Shared functions
require a proven common declaring class or base; otherwise preserve their existing
names and comment the ownership hold. `setEnabled` requires known boolean-enable semantics, not just a byte write;
`writeByteAtOffset` is still an implementation dump, not semantic recovery.

Medium-confidence conservative names need documented uncertainty, not an address
suffix when unique and free of overload ambiguity. Suffixes are allowed for
collisions or broad uncertainty; do not strip existing suffixes cosmetically.
`lookupTableEntry` requires evidence of actual table selection, not a lone DWORD
load. Hold unsupported precise-method, receiver, type, enum or return claims
separately instead of blocking a broader supported rename.

### 8.5 Signature Recovery

**Parameter names and types are part of the normal task.** When the evidence
supports them, rename generic parameters such as `param_2` to meaningful names
and submit their supported types during the same inspection as the function
rename. This applies to all parameters, not just `this`. Do not leave recoverable parameter
names or types for a separate cosmetic pass, particularly when working from MPR
or another class/debug-symbol reference.

Naming, parameter naming and type changes are independently reviewed/submitted
operations, not mandatory same-edit changes. A signature hold must not block a
supported name. For native functions, target-backed descriptive parameter labels
are allowed as inferred/native-derived, never donor-recovered; emitted-only donor
spelling rules apply to claims of recovered donor names, not these native labels.

For reference-backed functions:

- Match the specific method and overload using target behavior and callers
  before adopting its parameter names or types.
- Use recovered reference parameter names when their target argument roles agree;
  otherwise use conservative names supported by target code. Treat inferred
  reference names as hypotheses, not recovered debug information.
- Verify target argument count, order, register/stack use, widths, and hidden
  return parameters. Translate types to the target ABI and reuse established
  target structures; never copy x86 layouts or signatures blindly into x64.
- Preserve stronger existing names, types, and storage. Do not add an unused
  donor-only argument or invent a precise type merely to match the reference.
- Re-decompile after signature/type edits and check relevant callers for argument
  and return-value consistency. Record actual signature/type deltas in the short
  companion notes required by section 10. Record concrete unresolved typing
  blockers rather than silently treating a function-name-only result as complete.

Set a signature when supported by callers, callee usage, and the function body.
Check:

- Return value use at all inspected call sites.
- Register and stack parameter use.
- Signed versus unsigned comparisons.
- Pointer dereferences and established data types.
- Whether the first parameter is a class instance.
- Whether an apparent parameter is actually unused compiler residue.
- Calling convention and hidden return-object parameters.
- Whether a receiver type is independently supported under section 8.3; submit
  that type separately without making its approval a naming prerequisite.

Do not force exact enum, const, reference, or ownership semantics without
evidence. A correct `uint32_t` is better than an invented named enum. A correct
`<Class> *` receiver should be submitted when proven, with its own approval.

### 8.6 Structure Application

For a dedicated declarative class-layout campaign, use
[AI Mass Ghidra Field Recovery](ai_mass_ghidra_fields.md). That workflow assigns
exclusive class JSON files to researchers and serializes destructive field
application through `ApplyClassLayouts.java`; ordinary function workers still
follow the shared-structure boundaries below.

Class-layout assignments must follow the entire target ancestry root first using
`<ghidra-scripts>/parents.txt`, where `<ghidra-scripts>` is the configured
external Ghidra script directory. Every unfinished ancestor is prerequisite
work, not an optional follow-up. Release child field mapping after all ancestors
have reviewed, target-supported layouts with stable boundaries and relevant
fields; record the accepted versions and reuse unchanged work after drift checks.
Parent proposals need not be applied live before child research. Apply required
parent changes and verify them live before dependent child application. Unrelated
documented unknown regions do not block child research, but unresolved ownership
or base boundaries do. The file's `resolved` status establishes
ancestry, not field-recovery completion. Record prerequisite evidence and defer
children with unfinished parents or unresolved base boundaries as specified in
the field-recovery protocol.

When a donor class reference exists, field recovery means mapping its declarations
to target storage. After donor retrieval, check the target class's `createProperty`
first: when registered names match, use verified direct member registrations as
the primary offset anchors before broad constructor/accessor searches. Confirm
helper argument roles and receiver adjustments; follow callbacks, base builders,
and nested loops. Matching names alone do not prove storage offsets, and computed
properties must not become invented fields. If no usable builder exists, use
other native evidence. Field recovery means mapping donor declarations
to target storage, not inventing replacements for its names. Before proposing
field names or layout edits, read `query.py class <name> --section all` and follow
relevant base, embedded/nested, array, typedef, and enum type references with
`query.py type <ID>` or full class queries. Full class output is not recursive;
the default summary or fields-only output is insufficient. Record consulted donor
IDs and unresolved dependencies as required by the field-recovery protocol.
Preserve verified
donor member names, declaring owners, nested records, arrays, and target-width
enum mappings. Use target anchors to validate correspondence across ABI/build
differences. Duplicate flattened labels require containment research, not offset
or arbitrary occurrence suffixes to appease an importer. Request shared-type or
importer support if the verified representation is unavailable; keep unmatched
regions unresolved rather than forcing a name. Distinguish mapped donor fields
from verified target-only fields and unresolved mappings.

If a known structure clearly matches an unknown parameter or local variable,
submit/apply it within authorized type scope when it materially improves the
decompilation and the match is supported. Pending typing does not block naming.

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

For a class-owned function, consult the relevant available class reference as
needed for the naming decision and reuse it within the batch:

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

If the class reference exists, read the relevant evidence. Treat it
as accumulated reverse-engineering evidence: use its confirmed inheritance,
size, fields, methods, ownership rules, and unresolved questions as context.
The live Ghidra database and current instructions remain authoritative when a
reference is stale or conflicts with verified code.

If no reference exists, retain useful class findings for serialized reference
creation; this is not a rename prerequisite. Do not create an empty file merely
because a class name appears in a generated function name.

While decompiling, note relevant accesses to unnamed class fields. Examples
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
5. Record the discovery for serialized class-reference merging; a pending merge
   does not block the rename or naming shard closeout.
6. Submit a structure dependency request if applying the field to Ghidra would
   change a shared structure.

Preserve incidental field findings, but do not turn naming into mandatory field
or whole-class reconstruction. Dedicated field/layout assignments retain their
full donor correspondence, boundary, type and documentation requirements.

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

When a function produces class-reference updates, note the affected document
path in its result. Keep field details and evidence in that document rather than
duplicating them in a nested JSON record. Pending findings can be recorded briefly
in the worker log until serialized merging is possible. Reuse class references
already read within the batch unless another worker has updated them.

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

Record incidental findings for these class references when encountered; their
completion is campaign follow-up, not a prerequisite to a supported rename.

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

Evidence sources (a heuristic ordering, not a rule that metadata overrides instructions):

1. Explicit DTI, property, resource, or format string names, with their precise
   scope recorded (class/property identity does not establish every method symbol).
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
| `certain` | Explicit name or behavior confirmed from authoritative metadata and code. | Apply reviewed semantic name; review signature independently. |
| `high` | Multiple independent code facts agree. | Apply reviewed semantic name; review signature independently. |
| `medium` | Broad behavior is clear; exact semantic detail is uncertain. | Apply reviewed conservative name with documented uncertainty; suffix only when useful. |
| `low` | Plausible interpretation based on limited evidence. | Do not apply a precise semantic name; classify and queue for review. |
| `unknown` | No responsible behavioral conclusion. | Preserve or improve ownership-only placeholder and record unresolved state. |

Confidence is not a numeric decoration. It controls whether a precise name may
be written into the canonical database.

### 9.3 Minimum Naming Standard

A precise semantic rename requires `high` or `certain` confidence in the claimed
behavior and any claimed owner. `medium` confidence permits a conservative
behavioral name with a proven real declaring class prefix. Without that owner,
preserve the existing name and comment the hold. A unique name without overload ambiguity needs no suffix; record
uncertainty in evidence/comments. `low` confidence permits an ownership/category
placeholder or comment, but not a guessed operation. `unknown` confidence must retain the raw
name unless an existing meaningful owner-only name is already present.

The following are mandatory rejection cases, regardless of confidence claims:

- The name only describes a primitive load/store, field offset, register, or
  inferred table representation.
- The name uses `lookup`, `process`, `data`, `value`, `helper`, or `function`
  without an established object and operation.
- The name asserts an unproven owner, including one caller's class on shared code.
- The name relies only on decompiler-generated labels/casts or contradicts known
  direct context. Additional context is required to resolve a decision, not merely
  because more references exist.

Existing meaningful names must not be replaced by lower-confidence names.

## 10. Function Result Record

Every assigned function must produce one result record, including functions
that required no change:

For any proposed rename, establish behavior and any claimed ownership using
the evidence rules above. Do not rewrite that reasoning into checkpoint prose.
The checkpoint tracks progress and actual changes; it is not a research report.

Use one TSV file per worker with this minimum header:

```text
address\told_name\tnew_name\toutcome
```

Use the outcomes from section 3. Routine completed or already-complete entries
need only these four columns. Do not append explanations, confidence labels,
"live rename verified" statements, or typing caveats to every row. The evidence
and confidence policies still govern analysis and mutations, not mandatory prose.

Add a short companion note only for unresolved cases, conflicts, failures, or
signature/type changes. State the specific blocker or record the actual
before/after signature/type; do not repeat the entire investigation.

Record class-document updates once per batch, with their paths, rather than
repeating field findings already stored in those documents. No nested schema or
per-function list of references read is required.

Work in batches of at most 10 roots. Retain old/new names and outcomes in working
context, then write one compact checkpoint after the batch. Do not write a
`pending` row before each mutation and patch it again afterward. Do not repeatedly
rewrite or display earlier rows. If stopping early or encountering an access
failure, checkpoint completed work once before returning when possible.

This trades per-function durable checkpoints for fewer tool calls. If a worker
is interrupted before its checkpoint, some applied changes may be absent from
the log. Reconcile unreported roots against live Ghidra state before retrying;
never assume an unlogged mutation did not happen or count a proposal as applied.

Keep the live pre-mutation conflict check. For post-mutation confirmation, a
successful tool response reporting the updated name/signature is sufficient.
Retrieve live details again only when the response is unclear, the mutation
failed, or analysis needs the updated state.

Return counts, unfinished addresses if any, and the log path. Do not paste the
full records into the conversation or make workers maintain redundant totals.

### 10.1 Worker Self-Review

Before submitting each batch, the worker must challenge the correctness of its
final names and signatures, not merely confirm that the mutations succeeded.
A successful tool response confirms application, not semantic validity.

1. **Ownership:** Check that target evidence establishes the proposed owner.
   Generated names, existing parameter types, and decompiler casts are not
   independent proof.
2. **Shared implementations:** Inspect relevant references for conflicting
   receivers or uses, especially for trivial getters and stubs. Do not assign
   one class's identity to a folded implementation shared by unrelated classes;
    use a proven common declaring class only when supported; otherwise retain the
existing name and comment the ownership hold and differing per-use meanings.
3. **Semantic precision:** Ensure the name describes supported behavior, not a
   primitive offset or a plausible donor match. Downgrade unsupported precision
   to a permitted classification or unresolved outcome.
4. **Types and ABI:** Check argument roles, widths, return usage, and hidden
   parameters. Resolve conflicting object types through allocation, constructor,
   vtable, and DTI evidence as available, rather than trusting existing casts.
5. **Post-edit consistency:** Re-decompile signature/type changes and inspect
   representative affected callers. Check that the final interpretation agrees
   with the evidence; do not treat newly propagated types as corroboration.
6. **Final checkpoint:** Reconcile the TSV and signature notes with the final
   applied state, including self-review corrections and unresolved dependencies.
   Clear mutation responses suffice unless state is uncertain or concurrent
   changes require a fresh read.

Withdrawn or narrowed claims must be identifiable in the final handoff. If the
worker cannot amend a guarded request, explicitly identify the exact stored
operation that must not be approved and supply the supported correction. Do not
leave a known-wrong request presented as ready merely because submission succeeded.

Correct unsupported changes within the assigned roots before submission, using
the same live conflict checks as any other edit. Do not overwrite another
actor's changes or expand into unassigned functions or shared types; report
those dependencies instead. Keep notes limited to actual signature deltas,
corrections, and unresolved issues, without adding routine per-function essays.
Worker self-review is mandatory and does not replace the coordinator's
independent review in section 4.1.

## 11. Worker Prompt Contract

Every function worker must receive a prompt containing:

- Exact project and program identity.
- Explicit root address list.
- Statement that existing automated names must be preserved unless improved by
  evidence.
- Permission boundaries for database changes.
- Required per-function protocol.
- Confidence and naming rules.
- Mandatory worker self-review before submission (section 10.1).
- Compact TSV checkpoint format from section 10.
- A hard instruction to return a result for every root address.

Include the following contract in the dispatch, with the placeholders filled in.
It supplements rather than replaces reading the complete specification. If tool
output truncates the document, continue reading the remaining sections. Do not
shorten away the ownership, approval, shared-entry or scope restrictions.
Within the same agent context, reuse the already-read contract and specification;
subsequent batches need only their assignments, changed instructions and relevant
exceptions. The numbered rules are not twenty separate evidence reports or tool
calls for every function.

Reference worker prompt:

```text
Research exclusively assigned functions in umvc3.exe.unpacked.exe.
Discover the live instance and verify its actual program ID, not just its title.
Assigned roots: <EXPLICIT NON-OVERLAPPING ADDRESS LIST>
Checkpoint: <UNIQUE WORKER TSV PATH AND SHORT EXCEPTION-NOTES PATH>
Operation scope / existing approvals: <EXPLICIT AUTHORIZED SCOPE>

Read the COMPLETE specification:
/media/god/tempData/SteamLibrary/steamapps/common/UMVC3Rollback/doc/ai_mass_ghidra_doc.md
Also read AGENTS.md and FUNCTION_STATUS.md under:
/media/god/tempData/Lostplanet/mpr_reference/

NAMING
1. Every new semantic name requires VerifiedClass__supportedMethod. Preserve
   established flattened nested-class naming and actual native class spelling.
2. Establish ownership through native constructors, DTI/vtables, properties,
   caller receiver flow or matched declarations corroborated by target behavior.
   Existing names, casts, adjacency and argument types alone are not proof.
3. Distinguish receiver, embedded object, explicit input/output object and
   collaborator singleton/resource. An unused optimized receiver still requires
   caller inspection; do not automatically classify it as a free function.
4. No invented Helpers/NativeFighter/SharedState owners, subsystem replacement
   prefixes such as effects/sound/ui/networkSession, or global replacement names.
5. Conservative behavior is acceptable with a proven class. Exact original
   spelling, complete layouts and complete signatures are not naming prerequisites.
6. State provenance: exact emitted MPR match, declaration/layout-supported
   descriptive name, or native-derived description. Do not mislabel inferred names.
7. For shared code, record actual classes and per-use meanings; never invent a
   common base or silently pick one exclusive owner. Submit any multi-owner/display
   representation for coordinator review. Do not install a representative primary
   name yourself. Documented aliases are not installed secondary symbols.
8. Do not merely strip a suffix or substitute an offset/type/vague operation.
   Improve meaning. Use suffixes for ambiguity/collisions, not by default.

TYPES AND PARAMETERS
9. Research argument roles/types with the name. Reuse proven target types; check
   arity/order, registers, widths, returns and hidden buffers before type proposals.
10. Rename, signature, parameter-name and shared-layout approvals are independent.
    A type hold does not block a supported name or become authorized by that name.
11. Do not add donor-only arguments or copy x86 storage/layout into x64. Preserve
    stronger verified metadata. Re-decompile and inspect affected callers after
    authorized signature/type changes, not after proposals alone.
12. A pointer-cell guard must address actual function-pointer storage. A CALL/LEA
    instruction referencing a function is not that cell; a callee's pointer cell
    is not the root's. Do not invent or weaken guards to obtain acceptance.

WORKFLOW
13. Read history once; reuse class references and existing evidence. Begin with the
    body and best ownership reference; expand only to resolve a concrete question.
    Reuse a proven family anchor across roots. One decisive anchor can suffice;
    constructors, properties and callers are alternatives, not a mandatory checklist.
14. Checkpoint at most ten roots at a time. Continue after blockers instead of
    consuming the shard on one function. No mandatory exhaustive caller checklist.
15. Submit supported exact requests using the existing ledger CLI. No fresh
    self-approval or application claim from submission. Preserve historical holds;
    supply exact corrections/evidence when guarded old requests conflict.
16. Mutate only assigned roots and authorized operations. Other roots are read-only
    context. Shared structures, enums, globals, boundaries and class documents need
    serialized authorization; collect incidental findings without blocking names.
17. Follow repository access rules: assigned RE documents are allowed, delegated
    application-source exploration is not. No manual research hashes, reporting
    frameworks, bookkeeping wrappers or unauthorized database-wide analysis.

SELF-REVIEW AND HANDOFF
18. Challenge ownership, shared bodies, semantic precision, receiver/collaborator
    confusion and independent ABI/guard validity. Check fresh live state before edits.
19. Record an outcome for EVERY root. Keep the four-column TSV and short exception/
    type notes; explicitly distinguish proposed names from actual live names.
20. Return processed/proposed/applied/unresolved counts separately and checkpoint
    paths. Keep exact requests in the ledger; call out risky anchors, withdrawals
    and corrections in the handoff without repeating every routine row. Do not
    claim completion from tool success. Apply the throughput fast path in 4.1.2.

Do not overwrite stronger work or silently restore old names as a substitute for
fixing ownership. Authorized restoration preserves history and is not semantic
recovery. Unsupported claims remain explicit, not hidden in completion totals.
```

## 12. Time and Depth Budgets

### 12.1 Per-Function Budget

Use decision-bounded effort, not mandatory timeouts or deep-research budgets.
Start with the body and best available reference/direct use; expand only when
the additional context can resolve the decision. Stop when a conservative name
is supported, or record the specific remaining uncertainty and continue. No
sleep, delay, cooldown or minimum investigation duration is required. One
difficult function must not consume an entire shard.

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

- Attempt bounded real declaring-class recovery; preserve the existing name and
  comment known behavior and the hold when no class or common base is proven.
- Resolve obvious wrappers, getters, setters, table lookups, lifecycle helpers,
  and class-owned operations.
- Recover meaningful parameter names and improve parameter types using existing
  structures and matched reference declarations.
- Apply class-qualified names when the real operation is supported by callers,
  callees, vtables, constructors, DTI metadata, or class references.
- Preserve `FUN_<address>` with a comment when only an offset, primitive access,
  or guessed table role is known.
- Defer expensive dependencies.

Pass 1 optimizes for useful conservative semantic coverage without guessing. Removing
the `FUN_` prefix is not a completion metric by itself.

### 13.3 Pass 2: Dependency Resolution

These later passes target unresolved claims; they are not additional approval
rounds for names already independently reviewed. Reuse unchanged evidence and
track explicit reconsideration of policy-only holds rather than reopening all work.

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
- `medium` names with a concrete unresolved semantic risk, not merely no suffix.
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

A short note identifying the requesting functions, candidate type, minimum
size, observed field offsets/widths/usage, allocation evidence, and whether the
dependency blocks analysis. Reuse existing class-reference findings rather than
copying them into a second JSON representation.

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
namespace__Class__method
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

If behavior is known but real declaring-class ownership is not, preserve the
existing name and add a concise comment recording behavior and the ownership hold.
Do not introduce a global behavioral name or a subsystem bucket. Shared/folded code
requires proven common declaring-class or base ownership before semantic renaming;
heterogeneous uses do not establish an invented common base. A genuine emitted
namespace may qualify a proven class only when supported; a namespace alone does
not satisfy the class-prefix requirement. Restoring an explicitly authorized prior
name after a bad rename is a history-preserving correction, not proof that the old
generated owner was correct; record that uncertainty and retain the original history.
A queried class argument, destination singleton, or explicit object argument alone
does not establish a wrapper's declaring owner.

### 15.2 Name Quality Gate

Before applying a rename, the worker must answer these questions during analysis,
not rewrite the answers into its result record:

1. Is the real declaring class/base proven, with exact supported spelling and a
   Class__method prefix? If not, retain the existing name and comment the hold.
2. What does the function do for its callers, rather than what instruction or
   field access happens to be visible in the body?
3. What target evidence supports the proposed verb/category and object at the
   chosen precision, and what uncertainty remains?
4. Why is the proposed name more informative than the existing generated name?

Reject the rename and preserve `FUN_<address>` when the proposed name would be
unchanged in meaning by replacing the operation with `lookup`, `process`,
`data`, `value`, `helper`, `function`, or an offset or type description. A raw
placeholder plus a precise comment is better than a misleading semantic name.

For class-owned functions without even a supported broad category, the fallback is:

```text
<EstablishedClass>__method_<address>
```

with a comment containing the confirmed behavior and unresolved question. Prefer
a supported broad behavioral name when available; neither exact donor recovery
nor exhaustive caller, callee and class analysis is a naming prerequisite.

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

The root address, `bad_function_boundary` outcome, specific conflicting address
or control-flow fact, and recommended investigation. Put this in the normal
result log; a separate JSON record is unnecessary.

Boundary repair is serialized because it may invalidate shard inventories and
neighboring analysis.

Distinguish an actual bad entry from an imperfect body range. When a real entry
and its behavior are supported, approve its conservative name independently while
retaining boundary/type holds. This does not authorize boundary repair or disable
the applicator's live body-range checks; failed guards still block application.

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
4. Use a supported broader name if exact semantics remain disputed; add an
   address suffix for collision or broad uncertainty when useful.
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
conservative behavioral names applied (with suffixes where useful)
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

Derive applicable counts from worker checkpoints and summarize them in a short
message. Include actual processed and remaining totals, applied changes, and
unresolved outcomes. No JSON progress report or manually duplicated worker
counters are required. Omit metrics outside the explicitly assigned scope.

Do not report only the number of renames. A worker that correctly confirms an
existing name or classifies library code has still completed useful inventory
work.

## 19. Acceptance Criteria

The mass-documentation controller is ready for unattended operation when it
can:

1. Verify the expected Ghidra project, program, architecture, and image base.
2. Build a complete candidate inventory without erasing existing work.
3. Produce explicit, non-overlapping function shards.
4. Track exclusive assignments so each root function has at most one active worker.
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
15. Consult relevant class-reference evidence and preserve incidental field
    discoveries for serialized merging without blocking supported names.

## 20. Completion Criteria

The campaign is complete when:

- Every candidate function has a final result record.
- No shard remains pending, actively assigned, or partially reported.
- No raw `FUN_` function remains unexplained unless it is explicitly marked
  ambiguous, failed, boundary-invalid, or intentionally unresolved.
- All `deferred_dependency` entries have been retried after their dependency was
  resolved or formally accepted as residual unknowns.
- All `conflict` entries have been reviewed.
- All applied precise names are `high` or `certain` confidence.
- All applied medium-confidence names are conservative with documented uncertainty;
  suffixes are used for collisions or broad uncertainty where useful.
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
  facts, which can be supplied by one decisive native anchor plus the body; this
  is not a fixed quota of callers, tools or independent investigations.
- **Use existing structs:** Apply established types such as `cChrInput`,
  `sAction`, or other suitable known structures when the function evidence
  supports them.
- **Use class references:** Consult relevant `doc/ghidra_classes/<Class>.md`
  evidence and preserve incidental field findings for serialized merging;
  full class/field recovery does not block naming.
- **Do not erase uncertainty:** Document broad-name uncertainty; use suffixes for
  collisions or broad uncertainty, not by default. Unresolved offsets stay unknown.
- **Do not let one function stall a shard:** Use decision-bounded context and
  record specific unresolved work rather than imposing a deep-research budget.
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
    consult relevant owner/reference evidence, reusing batch context
    read current function and variables
    decompile
    if existing work is complete:
        record already_complete
        continue

    classify trivial behavior if possible
    start with the body and best reference/direct use; expand only to resolve a decision
    inspect disassembly when decompilation is questionable
    identify owner; submit an existing receiver type independently when justified
    recover meaningful parameter names and types from target evidence and matched references
    validate target ABI and re-decompile after signature/type edits
    submit proven receiver types independently; typing holds do not block naming
    preserve incidental field findings for serialized class-reference merging

    if precise behavior and real declaring class are high confidence:
        submit semantic name and any separately supported signature request
    elif a useful broad category is supported at medium confidence and real declaring class is proven:
        submit conservative Class__method name using the proven class/base
        document uncertainty; add a suffix only when useful
    else:
        preserve the existing name and document known facts and ownership hold

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
    personally review worker deltas and verify risk-bearing changes in live code
    pass section 4.1.1; amend or hold withdrawn stored claims before approval
    bulk-approve explicit accepted addresses, names and types independently
    reconcile committed live readback and project-save status separately
    confirm failed workers have stopped before reassignment
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
