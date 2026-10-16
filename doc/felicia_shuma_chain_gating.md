# Felicia And Shuma Chain Gating

Ghidra and read-only installed-resource investigation, 2026-10-08. This refines
the static overlap audit: a shared joint is not proof that physics reaches a hit.
No gameplay code or chain-enable policy was changed.

## Findings

| Case | Native behavior | Resim-only skip assessment |
|---|---|---|
| Felicia attack using tail collision set 30 | Action 150 explicitly requests immediate animation-only for every chain | Strong protection against tail physics affecting this attack; not a demonstrated desync |
| Shuma normal/main-script motions | Slot-zero selections use motion lists 0/1; default chain transition is immediate | Native synchronization, not physics; collision overlap alone overstates risk |
| Shuma down actions 300-303 and 311-313 | Slot-zero selections use motion list 2, enabling chain contribution | Conditional retained-pose/body-collision risk; not yet an observed accepted-hit divergence |

Neither character has a demonstrated chain-caused gameplay desync in this
investigation. Do not disable all Shuma chains merely on the earlier overlap
finding, and do not label their resim-only exclusion unconditionally safe.

## Blend And Solver Semantics

`uChrChain__move`, `0x140035F90`, writes effective animation weight to chain
`+0xD0`. `uChrChain__moveAfter`, `0x140036130`, runs the solver only when the
weight is below 1 and current-move flag 8 is set, with initialization flag 0x400,
force-simple flag 0x40, and stop-chain flag 0x800 clear.

Weight 1 means animation-only. Requested weight 0 is normally clamped to minimum
0.1, leaving substantial physics contribution. The force-motion-zero flag is an
exception that sets the effective value to zero. `0x1405580D0` verifies the blend
direction: `simulation + weight * (animation - simulation)`.

`uCharacter__processActionAnimation`, `0x1400391C0`, requests weight 1 for ordinary
slot-zero motion selections, but weight 0 with an immediate transition for packed
motion IDs 0x200-0x3FF. A force-common-animation-zero flag can also request zero.
Explicit `1_81` commands override selected chains through `0x14004CE80`.

The synchronization path, `0x1400366A0`, copies model matrices, reseeds nodes and
updates chain-owned work/history. It does not invoke model writeback
`0x140036800`. It does not reconstruct or cleanse a model pose already modified
by a previous solve.

## Installed Scripts

`0017_param.arc`, `chr\Felicia\anmchr`:

- 2268 commands enumerated; exactly one `1_81` command.
- Only action 150 (0x96) selects attack collision set 30.
- Its frame-zero commands load set 30, select slot-zero motion list 1/index 0,
  select an additional slot-3 motion, then execute `1_81` at command index 5.
- `1_81` payload: mask 0xFFFFFFFF, weight 1, transition frames 0, next-transition
  override 0, alternate-enabled false. It immediately makes all chains
  animation-only. This command is at decompressed file offset 29681.
- Thus the normal attack path actively excludes solved-tail contribution; the
  earlier matching joints 185-188 are not sufficient evidence of a physics-driven
  attack. Fresh evaluation and unusual runtime overrides still need verification.

`0037_param.arc`, `chr\Shuma\anmchr`:

- 2080 commands enumerated; no `1_81` commands.
- Enumerated slot-zero motion selections use lists 0/1, with fallback list 3.
- All six CST entries have flags 4, setting default chain transition frames to
  zero, not disabling the chain. Normal selections therefore request immediate
  animation-only behavior, assuming the primary motion exists and no runtime
  force-zero override applies.

`chr\Shuma\anmtdown`:

- 557 commands enumerated; no `1_81` commands.
- Actions 300-303 and 311-313 select list-2 motions, requesting weight 0 (clamped
  to 0.1) and allowing physics when other gates pass.
- No enumerated flag operation in Shuma's two scripts assigns owner +0x14F0 or
  explicitly sets/clears stop-chain bit 25. No enumerated +0x1500 operation touches
  pose-skip bit 0x800000. This does not exhaust native or shared-script writers.
- Body-collision joints 184 and 187 in `chain_Down`, and 198 in `chain_R_down`,
  are movable (node flag 2), not pinned roots. `chain_L_down` body joint 168 is
  pinned (flag 3); that overlap alone is a weaker physics-displacement argument.

Script enumeration preserves directory order but does not execute jumps,
conditionals, native callbacks, or fallback selection. Action IDs are not assigned
named moves here. Installed archives were not authenticated against a clean depot.

## Collision Timing And Retention

Native `sMvc3Main__move` orders unit movement (`0x140259670`), collision
(`0x14025967D`), hit solving (`0x140259693`), then unit sync/fence-3 finalization
(`0x1402596A0`, `0x1402596AD`). Chain solve/writeback is in the late +0x48 phase.
It cannot change the geometry already tested in that same frame.

On a normal next tick, shared-model constraints rebuild joint world matrices
from animation-local transforms before collision. `uCnsShare` helper
`0x14010D020` overwrites joint +0x20. Chain writeback modifies that world matrix,
not the animation-local rotation/translation. A successful fresh constraint pass
therefore removes the previous late chain result.

Retention exceptions:

- `uCharacter__move_base`, `0x14004C19E`, skips pose advancement when
  `canAdvanceSimulation` is false, including zero-time-scale cases.
- Owner +0x1500 bit 0x800000 skips model evaluation at `0x14004C2CE`.
- Joint attribute 0x20 skips world rebuilding AND constraint dispatch at
  `0x14056FC83`. CHN flags 2/3 do not establish this model attribute.
- The skipped-pose branches still reach body-sphere updates at `0x14004C324`.
  `0x14004B7E0` has no blanket rejection of all damage or frozen states.
- Actual hit acceptance adds independent gates. `0x1400438F0` rejects nonzero
  defender `cChrPause::mMode` at +0x2D78 and restricts attacks against particular
  down-state flags. Movement hitstop is not proven identical to that pause mode.

The conditional failure path is: down-state solver changes a movable body joint
after collision; a later pose-retained tick reads it; an eligible attack intersects
that displaced body sphere on one timeline but not the other. Every link is
structurally possible, but no runtime trace has demonstrated the complete path.

## Recommendation

Keep the current exclusion while collecting targeted evidence rather than adding
all six Shuma solvers. Test Shuma down actions 300-303 and 311-313 against eligible
follow-up/OTG attacks, including frozen frames. Record current blend, stop/init
flags, pose-evaluation decision, pause mode, joints 184/187/198 and their active
body spheres on live versus resim timelines. For Felicia, verify action 150's
immediate weight-1 command and matching tail capsules through startup and hitstop.

Without an in-game trace, this investigation can narrow the risk but cannot certify
a resim-only skip or claim that either character will desync. Solver/history and
blend state remain outside a dedicated chain snapshot policy.

Parser: `tools/audit_chain_gating.py`. Full extracted command inventory:
`/tmp/opencode/felicia_shuma_chain_gating.json`. Parser tests exercise the recovered
relative offsets and packed operands; they are not gameplay determinism tests.
