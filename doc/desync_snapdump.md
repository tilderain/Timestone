# Desync Snapshot Dumps

Enable the code-only diagnostic switch before building:

```cpp
// UMVC3Hook/code/rollback_snapdump.h
#define ROLLBACK_SNAPDUMP_ENABLED 1
```

It is currently enabled with value `1`. Setting it to `0` removes the recorder
implementation and all integration calls during preprocessing. Such a build
performs no snapdump hashing, allocation, copying, branching, or desync file
work and has no snapdump history memory allocation.

The rollback plugin retains the last 128 packed rollback snapshots independently
of optional NDJSON logging. At the normal confirmed-checksum desync trigger, both
peers write a `desync_*_host_fN.usdp` or `desync_*_client_fN.usdp` file beside the
plugin DLL.

Each frame contains:

- The exact bytes in `RollbackSnapshotSlot::data`.
- The ordered region manifest, addresses, owner slots, expected vtables, region
  hashes, sizes, and restore flags.
- Local and remote inputs assigned to the snapshot.
- Shot, effect, collision-clone, and model-render-state counts.
- Whole-snapshot and manifest hashes.
- Stable construction-time names such as `globalRng`, `sAction.full`,
  `uCharacter.team0`, and `uCharacter.motionChannels`.

Re-simulation replaces the same absolute frame in the 128-slot history, so the
dump represents the corrected canonical timeline rather than stale prediction.
The on-disk payload uses XOR run-length encoding against the preceding frame.
At the expected 2-3 MiB per snapshot without projectiles, the in-process history
uses approximately 256-384 MiB plus dynamic shot/collision data. Each slot is
released when its absolute frame is overwritten or rollback state is reset.

The per-frame capture path performs one snapshot-byte copy and one compact
region-metadata pass. Slot storage is reused and grows in 1 MiB increments.
Snapshot hashes, per-region hashes, name expansion, manifest construction, and
XOR/RLE compression are deferred until the desync trigger has paused the match.

## Compare Peers

Run:

```sh
python3 tools/compare_desync_dumps.py
python3 tools/compare_desync_dumps.py host.usdp client.usdp \
  --output desync_report.json
```

With no paths, the comparator selects the newest complete
`desync_TIMESTAMP_{host,client}_fN.usdp` pair found in either the current
directory or `tools/../../ULTIMATE MARVEL VS. CAPCOM 3`. Both the timestamp and
trigger frame must match, so a partially written newer pair is ignored.
Supplying two paths retains explicit selection.

The command prints a short verdict and writes a complete JSON report suitable
for inspection by a person or AI. It exits `0` when all overlapping snapshots
match, `1` for a confirmed divergence, and `2` for malformed input or I/O errors.

The comparison performs three checks in order:

1. Cross-peer input agreement: host-local must equal client-remote and vice versa.
2. Snapshot structure agreement: ordered region identities, sizes, owner slots,
   expected vtables, restore flags, and dynamic recipe counts.
3. Byte agreement after normalizing ASLR image pointers and heap pointers that
   target the same snapshot region and offset.

The report identifies the first divergent frame, region, byte offset, values,
and surrounding bytes. Later divergent frames are summarized as propagation.
Version 2 dumps carry region names directly. The comparison tool remains
compatible with version 1 dumps and infers names for known singleton and
fixed-size character regions where possible.
By default, the comparator records qword slots containing different user-space
addresses in the shared frame-0 snapshot and excludes those exact `(region,
offset)` slots from every compared frame. This suppresses the allocation noise
exposed by the baseline capture without applying a general heap-delta rule or
discarding newly appearing pointer slots. Use `--no-skip-frame0-pointers` to
process those slots conservatively.

The same default-on override excludes complete
`uCharacter.externalAnimation` and `uCharacter.jointStates` regions. These
captured animation-wrapper and joint-work buffers are derived visual state and
may reflect process-local render timing. `--no-skip-frame0-pointers` restores
their conservative comparison along with the other default overrides.

Two `sAction`-labelled cases are also explicitly excluded. The real `sAction`
contains a 0x20-byte `cRef` at `+0x1f0`; its vtable, resource/reference token,
and trailing token at `+0x18` are process-local reference infrastructure and
remain unchanged within each peer across all 46 captured frames. Both the full
and compact duplicate regions skip this complete `cRef`. The broad
`sAction.full` capture also skips twelve unaligned pointers at
`+0x163c + n*0x20` for `n=0..11`. These locations are far beyond the real
0x250-byte `sAction`, belong to adjacent repeated records, and differ only by
the peers' allocation displacement. No actual `sAction` gameplay scalar is
covered by either rule.

The comparator also excludes complete `uCharacter.motionBlend` regions by
default. These per-joint interpolation work buffers are treated as visual-only
derived state rather than authoritative rollback state. Like the external
animation and joint-work exclusions, this semantic override applies to every
byte in each buffer even when no difference was present at frame 0.

Investigation of the first frame-2 differences identified three additional
pointer-only cases. `uActionSideCamera+0x18` is inherited `cUnit` pointer state:
the generic `cUnit` constructor clears it, and the peers acquire allocation-
displaced values (`0x0f1400a0` versus `0x0f1000a0`) at the same frame.
`uCharacter+0x13b0` is `cActorGameModelAnm+0x68`, the current animation-command
record pointer consumed by `cActorGameModelAnm__advanceCommandBlock`; it is a
resource pointer, not command progress. Finally, the high-offset differences in
this capture occur only on `uChunLi` (vtable RVA `0xa71f90`) and lie beyond its
DTI size `0x68f4`, so they are adjacent-allocation spill. The comparator excludes
the complete `[0x68f4, 0x6a60)` tail, gated by the concrete Chun-Li vtable rather
than applying the boundary to all character regions.

The inherited `uBaseModel+0x18c` dword in each character is also excluded by
default. `uBaseModel_vfn22` at `0x14055fdc0` atomically copies the counter at
`sRender+0x6764c` into this field, then calls `FUN_1405dcca0` for attached models
only when the stamp changes. `sRender__beginFrame` at `0x140537080` increments
the source once per rendered frame. It is therefore a process-local render-cache
invalidation stamp: all models rendered in one process share its sequence, while
peers can differ according to local presentation history. It does not drive
simulation or gameplay. The override covers exactly four bytes at `+0x18c` in
`uCharacter.team0` and `uCharacter.team1`; the neighboring reflected LOD fields
at `+0x184` and `+0x188` remain compared. As with the other semantic overrides,
`--no-skip-frame0-pointers` restores conservative comparison.

Two neighboring model-only ranges are excluded for both character teams as
well. `uBaseModel+0x190..+0x197` is model resource/cache bookkeeping populated
during material setup. `uToonModel+0x12d0..+0x12db` belongs to the embedded
toon-parameter coordinate object at `+0x12c0`; `uToonModel__renderToonShading`
consumes it to build lighting and shading parameters. Neither range is fighter
gameplay position or action state. Character-specific bytes, including the
unresolved in-object Chun-Li tail, remain compared.

Three additional full pointer slots are excluded for both character teams:
`uModel+0x100` is the retained model resource, `uModel+0x110` is the attached
model pointer table, and `uModel+0x540` is the resource-owned joint-number
table. Native motion evaluation uses the resources through these pointers, but
the pointers themselves are peer-local allocation addresses. Motion timing,
flags, event masks, and transform scalars remain compared and restored
separately.

The comparator also excludes the verified motion-resource binding slots in the
first `uModel::Motion` record (`+0x640`, `+0x660`, `+0x680`, `+0x688`, `+0x690`,
`+0x698`, `+0x6c0`, `+0x6d0`, and `+0x6d8`) for every motion slot, plus the
`uModel+0x11b8..+0x1237` motion-list pointer table. These are resource and
allocation pointers populated by `uModel__bindMotionEntryToSlot` and
`uModel__getMotionListEntry`; motion-slot scalar state remains compared.

The embedded `cActorGameModelAnm` pointer slots at `uCharacter+0x1360`,
`+0x1368`, `+0x1370`, `+0x1380`, `+0x1390`, `+0x1398`, `+0x13a0`, `+0x13a8`,
and `+0x13b0`, `+0x13e8`, `+0x13f0`, and `+0x13f8` are also excluded.
`cActorGameModelAnm__advanceCommandBlock`
and `cActorGameModelAnm__advanceActionTime` consume these as owner, command
block, command cursor, and resource pointers. Command indices, counters, stop
flags, animation numbers, and animation times remain compared; only the raw
peer-local addresses are normalized.

The constraint/share pointer slots at `uCharacter+0x1468`, `+0x1478`, and
`+0x1540` are excluded as well. They sit in the `mCnsShareList` and adjacent
constraint/model ownership area; `uCharacter__updateConstraints` and
`uActorShareModel__vfn_2_19` consume the referenced model/constraint resources.
The constraint count, joint numbers, modes, flags, and other scalar state are
not excluded.

Additional repeated pointer tables in the cChr body are normalized at their
full qword boundaries, including the clusters around `+0x1898`, `+0x28a0`,
`+0x2ab8`, `+0x2dc8..+0x2e00`, `+0x2f90..+0x309f`, and
`+0x34f0..+0x35ff`. The frame-7 differences occur only in their peer-local heap
address bytes. Non-pointer bytes before, between, and after these slots remain
compared to expose any scalar desync.

The final pointer-shaped offsets in this capture are also normalized at
`+0x3270`, `+0x3608`, `+0x3618`, `+0x3628`, `+0x3c40`, `+0x3db0`, `+0x6460`,
`+0x6740`, and `+0x67f0`. Their changed bytes form peer-local heap addresses;
adjacent four-byte values remain compared.

`sAction.full+0x3634` is also excluded. The full region is a deliberate 0x20000
broad capture for defeat replacement, while current Ghidra gives the real
`sAction` size as 0x250. The byte is outside both that object and the separate
0x300 `sAction.compact` restore region. `Rollback_RestoreSnapshot` restores only
the documented compact ranges for the latter, so this broad-capture spill is
diagnostic-only and cannot alter rollback simulation through the snapshot path.

The `uCharacter+0x133c` differences remain compared. Ghidra identifies the full
32-bit field as `uActorGameModel::mAnmSerial`, but executable behavior overturns
the earlier animation-generation interpretation. `cChr__cacheMainField72` at
`0x140036cd0` copies `sMain::mFrameTimer` from `sMain+0x48` into this field. The
native `sMain__move` store at `0x140521f62` derives that frame timer from local
QPC elapsed time and target FPS. This explains why the field starts equal at
`0x117`, then becomes host `0x130` versus client `0x12b` at frame 2 even though
all six characters have matching animation numbers and animation times.

The cached frame stamp is gameplay-relevant. The virtual getter at
`0x140036fd0` returns `mAnmSerial`; AnmChr shot creation passes it to
`uShot__setAnmchrKeyframeParams`, which stores it in
`uShot::mRequestAnmSerial` at `+0x211c`. `uShot__getShotId` at `0x140038410`
returns that value, and `sAnmchrParser__pushShotIntoRingBuffer` at
`0x1400378c0` uses it to deduplicate an eight-entry shot-ID ring at
`cChr+0x6434`. Hit/contact and child-shot paths also consume the resulting shot
identity. The ring compares IDs generated within one process, however, so
different absolute host/client stamps do not by themselves prove different
gameplay. The relevant invariant is that corresponding local spawn events
preserve the same equality and uniqueness relationships. The comparator keeps
the field visible because it cannot establish that relationship from one byte
difference alone.

Native online does not overwrite `mFrameTimer` with its synchronized input
cursor. Instead, the deterministic branch in `aAction__init` calls
`sMain__setFrameWait`, which fixes simulation delta and resets pacing drift while
leaving the absolute QPC-derived stamp process-local. The plugin reproduces that
setup through `Rollback_ApplyNetplayDeterminism`, but rollback adds an invariant
native delay netplay does not need: `Rollback_ResyncMainTiming` resets the local
QPC epoch after replay. In the `20260729_061431` dump, subsequent animation
transitions consequently receive host/client stamps such as `19/308`, `0/325`,
`329/0`, and `9/349`. Reusing low IDs while the AnmChr eight-entry ring still
contains earlier stamps can suppress a distinct shot as a duplicate.

Custom netplay therefore normalizes this field to its shared relative frame
timeline. `hkMainMove` overwrites `sMain+0x48` with the current net frame after
native timing and before the rest of `sMvc3Main__move` executes.
`Rollback_HeadlessTick` assigns `g_resimulateCurrentFrame` before historical
character gameplay. This preserves a stable, monotonic shot-event namespace
across live and replayed ticks without snapshotting the broader `sMain` body,
whose QPC, pacing, and process-local service state is not rollback state.

## Ghidra Field Annotations

Run `ExportSnapdumpStructs.java` from the Ghidra script manager against the
current `umvc3.exe.unpacked.exe` program. It writes:

```text
ghidra_export/snapdump_structs.json
```

The export is generated directly from Ghidra's current data-type manager and
executable. It includes every current structure, recursively flattened named
fields, and recovered DTI/vtable identities. Character and shot families are
also identified through current virtual methods. It does not consume the older
property JSON or TSV audits.

`compare_desync_dumps.py` loads that path automatically when present. Exact
saved vtables select concrete character, helper, and shot types. Stable region
bindings annotate partial singleton captures such as `sReplay.frameCounter`,
`sEffect.rng`, `sShot`, `sAction`, `sCamera`, and `cChrTeam`. Each differing run
then reports overlapping Ghidra field names, types, full object offsets, and
decoded values for known scalar types.

Use a different export or disable annotations with:

```sh
python3 tools/compare_desync_dumps.py host.usdp client.usdp \
  --structs path/to/snapdump_structs.json
python3 tools/compare_desync_dumps.py host.usdp client.usdp --no-structs
```

Heap arrays keep their explicit snapdump labels. The current Ghidra database
also binds `uCharacter.motionBlend` to the verified 0x68-byte
`RollbackMotionBlendRecord` element and `uCharacter.jointStates` to the verified
0xC0-byte `RollbackJointWorkRecord` element. These bindings come from native
allocation strides and access sites, not size-only guesses. Other arrays remain
untyped until an element layout is independently established.

## Pointer Classification Notes

The current conservative comparison treats pointer values as equal only when
both peers resolve them to the same executable RVA or the same captured region
and offset. The following classifications aid diagnosis but do not add further
normalization:

- `sShot+0x8`, `sAction+0x8`, and `sCamera+0x8` begin inherited `cSystem`
  `CRITICAL_SECTION` state. They are process-local synchronization bytes, not
  gameplay object identities.
- `uModel+0x100` is a retained model resource, `+0x538` is the allocated joint
  array, `+0x540` is a resource-owned joint-number table, and `+0x548` is an
  allocated joint index/depth table.
- Each `uCharacter.motionBlend` element is a per-joint 0x68-byte work record.
  Its track descriptors are initialized from immutable motion-resource entries.
  The comparator now excludes the complete visual interpolation buffer through
  the explicit default override described above.
- Each `uCharacter.jointStates` element is a 0xC0-byte joint record. `+0x08` is
  an optional constraint or attachment, `+0x14` is hierarchy depth, `+0xA1` is
  parent index, `+0xA4` contains hierarchy flags, and `+0xA8` points to the
  containing `uModel`. The owner pointer is diagnosable as self-relative, but is
  not assigned a special normalization rule.
- The four `uCharacter.externalAnimation` regions are 0xE0-byte
  `nCollision::cCollisionNodeObject`-derived wrappers owned by their captured
  character. Their owner, callback, and user-data fields retain raw comparison
  unless ordinary image-RVA or captured-region resolution already proves
  identity.
- `sAction.full` is not a 0x20000-byte `sAction`; current Ghidra gives `sAction`
  size 0x250. Bytes above that size overlap adjacent allocations, including
  `cChrTeam[0]` at `sAction+0x350`, `cChrTeam[1]` at `+0x610`, and `sShot` at
  `+0x5540`. High-offset differences therefore cannot safely receive `sAction`
  field names or pointer recipes.

The remaining singleton-header differences are non-gameplay state. `sShot+0x8`,
`sCamera+0x8`, and `sAction+0x8` are the inherited `cSystem` synchronization
tail. `uActionSideCamera+0x18..0x1f` is the inherited `cUnit` pointer slot; the
reported `+0x1a` byte is inside that pointer, not an independent camera flag.
The compact capture also excludes the native `sAction` cRef at `+0x1f0..0x20f`
and adjacent-allocation spill from `+0x250..0x2ff`. These bytes are not
restored by the compact gameplay ranges and cannot affect simulation through
the rollback snapshot path.

The first frame-0 `sAction` differences illustrate the distinction:

- `+0x08` is inherited `cSystem` synchronization state. It is real memory, but
  not a gameplay field.
- `+0x122` is the third byte of the eight-byte `mpActionParam` pointer at
  `+0x120`. The pointer targets an uncaptured heap resource, so its differing
  low byte is currently meaningful evidence of different resource allocation or
  identity, not a fake byte and not a standalone `sAction` scalar.
- `+0x1d2` is the third byte of the eight-byte `mpCamList` pointer at `+0x1d0`.
  The pointer is a real `sAction` member registered by `sAction::createProperty`,
  but its target is not currently captured with a proven resource identity.
- Bytes at `+0x1e0` and `+0x1f0..+0x1ff` are also inside the real 0x250-byte
  object, but belong to currently unresolved embedded/reference state rather
  than the broad `sAction.full` spill. They should remain visible until their
  substructure is identified.

Therefore the `sAction` report is not fabricated: offsets below `0x250` are
bytes from the real object. However, byte-level output must be interpreted with
pointer alignment and inheritance in mind. Only offsets beyond `0x250` are
  guaranteed to be adjacent captured objects rather than `sAction` fields.

After removing those non-gameplay fields, the current pair leaves only two
pointer slots: `sAction+0x120` (`mpActionParam`) and `sAction+0x1d0`
(`mpCamList`). The dump values confirm that only allocation addresses differ:
the pointers have the same low-level resource role and stable peer-local
displacement across the timeline. Ghidra confirms that `sAction::classFunc`
loads `boot\\actionparam` and `boot\\actioncam`, while
`rActionParam::convert` copies the immutable ACP image into persistent storage.
The resources themselves are meaningful, but these raw pointer bytes are not
their payload and are now normalized at the aligned qword slots. A future
resource-key or payload-hash diagnostic can replace this address-level rule
with direct content verification.

RE5 and Dragon debug layouts supply cross-version semantic evidence for the
joint and motion field order. Devil 4 confirms the surrounding `uModel` order
(`mJointNum`, `mJoint`, `mJointTable`, `mBlendNum`, `mMotion`, model resource,
shader resource, and motion-list resources), but its nested Motion and Joint
definitions are incomplete. All three references are 32-bit, so their raw
offsets are not imported into UMVC3's 64-bit structures.

## Action Parameter Resource

Current Ghidra analysis identifies `sAction+0x120` as
`rActionParam *mpActionParam`. `sAction::classFunc` requests the
`boot\\actionparam` resource, and `rActionParam::convert` copies the complete
resource into persistent allocator-5 storage after validating magic
`0x00504341` and version `0x20110223`. `rActionParam::getData` skips the
16-byte ACP file header and returns the immutable gameplay-parameter payload.
The meanings of header bytes `+0x08..+0x0f` remain unproven.

The current Ghidra helper `/UMVC3/Rollback/rActionParamPayload` is a sparse
0x80-byte layout. Its names come from current executable consumers rather than
historical tables:

| Payload offset | Type | Confirmed use |
|---:|---|---|
| `+0x00` | `float` | Damage converted to recoverable/red life |
| `+0x10`, `+0x14` | `float` | Successful-hit attacker and defender meter-gain rates |
| `+0x18`, `+0x1c` | `float` | Guarded-contact attacker and defender meter-gain rates |
| `+0x20`, `+0x24` | `int` | Grounded hitstun decay start and frames removed per step |
| `+0x28`, `+0x2c` | `int` | Airborne hitstun decay start and frames removed per step |
| `+0x30`, `+0x34` | `int` | Grounded blockstun decay start and frames removed per step |
| `+0x38`, `+0x3c` | `int` | Airborne blockstun decay start and frames removed per step |
| `+0x40` | `int` | Minimum computed hitstun |
| `+0x44`, `+0x48` | `float` | AnmChr `1_93` multipliers for previous actions `0x170` and `0x171` |
| `+0x4c`, `+0x50` | `float` | AnmChr `1_94` values selected for DHC counts one and two; destination semantics unresolved |
| `+0x54`, `+0x58` | `float` | Action-joint motion limit and per-frame signed step |
| `+0x5c` | `int` | Minimum counter enforced by mission-flight action selection |
| `+0x68`, `+0x6c` | `int` | Team assist/tag entry timing thresholds |
| `+0x70`, `+0x74`, `+0x78` | `int` | Attacker contact-event delay by defender grounded/crouching/airborne state |
| `+0x7c` | `float` | Team defeat/death transition duration used by gameplay and life-gauge UI |

The accessor has 108 references spanning hit and guard resolution, stun decay,
meter and recoverable-life calculations, DHC commands, action-joint movement,
team transitions, mission flight logic, and life-gauge display. The resource is
therefore immutable but gameplay-authoritative. Existing dumps capture only the
raw `rActionParam *`, not its resource key or payload, so they cannot prove that
host and client pointers refer to identical content. `mpActionParam` must remain
a meaningful difference until a stable resource identity or payload hash is
captured and validated.

`sResource::hashKey_dtiIdCRCPath` establishes the native logical identity used
by the resource cache. It removes one leading backslash, collapses repeated
backslashes, computes the normalized path CRC32, and returns
`(DTI.id << 32) | pathCRC32`. The loader stores this key at resource-object
`+0x60`, the original path in the 64-byte buffer at `+0x0c`, and the source file
size at `+0x58`. Thus both peers loading `rActionParam` from
`boot\\actionparam` should have the same native key despite different heap
addresses. The key alone is not sufficient for snapdump normalization: a
modified or reloaded ACP can retain the same DTI and path. A future compact
identity should include the native key and a hash of the immutable converted
bytes (plus byte length), and normalization should require all values to match.

## Verification

```sh
python3 -m unittest tools/test_compare_desync_dumps.py
make -j8
```
