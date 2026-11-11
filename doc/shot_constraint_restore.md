# Shot Constraint Graph Restore

2026-11-09, Ghidra port 8192, `umvc3.exe.unpacked.exe`, preferred base
`140000000`. This implements common graph capture and an unpublished graph stage,
not complete derived-constraint or whole-shot restoration.

## Native Evidence

Common constraints occupy `90` bytes. Their adapter is at `40`, with complete
object backpointer `48`, previous/next complete-object pointers `50/58`, attached
joint `60`, blend weight `68`, ID `6C`, signed order `70`, unsigned priority `74`,
configured model `78`, blend speed/value `80/84`, ignore-rate/enable bytes `88/89`.
Joint `+8` stores an interface address (`constraint+40`), not the complete object.

- Initializer `1405EC000` is reached by 21 observed callsites, including constructors
  that bypass base constructor `1401091C0`. It establishes the adapter backpointer,
  links and common fields; derived construction finishes later.
- Native unlink `1405EC190` repairs neighbors, clears this node's links/joint and
  sets order `-1`, but does not renumber survivors or request dependency rebuilding.
- Native link `1405EC230` dispatches a priority virtual. Sorting `1405ECEC0` collects
  at most 32 nodes and does not preserve equal-priority order. Relinking is not an
  exact historical restore operation.
- Joint-head setter `14056F900` writes the head and, for active models, marks model
  `558` bit `01000000` and joint/ancestor packed word `A4` bit `00010000`.
- `updateJointDependency` `140569870` only sets model byte `123D`. Actual dependency
  calculation `14056ADA0` clears joint flags `00030000`, resets depths `+14`, calls
  interface dependency virtuals through `14056ABD0`, and sorts the byte order table
  at model `548` through `14056FAB0`.
- Joint-world update `14056FBB0` can invoke dependency getters and constraint apply
  (`1405EA340`), which dispatches each node's primary virtual `+B0`. It is not a
  passive cache flush. Disabling matrix calculation alone does not suppress apply.

The exact path restores graph and caches without invoking any of these operations.
Saved chain indices can legitimately be non-contiguous after native unlink.

## Construction Provenance

`rollback_unit_shot_constraints.inl` records an increasing construction identity after
the common initializer returns. The snapshot boundary, not that hook, observes
the completed derived object. No DTI whitelist or pointer-value classification is
used to discover the common nodes.

Cleanup hooks observed `MtObject` teardown at `1409BFE90` and erases the exact
address before forwarding. It does not inspect dying storage or invoke virtuals.
RotateLimit `1406F9650`, Group child/array `140921A30`, and IK owner-inline teardown
`1406E5970` reach this path while bypassing the old base constraint destructor
hook. Adapter destruction alone is not a sufficient cleanup convergence point.

This is coverage of the observed native construction/destruction paths, not proof
against arbitrary custom constructors or inlined final stores elsewhere. The
registry authorizes reading the common prefix only while its construction identity
has remained uninterrupted. It is not an allocation owner or a resurrection lease.

## Capture

Snapshots seed common nodes from proven containment in completed shot allocations
and from saved model joint heads. They retain complete neighboring chains,
including foreign nodes and detached inline constraints. Each common prefix is
copied once into immutable snapshot storage, with its construction identity.
Configured-model references (`C+78`) use the existing native unit dependency
leases before body capture, including a quarantined shot owner reached only
through a constraint. Newly adopted shot owners join the same capture traversal.
This retains outer units; it does not invent pose retention for non-shot models.
Historical dump export reads only these saved bytes, even after native storage
has been freed. It emits `lifetime.constraint.<construction>.common` regions.

An unknown interface or untracked neighboring node is never dereferenced. Capture
records `constraint_capture_untracked`, sets `constraintsComplete=false`, and
exports `diagnostic.lifetime.constraints`. Restore rejects the entire incomplete
graph before writing. It never restores the recognized subset and resumes.
This status concerns common graph discovery, not proof of all derived ownership.

## Prepare And Commit

`PrepareShotConstraintGraph` runs before pose/storage swaps. It validates source
slot, retained shot birth identities, pose generation IDs/masks, span bounds,
registered node lifetimes, both vptr identities, backpointers, configured models,
joint attachments, complete reciprocal chains and absence of cycles/duplicates.
Numeric interface/next addresses are looked up before their pointees are read.

The current joint set includes current and displaced retained pose generations,
not only the model's currently selected joint pointer. The plan records those
generation IDs. Collection is excluded during the native transaction; commit
also rejects collected/reused generations before touching outgoing joint storage.

A monotonic snapshot `captureId` rejects same-frame recapture as well as slot
turnover. Commit checks this before using pointers into the snapshot payload, so
an old plan cannot read overwritten/reallocated capture buffers.

After retained storage and the body/reference adapters have selected the saved
poses, `StageShotConstraintGraph`:

1. Clears heads in outgoing pose generations and detaches current chain nodes.
2. Copies saved Rno/parameter, delta time and common fields `48..8A`, preserving
   primary/interface vptrs and the unreviewed tail-alignment bytes.
3. Restores saved model `558` and `123D`, every saved joint's depth `14` and packed
   word `A4`, and the saved evaluation-order bytes owned through model `548`.
4. Publishes saved joint interface heads with their exact saved chain order.

The whole packed joint word is restored because the retained joint checkpoint
owns it; a separate flags-only layer would need mask `00030000` instead.
No sorting, dependency rebuilding, evaluation, constructor, destructor or native
callback runs during graph commit. Newly encountered current nodes are detached,
not freed or reconstructed by the graph layer. Only inline children proven to
have been initialized after the snapshot may be withdrawn without saved state;
an older foreign node that entered the graph later is rejected, not erased.

The shared execution stage also includes saved common constraints. Their native
category/group order, lifecycle/move/fixed flags and primary links rewind without
initialization or registration callbacks. Proven future inline children become
non-executing and detached. Standalone constraint identities use their existing
saved unit leases/accounting; absent historical unit identity is not guessed.

## Integration Limits

Preparation currently proves joint storage through the existing shot pose
generations. A chain attached to an external character/helper model without that
retained-pose proof fails preflight; it is not approximated using current pointer
equality. Foreign common nodes can be restored only while their construction
identity remains live. Their physical owner may need additional retention.

The common graph stage does not restore derived constraint payload beyond `90`
or their resource owners. Those must join the enclosing transaction, as must the
remaining concrete shot suffix and dependent-effect state. Snapshot capture is
active; graph preparation/commit remain deliberately unwired from production
frame restore.

## Regression Coverage

ASan/UBSan native mocks exercise a shared chain across two owners, a foreign node,
equal priorities, non-contiguous indices, repeat restore, complete dependency-cache
restoration, a displaced pose swap followed by physical collection, and a 40-node
chain. No native unlink/sort/evaluation is invoked. Rejection tests cover unknown
interfaces without pointee reads, cycles, duplicate records, destroyed/reinitialized
nodes, unsupported external models, expired pose generations and same-frame
snapshot replacement. Saved common regions also export after native storage free.

The combined graph/execution regression rewinds constraint Rno/delta, primary
category ordering, groups and lifecycle, including a future inline child's
callback-free withdrawal and repeated publication without accounting drift.
Rejection scenarios reuse one compiled harness, each in a fresh native process.

Validation: 83 Python tests, eight viewer tests, `make -j8`, and whitespace checks
passed. The earlier interrupted full run was not counted as a pass. No live-game
validation or completed Mystic Blast repair is claimed.
