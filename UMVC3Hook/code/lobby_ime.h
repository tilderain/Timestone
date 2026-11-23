#pragma once

// Input-method (IME) support for the native lobby's text fields, so names and
// chat can be typed in Japanese.
//
// The game switches input methods off for its window thread at startup
// (ImmDisableIME(0) at 0x1402559CD), which cannot be undone afterwards.
// Ime_InstallHook stops that; from then on the window's input context is
// detached whenever no lobby text field is open, so an input method never
// takes keys meant for the game.
//
// While a field is open the composition is drawn by the lobby itself
// (Input::composing) and the result is read as UTF-16 straight from the input
// context: the game pumps messages with PeekMessageA, which would squeeze
// WM_CHAR through the system code page. The candidate list stays the system's
// own window, placed under the text.

#include <Windows.h>
#include <cstddef>

namespace lobby {

// Once, with MinHook initialised and before the game's entry point runs.
void Ime_InstallHook();

// Window-procedure side, for every message of the game window. `wanted` is
// whether a lobby text field is open. True when the message is finished with
// (its result is in *result); otherwise it is passed on, with *lParam as it
// may have been changed here.
bool Ime_WindowMessage(HWND window, UINT message, WPARAM wParam, LPARAM* lParam, bool wanted,
                       LRESULT* result);

// Game-thread side.
// A text field opened or closed, or its text's end moved: has the window
// procedure look again now.
void Ime_Wake();
// An input method is on and takes the keys typed: characters only come from
// the window procedure, never from key states.
bool Ime_Open();
// A composition is in progress: Enter, Esc, Backspace and the arrows belong
// to it, not to the menu.
bool Ime_Composing();
// The unfinished composition as UTF-8 (empty when there is none).
void Ime_GetComposition(char* utf8, size_t size);
// Where the text being typed ends, as fractions of the client area: the
// candidate list opens there.
void Ime_SetCaret(float x, float y);

} // namespace lobby
