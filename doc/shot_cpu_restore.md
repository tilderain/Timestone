# Common Shot CPU Restore

2026-11-09, Ghidra port 8192, `umvc3.exe.unpacked.exe`, preferred base
`0x140000000`. This describes a tested component, not enabled whole-shot rollback.

## Implementation

`lifetime_shot_cpu.inl` implements one common `uShot` CPU kernel for the inherited
`2140`-byte prefix. It has no concrete-DTI whitelist, pointer-value heuristic, or
live/quarantined code split. Buffer validation precedes every write; the return
value is explicitly `ReviewedSubset`, not complete layout coverage.

`StageShotCpuFields` selects the snapshot-owned body, validates its bounds, owner
identity and rollback-slot lease, then calls the kernel. It neither reads saved
pointees nor runs native callbacks, constructors, motion sampling, or simulation.
It is not called by `Rollback_RestoreSnapshot` yet.

The parent transaction must establish compatible retained model/work storage and
resource bindings, restore reference and constraint graphs, and publish native
registration/lifecycle coherently. Copying these scalar fields by itself does not
meet those requirements.

## Restored Fields

Ranges below are hexadecimal, half-open, shot-relative. Unlisted bytes remain
live. The schema is deliberately non-contiguous.

| Range | CPU state |
| --- | --- |
| `08..10`, `28..2C` | Unit Rno/parameter and delta time, not lifecycle or membership |
| `48..100` | Coordinate selectors, position/quaternion/scale and local/world matrices |
| `55C..560`, `1180..11B0` | Motion interpolation attributes and root-motion scale/deltas |
| `1240..1250`, `1260..129F` | Inline morph/plane/waist/foot/IK parameters; no shader/map owners |
| `1300..1308`, `1310..1344` | Actor time coefficients and animation request/playback state |
| `1350..1360`, `13C0`, `13C4..13D0`, `13E0..13E8` | Interpreter control bytes, compare/jump registers and loop state |
| `1488..148C`, `1498..149C`, `14A0..14C0` | Attack reset/index, combo count, clocks and pending attack state |
| `1DD0..1E30`, `1E48..1F50` | Inline cArm attachment parameters/joint selectors and four inline matrices |
| `1F58..1F68` | Shot timer, delay, division-start time and active/watch flags |
| `1FE4..2000`, `2038..2058` | Gameplay/contact/end flags, division/durability/hit-clock/priority state |
| `2078..20B8`, `20C0..20CC`, `20D0..20D4` | Life/vanish, movement/homing and contact-displacement state |
| `2118..2133` | Animation-watch controls, phase/dispatch word and three child latches |

For each of the eight motion slots (`560 + 180*i`), only `08..0E`, `10..74` and
`80..E0` are copied. The checked `EaseCurve` is two floats, not a callback or
pointer. Unknown padding, samplers, sequence bindings/history, work pointers and
key counts/descriptor pointers are outside this scalar kernel. The existing
character model-motion adapter demonstrates why sampler bindings and cursors need
a separate resource-backed operation, not classification as arbitrary scalars.

Two flag words use masks: attack flag `149C` preserves the live `40` sound-emitted
bit, and system flag `1FE0` preserves the live `10000000` sound-cookie bit. Sound
cRefs remain live. This is a live-audio policy, not historical sound rewind or a
general audio event-deduplication implementation.

## Native Evidence

- `uShot::preMove` (`1401CCBE0`) consumes parent/target/owner/callback liveness,
  dispatches the low-byte phase, and invokes actor/model work before hitboxes on
  eligible paths. Do not call it merely to refresh a restored pose.
- `uShot::moveAfter` (`1401CCEB0`), constructor (`1401C9B00`) and properties
  (`1401CAAF0`) confirm the shot clocks, inline matrices, movement parameters,
  contact vector/scale, watch controls and reference holes. Several existing
  recovered `uShot` field names are shifted; instruction/property offsets take
  precedence over those names.
- `cAtkCtrl::update` (`14000D6E0`) advances attack clocks and uses flag `40` to
  gate attack sound. It also updates the three target-history banks.
- Initial/active sound helpers (`1401CD4F0`, `1401CD530`) and sound-cookie stop
  (`1401CE040`) couple system flag `10000000` to the live sound references.
- The checked `uCoord`, `uActorGameModel`, `cActorGameModelAnm`, `cAtkCtrl`,
  `uModel::Motion`, and `EaseCurve` layouts support the common scalar partition.
  Resource-backed interpreter type/key/cursors and return flags/value are left
  together for the reference adapter rather than partly restoring them here.

## Publication Boundary

These are deliberately **not** restored by the CPU kernel:

- Unit lifecycle/category flags, intrusive links, group membership and shot ID/team.
- Parent/owner/target/attachment references and all fourteen callback slots.
- All 96 attack-history records. Each target pointer must rewind with its hit
  count and time; copying counters while leaving the target live is incorrect.
- Resource owners, command bindings/cursors, allocation bounds and work pointers.
- Material/vjob/render ownership and payloads, sound cRefs, constraint links and
  the unreviewed concrete suffix after `2140`.

The native common vanish path (`1401A0050`, emptiness test `1401C99F0`) can finish
a shot when callback slots 1..13 are empty. Thus correct timer bytes alone cannot
establish correct lifetime when the callback state belongs to the future. The
registration stage also does not yet reconcile `sUnit` execution or constraints.
Those are remaining transaction work, not reasons to restore deep materials.

## Verification

The ASan/UBSan regression captures a shot at frame 280 with timer `3`, advances
eight fixture ticks to `11`, stages its retained joint pose and common CPU state,
then advances once to `4`. Position, life, attack clock and phase are checked
together. Repeated restores do not accumulate ticks. The same stage handles a
quarantined identity without changing its lifecycle, ID, or quarantine count.

An independent byte-mask test verifies every copied/preserved byte, including the
full derived suffix and the mixed sound flag words. Invalid sizes/null buffers
do not mutate output. Missing leases, malformed payload bounds and duplicate body
spans abort before any destination write. These are native-mock/component tests,
not a replay of the Mystic Blast dump inside the game.
