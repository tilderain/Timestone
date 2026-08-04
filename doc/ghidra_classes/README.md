# Ghidra Class References

This directory contains accumulated per-class reverse-engineering references
for the mass Ghidra documentation campaign defined in
[`../ai_mass_ghidra_doc.md`](../ai_mass_ghidra_doc.md).

Before analyzing a class-owned function, read the corresponding
`<Class>.md` file when it exists. While decompiling, record identifiable unnamed
fields, their complete-object offsets, types, access patterns, evidence, and
confidence. Keep uncertain findings separate from confirmed fields.

Create a class reference only after useful class information has been
established. Do not create empty placeholder files.

Minimum sections:

```markdown
# ClassName

**Ghidra type:** `ClassName`
**Known size:** `0x...`
**Base class:** `...`

## Confirmed Fields

| Offset | Size | Type | Name | Access | Evidence |
|---:|---:|---|---|---|---|

## Candidate Fields

| Offset | Size | Proposed type/name | Confidence | Evidence needed |
|---:|---:|---|---|---|

## Relevant Methods

| Address | Name | Relationship |
|---|---|---|

## Open Questions
```

The live Ghidra database and verified instructions are authoritative when a
class reference is stale. Preserve contradictory evidence until the conflict is
resolved rather than silently deleting it.
