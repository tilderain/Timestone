# mPrgF

**Status: Current focused native-field reference. Reviewed 2026-07-23.** Only
the face-left bit has a general-purpose meaning confirmed by a direct gameplay
reader. Other labels below are intentionally confidence-qualified and must not
be promoted to universal names without auditing their readers and writers.

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
| `0x00200000` | Confirmed input-facing flag. `cChrInput__swapLRIfFaceLeft` tests this bit and exchanges command-input bits 0 and 1. |
| `0x00400000` | Modified by linked-character facing logic. A general semantic name is not established. |
| `0x00600000` | Combined mask written by `cChr__faceLinkedCharacter`; it contains `0x00200000` and `0x00400000` and is not a separate bit. |
| `0x00000001`, `0x00000002`, `0x00000004` | Low program-state bits. Their individual names are not verified. The value `0x00000006` was consistently present during an observed airborne movement sequence. |
| `0x00000080` | Observed in `0x00600184`, but absent from `0x00600006`. Exact semantic meaning is not verified. |
| `0x00000100` | Observed in `0x00600184`, but absent from `0x00600006`. Exact semantic meaning is not verified. |
| `0x00800000` | Observed in `0x00E00006`. Exact semantic meaning is not verified. |
| `0x02000000` | Observed in `0x02600006`. Exact semantic meaning is not verified. |
| `0x04000000` | Observed during a transition to `0x04600006`. Exact semantic meaning is not verified. |

The `0x00400000`, `0x00800000`, `0x02000000`, and `0x04000000` bits are not
currently proven to represent Dark Phoenix or any other specific gameplay condition.
They are distinct flags in the same program-state word. Their purposes must be
determined from their individual readers and writers rather than inferred from
the combined values in which they appear.

## Confirmed Use

Bit `0x00200000` is the character's face-left flag. The game reads it in
`cChrInput__swapLRIfFaceLeft` at `0x14002E560`:

```c
if ((character->mPrgF & 0x00200000) != 0) {
    // Exchange command-input bits 0 and 1.
}

if ((input & 3) == 3) {
    input &= ~2;
}
```

This makes `mPrgF` part of input processing. The function exchanges bits 0 and
1 when the flag is set, then resolves the simultaneous `0x3` result by clearing
bit 1. Calling those bits logical forward/backward is plausible from the
function's role, but the bit meanings are not independently established here.

## Other Verified Behavior

`cChr__faceLinkedCharacter` at `0x140050A90` directly modifies `mPrgF` after it
resolves a linked character through throw/relationship state:

```c
if ((character->mNo2F & 0x00100000) == 0 &&
    (character->mPrgF & 0x00008000) != 0) {
    character->mPrgF ^= 0x00400000;
}

if (linkedStatePath) {
    if ((linkedFlags & 0x00400000) == 0) {
        character->mPrgF |= 0x00600000;
    } else {
        character->mPrgF &= ~0x00600000;
    }
}
```

The same function computes and applies a facing angle. This establishes that
both high bits participate in linked-character orientation state. It does not
establish `0x00400000` as a universal transformation flag.

## Interpretation

The safe interpretation of `mPrgF` is:

> A character-local program-state bitmask used by input-facing processing,
> linked-character orientation logic, and character-specific state machines.

Only bit `0x00200000` currently has a confirmed general-purpose name: the
face-left/input-mirroring flag. The remaining bits should be decoded from their
individual readers and writers before assigning names.
