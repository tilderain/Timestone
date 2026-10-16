# Filter resource inventory

Read-only asset/native audit, 2026-10-15. No runtime code changed.

## Identification

Native `uEffect__createParticleManager @ 0x1405E2860` selects
`cParticleGeneratorFilter` for particle type 9. EFL loader `0x1405EDEF0` and
resource conversion `0x1405EE320` establish this disk layout:

- Header magic `EFL\0`, version `0x20120306`.
- Node count is u16 at file `+0x10`; payload begins at `+0x30`.
- Nodes are four packed u32 references, stride `0x10`.
- Node word 1 low byte is particle type; upper 24 bits are the particle
  parameter offset relative to the payload.
- Node word 0 upper 24 bits select generator parameters; their first two u32s
  are the group and material masks used by `uEffect__checkEffectList`.

Scanned 1,735 EFL payloads from `eft/cmn/0000.arc` and character archives
`*_cmn.arc` / `*_00.arc`. This is not an exhaustive stage, alternate-costume,
or dynamically constructed resource inventory. ARC entries were parsed and
decompressed in memory without extracting or modifying game assets.

## Shared hyper startup effect

`eft\cmn\efl\0100` contains a Filter at entry 0, group mask `1` and material
mask `0xFFFFFFFF`. Direct animation commands request this resource with group
`1` at frame zero of these examined hyper-startup actions:

| Character | Action IDs requesting shared 0100 |
|---|---|
| Ryu | `0xDC..0xDF`, `0xE2..0xE5` |
| Akuma (`Gouki`) | `0xDC..0xDF` |
| Nemesis | `0xDC..0xDE` |
| Thor | `0xDC..0xDF` |
| Shuma-Gorath (`Shuma`) | `0xDC..0xDE` |
| Dormammu | `0xDC..0xDE` |

These are shared hyper-activation effects, not proof that each associated beam,
projectile or trail has its own Filter generator. The examined characters'
core hyper families, using the English command-list names, are:

- Ryu: Shinku Hadoken, Shinku Tatsumaki Senpukyaku, Hado Kakusei and Shin Shoryuken.
- Akuma: Messatsu Gohado, Tenma Gozanku, Messatsu Goshoryu and Raging Demon.
- Nemesis: Bioweapon Assault, Biohazard Rush and Fatal Mutation.
- Thor: Mighty Tornado, Mighty Thunder and Mighty Punish.
- Shuma-Gorath: Hyper Mystic Smash, Hyper Mystic Ray and Chaos Dimension.
- Dormammu: Chaotic Flame, Stalking Flare and Dark Dimension.

The name source is `ui/game_msg.arc`, `msg_commandlist_<character>_en` MSD
entries. The native command matcher uses action-success step type 9 with a
base/action pair; `sAction__lookupCharSlotTable` uses table `0x140A6F8C0`, whose
hyper base is 220 (`0xDC`). This audit does not supply an individual name for
every alternate/follow-up action in the ranges above.

## Additional character resources

The following table records matching Filter groups, not just any reference to
a resource containing a Filter. Other groups of the same resource can exclude
the Filter. Actual execution also depends on material/volume and draw gates.

| Resource | Filter entries | Matching animation use | Human-readable identification |
|---|---|---|---|
| `chr\Ryu\effect\efl\0019` | 3, group `4` | Action `0xE1`, frame 125, command `0x22B1E`, group `4` | Specific action/move label not independently established here |
| `chr\Gouki\effect\efl\0023` | 32, group `8` | Action `0x89`, frame 135, command `0x832B`, group `8` | Akuma victory-pose effect, not evidence for Raging Demon's unique effect |
| `chr\Nemesis\effect\efl\0023` | 0, group `1` | Actions `0xDF` / `0xE0`, frame 350, commands `0x19AB2` / `0x1A8B3`, group `1` | Specific action/move label not independently established here |
| `chr\Nemesis\effect\efl\1016` | 0 and 56 | Action `0x89`, frames 121 / 278, body-scoped resource 16, groups `1` / `2` | Nemesis victory-pose effect |
| `chr\Thor\effect\efl\0019` | 0, group `0xFFFFFFFF` | Action `0x10B`, frame 145, command `0x26AB1`, group `0xFFFFFFFF` | Specific action/move label not independently established here |
| `chr\Shuma\effect\efl\1010` | 33, group `1` | Actions `0x108` / `0x10F`, frame 20, body-scoped resource 10, group `1` | Specific action/move label not independently established here |
| `chr\Shuma\effect\efl\9999` | 3, 14, 25, 36 | No direct use identified in examined character CAC commands | Unmapped; not proven unused |
| `chr\Dormammu\effect\efl\0043` | 7, group `2` | Action `0x108`, frame 75, command `0x22A41`, group `2` | Specific action/move label not independently established here |

Common winner action `0x89` is established by `sAction__stepResult8` and
`outro.md`. Do not assign other action names from texture names, resource
numbers, or nearby native shot classes alone. For example, native DarkDimension
shot setup selects Dormammu `0040`, which does not identify the separately
scripted `0043` resource.

## Other common resources

Filter-containing common resources are `0017`, `0021`, `0100`, `0106` and
`9998`. Resource `0106` is the native companion to the comic-page/circular-tear
transition family; see `super_presentation_resim.md`. Names/triggers for `0017`,
`0021` and `9998` remain unresolved. A high resource number is not sufficient
evidence that a resource is debug-only or unused.

## Animation command evidence

The native group-3 interpreter `0x1401038E0` confirms direct effect spawn
opcodes 0-4. A command has u32 group/opcode at `+0` / `+4`, descriptor-word
count N at `+8`, and packed payload at `P = command + 0x10 + 4*N`.
The payload begins with a 64-byte class name, followed by scope/resource at
`P+0x40` / `P+0x44` and owner selectors at `P+0x48` / `P+0x4C`.
For opcode 3, factory group flags are at `P+0x84`; opcode 2 uses `P+0x88`,
and opcodes 1/4 use `P+0x78`.

CAC action and block directories, rather than byte-pattern matches, were used
to locate commands. Scope formatter `0x140130750` confirms scope 0 is common,
100 is character-common, and 150 adds `(bodyType+1)*1000` to the resource ID.
Resource presence is static evidence, not a runtime invocation count.
