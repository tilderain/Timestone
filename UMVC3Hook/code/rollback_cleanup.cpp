#include "../pch.h"

#include "rollback_cleanup.h"
#include "rollback_memory.h"
#include "mk10utils.h"

#include <vector>

static constexpr uintptr_t PTR_SCHARACTER = 0x140d44a70;
static constexpr uintptr_t PTR_SGAMEUI = 0x140d53a18;
static constexpr uintptr_t PTR_SGAMEEFFECT = 0x140d46470;
static constexpr uintptr_t PTR_SMVC3MGR = 0x140d510a0;
static constexpr uintptr_t FN_GAMEUI_SET_TEAM_CHARACTER_ID = 0x140022610;
static constexpr uintptr_t FN_MODEL_SET_CHILD_MATERIAL_MODE = 0x140200DB0;
static constexpr uintptr_t FN_MODEL_UPDATE_CAMERA_VISIBILITY_MASK = 0x1405641A0;
static constexpr uintptr_t DTI_UCUTIN = 0x140D6BCC8;
static constexpr uintptr_t TEAM0_OFFSET = 0x40;
static constexpr uintptr_t TEAM1_OFFSET = 0x300;
static constexpr uintptr_t NODE_CHR_OFFSET = 0x08;
static constexpr uintptr_t NODE_NEXT_OFFSET = 0x10;
static constexpr uintptr_t TEAM_FIRST_NODE_OFFSET = 0x08;
static constexpr uintptr_t SMVC3MGR_CUTIN_OFFSET = 0xB50;
static constexpr uintptr_t CUNIT_DRAW_LAYER_MASK_OFFSET = 0x11C;
static constexpr uintptr_t CCHR_WEAPON_COUNT_OFFSET = 0x3F38;
static constexpr uintptr_t CCHR_WEAPON_ARRAY_OFFSET = 0x3F40;
static constexpr uint32_t CUTIN_DRAW_LAYER_BITS = 0x00700000;
static constexpr uint32_t NORMAL_DRAW_LAYER_BIT = 0x00000001;
static constexpr uint32_t MTOBJECT_VISIBLE_FLAG = 0x00004000;

class MemoryRegionCache {
public:
    bool IsAccessible(uint64_t address)
    {
        const Region* region = Get(address);
        return region && region->accessible;
    }

    bool IsExecutable(uint64_t address)
    {
        const Region* region = Get(address);
        return region && region->executable;
    }

private:
    struct Region {
        uint64_t start;
        uint64_t end;
        bool accessible;
        bool executable;
    };

    const Region* Get(uint64_t address)
    {
        if (address < 0x10000 || address >= 0x0000800000000000ull)
            return nullptr;
        for (const Region& region : regions_) {
            if (address >= region.start && address < region.end)
                return &region;
        }

        MEMORY_BASIC_INFORMATION mbi = {};
        if (VirtualQuery((void*)address, &mbi, sizeof(mbi)) == 0)
            return nullptr;
        const DWORD executable = PAGE_EXECUTE | PAGE_EXECUTE_READ |
            PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
        bool accessible = mbi.State == MEM_COMMIT &&
            (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) == 0;
        regions_.push_back({
            (uint64_t)mbi.BaseAddress,
            (uint64_t)mbi.BaseAddress + mbi.RegionSize,
            accessible,
            accessible && (mbi.Protect & executable) != 0,
        });
        return &regions_.back();
    }

    std::vector<Region> regions_;
};

static bool HasCallableDtiVirtual(uint64_t object, MemoryRegionCache& memory)
{
    if (!memory.IsAccessible(object)) return false;
    uint64_t vtable = *(uint64_t*)object;
    if (!memory.IsAccessible(vtable) || !memory.IsAccessible(vtable + 0x20))
        return false;
    return memory.IsExecutable(*(uint64_t*)(vtable + 0x20));
}

static void ClearModelMaterialCache(uint64_t model, MemoryRegionCache& memory)
{
    if (!HasCallableDtiVirtual(model, memory)) return;

    uint64_t entries = *(uint64_t*)(model + 0x110);
    uint32_t count = *(uint32_t*)(model + 0x118);
    if (!entries || count > 4096 || !memory.IsAccessible(entries)) return;

    for (uint32_t i = 0; i < count; ++i) {
        uint64_t entry = entries + (uint64_t)i * sizeof(uint64_t);
        if (!memory.IsAccessible(entry)) break;
        uint64_t material = *(uint64_t*)entry;
        if (material && !HasCallableDtiVirtual(material, memory))
            *(uint64_t*)entry = 0;
    }
}

void Rollback_ClearStalePointers()
{
    uint64_t sCharacter = *(uint64_t*)_addr(PTR_SCHARACTER);
    if (!sCharacter) return;

    static constexpr uintptr_t SLOT_ARRAY_OFFSET = 0x6748;
    static constexpr uint32_t SLOT_COUNT = 21;
    static_assert(SLOT_ARRAY_OFFSET + SLOT_COUNT * sizeof(uint64_t) <= 0x67F0);
    static constexpr uint32_t STALE_SLOT_INDICES[] = {1, 14, 15};
    MemoryRegionCache memory;

    for (int teamIndex = 0; teamIndex < 2; ++teamIndex) {
        uint64_t team = sCharacter + (teamIndex == 0 ? TEAM0_OFFSET : TEAM1_OFFSET);
        uint64_t node = team + TEAM_FIRST_NODE_OFFSET;
        while (node) {
            uint64_t character = *(uint64_t*)(node + NODE_CHR_OFFSET);
            if (character) {
                ClearModelMaterialCache(character, memory);

                uint32_t weaponCount = *(uint32_t*)(character + 0x3F38);
                uint64_t weapons = *(uint64_t*)(character + 0x3F40);
                if (weaponCount <= 256 && weapons && memory.IsAccessible(weapons)) {
                    for (uint32_t i = 0; i < weaponCount; ++i) {
                        uint64_t slot = weapons + (uint64_t)i * sizeof(uint64_t);
                        if (!memory.IsAccessible(slot)) break;
                        ClearModelMaterialCache(*(uint64_t*)slot, memory);
                    }
                }

                // Effect retirement can leave any indexed slot pointing at an
                // allocation whose vtable has already been overwritten.
                for (uint32_t index = 0; index < SLOT_COUNT; ++index) {
                    uint64_t slot = character + SLOT_ARRAY_OFFSET +
                        (uint64_t)index * sizeof(uint64_t);
                    if (!memory.IsAccessible(slot)) break;
                    uint64_t object = *(uint64_t*)slot;
                    if (object && !HasCallableDtiVirtual(object, memory))
                        *(uint64_t*)slot = 0;
                }

                for (uint32_t index : STALE_SLOT_INDICES) {
                    uint64_t slot = character + SLOT_ARRAY_OFFSET +
                        (uint64_t)index * sizeof(uint64_t);
                    if (memory.IsAccessible(slot))
                        *(uint64_t*)slot = 0;
                }
            }
            node = *(uint64_t*)(node + NODE_NEXT_OFFSET);
        }
    }
}

void Rollback_SyncActiveCharacterUi()
{
    uint64_t sCharacter = *(uint64_t*)_addr(PTR_SCHARACTER);
    uint64_t sGameUi = *(uint64_t*)_addr(PTR_SGAMEUI);
    if (!sCharacter || !sGameUi) return;

    using SetTeamCharacterId = void(__fastcall*)(uint64_t, int, int);
    auto setTeamCharacterId = (SetTeamCharacterId)_addr(FN_GAMEUI_SET_TEAM_CHARACTER_ID);
    for (int teamIndex = 0; teamIndex < 2; ++teamIndex) {
        uint64_t team = sCharacter + (teamIndex == 0 ? TEAM0_OFFSET : TEAM1_OFFSET);
        uint64_t active = *(uint64_t*)(team + 0x48);
        if (!active) continue;

        int playerId = *(int*)(active + 0x2DA0);
        int memberId = *(int*)(active + 0x2DB0);
        setTeamCharacterId(sGameUi, playerId, memberId);
    }
}

void Rollback_SyncActiveCharacterVisibility()
{
    uint64_t sCharacter = *(uint64_t*)_addr(PTR_SCHARACTER);
    if (!sCharacter) return;

    uint64_t manager = *(uint64_t*)_addr(PTR_SMVC3MGR);
    uint64_t cutin = manager ? *(uint64_t*)(manager + SMVC3MGR_CUTIN_OFFSET) : 0;
    bool validCutin = cutin && HasCallableDtiVirtual((void*)cutin) &&
        GetObjectDti(cutin) == (uint64_t)_addr(DTI_UCUTIN) &&
        ((*(uint32_t*)(cutin + 0x10) & 7) - 1 < 2);

    auto isCutinParticipant = [cutin, validCutin](uint64_t character) {
        if (!validCutin) return false;
        uint32_t count = *(uint32_t*)(cutin + 0x48);
        uint64_t entries = *(uint64_t*)(cutin + 0x58);
        if (count > 16 || (count && !IsAccessibleRange(
                entries, (size_t)count * sizeof(uint64_t)))) {
            return false;
        }
        for (uint32_t i = 0; i < count; ++i) {
            if (*(uint64_t*)(entries + (uint64_t)i * sizeof(uint64_t)) == character)
                return true;
        }
        return false;
    };

    using SetChildMaterialModeFn = void(__fastcall*)(void*, int, uint8_t);
    auto setChildMaterialMode =
        (SetChildMaterialModeFn)_addr(FN_MODEL_SET_CHILD_MATERIAL_MODE);
    using UpdateCameraVisibilityMaskFn = void(__fastcall*)(void*);
    auto updateCameraVisibilityMask =
        (UpdateCameraVisibilityMaskFn)_addr(FN_MODEL_UPDATE_CAMERA_VISIBILITY_MASK);
    auto restoreNormalModel = [setChildMaterialMode, updateCameraVisibilityMask](
                                  uint64_t model, bool restoreVisibility) {
        if (!model || !HasCallableDtiVirtual((void*)model)) return;
        uint32_t& drawLayer = *(uint32_t*)(model + CUNIT_DRAW_LAYER_MASK_OFFSET);
        drawLayer = (drawLayer & ~CUTIN_DRAW_LAYER_BITS) | NORMAL_DRAW_LAYER_BIT;
        if (restoreVisibility) {
            *(uint32_t*)(model + 0x10) |= MTOBJECT_VISIBLE_FLAG;
            // uModel::submitForRender has a separate hard gate on
            // mTransparency. A restored bench/cutin value of zero bypasses
            // rendering even when the MtObject draw flag is set.
            *(float*)(model + 0x180) = 1.0f;
        }
        setChildMaterialMode((void*)model, -1, 3);
        updateCameraVisibilityMask((void*)model);
    };

    for (int teamIndex = 0; teamIndex < 2; ++teamIndex) {
        uint64_t team = sCharacter +
            (teamIndex == 0 ? TEAM0_OFFSET : TEAM1_OFFSET);
        uint64_t character = *(uint64_t*)(team + 0x48);
        if (!character || !HasCallableDtiVirtual((void*)character)) continue;

        // Draw-layer bits can themselves be stale, so only a live uCutIn that
        // actually lists this character may keep the active point hidden.
        if (isCutinParticipant(character)) continue;

        restoreNormalModel(character, true);
        uint32_t weaponCount = *(uint32_t*)(character + CCHR_WEAPON_COUNT_OFFSET);
        uint64_t weapons = *(uint64_t*)(character + CCHR_WEAPON_ARRAY_OFFSET);
        if (!weapons || weaponCount > 256 || !IsAccessibleRange(
                weapons, (size_t)weaponCount * sizeof(uint64_t))) {
            continue;
        }
        for (uint32_t weapon = 0; weapon < weaponCount; ++weapon) {
            restoreNormalModel(
                *(uint64_t*)(weapons + (uint64_t)weapon * sizeof(uint64_t)),
                false);
        }
    }
}

void Rollback_RepairOrphanedCutinRenderState()
{
    uint64_t manager = *(uint64_t*)_addr(PTR_SMVC3MGR);
    uint64_t cutin = manager ? *(uint64_t*)(manager + SMVC3MGR_CUTIN_OFFSET) : 0;
    if (cutin && HasCallableDtiVirtual((void*)cutin) &&
        GetObjectDti(cutin) == (uint64_t)_addr(DTI_UCUTIN) &&
        ((*(uint32_t*)(cutin + 0x10) & 7) - 1 < 2)) {
        return;
    }
    if (manager && cutin)
        *(uint64_t*)(manager + SMVC3MGR_CUTIN_OFFSET) = 0;

    uint64_t sCharacter = *(uint64_t*)_addr(PTR_SCHARACTER);
    if (!sCharacter) return;

    using SetChildMaterialModeFn = void(__fastcall*)(void*, int, uint8_t);
    auto setChildMaterialMode =
        (SetChildMaterialModeFn)_addr(FN_MODEL_SET_CHILD_MATERIAL_MODE);
    auto restoreModel = [setChildMaterialMode](uint64_t model, bool restoreVisibility,
                                                bool validateUnit) {
        if (!model || (validateUnit && !IsUnitObject((void*)model))) return;
        uint32_t& drawLayer = *(uint32_t*)(model + CUNIT_DRAW_LAYER_MASK_OFFSET);
        if ((drawLayer & CUTIN_DRAW_LAYER_BITS) == 0) return;
        drawLayer = (drawLayer & ~CUTIN_DRAW_LAYER_BITS) | NORMAL_DRAW_LAYER_BIT;
        if (restoreVisibility)
            *(uint32_t*)(model + 0x10) |= MTOBJECT_VISIBLE_FLAG;
        setChildMaterialMode((void*)model, -1, 3);
    };

    for (int teamIndex = 0; teamIndex < 2; ++teamIndex) {
        uint64_t team = sCharacter + (teamIndex == 0 ? TEAM0_OFFSET : TEAM1_OFFSET);
        for (uint64_t node = team + TEAM_FIRST_NODE_OFFSET; node;
             node = *(uint64_t*)(node + NODE_NEXT_OFFSET)) {
            uint64_t character = *(uint64_t*)(node + NODE_CHR_OFFSET);
            if (!character) continue;
            // Native cut-in entry routes the character and all its weapons in
            // one operation. Checking the trusted roster character first keeps
            // the ordinary post-rollback path free of RTTI and VirtualQuery.
            if ((*(uint32_t*)(character + CUNIT_DRAW_LAYER_MASK_OFFSET) &
                 CUTIN_DRAW_LAYER_BITS) == 0) {
                continue;
            }
            restoreModel(character, true, false);

            uint32_t weaponCount = *(uint32_t*)(character + CCHR_WEAPON_COUNT_OFFSET);
            uint64_t weapons = *(uint64_t*)(character + CCHR_WEAPON_ARRAY_OFFSET);
            if (!weapons || weaponCount > 256 ||
                !IsAccessibleRange(weapons, (size_t)weaponCount * sizeof(uint64_t))) {
                continue;
            }
            for (uint32_t weapon = 0; weapon < weaponCount; ++weapon)
                restoreModel(*(uint64_t*)(weapons + (uint64_t)weapon * sizeof(uint64_t)),
                             false, true);
        }
    }
}

void Rollback_ClearGameEffectSlots()
{
    uint64_t sGameEffect = *(uint64_t*)_addr(PTR_SGAMEEFFECT);
    if (!sGameEffect) return;

    static constexpr size_t GENERIC_SLOTS_OFFSET = 0x40;
    static constexpr size_t GENERIC_SLOTS_SIZE = 10 * sizeof(uint64_t);
    static constexpr size_t CHARACTER_SLOTS_OFFSET = 0x90;
    static constexpr size_t CHARACTER_SLOTS_SIZE = 6 * 0x18;
    memset((void*)(sGameEffect + GENERIC_SLOTS_OFFSET), 0, GENERIC_SLOTS_SIZE);
    memset((void*)(sGameEffect + CHARACTER_SLOTS_OFFSET), 0, CHARACTER_SLOTS_SIZE);
}
