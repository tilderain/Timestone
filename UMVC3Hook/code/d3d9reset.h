#pragma once

#include <cstdint>

void D3D9ResetDiagnostics_RecordGraphicsSync();
void D3D9ResetDiagnostics_BeginAppReset(bool forwarded);
void D3D9ResetDiagnostics_EndAppReset(uint64_t result, uint64_t elapsedMs);
void D3D9ResetDiagnostics_RecordDeviceReset();
void D3D9ResetDiagnostics_StartStartupWatchdog();
void D3D9ResetDiagnostics_CompleteStartup();
