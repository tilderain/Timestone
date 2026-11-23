#pragma once

// Native-engine backend for the lobby UI: draws lobby::Canvas primitives with
// the game's own uUiAnmSprite / uUiAnmSpriteFrame / uUiAnmSpriteFont units,
// reads menu input from sPad and plays the native menu sound cues.
// See doc/native_lobby_ui.md for the reverse-engineering notes behind it.
//
// Everything here must run on the game thread while a 2D UI view exists
// (the title area hold).

#include "lobby_canvas.h"
#include "lobby_model.h"

#include <cstdint>

namespace lobby {

// Requests archives, builds the prototype units and resets the per-frame
// pools. Returns the canvas once the backend can draw; nullptr while native
// resources are still loading (call again next frame).
Canvas* Native_BeginFrame();
// Hides units that were not used this frame.
void Native_EndFrame();
// Kills every unit the backend created and drops its resource references.
// Call before requesting an area transition away from the title area.
void Native_Shutdown();
// Forgets all unit pointers without touching them. Use when the engine has
// already destroyed the units (e.g. the title area re-ran sUnit::killAll).
void Native_Invalidate();
bool Native_IsReady();
// Texture showing a Steam user's profile picture; the default player icon
// until it is available. Game thread, while the backend is up.
Tex Native_AvatarTex(uint64_t steamId);

// Merged pad + keyboard menu input with edge detection and direction repeat.
// textActive routes typed characters to Input::typed and suppresses the
// letter-key shortcuts.
void Native_ReadInput(Input& input, bool textActive);
// Window-procedure side of text entry: one UTF-16 code unit of what was
// typed (see lobby_ime.h, which decodes WM_CHAR and input-method results).
void Native_OnChar(uint32_t utf16);

void Native_PlaySfx(uint32_t sfxMask);
// Brings the game window to the front (and flashes it if Windows refuses).
void Native_FocusGameWindow();
// Makes sPad start polling its controller slots (see the definition). Safe to
// call every frame; needed by the native main menu as well as the lobby.
void Native_EnsurePadSlotsActive();

} // namespace lobby
