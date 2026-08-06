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
`sPad` state is outside this note's verified scope. Gameplay button bindings
are separate again: `cChrInput__buttonRemap` reads them from `sGameConfig` via
`0x1402522A0`. The native option menu writes an 11-entry player table with
`0x140254A20` and selects that custom table with `0x140254C40`.

The title/menu save-data path starts a full `sGameConfig` load through
`0x140253620`. That function creates a `uStoragePC`, registers the `0x2550`-byte
blob at `sGameConfig+0x4AE00`, installs apply callback `0x140253370`, and sets
request 6. Its caller schedules the returned unit in `sUnit` category `0x19`.
Calling the lower-level `sSavedata` request at `0x14075B970` without this
registration and callback does not load the live gameplay binding tables.

The stock apply callback copies the complete saved profile and then calls
`0x1402555B0`, which both resolves per-player binding pointers and immediately
updates seven live audio gains. Direct training boot keeps the native storage
unit but replaces this callback. It imports only P1's two 11-entry gameplay
binding tables, reads the saved P1 selector fields, and resolves the active
table through `0x14024F2C0`. This avoids importing unrelated voice, announcer,
presentation, and audio state.

## Plugin Boundary

The plugin reads live translated input but owns netplay history in
`controller.cpp`. It indexes local and remote values by custom frame number and
injects them at `cChrInput__processFrameInput`. Restoring either native pad
singleton wholesale would rewind hardware/configuration ownership and is not
the rollback design.
