#pragma once
#include <d3d9.h>

bool D3D9Ex_Initialize();
bool D3D9Ex_Enabled();
// The Ex device could not be created: use the ordinary backend for this run.
void D3D9Ex_FallBack();
// What the game is rendering with, for display.
const char* D3D9Ex_BackendName();
void D3D9Ex_Prepare(D3DPRESENT_PARAMETERS& parameters);
HRESULT D3D9Ex_Create(UINT adapter, D3DDEVTYPE type, HWND window, DWORD behavior,
    D3DPRESENT_PARAMETERS* parameters, IDirect3DDevice9** output);
bool D3D9Ex_IsDevice(IDirect3DDevice9* device);
HRESULT D3D9Ex_Reset(IDirect3DDevice9* device, D3DPRESENT_PARAMETERS* parameters);
HRESULT D3D9Ex_Check(IDirect3DDevice9* device);
// One log line of what the Ex backend was doing, written when a Direct3D call
// has failed: the game treats any such failure as fatal.
void D3D9Ex_LogState();
