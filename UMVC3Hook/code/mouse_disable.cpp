#include "../pch.h"
#include "mouse_disable.h"
#include <cstring>

void __fastcall MouseDisable_Move(uint64_t mouse)
{
    // sMouse::move is bypassed entirely: no DirectInput or native cursor APIs.
    // Keep configuration, COM ownership and visibility flags intact.
    const int32_t outside[] = {-1, -1};
    std::memcpy((void*)(mouse + 0x38), outside, sizeof(outside));
    // Relative axes/wheel, held/previous/edge/repeat masks and eight timers.
    std::memset((void*)(mouse + 0x44), 0, 0xA8 - 0x44);
}
