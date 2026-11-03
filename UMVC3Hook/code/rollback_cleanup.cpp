#include "../pch.h"

#include "rollback_cleanup.h"
#include "rollback_memory.h"
#include "mk10utils.h"

#include <vector>

extern void HookLog(const char* fmt, ...);

static constexpr uintptr_t PTR_SCHARACTER = 0x140d44a70;
static constexpr uintptr_t PTR_SGAMEUI = 0x140d53a18;
static constexpr uintptr_t PTR_SGAMEEFFECT = 0x140d46470;
static constexpr uintptr_t PTR_SMVC3MGR = 0x140d510a0;
static constexpr uintptr_t PTR_SUNIT = 0x140e17698;
static constexpr uintptr_t FN_GAMEUI_SET_TEAM_CHARACTER_ID = 0x140022610;
static constexpr uintptr_t FN_MODEL_SET_CHILD_MATERIAL_MODE = 0x140200DB0;
static constexpr uintptr_t FN_MODEL_UPDATE_CAMERA_VISIBILITY_MASK = 0x1405641A0;
static constexpr uintptr_t DTI_DR_STRANGE = 0x140D450A8;
static constexpr uintptr_t DTI_UCUTIN = 0x140D6BCC8;
static constexpr uintptr_t TEAM0_OFFSET = 0x40;
static constexpr uintptr_t TEAM1_OFFSET = 0x300;
static constexpr uintptr_t SGAMEUI_TEAM_CHARACTER_ID_OFFSET = 0x5960;
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
static constexpr uint32_t SUNIT_CATEGORY_COUNT_OFFSET = 0xC38;
static constexpr uint32_t SUNIT_CATEGORY_LISTS_OFFSET = 0x50;
static constexpr uint32_t SUNIT_CATEGORY_LIST_STRIDE = 0x30;
static constexpr uintptr_t MT_DTI_VTABLE = 0x140A69ED8;


static bool HasVirtualMethod(uint64_t object, size_t offset)
{
    if (!object) return false;
    uint64_t vtable = *(uint64_t*)object;
    return vtable && *(uint64_t*)(vtable + offset) != 0;
}

static void ClearModelMaterialCache(uint64_t model)
{
    if (!model) return;

    uint64_t entries = *(uint64_t*)(model + 0x110);
    uint32_t count = *(uint32_t*)(model + 0x118);
    if (!entries || count > 4096) return;

    for (uint32_t i = 0; i < count; ++i) {
        uint64_t entry = entries + (uint64_t)i * sizeof(uint64_t);
        uint64_t material = *(uint64_t*)entry;
        if (material && !HasVirtualMethod(material, 0x38))
            *(uint64_t*)entry = 0;
    }
}

static void RemoveMtDtiUnits()
{
    uint64_t sUnit = *(uint64_t*)_addr(PTR_SUNIT);
    if (!sUnit) return;

    uint32_t categoryCount = *(uint32_t*)(sUnit + SUNIT_CATEGORY_COUNT_OFFSET);
    if (categoryCount > 64) categoryCount = 64;
    for (uint32_t category = 0; category < categoryCount; ++category) {
        uint64_t* headSlot = (uint64_t*)(sUnit + SUNIT_CATEGORY_LISTS_OFFSET +
            category * SUNIT_CATEGORY_LIST_STRIDE);
        uint64_t previous = 0;
        uint64_t current = *headSlot;
        for (int safety = 256; current && safety-- > 0;) {
            uint64_t next = *(uint64_t*)(current + 0x18);
            uint64_t vtable = *(uint64_t*)current;
            if (vtable == _addr(MT_DTI_VTABLE)) {
                HookLog("Rollback: removed MtDTI object from sUnit category=%u "
                        "object=0x%llX\n", category, current);
                if (previous)
                    *(uint64_t*)(previous + 0x18) = next;
                else
                    *headSlot = next;
            } else {
                previous = current;
            }
            current = next;
        }
    }
}

void Rollback_RemoveStaleUiUnits()
{
    // No-op: restart owns scheduler teardown; do not modify sUnit lists here.
}

void Rollback_ClearStaleMaterialPointers()
{
    uint64_t sCharacter = *(uint64_t*)_addr(PTR_SCHARACTER);
    if (!sCharacter) return;

    for (int teamIndex = 0; teamIndex < 2; ++teamIndex) {
        uint64_t team = sCharacter + (teamIndex == 0 ? TEAM0_OFFSET : TEAM1_OFFSET);
        uint64_t node = team + TEAM_FIRST_NODE_OFFSET;
        while (node) {
            uint64_t character = *(uint64_t*)(node + NODE_CHR_OFFSET);
            if (character) {
                ClearModelMaterialCache(character);
                uint32_t weaponCount = *(uint32_t*)(character + 0x3F38);
                uint64_t weapons = *(uint64_t*)(character + 0x3F40);
                if (weaponCount <= 256 && weapons) {
                    for (uint32_t i = 0; i < weaponCount; ++i) {
                        uint64_t slot = weapons + (uint64_t)i * sizeof(uint64_t);
                        ClearModelMaterialCache(*(uint64_t*)slot);
                    }
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

        int teamId = *(int*)(active + 0x2DA0);
        int memberId = *(int*)(active + 0x2DB0);
        int* cachedMemberId = (int*)(sGameUi +
            SGAMEUI_TEAM_CHARACTER_ID_OFFSET + (uintptr_t)teamId * sizeof(int));
        if (*cachedMemberId != memberId)
            setTeamCharacterId(sGameUi, teamId, memberId);
    }
}

void Rollback_SyncActiveCharacterVisibility()
{
    uint64_t sCharacter = *(uint64_t*)_addr(PTR_SCHARACTER);
    if (!sCharacter) return;

    uint64_t manager = *(uint64_t*)_addr(PTR_SMVC3MGR);
    uint64_t cutin = manager ? *(uint64_t*)(manager + SMVC3MGR_CUTIN_OFFSET) : 0;
    bool validCutin = cutin && HasDtiVirtual((void*)cutin) &&
        GetObjectDti(cutin) == (uint64_t)_addr(DTI_UCUTIN) &&
        ((*(uint32_t*)(cutin + 0x10) & 7) - 1 < 2);

    auto isCutinParticipant = [cutin, validCutin](uint64_t character) {
        if (!validCutin) return false;
        uint32_t count = *(uint32_t*)(cutin + 0x48);
        uint64_t entries = *(uint64_t*)(cutin + 0x58);
        if (count > 16 || (count && !entries)) return false;
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
        if (!model) return;
        uint32_t& drawLayer = *(uint32_t*)(model + CUNIT_DRAW_LAYER_MASK_OFFSET);
        drawLayer = (drawLayer & ~CUTIN_DRAW_LAYER_BITS) | NORMAL_DRAW_LAYER_BIT;
        if (restoreVisibility) {
            *(uint32_t*)(model + 0x10) |= MTOBJECT_VISIBLE_FLAG;
            // Zero transparency hides the model even when its visible flag is set.
            *(float*)(model + 0x180) = 1.0f;
        }
        setChildMaterialMode((void*)model, -1, 3);
        updateCameraVisibilityMask((void*)model);
    };

    for (int teamIndex = 0; teamIndex < 2; ++teamIndex) {
        uint64_t team = sCharacter +
            (teamIndex == 0 ? TEAM0_OFFSET : TEAM1_OFFSET);
        uint64_t character = *(uint64_t*)(team + 0x48);
        if (!character) continue;

        // Preserve active cut-in participants; draw-layer bits alone may be stale.
        if (isCutinParticipant(character)) continue;

        restoreNormalModel(character, true);
        uint32_t weaponCount = *(uint32_t*)(character + CCHR_WEAPON_COUNT_OFFSET);
        uint64_t weapons = *(uint64_t*)(character + CCHR_WEAPON_ARRAY_OFFSET);
        if (!weapons || weaponCount > 256) {
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
    if (cutin && HasDtiVirtual((void*)cutin) &&
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
    auto restoreModel = [setChildMaterialMode](uint64_t model,
                                                bool restoreVisibility) {
        if (!model) return;
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
            // Skip the character and its weapons when no cut-in layer bits remain.
            if ((*(uint32_t*)(character + CUNIT_DRAW_LAYER_MASK_OFFSET) &
                 CUTIN_DRAW_LAYER_BITS) == 0) {
                continue;
            }
            restoreModel(character, true);

            uint32_t weaponCount = *(uint32_t*)(character + CCHR_WEAPON_COUNT_OFFSET);
            uint64_t weapons = *(uint64_t*)(character + CCHR_WEAPON_ARRAY_OFFSET);
            if (!weapons || weaponCount > 256) {
                continue;
            }
            for (uint32_t weapon = 0; weapon < weaponCount; ++weapon)
                restoreModel(*(uint64_t*)(weapons + (uint64_t)weapon * sizeof(uint64_t)),
                              false);
        }
    }
}
