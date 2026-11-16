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
the title area back to the game, and "Online" on that main menu (or "Online
Lobby" in the plugin's Esc menu) takes it back. Both directions reload the
title area instead of touching menu units: `BeginTitleReload` sets the
`sGameUi` boot state (`+0x5798`; 1 = main menu with music and camera, 0 =
press-start screen) and queues `aTitle` again. `sArea__applyPendingAreaTransition`
`0x140545640` rebuilds the leaf even when the target equals the current area,
and `aTitle__init` runs `sUnit__killAll`. The outgoing area's
`mPresentationMode` (`+0xA0`) is overwritten with a marker so the first tick of
the rebuilt area can be recognised even at the same address. The lobby then
takes over a fresh press-start screen exactly as it does at boot; returning
from a match also sets boot state 0 for the same reason.

An earlier version spawned `uMenuMain` with `aTitle__setPresentationMode`
`0x140216600` and killed it by hand. In game that left the menu layout, the
status bar and the license hotkey alive and could produce two menus. Do not go
back to it.

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
entry: the plugin plays the decide cue, fades to black with an ImGui rectangle
(200 ms), reloads the title area and fades the lobby in (250 ms) once it can
draw. Leaving the lobby uses the same fade.

`HideNativeMenuChrome` still clears the guide mode
(`sMvc3Guide__setUiGuideMode` `0x1402837C0`, `sMvc3Guide+0x4B0`) and disarms
the license hotkey (`sLicense__handleReturnToTitleFlag` `0x140279680`) every
lobby frame as a second line of defence.

### Music

The lobby plays the game's own tracks through `sGSound__playSystemCue`
`0x1402AC5C0` `(sGSound, cRef* out, cue, 0, 0, 1)`, with `sGSound` from
`0x140003AA0`. The cue table is `sound\bgm\bgm` in `sound\cmn.arc` (`STQR`:
40-byte file records from `+0x50`, 152-byte requests from `+0x1180`; request
dword 0 is the cue id, dword 2 the stream type, dword 23 the file index).

- Cue 16 (`bgm_gm` file 13, type 1) is the native lobby room
  (`uMenuNetRoomMain__playRoomTransitionCue` `0x14042F870`). It plays as soon
  as it is requested. Confirmed in game.
- Cues 13 and 14 are the main menu theme: two synchronised layers
  (`bgm_gm_035`, `bgm_gm_036`), stream type 10. `aTitle__init` and
  `uMenuTitle__move` request the pair, but that only queues them. They stayed
  silent in the lobby until the step `uMenuMain` performs in its vfn38
  `0x1403E89A0` was added: mute the layer the BGM-style setting
  (`sGameConfig` `0x140251640`) does not use with
  `sGSound__playVoiceLoopOrIndex` `0x1402AF010` `(sGSound, bank ref 0, cue, 0,
  0xFFFFFFFF, 0.0f)`, then call `0x1402AEAE0` `(sGSound, cRef* out, cue)` for
  13 and 14. `ReleaseMenuThemeLayers` does this. Confirmed in game.
- `0x1402B0D20` `(sGSound, ms)` fades out what is playing; the game calls it
  before nearly every music change.

#### Room music chosen by the host

A room's music is the host's choice: LOBBY MUSIC on the create screen sets
what a new lobby starts with, and the same item in the room menu (Start)
changes it afterwards. The last choice made in either place is remembered
(`[Lobby] RoomMusic`) and preselected the next time a lobby is created.
The track id is written to the Steam lobby's own data under `bgm`, so every
member and every later joiner reads the same value and it survives the host
leaving; each client plays it locally when the value changes. Only the
choice is shared, not the playback position. The default (id 0) is cue 16.
The table is `lobby_music.h`; an id is `cue | variant << 8`, and anything not
in the table is treated as the default.

`sGSound__playSystemCue` is `(sGSound, cRef* out, cue, variant, release,
restart)`:

- `restart` 0 leaves a cue alone if it is already playing.
- `release` 1 starts a cue that would otherwise only be queued. Which cues
  those are is `sGSound__cueNeedsRelease` `0x1402AB4A0`: 8, 13, 14, 20..89 and
  120..131, which are exactly the stream-type-10 requests. The lobby passes
  1 for every room track.
- Cues 71..80 (`sGSound__isLayeredStageCue` `0x1402AB490`) are stage themes
  with a second mix in cue + 20 (`bgm_st_NNN_b`). Both are started together
  and `variant` picks the audible one (0 main, 1 the `_b` file, 2 a third
  channel pair that the lobby does not offer). Cues 90..104 cannot be
  requested directly (`sGSound__isStageAltMixCue` `0x1402AB480`).

Where the names in the table come from (static analysis of the executable and
the cue table; none of the tracks beyond 13/14/16 has been heard in the lobby
yet):

- Characters: every character class's theme virtual (`+0x2E0`) is
  `mov eax, cue; ret`. Cues 20..39 are the original Capcom cast, 40..55 and
  57..59 the Marvel cast, 60 Galactus, 120..125 and 126..131 the Ultimate
  additions; 121 is Wright's Turnabout theme. Cue 56 has no file.
- Stages: `uStageNNN`'s theme virtual (`+0x238`) returns 70 + stage number
  (training room 70, Danger Room `uStage100` 71); the 1xx variants share the
  base stage's cue. No stage class returns 78, listed as "Stage 8".
- Menus, from the call sites of `sGSound__playSystemCue`: 2/3
  `sAction__executeMatchPhase` (listed as Victory, a guess from context), 4/5
  and 106..108 character select, 9/10 `uBtContinue__setup`, 15 gallery, 17
  `uMenuLicense`, 19 `aFinalResut__init`, 84..88 `uEnding`, 134..137 the
  gallery's content table, 138 `aCredit`. The use of 18, 112, 113, 132 and 133
  was not identified and they are listed by number. Where a pair exists the
  second entry ("B") is the one the BGM-style option switches to.
- Left out: the short stingers (6, 8, 11, 12).

The lobby's menus use the menu theme and a room uses the host's track (cue 16
unless changed); picking "Main Menu" in a room plays the same two-layer theme
as the menus. The theme runs
straight through a switch between the lobby and the real main menu (confirmed
in game): while the title area reloads with it playing, `g_keepMenuTheme`
drops the two things that would break it, the fade the dying `uMenuMain`
issues (a hook on `0x1402B0D20`) and the restart `aTitle__init` makes for the
main-menu boot state (`LobbyFlow_ShouldSkipSystemCue`, checked in the
plugin's existing `sGSound__playSystemCue` hook). The lobby starts its own
music from a hook at the end of `aTitle__init`. The
launcher's own music (`launch_music.cpp`) is no longer used by the lobby.

## Player list actions

Left from the leftmost seat column moves the cursor into the player list
(right or cancel returns to the seats). Confirm on a player opens their
actions: VIEW STEAM PROFILE (`ISteamFriends::ActivateGameOverlayToUser`,
"steamid") and, for the host on anyone else, KICK FROM LOBBY.

Steam lobbies have no kick, so a kick is a ban list in the lobby's own data
(`ban`: comma-separated hex SteamIDs, at most 64, oldest dropped first). It
exists exactly as long as the lobby does. Three checks enforce it, all in
`lobby_room.cpp`:

- a listed client leaves by itself on its next room refresh (after its match,
  if it is fighting) and is told the host removed it;
- every other client leaves listed members out of its room state, so a client
  that refuses to leave still has no seat and is not shown;
- `Join` refuses a lobby whose data lists the local player. A join through a
  Steam invite skips that check and is caught by the first one.

Only the lobby owner can write lobby data, so only the host can ban. Nothing
removes the player from the Steam lobby itself: a modified client can stay in
it and read its chat, and still counts towards the member limit.

## Pre-match prompts

There is no ready button on the room screen. Once both seats of a match slot
are taken, each of the two players gets a modal that walks through three
steps. The state is three fields each player publishes in their member data
(version 3: `accepted`, `delayPick`, then the existing `ready`), so both
clients derive the same step (`LobbyUi::HandshakeStep`):

1. **Ready up** (15 s). READY or LEAVE SEAT. No answer, LEAVE SEAT or cancel
   gives the seat up. When this prompt appears the game window is brought to
   the front (`Native_FocusGameWindow`) and the stage-select cue plays: voice
   `0xA0` of sound bank 6, as `aChrSelect__enterStageSelection` `0x140366EF0`
   plays it through `sGSound__playOrdinaryVoice` `0x1402AAC40`. If that bank
   is not loaded in the title area the decide cue plays instead; the log line
   `LobbyNative: match-found cue` says which. **Unverified in game.**
2. **Input delay** (20 s, then the highlighted value). AUTO or 0..8, with the
   other player's pick shown once they have chosen. Both players run one
   delay, so player 1 uses the higher of the two picks (`SendLaunchAsHost`).
   AUTO is shown with its number, e.g. `AUTO (2)`, worked out from the ping
   **measured between the two players**: the match connection is opened as
   soon as both seats are taken (`FindPairing` pairs on being seated, before
   either has readied up), stays up behind the prompts, and its round trip
   (`SteamTransport_GetRTT`) replaces the room's estimate for that opponent
   (`SetMeasuredPing`) in the prompt's ping display and in AUTO. Player 1
   holds the launch packet until both are `ready`; the exchange's 15 s
   timeout only runs from then. Until the first measurement arrives (a second
   or two into the ready-up prompt) the estimate is shown. A connection that
   fails stands the local player up, so the pair is not retried every frame.
3. **Start immediately / character select** (30 s, then start). Character
   select opens the team screen (90 s) whose last row is START. Either way
   the player ends up `ready`; when both are, the match is started as before.

Cancel at any step gives the seat up. The service forgets a player's answers
whenever the opposite seat empties or changes hands, and after every match,
so the next pairing (including a rematch) starts from step 1. There is no
queue yet: "the next player" is whoever takes the vacated seat. The per-player
quick start / character select setting is gone.

## Sets (first to N)

A room is created with a set length (`MATCH FORMAT`: first to 1, 2, 3, 5, 7 or
10), stored in the lobby data as `ft` and fixed for the room's life.

A room match is one set. The game's result phase is already hooked for
rematches (`sAction__stepResult8` / `9_10_11`, see `outro.md`); those hooks
now also hand `sAction::mWinTeam` (`+0x114`) to `LobbyFlow_OnResultPhase`.
`UpdateSet` counts a game once the result phase has held for 30 game ticks
(longer than any rollback window) and re-arms only after it has been gone for
120, so skipped frames cannot count a game twice. Both peers run the same
simulation, so both count the same games; nothing is exchanged. Team 0 is
taken to be player 1 (the pairing's side 0): **unverified**, the log line
`LobbyFlow: game finished winTeam=...` shows it.

Until the set is decided a confirm in the result phase restarts as before
(only after the game has been counted). Once someone reaches N the restart is
refused, the win pose plays for 150 ticks and each player returns to the room
on their own with the set result; the other player dropping at that point is
treated as the normal ending. Only a set played to its end goes on the room's
win/loss record. The running score is published in the member data (version
2, last field) for the room's slot header and drawn above the health bars
with ImGui during the match.

## Testing alone: `kLobbyTestBuild`

Steam lobbies need two accounts. Setting `kLobbyTestBuild` to `true`
in `lobby_local.h` and rebuilding (never ship that build) replaces the Steam lobby with
one room in named shared memory on this PC (`lobby_local.cpp`) and starts a
second game process (marked with the internal argument `-lobbytest-child`) once the first has reached the
lobby. `steam_lobby.cpp` forwards every call the in-game lobby makes to that
backend while the mode is on, so `lobby_room.cpp` and everything above it run
unchanged: create, search, join, seats, chat, music, kick and ban, sets.
Matches connect with plain ENet on `127.0.0.1` (port 27100 + the player-1
process id modulo 2000). The two players are "Tester One" and "Tester Two",
logs go to `Timestone_host.log` and `Timestone_client.log`, and only the
focused window takes input. Not covered: anything Steam itself does (relay
routing, invites, the overlay, friends-only visibility).

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
