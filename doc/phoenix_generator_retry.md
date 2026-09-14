# Phoenix generator-construction retry — static trace, 2026-09-13

## Finding

Dark Phoenix's persistent slot `0xF` requests character EFL `0023` with group
mask `2`. All 41 entries in the installed `0023` resource have group mask `1`.
Consequently this request selects zero generators, without attempting any
generator allocation. Native lifecycle code makes the failed effect eligible
for replacement on the next character tick. This is a deterministic native
retry path with these assets, not evidence of memory exhaustion.

Normal Phoenix's slot `0x11`, EFL `0029`, does not have this mask mismatch.
Its 19 entries all pass the supplied group/material masks. This does not prove
all later resource-resolution, initialization, or allocation stages succeed.

## Inputs and reproducibility

Ghidra: `ichneumon_sub / umvc3.exe.unpacked.exe`, image base `0x140000000`.
Resource inspected read-only:
`../ULTIMATE MARVEL VS. CAPCOM 3/nativePCx64/chr/archive/0036_cmn.arc`.
These are installed assets; no pristine-distribution comparison was performed.
The same five resource paths were absent from `0036_00.arc` through
`0036_07.arc` and `0036_param.arc`.

ARC header: `ARC\0`, version 7; u16 entry count at +6; entries start at +8
with stride 80. Each entry has a 64-byte name followed by four u32s: type,
compressed size, packed size/flags, file offset. Selected payloads were
decompressed with zlib in memory.

EFL layout was checked against `rEffectList__convert` (`0x1405EDEF0`) and
`rEffectList__resolveNodeResources`: header magic `EFL\0`, version
`0x20120306`, node count u16 at +0x10, payload beginning at +0x30. Each
payload node is 16 bytes. Its first u32 shifted right by 8 is a payload-relative
aux-record offset; the first two u32s of that record are group/material masks.

All five inspected EFL headers have mode word `0x100`, whose low nibble is
zero: these requests take the ordinary generator-list branch, not the
aggregate-generator branch.

| Phoenix slot | EFL (decimal filename) | Request group mask | Node count | Passing group/material masks, either facing |
|---|---|---|---|---|
| `0xE` | `0023` | `1` | 41 | 41 |
| `0xF` | `0023` | `2` | 41 | **0** |
| `0x10` | `0022` | `0xFFFFFFFF` | 46 | 46 |
| `0x11` | `0029` | `0xFFFFFFFF` | 19 | 19 |
| `0x13` | `0017` | `1` | 45 | 45 |
| `0x15` | `0040` | `0xFFFFFFFF` | 9 | 9 |

All entries in these resources have material mask `0xFFFFFFFF`. Phoenix's
facing-selected request mask is `0x7FFFFFFF` or `0xBFFFFFFF`, so facing does
not change these results. The separate runtime disable-mask gate can exclude
additional entries; it cannot make slot `0xF`'s disjoint group masks intersect.

Decompressed EFL SHA-256:

```text
0017 bf17c55fca590a82a82a418789c70edfe543461c1d66641c1b61f74ec26f9bdd
0022 7806c5da93c9b8cf4426532e851a6b9975e27c28adb5ad6a91f93c2721d353ca
0023 ab94428306fbd88d6ccb79a26275cdb600413985324a102ff27dc9a63396372f
0029 d8d8219415e0e82fbb24476749ac288721290193a16b5cabcf042f381ee5ebed
0040 059276d9d7998367d0e5bde3c32d529def2d1b9ec8dac1ae5cf2158718c5785b
```

## Native call and lifecycle trace

1. `uPhoenix__tick` (`0x1400BBBA0`) calls base-character move before its
   persistent-effect checks. In the Dark Phoenix branch it requests slots
   `0xE`, `0xF`, and `0x10` each eligible tick. Early-return/state conditions
   still apply.
2. `uPhoenix__effectUpdate_9B80` (`0x1400B9B80`) returns an existing slot
   pointer, otherwise spawns. Assembly at `0x1400B9BFA..0x1400B9C09` selects
   variant `0x17` (decimal 23), group `1 << (slot - 0xE)` for slots E/F.
   The full stack arguments, missing from the four-argument decompilation,
   carry group/material masks at call site `0x1400B9C8A`.
3. `sGameEffect__formatResourcePath` maps scope 100 to
   `chr\\Phoenix\\effect\\efl\\0023` without costume-number remapping.
4. `sGameEffect__spawnFromParentQuaternion` (`0x140131410`) obtains the
   resource and allocates a `0x2C0`-byte `uGameEffect`. Spawn finalization
   writes group/material masks before binding the EFL through virtual +0xD0.
5. `uEffect__setEffectList` (`0x1405E6C40`) invokes virtual +0x198
   (`uEffect__createGenerator`, `0x1405E27C0`) when mode bit 4 is clear.
   Fresh ordinary effects have mode `0x100001`; the inspected setup path
   does not enable keep mode. The construction return is ignored here.
6. `uEffect__isEffectListEntryEnabled` (`0x1405E25F0`) requires both
   `(entryGroup & effectGroup) != 0` and
   `(entryMaterial & effectMaterial) != 0`, after a disable-mask check.
   For slot F every group intersection is `1 & 2 == 0`.
7. `uEffect__buildGeneratorObjectList` (`0x1405E2860`) visits the 41 entries,
   creates no nodes, sets `effect+0x100` bit `0x01000000`, and returns false.
   This bypasses the generator allocation/constructor switch and the later
   particle-manager/buffer allocation stage. `createGenerator` releases the
   empty graph and leaves ready bit `effect+0x130 & 0x01000000` clear.
8. The factory still returns the effect object. Phoenix sets owner/control
   flags, adds it to category 13, and stores it in slot F.
9. `uEffect__move` (`0x1405E5720`) sees the unset ready bit, tries construction
   again, and takes `uBaseEffect__sleepOrKillOnCompletion` (`0x1405EF2B0`)
   on failure. Ordinary non-keep, non-fixed effects enter retiring state 3
   through `uEffect__kill` (`0x140016F20`).
10. On the next base-character move,
    `uCharacter__clearDeadAuxiliaryRefs` (`0x140055970`) nulls slot F because
    `MtObject__isAliveState` only accepts states 1/2. Phoenix can immediately
    spawn another object. Deferred destruction of the old object need not
    have finished for this replacement to occur.

Thus the normal eligible path predicts two unsuccessful construction calls
per replacement: one during spawn/resource binding and one during effect
move. Precise observed counts depend on scheduler/state gates; no runtime
frequency measurement was made.

## Interpretation for rollback diagnostics

The current `hkEffectCreateGenerator` logs `EFFECTALLOCFAIL` for every false
return, including the zero-eligible-entry case. For this slot the label is
misleading: the EFL has 41 source entries but zero selected generators, and
neither generator-object nor particle-buffer allocation is attempted.

Replaying the native lifecycle can multiply empty-object creation, eligibility
scans, retirement, and failure logging. Static analysis does not establish how
much of the reported rollback slowdown this accounts for. It also does not
establish the same failure for normal Phoenix's EFL `0029`.
