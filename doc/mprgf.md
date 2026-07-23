# mPrgF

`mPrgF` is the 32-bit **program-flags** field in `uCharacter`:

```text
uCharacter + 0x14F8
```

The field is registered by the game as `cChr::mPrgF` in
`cChr__createProperty` at `0x1400511D0`:

```c
MtProperty__createU32(..., this, "mPrgF", &this->mPrgF, ...);
```

It is a bitmask containing character program and runtime-state flags. It is not
a single Boolean and should not be interpreted as only a facing value.

## Known Bits

| Bit | Meaning or evidence |
|---|---|
| `0x00200000` | Confirmed face-left flag. `cChrInput__swapLRIfFaceLeft` tests this bit and swaps left/right input. |
| `0x00400000` | Persistent character-program state bit. Existing project analysis identifies it as Phoenix's Dark Phoenix state, although its interpretation may be character-specific. |
| `0x00600000` | Combined face/program-state mask written by `cChr__faceLinkedCharacter`. This is a mask containing `0x00200000` and `0x00400000`, not a separate bit. |
| `0x00000001`, `0x00000002`, `0x00000004` | Low program-state bits. Their individual names are not verified. The value `0x00000006` was consistently present during an observed airborne movement sequence. |
| `0x00000080` | Observed in `0x00600184`, but absent from `0x00600006`. Exact semantic meaning is not verified. |
| `0x00000100` | Observed in `0x00600184`, but absent from `0x00600006`. Exact semantic meaning is not verified. |
| `0x00800000` | Observed in `0x00E00006`. Exact semantic meaning is not verified. |
| `0x02000000` | Observed in `0x02600006`. Exact semantic meaning is not verified. |
| `0x04000000` | Observed during a transition to `0x04600006`. Exact semantic meaning is not verified. |

The `0x00800000`, `0x02000000`, and `0x04000000` bits are not currently proven
to represent facing, Dark Phoenix, or any other specific gameplay condition.
They are distinct flags in the same program-state word. Their purposes must be
determined from their individual readers and writers rather than inferred from
the combined values in which they appear.

## Confirmed Use

Bit `0x00200000` is the character's face-left flag. The game reads it in
`cChrInput__swapLRIfFaceLeft` at `0x14002E560`:

```c
if ((character->mPrgF & 0x00200000) != 0) {
    // Swap the left and right directional input bits.
}
```

This makes `mPrgF` part of input processing. When the bit is set, directional
input is mirrored so that the character's logical forward and backward
directions remain correct while facing left.

## Other Verified Behavior

`cChr__faceLinkedCharacter` at `0x140050A90` directly modifies `mPrgF`. It can:

- Toggle bit `0x00400000` based on linked-character state.
- Set or clear the combined mask `0x00600000`.
- Update the face-left bit as part of linked-character facing logic.

Project analysis also associates bit `0x00400000` with Phoenix's Dark Phoenix
program state. That association is character-specific; the general meaning of
the bit is a program-state flag, not universally a transformation Boolean.

## Interpretation

The safe interpretation of `mPrgF` is:

> A character-local program-state bitmask used by movement/input processing,
> facing logic, linked-character logic, and character-specific state machines.

Only bit `0x00200000` currently has a confirmed general-purpose name: the
face-left/input-mirroring flag. The remaining bits should be decoded from their
individual readers and writers before assigning names.
