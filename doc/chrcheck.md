Audit the complete UMVC3 x64 `cChr`/`uCharacter` object layout in Ghidra and determine which byte ranges contain authoritative gameplay state for rollback.
Repository context:
- Project: UMVC3Rollback
- Binary: `umvc3.exe.unpacked.exe`
- Current rollback restoration: `UMVC3Hook/code/rollback_restore.cpp`
- Relevant array: `CCHR_SCALAR_RESTORE_RANGES`
- Existing notes: `doc/umodel.md`, `doc/rollback.md`, `doc/chrfields.md`, `doc/sunit_categories.md`
- Some existing comments and documentation contain obsolete PPC-era or incorrectly converted offsets. Do not trust them without native x64 evidence.
- Actual `cChrInput` is embedded at `uCharacter+0x21C8`.
- Verified x64 base boundaries currently include:
  - `uCoord`: `0x100`
  - `uBaseModel`: `0x510`
  - `uModel`: `0x12C0`
  - `uToonModel`: `0x1300`
  - `uActorGameModel`: `0x1450`
Primary objective:
Produce a byte-range manifest for the entire common `cChr`/`uCharacter` object, from offset `+0x0000` through the end of the common class before character-specific subclass fields. Determine the exact class boundary from constructors, DTI allocation sizes, inheritance, and native access patterns rather than assuming it.
For every field or contiguous range, classify it as one of:
- Authoritative gameplay state that must be restored
- Derived gameplay state that should be recomputed
- Presentation/render state that should remain live
- Resource/allocation/ownership state that must remain live
- Intrusive scheduler/lifecycle state that must remain live
- Pointer requiring ownership-aware remapping
- Mixed range requiring subdivision
- Unknown or insufficient evidence
Required analysis:
1. Recover and verify the complete inheritance chain and byte boundaries:
   `MtObject -> cUnit -> uCoord -> uBaseModel -> uModel -> uToonModel -> uActorGameModel -> ... -> cChr/uCharacter`.
2. Apply the recovered structs to relevant functions in Ghidra. Do not leave a function parameter as an untyped pointer when a verified structure is available.
3. Identify every constructor, destructor, `createProperty`, setup, move, phase-2, phase-3, action, input, collision, and status function that reads or writes the object.
4. For each range, collect concrete evidence:
   - Native readers
   - Native writers
   - Constructor initialization
   - Destructor or release behavior
   - Property metadata
   - Whether it is accessed during headless resimulation
   - Whether it affects combat, input, movement, collision, RNG, action selection, health, meter, assists, tags, throws, hitstop, or animation timing
   - Whether it contains pointers, allocator headers, resources, vtables, intrusive links, render caches, or synchronization objects
5. Distinguish gameplay animation state from process-local model ownership. Motion number, frame, rate, root motion, and gameplay-facing animation flags may require restoration, while resource pointers, material caches, command caches, render stamps, draw bounds, and allocator-owned arrays must remain live.
6. Audit all current entries in `CCHR_SCALAR_RESTORE_RANGES`. For each entry, report:
   - Whether the entire range is safe
   - Whether it overlaps pointer or ownership fields
   - Whether it should be retained, removed, split, remapped, or recomputed
   - Exact replacement subranges when splitting is required
7. Specifically re-check these disputed ranges:
   - `+0x28..+0x2F`
   - `+0x48..+0x7F`
   - `+0x80..+0xFF`
   - `+0x100..+0x50F`
   - `+0x510..+0x55F`
   - `+0x560..+0x12BF`
   - `+0x1190` and `+0x1198`
   - `+0x1300..+0x144F`
   - `+0x14F0..+0x152F`
   - `+0x1548..+0x155F`
   - `+0x18A0..+0x18C7`
   - `+0x21C8` embedded input object
   - `+0x2738` and `+0x2950` command objects
   - `+0x27B0`, `+0x2892`, `+0x29C8`, and `+0x2AAA`
   - `+0x2D80..+0x2DBF`
   - `+0x3D38`
   - `+0x3F38..+0x40D3`
   - `+0x40B0..+0x41F7`
   - `+0x4D10..+0x4E0F`
   - `+0x6340..+0x645B`
   - `+0x6490..+0x670F`
   - `+0x67F8..+0x68FF`
8. Explicitly verify or disprove old labels such as `mAssistFlags` at `+0x29E`. Do not accept a name merely because it appears in project documentation.
9. Search for overlapping restore ranges. Flag redundant entries and cases where a narrow pointer-safe policy is later overwritten by a broader scalar copy.
10. Do not classify a field as gameplay merely because its value differs between snapshots. Establish that native simulation consumes it or that it is authoritative state.
Deliverables:
- Exact common-class size and inheritance map.
- Markdown table covering the entire object without unexplained gaps.
- Columns:
  `Start`, `End`, `Size`, `Field/Structure`, `Classification`, `Readers`, `Writers`, `Rollback Policy`, `Confidence`, `Evidence`.
- Separate audit table for every current `CCHR_SCALAR_RESTORE_RANGES` entry.
- List of confirmed documentation errors.
- Proposed corrected `CCHR_SCALAR_RESTORE_RANGES`.
- List of pointer fields requiring explicit preservation or remapping.
- List of derived gameplay fields requiring a native rebuild helper.
- List of unresolved ranges requiring runtime tracing.
- A concise final conclusion describing the minimum safe rollback policy.
Evidence requirements:
- Include native function names and addresses.
- Include exact object-relative offsets.
- Mark inference separately from confirmed evidence.
- Use `High`, `Medium`, or `Low` confidence.
- If evidence conflicts, preserve the conflict instead of choosing the convenient interpretation.
- Do not modify source code during this pass. Return analysis and proposed changes only.