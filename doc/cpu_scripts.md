# Fighter CPU script system

Verified in Ghidra against `ichneumon_sub/umvc3.exe.unpacked.exe` on
2026-08-23. Addresses use the preferred image base `0x140000000`.

## Counts

The executable has two global reaction-selector tables used by
`uChrCpu__buildReactionCandidates` at `0x140119EB0`:

- Normal table at `0x140C5DBC0`: 70 entries, followed by a `0xFFFFFFFF`
  sentinel.
- Restricted fallback table at `0x140C5DF18`: 3 entries, followed by a
  `0xFFFFFFFF` sentinel.

These are reaction-condition entries, not the character action scripts.

The interpreter supports 210 named opcodes across eight groups. The exact
group sizes are returned by `rChrCpu__getOpcodeCount` at `0x14011FCD0`:

| Group | Category | Opcodes |
|---:|---|---:|
| 0 | Action and interpreter control | 20 |
| 1 | Movement and directional input | 19 |
| 2 | Attack-button and motion sequences | 11 |
| 3 | Waits and decision operations | 23 |
| 4 | Character/state flag operations | 17 |
| 5 | Branches, calls, and weighted selection | 34 |
| 6 | Character, opponent, projectile, and game-state queries | 62 |
| 7 | Character-specific queries | 24 |
| | Total | 210 |

There is no fixed action-script count in the executable. `rChrCpu` resources
are loaded separately from `chr\\cmn\\cpucmn` and
`chr\\<character>\\cpuchr`, and each resource header supplies its own action
count at runtime.

The installed `nativePCx64/chr/archive` assets were scanned and decompressed on
2026-08-23. They contain:

- 57 CPU resources: the common resource, 50 playable-character resources,
  Galactus, and five helper-character resources.
- 11,646 action scripts in total.
- 158,681 command records in total.
- 4,978,908 bytes of decompressed CPU resource data.

The helper resources are `FeliciaC`, `FeliciaF`, `Mayoi`, `RedArremerSh`, and
`Zombie`. The totals describe the complete resource corpus; the game does not
load every resource for one fighter. For example, Ryu's character resource has
209 actions and 2,929 commands, while the reused common resource has 246
actions and 3,147 commands. A Ryu CPU therefore draws from 455 available action
records across those two resources before considering alternate `excpu` data.

## Resource format

`rChrCpu__convert` at `0x14005BC20` loads a complete resource into one
contiguous allocation stored at `rChrCpu+0x70`. Recovered accesses establish
this layout:

```text
resource blob
  +0x00  format identifier
  +0x04  format version
  +0x08  action count
  +0x10  action index entries, 0x10 bytes each

action index entry
  +0x0C  blob-relative action-record offset

action record
  +0x04  action type (accepted values are 1 through 4)
  +0x0C  action name storage
  +0x4C  command count
  +0x50  command index entries, 8 bytes each

command index entry
  +0x04  blob-relative command-record offset

command record
  +0x00  opcode group (0 through 7)
  +0x04  opcode within the group
  +0x08  opcode-specific parameter payload
```

Relevant accessors are:

- `rChrCpu__getActionCount`: `0x14005BA70`
- `rChrCpu__getActionIndexRecord`: `0x14005BA90`
- `rChrCpu__getActionRecord`: `0x14005BA40`
- `rChrCpu__getCommandIndexRecord`: `0x14005BB20`
- `rChrCpu__getCommandRecord`: `0x14005BAC0`
- `rChrCpu__getCommandParams`: `0x14005BB60`

## Script language

The opcode-name tables show that the format is a small assembly language, not
a simple behavior tree. It includes:

- Movement and input generation such as `Pad`, `Atk Pad`, `Super Jump`,
  `Forward Step`, `Back Step`, special-move motions, assists, and guard input.
- Blocking operations such as `Wait`, `Rand Wait`, distance/height waits,
  animation waits, hit/guard waits, and attack-end waits.
- Control flow such as branches to actions or labels, calls, returns, loops,
  branch tables, and weighted or difficulty-weighted selections.
- State operations that test character flags and manipulate CPU work flags.
- Queries for distance, health, meter, animation, projectiles, throws, X-Factor,
  assists, tags, screen position, movement, and other combat state.
- Character-specific tests for Arthur, Amaterasu, Dormammu, Doctor Strange,
  Phoenix Wright, Frank West, Ryu, Wolverine, Firebrand, Strider, Vergil,
  MODOK, and related special mechanics.

`uChrCpu_processScript` at `0x140116C40` executes commands until the current
step blocks/completes. Immediate branch, state, and query instructions can all
run in one game frame. A hard limit of 10,000 interpreter iterations prevents
an invalid immediate-instruction loop from hanging the game.

The global reaction selector chooses an action ID. The selected resource action
then performs lower-level decisions and emits ordinary direction/button input,
assist/tag requests, waits, or another action transition. Difficulty is mostly
implemented through random values compared against difficulty-indexed tables
and through the weighted branch opcodes.
