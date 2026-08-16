# Match Outro Continue and Restart

**Status: Native reference with custom-netplay override. Reviewed 2026-08-15.**

## Native Flow

`sAction__move` dispatches action phase 8 to `sAction__stepResult8` at
`0x14000A990`. This phase owns the winner outro and the transition to the result
screen.

For ordinary winner actions `0x89` and `0x8B`, the native function allows the
outro to finish when the animation ends, after a 900-frame timeout, or when its
continue input is accepted. The input path:

1. Requires a local-only match and a nonnegative winning team.
2. Resolves only the winning team's pad through
   `sMvc3Manager__getPlayerCharIdx` at `0x14025AC40`.
3. Reads that pad with `sPad__getRawStickInput_noSOCD` at `0x1402B3B80`.
4. Reads the menu confirmation mask from `sPad+0xBD8` through the wrapper at
   `0x140001A70`.
5. Accepts continue when `rawInput & confirmationMask` is nonzero.

After acceptance, ordinary versus modes allocate a `uBtResult`, add it to
`sUnit` category `0x1C`, and request action phase 9 through
`sAction__setNextStep` at `0x14000AE00`. `sAction__move` applies that queued
phase on the next update.

## Custom Netplay Override

`controller.cpp` hooks `sAction__stepResult8`. During custom netplay it performs
the native confirmation-mask test on both teams' mapped pads. Either player's
confirmation calls `FrameStepper_RequestMatchRestart`, which sends the existing
reliable synchronized restart request and schedules native phase-12 cleanup.

The same override hooks `sAction__stepResult9_10_11` at `0x14000A3B0`. If the
outro reaches the actual result screen without an earlier confirmation, either
player's confirmation there requests the direct restart as well.

While that restart is pending, the hook does not call the original phase-8
handler. This prevents phase 9 and its result-screen objects from being created
during the synchronization lead time. Offline and native game flows retain the
original phase-8 behavior.

## Key Addresses

| Address | Verified role |
|---|---|
| `0x140006D20` | `sAction__move`, action-phase dispatcher |
| `0x14000A990` | `sAction__stepResult8`, winner outro/result transition |
| `0x14000A3B0` | `sAction__stepResult9_10_11`, active result-screen flow |
| `0x14000AE00` | Queue the next action phase in `mStepOrder` |
| `0x140001A70` | Return the pad menu-confirmation mask at `sPad+0xBD8` |
| `0x14025AC40` | Resolve a team's mapped pad index |
| `0x1402B3B80` | Read raw pad input without SOCD processing |
