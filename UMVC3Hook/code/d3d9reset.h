#pragma once

#include <cstdint>
#include <d3d9.h>

// Install the property queue, apply/release boundary and UI job controller
// before the renderer starts. Device ownership comes from the native renderer's
// UI creation job, never from arbitrary CreateDevice calls. A pending transition keeps the
// native game stack suspended; the UI and network threads remain independent.
bool D3D9Reset_InstallNativeHooks();
[[noreturn]] void D3D9Reset_AbortStartup(const char* reason);

// A successful Begin holds the plugin device lock until End. Never hold this
// lease across the original EndScene, Reset, or a native UI-thread submission.
bool D3D9Reset_BeginOverlay(IDirect3DDevice9* device);
void D3D9Reset_EndOverlay();

void D3D9ResetDiagnostics_RecordGraphicsSync();
void D3D9ResetDiagnostics_BeginAppReset(bool forwarded);
void D3D9ResetDiagnostics_EndAppReset(uint64_t result, uint64_t elapsedMs);
void D3D9ResetDiagnostics_RecordDeviceReset();
void D3D9ResetDiagnostics_StartStartupWatchdog();
void D3D9ResetDiagnostics_CompleteStartup();
