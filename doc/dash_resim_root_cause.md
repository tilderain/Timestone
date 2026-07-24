# Dash Loss During Rollback Re-Simulation

**Status: Corrected historical investigation. Ghidra-verified 2026-07-23.**

This document preserves the useful findings from the original investigation but
removes contradictory call-order reconstructions and field semantics that the
instructions do not support. Session-specific conclusions describe the old
plugin build used for those captures, not current source behavior.

## Verified Native Input Boundary

`cChrInput__processFrameInput` begins at `0x14002DD60`. Its `this` pointer is the
embedded `cChrInput`; the owner is read from `this+0x08`.

Verified writes include:

| Offset | Instruction evidence |
|---|---|
| `+0x10` | Previous/current edge-related input word; initially receives old `+0x14` |
| `+0x14` | Current processed input word |
| `+0x560` | Intermediate/remapped input word |
| `+0x568` | Suppression mask used near the function tail |
| `+0x56C` | Final companion input word written on all major paths |

Names such as `mOldHeld`, `mHeld`, `mRemapped`, and `mProcessed` are useful
working names, but exact script-facing semantics should be established from each
reader before treating them as ABI names.

The native resolver contains distinct paths for physical pad input, replay
record/playback, and retail online input. The retail-online call at
`0x14002E272` invokes `0x140289520` only after predicates at `0x140006A60` and
the object returned by `0x140004770` succeed. Whether that branch executes in a
custom ENet match is a plugin/runtime configuration fact, not something Ghidra
alone proves.

## Verified Character Call Order

In the containing character routine at `0x14004BD30`, the relevant sequence is:

```text
0x14004C0A6  virtual call vtable+0x108
0x14004C0C6  call 0x140051040
0x14004C0CB  lea rcx,[character+0x21C8]
0x14004C0D2  call cChrInput__processFrameInput
0x14004C0D7  lea rcx,[character+0x21C8]
0x14004C0DE  call cChrInput_inputMaskPostProcess
0x14004C0E3  lea rcx,[character+0x2950]
0x14004C0EA  call cChrCmd__update
0x14004C0EF  lea rcx,[character+0x2738]
0x14004C0F6  call cChrCmd__update
0x14004C129  call 0x1400394C0
0x14004C148  virtual call vtable+0x140
```

This confirms:

- `cChrInput` is embedded at `uCharacter+0x21C8`.
- Two `cChrCmd` objects are embedded at `+0x2738` and `+0x2950`.
- Input resolution and mask post-processing run before both command updates.
- The earlier claim that the main state-machine decision necessarily runs
  before input processing was wrong. The purpose of vtable slots `+0x108` and
  `+0x140` still needs caller/override analysis; neither should be named solely
  from its position in this sequence.

## Input Initialization

`cChrInput__initInputBuffer` at `0x14002E440` clears substantially more than the
old investigation claimed:

```text
+0x010..+0x01F  scalar input/frame state
+0x028..+0x0A7  0x80-byte block
+0x0AC..+0x12B  0x80-byte block
+0x130..+0x1AF  0x80-byte block
+0x1B0..+0x22F  0x80-byte block
+0x260..+0x2DF  0x80-byte block
+0x2E0..+0x35F  0x80-byte block
+0x360..+0x55F  0x200-byte block
```

It also clears scalar fields around `+0x230..+0x25F` and the words at
`+0x560..+0x56F`. Therefore:

- The old statement that initialization does not clear `+0x360` is false.
- The old names “16-entry u64 rings” and “64-entry direction ring” are not
  established by initialization alone. They are arrays/blocks until their
  readers and writers prove element types and indexing.

## Facing And SOCD

`cChrInput__swapLRIfFaceLeft` at `0x14002E560` is verified:

- It tests owner `uCharacter+0x14F8` bit `0x00200000`.
- When set, it exchanges logical bits `0x1` and `0x2`.
- If both remain set, it clears bit `0x2`.

`sPad__getPlayerInput_withSOCDClean` at `0x1402B3700` is also verified to:

- Reject slot indices above 3.
- Read the processed pad word at `sPad + slot*0x2E0 + 0x1BC` when not using the
  keyboard path.
- Resolve conflicts across digital and stick-direction representations.

The exact human direction attached to every physical/stick bit should come from
the conversion routines, not from the low logical bits used after remapping.

## Root Cause Result

The original investigation eventually found that overriding only `+0x14`,
`+0x560`, and `+0x56C` left `+0x10` on the native physical-input timeline. That
is a valid plugin-level asymmetry if downstream command processing reads
`+0x10`. Ghidra confirms the native function writes and later masks both
`+0x10` and `+0x14`, and confirms both `cChrCmd` updates occur after the hook
boundary.

However, Ghidra alone does not prove that this was the sole cause of every dash
desync. Later work identified heap-backed command-line progress as another
rollback requirement. The corrected conclusion is:

1. Every input word consumed by post-processing and command matching must be on
   the same historical timeline during live and re-simulated frames.
2. Both embedded `cChrCmd` objects and their external mutable buffers must be
   restored consistently.
3. A changed action after corrected remote input may be intended rollback
   behavior rather than nondeterminism.
4. Capture-specific claims require paired live/resimulation logs; static
   disassembly cannot identify which state first diverged at runtime.

## Verified Addresses

| Address | Role |
|---|---|
| `0x14002DD60` | `cChrInput__processFrameInput` |
| `0x14002E440` | `cChrInput__initInputBuffer` |
| `0x14002E560` | Logical left/right facing transform |
| `0x14002E5A0` | Input mask/history post-processing |
| `0x14002DA30` | Physical-to-logical button remapping |
| `0x1402B3700` | Pad/keyboard read with SOCD cleanup |
| `0x14024CFA0` | Retail-online input preprocessing helper |
| `0x14024D500` | Predicate gating that helper |
| `0x140289520` | Retail-online buffered input lookup |
| `0x140028D10` | Per-frame `cChrCmd` update called twice per character |
