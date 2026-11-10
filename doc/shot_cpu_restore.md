# Common Shot CPU Restore

2026-11-09, Ghidra port 8192, `umvc3.exe.unpacked.exe`, preferred base
`0x140000000`. This describes a tested component, not enabled whole-shot rollback.

## Implementation

`lifetime_shot_cpu.inl` implements one common `uShot` CPU kernel for the inherited
`2140`-byte prefix. It has no concrete-DTI whitelist, pointer-value heuristic, or
live/quarantined code split. Buffer validation precedes every write; the return
value is explicitly `ReviewedSubset`, not complete layout coverage.

`StageShotCpuFields` selects the snapshot-owned body, validates its bounds, owner
identity and rollback-slot lease, then calls the kernel. Borrowed unit references
must also match a retained identity in that slot's manifest, including its birth
ID; an expired lease or reused address fails before writing. It neither reads saved
pointees nor runs native callbacks, constructors, motion sampling, or simulation.
It is not called by `Rollback_RestoreSnapshot` yet.

The parent transaction must establish compatible retained model/work storage and
resource bindings, restore reference and constraint graphs, and publish native
registration/lifecycle coherently. Copying these fields by itself does not
meet those requirements.

## Restored Fields

Ranges below are hexadecimal, half-open, shot-relative. Unlisted bytes remain
live. The schema is deliberately non-contiguous.

| Range | CPU state |
| --- | --- |
| `08..10`, `28..2C` | Unit Rno/parameter and delta time, not lifecycle or membership |
| `40..100` | Borrowed coordinate parent, selectors, position/quaternion/scale and local/world matrices |
| `55C..560`, `1180..11B0` | Motion interpolation attributes and root-motion scale/deltas |
| `1240..1250`, `1260..129F` | Inline morph/plane/waist/foot/IK parameters; no shader/map owners |
| `1300..1310`, `1310..1344` | Actor time coefficients, borrowed owner and animation request/playback state |
| `1350..1380`, `1388..13B8`, `13C0`, `13C4..13D4`, `13D8..13E8` | Interpreter controls, resource mirrors and cursors, compare/jump/return state and loop state |
| `1480..148C`, `1490..149C`, `14A0..14C0` | Borrowed attack owner/resource, reset/index, combo count, clocks and pending attack state |
| Each `TargetInfo+08..18`, 96 records at `14C0`, stride `18` | Target pointer, hit count and hit time together; vtables preserved |
| `1DD0..1F50` | Inline cArm parameters, borrowed self/attachment parents, joint selectors and four matrices |
| `1F58..1F68` | Shot timer, delay, division-start time and active/watch flags |
| `1F70..1FE0`, `2000..2038` | Fourteen callback references and seven borrowed unit references |
| `1FE4..2000`, `2038..2058` | Gameplay/contact/end flags, division/durability/hit-clock/priority state |
| `2078..20B8`, `20C0..20CC`, `20D0..20D4` | Life/vanish, movement/homing and contact-displacement state |
| `2118..2133` | Animation-watch controls, phase/dispatch word and three child latches |

For each of the eight motion slots (`560 + 180*i`), playback/root history ranges
`08..0E`, `10..74`, `80..E0` are copied. The checked `EaseCurve` is two floats.
The two samplers at `E0` and `100` each restore `00..0C` and `10..1C`: track,
frame cursor, segment pointer and metadata, not their unknown alignment words.
`120..160` restores four sequence bindings and current/previous masks. `168..16C`
restores channel count and `170..178` the descriptor binding. Owned work at `160`
and key-data aliases at `178` remain the storage stage's responsibility.

`setBlendNum` (`1405291E0`) proves the slot-zero key slab ownership and per-slot
alias stride `modelKeyCount * 0C`. `setupMotionParam` (`14052A880`) binds sequence
and key descriptors from motion resources. `updateFrame` (`14052BBD0`) moves
current masks into previous masks and copies each key-work value `+4` into `+8`
before `calcKeyFrame` (`1405260B0`) updates cursor/value. All three key-work words
therefore rewind in the retained slab. `calcSequence` (`140526460`) reads resource
sequence entries without publishing ownership. Sampler init/sample
(`14079F7E0`/`14079F5D0`) likewise own no allocations. Motion-resource retention
must precede these direct binding/cursor copies; no setup/move/update is invoked.

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
  Resource-backed interpreter type/key/cursors now restore together with their
  mirrors after retained resource staging. The return value is a borrowed unit
  pointer, not a scalar-only result. See `shot_reference_restore.md` for the deeper
  assignment/cleanup/destruction audit and dependency retention.

## Publication Boundary

These are deliberately **not** restored by the CPU kernel:

- Unit lifecycle/category flags, intrusive links, group membership and shot ID/team.
- Owned resource references, allocation bounds and work pointers (storage stage).
- Interpreter self-owner, opaque words at `13B8`, `1478`, `1DC8` and padding.
- Material/vjob/render ownership and payloads, sound cRefs, constraint links and
  the unreviewed concrete suffix after `2140`.

The native common vanish path (`1401A0050`, emptiness test `1401C99F0`) can finish
a shot when callback slots 1..13 are empty. The callback pointers now rewind, but
their pointees must also have their historical simulation state restored. The
registration stage is separate from the tested `sUnit` execution/lifecycle stage;
neither is published from production yet. Constraint repair is still unfinished.
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
