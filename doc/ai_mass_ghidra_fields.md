# AI Mass Ghidra Field Recovery

**Status:** Initial implementation; isolated SDK tests, live pilot still required.
**Target:** `umvc3.exe.unpacked.exe`, canonical types under
`/UMVC3/Auto`.

This is the class-layout counterpart to [AI Mass Ghidra Documentation](ai_mass_ghidra_doc.md).
Retain that document's target-evidence standards, ownership rules, rollback
priorities, worker self-review (10.1), and mandatory coordinator review (4.1).
The difference is the write boundary: field workers edit declarative files;
one coordinator applies reviewed shared datatype changes.

## Primary Objective: Map Donor Declarations

**This is MPR-to-MVC3 declaration mapping, not open-ended field-name invention.**
For shared classes, MPR supplies the member names, declaring owners, nested
records, arrays, base relationships, types, and enums. The worker's primary job
is to establish where those declarations belong in the MVC3 target layout.
Once correspondence is verified, use the donor member name rather than inventing
a synonym. Preserve stronger verified target-specific information when builds
differ; report incompatible mappings instead of overwriting it.

Each accepted mapping must connect:

```text
donor/type ID + declaring class + member path
    -> target full datatype path + offset + storage width/type
    -> target evidence establishing correspondence
```

For nested/array members, include the containing member, element count/stride,
and both complete-object and element-relative offsets. Donor offsets remain
donor facts, never target addresses. Distinguish MPRCenter and MPRSatellite
variants when their declarations disagree.

First check for the target class's `createProperty` implementation. When its
registered field names correspond to the donor class, start offset recovery
there, following the property-registration rules below. Otherwise use strong
anchors such as constructor initialization, resource bindings, or matched methods.
Align the surrounding declaration
sequence against target access widths, ordering, and array strides. A coherent,
independently anchored block can establish several members together; exhaustive
reinvention of each name is unnecessary. Check the block boundaries and any
contradictory access before accepting it. Declaration order or matching size
alone is insufficient. Account for pointer expansion, alignment, and inserted,
removed, or changed members; never carry one offset adjustment across a boundary
where those assumptions stop holding.

Classify results as **mapped donor fields**, **verified target-only fields**, or
**unresolved mappings**. Invent semantic names only for verified target-only
members after checking the relevant donor class, bases, and nested declarations.
An unmatched donor candidate is not automatically a target-only field. Keep
unresolved bytes neutral and candidate names unapplied. High donor coverage is
the objective, not an assumed percentage or a quota to satisfy by guessing.

## Preserve Declaration Structure

Duplicate flattened names are a signal to recover their containing declaration,
not a request to make strings unique. Investigate which donor record owns the
member, which class member contains that record, and which array element or
instance maps to the target location. Preserve verified member names inside
properly named target-sized records and arrays instead of encoding containment
into long flattened labels.

**Do not append offsets or arbitrary occurrence numbers to semantic member names
merely to satisfy the importer.** `_5_mAnmId__at_3e918` is a rejected workaround,
not field recovery. If the donor correspondence supports it, represent the
containing array/record and retain `mAnmId` as that record's member. Do not invent
the container's identity to make the example look complete. Existing neutral
unknown-region labels may retain offsets; that does not authorize offset-based
renaming of meaningful members.

If the importer cannot represent a verified nested record, union, array, or
inheritance relationship, submit an importer/type dependency. Extend the
representation through reviewed, serialized work rather than flattening away
the declaration or changing names to make validation pass. Restoring deleted
datatype references is a separate repair from semantic field mapping and does
not authorize unrelated renames.

## Files And Authority

The implementation is `<ghidra-scripts>/ApplyClassLayouts.java`.
Its sibling `ApplyClassLayouts.md` specifies commands, JSON syntax, supported
types, destructive semantics, and limitations. Layout files live in per-wave
directories under `<ghidra-scripts>/layouts/`.

Live target code and metadata establish facts. Existing Ghidra types and donor
declarations are evidence to investigate, not automatic truth. The accepted
JSON file is the intended complete field-definition layout for its class.
Preview and post-commit audit establish whether the database matches it;
neither establishes that its semantic claims are correct.

MPR is x86 and the target is x64. Never copy donor offsets, double offsets, or
assume matching names establish matching layouts. Read the focused MPR class,
base/nested declarations, emitted methods, and relevant enum owners.

## Bootstrap

1. Save a program backup and identify the live program, image base, ABI, and
   analysis state. Pause unrelated datatype/function mutations during collection.
2. Export the current Auto structures into a fresh directory. Retain completion
   status; an interrupted export is not a full inventory.
3. Inventory class identity, size, inherited/embedded dependencies, unknown
   regions, and invalid types. Do not hardcode 650 as verified coverage: donor
   packets, DTI classes, vtable owners, and actual datatypes are different sets.
4. First preview/audit unchanged supported class files. Preserve all existing
   names, comments, bitfields, and developed layouts before refinements.
5. Investigate invalid components through direct Ghidra datatype inspection.
   A `-BAD-` report alone does not identify a replacement. A deleted old type may
   leave stale references even when a same-named canonical type exists.
6. Pilot one reviewed fixed-size replacement on a program copy. Verify final
   identity, layout, dependent classes, and decompiler interpretation before a
   broad wave.

Packed types, missing canonical structures, and size disagreements are separate
dependencies in v1. Do not clear, flatten, or resize them to get past validation.
`ApplyDtiInheritance` offers separate inheritance/size preparation, but its
parent-wins policy requires its own preview and review; it is not an automatic
prerequisite to run across the whole database.

## Parallel Assignments

Use explicit full datatype paths and exclusive JSON file ownership. Start with
one complex class or a small related group per worker; large classes may instead
be split into exclusive research regions, with one designated file editor.
Twenty Luna researchers are possible; twenty concurrent datatype applicators
are not the workflow.

Supply each assignment with the focused MPR declaration packet and current MVC3
layout, including relevant base/nested/resource/particle-owned enums and target
anchor functions. For a new class family or substantial record reconstruction,
require a small mapping proposal before broad JSON edits: donor member paths,
target locations, proposed record/array boundaries, anchors, and exceptions.
The coordinator reviews that correspondence before scaling it across classes.

**Parent-first is mandatory, not conditional on whether a base looks uncertain.**
Before assigning a class, consult `<ghidra-scripts>/parents.txt` and walk
its entire target ancestry to the root. Schedule every unfinished ancestor in
root-to-leaf order before starting the child's field-mapping work. For example,
the recorded `uAmaterasu` chain requires this order:

```text
MtObject -> cUnit -> uCoord -> uBaseModel -> uModel -> uToonModel
    -> uActorGameModel -> uActorShareModel -> uCharacter -> uAmaterasu
```

Check each ancestor's class notes, reviewed layout, and current target state.
An ancestor satisfies the research prerequisite when the coordinator has accepted
its target-supported donor mapping, stable size/base boundaries and relevant
fields, with remaining unknown regions explicitly documented. The accepted layout
need not be applied live before child research begins. Record the exact reviewed
layout version used, distinguishing proposed parent fields from current live
fields. A previously accepted unchanged layout may be reused after checking for
target drift; do not redo it solely for another child.
Existence of a struct, plausible names, an exported JSON file, or `resolved` in
`parents.txt` is not completion. That status describes ancestry resolution, not
field recovery. Completion does not require inventing names for every byte, but
unresolved ownership or base boundaries block dependent child work. An unrelated
unknown region within an otherwise established parent does not block descendants.

Record the ancestry and completion evidence or blockers in the assignment note.
An unfinished parent becomes a prerequisite assignment; the child remains
`deferred_dependency` until the parent's research prerequisite is satisfied.
Do not let child workers silently edit unassigned parent files. Give
each shared parent one owner, then release independent descendants in parallel
after the ancestor research gate passes. If an accepted parent proposal changes,
re-review affected child mappings against that version. If live parent state
changes, obtain fresh dependent exports and reconcile pending edits and guards;
never rewrite fingerprints merely to pass validation.

Application has a separate gate: apply required parent changes before children
and verify the committed parent layouts in the live target before applying the
dependent children. Preview children against the resulting target state. Pending
parent application blocks child application, not otherwise sound child research.

Treat the ancestry file as target evidence, not proof of all C++ base-subobject
offsets or secondary bases. Check its identity against the current target and
donor declarations. Missing rows, unresolved chains, cycles, identity conflicts,
or additional target-supported bases require investigation before release;
absence from the file is not proof that a type is a root.

A field discovered in a derived method
belongs in the actual base or embedded record when evidence establishes that
ownership. Submit a dependency to its assigned worker rather than copying the
field into every descendant or editing someone else's file.

Workers may read neighboring functions, classes, and reference documents. They
must not mutate live datatypes, enums, global data, signatures, or functions as
part of this assignment. Separate function-renaming campaigns can resume for
stable class families after a wave is applied and evidence is refreshed.

## Per-Class Protocol

### Mandatory Donor Retrieval

Before proposing field names or layout edits, retrieve and read the assigned
class's full stored JSON declaration using the existing reference tool. Run from
`/media/god/tempData/Lostplanet/mpr_reference/`, substituting the actual class:

```bash
python3 query.py class cSPCData --section all
```

Omitting `--donor` returns matching declarations from both donors separately.
Use `--donor MPRCenter` or `--donor MPRSatellite` to focus subsequent queries;
retain donor/type IDs and document relevant variant differences. The default
summary, a fields-only query, and existing target labels are not substitutes for
reading the full declaration. No new bulk export is required.

`--section all` returns the stored class record, not a recursive dependency
packet. Follow its relevant references explicitly with
`python3 query.py type <donor:tpi:0xINDEX>` (replace the placeholder with the exact
returned type ID), or `class <name> --donor <donor> --section all` for a named
class. Read bases, embedded/nested records, array element declarations, typedef
chains, and relevant enum owners before mapping affected members. Include enums
owned by resource and particle record types where relevant. Follow pointer
pointees when needed to establish the mapping, not recursively through the
entire engine. Reuse already-read declarations within the assignment.

Record the consulted class/type IDs and relevant missing or unparsed declarations
in the worker note. If a dependency cannot be resolved, leave the affected mapping
unresolved and report the blocker; do not infer a semantic name merely because
the reference was not retrieved. Retrieval supplies donor evidence, not proof of
target correspondence.

### Property Registration First

After reading the donor declaration, locate the target `createProperty` for the
class (including unnamed candidates reached through its property-builder virtual
slot or callers). Confirm ownership and compare the registered strings/member
names with the donor declarations. An existing generated function name alone
does not establish ownership. If the names match, **start target offset recovery
from that builder before broadly searching constructors and accessors**.

Trace each matching registration to the actual target member address. Record the
registered name, donor member path, builder and registration call addresses,
receiver adjustment, resulting owner-relative byte offset, and native storage
width where established. A verified direct registration of `this + offset` for
a matching stored member is primary target offset evidence; use constructors,
readers/writers, and allocation/DTI to corroborate type, extent, and ownership,
not to rediscover an already established offset. Do not delay recording such
verified offsets just because other members remain unresolved.

Inspect the registration helper's argument roles before interpreting constants
as offsets or widths. Distinguish direct storage addresses from getter/setter
callbacks, counts, flags, type IDs, UI values, and computed properties. A matching
property name without a verified storage binding is a candidate, not a verified
field offset. Follow callbacks to their native accesses where necessary; reflected
type codes alone do not establish physical storage width or enum identity.

Follow base-builder calls and nested-record/array loops. Attribute fields to the
actual declaring owner; preserve container names, index/stride relationships,
and complete-object versus adjusted-receiver offsets. Do not flatten repeated
property labels or copy inherited fields into the child. Note aliases where
multiple labels refer to the same storage. If no usable matching builder exists,
record that finding and proceed with other target anchors; do not fabricate a
match or let absence of reflection block unrelated recovery.

### Mapping Steps

1. Pass the parent-first gate for every ancestor, recording completion evidence
   or deferring the class for prerequisite work. Complete mandatory donor
   retrieval, including declaring owners, member
   order, bases, nested records, arrays, and relevant enums. Compare them with
   the existing class reference under `doc/ghidra_classes/`, the exported target
   full-path layout, and its dependencies. Do not begin by inventing names for
   the target's flattened placeholders.
2. Apply the property-registration-first rule when a matching target builder
   exists, recording verified direct member bindings and unresolved callbacks.
   Establish target size and base boundaries from constructors, DTI, allocation,
   property registrations, and native accesses. Separate complete-object and
   embedded-relative offsets. DTI registration alone does not prove secondary
   bases or arbitrary zero-offset embeddings.
3. Map donor member paths to target storage using strong anchors and coherent
   declaration blocks. Validate reads/writes, widths, array indexing/strides,
   initialization, cleanup, and callers. Preserve donor names and containment
   where correspondence holds. Distinguish computed properties from storage and
   pointers from their pointed-to records. Record target-only members and
   unresolved mappings separately; do not force a match through contradictions.
4. Refine only supported regions. Keep untouched exported fields and comments.
   Preserve unknown bytes explicitly or as undefined gaps; do not call them
   padding without evidence. An omitted field is a destructive removal.
5. Apply target-width enum types to verified field mappings in the proposed
   default layout. Include source IDs, underlying types, aliases, combined masks,
   numeric values, and related enums outside the immediate class. Unverified
   mappings remain integer/unknown storage with a documented candidate.
6. Record donor provenance, member-to-target correspondence, and succinct target
   evidence in JSON's supported `evidence`/`notes` fields or a compact mapping
   table in the worker note. Include class-document findings. Coordinate shared
   class-document edits rather than racing them; do not invent unsupported JSON
   schema keys for the mapping.
7. Self-review before submission. Challenge ownership, widths, overlap, base
   versus derived fields, shared/folded accessor semantics, and types inferred
   from existing casts. Check allocation/constructor/DTI evidence when types
   disagree. Review the entire file diff, including accidental field omissions.
   Ask: "Am I mapping a donor declaration, or merely making a label unique so
   the importer passes?" Reject suffix-based duplicate fixes, needless synonyms
   for mapped donor names, and flattened representations that discard verified
   record/array structure. Self-review does not replace coordinator review.
8. Submit counts and paths. Leave `review` unapproved; the coordinator reviews
   semantic evidence and marks accepted files `approved` for preview/apply.

Do not modify baseline hashes or dependency inventories while researching.
If live state drifts, the coordinator must obtain a fresh export and merge the
actual differences. Never refresh fingerprints blindly to force application.

## Applying A Wave

1. Coordinator reads every file delta and worker exception note. Independently
   inspect code for precise new semantics, ownership changes, enum mappings,
   deleted-reference repairs, pointer targets, and inherited/embedded boundaries.
   Review the donor-to-target mapping itself, not just the uniqueness of names
   or successful validation. Reject invented names for mapped donor members and
   offset/occurrence suffixes used instead of recovering containment.
   Check the worker's consulted donor class/type IDs and dependency exceptions.
   Return mappings for further research when full declarations or relevant type
   follow-ups were skipped; do not approve them from a summary or field list alone.
   Recheck the entire ancestry research gate against the accepted parent layout
   versions; unresolved or changed prerequisites require dependent mapping review.
   Research approval does not authorize application against unapplied parents.
2. Mark accepted files `approved` and preview the selected wave. Inspect every
   removal, width/type replacement, description change, and enum coverage report.
    The script checks geometry and stale state, not research correctness.
3. Apply through the single writer. The script rebuilds component definitions
   inside existing fixed-size datatype objects, in by-value dependency order.
   Unselected composite layouts must remain unchanged. Exceptions roll back the
   batch; shared structures are never edited independently by worker agents.
4. Wait for the enclosing Script Manager transaction to finish, then run audit.
   `VERIFIED_IN_TRANSACTION` is not committed success. Require `ALREADY_CORRECT`
   for the selected layouts before counting application as verified.
5. Inspect live decompilations of representative constructors, accessors,
   writers/readers, and affected callers. Check actual enum constant rendering.
   Existing stale function signatures may need a separately assigned follow-up.
6. Merge accepted class-document findings serially and refresh affected packets
   before scheduling the next dependent wave.

## Lightweight Reporting

Maintain the single shared-class status source at
`/media/god/tempData/Lostplanet/mpr_reference/mvc3/class_status.json` using
`class_status.py` in the reference directory. See `CLASS_STATUS.md` there for
query/update commands, ancestry gates, and reviewed byte-coverage records.
Use `summary`, `show <class>`, `list`, and `next` for readable views; there is no
separate checklist or regeneration step. Coverage is computed from verified
ranges, not named placeholders. Track padding separately and leave unassessed
coverage explicit. Coverage percentages do not replace semantic review or audits.
Record research acceptance separately from committed live application, with
completion evidence and parent layout versions. Initial `unverified` statuses
mean completion has not been established, not that no previous work exists.
Historical evidence links are research leads, not automatic completion credit.
Update statuses after coordinator review and post-commit verification, and
invalidate affected statuses when accepted dependencies or live state change.

Use a compact per-worker TSV:

```text
class_path	layout_file	outcome	mapped_donor_fields	target_only_fields	unresolved_mappings
```

Research outcomes: `ready_for_review`, `already_complete`, `partial`,
`deferred_dependency`, `ambiguous`, `conflict`, or `analysis_failed`.
Keep candidate mappings and exact blockers in a short companion note. Do not
build a separate JSON reporting framework or repeat long evidence essays.

Coordinator application status is separate: `approved`, `pending_apply`,
`pending_commit_verification`, `verified_applied`, or `rejected`.
Count researched classes, reviewed fields, committed changes, enum field paths,
remaining unknown regions, and blocked classes separately. A class is not
complete merely because every byte has a field name.
Count a member declaration once rather than once per repeated array element or
inherited appearance in descendants. Name uniqueness and placeholder removal
are not semantic coverage metrics.

## Worker Prompt

```text
Research the assigned class layouts for umvc3.exe.unpacked.exe.
Read ai_mass_ghidra_fields.md, ai_mass_ghidra_doc.md section 10.1, and
<ghidra-scripts>/ApplyClassLayouts.md.

Exclusive datatype paths and JSON files:
<EXPLICIT LIST>

Before starting child field mapping, consult <ghidra-scripts>/parents.txt
and check every ancestor, root first. Record the exact accepted parent layout
versions and supporting target evidence. The file's resolved status means
ancestry resolved, not fields recovered. Every unfinished parent must first have
reviewed, stable boundaries and relevant field mappings; return deferred_dependency
until that research gate passes. Parent proposals need not be applied live before
child research, but distinguish proposed fields from live fields. Unrelated
documented unknowns do not block children; unresolved base boundaries or ownership
do. Required parent changes must be applied and verified live before child
application. Do not edit an unassigned parent or duplicate its fields in
descendants. Report missing/conflicting chains rather than assuming completion.

Mandatory before field naming or layout edits: from the MPR reference directory,
run query.py class <assigned donor class> --section all and read the full JSON.
Without --donor it returns both donor variants separately; retain their IDs and
document relevant differences. Follow relevant type IDs with query.py type <ID>
and named classes with class <name> --donor <donor> --section all. Read bases,
embedded/nested records, array element types, typedef chains, and relevant enum
owners, including resource/particle records. The default summary or fields-only
output is insufficient; --section all does not recursively fetch dependencies.
Record consulted class/type IDs and missing/unparsed dependencies in your note.
Leave affected mappings unresolved when required declarations cannot be retrieved.

Your primary task is mapping MPR declarations to MVC3, not inventing field names.
After donor retrieval, check the target class's createProperty first. If its
registered names match the class, start recovering target offsets from those
registrations before broad constructor/accessor searches. Confirm builder owner,
helper argument roles, direct storage binding and receiver adjustment; record
each matching member's target offset and evidence address. Follow callbacks,
base builders and nested array loops rather than treating computed properties,
type IDs or repeated labels as direct fields. Use other native accesses to
corroborate widths/types/ownership. If no usable matching builder exists, record
that and continue with other anchors. Name matches alone are not verified offsets.
Use the donor's member names, declaring owners, nested records, arrays, bases,
and enums once target correspondence is established. Build mappings from donor
member paths to exact target owner/offset/width/type, with provenance and target
anchors. Validate coherent declaration blocks and explain ABI or build changes;
do not copy x86 offsets or assume one class-wide offset adjustment.

Before broad edits for a new family or record reconstruction, submit a compact
mapping proposal with record boundaries, array dimensions/strides, and exceptions.
Duplicate flattened names require containment recovery, not offset or arbitrary
occurrence suffixes. Preserve mAnmId inside its verified record rather than using
names such as _5_mAnmId__at_3e918. Request importer support if the correct layout
cannot be represented. Name invention is reserved for verified target-only
members; distinguish those from unresolved donor mappings.

Preserve stronger existing target work and report conflicting donor variants.
Generated owners and decompiler casts are not independent evidence. Include
relevant enums outside the immediate class and apply verified mappings at the
actual target storage width.

Edit only your assigned JSON files and worker checkpoint/note. Do not modify
live Ghidra or shared documentation. Do not approve your own files, alter baseline
hashes, or remove dependency guards. Keep unknown regions honest. Report base,
shared-type, size, and invalid-reference dependencies to the coordinator.

Self-review the full diff and evidence, especially omitted fields, complete-object
offsets, native widths, base boundaries, conflicting type casts, and enum mappings.
Challenge whether each name recovers a declaration or merely satisfies validation.
Return one result per class, separate mapped-donor/target-only/unresolved counts,
unresolved issues, and output paths. Successful parsing is not semantic recovery.
```
