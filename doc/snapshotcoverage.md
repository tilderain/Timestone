# Snapshot Field Coverage

**Status: Implementation plan.** This design uses the bytes and region manifest
already stored in each rollback snapshot. It does not maintain a second list of
what the snapshot is expected to cover.

## Goal

For a selected live class instance, show every known `MtProperty` field and
answer:

1. Was the field present in the saved snapshot?
2. If it was present, did the field contain the saved value after restore?
3. Does the field change during normal frame-to-frame capture, and how often?

The result should make snapshot omissions and restore gaps visible immediately:

| Field | Offset | Size | Activity | Capture | After restore |
|---|---:|---:|---|---|---|
| `mPos` | `+0x50` | 12 | Changes every frame | Covered | Match |
| `mState` | `+0x84` | 4 | Intermittent | Covered | Mismatch |
| `mTimer` | `+0xA0` | 4 | Stable (240 frames) | Missing | Not compared |

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
- Per-field capture history for the selected owner.

The initial implementation audits one selected instance, not every property in
the process. This keeps the normal per-frame snapshot path cheap and makes the
result unambiguous when several fighters share the same concrete class.

The existing tracked property owners can populate an instance selector. Reject
an owner when it is no longer readable or its vtable no longer matches the
selected class.

Reset field history when the selected owner changes, the match restarts, frame
numbers move backward outside a restore comparison, or the user presses a
`Reset activity` button. A different instance at the same address must not
inherit the previous object's statistics.

## Frame Activity

Track activity from the bytes already written to consecutive snapshots. Do not
perform a second live-object sampling pass.

For each fully covered, sized field in the armed instance, retain:

```cpp
struct FieldActivity {
    uint32_t observedFrames;
    uint32_t changedFrames;
    uint32_t consecutiveStableFrames;
    int32_t firstObservedFrame;
    int32_t lastObservedFrame;
    int32_t lastChangedFrame;
    uint64_t lastValueHash;
};
```

After packing a new snapshot, hash the field bytes in `slot.data`. Compare that
hash with the previous capture for the same `(owner, offset, size, name)` and
update the counters. The first observation establishes a baseline and does not
count as either changed or stable.

Only compare adjacent forward frames. If the new frame is not
`lastObservedFrame + 1`, establish a new baseline so rollback slot replacement,
manual restore, frame stepping, and match transitions do not look like ordinary
frame-to-frame changes.

Use the following display classifications:

| Classification | Meaning |
|---|---|
| `Unobserved` | No usable captured value yet |
| `Baseline` | One captured value; no comparison yet |
| `Stable (N)` | No changes in the last `N` adjacent captures |
| `Changes every frame` | Every comparison in the current observation window changed |
| `Frequently changing` | At least half of comparisons changed |
| `Intermittent` | Changed at least once, but in fewer than half of comparisons |
| `Changed this frame` | Optional immediate marker applied in addition to the long-term class |

Show the exact ratio in a tooltip, for example `37 / 120 compared frames changed;
last change: frame 442`.

`Stable` means only that the bytes did not change during the observed interval.
It must not be labeled immutable, constant, safe to omit, or irrelevant to
rollback. Mode flags, round state, timers, and cached references may remain
stable for hundreds of frames and still be required when an event occurs.

Hashes are an optimization for activity detection. When a hash differs, a raw
byte comparison may confirm the change before incrementing `changedFrames`.
For fields in this feature's small selected-instance set, retaining the prior
raw value is also acceptable and eliminates collision ambiguity.

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
9. For each fully covered field, update its frame-activity history from the
   saved bytes.

This matching must use `slot.regions`, not a fresh call to `BuildRegions()`.
Dynamic ownership and object lists may have changed by the time the UI reads the
result, while `slot.regions` describes exactly what that frame captured.

Deduplicate fields by `(owner, offset, size, name)`. Property creation can be
observed more than once for the same instance.

Activity is independent of restore status. A covered field can be stable and
still mismatch after restore; that combination is especially useful because it
points to restore logic altering or preserving a value that capture expected to
remain unchanged.

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
- Add an `Activity` column with stable-frame count or change classification.
- Add `Capture` and `After restore` columns.
- Add a `Problems only` checkbox that keeps `Missing`, `Partial`, `Mismatch`,
  `Unreadable`, and unknown-size fields visible.
- Add activity filters: `Changed this frame`, `Ever changed`, and `Stable`.
- Add a `Reset activity` button.
- Show summary counts: covered, missing, partial, matched, and mismatched.
- Show activity summary counts: changed this frame, ever changed, and stable in
  the current observation window.
- For a mismatch, show saved and restored bytes in a tooltip. Use typed values
  for known scalar types and hexadecimal bytes for vectors, matrices, strings,
  and references.

Suggested colors:

- Green: covered and matched.
- Red: missing or mismatched.
- Yellow: partial, unreadable, or unknown size.
- Gray: not audited or accessor-backed.

Use a separate activity marker or text color so coverage and activity remain
distinct. A red `Missing` field must not become visually harmless merely because
it has been stable in the short observation window.

## Files

### `UMVC3Hook/code/dtieditor.h`

- Port the stashed `CapturedProperty` metadata fields.
- Declare the audit visitor and result APIs.

### `UMVC3Hook/code/dtieditor.cpp`

- Port the stashed property descriptor decoding, class attribution, and offset
  validation.
- Add field-size lookup, selected-instance state, result storage, filters, and
  table columns.
- Track adjacent-frame field activity and reset it when the selected instance or
  timeline changes.

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
10. Hold a field constant for several captures and confirm it reports
    `Stable (N)` with an increasing count.
11. Change a field on one frame and confirm it reports `Changed this frame`, its
    changed-frame count increments once, and it later becomes `Intermittent`.
12. Restore or jump the timeline and confirm the next capture becomes a new
    baseline rather than an artificial frame-to-frame change.

## Initial Scope

The first implementation intentionally does not attempt to name unreflected
bytes, follow pointer graphs, compare peers, or classify every specialized
restore policy. It reports only what can be proven from two existing sources:

- Known fields captured by the DTI editor.
- Actual regions and bytes stored in the selected snapshot slot.

That is enough to distinguish the two primary failure classes quickly: a known
field was not captured, or it was captured but did not survive the complete
restore path.
