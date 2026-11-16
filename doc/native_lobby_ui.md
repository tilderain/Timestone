# Native Lobby UI

**Status: research plus first implementation, 2026-11-15.**
Native facts are from static analysis in Ghidra 8192 (`umvc3.exe.unpacked.exe`)
and from decoding the retail UI archives offline. The plugin code builds
(`make -j8`) and its screens are verified by an offline renderer against the
real game textures. In-game runs on 2026-11-15 confirmed drawing: layout,
frames, anchors, colours, draw order and both fonts match the offline preview.
Two differences found from those screenshots are recorded under Text and
Drawing below. The match and return-to-lobby flow is still untested in game.
Sections say "verified" when a statement was read from decompilation or
resource bytes, and "unverified" when it is an inference that the first
in-game run has to confirm.

## Summary

- The game's own eight-player lobby (`uMenuNetRoomMain`) is not special code:
  it loads scheduler layouts (`.sdl`), looks units up by class and name,
  **copy-constructs** them for each player row, and then drives position, draw
  flag, UV, texture and text. Timestone's lobby uses exactly that path.
- Only three unit classes are involved: `uUiAnmSprite` (quad),
  `uUiAnmSpriteFrame` (9-slice) and `uUiAnmSpriteFont` (text). UI space is
  1280x720 and the engine scales it to the viewport itself.
- The lobby lives in the **title area** (`aTitle`). The plugin already hooks
  `aTitle::move` for its boot path, the title area is where the main-menu
  archive (which holds the lobby art) is resident, and it is where the game
  natively returns after a battle.
- Rooms are Steam lobbies with no room server: lobby data holds settings, each
  member publishes its seat/ready/team as member data, and every client
  derives the same room from it.

The in-game lobby is the default entry. `-launcher` on the command line or
`[Launch] NativeLobby=0` in `Timestone.ini` brings back the external launcher
and 1v1 lobbies; `-training` and `-autotest` launches are unaffected.

## How the native UI is built

### Unit classes (verified)

| Class | DTI | vtable | Copy constructor | `operator new` |
|---|---|---|---|---|
| `uUiAnmSprite` | `0x140D53CF8` | `0x140B15A70` | `0x1402C9DC0` | `0x1402C9E30` |
| `uUiAnmSpriteFrame` | `0x140D543A8` | `0x140B16350` | `0x1402CF630` | `0x1402CF8C0` |
| `uUiAnmSpriteFont` | `0x140D53D88` | `0x140B15CE0` | `0x1402CA2D0` | `0x1402CAA50` |

Each class `operator new` takes `(size, alignment)` and forwards them to the
class allocator with the DTI id (`newInstance` passes the object size and
`0x10`); the decompiler shows it as taking no arguments.

Inheritance: `cUnit` (0x40) -> `uCoord` (0x100) -> `uBaseSprite` (0x3C0) ->
`uActorGameSprite` -> `uUiAnmSprite` (0x540) -> Frame (0x580) / Font (0x760).
Struct layouts are in the Ghidra database (`uBaseSprite`, `uCoord`, `cUnit`,
`uUiAnmSpriteFrame`, `uUiAnmSpriteFont`, `uScheduler`).

| Offset | Field | Notes |
|---:|---|---|
| `+0x10` | `cUnit` flags | bits 0-2 state (1 queued, 2 running = alive), bits 3-9 move line, bit 11 (`0x800`) draw |
| `+0x40/+0x48` | `mpParent`, `mParentNo` | scheduler hierarchy; clones are detached |
| `+0x50` | `mPos` | UI pixels |
| `+0x70` | `mScale` | |
| `+0x108` | `mGeometryDirty` | must be set after changing size/UV/anchor/colour |
| `+0x310` | `mColor` | four bytes in R,G,B,A order (confirmed in game) |
| `+0x330` | `mSize` | frames: size of the stretched centre |
| `+0x338/+0x340` | `mUV0`, `mUV1` | |
| `+0x350` | texture `cResource*` | font units store their font texture here |
| `+0x360` | `mDepth` | draw order is `0x7FFD - mDepth`: smaller is nearer |
| `+0x364` | `mAttr` | bit 0 fit size to texture on assignment, bit 1 shadow, bit 4 `mColor1` gradient, bit 13 use 1920x1080 space |
| `+0x368` | `mCenterAttr` | anchor, see below |
| `+0x540` | `mFrameAttr` (frame) | bit 0 tiles the centre cell instead of stretching |
| `+0x558` | `mCenterUv[2]` (frame) | defaults 1/3 and 2/3 |
| `+0x594` | message index (font) | `-1` while showing a raw string |
| `+0x6F0` | `mCachedMsgId` (font) | differs from the index when text must be laid out again |
| `+0x700` | `fontType` (font) | 0 `nrodinpro`, 1 `acme`, 2 `bada`, 3 `news`, 4 `bada_b` |
| `+0x75B` | `mFixSize` (font) | |

Native setters that were mis-named in the database and are now renamed:
`uBaseSprite__setTexture` `0x1402C6E00`, `uBaseSprite__setSize` `0x14020CBA0`,
`uBaseSprite__setColor`, `uBaseSprite__setOffset`,
`uBaseSprite__fitSizeToTexture`, `uBaseSprite__setMaskTexture` `0x1402C6D40`.
`uBaseSprite__createProperty` `0x1402C41D0` is the authoritative name-to-setter
map and matches the property names found in `.sdl` files.

### Geometry (verified)

- `uBaseSprite__draw` `0x1402C5070` scales vertices by viewport/1280 and
  viewport/720 (or 1920/1080 when `mAttr & 0x2000`). Layout code never needs
  the real resolution.
- `uBaseSprite__move` `0x1402C65B0` places the quad from the anchor:
  `row = mCenterAttr / 3`, `col = mCenterAttr % 3`,
  `right = w * (2 - col) / 2`, `bottom = h * (2 - row) / 2`. So 0 is top-left,
  3 middle-left, 4 centre, 5 middle-right, 8 bottom-right. Value 9 uses
  `mCenterPos`.
- `uUiAnmSpriteFrame__move` `0x1402D0400` builds a 4x4 vertex grid: the UV
  window is cut at `mCenterUv`, the outer cells keep texture size, the centre
  is `mSize`. Total size is `mSize + 2 * border`, and the anchor applies to the
  total. Many frame textures only use part of the texture (`uv` up to 0.75)
  and carry a transparent margin; `lobby_canvas.cpp` records both per texture.
- Colour and UV are rewritten to the sprite's vertices every `move`. Frames
  rebuild only when the world matrix changed or `mGeometryDirty` is set.

### Text (verified)

- `uUiAnmSpriteFont__resetIndexAndSetText` `0x1402CEF30` sets the message index
  to `-1` and calls `setText` `0x1402CEE50`, which takes UTF-8, ignores an
  unchanged string, and otherwise marks the text for layout. This is the call
  the native lobby uses for player names.
- `uUiAnmSpriteFont__isTextGeometryDirty` `0x1402CE2F0` compares message id,
  language, outline flag, text flags and `mAttr`. Position, scale and colour
  are applied at draw time, so they do not need a relayout. Alignment is baked
  in at layout, so the backend marks `mCachedMsgId` stale when a pooled unit's
  anchor changes.
- Scale is applied twice. `uUiAnmSpriteFont__rebuildTextQuadGeometry`
  `0x1402CB440` lays glyphs out at `cell * mScale` and then transforms them by
  the world matrix, which carries `mScale` again, so on-screen size goes with
  `mScale` squared (confirmed in game). The backend passes `sqrt(scale)`.
- The horizontal anchor works as for sprites. The vertical one does not for a
  single line: `uUiAnmSpriteFont__measureResolvedText` `0x1402CF3B0` only adds
  height per line break, so the measured height is 0 and the text's top sits at
  the anchor (confirmed in game). The backend offsets `y` by half or a whole
  cell itself.
- `uUiAnmSpriteFont__fontType__setter` `0x1402CE4E0` swaps font texture and
  metrics. Glyph cell is 32 px for `nrodinpro` and 64 px for `bada_b`.
- Font metrics (`.fsd`, type `0x1DB58B09`) are a table of advance widths; glyph
  index for U+0020..U+007E is `codepoint - 0x20`. Message tables (`MSD`) store
  strings as those glyph indices. `msg_sys_menu_en` already contains lobby and
  spectator strings (ids 20 "SEARCH LOBBY", 21 "CREATE LOBBY", 1559
  "SPECTATING", 2033 "The battle will begin when both players are ready.").

### Schedulers and resources (verified)

- `ui__loadResourceAndAttachUiScheduler_1403E9550(ctx, path, out, moveLine)`
  loads an `rScheduler` (DTI `0x140E17830`) through `sResource` vtable `+0x60`,
  constructs a `uUiScheduler`, calls `uScheduler__setData` and adds it to
  `sUnit`. The first argument is unused.
- `uScheduler__getUnitIndex(sched, DTI*, name)` `0x14050EE60` and
  `uScheduler__getUnit(sched, index)` `0x14050EE40` resolve a spawned unit.
  Units appear over the frames after creation; native code polls for them.
- Textures are `rTexture` (DTI `0x140E17570`) loaded by path through the same
  `sResource` call with flag 1. A resource is found only while its archive is
  resident.
- `cRef__composeLocalizedName(mode, base, out, tag)` `0x1404552A0` adds the
  language suffix. Mode 3 gives `ui\netroom` -> `ui\netroom_en` for archives
  and `..._typeD_en` for textures (base/Japanese archives have no suffix).

| Archive | Loaded by | Holds |
|---|---|---|
| `ui\cmn` | boot | fonts, button glyph sheets, common windows |
| `ui\netroom[_lang]` | `sGameUi__loadNetRoomArchive` `0x1402BDE30`, status `0x1402BCEF0` | native lobby layouts and art, comic backdrop |
| `ui\mnmain[_lang]` | `sGameUi__loadMainMenuArchives` `0x1402BDD60`, polled by `aTitle::load` | "SEARCH LOBBY" / "CREATE LOBBY" art, ping bars, row plates, main-menu bars |

### Native lobby anatomy (verified)

`aNetworkRoom__init` `0x1402129D0` creates `uMenuNetRoomMain`; its `setup`
`0x140435F90` attaches `net_room_free2` (77 named units), `net_room_card`,
two `net_room_player1`, `net_room_wins`, `net_room_word` and the `a_lic_bg`
backdrop. `bindRoomUnitsAndClonePlayerPanels` `0x140431FF0` resolves the units
by name and clones the 19-unit player row seven times with the copy
constructors, shifting each row by the row pitch. `refreshRoomDisplay`
`0x140433480` then sets draw flags, names (`setText`), icons (`setTexture`)
and tag sprites (`uBaseSprite__setSpriteUvRectangle`). `kill` `0x1404319C0`
kills every clone through vtable `+0x78` after `MtObject__isAliveState`.

## Menu input and sound (verified)

- Native menus read three masks per pad slot: trigger
  `sPad__getRawStickInput_noSOCD` `0x1402B3B80`, repeat `0x1402B3950`, held
  `sPad__getPlayerInput_withSOCDClean` `0x1402B3700`. Decide and cancel masks
  come from `sPad__getDecideButtonMask` `0x140001A70` and
  `sPad__getCancelButtonMask` `0x140001A50`. Directions are `0x10010` up,
  `0x40040` down, `0x80080` left, `0x20020` right.
- For the slot mapped to player 0 the trigger getter returns a fixed keyboard
  mapping instead (`sPad__getKeyboardMenuTrigger` `0x1402B3390`): arrows,
  Enter/A decide, Backspace/B cancel, X, Y, Esc as Start, Tab as Back. The
  keyboard glyph sheet `mai_hlp04` has exactly these keys.
- Menu cues are system voices played through `sGSound__playOrdinaryVoice`: 8
  cursor, 9 decide, 10 cancel. Wrappers: `uMenuSubBase__playDecideSe`
  `0x14032A7F0`, `uMenuSubBase__playCancelSe` `0x1403F78C0`,
  `uMenuSubBase__playCursorSe` `0x1403F7920` (plays only when `+0x88 != +0x8C`).

## Title area and flow (verified natively, plugin use unverified)

- `aTitleLogo__update` `0x140217700`: phase 0 boot archives, phase 1 logos,
  phase 2 opening movie that queues `aTitle`.
- `aTitle__init` `0x140216940` calls `sUnit__killAll`, creates `uViewDraw` and
  `uViewDrawUi`, the menu camera, and through `aTitle__setPresentationMode`
  either `uMenuTitle` (`aTitle+0x90`, default boot state) or `uMenuMain`
  (`aTitle+0x98`, boot states 1-6 and 13-15, which includes the state the
  plugin sets when booting a battle).
- `aTitle__load` `0x140216F50` waits for the main-menu archives.
- `aChrSelect__onSelectionComplete` `0x1402131C0` shows how the native
  character select hands off: `populateCharSelectSlots`, `commitOnePlayConfig`,
  loading unit, `sUnit__killAll`, queue `aChrSelectBlank`; cancel queues
  `aTitle`.

## Offline tools

`tools/umvc3_ui/umvc3_ui.py` reads ARC, TEX, SDL and FSD (formats are
documented in its header) and renders draw commands in 1280x720 with the real
textures and font. `tools/lobby_preview/preview.sh <game-dir>` builds the real
screen code for the host with a scripted room, and writes one PNG per screen
to `build/lobby_preview`. Extracted assets go to `build/ui_cache` and must not
be committed.

## Plugin implementation

| File | Role |
|---|---|
| `lobby_canvas.*` | Draw vocabulary (sprite, frame, text), texture table, frame styles, font metrics |
| `lobby_model.h` | Room model and the `RoomService` interface |
| `lobby_ui.*` | Screens: top menu, lobby search, create, room, team setup, modals. No game or Steam dependency |
| `lobby_native.*` | Native backend: resources, prototype scheduler, unit pools, input, sound |
| `lobby_room.*` | `RoomService` on Steam lobbies |
| `lobby_flow.*` | Title-area hold, match start handshake, return to room |
| `steam_lobby.cpp` | Room mode added beside the existing 1v1 mode |

### Drawing

Each frame the screens issue draw commands back to front. The backend maps
them onto pooled clones of three prototypes from the native lobby's own
layout (`icon_RoomFriend`, `Frame_RoomTitle`, `Font_Log_a2` in
`ui\net\net_sprite\net_room_free2`), assigns texture, size, UV, position,
anchor, colour and a decreasing `mDepth`, and hides clones not used this
frame. The prototype scheduler is paused and its own units stay hidden. Before
each frame the backend checks that the scheduler, the prototypes and the first
and last clone of each pool still have the expected vtable and an alive state,
and rebuilds from scratch if not.

The backdrop is the title area's own animated key art, dimmed with a
full-screen fill. The native lobby's comic plate
(`ui\mainmenu\ma_sprite\meku_menu_co00_BM_NOMIP`) loads but did not draw
through a cloned sprite in game. It is the only DXT1 texture that was tried;
every `_HQ_` texture (format `0x2A`) draws. The cause is not established.

### Rooms

- A room is a Steam lobby tagged `TimestoneRoom=1` (not `Timestone=1`, so
  launcher clients never auto-connect into one). Lobby data: `name`,
  `protocol`, `slots`, `location`, `live`.
- Member data `ts`: `version|slot|side|ready|status|entry|chars|assists|colors|wins|losses`;
  `loc` holds the member's ping location.
- A seat claimed by two members goes to the lower SteamID; everyone applies
  the same rule, so there is no arbiter and host migration needs no handling.
- Chat is lobby chat with a one-byte type prefix.

### Match start and return

1. Both seats of a slot ready: each side opens the Steam P2P connection to
   the other (`SteamRoom_ConnectPeer`, symmetric connect). Side 0 takes the
   ENet server role.
2. Player 1 waits for a ping reading, then sends `DelayPacket` and
   `LaunchTrainingPacket` (both rosters as published, its stage), exactly what
   the launcher's host sends. Player 2 applies them with the existing
   `ApplyTrainingLaunchPacket`.
3. Both call `FrameStepper_InitNetplay` and `BootIntoTraining` from the title
   hook. The lobby stays on screen under a busy overlay until the boot commits,
   then its units are killed before the area changes.
4. Leaving (pause menu "Return to Lobby", or the opponent leaving) runs
   `FrameStepper_EndNetplaySession`, which resets the same per-match state as a
   synchronized restart and shuts netplay down while keeping the room, then
   queues `aTitle`. The title hook rebuilds the lobby and shows the room.

### Leaving for the main menu

"MAIN MENU" on the lobby's top screen (or cancel there, after a prompt) hands
the title area back to the game: the lobby's units are killed, any room is
left, `aTitle+0xA0` (`mPresentationMode`) is reset to `-1` and
`aTitle__setPresentationMode(area, 1)` `0x140216600` spawns `uMenuMain`, the
same call the press-start screen makes. From then on the title hook runs the
native `aTitle::move`. The plugin's Esc menu shows "Online Lobby" while the
title area is running; it retires `uMenuMain` again and rebuilds the lobby.
Going to the main menu this way is confirmed in game.

The press-start screen is skipped, so `ReleaseToMainMenu` applies what
`uMenuTitle__move` `0x1403F7DB0` sets on Start: player 0's pad slot
(`sMvc3Manager+0x140`), `sGameUi+0x5C40` and `+0x5EA8`, plus the `sPad`
keyboard-block bytes. Without this the keyboard did nothing on the main menu.

"Online" (item 1) is greyed out and skipped natively while
`isPlatformContextReadyForMatchmaking` `0x14045CEF0` is false, which is always
the case here because native networking is switched off. Three main-menu
sites ask through the thunk at `0x14045CFF0`: `uMenuMain`'s move (return
`0x1403E7514`, colour and animation), `uMenuMain__getInitialSelection`
(return `0x1403E7274`) and the shared navigation state
`uMenu__modeFlow_s7_inputNavigation` `0x1403E9D90` (cursor at `+0x78`, writes 1
to its out parameter on decide). While released, the thunk is answered "ready"
for those three only (the navigation one only for `uMenuMain` units, told
apart by DTI `0x140D6AE60`). A pad or keyboard decide on item 1 is taken
before the menu sees it, because its own handler starts the native network
entry and leaves the menu half torn down: the plugin plays the decide cue,
fades to black with an ImGui rectangle (200 ms), kills the menu and fades the
lobby in (250 ms) once it can draw. `uMenuMain::kill` does not always take
its layout scheduler (unit reference at `+0xD0`, getter `0x1402B6D80`) with
it, so that is killed explicitly. Native modes started from that menu run with the lobby's
launch option still set, which has not been exercised either.

## First in-game run: what to check

Log lines are prefixed `LobbyNative:`, `LobbyFlow:` and `LobbyRoom:`.
If the native UI has not come up after three seconds an ImGui notice says
"Starting lobby..."; after fifteen it reports failure and the plugin's Esc
menu (with Exit Game) becomes available again.

1. A plain launch reaches the title area: `LobbyFlow: requested the title area from
   the logo sequence`, then `title area ready for the lobby`. If the game sits
   on a black screen, `aTitle::load` is not completing.
2. `LobbyNative: ready sprite=... frame=... font=...`. If prototypes stay
   missing, the layout name or the archive language suffix is wrong.
3. Colours: confirmed correct.
4. Text: right-aligned and centred strings confirmed. Still to check after the
   size and vertical-offset fix: small labels match the preview, and names
   with non-Latin characters render.
5. Draw order: confirmed.
6. Input: pad and keyboard both navigate; footer glyphs follow the last
   device used.
7. Two clients: create, join, sit, ready, match starts, "Return to Lobby"
   brings both back seated with the room intact. Step 4 of the sequence above
   is the least certain part of this work.

## Not done yet

- **Spectating.** The room model and UI already carry a `Watching` status and
  the native WATCH tag, but nothing streams a match. The simulation has no
  mid-match state transfer, so a spectator has to join before a match starts,
  boot the same rosters and seed, and consume a relayed stream of *confirmed*
  inputs for both teams with no rollback and no prediction (stall when the
  stream runs dry). Two ways to feed those inputs:
  - Timestone's own timeline: a spectator is a client for which both teams are
    "remote". This needs a relay from player 1, a both-teams-remote mode in
    `controller.cpp`, and a spectator path through the battle-ready handshake
    in `framestepper.cpp`. It matches the plugin's canonical input timeline
    (logical inputs after button remap) and is the recommended route.
  - The game's own mechanism: native spectating is built on `sReplay`
    playback. `aChrSelect__init` resets `sReplay` (vtable `+0x28`) when
    `sBattleSetting__isOnlineSpectatorBattle` is true, and
    `sReplay__writeBothTeamInputs` `0x1401CF5D0` / `sReplay__readInput`
    `0x1401CECB0` are the stream write and read ends (see `replay.md`). Its
    records hold native replay inputs, not Timestone's timeline values, so it
    would need its own determinism validation.
  Battle type 4 and the spectator branches listed in `ingameonlinechecks.md`
  are the native presentation hooks (banners, non-pausing overlay) either way.
- **Native character select.** "Character select" entry currently opens the
  native-styled team panel. Driving the real screen means queueing `aChrSelect`
  with only the local side active and capturing the result at
  `aChrSelect__onSelectionComplete` instead of letting it queue the battle.
- Simple mode, stage choice and win/loss records are not exposed in rooms.
- The pause-menu display settings are not reachable from the lobby.
- Returning to the room stalls for about a second while ENet waits for a
  disconnect acknowledgement from a bridge that is already gone.
