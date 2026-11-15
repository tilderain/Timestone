# Rocket Raccoon Tail Chain Under Rollback: Saving And Restoring It

Ghidra 8192 `umvc3.exe.unpacked.exe`, 2026-11-15. Static analysis only; nothing
here is implemented. Write this up if Rocket's `uChrChain` is ever re-enabled.

## Why It Matters

- Rocket's tail (`chr\RRaccoon\model\chain_sippo`) has one group of five nodes:
  one pinned, four movable. Its logical joints 131–135 drive attack collision
  sets 11 and 34 (see `gameplay_chain_audit.md`), so the tail is gameplay state.
- `uChrChain__moveAfter` (`0x140036130`, unit vtable `+0x48`, fence 3) runs
  **after** collision and hit solving. The solver writes the tail joints, and the
  next frame's hitboxes use them.
- The character snapshot already restores those joint matrices (`cChr+0x538`,
  0xC0 bytes per joint; `rollback_snapshot.cpp` `addActorBuffer(0x538, …)`).
  It does **not** restore the solver. After a restore the tail is posed correctly,
  but the next simulated step continues from the abandoned future's node positions
  and velocities.
- Nothing else captures `uChrChain`: not `RollbackUnit`, not the region builder,
  not the character tail restore.

So if the chain runs in resim, its state must be rewound. If it runs live only,
resim is wrong anyway.

## Global RNG (checked; normally not used)

`uChain__simulateGroup` (`0x140550CB0`) draws from `g_MtXorShift__globalState`
only when one of two flags is set:

| Byte | Property name | Default |
|---|---|---|
| `uChain+0xCD` | `mCP.mExforceFlag` (wind / external force) | 0 in `uChain__ctor`. No code writes it except the property table (`uChain__createProperty`). `uChain__setChainResource` does not set it. |
| `uChain+0xCF` | `mCP.mRandomDumpingFlag` (random damping) | 1 in `uChain__ctor`, but `uChrChain__ctor` calls `uChrChain__resetRuntimeState` (`0x140036350`), which sets it to 0 for every character chain. |

Neither flag is expected to be set for Rocket. Confirm at runtime with a one-time
log of both bytes. If either is set, the save/restore below makes the RNG draws
deterministic too, because the wind countdown is included. The global RNG itself
is already snapshotted.

## Object Layout

Ghidra structs: `uChrChain` is 0x1380 bytes, `uChain` 0x1320.

| Offset | Field | Notes |
|---|---|---|
| `+0x00..+0x90` | `uJointAnimator` | `+0x40` owner model; `+0x89` error byte; `mpCloneBuff` work matrices (rebuilt from parent joints every step, so not state) |
| `+0x90` | `mpChain` | chain resource. Group table at `mpChain+0x80`, 0x20 per group; byte 0 = node count. `mpChain+0x7C` = group count. |
| `+0x98..+0x1D1` | inline simulation block | see the next table |
| `+0x1D1` | `mColliderCollision` | |
| `+0x218` | collision transform records (pointer) | count `+0xB4`, stride 0x130 (`uChain__commitNodeTransformHistory`) |
| `+0x220` | resource collision shapes present (byte) | |
| `+0x1320` | `mpChr` | owner. Read by `0x140035DC0`. |
| `+0x1328` | `mAimBlend` | inline blend object: vtable, then state through `+0x1357` |
| `+0x1358` | `mBlendBase` | |
| `+0x135C` | `mBlendOrderFrame` | |
| `+0x1360` | `mBlendMin` | |
| `+0x1378` | runtime flags | `8` moved, `0x10`, `0x40` ForceSimple, `0x80`, `0x100`, `0x400` reset/sync, `0x800` StopChain |

Inline block, as `uChain+0x98+k` (absolute offset in parentheses):

| k | Absolute | Meaning |
|---|---|---|
| `+0x08` | `+0xA0` | pointer to the per-group node array pointers (keep live) |
| `+0x18` | `+0xB0` | group count |
| `+0x1C` | `+0xB4` | collision record count |
| `+0x20`, `+0x24` | `+0xB8`, `+0xBC` | step dt, per-iteration dt |
| `+0x30` | `+0xC8` | iterations (`uChrChain__ctor` sets 1) |
| `+0x34`..`+0x37` | `+0xCC`..`+0xCF` | per-step flags; `+0x35` wind, `+0x37` random damping |
| `+0x38` | `+0xD0` | blend (`moveAfter` solves only while it is below 1) |
| `+0x48..+0x60` | `+0xE0..+0xF8` | wind amplitudes and direction |
| `+0x68`, `+0x6C`, `+0x70` | `+0x100`, `+0x104`, `+0x108` | wind period, phase, countdown |
| `+0x74` | `+0x10C` | time scale factor |
| `+0x80` | `+0x118` | deferred simulation step count (`uChain__runDeferredSimulationSteps`) |
| `+0x88..+0x94` | `+0x120..+0x12C` | previous root position |
| `+0x98..+0xA0` | `+0x130..+0x138` | root delta |
| `+0xA8..+0xD0` | `+0x140..+0x168` | root rotation matrix |
| `+0x138` | `+0x1D0` | scene collision flag |

Each group's node array is `*(*(uChain+0xA0) + g*8)`, with nodes × 0x140 bytes
(allocated in `uChain__setChainResource`). Node fields used by the solver:

| Offset | Meaning |
|---|---|
| `+0x00..+0x3C` | matrix |
| `+0x50` | velocity |
| `+0x60` | position |
| `+0x70` | rest position |
| `+0x80` | force |
| `+0x90` | wind force |
| `+0xA0` | wind random vector (node 0 holds the group's) |
| `+0x110..+0x12C` | neighbour indices, lengths, types |
| `+0x130` | mode |
| `+0x134` | joint id |

Nodes are plain data. `uChain__setChainResource` only fills each node's matrix
(`+0x00`) with identity and zeroes `+0xB0..+0xEF` through
`MtArray__initElements` (which is an element filler, not an array header). Whole
nodes can be copied.

## What To Save (per allowed chain, about 2.5 KB for Rocket)

1. `uChain+0x98..+0x1D1`, **except** the 8-byte pointer at `+0xA0`. Easiest is
   to save it all and restore it in two pieces around `+0xA0..+0xA8`.
2. For each group g < `*(u32*)(+0xB0)`: the node array, nodes × 0x140, where
   nodes = `*(u8*)(*(mpChain+0x80) + g*0x20)`.
3. Collision records: `*(+0x218)`, `*(u32*)(+0xB4)` × 0x130.
4. `uChrChain+0x1330..+0x1380`: blend object state, blend scalars, runtime flags.
   Leave the vtable at `+0x1328` and the owner at `+0x1320` alone.

The work matrices (`mpCloneBuff`) are not saved. `uChain__copyParentJointMatricesToWorkBuffers`
(`0x14054C010`) rebuilds them from the restored character joints at the start
of each step.

## How To Wire It

- **Enumerate:** walk sUnit lane 13 (where character chains live) for exact
  `uChrChain` DTI `0x140D44920` whose owner `+0x1320` has DTI `0x140D459D8`
  (Rocket). The resim gate in `rollback_tick.cpp IsHeadlessGameplayUnitObject`
  has the same test.
- **Save** in `Rollback_SaveSnapshot` next to `Rollback_SaveSoundState`, into a
  per-slot array (`frame % ROLLBACK_WINDOW`). Key each record by
  {chain pointer, owner, group-array pointer `+0xA0`, collision pointer `+0x218`,
  node counts}.
- **Restore** in `Rollback_RestoreSnapshot`, after the character restore, so the
  joints and the solver agree. Restore a record only if the chain still exists
  in the lane with the same key. A changed pointer means `setChainResource`
  rebuilt the chain, and its old state no longer applies.
- **Resim:** let `IsHeadlessGameplayUnitObject` return true for Rocket's chain.
  It must run in both the move (`+0x30`) and moveAfter (`+0x48`) passes, as natively.
- **Cost:** about 2.5 KB of memcpy per tail per snapshot, plus a lane scan. That's
  negligible. The solver itself costs about 15–18 µs per tail per replayed frame
  (see `rocket_tail_chain_performance.md`).

## Verification

1. One-time log of `+0xCD` and `+0xCF` for Rocket's chain: both expected 0.
2. Desync test with Rocket in netplay, forcing rollbacks during tail-hitbox
   attacks (collision sets 11 and 34). Compare checksums of the character joint
   state at `cChr+0x538` for joints 131–135 across peers.

## Current State

Rocket's tail chain is disabled on live frames and in resim:
- `rollback_hooks.cpp` `hkChrChainMove` / `hkChrChainMoveAfter` skip
  `0x140035F90` / `0x140036130` when the owner is alive and is Rocket.
- `rollback_tick.cpp` skips every `uChrChain` in resim.

To go back, remove those two hooks, allow Rocket's chain in
`IsHeadlessGameplayUnitObject`, and add the save/restore above.
