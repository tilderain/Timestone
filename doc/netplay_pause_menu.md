# Synchronized netplay pause with owner-local menus

**Investigation: 2026-09-04. Static Ghidra analysis + current plugin source review.**
**Status: proposed implementation, not implemented or runtime-validated.**

## Recommendation

Synchronize the **gameplay stop/resume boundary and pause owner**, not the native
menu cursor or button bindings. Both peers stop at the same confirmed simulation
boundary. Only the owner opens the full native menu; the other peer displays
"Player N paused". The owner can use the native Command List and Controller
Settings without sending menu navigation or configuration changes to the peer.

This assumes the match pauses for both players; "without affecting the other
player" means their bindings and menu input remain independent.

The native pieces exist. The missing work is a custom pause protocol, an
owner-only input path, and integration with rollback/frame scheduling. Simply
creating the native menu or forwarding Start is not sufficient.

## Verified native paths

Addresses are preferred-image VAs; plugin hooks must relocate with `_addr()`.

| Address | Role / evidence |
|---|---|
| `0x1402BA150` | `sAction__openVersusPauseMenu`; full pause object is omitted for native online battle types (see `pause.md`). |
| `0x1402E2E60` | Full `uBattleUiPause` move state machine; dormant state 2 polls open eligibility, state 6 runs menu/submenus, state 9 performs final exit. |
| `0x1402E0130` | `uBattleUiPause__pollOpenBlocked`; returns **0 to accept opening**, 1 otherwise. Polls both sides' Start (`mask & 8`), also handles disconnect/guide-triggered opening. |
| `0x140255150` | Writes cursor owner at cursor `+0x60`. Caller supplies pause owner 0/1. Existing name is `uBattleUiPauseSetCursorPlayer_maybe`. |
| `0x1402DF4B0` | Root-menu/submenu dispatcher; root Start-to-close reads the pause owner's input. |
| `0x1402E1D20` | `uBattleUiPause__openCommandList`; creates `uBattleUiCommandList(owner)` on move line `0x18`. Menu action ID 4. |
| `0x1402D3460` | Command-list constructor stores supplied owner at `+0x700`. |
| `0x1402E1DB0` | `uBattleUiPause__openControlSettings`; creates `uMenuOptionCtrl` on line `0x18`. Menu action ID 5. |
| `0x140440A40` | `uMenuOptionCtrl_vfn29`; polls both sides, edits per-player bindings, supports Start-to-join/leave. |
| `0x1402E10D0` | `uBattleUiPause__pollControlSettingsClosed`; returns to root when child flags `& 6` become zero. |
| `0x1402E13D0` | Pause state-entry side effects, including local scene exits; must not run arbitrary exits independently during netplay. |
| `0x1402E13A0` | Wraps `sGameUI__requestPause(ui, argument ^ 1, 0)`; called on entering state 4 and state 9. |

### Full pause object fields

| Offset | Observed use |
|---:|---|
| `+0x1310` | Root cursor pointer |
| `+0x1368` | Command-list child pointer |
| `+0x1410` | Controller-settings child pointer |
| `+0x1438` | Menu action-ID table pointer |
| `+0x1448` | Main state |
| `+0x144C` | Submenu state: 1 command list, 2 controls |
| `+0x1450` | Selected root row |
| `+0x1458` | Owner side, 0 or 1 |
| `+0x145C` | Root row count |

Do not overwrite the action table globally: different modes select different
tables in `uBattleUiPause__setup` (`0x1402E35A0`). A scoped allowlist at dispatch
is safer than assuming fixed row numbers.

### Controls are already per-player, but ownership is not enforced

The pause controls opener enables `uMenuOptionCtrl+0x2B4` bit `0x2` for owner 0
or `0x4` for owner 1. These bits are **initially active panels, not a lock**.
`0x140440A40` still loops over both players, toggles the respective bit on Start,
and can assign an unassigned P2 controller from physical slots.

When editing, that function passes the loop's player index to:

- `0x140254A20`, `sGameConfig__setPlayerBindingTable`: copies 11 dwords into
  `sGameConfig + 0xC0 + player*0x2C`, or `+0x118 + player*0x2C` for the alternate
  table, and marks configuration for saving.
- `0x140254C40`, `sGameConfig__selectPlayerBindingTable`: selects a layout,
  updates the active binding-table pointer, and marks configuration for saving.
- `0x140254B10`, `sGameConfig__resetPlayerBindingTable`: reset action used by
  the same screen.

Therefore an owner-local controls screen should suppress the other side's
entire input/join path, and reject non-owner binding writes in this scoped menu
context as a defensive check. Setting `+0x2B4` once is insufficient. Device
assignment, logical battle side, and keyboard ownership must remain distinct.

## Native pause gate that leaves menus alive

`sGameUI__requestPause` (`0x1402BF730`) queues requested state at `+0x5AB8`,
sets request flag `+0x5AB9`, and writes reason at `+0x5ABD` (ordinary menu 0,
network wait 1). `sGameUI__getEffectivePause` (`0x140006BA0`) returns the pending
value when there is a request, otherwise committed state at `+0x5ABA`.

`sGameUI__consumePauseRequest` (`0x1402C1600`) commits the state and passes it
as the **third argument** to `sUnit__updateMoveLineVariantA` (`0x1402C0210`).
Disassembly verified the R8 argument, missing from its previous prototype.
The helper sets/clears bit `0x2` at move-line `+0x10`.

Affected lines are `0x00..0x16` and `0x1C..0x1F`; lines `0x17..0x1B` are left
untouched. This is consistent with the root pause on `0x19`, submenus on
`0x18`, and pause text sprites on `0x1A` continuing to run.

This is preferable to the plugin frame stepper's `isPaused` path, which skips
`oMove` and presents a captured frame: that would also freeze native menu logic.
It is not proof that every gameplay subsystem/timer/RNG is invariant during
arbitrarily many paused UI ticks. That still needs runtime measurement.

The project already uses request + immediate consume in
`UMVC3Hook/code/gameui.cpp::GameUI_SetWaitingForOpponent`, and has UI-only wait
ticks in `UMVC3Hook/code/framestepper.cpp`. Reuse that architectural pattern,
not the waiting-popup function as an independent pause owner: closing that
popup currently requests an unconditional unpause.

## Integration with current input and rollback

`UMVC3Hook/code/controller.cpp::hkbuttonRemap` captures local **logical actions
after native remapping**, before facing conversion. Remote/resim paths inject
stored logical timeline input. Thus the peer does not need the owner's physical
binding table, and old recorded inputs must not be remapped after a change.

Important integration constraints:

1. Current UI-facing pad hooks also route physical input by host/client side.
   The client keyboard is merged into P2 at the gameplay remap hook, not as a
   general native-menu solution. Test/fix menu keyboard and physical-slot
   routing explicitly for both sides.
2. Paused UI ticks must not call the ordinary gameplay frame-end path, create
   repeated snapshots/checksums, or fill the same scheduled input slot with menu
   actions. Keep network receive/control traffic running independently.
3. `Controller_OnFrameStart` does more than receive packets: it resets local
   pending input, processes rollback and frame transitions. Extract/use a
   deliberate paused service path rather than blindly calling it each UI tick.
4. `sGameUI` is neither snapshotted nor advanced during resimulation. Do not
   rewind a live menu or open it from resim. Settle rollback first, and keep the
   committed pause outside the rollback timeline.
5. Preserve already-scheduled pre-pause logical input frames. Define when the
   new mapping first produces newly sampled input; do not clear all histories
   or reuse the match-start frame-epoch reset on resume.
6. Consume/reset UI edge state on exit so Start/Confirm does not reopen the
   menu or become an accidental first-frame attack. Do not indiscriminately
   clear deterministic character input history or invalidate sent inputs.

## Proposed protocol and hook order

Use reliable control packets carrying battle generation, pause sequence, owner,
and frame boundary. Host arbitration gives simultaneous requests one winner.
Network callbacks enqueue events; game-thread code owns native objects.

1. **Request:** intercept native open polling before it changes effective pause.
   Capture the local owner's intent out-of-band, withhold native activation,
   and send a pause request. Do not forward Start as remote menu input.
2. **Arm a barrier:** negotiate a future boundary `F` (defined as next unexecuted
   gameplay frame). Both peers must install a hard stop before executing `F`.
   A fixed frame lead is only an optimization: a peer already beyond `F` must
   reject/re-negotiate it, never silently open late. An armed peer stays stopped
   while negotiation finishes; do not depend on a timely final commit packet.
3. **Settle:** at the barrier, continue ingesting input, confirm all completed
   frames through `F-1`, perform outstanding corrections, and exchange ready
   acknowledgements/checksums. If negotiation overshot available history, use
   a later barrier rather than attempting an unsupported restore.
4. **Commit pause:** only after both peers are at the same settled boundary,
   request/consume native pause before the next outer move. Open the owner's
   menu through its normal state transition with owner/cursor explicitly set.
   The other peer keeps a local passive overlay. Keep UI ticks alive but the
   gameplay clock and deterministic state frozen.
5. **Resume:** owner menu close becomes a resume request, not permission for
   unilateral native unpause. Finish submenu teardown while paused; acknowledge
   readiness and release gameplay at the same logical `F`. Transport arrival
   need not be wall-clock simultaneous; normal input availability/prediction
   rules still bound progress. No history/epoch reset.

A pause-reason arbiter is required so prediction wait, user menu, disconnect,
and desync cannot clear each other's stop. Normal `GameUI_SetWaitingForOpponent`
calls and native menu state-9 unpause must respect it. Disconnect must always
have an escape path even when gameplay/frame-dependent work is stopped.

Initially allow only **Resume, Command List, Controller Settings**. Disable
training/gameplay options and native scene exits. Character select/title/restart
actions require explicit existing or new synchronized session transitions;
`0x1402E13D0` and confirmation handlers can otherwise change scenes locally.

## Validation required before shipping

- Pause from host and client, simultaneous Start, retransmissions, high latency,
  and pause requests racing a rollback or restart/generation change.
- Hash gameplay state and monitor replay/fight clocks and RNG across hundreds
  of owner/non-owner UI ticks; command-list resource loading must not alter
  deterministic state. Audit any subsystem that still advances.
- Remap each owner's normal and alternate table; verify the other table and
  remote machine's configuration stay unchanged. Check gameplay after resume
  and a subsequent rollback against the stored canonical masks.
- Test controller, keyboard, multiple local devices, disconnect/reconnect,
  Start-to-join suppression, and no first-frame input leakage.
- Close command list/controls repeatedly; test pause-reason overlap, scene
  teardown, snapshot ownership and object lifetimes.

Ghidra updates from this investigation: named/typed the four pause open/poll
helpers above, applied `sGameUi*` to pause consumption, and corrected the
move-line helper's missing pause argument. No executable bytes or plugin source
were patched.
