# Native Pad Input

**Status: Native reference with current-plugin notes. Reviewed 2026-07-23.**
`sPad` performs hardware polling and produces game input masks. The rollback
timeline is applied later by `controller.cpp` at
`cChrInput__processFrameInput`; live `sPad` state is not authoritative history.

## Address Conventions

`sPad` embeds four hardware slots beginning at object offset `+0x58`, with
stride `0x2E0`:

```c
slotObject = sPad + 0x58 + slotIndex * 0x2E0;
```

Some native getters instead use offsets relative to the `sPad` object:

```c
currentButtons = *(uint32_t *)(sPad + 0x1BC + slotIndex * 0x2E0);
```

These identify the same field because `0x58 + 0x164 == 0x1BC`. Tables below
state which origin they use; older notes mixed these origins.

## `sPad` Layout

| Object offset | Verified role |
|---:|---|
| `+0x38` | Enables decoded XInput/DirectInput input; polling clears button state when disabled |
| `+0x40` | Previous millisecond timer sample |
| `+0x48` | Millisecond delta used by vibration/repeat processing |
| `+0x58` | First of four `0x2E0` hardware slots |
| `+0xBE0` | Vibration/capability-related global byte initialized by the constructor |
| `+0xBE2` | Keyboard mapping/configuration byte |
| `+0xBE3` | Deferred per-frame poll flag |
| `+0xBE8` | `IDirectInput8` interface pointer |
| `+0xBF0` | Four DirectInput device pointers |
| `+0xC20` | Keyboard blocking state consulted by keyboard readers |
| `+0xC30` | Keyboard blocking state consulted by the main input getter |

The constructor is `sPad__sPad_ctor` at `0x14050FBA0`; the singleton pointer is
at `0x140E174B8` and the getter is at `0x140001B00`.

## Slot Layout

Offsets in this table are relative to `slotObject` (`sPad+0x58+index*0x2E0`):

| Slot offset | Type | Verified role |
|---:|---|---|
| `+0x08` | byte | Device-active/present state |
| `+0x09` | byte | Device readiness/state byte |
| `+0x0A` | byte | Slot index |
| `+0x0B` | byte | Device kind: 0 triggers detection, 2 is DirectInput, other active kinds use XInput |
| `+0x10` | `uint32_t` | Detection capability/selection flags |
| `+0x18` | `uint32_t` | XInput or DirectInput device index |
| `+0x1D` | byte | Trigger threshold |
| `+0x1E` | `int16_t` | Analog-stick threshold |
| `+0x164` | `uint32_t` | Current decoded buttons (`sPad+index*0x2E0+0x1BC`) |
| `+0x168` | `uint32_t` | Previous decoded buttons |
| `+0x16C` | `uint32_t` | Rising-edge buttons |
| `+0x170` | `uint32_t` | Falling-edge buttons |
| `+0x174` | `uint32_t` | Changed-button mask |
| `+0x178` | `uint32_t` | Repeat-button mask/state |
| `+0x17C..+0x188` | `int32_t[4]` | Raw right/left stick axes |
| `+0x18C/+0x18D` | bytes | Raw right/left trigger values |

## Hardware Polling

`sPad__perFrameUpdate` at `0x140514570` updates the global time delta and visits
all four slots. Active slots call `sPad__pollActiveDevice` at `0x140514B90`; a
successful poll then runs button/vibration post-processing.

`sPad__pollActiveDevice` dispatches by `slot+0x0B`:

- Kind 0 invokes rate-limited device redetection.
- Kind 2 polls an `IDirectInputDevice8`, reads a `0x110`-byte device state, and
  decodes it when `sPad+0x38` is enabled.
- Other active kinds call `XInputGetState` and decode the returned state when
  `sPad+0x38` is enabled.
- A successful hardware poll while `+0x38` is disabled clears
  `slot+0x164..+0x18F`.
- A failed poll closes/resets the slot handle.

Native redetection at `0x140514950` commonly assigns active XInput slots a kind
greater than 3. Documentation should not claim that the poller checks exactly
kind 4; its native branch is `kind != 0 && kind != 2`.

## XInput Translation

`sPad__decodeXInputState_toButtons` at `0x1405111B0` converts XInput state into
the game's internal mask at `slot+0x164`:

| Internal bit | Verified XInput source |
|---:|---|
| `0x000001` | Back |
| `0x000002` | Left thumb |
| `0x000004` | Right thumb |
| `0x000008` | Start |
| `0x000010` | D-pad up |
| `0x000020` | D-pad right |
| `0x000040` | D-pad down |
| `0x000080` | D-pad left |
| `0x000100` | Left shoulder |
| `0x000200` | Right shoulder |
| `0x000400` | Left trigger above threshold |
| `0x000800` | Right trigger above threshold |
| `0x001000` | Y |
| `0x002000` | B |
| `0x004000` | A |
| `0x008000` | X |
| `0x010000/0x020000/0x040000/0x080000` | Left-stick up/right/down/left above threshold |
| `0x100000/0x200000/0x400000/0x800000` | Right-stick up/right/down/left above threshold |

It also stores the prior mask, derives rising/falling/change masks, and updates
repeat state.

## Game Input Getter

`sPad__getPlayerInput_withSOCDClean` at `0x1402B3700` accepts a slot index:

1. Indices above 3 return zero.
2. If the requested index equals the manager's player-0 mapping and keyboard
   input is not blocked at `sPad+0xC30`, it returns keyboard translation instead
   of the hardware slot.
3. Otherwise it reads
   `sPad + slotIndex * 0x2E0 + 0x1BC` and applies SOCD cleaning.

The keyboard and pad sources are alternatives in this native getter, not an
automatic OR. Current plugin capture may explicitly combine sources where its
role/mode policy requires that behavior.

`0x1402B3950` is a related getter that reads object-relative `+0x1D0` instead of
`+0x1BC` and uses an alternate keyboard translator. Intro code that calls this
family should not be described as reading the same field without checking the
exact callee.

## SOCD Rules

The `0x1402B3700` getter applies these exact mask transformations in order:

```c
if ((input & 0x00080020) == 0x00080020) input &= ~0x00000020;
if ((input & 0x00020080) == 0x00020080) input &= ~0x00020000;
if ((input & 0x000000A0) == 0x000000A0) input &= ~0x00000020;
if ((input & 0x000A0000) == 0x000A0000) input &= ~0x00020000;
if ((input & 0x00040010) == 0x00040010) input &= ~0x00040000;
if ((input & 0x00010040) == 0x00010040) input &= ~0x00000040;
if ((input & 0x00000050) == 0x00000050) input &= ~0x00000040;
if ((input & 0x00050000) == 0x00050000) input &= ~0x00040000;
```

This resolves D-pad/analog cross-conflicts as well as conflicts within one
direction source. A short statement such as "left wins and up wins" omits half
of the tested combinations.

## Training Boot

The current training boot does not force every slot to kind 4 or synthesize a
connected player-2 device. `SetupPadToPlayerMapping` currently:

- Maps manager slots 0 and 1 to players 0 and 1, leaving slots 2 and 3
  unassigned.
- Clears keyboard-block bytes at `sPad+0xC20` and `sPad+0xC30`.
- Reinitializes the player-1 translation map through `0x1405142A0`.
- On a netplay client, copies both resolved P1 gameplay binding tables to P2
  through the native `sGameConfig` setters before P2 input is transmitted.

Comments showing forced player-2 active flags are disabled code and must not be
reported as current behavior.

## Rollback Boundary

`controller.cpp` hooks `cChrInput__processFrameInput` at `0x14002DD60` to scope
timeline ownership to the character currently being processed. A nested hook
on `cChrInput__buttonRemap` at `0x14002DA30` captures local input after physical
button bindings become logical actions and substitutes delayed or remote input
on the ordinary pad path. A second scoped hook at
`cChrInput__swapLRIfFaceLeft` (`0x14002E560`) enforces the timeline for native
action/CPU sources that bypass `buttonRemap`. The rest of native processing
remains authoritative for facing conversion, ability inversion, team state,
pause suppression, and final per-character fields. Re-simulation injects the
same canonical pre-facing logical values from snapshots.

The attached `uChrCpu` source reaches the first facing-conversion call with its
fresh logical mask at return site `0x14002DE57`. The owning peer captures that
specific value into the local timeline. Other calls that bypass `buttonRemap`
are substituted but not captured, preventing replay or special-state values
from becoming new network input.

The native function gates character input through
`cChrInput__isGameplayActive` at `0x14002DCE0`. Member 0 is accepted directly.
A reserve member is accepted only while eligible for a team transition, or
while native active/local-control rules select it. An ordinary on-screen assist
fails this gate after native processing clears its input. Because the timeline
hooks run only inside the native function after this gate, they cannot
repopulate an assist after its early return. The explicit predicate retained by
the outer hook protects the separate offline post-native autotest path.

## Key Addresses

| Address | Function |
|---|---|
| `0x140001B00` | `sPad__getInstance` |
| `0x14002DA30` | `cChrInput__buttonRemap` |
| `0x14002DCE0` | `cChrInput__isGameplayActive` |
| `0x14002DD60` | `cChrInput__processFrameInput` |
| `0x14002E560` | `cChrInput__swapLRIfFaceLeft` |
| `0x1402B3700` | Main slot/keyboard getter with SOCD cleaning |
| `0x1402B3950` | Alternate slot/keyboard getter with SOCD cleaning |
| `0x14050FBA0` | `sPad` constructor |
| `0x1405111B0` | Decode XInput state to internal button mask |
| `0x1405142A0` | Initialize a pad translation map |
| `0x140514570` | Per-frame four-slot poll |
| `0x140514950` | Redetect a hardware slot |
| `0x140514B90` | Poll the active device for one slot |
