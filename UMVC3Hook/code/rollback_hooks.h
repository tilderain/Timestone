#pragma once

#include <cstdint>

using RollbackMainMoveFn = void(__fastcall*)(uint64_t);

void Rollback_InitMainHooks(RollbackMainMoveFn* originalMove);

// Battle theme: save the live BGM handle before restore, then put it back and
// start the corrected timeline's theme if it differs from the one playing.
void Rollback_BeginThemeResim();
void Rollback_EndThemeResim();

// Round-end UI: record sAction's step and defeated fighters after each
// simulated frame (live frames record from the battle UI hook).
void Rollback_RecordBattlePresentation(int32_t frame);
