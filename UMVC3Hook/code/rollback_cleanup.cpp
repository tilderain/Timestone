#include "../pch.h"

#include "rollback_cleanup.h"
#include "mk10utils.h"

static constexpr uintptr_t PTR_SCHARACTER = 0x140d44a70;
static constexpr uintptr_t PTR_SGAMEUI = 0x140d53a18;
static constexpr uintptr_t PTR_SGAMEEFFECT = 0x140d46470;
static constexpr uintptr_t FN_GAMEUI_SET_TEAM_CHARACTER_ID = 0x140022610;
static constexpr uintptr_t TEAM0_OFFSET = 0x40;
static constexpr uintptr_t TEAM1_OFFSET = 0x300;
static constexpr uintptr_t NODE_CHR_OFFSET = 0x08;
static constexpr uintptr_t NODE_NEXT_OFFSET = 0x10;
static constexpr uintptr_t TEAM_FIRST_NODE_OFFSET = 0x08;

static bool IsAccessibleAddress(uint64_t value)
{
    if (value < 0x10000 || value >= 0x0000800000000000ull) return false;
    MEMORY_BASIC_INFORMATION mbi = {};
    if (VirtualQuery((void*)value, &mbi, sizeof(mbi)) == 0) return false;
    if (mbi.State != MEM_COMMIT) return false;
    return (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) == 0;
}

static bool IsExecutableAddress(uint64_t value)
{
    if (!IsAccessibleAddress(value)) return false;
    MEMORY_BASIC_INFORMATION mbi = {};
    if (VirtualQuery((void*)value, &mbi, sizeof(mbi)) == 0) return false;
    const DWORD executable = PAGE_EXECUTE | PAGE_EXECUTE_READ |
        PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    return (mbi.Protect & executable) != 0;
}

static bool HasCallableDtiVirtual(uint64_t object)
{
    if (!IsAccessibleAddress(object)) return false;
    uint64_t vtable = *(uint64_t*)object;
    if (!IsAccessibleAddress(vtable) || !IsAccessibleAddress(vtable + 0x20))
        return false;
    return IsExecutableAddress(*(uint64_t*)(vtable + 0x20));
}

static void ClearModelMaterialCache(uint64_t model)
{
    if (!HasCallableDtiVirtual(model)) return;

    uint64_t entries = *(uint64_t*)(model + 0x110);
    uint32_t count = *(uint32_t*)(model + 0x118);
    if (!entries || count > 4096 || !IsAccessibleAddress(entries)) return;

    for (uint32_t i = 0; i < count; ++i) {
        uint64_t entry = entries + (uint64_t)i * sizeof(uint64_t);
        if (!IsAccessibleAddress(entry)) break;
        uint64_t material = *(uint64_t*)entry;
        if (material && !HasCallableDtiVirtual(material))
            *(uint64_t*)entry = 0;
    }
}

void Rollback_ClearStalePointers()
{
    uint64_t sCharacter = *(uint64_t*)_addr(PTR_SCHARACTER);
    if (!sCharacter) return;

    static constexpr uintptr_t SLOT_ARRAY_OFFSET = 0x6748;
    static constexpr uint32_t STALE_SLOT_INDICES[] = {1, 14, 15};

    for (int teamIndex = 0; teamIndex < 2; ++teamIndex) {
        uint64_t team = sCharacter + (teamIndex == 0 ? TEAM0_OFFSET : TEAM1_OFFSET);
        uint64_t node = team + TEAM_FIRST_NODE_OFFSET;
        while (node) {
            uint64_t character = *(uint64_t*)(node + NODE_CHR_OFFSET);
            if (character) {
                ClearModelMaterialCache(character);

                uint32_t weaponCount = *(uint32_t*)(character + 0x3F38);
                uint64_t weapons = *(uint64_t*)(character + 0x3F40);
                if (weaponCount <= 256 && weapons && IsAccessibleAddress(weapons)) {
                    for (uint32_t i = 0; i < weaponCount; ++i) {
                        uint64_t slot = weapons + (uint64_t)i * sizeof(uint64_t);
                        if (!IsAccessibleAddress(slot)) break;
                        ClearModelMaterialCache(*(uint64_t*)slot);
                    }
                }

                for (uint32_t index : STALE_SLOT_INDICES) {
                    uint64_t slot = character + SLOT_ARRAY_OFFSET +
                        (uint64_t)index * sizeof(uint64_t);
                    if (IsAccessibleAddress(slot))
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
