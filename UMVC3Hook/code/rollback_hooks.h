#pragma once

#include <cstdint>

using RollbackMainMoveFn = void(__fastcall*)(uint64_t);

void Rollback_InitMainHooks(RollbackMainMoveFn* originalMove);
