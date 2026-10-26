# Rocket Tail Chain Cost Investigation

Original static Ghidra investigation: 2026-10-08. The native-work table identifies
work and scaling, not a solver-only timing measurement. Enabled callback timings
from a later October 8 profile are linked below; they do not invalidate the older
zero-call profiles or establish a worst-case budget.

**Checked 2026-10-26:** Ghidra **8193**, `umvc3.exe.unpacked.exe`;
freshly re-read `uChrChain__move @ 0x140035F90` and the permitted
`rollback_model_policy.cpp`/`rollback_tick.cpp` source. Other table entries,
resource dimensions and profiling captures retain prior provenance; no resource
scan, timing rerun or in-game validation was performed.

## Resource Scope

`nativePCx64/chr/archive/0046_00.arc` contains
`chr\RRaccoon\model\chain_sippo`. Its CHN resource has one group, five nodes,
force mode 1, logical joints 131-135, one pinned node and four movable nodes.
All five angular-mode bytes are zero; their collision shape mode is 1.
The model maps logical joints 132-135 to physical joints 107-110, which drive
the divergent attack capsules in the `20261008_090835` capture.

## Native Work

| Address | Function | Cost-relevant behavior |
|---|---|---|
| `0x140035F90` | `uChrChain__move` | Validates active owner, updates blend/pause/time state, delegates bounds/collider preparation. |
| `0x140036130` | `uChrChain__moveAfter` | Chooses simulation or model-synchronization branch according to blend and runtime flags. |
| `0x1400361C0` | `uChrChain__simulateChainAfterMove` | Copies parent model work, handles deferred steps, runs group solver and specialized joint commit. |
| `0x1400366A0` | `uChrChain__syncChainAfterMove` | Copies parent model work and synchronizes nodes from animation without invoking the group solver. |
| `0x14054C010` | `uChain__copyParentJointMatricesToWorkBuffers` | Copies every parent joint's matrix into two work buffers, plus attributes/scale; optional scale extraction uses square roots. Rocket has 127 model joints, not just five. |
| `0x140550CB0` | `uChain__simulateGroup` | Each configured iteration repeats a full-group force/constraint/integration sweep once per node. Approximate node scaling is quadratic. |
| `0x14054FE30` | `uChain__accumulateNodeForcesMode1` | Traverses movable nodes and their neighbors, normalizes directions, computes springs and damping. |
| `0x14054E7E0` | `uChain__resolveColliderNodeGeometryCollisions` | Traverses group nodes and collected collider candidates; shape-specific narrow-phase math is repeated inside solver sweeps. |
| `0x14054F720` | `uChain__resolveSceneCollisions` | Issues scene sphere/segment queries for eligible nodes/connections inside solver sweeps. |
| `0x1405531A0` | `uChain__solveNodeAngularLimits` | Traverses nodes; expensive angular math is conditional on node mode. Rocket's supplied resource disables it. |
| `0x140557380` | `uChain__runDeferredSimulationSteps` | Repeats complete group simulation for a requested catch-up count; the counter must be audited and rewound, not blindly cleared. |
| `0x140036800` | `uChrChain__enforceChainSegmentLengths` | Actual character-chain virtual +0xA0. Resolves parents, checks/clamps segment lengths, and commits affected joint matrices. It is not the generic all-joint commit. |

`uChain__ctor` initializes iteration count at +0xC8 to 2, but
`uChrChain__ctor` overrides it to 1. With that default, Rocket's five-node
group performs five complete group sweeps per simulation step, including twenty
movable-node integrations. This is more than one five-node pass, but much less
than a large cloth chain. Runtime property overrides can change the count.

Collision work is gated by +0x1D1 (collected collider geometry), +0x220
(resource collision shapes), and +0x1D0 (scene collision). The active runtime
values are not available in the desync dump; do not claim these queries dominate
Rocket's cost without measuring the flags and their timings.

The refreshed move body confirms owner/alive and virtual activity gates, stop/init
flag preparation, owner time-scale propagation, blend advancement and delegation
to `uChain__move`. Bypassing it is broader than bypassing solver arithmetic.
Do not confuse it with `uCnsChain__move @ 0x14070D830`, whose checked body handles
resource refresh, model/collider rebinding/reset and collision queue preparation.

## Determinism Boundaries

The repeated sweeps use evolving positions/velocities and divide integration
time by node count times iteration count. They are not redundant passes that can
be removed or hoisted without changing the trajectory.

Conditional wind/random-drag paths consume the global MtXorShift generator.
Changing RNG call count/order can affect other gameplay, even when a force
coefficient is zero. Preserve the native branches, arithmetic order and phase
ordering when validating an optimization.

Resource-binding changes can enter allocating/reinitializing paths, but those
are conditional; they are not evidence of allocation on every steady-state tick.

## Available Timing Evidence

Eight existing client profiles checked from 2026-08-29 and 2026-09-11 all record
`uChrChain` visits with **zero calls and zero measured time** in move, sync-job,
and sync-save phases. Those profiles were collected with chains skipped. They
cannot establish how many milliseconds an enabled Rocket tail costs.

The subsequent `nplive_20261008_103624` enabled-hook evidence is preserved in
[Gameplay Chain Resource Audit — Rocket Timing](gameplay_chain_audit.md#rocket-timing):
combined move/post-move averages were **15.02 us per tail** on host and
**18.30 us per tail** on client, or **30.03/36.60 us per replay frame** for two
tails. These are historical elapsed callback timings (nested in object timing),
not new measurements, solver-only costs, or timings of today's skipped-tail path.

## Optimization Targets

1. Profile only Rocket's tail, retaining native owner/blend/pause gating. Separate
   matrix preparation, solver, collision queries, deferred work, and joint commit.
2. Keep presentation-only chains excluded; do not benchmark or enable the entire
   character-chain category as a proxy for this five-node resource.
3. Audit deferred counters and resource reinitializations for accidental replay
   amplification before rewriting arithmetic.
4. Investigate preparing only the node/ancestor dependency closure instead of
   copying all 127 joints. This needs a complete helper-read audit before it is
   safe; unchanged solver math alone does not prove a sparse copy correct.
5. Preserve the existing non-solver synchronization branch when native flags
   select it. Do not force the expensive simulation branch on paused/blended
   states, or force the cheap branch where native gameplay requires simulation.

No chain-enable policy or gameplay code was changed in this investigation.

## Animation-Only Tail Trial

At the user's request, the plugin now bypasses `uChrChain__move` and
`uChrChain__moveAfter` for live Rocket owners identified through chain +0x1320.
This applies to ordinary gameplay as well as resimulation; native character
animation remains responsible for the tail joint pose. It does not freeze chain
history, force a reset pose, or enable chains during headless replay. Other
characters and native dead-owner teardown remain unchanged. The two hooks must
both install successfully before the policy becomes active; partial installation
is pass-through and logs the policy as unavailable.

More precisely, the source predicate selects the Rocket vtable at owner
`chain+0x1320` and lifecycle state 1 or 2; it does **not** inspect the CHN resource
name. Its scope is all matching Rocket-owner `uChrChain` callbacks, not a proved
resource-specific `chain_sippo` filter. The dead-owner path passes through these
hooks. Independently, headless dispatch excludes exact `uChrChain` DTI for every
owner, so live-hook pass-through does not imply headless execution.

This intentionally changes native tail-driven collision geometry and must run
on both peers with the same build. It avoids the chain solver and its collider
preparation, with only a constant-time owner/state check per hooked phase.
Runtime testing must confirm the frame-6355 joint/capsule split is gone and that
Rocket's animation-only tail still renders and attacks correctly. The regression
checks live/resim parity, other-character pass-through, dead-owner handling and
hook-installation failure behavior; it is not a native in-game geometry test.
