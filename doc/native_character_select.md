# Character Select: native screen, native netcode, and a design for rooms

**Status: research, design and a first implementation, 2026-11-24.** Static
analysis only, Ghidra 8192 (`umvc3.exe.unpacked.exe`). The implementation
(`native_charsel.cpp`, see "Implementation") builds and has **never run in
game**. "Verified" means read from decompilation or bytes; what only a run
can show is listed near the end.

## Summary

- The native select screen is one unit, `uMenuChrSel`, spawned by the area
  `aChrSelect`. Each team has a small state machine whose state lives in
  `sGameUI`, and every mode ends in the same call,
  `sBattleSetting__requestEnter`.
- Native online play does not synchronise inputs on this screen. Each client
  runs its own side and sends its state to the other as `cNetChrSel`
  messages, which the receiver applies **without any validation**. The
  opponent's side is hidden on screen, but their cursor and picks arrive in
  clear, and the host alone decides the final roster, the stage and the seed.
- Timestone's rooms have the same two weaknesses today: teams are published
  in Steam member data before the match, and player 1 builds the launch
  packet for both teams.
- Proposal: both players on the real screen in versus mode. Each client
  runs its own side natively with no delay and draws the other side from a
  small validated state message. No hashes.
- Presets are a pop-up list over the screen, applied through the game's own
  reserve-unit code path without touching the save.

## The native screen (verified)

### Objects

| Thing | Address | Notes |
|---|---|---|
| `aChrSelect` DTI | `0x140D50128` | queued through `sArea` vtable `+0x50` like any area |
| `aChrSelect__init` | `0x140208B70` | `sUnit__killAll`, resets all six slots, guide mode |
| `aChrSelect__load` | `0x140208EE0` | `ui\mnchscmn`, `sound\chs`, `ui\mnchsstg`, nine reserve faces |
| `aChrSelect__spawnMenuChrSel` | `0x1402084D0` | creates `uMenuChrSel` on move line 1 |
| `aChrSelect__final` | `0x140208630` | `sUnit__killAll`, releases every archive it took |
| `uMenuChrSel__ctor` / `setup` / `move` / `kill` | `0x1403608F0` / `0x1403690F0` / `0x1403625E0` / `0x140362210` | size `0x670` |
| `uMenuChrSel__tickSlotStateMachine` | `0x140368300` | one caller, `0x1403675FC` |
| `sBattleSetting__requestEnter` | `0x14024B530` | queues `aChrSelectBlank`; called from `0x1403675CC`, `0x140366945`, `0x140215A14`, `0x1403EA6E4`, `0x14024E2ED` |

Entering needs no more than the mode and the DTI. The pause menu's
"Character Select" (`uBattleUiPause__changeMenuState` `0x1402E13D0`, case 9)
sets boot state 0 and queues `aChrSelect`. The arcade entry sets
`sGameUI__setCssState(1)` (`0x140214AC0`), battle type 1 and
`sMvc3Guide__setMenuMode(0)`; training sets CSS state 0 and battle type 5.
CSS states 6, 7 and 8 are the three online kinds.

### Phases (`uMenuChrSel::mPhase`, `+0x2C8`)

| Phase | Function | What it does |
|---:|---|---|
| 0 | `uMenuChrSel__phase0_beginSelection` `0x140366EF0` | music, voice `0xA0`, online info cards |
| 1 | `uMenuChrSel__postSelectAdvance_3675F0` | ticks the slot machine until it returns 0, then stores control types |
| 2 | `uMenuChrSel__waitForCharacterArchives` `0x140366EA0` | waits for all six archive sets |
| 3 | `uMenuChrSel__postSelectCommitAndEnter_367C40` | 95 frames, VS cards |
| 4 | `uMenuChrSel__postSelectTickStageLoadAndBgm` `0x1403678D0` | stage archives, versus jingle (cue 6) |
| 5 | `uMenuChrSel__phase5_storeReserveIdsAndOpeningMember` `0x140367B30` | remembers the reserve unit used; online, the opening-member buttons |
| 6 | `uMenuChrSel__phase6_requestEnter` `0x1403674F0` | `sBattleSetting__requestEnter` |

### Slot states (`mMode[2]`, `+0x2B4`)

The tick reads each team's state from `sGameUI__getSlotState` `0x1402BBA60`
and writes it back with `sGameUI__setSlotState` `0x1402BFCE0`, which is why
a network message can move the other team's slot.

| State | Meaning |
|---:|---|
| 0, 1 | enter; control-mode chooser and reserve widget are created |
| 2, 4, 6 | picking member 1, 2, 3 (`0x140364C50`) |
| 3, 5, 7 | picking that member's assist (`0x140362EF0`) |
| 8 | animation, then `0xB` |
| 9, 10 | back-out popup, confirm |
| `0xB` | team complete: archives requested, then stage select or done (`0x140366390`) |
| `0xC`..`0xE` | Heroes & Heralds cards |
| `0xF`, `0x10` | stage select, stage confirm and handicap |
| `0x11`, `0x12` | done, idle |

The tick returns 0 once both teams are above `0x10`.

A member's pick is written straight into the match descriptor: slot
`team * 3 + member` gets the character id, the color (through
`sBattleSetting__resolveCharSlotForPlayerSide`, which moves a color that
clashes with the other side), the assist and the voice language. The picked
team can therefore be read from `sBattleSetting` as soon as a team reaches
`0xB`, whatever drew the screen.

### Who gets input

There is no single gate. Each widget decides for itself:

- `uMenuChrSel__captureInputForSlot` `0x140360F70` (decide and cancel);
- the grid cursor `uMenuChrSelCursor`, which reads its own pad and is
  switched off by a pad id of -1 (`uMenuChrSel__setup`);
- `uMenuChrSelReserveUnit__captureSlotInput` `0x14037A080`,
  `uMenuChrSelAssist__captureSlotInput` `0x14036A7F0`, the control-mode
  chooser, the stage page and the handicap unit.

Arcade (battle type 1) already gives a one-sided screen: team 1 has no
cursor input and walks itself to `0xB`, its card and its half of the
background are hidden (`uMenuChrSelCardPlayer::mHide`,
`uMenuChrSelBgMain::mHideTbl[1]` at `+0xB9`), and there is no stage select or
handicap.

### Timer

`uMenuChrSelBgMain::mTimeLimit` (`+0xCC`) starts at 3599 frames and counts
down unless `mTimeStop` or `mTimeInfinite` (`+0xB1`; set for every offline
mode). `uMenuChrSelBgMain__isTimeUp` `0x14036DD50` makes each state decide by
itself, and an undecided member becomes a **random** character
(`uMenuChrSel__pickRandomCharacterForTeam` `0x140360DA0`, global XorShift).

### Reserve units: the built-in preset picker

`uMenuChrSelReserveUnit` (size `0x128`) is created in state 1 and is live in
state 2, before the first pick. L1 / R1 (pad buttons 8 and 9, or the mouse)
step through three units with `MtMath__wrapRange(cur, +-1, 0, 2)`; confirm
puts the index in `mResult` (`+0x10C`) and `uMenuChrSel__move` calls
`uMenuChrSel__commitCharSelectionForSlot` `0x140368BF0`, which fills all
three members and jumps the team to `0xB`. A member whose id is not above 0
is skipped, and the player then picks the remaining ones by hand.

The teams come from `sLicense__copyLockedBuffer_0x690_at64` `0x140203550`,
a copy of the license profile (characters `+0xB8`, assists `+0xDC`, colors
`+0x100`, use counts `+0x124`, each `int[3][3]`). The select screen calls it
from exactly four places:

| Call site | In | Use |
|---|---|---|
| `0x140209211` | `aChrSelect__load` | preloads the nine faces |
| `0x140371942` | `uMenuChrSelCardPlayer__setup` | loads the nine faces it draws, picks the starting unit from the use counts |
| `0x14036F56F` | `uMenuChrSelCardPlayer__refreshPlayerCardDisplay` | draws the highlighted unit |
| `0x140368C35` | `uMenuChrSel__commitCharSelectionForSlot` | applies it |

All four are five-byte `E8` calls, so the return addresses a hook would test
are `0x140209216`, `0x140371947`, `0x14036F574` and `0x140368C3A`.

The widget's own units are `Team_icon_A_shad`, `hlp_icon_L1`, `Font_icon_A`
(the letter) and `Frame_icon_A_sel`.

### Roster tables and their limits

| Table | Address | Size | Read by |
|---|---|---|---|
| grid, `int[7][8]` | `0x140B3E580` | 56 cells | `uMenuChrSel__getGridCellCharacter` `0x140361FD0`, `uMenuChrSel__findGridIndexForCharacter` `0x140361F50` only |
| selectable flag, `byte[]` | `0x140A6FD30` | 60 | `getCharacterUnlockFlag` `0x140059520`; ids 1..50 are set |
| variant count, `int[]` | `0x140A6F9F0` | 60 | `sBattleSetting__getCharVariantCount` |
| roster order, `int[]` | `0x140B14820` | 52 | `sGameUi__getCharacterRosterIndex` `0x1402B9BD0`, clamps to `0x33` |
| resource name, `char*[]` | `0x140C553B0` | | `Mvc3CharacterId__getResourceName` `0x140058F90` |

What a mod that adds characters runs into on this screen:

- The grid is two models of 28 faces each (`chs_meku_face_a` / `_b`), so 56
  cells. Row 0 is `{0x14, 0, 0, 0x35, 0x36, 0, 0, 0x25}`: four cells are free.
- Grid codes `0x34` and `0x37` (the "?" face), `0x35` (RANDOM) and `0x36`
  (RANDOM ALL) share the number space with character ids, so ids 52..55
  cannot sit in the grid as they are.
- `uMenuChrSel::mChrTbl` holds 60 ids. The random pick uses a 64-bit mask and
  only draws ids below `0x34`. The card arrays (`mpTexChr`, `mpTexChrName`)
  are filled for ids below `0x34` and indexed up to `0x37`.
- Colors 6 and 7 are gated by cSecret bits `id + 0x33` and `id + 0x352`.
- The native messages carry a character id in one signed byte.

## The native netcode (verified)

`cNetChrSel` (`0x90` bytes, ctor `0x140261F20`) is rebuilt for each visit
(`sAgentSession__rebuildLocalSlot_75750`). The screen marks what changed with
`sAgentSession__setSyncDirtyBit` `0x140274390`, and
`cNetChrSel__publishDirtyMessages` `0x1402637B0` turns each bit into a
message:

| Bit | Message | Content |
|---:|---|---|
| 0..2 | `MsgChrType` | team `+0x10`, member `+0x14`, character `+0x18`, color `+0x1C`, voice `+0x20`, simple mode `+0x24`, auto jump `+0x25`, UI flag `+0x26`, seed `+0x28` |
| 3..5 | `MsgChrAssist` | team, member, assist |
| 6, 9 | `MsgFinish` | 1 = team complete, 2 = all done |
| 7 | `MsgStg` + `MsgBattleInfo` | stage id |
| 8 | `MsgBattleData` | six `{character, color, assist, voice}` byte tuples at `+0x10`, simple mode and auto jump per team, seed `+0x40` |
| 10..12 | `MsgChrTypeSel` | where the cursor is |
| 13..15 | `MsgChrAssistSel` | which assist is highlighted |
| `0x10`, `0x11` | `MsgCurrentMember` | opening character |
| `0x12`, `0x13` | `MsgManner`, `MsgSpCard` | |

`cNetChrSel__dispatchInboundMessage` `0x140264420` picks a handler by DTI.
What the handlers do with a message:

- `applyCharacterSelectionMessage` `0x140262E00` takes the **team** and
  member from the message and writes that slot. The member is used as an
  index into a three-entry stack array with no check, and nothing stops a
  peer naming the receiver's own team.
- `applyBattleDataMessage` `0x140262B50` overwrites all six slots, both
  teams' simple mode and auto jump, and the replay seed, from the host's
  message.
- `applyStageSelectionMessage` `0x140263270` sets any stage id it is given.
  Only the host gets the stage page (`uMenuChrSel__processStageSelect`).
- `applyCurrentMemberMessage` `0x140262900` writes the opening member with
  an unchecked team index.
- No handler checks a character id, a color against what is installed, or an
  assist.

So the native design is: state replication, host authority, hidden only on
screen, and trust in every byte. The seed comes from the host's global RNG
(`uMenuChrSel__setup` draws it into `sReplay`).

## Where Timestone stands (read from the code)

- `lobby_room.cpp` `EncodeMember` publishes each member's team in Steam
  member data while the prompts run. The screens do not show it, but any
  client can read it before choosing.
- `SendLaunchAsHost` (`lobby_flow.cpp`): player 1 sends both teams, draws both
  players' random members and picks the stage.
  `ApplyTrainingLaunchPacket` checks only that the values are in range, not
  that player 2's team is the one player 2 chose.
- Character ids are assumed to be 1..50 in about fifteen places
  (`IsPlausibleTeam`, `RosterCharacterCount`, `chr_colors.cpp`,
  `lobby_canvas.cpp` face table, `spectate.cpp`, `LaunchMenu.cpp`,
  `chrsel_imgui.cpp`).
- CHARACTER SELECT in the start prompt opens the lobby's own team panel
  (`LobbyUi::UpdateHandshake`, `Handshake::Entry`).

## Design

Decided with the user on 2026-11-24: both players are on one screen and see
each other's cursor live; the local side has no input delay; no hashes or
commitments; blind pick becomes a room setting later; presets are a pop-up
list.

### 1. One screen, each side simulated where its player sits

The screen runs in versus mode (battle type 0), which is the two-sided
layout. Each client runs **its own team's** slot natively, from its own pad,
with nothing in between: that is the zero delay. The other team's slot is
not simulated at all; it is drawn from the state its owner sends.

Input lockstep was rejected: it would add delay, and the screen's flow
depends on things that differ per machine (load timing, unlock bits, control
type, presets). This is the native game's own model, with two differences:
the native screen hides the opponent's half and switches their cursor unit
off (pad id -1, so `uMenuChrSelCursor__move` returns at once), and it trusts
what it receives.

The local player sits on their real side, player 1 left and player 2 right.

### 2. What is sent

One small message on the match connection that is already up, sent when
anything in it changes and a few times a second otherwise. It is a snapshot,
not an event, so a lost or repeated one does no harm:

```text
sequence, slot state, grid cell under the cursor,
3 x { character, color, assist }, preset list open, locked in
```

The receiver applies it to the **sender's team only** and only after
checking every field: state one of 0..8 or `0xB`, cell below 56, character
in the roster table, color installed, assist 0..2, no character twice.
Anything else is dropped. Character ids are 32 bit.

When both are locked in, player 1 sends the launch as it does now, with two
changes: each client draws its own random members before locking in, and
each client compares the launch packet's copy of its own team with what it
picked and refuses the match if they differ. The stage is still picked by
player 1 from the two preferences.

**Blind pick (later, a room setting):** the same screen with the opponent's
half hidden (`mHideTbl`, the card's `mHide`) and the message carrying only
the state and "locked in" until both are. No protocol change beyond that.

### 3. Drawing the other team

Checked in Ghidra on 2026-11-24: everything the opponent's half shows is
derived from stored state each frame, with one exception (the assist list).

| What is on screen | Drawn from | Who writes it natively |
|---|---|---|
| Grid highlight | `uMenuChrSelBgMain::mCursorPos[team]` (`+0xBC`, `+0xC0`), shown while the team's slot state is 2..8 (`refreshCharacterSelectDisplay` `0x14036CE80`, `animateCursorHighlight` `0x14036C930`) | the cursor's `move`, through `uMenuChrSelBgMain__setCursorPos` `0x14036E820` |
| Faces dimmed as taken | `sBattleSetting` slots and the slot state | the pick |
| Card: which panel, animations | slot state changes (`uMenuChrSelCardPlayer__updatePlayerCardAnimationState` `0x1403711E0` compares with the last one it saw) | the tick |
| Card: member being hovered | `sGameUI` character and color per member, copied every tick from the records at `uMenuChrSel +0x178` (`{grid index, color, assist}`, 12 bytes, three per team) | `slotState246` |
| Card: picked members, body art | `sBattleSetting` slot; `uMenuChrSelMakeTex` requests the character's archive itself and loads `b_<Name><NN>` when it is in | the pick |
| Assist list | the widget's own state | `uMenuChrSelAssist__open` `0x14036BC50`, then its own input |

`uMenuChrSelCursor` has no graphics: it is a `uScheduler` that is never
given a layout, its `move` is not run by `sUnit` (`cUnit__setMove(…, 0)` in
`setup`) but called from `slotState246`, and all it leaves behind is
`mCursorPos`. So there is no cursor unit to animate for the opponent.

What the plugin does for the remote team:

- MinHook the five handlers and return 1 at once when the team is the remote
  one: `uMenuChrSel__state0_enter` `0x140363D80`, `slotState1_loadTeam`
  `0x140363350`, `slotState246_processSelection` `0x140364C50`,
  `slotState357_processCharSelectConfirm` `0x140362EF0`,
  `slotState11_teamLoadedDispatch` `0x140366390`. The tick itself only reads
  the state from `sGameUI`, calls the handler and writes the state back, so
  nothing else moves that slot.
- Each frame, from the last message, in this order:
  1. for each member already picked, the descriptor slot, with the setters
     `slotState246` uses (character, color, team, assist, voice language).
     This has to be in place before the state moves on, because the card
     reads it on the state change;
  2. the three records at `uMenuChrSel +0x178` and the cursor cell in
     `mCursorPos` (and in the cursor's `mSelection`, which the tick reads for
     the RANDOM ALL flag);
  3. the slot state, with `sGameUI__setSlotState` `0x1402BFCE0`.
- Assist list: MinHook `uMenuChrSelAssist__captureSlotInput` `0x14036A7F0`
  to return 0 for the remote team (every read the widget makes goes through
  it). On the remote entering state 3, 5 or 7 call `uMenuChrSelAssist__open`
  (widget, team * 3 + member, assist); while there, write the highlighted
  assist into the widget's byte at `+0xA`; on leaving, call
  `uMenuChrSelAssist__close` `0x14036AB20`. The step animation between
  assists is not replayed.

Archives for the remote characters are requested by the card's
`uMenuChrSelMakeTex` units, and again for all six slots by the local team's
`0xB` handler.

Two clients can each pick the same character in the same color in the same
instant: each keeps its own color and shows the other's as sent, so both
screens agree. The clash is resolved once, at launch, as it is now.

### 4. Session

- **Enter**: if either player confirms CHARACTER SELECT, both go to the
  screen and both pick from nothing (decided 2026-11-24: a player who chose
  START is not shown with their team already in). If both choose START
  there is no screen. Entering: release the
  lobby's units as `LobbyFlow_OnBattleBootCommitted` does, set battle type 0
  and queue `aChrSelect`. That is all the game's own Versus entry does from
  the same area: `OfflineMenuButtonCheckThing` `0x1403E65F0` calls
  `sBattleSetting__setBattleType(0)` and
  `uMenu__modeFlow_s9_loadStageOrRouteArea` `0x1403EA580` queues the menu
  entry's area. `aChrSelect__init` sets the guide mode itself. CSS state is
  also set to 0, since phase 3 reads it without checking the mode.
- **Pad**: `sMvc3Manager::mPadId[1]` is set to `mPadId[0]` each frame of the
  session. Every read on this screen takes a player index through
  `sMvc3Manager__getPlayerCharIdx` `0x14025AC40` (the cursor's
  `uUiCursor__getNavigationInput` `0x140323600`, its confirm and cancel,
  `captureInputForSlot`, the assist widget), and `sPad`'s getters answer
  with the keyboard when the slot equals `mPadId[0]`, so player 2's side
  reads the same pad or keyboard. The mouse is the exception: its hit tests
  are written for player 0 only, so it works on the left side and not on
  the right. Because both indices now read one device, the remote team's
  handlers and assist widget must be blocked as in section 3 before this
  is switched on.
- **End** (changed 2026-11-24 at the user's request): the screen goes on
  past `0xB` as it does offline, through its stage select (one player
  picks, chosen at random) and its versus sequence, and the battle is booted
  at the point where the game calls `sBattleSetting__requestEnter`, which is
  refused. See "Implementation".
- **Clock**: one deadline for both; clear `mTimeInfinite`, set `mTimeLimit`.
  Just before 0 the client locks in what it has, filling unpicked members
  from the team in use, so the game's random fallback never fires.
- **Leaving**: backing out past the first member, or the opponent leaving,
  queues `aTitle`; the existing `aTitle__init` hook sets boot state 0 first
  so the lobby takes the title area back.
- **Control type**: forced "fixed and configured" (`sGameConfig +0x190`,
  `+0x191`) so the Normal / Simple chooser is skipped, until rooms offer
  simple mode.

### 5. Presets: a pop-up list

The native reserve widget is not used: it shows three units and steps
through them one at a time. Instead:

- The widget is killed when `slotState1_loadTeam` creates it (the game's own
  pattern: vtable `+0x78`, then clear `mpReserveUnit[team]`).
- Start opens the lobby's existing preset list. That is the button the
  game's own reserve unit is taken with: `slotState246` tests trigger bit
  `0x8` in state 2 (L1 / R1, mask `0x300`, change the color). The list
  (`LobbyUi::DrawPresetList`, all thirteen slots with names and faces) over
  the screen, drawn by the lobby's native backend. While it is open the
  local slot gets no input. What that needs here, checked:
  - **A view.** `sGameUI` normally keeps a UI view for scene `0x80`
    (`+0x56D0`), but `sGameUI__beginCharacterSelect` `0x1402C0440` switches
    it off for the life of the screen (`+0x56D8`). The screen's own
    `uViewDrawUi(0x80, 0xC, 0, 0x11, 200000000)` replaces it, and
    `uViewDrawUi__draw` `0x1404537B0` takes every unit whose draw view has
    that bit. The lobby's three prototypes in `net_room_free2` are draw view
    `0x80`, so their clones are picked up.
  - **Archives.** The list uses `ga_net_waku_f` and `ga_net_sel_a`
    (`ui\netroom`), `ga_cmn_bar` and the button glyphs (`ui\cmn`), the
    faces (loose files) and the fonts. Nothing from `ui\mnmain`, which
    `aTitle__final` releases. The `ui\netroom` handle (`sGameUI +0x5A50`)
    is only ever released by the native lobby area's own exit.
  - **Order.** A sprite's priority is `0x7FFD - mDepth` within its pass
    (`uBaseSprite__draw` `0x1402C5070`). The grid and the cards are model
    units that are not flagged 2D in their layouts (`chs_meku`,
    `chs_card1p`), so they are not sorted with the sprites at all. That
    sprites show over them is an inference, not something read from the
    renderer: the screen's assist list and stage page are its farthest
    sprites and are laid out on top of the cards and the grid. Its own sprites sit at depths 16995..17003 (name plates,
    VS art), 29986..29991 (timer) and 32740..32765 (assist list, reserve
    icon, stage page). The backend starts at 31800 and counts down, which
    would put the list under the name plates and the timer: in this area
    its depths must start below 16990.
- Confirm applies the team through the game's own path: the hook on
  `sLicense__copyLockedBuffer_0x690_at64` puts the chosen team in unit 0 of
  the caller's copy (return address `0x140368C3A` only) and
  `uMenuChrSel__commitCharSelectionForSlot(menu, team, 0)` is called. That
  fills all three members, requests their archives and puts the team at
  `0xB`. Random members are drawn before the call. The save is not touched.
- The list can be opened whenever the cursor is on the roster (states 2, 4
  and 6); the team picked replaces whatever was picked before.
- The opponent sees "choosing a preset" from the flag in the message.

### 6. The start prompt

`Handshake::Entry` keeps its two rows. CHARACTER SELECT raises a request the
flow takes (as `TakeMainMenuRequested` works) instead of opening the team
panel. The team panel stays as the top menu's team editor, and is what
CHARACTER SELECT falls back to if the area has not come up after a few
seconds.

### 7. Room for mods

- **One roster table in the plugin**, built at start from the game's own
  tables above instead of the constant 50, and used by every range check,
  the random draw, the face and icon lookups and the color rule. A mod that
  patches the game's tables is then picked up with no plugin change.
- A character one side does not have fails that side's check in step 2 and
  ends the pairing with a notice saying which character.
- The message has a version and a length, so a mod's extra per-character
  option can be appended later.
- Nothing here reads the grid table or `mChrTbl` except the cursor cell, so
  a mod can rearrange or extend the grid as long as both players run it.

### Left to do

1. Roster table (everything validates against it).
2. Blind pick as a room setting.

## Implementation (built, not run)

| File | What it holds |
|---|---|
| `native_charsel.h/.cpp` | The session: thirteen hooks, the remote team, the local capture, the clocks, putting a whole team in, the stage select and the versus sequence. No lobby or connection code. |
| `netplay.h` | `PACKET_CHARSEL` (22), `CharSelPacket`, `CharSelFlags` |
| `lobby_flow.cpp` | Opening and leaving the screen, the packets, the preset list over it, the battle boot from its area (`BeginCharSelect`, `CharSelFrame`, `LeaveCharSelect`) |
| `lobby_ui.*` | CHARACTER SELECT raises `TakeCharSelectRequested`; the preset list can run on its own (`OpenPresetPicker` and friends) |
| `lobby_native.*` | `Native_SetDepthBase` |

How a match goes through it:

1. CHARACTER SELECT in the start prompt sets a wish; the prompt holds under
   "Opening character select..." until the match connection is up
   (`Phase::Handshake`).
2. `BeginCharSelect` releases the lobby's units, calls `charsel::Begin`
   (battle type 0, CSS state 0, control type forced fixed, `aChrSelect`
   queued) and sends an empty `CharSelPacket`. The other player's
   `PumpHandshake` sees it and opens the screen too. Both pick from nothing:
   a player who had chosen START is set not ready again, and nobody's team
   is put on the screen in advance.
3. While the screen's area runs, `LobbyFlow_GameTick` calls `CharSelFrame`
   instead of the title hook: room upkeep, `UpdateMatchStart` (pairing,
   packets, launch), the outbound packet, and the preset list.
4. A team reaching `0xB` is locked in for good and the player is set
   `ready`.
5. **Stage select.** With both teams locked in and the other's final team on
   the screen (`StagePhase`), one team picks the stage. Which one is a coin
   toss: each player sends one bit chosen when they opened the screen, and
   the XOR is the team (`StageTeam`). For that team alone the game's own
   "team complete" handler runs, on both clients; it opens the stage page
   (`uMenuChrSelOption`) and the slot goes `0xF` and then `0x11`. The other
   team's slot is held at `0xB` and set to `0x11` with it
   (`DriveStagePhase`), so its own `0xF` / `0x10` states, which create the
   handicap unit, never run.
   - The page always reads player index 0, the local pad on either client.
     On the picker's client it is the game's own, without its two rows of
     match options (up / down are cut, and a page found on those rows is put
     back on the stage row).
   - On the other client its input is cut (`Hook_OptionSample`,
     `Hook_OptionConfirm`), its cursor is written from the packets, and when
     the picker has decided it is closed with the picker's stage
     (`sGameUI +0x5934`), a random pick included.
   - The picker has 20 seconds; then the game's own "clock ran out" call
     decides for them, the stage under the cursor.
6. **Versus sequence.** With the stage decided the tick hook stops holding
   the screen, and it plays what it plays offline: wait for the archives,
   the VS cards, the stage load and jingle. Where it would enter the battle
   (`sBattleSetting__requestEnter`) the hook refuses and records it
   (`EnterRequested`).
7. **Launch.** Player 1 sends the launch once both teams and the stage are
   known and its own versus sequence is through; the teams and the stage
   are the screen's. Player 2 refuses a launch whose copy of its own team or
   of the stage differs from what it has. `BeginLaunch` calls
   `charsel::ReleaseSettings`; each client then calls `BootIntoTraining`
   once its own versus sequence is through (or 15 seconds after the launch
   at the latest), and when that commits, `charsel::End(true)` and
   `LobbyFlow_OnBattleBootCommitted`. For two seconds after `End` nothing on
   the screen runs.
8. Backing out past the first member (the game's own "return to the menu?"
   prompt), the opponent doing so, the pairing ending or 155 seconds
   without a match all end in `LeaveCharSelect`: title area with boot state
   0, and the room is shown again.

Choices made while writing it, where the design left room:

- **States sent.** 0..7 and `0xB`. State 8 (the reserve unit's animation)
  cannot occur with the widget gone, and 9 / 10 (back-out prompts) are sent
  as 2. Past `0xB` only the stage cursor, "decided" and the stage id change.
- **Clock.** 60 seconds from the moment the screen is up, each client on its
  own. The game's clock is shown but held a second short of 0, so its
  random fallback never runs; at the deadline the team in use is put in.
- **Team in use.** `UiPrefs::team` with any random members drawn when the
  screen opens. It is only the fallback.
- **Putting a team in** always goes through the reserve-unit commit, queued
  and applied inside the tick hook, and the slot state is then written to
  `sGameUI` as well: the tick takes the state from there, and a state set
  outside a handler would otherwise be overwritten on the next tick.
- **Match options.** Opening the stage page writes the player's saved versus
  options into the match, on each client from its own save: the time limit
  (`sBattleSetting +0x35C`) and `sAction`'s damage rate (`+0x128`). Both are
  read at `Begin` and put back before the boot, the damage rate again at
  `End`, so the players cannot end up with different rules.
- **The stage page has to be started.** It is created idle (state 2) and
  the game starts it (`0x140376070`) from the *waiting* team's state `0x10`
  handler, once the background has changed over. That handler never runs
  here, so `DriveStagePhase` makes the call under the same two conditions.
  Found in the second run: the page existed and never appeared.
- **Voice of a remote pick.** The record's voice field must be 0 or 1 (1 for
  ids above 25). The getter returns one byte; reading it as four wrote
  garbage, the card asked for an archive that does not exist, and "team
  complete" waited on it for ever. Found in the first run.
- **Sounds of a preset.** Putting a team in plays what taking a reserve unit
  plays: bank 1 voice 9 / `0xF` (player 1 / 2), then bank 6 voices `0x91` /
  `0x9B` and the announcer `0x54` / `0x86`.
- **Team complete waits for both.** The handler only goes on once both
  teams' frame counters (`uMenuChrSel +0x2CC`) pass 5, and each counts in
  its own handler; the held team's is set by hand.
- **Without the hooks** (`charsel::Install` failed) or if `Begin` fails,
  CHARACTER SELECT opens the team screen as before.

Not done: blind pick; the roster table (only `IsSelectableCharacter`, which
reads the game's selectable table and the installed colors); showing that
the opponent has their preset list open (the flag is sent); spectators see
nothing of the screen.

## Still to be seen in game

The four open points of the first draft were settled by reading the code
(sections 3 to 5). What static analysis cannot show:

- Everything in "Implementation": none of it has run. The first things to
  watch in the log are `NativeCharSel: the screen is up`, the two
  `locked in` lines and `LobbyFlow: launching match`.
- How the remote half looks when its state jumps (two messages apart) instead
  of stepping: the card plays its animation for the state it lands on.
- Holding the screen at both teams `0xB` for the second or two the launch
  exchange takes.
- `aChrSelect` to `aChrSelectBlank` through `BootIntoTraining`. The game
  makes this change itself (phase 6), but from its own bookkeeping.
- The layering of the list against the screen's model units and its pass 21
  sprites: argued from how the screen's own sprites behave, not read from
  the sort.
- `aChrSelect__onSelectionComplete` `0x1402131C0` reads like a mode-menu
  result handler, not the select screen's end; its name was left alone.

## Ghidra changes made in this session

Renamed (all were `uMenuChrSel` or `cNetChrSel` code filed under another
class): `0x140361FD0` `uMenuChrSel__getGridCellCharacter`, `0x140361F50`
`uMenuChrSel__findGridIndexForCharacter`, `0x140361FA0`
`uMenuChrSel__gridIndexToCharacter`, `0x140360DA0`
`uMenuChrSel__pickRandomCharacterForTeam`, `0x140360F70`
`uMenuChrSel__captureInputForSlot`, `0x140364C50`
`uMenuChrSel__slotState246_processSelection`, `0x140366390`
`uMenuChrSel__slotState11_teamLoadedDispatch`, `0x140366EF0`
`uMenuChrSel__phase0_beginSelection` (was `aChrSelect__enterStageSelection`,
the name other notes use for the voice `0xA0` cue), `0x1403674F0`
`uMenuChrSel__phase6_requestEnter`, `0x140367B30`
`uMenuChrSel__phase5_storeReserveIdsAndOpeningMember`, `0x1403686D0`
`uMenuChrSel__advanceStageLoad`, `0x1402637B0`
`cNetChrSel__publishDirtyMessages`, `0x1402084D0`
`aChrSelect__spawnMenuChrSel`, `0x14036DD50` `uMenuChrSelBgMain__isTimeUp`.
Plate comments were added to six of them and to
`uMenuChrSel__commitCharSelectionForSlot`, `tickSlotStateMachine` and
`uMenuChrSelReserveUnit__processSelectionInput` (whose old comment described
a different function).

Second pass, same day: `0x1402C0270` `sGameUI__setGlobalUiViewSuppressed`,
`0x1402C0440` `sGameUI__beginCharacterSelect`, `0x1402BB570`
`sGameUI__endCharacterSelect`, `0x14036E820` `uMenuChrSelBgMain__setCursorPos`,
`0x14036BC50` `uMenuChrSelAssist__open`, `0x14036AB20`
`uMenuChrSelAssist__close`, `0x140370F80`
`uMenuChrSelCardPlayer__spawnMemberBodyTexture`, `0x140374320`
`uMenuChrSelMakeTex__setPhase`, and `0x1400F3150` `cUnit__setDrawView` (was
`MtAnim__setFrameIndex`: it writes the ten draw-view bits at `cUnit +0x12`).

## Next: the result screen (asked for 2026-11-24, not started)

Requested, in the user's words: remove the instant restart on the match
outro; hook pad input to the game's own REMATCH / CHARACTER SELECT / MAIN
MENU choice with no input delay; make the character select reachable again
from there; send the character select to spectators; keep the set count
(first to N) running through the character select.

Only the reading below was done. No code was changed for it.

Where things are now:

- `controller.cpp` `hkStepResult8` and `hkStepResult9_10_11` are the instant
  restart: either player's confirm calls `FrameStepper_RequestMatchRestart`
  once `LobbyFlow_AllowResultRestart` allows it, and while that is pending
  the native phase handlers are not called, so the result screen's own
  objects are never created. Taking this out alone leaves no way to play
  the next game of a set, so it has to go in together with the menu.
- Natively, phase 8 allocates `uBtResult` (unit category `0x1C`) and asks
  for phase 9. `sAction__stepResult9_10_11` `0x14000A3B0`, outside online
  modes, ends in `sAction__setRestartMode(this, 1)` and
  `sAction__setNextStep(this, 0xC)` once
  `sGameUI__getOnlineStateGate_23000` (the Ghidra name; what the gate is has
  not been read) returns 0. The choice
  itself is not in that function.
- The choice lives in the result units: `uBtResult__move` `0x1402FC790`,
  `uBtGameOver__processResultAction` (around `0x1402FB170`) and
  `uBtGameOver__selectNextState` (around `0x1402FBA1A`). The unnamed helper
  `0x1402FA9D0` is the one that names `aChrSelect` (reference at
  `0x1402FA9FF`), so it is the CHARACTER SELECT outcome. None of these has
  been decompiled yet: which pad they read, where the cursor is kept and how
  the outcome reaches `sAction` are all still to be read.

A shape for it, to be checked against that reading:

1. **Menu, zero delay.** The same split as the select screen: each client
   runs the result menu from its own pad, and sends cursor and decision in a
   small packet (the match connection is netplay's by then, so this goes on
   the extension channel like `chrsel_imgui.cpp`'s `CLS1`). The simulation is
   over at that point, so nothing here needs rollback.
2. **Agreeing** (decided by the user, 2026-11-24). The existing instant
   restart stays as the mechanism, but fires only once **both** players have
   pressed the game's own REMATCH. CHARACTER SELECT happens if **either**
   asks for it. MAIN MENU by either ends the set and returns that player to
   the room. The F11 restart is to stay, later as a lobby setting.
3. **Back to the select screen.** The session in `native_charsel.cpp` is
   already independent of how it was entered. What is missing is the entry
   from a running match: end the netplay session's game (not the pairing or
   the connection), then `BeginCharSelect`. `LobbyFlow`'s `CharSelFrame`
   assumes `Phase::Handshake` with the lobby's own ENet host; a match in
   progress has handed that host to netplay, so the packets need a second
   transport path or the host has to be handed back.
4. **Set count.** `g_setWins` / `ResetSet` in `lobby_flow.cpp` are reset in
   `BeginLaunch` through `g_service->BeginSet`. A launch that follows a
   mid-set character select must not reset them: `BeginLaunch` needs to know
   it is continuing a set. The count can be drawn on the select screen with
   the same canvas the preset list uses.
5. **Spectators.** A spectator boots from the logged boot configuration and
   inputs (`spectate.cpp`). The select screen has neither. The cheapest
   honest version is to forward both players' `CharSelPacket`s down the
   feed and have the spectator run a session with both teams remote; that
   needs `native_charsel.cpp` to support "no local team", which it does not
   yet.

### The result unit, read 2026-11-24 (second pass)

`uBtResult` (size `0x108`) is a phase machine: `uBtResult__move`
`0x1402FC790` switches on `mPhase` (`+0xF4`; `mWinTeam` `+0xF0`,
`mSubPhase` `+0xF8`). The menu itself is phase 7,
`uBtGameOver__processResultAction`, which sets the phase for what was
chosen. Each outcome is its own small function, run from the switch, and
each ends by killing the unit (vtable `+0x78`):

| Phase | Function | What it does |
|---:|---|---|
| 8 | `0x1402FAA20` | **REMATCH**: `sAction__setRewardCommitRequest(sAction, true)`, kill. `sAction__stepResult9_10_11` then sets restart mode 1 and step `0xC` |
| 9 | `0x1402FAD00` | **MAIN MENU**: boot state 1, fade the music (2000 ms), queue `aTitle`, kill |
| 10 | `0x1402FA9D0` | **CHARACTER SELECT**: boot state 0, fade the music, queue `aChrSelect`, kill |
| `0xC` | `0x1402FAC90` | title with boot state 0, after a child unit has gone |
| `0xE` | `0x1402FAB40` | arcade ending (`aEnding`) |
| `0xF` | `uBtResult__returnToNetworkRoom` `0x1402FADC0` | native online |
| `0x12`, `0x13` | `0x1402FAA50`, `0x1402FAAB0` | Heroes & Heralds (`aEarthAtk`) |

Phases 1..6 are the presentation (`0x1402FB0D0`, `0x1402FA9C0`,
`0x1402FB5E0`, `0x1402FB660`, `uBtGameOver__selectNextState`,
`0x1402FA960`); `0xB`, `0xD`, `0x10`, `0x11`, `0x14` were not read.

What this gives the design:

- The three outcomes are three function entries. Hooking them is enough to
  take the choice without touching the menu:
  - **8**: do not run the original. Mark "this player wants a rematch" and
    tell the peer. The unit stays in phase 8, so the hook is called every
    frame, which is where "waiting for the other player" can be shown. When
    both want it, call the existing `FrameStepper_RequestMatchRestart`.
  - **10**: do not run the original (its `aChrSelect` request is not a
    session). Tell the peer; either player's choice takes both to
    `BeginCharSelect`.
  - **9**: `LobbyFlow_RequestLeaveMatch` in place of the title with boot
    state 1.
- For the menu to exist, `hkStepResult8` / `hkStepResult9_10_11` must stop
  restarting on confirm and let the native phases run (phase 8 creates
  `uBtResult`). The set-decided case (`LobbyFlow_AllowResultRestart` false)
  needs REMATCH refused or hidden.
- F11 restart is a separate path and is not touched by any of this.

Still unread, and needed before writing it:

- `uBtGameOver__processResultAction` (phase 7): which player index's pad
  moves the menu (natively probably one player's), so that each client can
  drive its own copy from its local pad, and whether netplay's input hooks
  let that read through after the match has ended.
- Whether the menu offers the three entries in versus (battle type 0) as it
  does offline, or needs a mode flag.
- The gate `sGameUI__getOnlineStateGate_23000`, which holds
  `stepResult9_10_11` back while the result unit is up.

## The result screen and the select reached from it (built 2026-11-24, not run)

### What the game does (third pass)

- In versus (battle type 0, which room matches boot with) the board under
  `uBtResult` is `uBtResultScoreVersus` (`0x968` bytes). Its running update is
  vtable slot 29, `0x14030A770`. It loops over both players:
  `mChoice[2]` at `+0x940` (0 none, 1 REMATCH, 2 CHARACTER SELECT, 4 MAIN
  MENU), `mSelectPos[2]` at `+0x948` (0..2). Input is taken once the board's
  scheduler (`+0x58`) is past frame `0x78`. MAIN MENU opens a "really?"
  pop-up (`mRno[2]` = player + 1) before it counts.
- When both have chosen, `mResultType` (`+0x8E4`) is the larger of the two
  and `mRno[1]` goes to 2. `uBtGameOver__selectNextState` (phase 5) maps it:
  1 -> phase 8, 2 -> phase 10, 3 -> `0x10`, anything else -> `0xD` (title,
  boot state 1). So natively: a rematch only if both chose it, the select if
  either did, the main menu over both.
- Three functions read a player's pad for this screen, each through
  `sMvc3Manager__getPlayerCharIdx(player)`: `0x1402FE180` (raw, confirm and
  skip), `0x1402FE130` (directions), `0x1402FE0E0` (repeat).
- `uBtResult` is in unit category `0x1C`, which a rollback neither restores
  nor replays (`IsHeadlessGameplayUnitCategory`). `sAction` is replayed, and
  `sAction__stepResult8` is what creates the unit.
- `sAction__stepResult8` ends the winner's outro early on a confirm read
  from the winning team's pad slot, outside the synchronized inputs.

### What was built

- `native_result.cpp` (`resultmenu`): hooks the board's update, the three pad
  reads, `uBtResult__move` and the outcome phases 8, 9, 10, `0xD`, `0x10`.
  - Local player: the game's own code, from the local pad on any slot.
  - Other player: `mChoice` / `mSelectPos` written from `ResultMenuPacket`
    (extension channel, checked, tagged with the battle generation).
  - MAIN MENU: the confirm is kept from the game while the cursor is on it,
    and a second press takes the place of the pop-up (the pop-up reads a pad
    slot a netplay client has nothing in). The overlay says so.
  - The outcome phases do nothing in a session; a second `uBtResult` (from a
    replayed creation frame) is killed in its first move.
- `controller.cpp`: in a room match the result hooks no longer restart on a
  confirm, and the raw pad read is cut while `stepResult8` runs. The win pose
  therefore cannot be skipped. Outside a room (launcher netplay) nothing
  changed. F11 is untouched.
- `lobby_flow.cpp`, `UpdateResultMenu`: the host asks for the restart when
  both chose REMATCH, and for `RESTART_CHARSEL` when either chose CHARACTER
  SELECT; whoever chose MAIN MENU calls `LobbyFlow_RequestLeaveMatch`.
  Nothing follows a game that decided the set.
- `RESTART_CHARSEL` (restart packet kind 2) runs the ordinary synchronized
  restart, except that `LobbyFlow_BeginMatchCharSelect` asks for `aChrSelect`
  in place of the reload and `TrainingBoot_BeginSelectRestart` holds the
  restart pending. The netplay session, the connection and the set stay up.
  `CharSelPacket` travels as `CharSelExtPacket` on the extension channel.
  When a player's screen is through (teams, stage, versus sequence) it boots
  with `TrainingBoot_FinishSelectRestart`; the start-up barrier joins the two.
- Spectators: the feed has `MSG_SELECT` (one team's side, latest wins, sent
  once a game's frames are all out). `SpectateStep::Select` takes a spectator
  through the same restart with `charsel::Config::watch`: both teams drawn
  from packets, no pad, nothing sent. It boots on the `MSG_FINISH` the
  players record when they boot. `native_charsel.cpp`'s remote state is now
  per team for this.
- The set score is drawn under the status bar while the select is up.

### To be seen in game

- The menu appears and both cursors move; the three outcomes.
- The area change battle -> `aChrSelect` under a live netplay session, and
  the boot out of it.
- That killing a duplicate `uBtResult` is clean, if one ever appears.
- A spectator through the whole of it.
- Not covered: the select opened from the room's start prompt is not sent to
  spectators (no game is being watched yet), and a spectator does not see the
  players' result-menu choices.
