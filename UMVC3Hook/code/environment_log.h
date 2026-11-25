#pragma once

#include <windows.h>

// What a tester's machine is, written to Timestone.log so a report can be
// read without asking. None of this runs per frame.

// Once, at process start: processor, memory, Windows build, power.
void EnvironmentLog_Boot();

// The DLLs in the process that are neither Windows' own nor the game's:
// overlays, injectors, other mods. Written only when the list differs from
// the one last written, so it can be called wherever something may have been
// injected since (device creation, match launch).
void EnvironmentLog_Modules();

// The game controllers plugged in, by name, with the API each answers to.
// Opening the devices to ask their names can take a moment, so this is done
// on a thread of its own and the lines arrive shortly after the call.
void EnvironmentLog_Controllers();

// The picture: back buffer, windowed/borderless/fullscreen, vsync, and the
// mode of the monitor the window is on. Written only when it changes.
void EnvironmentLog_Display(HWND window, unsigned width, unsigned height, bool windowed,
                            bool vsync);
