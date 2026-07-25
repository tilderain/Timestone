# Snapshot Field Coverage

**Status: Implementation plan.** This design uses the bytes and region manifest
already stored in each rollback snapshot. It does not maintain a second list of
what the snapshot is expected to cover.

## Goal

For a selected live class instance, show every known `MtProperty` field and
answer:

1. Was the field present in the saved snapshot?
2. If it was present, did the field contain the saved value after restore?

The result should make snapshot omissions and restore gaps visible immediately:

| Field | Offset | Size | Capture | After restore |
|---|---:|---:|---|---|
| `mPos` | `+0x50` | 12 | Covered | Match |
| `mState` | `+0x84` | 4 | Covered | Mismatch |
| `mTimer` | `+0xA0` | 4 | Missing | Not compared |

## DTI Editor Prerequisite

Use the property metadata work from `stash@{1}` (`rollback debug settings and
dtieditor tweaks`), but do not apply the entire stash. It also contains unrelated
rollback and collision experiments.

Port only the relevant `dtieditor` changes:

- Decode the property descriptor with the explicit `MtPropertyView` layout.
- Store the declaring `className`, property `flags`, validated `offset`, and
  `offsetValid` in `CapturedProperty`.
- Attribute a property from the relocated `createProperty` caller table, with
  the owner's vtable class only as a fallback.
- Treat `mFieldAddr` as a real instance field only when it forms a valid
  non-negative offset from `ownerPtr` and falls inside the selected DTI size.
- Keep accessor-backed properties in the editor, but mark them as having no
  comparable storage.

The stashed JSON export is not required for this feature.

## Field Sizes

Add one helper in `dtieditor.cpp` that maps known property types to their byte
sizes:

| Type | Size |
|---|---:|
| `PROP_BOOL` | 1 |
| `PROP_U32` | 4 |
| `PROP_S32` | 4 |
| `PROP_F32` | 4 |
| `PROP_CLASSREF` | 8 |
| `PROP_STRING` | 8 |
| `PROP_MATRIX` | 64 |
| `PROP_VECTOR3` | 12 |

`PROP_CLASS` and unknown/custom types should initially be reported as
`Unknown size` and not byte-compared. This is preferable to guessing a size and
reporting false coverage.

## Audit Target

Add a small DTI editor audit state containing:

- Selected class hash.
- Selected live owner address.
- Whether snapshot auditing is armed.
- Frame number of the latest captured audit.
- Results from the latest restore.

The initial implementation audits one selected instance, not every property in
the process. This keeps the normal per-frame snapshot path cheap and makes the
result unambiguous when several fighters share the same concrete class.

The existing tracked property owners can populate an instance selector. Reject
an owner when it is no longer readable or its vtable no longer matches the
selected class.

## Snapshot-Side Data

Add a field audit vector to `SnapshotSlot` in `rollback.cpp`:

```cpp
enum class FieldCaptureStatus : uint8_t {
    Covered,
    Partial,
    Missing,
    UnknownSize,
};

enum class FieldRestoreStatus : uint8_t {
    NotCompared,
    Match,
    Mismatch,
    Unreadable,
};

struct SnapshotFieldAudit {
    const char* className;
    const char* fieldName;
    uint16_t type;
    uintptr_t owner;
    uintptr_t address;
    uint32_t fieldOffset;
    uint32_t fieldSize;
    int regionIndex;
    size_t regionDataOffset;
    FieldCaptureStatus captureStatus;
    FieldRestoreStatus restoreStatus;
};
```

The names are game-owned static property strings. Do not store pointers into
`g_props`, because that vector can reallocate as more properties are captured.

`regionDataOffset` is the offset of the field's first byte in `slot.data`, not
just its offset within the region. A fully covered field can therefore be
compared directly against the authoritative saved bytes.

## Capture Flow

At the end of `Rollback_SaveSnapshot`, after `slot.regions` and `slot.data` have
been populated:

1. Clear `slot.fieldAudits` when the slot is reused.
2. If auditing is not armed, stop without doing any property work.
3. Ask the DTI editor for known fields belonging to the selected owner, including
   inherited fields already collected by the editor.
4. Ignore fields with invalid offsets. Display them separately as
   `Accessor/no address` in the editor.
5. For each sized field, intersect `[fieldAddress, fieldAddress + fieldSize)`
   with the actual regions stored in this `SnapshotSlot`.
6. If one region contains the entire field, record `Covered`, its region index,
   and its exact offset in `slot.data`.
7. If snapshot regions contain only some bytes, record `Partial`.
8. If no snapshot region intersects the field, record `Missing`.

This matching must use `slot.regions`, not a fresh call to `BuildRegions()`.
Dynamic ownership and object lists may have changed by the time the UI reads the
result, while `slot.regions` describes exactly what that frame captured.

Deduplicate fields by `(owner, offset, size, name)`. Property creation can be
observed more than once for the same instance.

## Restore Comparison

Run the comparison at the end of `Rollback_RestoreSnapshot`, after all region
handlers, pointer preservation, concrete character restoration, projectile
reconstruction, collision reconstruction, and final fixups have completed.

For each audit entry:

1. Leave `Missing`, `Partial`, and `UnknownSize` fields as `NotCompared`.
2. Verify that the owner and complete field range are still readable.
3. For `Covered` fields, compare live bytes at `address` with the saved bytes at
   `slot.data + regionDataOffset`.
4. Record `Match`, `Mismatch`, or `Unreadable`.
5. Publish the completed result set to the DTI editor together with the restored
   frame number.

Do not compare inside the region restore loop. The current restore path
intentionally preserves pointers and applies later corrective ranges, so an
intermediate comparison would report values that have not reached their final
post-restore state.

The first version should report intentional pointer-preservation mismatches
rather than hide them. If these dominate real reports, add a pointer/reference
filter after observing actual output instead of introducing a speculative
policy system now.

## DTI Editor API

Keep `Region` and `SnapshotSlot` private to `rollback.cpp`. Add a narrow API to
`dtieditor.h` for the two modules to exchange fields and completed results:

```cpp
bool DtiEditor_IsSnapshotAuditArmed();
void DtiEditor_VisitSnapshotAuditFields(
    void (*visitor)(const CapturedProperty&, uint32_t fieldSize, void*),
    void* context);
void DtiEditor_SetSnapshotAuditResults(
    int32_t frame, const SnapshotFieldAudit* results, size_t count);
```

The exact result type may be moved to `dtieditor.h`, but snapshot storage and
lookup remain owned by rollback. Copy completed results into editor-owned
storage so the UI does not retain pointers to a ring slot that will be reused.

## UI

Extend `DrawPropertiesForDti()` rather than creating another debug window:

- Add an instance selector for tracked owners of the selected class.
- Add an `Arm snapshot audit` checkbox.
- Show the audited frame number.
- Add `Capture` and `After restore` columns.
- Add a `Problems only` checkbox that keeps `Missing`, `Partial`, `Mismatch`,
  `Unreadable`, and unknown-size fields visible.
- Show summary counts: covered, missing, partial, matched, and mismatched.
- For a mismatch, show saved and restored bytes in a tooltip. Use typed values
  for known scalar types and hexadecimal bytes for vectors, matrices, strings,
  and references.

Suggested colors:

- Green: covered and matched.
- Red: missing or mismatched.
- Yellow: partial, unreadable, or unknown size.
- Gray: not audited or accessor-backed.

## Files

### `UMVC3Hook/code/dtieditor.h`

- Port the stashed `CapturedProperty` metadata fields.
- Declare the audit visitor and result APIs.

### `UMVC3Hook/code/dtieditor.cpp`

- Port the stashed property descriptor decoding, class attribution, and offset
  validation.
- Add field-size lookup, selected-instance state, result storage, filters, and
  table columns.

### `UMVC3Hook/code/rollback.cpp`

- Add `SnapshotFieldAudit` storage to each `SnapshotSlot`.
- Build audits from the slot's saved regions after capture.
- Compare against `slot.data` after the complete restore.
- Clear audit records in `Rollback_FreeSnapshots()`.

No changes to `BuildRegions()` or the restore policies are required for the
first implementation.

## Verification

1. Build with `make -j8`.
2. Enter a match, select a class with captured properties, choose one live
   instance, and arm auditing.
3. Save and restore a snapshot through the existing frame-step/F6 path.
4. Confirm a field inside a flat snapshot region reports `Covered / Match`.
5. Confirm an accessor-backed property reports `Accessor/no address`.
6. Temporarily audit a known omitted field and confirm it reports `Missing`.
7. Temporarily alter a covered field after restore but before the final audit
   call and confirm it reports `Mismatch` with different saved/restored bytes.
8. Exercise a character restore and confirm the comparison runs after selective
   restore and pointer preservation, not during the raw body copy.
9. Leave auditing disarmed and verify no audit vectors are populated during
   ordinary snapshot capture.

## Initial Scope

The first implementation intentionally does not attempt to name unreflected
bytes, follow pointer graphs, compare peers, or classify every specialized
restore policy. It reports only what can be proven from two existing sources:

- Known fields captured by the DTI editor.
- Actual regions and bytes stored in the selected snapshot slot.

That is enough to distinguish the two primary failure classes quickly: a known
field was not captured, or it was captured but did not survive the complete
restore path.
