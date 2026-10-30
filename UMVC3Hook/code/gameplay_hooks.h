#pragma once

#include "../kiero/minhook/include/MinHook.h"
#include <cstddef>

// Only gameplay detours use this registration path. Graphics/launcher hooks
// remain installed when the user chooses Normal Launch.
MH_STATUS GameplayHooks_Create(LPVOID target, LPVOID detour, LPVOID* original);
// Queue only intended enables, then commit once after all gameplay installers.
MH_STATUS GameplayHooks_QueueEnable(LPVOID target);
bool GameplayHooks_Apply();
void GameplayHooks_RecordPatch(void* target, size_t size);
bool GameplayHooks_Disable();
bool GameplayHooks_IsActive();
void GameplayHooks_Activate();
