# `sPad` and `sMvc3Pad`

**Status: Native reference. Reviewed 2026-07-23.** These are distinct singleton
objects with related pad layouts and responsibilities. Neither is the custom
rollback input-history authority.

## `sPad`: Live Hardware State

| Item | Address/evidence |
|---|---|
| Singleton pointer | `0x140E174B8` |
| Getter | `0x140001B00` |
| Constructor | `0x14050FBA0` |
| Vtable | `PTR_sPad__dtor_140BA5E60` |

`sPad` derives from `cSystem`, embeds four `sPad::Pad` objects at `+0x58`, and
polls XInput/DirectInput hardware. Each embedded object has native stride
`0x2E0`, not `0x58`. Current decoded input for slot `n` is at
`sPad + 0x1BC + n * 0x2E0`.

The native reflection names and polling accesses establish global properties
such as `mXInput`, `mDecideButton`, `mCancelButton`, repeat defaults, and the
four-element `mPad` array. See [`pad.md`](pad.md) for the verified hardware-slot
layout and translation flow.

## `sMvc3Pad`: Saved Controller Configuration

| Item | Address/evidence |
|---|---|
| Singleton pointer | `0x140E1B3E0` |
| Constructor | `0x14075B620` |
| Destructor clears singleton | `0x14075B730` |
| DTI getter | `0x140289440` |
| Property registration | `0x1402B2F80` and `0x140513720` |
| Vtable | `PTR_sMvc3Pad__dtor_140BBC450` |

`sMvc3Pad` is another `cSystem`-derived object. Its constructor installs a
different vtable, initializes a storage-thread subobject, and writes a different
singleton pointer. Its state machine loads and validates persisted controller
configuration through Steam remote-storage interfaces, including encrypted
stream handling and checksums.

Recovered reflection registers these properties:

| Property | Offset | Evidence |
|---|---:|---|
| `mActive` | `+0x38` | Reflected Boolean/state property |
| `mSysRepeadStDefault` | `+0x4C` | Repeat-start default |
| `mSysRepeadNxDefault` | `+0x50` | Repeat-next default |
| `mPad` | `+0x58` | Four reflected `sPad::Pad` elements |
| `mDecideButton` | `+0xBD8` | Confirm-button setting |
| `mCancelButton` | `+0xBDC` | Cancel-button setting |
| `mXInput` | `+0xBE0` | XInput setting |

The property array begins at `+0x58`; that does not mean its element stride is
`0x58`. The element DTI/layout gives the same large pad-record shape reflected
by native accesses.

## Relationship

The objects do not share an address and one is not merely an alias for the
other:

```text
sMvc3Pad
  persisted controller settings and load/save state
       |
       v
sPad
  live hardware polling, decoded slot masks, edges, repeat, vibration
       |
       v
sMvc3Manager mapping + cChrInput processing
  routes translated input to gameplay
```

The exact function that copies every persisted `sMvc3Pad` field into live
`sPad` state is outside this note's verified scope. The safe distinction is
persisted game-specific controller configuration versus live hardware state,
not the older claim that `sMvc3Pad` has unrelated `0x58`-stride slots.

## Plugin Boundary

The plugin reads live translated input but owns netplay history in
`controller.cpp`. It indexes local and remote values by custom frame number and
injects them at `cChrInput__processFrameInput`. Restoring either native pad
singleton wholesale would rewind hardware/configuration ownership and is not
the rollback design.
