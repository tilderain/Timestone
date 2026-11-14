#pragma once

#include <d3d9.h>

// The renderer sets its shader constants again for every draw, whether or not
// they changed: about 4,600 SetVertexShaderConstantF / SetPixelShaderConstantF
// calls per frame, three quarters of them writing the values the registers
// already hold. This keeps a copy of both register files and answers those
// calls itself, so the device is not told that unchanged constants are dirty.
//
// Measured 2026-11-13 by switching it on and off every two seconds in a match:
// 70-75% of the calls dropped, 0.12-0.27 ms per frame less on the render path
// (the set calls themselves, and the draws that no longer re-upload constants).
// Rendering only; nothing here is read by the simulation.

// Hooks the two methods on `device`. Call once, when the device is recorded.
void ShaderConstants_Install(IDirect3DDevice9* device);
// The copy may no longer match the device: after a Reset, and once per frame
// so that state restored behind these hooks (the overlay's state block) cannot
// leave a stale entry for longer than a frame. Safe from any thread.
void ShaderConstants_Forget();
