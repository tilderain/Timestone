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
- Font metrics (`.fsd`, type `0x1DB58B09`) are a 28-byte header and one
  `uint32` advance width per glyph (4096 for `nrodinpro`, 256 for `bada_b`).
  Message tables (`MSD`) store strings as glyph indices.
- UTF-8 becomes glyph indices in `uUiAnmSpriteFont__encodeTextGlyphIndices`
  `0x1402CB320` (vtable `+0x1D0`, called by `setText`), one character at a time
  through `sGameConfig__mapUtf8GlyphToFontIndex`: ASCII is `codepoint - 0x20`;
  anything else is searched for in a string at `sGameConfig + 0x4D39E` that
  lists the glyphs in atlas order, and is `0x60 +` its position there. A
  character that is not in the string becomes `0x1F`, a question mark. The
  string is UTF-16 in the executable (3521 characters starting
  `U+00A0 ¡ ¢ £ ™`): Latin-1, the JIS symbol rows, full-width alphanumerics,
  hiragana, katakana and the JIS level 1 kanji with a few more at the end. No
  Cyrillic, Greek, Hangul or half-width katakana.
- The same indices are used for every font. `bada_b` has 256 cells, so only
  the Latin-1 part reaches it, and five of those cells hold a different
  character than the string says (`0x64` ¤, `0x65` backslash, `0x77` œ,
  `0x78` Œ, `0x7E` ¿). `FontHasGlyph(codepoint, FONT_HEADING)` leaves those
  out; a name with a character the heading font lacks is drawn in the body
  font.
- `tools/gen_lobby_font_tables.py` writes the glyph order and both width
  tables to `lobby_font_tables.inc`, which `lobby_canvas.cpp` uses for
  `FontHasGlyph`, `TextWidth` and `FitText`. `msg_sys_menu_en` already contains lobby and
  spectator strings (ids 20 "SEARCH LOBBY", 21 "CREATE LOBBY", 1559
  "SPECTATING", 2033 "The battle will begin when both players are ready.").

### Text entry

- A text field (room name, preset name, chat) takes only characters the body
  font has; the rest are refused with the cancel sound instead of being kept
  as question marks. Limits are in characters (`kRoomNameChars`,
  `kPresetNameChars`) with the byte size of the buffer as a second bound.
- Names are written to `Timestone.ini` with every byte above ASCII, and `%`,
  as `%XX`. The ini goes through the system code page, which raw UTF-8 does
  not survive on a double-byte one.
- The game pumps messages with `PeekMessageA` / `TranslateMessage` /
  `DispatchMessageA` (`sApp__execute`), so `WM_CHAR` reaches the plugin's
  window procedure in the system code page, double-byte characters as two
  messages. `lobby_ime.cpp` decodes that, and `lobby_native.cpp` still makes
  characters from key edges (`ToUnicode`) in a frame where none arrived.
  Ctrl+Alt is AltGr there and types.
- Input methods: the game calls `ImmDisableIME(0)` at `0x1402559CD` on its
  window thread, which cannot be undone. `Ime_InstallHook` hooks the API so
  the call does nothing, and the window procedure detaches the window's
  input context (`ImmAssociateContextEx(window, NULL, 0)`) whenever no text
  field is open, attaching it again (`IACE_DEFAULT`) when one is. While
  attached, `WM_IME_STARTCOMPOSITION` / `WM_IME_COMPOSITION` /
  `WM_IME_ENDCOMPOSITION` are not passed on: the result string is read as
  UTF-16 with `ImmGetCompositionStringW` and queued like typed characters,
  and the composition string goes to `Input::composing`, which the field
  draws underlined after its text. The candidate list is the system's own
  window; it cannot show over exclusive fullscreen. Its place is given with
  `ImmSetCompositionWindow`, `ImmSetCandidateWindow` and a hidden system
  caret over the text line, since input methods differ in which they follow.
- Under Wine (read from Proton's `winex11.drv/xim.c`, branches 9.0 and 10.0):
  - The place reaches the Linux input method only through
    `X11DRV_SetIMECompositionRect`, which exists from Wine 10 and does
    nothing unless the driver's input style is "over the spot"
    (`InputStyle=overthespot` under
    `HKCU\Software\Wine\AppDefaults\<exe>\X11 Driver`). In that style the
    input method draws the unfinished text itself and the lobby only
    receives results, so the plugin leaves the default style: the unfinished
    text shows in the field and the list sits at the window's bottom left
    corner. (An unreleased build set the value; `RestoreWineInputStyle`
    removes it once.)
  - Nothing a Windows program can call takes the X input context's focus
    away, so with IBus the Linux input method keeps taking keys outside text
    fields. `ImmSetOpenStatus(FALSE)` (sent when the last field closes)
    becomes `XNPreeditState` disabled plus `XmbResetIC`, which clears a
    stuck composition; IBus's XIM server ignores the state itself.
- While a composition is in progress, and until Enter, Esc, Backspace, Space
  and the arrows are released after it, the keyboard is not read as menu
  input: those keys belong to the input method.

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
full-screen fill. In a room (and on the team screen opened from one) the
native lobby's comic plate (`ui\mainmenu\ma_sprite\meku_menu_co00_BM_NOMIP`,
1280x720, DXT1, in `ui\netroom`) fades in over it across 90 frames, in the
tint its own layout gives it (`a_lic_bg`: one `Sprite_bg`, `mColor`
0.2235, 0.2235, 0.2745, `mPass` 11, `mBlendState` 1, `mPrimitiveAttr` `0x1000`).

An earlier attempt drew nothing: the plate loaded but did not show through a
cloned sprite. The clone's prototype (`icon_RoomFriend`) has `mPrimitiveAttr`
(`+0x39C`) `0x1004`, while every native sprite with a plain `_BM_NOMIP`
texture keeps the constructor's `0x1000` (`uBaseSprite__ctor` `0x1402C33B0`;
`mPass` defaults to 22). The backend now clears bit 2 for this texture only.
The fix is **unverified in game**; if the plate still does not draw, the room
looks as it did before.

Bit 2 of `mPrimitiveAttr` selects the texture filter.
`cPrim__changeSpriteSamplerState` (`0x140785BD0`) binds the shader function
`FPrimitiveSampleBaseMap` when it is set and `FPrimitiveSampleBaseMapLin` when
it is clear. In `nativePCx64\UserShader\UserShaderPackage.mfx` the two records
differ only in the sampler paired with `tBaseMap`: `SSWrapPoint` (nearest
texel) for the first, `SSWrapLinear` (linear) for the second. The prototype
`icon_RoomFriend` carries `0x1004` (point); the backend clears the bit on
every sprite it draws (frames keep their prototype's value). The UI
textures are `_NOMIP`, so neither path has mip levels to fall back on when a
picture is drawn much smaller than its texture.

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

It also leaves the pad table (`sMvc3Manager::mPadId[4]`, `+0x140`) as that
screen does: `uMenuTitle__setup` empties it (`sMvc3Manager__resetPadIds`
`0x14025B070`) and Start gives player 1 a pad, so players 2 to 4 have none.
`ApplyPressStartState` keeps player 1's slot and clears the others, both when
the lobby takes the title area and when it hands the main menu back. A netplay
match maps slots 0 and 1 to both players (`SetupPadToPlayerMapping`), and
offline the game reads "player 2 has a pad"
(`sMvc3Manager__isPadAssigned` `0x14025AD10`) as a second local player:
`u2PCtrlSelect__setup` `0x140311A60` skips the 2P controller prompt when that
pad is connected, `cChrInput__processFrameInput` reads it for player 2's
characters, and training keeps its player 2 option
(`uBattleUiTrainingCheckP2PadAssigned_maybe`, call at `0x1402E57B6`). Left in place, a second device that is only plugged in
became player 2 after any online match.

On the top level of the main menu player 1 follows whichever pad is pressed
(`FollowActivePad`), since no Start press chose one. A pad assigned to another
player is left alone: one paired for local versus by `u2PCtrlSelect__move`
`0x140311800` stays player 2's, as natively. The Offline menu (`uMenuLocal`)
and the 2P controller select run unhooked.

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
  and `variant` picks the audible one: 0 is channels 0/1 of the six-channel
  main file, 1 the two-channel `_b` file, 2 channels 4/5 of the main file.
  The lobby offers all three, as the gallery's sound test does, numbered
  1 to 3 after the stage's name. Cues 90..104 cannot be
  requested directly (`sGSound__isStageAltMixCue` `0x1402AB480`).

The table's names are the soundtrack's track titles (fan rips of the two
albums; character themes without their "Theme of"). Which cue is which: only
13/14/16 have been heard in the lobby; the rest was checked against the executable, the cue table, the audio
files (`game_sounds/sound/bgm/source`, loop points in the Ogg tags), the
game's own sound test and the soundtrack listings.

- The gallery has a sound test (`uMenuGallery__activateSelectedContent`
  `0x14037CCE0`, cases 12..14) with three cue tables: characters
  `0x140B40930`, stages `0x140B40A20` (variant per entry at `0x140B40AA0`)
  and "other" `0x140B40B20`. Its text is in `msg_sys_menu2_en`
  (`ui\mnmain_msg.arc`, `MSD`: UTF-16 units of ASCII minus 0x20). The other
  list is 2, 3, 4, 5, 6, 8..19, 106..108, 112, 113, 84..88, 138, 132..137.
- Characters: every character class's theme virtual (`+0x2E0`) is
  `mov eax, cue; ret`. Cues 20..39 are the original Capcom cast, 40..55 and
  57..59 the Marvel cast, 60 Galactus, 120..125 and 126..131 the Ultimate
  additions; 121 is Wright's Turnabout theme. Cue 56 has no file. The sound
  test's character table holds exactly this set.
- Stages: `uStageNNN`'s theme virtual (`+0x238`) returns 70 + stage number
  (training room 70, Danger Room `uStage100` 71); the 1xx variants share the
  base stage's cue. The sound test lists each of 71..77, 79, 80 three times
  with variants 0, 1, 2 ("Stage theme 1/2/3"), and 81..83 as Fate of the
  Earth (1)(2)(3). Cue 78 (`bgm_st_008`) has no stage class and is not in the
  sound test: an unused stage theme. The r/mahvelmods index and fan
  soundtrack rips both call it the unused Okami stage; not heard here.
- BGM STYLE (`sGameConfig` `0x140251640`) is CLASSIC SOUND (0) or DYNAMIC
  SOUND (1); the pairs 2/3, 4/5, 9/10, 13/14 are classic/dynamic.
- 2/3 `sAction__executeMatchPhase`: the win screen (soundtrack "Victory").
- 4/5 character select. 106..108 are the three Marvel vs. Capcom 2 "Take You
  for a Ride" remixes the character select plays when a trigger is held
  (`aChrSelect` `0x140368840`): `bgm_gm_019` traditional, `bgm_gm_028`
  techno, `bgm_gm_029` rock. This is the r/mahvelmods filename index, and the
  loop lengths agree with the soundtrack's 1:30 / 1:34 / 1:26; a Steam
  modding guide labels 019 techno and 028 rock, which the lengths rule out.
- 9/10 `uBtContinue__setup`. 15 gallery. 16 the room, "Online Lobby music"
  in the sound test, "Network" on the soundtrack.
- 17 is what the leaderboard (`uMenuNetLeaderBoard_vfn21`) and the license
  screen return to: soundtrack "Ranking" (its loop gives the soundtrack's
  1:33). It was listed as "License" before, which was wrong.
- 18 `uMenuMission` (vtable slot `0x1403EC2C0`): "Mission Mode".
- 19 `aFinalResut__init`: "Congratulations screen music" in the sound test,
  "Arcade Clear" on the soundtrack.
- 112/113 are the same recording (`bgm_gm_026` without loop points,
  `bgm_gm_027` with): the sound test's "Main Theme music" full and looped
  versions. 113 is what the License Card screen plays
  (`uMenuLicense__setup`), hence "License Card" on soundtrack rips; 112 is
  played by `sMain__playInitialModeSound` for game mode 2.
- 132 (`bgm_gm_037`, no loop) is played by `uMenuTitle__move`; the
  r/mahvelmods index identifies it as the comic book intro.
- 133 Heroes & Heralds; 134/135 and 136/137 are its two themes per side
  (`aChrSelect__playRoundResultSound`, side 1 gets 134/135, side 2 136/137),
  "Heroes Ver. 1/2" and "Heralds Ver. 1/2" on the soundtrack. Which side is
  which follows the loop lengths against the soundtrack's track lengths.
- 84..88 `uEnding`, 138 `aCredit`.
- Left out: the short stingers (6 versus screen, 8, 11 game over, 12 retry).

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

## Steam invites

Read from the code and Ghidra on 2026-11-18; **not yet run with two Steam
accounts** (the test build has no Steam, and no log on record shows an invite).

Sending: INVITE FRIENDS opens Steam's invite dialog for the lobby
(`SteamRoom_InviteFriends`); a notice says so if Steam reports the overlay off.
Every room member also publishes `connect = "+connect_lobby <id>"` as rich
presence, which is what puts "Join Game" on the friends list, except in a
private room (lobby data `private = 1`): that one is reached by invitation only.

Receiving, by where the player is:

| Where | What happens |
| --- | --- |
| Game not running | Steam starts it with `+connect_lobby <id>`; `EnsureObjects` queues the lobby (`SteamLobby_QueueJoin`) and the first lobby frame joins it. |
| Lobby menus | The callback (`GameLobbyJoinRequested` 333, or `GameRichPresenceJoinRequested` 337 for "Join Game") queues the lobby; `SteamLobby_Pump` joins it. |
| In another room | `SteamLobby_Join` hands the lobby back as a replacement; `SteamRoomService::Pump` leaves the room and joins. |
| Match starting or running, or watching one | The move is held (`HoldRoomChange`, set from `SetPhase`); `LobbyFlow_GameTick` leaves the match as its menu would and the move is made back at the title. |
| Main menu (lobby released) | `LobbyFlow_ReleasedTitleTick` opens the lobby as Online does, once per invite. |
| Offline modes, training | Nothing pumps the lobby: the invite stays queued and is joined when the lobby is next opened. |

A failed join (full, gone, wrong version) shows the same notices as a join
from the list. A banned player gets in and is removed by the room refresh.

The game listens too: `MtNetMediator` registers its own handler for 333
(`MtNetMediator__onGameLobbyJoinRequested` `0x1404DA0A0`) and parses
`+connect_lobby` in its constructor. It only records the lobby and asks Steam
for its data. What would act on it (the MtNet context move at `0x140509174`,
then the join or error dialog in `sMvc3Guide::move`) sits under
`nNet::sComponent::move`, which the mod NOPs at startup
(`native_network_disable.md`), so the game never reacts to an invite of ours.
If that NOP is ever lifted, a Timestone lobby fails the game's check (no
`OwnerId` in its data) and the game would raise its "could not join" dialog.

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
2. **Input delay** (20 s, then the highlighted value). AUTO or 0..6, with the
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
3. **Start / character select** (30 s, then start). The START row names the
   team it starts with and left / right go through the player's teams (see
   Team presets); the team is shown under it as the three "next battle"
   cards with the assists named. Character select opens the team screen
   whose last row is START. Either way the player ends up `ready`; when both
   are, the match is started as before.

Cancel at any step gives the seat up. The service forgets a player's answers
whenever the opposite seat empties or changes hands, and after every match,
so the next pairing (including a rematch) starts from step 1. There is no
queue yet: "the next player" is whoever takes the vacated seat. The per-player
quick start / character select setting is gone.

## Queues and rotation

Each match slot has its own queue. A player joins one with confirm on a slot
that is full or already has people waiting (a free seat with nobody waiting
is still taken directly), and leaves it with confirm on that slot again, X or
cancel. A player is either seated or in one queue, never both.

There is no arbiter. Each player publishes `queueSlot` and `queueSeq` in
their member data (version 4); a joiner takes one more than the highest
`queueSeq` anyone in the room holds, so no clocks are involved. Every client
sorts a slot's queue by `(queueSeq, SteamID)` and gets the same order
(`Member::queuePos`, 1 = next). `SteamRoomService::RunQueue` runs after each
room refresh and only ever moves the local player: if they are at the front
and a seat at their slot is open, they sit (two open seats take the first two
in line). Sitting starts the pre-match prompts as usual; a player who lets the
ready prompt run out is stood up and not put back in the queue, so the seat
goes to the next in line.

The room's `ROTATION` setting (lobby data `rot`, fixed at creation) decides
who makes way after a set played to its end, in `FinishMatch`: with WINNER
STAYS the loser stands and rejoins the back of that slot's queue, with WINNER
LEAVES the winner does. Nobody moves when the queue is empty: the two then
get the prompts again as a rematch.

`MATCH FORMAT` also offers ENDLESS (`ft` = `0`; a missing or bad value means
first to 1). An endless match has no set: the two rematch until one of them
leaves, nothing goes on the room record, and since no set ever ends nobody is
rotated. Players queued for that slot wait for a seat to open, and the
rotation row on the create screen is inert.

The slot header shows `NEXT: name +n` and the player list shows `M2 #1`
(match, position) for everyone waiting. As with kick, the order is only as
honest as the clients: a modified one could publish a low `queueSeq`.

A lobby test build opens `kLobbyTestExtraInstances` extra windows
(`lobby_local.h`, 1 by default); a queue needs at least 2.

## Chat panel

Stepping off either end of the player list puts the cursor on the chat panel;
confirm expands it over the list (23 rows) and up/down or the shoulder buttons
scroll. The service keeps the last 5000 lines in a ring. Long messages wrap in
both views (`WrapChat`: at spaces, inside a word only when it must, at most 8
rows a message), so scrolling counts display rows from the newest; the window
is filled by walking back from the newest message, and the full history is
only measured when scrolling up, to find the top. Lines that arrive while
scrolled back push the view up by their own height.

## Match slot contents

A seat shows the player's name and their games in the current set (member
data `setWins`); there is no READY lamp. The team is not shown before the
match, so it cannot be counterpicked and is not shown stale.

While a match runs, the seat shows the team as three portraits with a life
bar each:

- **Portraits** are the license card icons, `ui\lic\lic_user_i\lic_user_iNNN`
  (64x64). `CharacterIcon` maps a character id to one; the id-to-icon table
  in `lobby_canvas.cpp` was read off the icon sheet (001..037 are the
  original cast in the license screen's order, 171, 174 and 177..188 the DLC
  and Ultimate characters). Nothing in the title area loads `ui\lic`, so
  `RequestLicenseArchive` asks for it the first time a portrait is drawn,
  the way `sGameUi__loadNetRoomArchive` `0x1402BDE30` asks for `ui\netroom`
  (name composed by `0x140455260`, `sResource` create with the archive DTI
  `0x140E17BD8` and flag 2); the textures resolve on a later retry.
  **Unverified in game.**
- **Life** is sampled every 3 seconds by each fighting player for their own
  team (`SampleTeamHealth`): `cChrStatus__getLifeRatio` `0x1400315C0` on
  `uCharacter+0x1530` for every character in the team list that has no parent
  (helpers are skipped), indexed by `mDefMemberId` `+0x2DAC`, rounded up to 4%
  steps and published in the member data (version 6, last field) only when it
  changes. Zero means defeated; 255 means not reported yet.

## Sets (first to N)

A room is created with a set length (`MATCH FORMAT`: first to 1, 2, 3, 5, 7 or
10, or endless, see "Queues and rotation"), stored in the lobby data as `ft` and fixed for the room's life.

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
on their own. The result is a line in the room's chat, not a modal; the other
player dropping at that point is treated as the normal ending. Only a set played to its end goes on the room's
win/loss record. The running score is published in the member data (version
2, last field) for the room's slot header and drawn above the health bars
with ImGui during the match.

## Spectating

**Status: built 2026-11-16, compiled only. Nothing here has run in game.**

A match is watched by simulating it. A spectator boots the same battle and is
fed the two players' confirmed inputs; no game state is ever sent, and a
spectator never predicts or rolls back. Code: `spectate.cpp` (log, feed,
playback decisions), plus the hooks named below.

### The log

One per game of a set (`Game` in `spectate.cpp`):

- the boot configuration (`TrainingBootConfig`, CPU mode, five bars);
- the round seed and both opening characters, known when the start-up
  barrier releases (`Spectate_RecGameStart`, from `HookedMove`);
- one input pair per frame, player 1 first, appended once the frame is final:
  `RecordSpectateFrames` in `controller.cpp` walks forward from the last
  logged frame while `IsFrameConfirmed` holds. The local value is
  `g_recordedLocalInput`, the remote one `g_receivedInput`; the neutral
  start-up frames (`<= delay + rollback`) are logged as zero for both;
- the player's checksum every 60 frames, logged a tick after the frames up to
  it were confirmed so any correction of them has already run;
- how it ended: restarted (with the restart seed and the roster the restart
  was given, so a roster change carries over) or the match is over
  (`Spectate_RecRestart` at the synchronized restart, `Spectate_RecEnd` from
  `FrameStepper_EndNetplaySession`).

The game id is the battle generation, which both players count alike, so the
two players' logs name the same games and a spectator can change who feeds it
and resume by frame number. The current game and the one before it are kept.

### The feed

Every member can hold the log and serve it. Links are reliable message links
between room members on their own virtual port (734; the match uses 733):
`SteamRoom_Spec*` in `steam_lobby.cpp`, ENet on localhost in the test lobby.
Both ends open a link, as with the match connection.

Who feeds whom is published, not negotiated. Member data (version 7) carries
`watchSlot`, `watchSeq` (order of starting to watch, assigned like queue
numbers) and `upstream`, the member the spectator chose to be fed by.
`ChooseUpstream` picks one of the two players while they have room, otherwise
a spectator of the same slot who started earlier (that ordering keeps the
links loop-free). A fighting player feeds one spectator, a spectator feeds
two; whoever claims a full source and is not among its earliest claimers
picks again. A member serves exactly the members whose `upstream` names it.

Links are opened ahead of need (`Spectate_Pump`): a setup's two players and
everyone watching it link up as soon as there is a watcher, and lining up
starts the watching once both players are ready (`SlotState::Starting`), not
only when they fight. A spectator therefore has links to both players before
the match runs; `ChooseUpstream` takes a source whose link is already up over
one still dialling. On Steam a request that arrives before this side has
dialled is kept and accepted (`AcceptIncomingConnection`), and the listen
sockets stay open for the life of the room.

The child sends `MSG_SUB` (the game and frame it has); the parent answers
with `MSG_GAME`, `MSG_START`, run-length encoded `MSG_FRAMES`, `MSG_CHECK`
and `MSG_FINISH`, then keeps appending. Live frames go out in batches of 12.
A backlog is capped per frame at 700 bytes from a player and 12000 from a
spectator. `Spectate_Pump` runs once per frame on the game thread, in the
lobby and in a battle. A lost or refused source is avoided for 8 seconds and
another is chosen; the subscription then resumes from the frame already held.

### Playback

A spectator runs the netplay client path against a virtual opponent:

- `Netplay_InitSpectator` starts a session with no peer and no service
  thread, so `Netplay_IsActive` is true and every determinism path the
  players use is on. Rollback frames are set to 0: no snapshots are taken.
- `HookedMove` answers the start-up barrier from the log
  (`Spectate_GetGameStart`): remote ready, release, seed and opening
  characters are set as if a peer had sent them.
- `ResolveTimelineInput` returns `Spectate_GetInput` for both teams;
  `hkGetsCurrentInput` and the raw pad read `0x1402B3B80` return nothing, so
  the spectator's own pad cannot reach the match. The result screen's native
  handler (`sAction__stepResult9_10_11`) does not run for a spectator.
- Each tick `Spectate_Next` decides: hold (nothing buffered; after 1.5 s the
  game's waiting notice is shown), play, restart (the log says the game was
  restarted: the same synchronized restart runs with the logged roster) or
  leave (the match ended).
- Catching up: with more than 90 frames buffered, up to 8 frames are
  simulated ahead of each drawn one, two ways (`SpectatorFrameStart`):
  - In the fight (action step 5), the rollback path itself:
    `Rollback_SaveSnapshot` of the current frame, the native frame counter
    moved on by the batch, then `Rollback_ReSimulate` over the batch. The
    snapshots are freed once caught up.
  - Anywhere else (the intro above all), undrawn live ticks
    (`Rollback_SetUndrawnLiveTicks`): the whole native tick, with only
    `sMain::move`'s pacing and the render end-frame call skipped, within a
    24 ms budget. Re-simulating in the intro crashed in the HUD clean-up.
  History, from the 2026-11-16 tests: the first version called the headless
  tick directly, with no snapshot or restore around it. The intro checks
  (frames 60 and 120, drawn) matched the players and every check after such
  a stretch differed in the two point characters' RNG only, with positions
  drifting a few seconds later. Which of the three differences from a real
  rollback was responsible (no restore, no per-frame saves, frame counter
  left behind) was not isolated. The user reported the rollback-path version
  in sync; the log of that run was not kept, so it is not known whether the
  fight-time path ran in it. `kFastForward` in `spectate.cpp` turns
  catching up off.
- Every 60 frames the spectator's checksum is compared with the players'. A
  mismatch is logged (`Spectate: out of sync...`) and shown; the next game
  starts from a fresh restart either way.

Every spectator is a late joiner: it boots only after the first game's
configuration arrives, so it starts behind by its own loading time and relies
on the catch-up. A game that was already restarted when the spectator arrived
is skipped; it boots the roster that follows and waits for the next game.

### Intro picks (found in the first test, 2026-11-16)

The first run matched the players on positions, life and action ids for a
whole game while every character-RNG hash differed, and the intros looked
different. `uCharacter__selectProfileVariant` `0x140055B50` picks each
character's intro row with percent rolls on the *global* XorShift
(`0x14005BF70`) and caches it at `+0x6828`. That happens during battle
set-up, before the start-up barrier seeds anything, so each machine rolled on
its own stream; a different intro then draws the character's RNG differently.
`hkSelectProfileVariant` (`rollback_rng.cpp`) now makes the intro pick roll on
a stream derived from the game number, the six characters and the asking
character, and restores the global stream. This applies to the two players as
well. **Unverified in game.**

### In the room

Confirm on a setup that cannot be sat at directly opens a three-item menu:
JOIN QUEUE & SPECTATE, JOIN QUEUE, SPECTATE ONLY. Watching is a standing wish
(`RoomService::SetWatchIntent`, local to the client): while it names a setup
and the player is not seated, `UpdateSpectateJoin` calls `Watch` for each
match there as it comes up. It is withdrawn by Stop Watching in the pause
menu, by cancelling while a match is being joined, by a join that fails, by
taking a seat (`Sit`), and by leaving the queue it came with. A spectator
whose queue reaches the front is taken back to the room.
X on a pad, Page Up on the keyboard (`Input::spectate`), toggles that wish for
the setup under the cursor without the menu; it does nothing from a seat, and
keeps a place in line. Leaving a seat or a queue is the cancel button's alone.
The setup card shows the WATCH tag and a count.
In the battle the top of the screen names the two players, the set score and
the state of the feed; the pause menu offers Stop Watching only.

### Known gaps

- Whether a freshly booted battle matches one reached by a rematch restart is
  assumed (both pass the same seeded barrier). The checksums will say.
- Undrawn live ticks are unverified in game: whether skipping the render
  end-frame call for several ticks in a row is harmless, and how the
  sped-up sound of a catch-up comes across.
- If the feed moves on while a roster change is pending and the spectator is
  between games, it can load the old roster for one game.
- The native spectator presentation (battle type 4, `ingameonlinechecks.md`)
  is not used.

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
focused window takes input. With `kLobbyTestExtraInstances` at 2 the third
window can watch the other two play (their spectator links are ENet on port
29100 + process id modulo 2000). Not covered: anything Steam itself does
(relay routing, invites, the overlay, friends-only visibility).

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

## Battle cards (research, 2026-11-16, not implemented)

The swaying license cards of the native room are `ui\net\net_sprite\net_room_card`
(loaded into `uMenuNetRoomMain::mpRoomSdlArray` by `setup`). Everything it needs
is in `ui\netroom[_lang]`, which the lobby already keeps resident: the layout,
the models `ui\lic\ga_lic_card_b\ga_lic_card_b_typeC` (1P, warm tint) and
`..._c\..._c_typeC` (2P, cool tint), their textures and the radar texture. Only
placeholder `lic_user/syogo/rank/flag` textures are in `netroom`; the real ones
are in `ui\lic` (`RequestLicenseArchive`). Verified statically.

Layout (dumped offline):

- `ga_LicCard1P` / `ga_LicCard2P` are `uUiModelBase` (DTI `0x140D559B0`, hash
  `0x000E25FF`; a DTI hash is `~crc32(name) & 0x7FFFFFFF`). Each has 15 children
  parented to it (`mpParent` = the card's track): `icon_LCnP_face` (40x40),
  `_syogo` (title plate) + `Font_syogoN`, `Font_LCnP_name`, `_rank`, `_flag`,
  `_num_1..3` + `_wins` (win digits), `_radar` + `_radar_p1` (`uUiAnmSpriteRadar`,
  5 values at `+0x704`, `uUiAnmSpriteRadar__setParam` `0x140309C90`). Children
  carry `mAngle.x = pi` because the card space is y-up.
- Coordinates: `uUiModelBase__draw` `0x140326A70` with `m2D` uses
  `sPrimitive__buildSpriteCamera` (eye `(0,0,990)`, 40 degree FOV, 16:9). At
  z=0 that spans 720 units, so card space is UI pixels **centred on the screen,
  y up**: UI `(x, y)` is card `(x - 640, 360 - y)`. Resting positions are 1P
  `(160, 160)`, 2P `(160, -35)`, both tilted by `mAngle` (-0.26, +/-0.26, 0).
- Frame ranges (`g_netRoomCardRange_*`, set by static initialisers
  `0x140A3A520..0x140A3A660`): slide-in with overshoot 0-30; clash bounce 80-120
  (looped while the match starts); 2P flies off 160-190; idle sway 200-490
  (looped: yaw rocks +/-0.26 rad over ~5 s); 1P drops out and 2P moves into
  its place 540-570; a new 2P slides in 600-650; both leave 650-670.

Driving (`uMenuNetRoomMain__updateBattleCardAnimation` `0x14042F8B0`):
`uMenuSubBase__setSchedulerRangeStartAndPause` `0x1403F6E80` seeks the scheduler
(`uScheduler::mFrame` `+0x44`) to a range start, `playSchedulerFrameRange`
`0x1403F6DA0` plays and pauses at the end (returns true when done), and
`uMenuSubBase__loopSchedulerFrameRange` `0x1403F6E20` seeks back to the start
once the end is passed. `refreshRoomDisplay` fills the cards at `this+0xA68`
(stride `0xF0`): name with `resetIndexAndSetText`, face/title/rank/flag with
`setTexture`, title text by message index, radar values, three character
portraits.

What a per-slot copy needs (static analysis; **unverified in game**):

- One `net_room_card` scheduler per match slot, loaded with
  `UI_LOAD_SCHEDULER` exactly like the prototype scheduler. Each instance
  spawns its own cards and children, so no copy constructor for
  `uUiModelBase` is needed.
- The keyframes are absolute, so each instance has to be moved as a whole.
  `uCoord__composeWorldMatrix` `0x140548730` multiplies by a live parent's
  `mWmat` (`mParentFlags` 3 on the cards inherits position, rotation and
  scale), so giving both cards a parent anchor at the slot's position (and a
  scale below 1) moves and shrinks the pair with its children, and keeps the
  animation local to the anchor.
- The scheduler would overwrite that parent: `uScheduler__updateTrack`
  `0x14050F590` re-applies every property track whenever the frame changes,
  and for a unit-reference track (`mpParent`, kind `0xC`) value 0 writes null.
  A track-work entry (`mTrackWork` `+0x58`, `0x68` bytes each) whose
  descriptor pointer (`+0`) is null is skipped for good; the scheduler does
  that itself when a binding fails. Clearing the two cards' `mpParent` entries
  in each instance and then setting `uCoord::mpParent` to the anchor is the
  intended route. SDL track indices are 1-based (track 0 is the header):
  `ga_LicCard1P` is track 32, its `mpParent` track 41.
- The plugin would drive the frame itself (seek, play, loop) with the same
  ranges. Room members carry no native license data, so face, title, rank,
  flag and radar would be the plugin's own choice (or hidden).

## Savedata (settings, controls, reserve teams)

The game loads `savedata.bin` on Press Start, not at boot. The lobby skips
Press Start, so it runs that step itself (`PumpBootSavedataLoad`).

- `uMenuTitle__move` (`0x1403F7DB0`) on Press Start calls
  `sGameConfig__spawnBootSavedataPreMenu` (`0x140253380`, mode 0). It spawns a
  `uUiPreMenu` at `sGameConfig+0x4ADF0` (alive test `0x1402532F0`, state at
  `+0x60`, dispatch `0x14032C8A0`). State 10 starts `startSavedataLoad`
  (`0x140253620`); its callback applies the payload (`0x14024F080`). On a good
  load the pre-menu sets `+0x4ADEC` (savedata loaded); otherwise it posts the
  native corrupt/missing/create-new prompts.
- `aTitle__init` (`0x140216940`): with `+0x4ADEC` clear, every title-area
  entry resets the character presets and the settings block at `+0x260`.
- Saves: option setters set `+0x4D351` (save reserved). Menus then call
  `0x140253B80`, which needs that flag and `+0x4D353` (storage enabled;
  `sGameConfig` starts with it set, `uMenuTitle__setup` sets it again). The
  pre-menu's save states call `startSavedataSave` (`0x1402536E0`), whose only
  payload writer is `packLiveStateIntoSavePayload` (`0x14024F210`).
- Without the load, settings were reset to defaults and later saves wrote
  those defaults over the player's save.
- The lobby retires `uMenuTitle` in state 1, before its setup runs, so the
  setup's `resetProfileAndMissionState` never runs in lobby mode.
- After a good load the pre-menu shows message 2900 when native matchmaking
  is unavailable (states `0x37`/`0x38`). The lobby ends the pre-menu at `0x39`
  there instead.
- Netplay clients copy their P1 bindings into P2 (`ApplyPlayerOneBindingsToPlayerTwo`).
  P2 binding state is `+0xEC` and `+0x144` (11 x u32 each), layout `+0x17C`
  and table cache `+0x328`. The player's own copy is swapped in around
  `packLiveStateIntoSavePayload` and written back when the lobby title area is
  set up after the match.
- Control type (`uMenuOptionFantastic`): `+0x191` fixed control type, `+0x190`
  set once that row has been used, `+0x188` `CtrlType` (0 Normal, 1 Simple),
  `+0x192` AutoJump. Character select applies `CtrlType` only when
  `+0x191` and `+0x190` are both 1; otherwise it asks. Battle boots send the
  resolved simple mode in the settings exchange
  (`LAUNCH_TRAINING_SIMPLE_MODE_VALID`).
- `sBattleSetting__restoreBackupAndInit` (`0x14024DE40`) writes
  `sBattleSetting+0x58C` into `CtrlType`. The battle boot puts the player's
  own `CtrlType` there first, so the save keeps it.

## Team presets

The team in use always belongs to a slot (`UiPrefs::slot`); there is no
unsaved "custom" team. Slots 0..9 are the player's own presets, kept in
`Timestone.ini` (`[LobbyPresets] NameN`, and `TeamN` as nine numbers:
character, assist, color for each member; an empty `TeamN` is a free slot).
Slots 10..12 are the game's reserve units A..C. `[Lobby] TeamSlot` is the slot
in use. On the first run the team so far becomes preset 1; the old
`[Lobby] Preset` (a reserve unit) is still read when `TeamSlot` is missing.

- Confirm on the team screen's PRESET row lists all thirteen slots (the row
  itself does not step; the start prompt's START row does). In the list confirm uses a slot (a free
  one first takes a copy of the team in use), X renames a preset and Y deletes
  it after a question. Names are typed on the keyboard, like the room name.
- Editing a character, assist or color writes straight into the slot
  (`LobbyUi::StoreTeam`).
- A reserve unit is written into the live license profile
  (`RosterSetPreset`: `sLicense+0x40`, tables `+0xB8` / `+0xDC` / `+0x100`),
  where the license screen edits them too. A color the install cannot load
  reads as color 1 and is only written back if the player edits the unit.
- No team holds a character twice, as in the game's own team editor
  (`0x1404214E0`; the game does not check stored teams). Picking a character
  already on the team makes the two members trade places, and a team read
  from the ini with a repeat has the later one replaced (`LoadPrefs`).
- The save file follows once the team screen is closed and no match is being
  agreed (`PumpReserveSave`): `sGameConfig__spawnSavePreMenuIfDirty`
  (`0x140253B80`) called as the gallery menu calls it (mode 1, reserve the
  save). It spawns the pre-menu in its save states (`0x20`, `0x21`), which
  pack the live state (`0x14021BA70` copies the `0x690`-byte license profile
  into the payload at `+0x20`) and write `savedata.bin`; on success it goes
  to `0x39` and ends. The pre-menu clears "savedata loaded" (`+0x4ADEC`) when
  built and sets it when the save is through, and `aTitle__init` resets every
  setting when it finds that byte clear, so the lobby does not leave for the
  main menu while the save runs and sets the byte again should the pre-menu
  end without doing so. **Unverified in game.**

### Random members

A team member can be `kCharRandom` (-1, `lobby_model.h`): the cell under the
roster in the character grid, drawn with the stage select's RANDOM icon. Its
assist and color rows show a dash and cannot be set. Any number of members may
be random. Only the player's own presets hold one (`[LobbyPresets] TeamN`
stores -1); the grid does not offer it while a reserve unit is in use, and a
team with one cannot be copied into a reserve unit.

The room is published the -1, so the opponent sees RANDOM until the match.
Player 1 draws both teams' random members in `SendLaunchAsHost`
(`ResolveRandomMembers`: a character not already on that team, one of its
installed colors, one of three assists) and the launch packet carries real
ids, so the receiving side and `ApplyTrainingLaunchPacket` are unchanged. A
set is one launch (rematches stay in the game), so the draw holds for the
whole set and is made again for the next one. `BeginLaunch` publishes the
team actually being played for the match slot's portraits and spectators;
`LobbyUi::ReturnToRoom` publishes the player's own choice again.

`kCharRandomAll` (-2, "RANDOM ALL", the cell beside RANDOM) is the same but
drawn again for every game of the set. Its first draw is the one above. For
the later ones `SetFormatPacket` carries which members are concerned
(`rerollMask`) and a seed (`rerollSeed`), both from player 1. Every rematch is
a synchronized full restart (`ChrSelImGui_ApplyRestart`), where each player
calls `LobbyFlow_RerollRandomMembers` and works the new members out from that
seed and the battle generation, so no draw is sent and both arrive at the same
team. These draws keep to the six standard colors, which every install has,
and take the owner's voice choice for the new character from the mask each
player sends in the initial settings exchange (`LaunchTrainingPacket::voiceMask`).
Spectators get the new roster with the restart, as for any roster change. A
team taken to the game's own character select is drawn once before the screen
and is not drawn again. **Unverified in game.**

A client from before this (0.6.4) fails `IsPlausibleTeam` on a -1 and shows
that member as not seated; lobbies are matched by version, so this only
arises between a development build and the release of the same number. Not
run in the game yet (2026-11-22); screens checked in the offline preview
(`team_random*`, `match_entry_random`).

### Preferred stage

Confirm on the team screen's PREFERRED STAGE row opens a grid of the stage select
screen's icons: its RANDOM icon for ANY STAGE, then the launcher's stage list
(`kStages`). The icons are `ui\chs\chs_stgt\chs_st_m\chs_stgm<NN>_BM_HQ_NOMIP`
(`_b` after the number for stage ids 100 and up, `99` with `_typeC` for
RANDOM): 128x128, the art in x 7..120, y 1..70. They are in `ui\mnchsstg`
(about 7 MB with the large previews), which the backend requests the first
time one is drawn and releases with its units. The choice is saved as `[Lobby] Stage` (a stage id, -1 for
any; it starts as Bonne Wonderland, 105) and published as its own member value
`tg`: 0 for no preference, else the stage id + 1; anything else reads as 0.
When player 1 sends the launch (`SendLaunchAsHost`) the stage is one of the
two players' preferred stages picked at random (`PickMatchStage`); a player
with no preference goes along with the other's, and when neither has one it
is any stage but the training room. **Unverified in game.**

### The lobby list's "more below" arrow

When the search results run past the last row, an arrow under it points down
at them (`DrawStageArrowDown`). It is the stage select page's arrow, drawn the
way that page's layout `ui\chs\chs_sprite\chs_option_sel` draws its left one
and turned a quarter turn with `Canvas::SpriteTurned`:

- Texture `ui\chs\chs_sprite\chs_opt_sel_b_BM_HQ_NOMIP`, 128x64, pointing
  left: the filled arrow in the left half, a glow outline in the right.
- `chs_sell_La` is the outline (UV 0.5..1), 32x32 at scale 1.5, with sprite
  animation 21. `chs_sell_Lb` is the filled arrow (UV 0..0.5), its child, in
  front of it, with a black shadow at (3, 3) and sprite animation 20. The
  right arrow is the mirror image (`chs_sell_Ra`/`Rb`, animations 22 and 20).
- The animations are in `ui\cmn\anm\sprite_base`, both 59 frames long.
  21: from frame 1 position x 0 -> 4 over 30 frames (mode 2), from frame 30
  back to 0 over 30 (mode 1). 20: from frame 1 angle x 0 -> 360 degrees over
  60 frames (mode 1), the arrow turning about its own long axis. The plugin
  draws that turn as the sprite's width across the arrow times |cos|.
- Modes as `uUiAnmSprite__interpolateChannel` `0x1402C7B90`: 1 linear,
  2 u^2, 5 1-(1-u)^2.

The texture is in `ui\mnchs` (about 1.6 MB; `ui\mnchscmn` has it too),
requested the first time it is drawn and released with the units, like the
stage icons' archive. `ui\mnchsstg` has no arrow art. **Unverified in game.**

### The cards

`ui\res\res_stgm\stgm_p\p_<Name><NN>_BM_HQ_NOMIP_typeB` (Japanese) and
`..._typeB_other` (every other language): 512x256, format `0x2A`, the art in
the window x 32..479, y 40..215, with the character's name in it. These are
player 1's cards on the "next battle" screen (`0x140414500`); player 2's are
`stgm_q\q_..._BM_NOMIP`, DXT1, which the lobby does not use. Each character
and color has an archive of its own, `ui\res\arc\chr\l%04d_%02d` (`s...` for
the `q_` set), which the game requests asynchronously
(`sGameUI__loadNextBattlePreviewArchives` `0x1402BDEC0`). The backend does the
same the first time a card is drawn (`BannerArchiveReady`), keeps the handle
until its units are released, and creates the texture once the archive's state
word (`cResource+0x54`) has bit 0 (loaded) or bit 4 set. The name is composed
with `0x1404552A0` mode 2. A dark plate holds the card's place until it arrives. **Unverified in game.**

## Player icons and Steam profile pictures

Each player picks a lobby icon on the team screen (ICON row): their Steam
profile picture (the default) or any of the game's 240 license icons
(`ui\lic\lic_user_i000`..`239`), picked from a grid. The choice is saved as
`[Lobby] Icon` and published as its own member value `ti` (a decimal number:
0 for the Steam picture, else the icon's number + 1), beside the member string
`ts`, whose format is unchanged. A value outside that range reads as 0.

Steam pictures are drawn through the game's own sprites:

- `rTexture` keeps its GPU texture (`nDraw::Texture`) at `+0xE0`
  (`rTexture__load` `0x140515240`, `rTexture__setHandle` `0x140515A60`); width
  and height are at `+0xF0` / `+0xF4`.
- `rTexture__load` builds that texture with `0x140781C40` (allocate `0x78`
  bytes, align `0x10`) and the constructor `0x140781720`
  `(this, u16 width, height, u8 mips, type 1, format, usage 0, flags, 0, 0, pixels)`.
  `nDraw__Texture__create` `0x140782060` copies the pixels in at once.
- Release is vtable `+0x28` (`0x140783650`); the engine defers the free while
  a recent frame still references the texture.
- The license icons (`ui\lic\lic_user_i`, 64x64, one mip) are TEX format
  `0x2A`: DXT5 with luma in the alpha block and Cr / real alpha / Cb in the
  colour block's red / green / blue. `lobby_avatar.cpp` encodes a 64x64 RGBA
  picture into that layout (4096 bytes).
- Nine country flags (`ui\lic\lic_flag_i`, the same 64x64 `0x2A` format,
  never drawn by the lobby) are the slots (`TEX_AVATAR_FIRST`); every license
  icon is selectable, so none of those can be used. `Native_AvatarTex` gives a
  Steam id a slot, builds a texture from `SteamLobby_GetAvatar64` and swaps it
  into the flag's `+0xE0`, keeping the original pointer. `Native_Shutdown` /
  `Native_Invalidate` put the originals back and release ours before dropping
  the references.
- Nothing parses image data from another player: Steam returns decoded
  pixels, only an exactly 64x64 picture is accepted into a fixed buffer, and
  the encoder's input and output sizes are constants.

## Character faces

The team screen draws the character select faces: loose files
`ui\chs\chs_face_a\chs_cs_f\f_<Name><NN>_BM_HQ_NOMIP.tex`, 128x128, format
`0x2A`, one per character (id order: Ryu, Chunli, Gouki, ...) and color
`00`..`07`. The art is the same 120x84 window in every file (x 4..123,
y 42..125); the rest is transparent. They are not in any archive the lobby
holds, so the backend loads them on first draw:
`sResource__createFromDTIPath` (`0x140516D70`) returns a cached resource, else
with flag bit 0 reads the file synchronously (bit 1 would queue it on the
loader thread). At most four are read per frame (`ARC_LOOSE` in
`lobby_native.cpp`). The character grid shows every face; the team card shows
the three picks in their set colors.

## Not done yet

- **Native character select.** "Character select" entry currently opens the
  native-styled team panel. Driving the real screen means queueing `aChrSelect`
  with only the local side active and capturing the result at
  `aChrSelect__onSelectionComplete` instead of letting it queue the battle.
- Simple mode, stage choice and win/loss records are not exposed in rooms.
- The pause-menu display settings are not reachable from the lobby.
- Returning to the room stalls for about a second while ENet waits for a
  disconnect acknowledgement from a bridge that is already gone.

## Character colors

Reverse engineered for `chr_colors.cpp`; the select screen's color ids are
zero-based archive variants (`chr\archive\%04d_%02d.arc`).

| Color | Label (msg id) | Gate |
| --- | --- | --- |
| 0..5 | `1`..`6` (`0xE1`..`0xE6`) | none |
| 6 | `H` (`0x95C`), the Herald color | cSecret bit `characterId + 0x33` |
| 7 | `ALT` (`0x95D`), the alternate costume | cSecret bit `characterId + 0x352` |
| 8 | never offered | Akuma, Wesker, Doctor Doom, Dormammu only (Herald boss forms) |

- cSecret is `sGameConfig + 0xF70`, a bitfield at `+0xC`, tested by
  `0x140207C60`. It is part of the save file.
- `sGameConfig__synchronizeUnlockAwards` (`0x140255370`) rewrites the `+0x352`
  bits from the DLC ownership bits (`sGameConfig + 0x4D35C`) each time the main
  DLC phase runs, setting or clearing them.
- The gates are `aChrSelect__cycleUnlockedColor` (`0x1402BC1A0`),
  `RandomColorIDGenerator` (`0x1402BC410`), `sGameUI__isCharVariantSelectable`
  (`0x1402BC990`), the model viewer and a team edit menu (`0x1404214E0`).
- Loading has no gate: `cChrArchiveTable__requestArchive` (`0x14001EF00`)
  only checks the variant count table at `0x140A6F9F0`.
- Magneto has a face for color 7 but no `0029_07.arc` on PC.

Timestone hooks the bit test and answers "owned" for those gate callers when
the archive is installed, so nothing is written to the save and real unlocks
are still recorded. The lobby, the in-match selector and the match boot all
use the same install-based rule, so both peers resolve a team's colors alike.

## The Xbox 360 lobby backdrop (`ui\lic\lic_bg\a_lic_bg`)

From the Xbox 360 build (`nativeXenon`, `Defaultdec.xex` in Ghidra on 8193),
2026-11-20. The PC build still packs the scheduler `a_lic_bg` (1177 bytes) but
none of its model, material or textures.

There is no drawing code for it. `Function_8271E2A0` hands the path
`ui\lic\lic_bg\a_lic_bg` to the generic scheduler loader `Function_82727B90`,
which stores the `uUiScheduler` at `this+0x3C`; `Function_82A48888` only names
the same path in a list of resources it registers (`0x82A33618`,
`0x8278F160`). Everything else is data, in `ui\netroom.arc` and
`ui\mnmain_xbox.arc`:

- **Scheduler** (`.sdl`, 0x4C0DB839): a 6000-frame loop (100 s at 60 fps) with
  two units. `a_lic_bg` is a model unit (`mpModel` = the rModel below, draw
  view 0x80, no movement of its own). `MaterialVjobCngColor` targets it with
  four controllers, one per material control id, each with two tracks:

  | id | material | `UVOffset0` over the loop | `BaseMapFactor` |
  |---|---|---|---|
  | 1 | stars, bottom plane | v 0 -> -1 | 1.0, rising to 1.4 at frame 4499, back to 1.0 |
  | 2 | grid, bottom sheet | v 0 -> 25 | (0.4,1,1,0.5) -> (0.7,0.4,1,0.25) -> (1,0.4,0.4,0.5) -> (0.7,1,0.4,0.5) -> back, at frames 0/1499/2999/4499/5999 |
  | 3 | grid, top sheet | v 0 -> 25 | as id 2 |
  | 4 | stars, top plane | v 0 -> -1 | as id 1 |

- **Model** (`.mod`, 0x58A15856, v0xCA, 426 vertices, 24-byte vertices:
  float3 position, packed normal, half2 UV, colour), five meshes:
  - two star planes, 10 vertices each: x +-1160, y 0..+-1160, sloping from
    z -700 on the centre line to z -450 at the top and bottom edges; UV
    u 0.53..2.47, v 0.5..1.5 (edge to centre). The bottom one is the top one
    turned half a turn.
  - two grid sheets, 200 vertices each: x +-2381, |y| 127..1387, curved from
    z -500 near the centre to z -407 at the edge; UV u 0..12, v 0..6.
  - a band across the centre, 6 vertices: x +-1200, y +-300, z -600..-650,
    textured with `chs_bg_black` to hide where the halves meet.
- **Materials** (`.mrl`, 0x2749C8A8): five materials over three textures,
  `chs_bg_a01` (1024x1024 DXT1 starfield), `chs_bg_grid` (256x256 DXT5, one
  cross per tile) and `chs_bg_black`. The grid materials are the blended ones
  (`XfBA_*` in the model's material names).

So the picture is a floor and a ceiling receding to a horizon across the
middle of the screen: stars drifting slowly on the two planes, and over them
two grid sheets whose lines run towards the horizon at a quarter of a tile per
second while their tint cycles cyan, violet, red and yellow-green.

Unpacking: Xbox archives are `\0CRA` big-endian with LZX (XMemCompress) frames,
`[0xFF, usize16, csize16]` or `[csize16]` for a full 0x8000 frame; the table's
size field is the true size shifted left three bits. Textures are `\0XET`,
DXT with 16-bit words swapped and Xbox 360 tiling.

### The grid sheets in detail, and the plugin's version

The grid mesh is a lattice half a tile apart: 25 points across (u 0..12) by
rows at v 0, 0.5 .. 3 and one more at v 6. Since the cross texture has a line
on each tile edge and through its middle, the grid's lines are exactly the
lattice's edges. The rows are arcs, not straight: at v 0 the row runs from
(x -1269, y 127) at its ends up to y 196 in the middle, and each row out is
wider (x +-1269 at v 0, +-1825 at v 3) and higher. That is what bends the grid
like a map's graticule. Vertex colour carries alpha, 0 on the row at v 0 and
rising by 42 per row to 255 at v 3: the grid fades in from the horizon. With
the material's own alpha (0.5, dipping to 0.25 a quarter of the way through
the loop) that is all of its fading.

`space_bg.cpp` draws this from the same numbers (`DrawSpaceBackground`, called
by `LobbyUi::DrawBackdrop` in a room in place of the comic plate):

- The view is ours, the game's was not recovered: a 45 degree vertical field,
  876 units in front of the model's origin. At that distance the star planes'
  far edge exactly spans a 16:9 screen, and their u range 0.53..2.47 comes out
  edge to edge, which suggests it is close to the original.
- Stars: 18 horizontal strips per half, each a window of the star texture
  (`TEX_SPACE_STARS`) sized for its depth.
- Grid: the lattice (`kGridMesh`, the upper sheet; the lower is its mirror)
  projected and drawn edge by edge with `Canvas::Line`, about 300 segments a
  frame. Columns stay put; the rows slide in by `kGridTilesPerLoop`.
- `Canvas::Line` is a `TEX_WHITE` sprite turned through `uCoord`'s rotation
  quaternion (`+0x60`, read by `uCoord__updateLocalMatrix 0x140548570`).
- The star image is the plugin's own (`UMVC3Hook/assets`, CC0 source): the
  `SPACE_STARS` resource, decoded, laid out 2 x 2, converted to the icon
  format and put into the `germany` flag texture the way profile pictures
  are (`EnsureSpaceStars`).

### What is and is not exact in the plugin's backdrop (2026-11-20)

Exact, read from the Xbox files: the timeline (6000 frames), both scroll
amounts, the five colour keys and their frames, the grid lattice (every
vertex of rows v 0..3), the vertex alpha ramp, the star planes' geometry and
texture window, and the cross texture's line (core colour 0.46/0.74/0.86 at
alpha 230, two texels wide; halo from 72 down over about thirteen texels).

Not recovered:

- **The camera.** The unit is a `uUiModelBase` (DTI hash `0x000E25FF`, as the
  license cards). `uUiModelBase__ctor` `0x140326200` leaves `m2D` false and
  the layout does not set it, so `uUiModelBase__draw` `0x140326A70` skips
  `sPrimitive__buildSpriteCamera` (eye (0,0,990), 40 degrees) and the model
  is drawn by whatever camera its draw view (`mDrawView` 0x80) has in the
  lobby scene. That camera was not found. `space_bg.cpp` uses 45 degrees from
  876 units, where the star planes exactly span the screen; the sprite
  camera's numbers would overscan them by 6%.
- **Blend modes.** The materials' blend-state hashes (`62B2D152` stars,
  `D3B1D158` grid, `23BAF154` band) were not resolved; the plugin blends by
  alpha. If the grid is additive on the Xbox it is brighter there.
- **Key interpolation.** The keys carry type 3; treated as linear.
- **The centre band's texture**, which did not decode to anything usable; the
  plugin fades black out from the centre line over the band's extent.

Deliberate differences: the star art; the stars' factor, which peaks at 1.4
on the Xbox and here rests at 1/1.4 and peaks at 1; the lower star plane,
turned half a turn on the model and here reading further down the tile.
