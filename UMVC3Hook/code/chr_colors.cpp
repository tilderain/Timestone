#include "../pch.h"
#include "chr_colors.h"
#include "mk10utils.h"

#include <Windows.h>
#include <atomic>
#include <cstdio>
#include <cstring>

#include "../kiero/minhook/include/MinHook.h"

extern void HookLog(const char* fmt, ...);

namespace {

constexpr int kFirstCharacter = 1;
constexpr int kLastCharacter = 50; // 51 is Galactus, with a single variant

// sBattleSetting__getCharVariantCount's table: archive variants per character
// id (int32 each, ids 0..53). 8 for the cast, 9 for the four Herald bosses.
constexpr uintptr_t CHAR_VARIANT_COUNT_TABLE = 0x140A6F9F0;

// cSecret's ownership bit test (cResource__getAnmchrOwnedBit in Ghidra).
constexpr uintptr_t SECRET_GET_OWNED_BIT = 0x140207C60;
constexpr uint32_t kHeraldBitBase = 0x33;  // + character id
constexpr uint32_t kAltBitBase = 0x352;    // + character id

// Return addresses of the ownership tests that gate a color choice. Only
// these are answered for; the tests that grant and record real unlocks keep
// seeing the save file's own bits, so progress is still saved as normal.
constexpr uintptr_t kColorGateReturns[] = {
    0x1402BC23E, // aChrSelect__cycleUnlockedColor
    0x1402BC48E, // RandomColorIDGenerator
    0x1402BCAB2, // sGameUI__isCharVariantSelectable, color 7
    0x1402BCACE, // sGameUI__isCharVariantSelectable, color 6
    0x14020F3AA, // aModelViewer__requestSelectedCharacterArchives, color 7
    0x14020F3CF, // aModelViewer__requestSelectedCharacterArchives, color 6
    0x1402113DC, // aModelViewer__updateModelViewerControlsAndSelection, color 7
    0x140211401, // aModelViewer__updateModelViewerControlsAndSelection, color 6
    0x1404219AF, // team edit menu (FUN_1404214e0), color 6
    0x1404219E1, // team edit menu (FUN_1404214e0), color 7
};

// 0 = not looked at yet, 1 = archive present, 2 = archive missing.
std::atomic<uint8_t> g_archive[kLastCharacter + 1][kChrColorSlots];

bool ArchiveInstalled(int32_t characterId, int32_t color)
{
    std::atomic<uint8_t>& cached = g_archive[characterId][color];
    uint8_t state = cached.load(std::memory_order_relaxed);
    if (state == 0) {
        char path[MAX_PATH + 64] = {};
        const DWORD length = GetModuleFileNameA(nullptr, path, MAX_PATH);
        char* slash = length > 0 && length < MAX_PATH ? strrchr(path, '\\') : nullptr;
        if (slash) {
            snprintf(slash + 1, sizeof(path) - static_cast<size_t>(slash + 1 - path),
                     "nativePCx64\\chr\\archive\\%04d_%02d.arc", characterId, color);
            const DWORD attributes = GetFileAttributesA(path);
            state = attributes != INVALID_FILE_ATTRIBUTES &&
                    !(attributes & FILE_ATTRIBUTE_DIRECTORY) ? 1 : 2;
        } else {
            state = 1; // cannot tell: trust the game's own variant table
        }
        cached.store(state, std::memory_order_relaxed);
    }
    return state == 1;
}

int VariantCount(int32_t characterId)
{
    const int32_t count =
        reinterpret_cast<const int32_t*>(_addr(CHAR_VARIANT_COUNT_TABLE))[characterId];
    return count < kChrColorSlots ? count : kChrColorSlots;
}

typedef bool(__fastcall* GetOwnedBit_t)(void* secret, uint32_t bit);
GetOwnedBit_t oGetOwnedBit = nullptr;

bool __fastcall Hook_getOwnedBit(void* secret, uint32_t bit)
{
    if (oGetOwnedBit(secret, bit)) return true;

    int32_t characterId, color;
    if (bit >= kHeraldBitBase + kFirstCharacter && bit <= kHeraldBitBase + kLastCharacter) {
        characterId = static_cast<int32_t>(bit - kHeraldBitBase);
        color = 6;
    } else if (bit >= kAltBitBase + kFirstCharacter && bit <= kAltBitBase + kLastCharacter) {
        characterId = static_cast<int32_t>(bit - kAltBitBase);
        color = 7;
    } else {
        return false;
    }

    const uintptr_t caller = reinterpret_cast<uintptr_t>(__builtin_return_address(0));
    for (const uintptr_t gate : kColorGateReturns) {
        if (caller == static_cast<uintptr_t>(_addr(gate)))
            return ChrColors_Available(characterId, color);
    }
    return false;
}

} // namespace

bool ChrColors_Available(int32_t characterId, int32_t color)
{
    if (characterId < kFirstCharacter || characterId > kLastCharacter) return false;
    if (color < 0 || color >= VariantCount(characterId)) return false;
    return ArchiveInstalled(characterId, color);
}

int ChrColors_Count(int32_t characterId)
{
    int count = 0;
    while (count < kChrColorSlots && ChrColors_Available(characterId, count)) ++count;
    return count;
}

const char* ChrColors_Label(int32_t color)
{
    static const char* const labels[kChrColorSlots] = {"1", "2", "3", "4", "5", "6", "H", "ALT"};
    return color >= 0 && color < kChrColorSlots ? labels[color] : "?";
}

void ChrColors_InstallUnlock()
{
    static bool installed = false;
    if (installed) return;
    installed = true;

    void* target = reinterpret_cast<void*>(_addr(SECRET_GET_OWNED_BIT));
    // With it, every installed character color is selectable.
    if (!(MH_CreateHook(target, reinterpret_cast<void*>(Hook_getOwnedBit),
                        reinterpret_cast<void**>(&oGetOwnedBit)) == MH_OK &&
          MH_EnableHook(target) == MH_OK)) {
        HookLog("ChrColors: FAILED to hook the color ownership test\n");
    }
}
