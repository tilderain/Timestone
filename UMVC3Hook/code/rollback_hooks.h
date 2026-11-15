#pragma once

#include <cstdint>

using RollbackMainMoveFn = void(__fastcall*)(uint64_t);

void Rollback_InitMainHooks(RollbackMainMoveFn* originalMove);

// Battle theme: save the live BGM handle before restore, then put it back and
// start the corrected timeline's theme if it differs from the one playing.
void Rollback_BeginThemeResim();
void Rollback_EndThemeResim();
