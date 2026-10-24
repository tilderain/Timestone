#pragma once

#include <Windows.h>

bool KeyboardInput_Initialize();
// Called once by the serialized render callback, then shared with overlay consumers.
bool KeyboardInput_Read(BYTE (&state)[256], bool (&pressed)[256], bool frozen);
