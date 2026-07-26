### `sCollision` snapshot necessity

Ghidra confirms that `sCollision` is not immutable. `sCollision__move` at
`0x1405BAF60` mutates the singleton and allocation-backed state every tick:

- It drains the `SbcObject` dirty, merge, reactivate, and retire queues.
- It inserts, removes, and refits leaves in dynamic broadphase BVHs.
- It updates active-object arrays, cached bounds, and BVH-node pointers.
- It consumes deferred cluster operations and clears their counts.
- It runs skin-mesh and move-matrix work.
- It clears `sCollision+0x220`, sets `sCollision+0x221`, and increments the
  move-matrix frame cursor at `sCollision+0x7368+0x28`.

The deferred cluster rows occupy much of the range near
`sCollision+0x22230..+0x2396F`. `sCollision__drainNodeLists` processes pending
BVH insertion, removal, registration, release, and counter operations, then
zeros the corresponding row and aggregate counts. The `SbcObject` and
move-matrix subsystems similarly consume queues and update their persistent
registries and acceleration structures.

These writes prove that the state changes, but do not prove that the full
`0x239E0` singleton and all discovered external buffers belong in rollback
snapshots. Most observed state has one of these roles:

- Generic MT Framework scene, model, skin-mesh, particle, chain, or terrain
  collision.
- Derived broadphase trees and cached object bounds.
- Temporary registration and movement queues.
- Per-frame or per-worker dispatch state.
- Allocator, synchronization, and ownership infrastructure that must remain on
  the live timeline.

The replay tick calls `sCollision__move` again through the member at
`sMain+0x401A0`. Given restored characters and other collision-object owners,
this can deterministically refit or rebuild much of the derived broadphase
state. This explains why disabling the collision snapshot can survive ordinary
play without an immediate desync.

The unresolved risk is object lifecycle rather than ordinary cached bounds. A
rollback can still fail if it crosses registration, removal, replacement, or
collision-node topology changes that the replay cannot regenerate from the
restored owner state. Relevant stress cases include assists, projectile
creation and destruction, transformations, unusual movement, and rollback
across deferred queue activity.

Current conclusion: `sCollision` is heavily modified, but no static evidence
establishes that its complete state is authoritative MVC3 combat state. The
bulk snapshot is likely over-inclusive. Removing it is plausible but should be
validated with two-peer, rollback-heavy lifecycle and contact tests before it
is treated as proven safe.

### `sHitSolver` snapshot necessity

The full `sHitSolver` snapshot is even less justified by the recovered native
lifecycle. Its approximately `0x3808C`-byte body is primarily transient
per-frame hit-solving work:

- `+0x38` is an `MtArray` of pending `sHitSolver::cNode*` contacts.
- The large body through approximately `+0x30060` is a temporary node pool.
- `+0x30060` and `+0x30064` are pool/solver counters.
- `+0x30070` and `+0x34078` begin temporary sort/work buckets.
- Tail fields hold work partition counters used by the multithread path.

Producers such as characters and shots create candidate contact nodes during a
frame. `sHitSolver__move` at `0x140011D90` coalesces compatible contacts, sorts
them into resolution order, and invokes their callbacks. Durable hit results
are then represented by character, shot, team, gauge, and action state rather
than by retaining the solver's work list.

The current replay pipeline already reflects this lifecycle:

1. `MgrSubsysCall(FN_GETHITSOLVER, 0x30)` resolves pending contacts.
2. Character, stage, shot, and effect updates continue.
3. `FN_RESET_HITNODES` clears the active-node array and resets the node-pool
   count at the end of the replay tick.

Restore also explicitly calls `FN_RESET_HITNODES` and clears the counters at
`+0x30064`, `+0x30070`, and `+0x34078`. This was added because restoring
already-consumed active hit nodes replayed resolver callbacks and compounded
launch or pushback. Consequently, the previous bulk snapshot copied transient
work that restore subsequently discarded.

The preferred policy is therefore to preserve the live solver object,
allocator, array infrastructure, and configuration, while clearing transient
work and allowing replayed gameplay objects to regenerate contacts. The project
also forces `sHitSolver::mMultiThread` off, selecting the deterministic in-place
coalesce and shell-sort path rather than worker bucket dispatch.

This conclusion depends on snapshots being taken at a stable frame boundary.
If capture occurs after contact producers queue nodes but before the solver
consumes them, the pending contacts are logically part of that intermediate
state. Rawly copying their pointer-backed pool is still undesirable; capture
should instead move to the post-reset boundary or the contacts should be
regenerated explicitly.

Current conclusion: the full `sHitSolver` body should not be snapshotted in the
present frame-boundary design. Keep the explicit reset on restore and validate
the policy across hits, trades, throws, armor, assists, projectiles, multi-hits,
and hitstop. This supersedes older notes that marked the complete solver body as
mandatory rollback state.

### Other speculative snapshot-removal candidates

The following regions are candidates for removal or replacement with compact
ranges. Except where the current restore code already proves bytes are unused,
these are hypotheses rather than correctness claims.

#### High confidence: duplicate `sAction` `0x20000` region

`Rollback_BuildRegions()` currently captures `sAction` twice:

- A `0x20000`-byte region through the global at `0x140D47E68`.
- A later `0x300`-byte region through `sMvc3Mgr+0x48`, plus the separately
  owned model-joint allocation.

Both access paths resolve the same singleton. Ghidra shows that
`sAction__getInstance` simply returns the pointer at `0x140D47E68`, while the
recovered constructor and property list place the manager's meaningful body in
the first few hundred bytes. The `0x20000` capture has no corresponding
ownership-aware or range-limited restore case. It therefore falls through to a
flat `memcpy`, including pointer-bearing and likely unrelated memory far beyond
the known `sAction` body. The later `0x300` restore already applies only these
selected ranges:

```text
+0x028 size 0x0E8
+0x140 size 0x008
+0x150 size 0x008
+0x168 size 0x009
+0x218 size 0x004
+0x250 size 0x010
+0x268 size 0x034
```

The duplicate `0x20000` region is the strongest removal candidate. Retain the
compact `sAction` ranges and the dedicated model-joint payload until each field
is independently reduced. Removing the broad region also avoids the current
later `0x300` selective restore being preceded by an unsafe broad restore.

#### High confidence: unused portions of the full `sMain` image

Snapshot capture copies `0x40080` bytes from `sMain`, but restore deliberately
uses only five small ranges totaling approximately 180 bytes:

```text
+0x0030 size 0x064
+0x00A4 size 0x01C
+0x40010 size 0x010
+0x40028 size 0x018
+0x40048 size 0x014
```

The omitted body contains regular-job records, a 4096-entry asynchronous job
ring, worker descriptors, handles, synchronization objects, and queue cursors.
Those fields intentionally remain owned by the live timeline. Since restore
does not read their saved bytes, packing them is unnecessary under the current
policy. `sMain` capture should eventually emit the five explicit ranges instead
of copying approximately 256 KiB merely to discard nearly all of it.

The selected timing ranges themselves are a separate question. Custom netplay
already derives important fight clocks from the synchronized simulation frame,
and resimulation may be able to pin or recompute delta/time fields rather than
rewind wall-clock samples. Do not remove those selected ranges until pause,
slowdown, training reset, hitstop, and timeout behavior has been tested.

#### High confidence: unused portions of `cChrTeam`

Each team currently contributes a full `0x2C0` bytes to the packed snapshot,
while restore preserves list topology and applies only:

```text
+0x048 size 0x008  active-character pointer, with live-roster validation
+0x050 size 0x0C0
+0x110 size 0x1B0
```

This leaves the vtable, intrusive-list head, and initial ownership fields on the
live timeline. The unused prefix can be omitted from packing. This is a compact
manifest optimization, not evidence that team gameplay aggregates are
unnecessary; active selection, health/gauge aggregates, tag state, and member
activation remain authoritative rollback state.

#### Medium confidence: full `sCamera` singleton

The `0xD80`-byte `sCamera` singleton is mostly viewport, view/projection matrix,
frustum, aspect-ratio, vibration, render-region, and camera-unit reference state.
Ghidra's `sCamera__move` at `0x140532D80` derives this data each frame from the
live camera units and render dimensions. This makes the full singleton a strong
presentation-derived candidate that could remain on the live timeline and be
refreshed once resimulation finishes.

Do not conflate `sCamera` with `uActionSideCamera`. The latter has persistent
smoothing and anchor state and directly reads fighters, stage-edge parameters,
throw/cinematic flags, and action time. Existing investigation associates its
state with camera-relative screen-edge clamps and position divergence. A useful
experiment is therefore to disable only the `sCamera` `0xD80` region while
retaining the `uActionSideCamera` body. Test corner movement, side switches,
throws, supers, camera locks, stage-edge clamps, and rollback during cinematic
camera transitions.

#### Medium confidence: `sReplay+0x54` frame counter

Only four bytes at `sReplay+0x54` are captured. Ghidra's `sReplay__move` primarily
manages recording/playback mode, replay seeding, and training playback limits;
it does not establish that this field is an authoritative live-match simulation
cursor. Custom netplay already owns a synchronized frame index, so this counter
may be derivable or irrelevant when neither recording nor playback is active.

Before removal, identify all writers and consumers of `sReplay+0x54` and test
rollback while recording, training playback, round transitions, and RNG reseed
paths. It is cheap enough that removal has little performance value; its main
benefit would be a cleaner deterministic-state contract.

#### Medium confidence: broad `sShot` manager header

The current `sShot` region copies the first `0x50` bytes while comments describe
it as manager-header and shot-ID state. Shot recipes and explicit lifecycle
reconciliation now carry most historical shot existence and concrete state.
Parts of the first `0x50` bytes are likely `cSystem`, lock, vtable, list, or
allocator ownership and should remain live rather than be flat-restored.

This region should not simply be removed yet because deterministic shot-ID
assignment and manager counters can affect later spawns. Decompile and map
`sShot__assignShotId`, `sShot__clearShotIdEntry`, list registration, and the
pending-unit queue first. Replace the broad header with only proven scalar ID
and generation fields, while lifecycle recipes retain responsibility for list
topology.

#### Lower confidence: animation evaluation output buffers

Per-character snapshots include blend arrays, motion-channel storage, joint
state arrays, joint-index arrays, and blended-joint outputs. Several are outputs
or working buffers that native motion evaluation could theoretically regenerate
from restored action, motion, frame, and model state.

They are not immediate removal candidates. Existing rollback investigation
found that leaving these buffers ahead of restored motion state can split root
motion and joint transforms across timelines, including throw displacement.
The safer long-term experiment is to restore only authoritative motion inputs,
run the native animation evaluation before any gameplay consumer, and compare
the regenerated buffers byte-for-byte with captured values. Until that ordering
is proven, retain them.

#### Packing duplicates that do not change semantics

Some embedded data is packed both as part of a complete character body and as a
separate region. The clearest example is the `0x5A0`-byte `cChrInput` at
`uCharacter+0x21C8`. The separate copy currently has a purpose: the broad
character restore preserves pointer-like qwords and applies only selected scalar
ranges, whereas the later flat input-region restore guarantees complete command
history. A compact character format should store `cChrInput` once explicitly
rather than relying on overlapping regions.

The same principle applies generally: if restore uses only explicit ranges from
a broad object, capture those ranges directly. This reduces snapshot bytes and
makes it visible which state is actually part of the deterministic contract.

#### Recommended removal experiment order

1. Remove the duplicate `sAction` `0x20000` region while retaining the compact
   `0x300` restore and model-joint payload.
2. Pack only `SMAIN_RESTORE_RANGES` instead of the complete `sMain` body.
3. Pack only the restored `cChrTeam` ranges and validated active pointer.
4. Disable the full `sCamera` singleton while retaining `uActionSideCamera`.
5. Test removal of `sReplay+0x54` outside replay recording/playback.
6. Map and narrow the `sShot` header rather than removing it wholesale.
7. Treat animation buffers as regeneration experiments, not immediate cuts.

Each stage should be tested independently. Compare two-peer checksums immediately
after restore and after every replayed frame so a derived state that self-heals
one tick later is distinguishable from an authoritative divergence.
