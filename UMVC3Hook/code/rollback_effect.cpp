#include "../pch.h"

#include "rollback_effect.h"

#include "mk10utils.h"
#include "rollback.h"
#include "rollback_memory.h"
#include "rollback_profile.h"
#include "rollback_snapshot.h"
#include "../kiero/minhook/include/MinHook.h"

#include <algorithm>
#include <cstring>
#include <vector>

extern void HookLog(const char* fmt, ...);

static_assert(sizeof(RollbackRegion) <= 64);

static constexpr uintptr_t PTR_SMVC3MGR = 0x140d510a0;
static constexpr uintptr_t PTR_SSTAGE = 0x140d4f718;
static constexpr uintptr_t PTR_SCHARACTER = 0x140d44a70;
static constexpr uintptr_t FN_SGAMEEFFECT_FLUSH_PENDING = 0x140130D00;
static constexpr uintptr_t FN_UGAMEEFFECT_CREATE = 0x140131410;
static constexpr uintptr_t FN_DRSTRANGE_UPDATE_SUMMONS = 0x14007B9E0;
static constexpr uintptr_t FN_EFFECT_DRAW = 0x1405E4E70;
static constexpr uintptr_t FN_EFFECT_MOVE_AFTER = 0x1405E57A0;
static constexpr uintptr_t FN_EFFECT_DTOR_RELEASE = 0x1405E07F0;
static constexpr uintptr_t FN_EFFECT_VDTOR = 0x1405E08C0;
static constexpr uintptr_t FN_PARTICLE_MANAGER_RELEASE_SUBBUFFER = 0x14080C120;
static constexpr uintptr_t FN_CATKCTRL_CREATE_PROPERTY = 0x14000C810;
static constexpr uintptr_t FN_KIRIGAKURE_EFFECT_UPDATE = 0x14013B050;
static constexpr uintptr_t SMVC3MGR_ACTION_OFFSET = 0x48;
static constexpr uint32_t SUNIT_CATEGORY_COUNT_OFFSET = 0xC38;
static constexpr uint32_t SUNIT_CATEGORY_LISTS_OFFSET = 0x50;
static constexpr uint32_t SUNIT_CATEGORY_LIST_STRIDE = 0x30;
static constexpr uint32_t SSTAGE_EFFECT_OFFSETS[] = {0x2050, 0x2058};
static constexpr uint32_t PARTICLE_MANAGER_STRIDE = 0x150;
static constexpr uint32_t PARTICLE_MANAGER_BUFFER_OFFSET = 0x130;
static constexpr uint32_t MAX_PARTICLE_MANAGERS = 256;
static constexpr uint32_t TEAM_OFFSETS[] = {0x40, 0x300};
static constexpr uint32_t CHARACTER_EFFECT_SLOT_OFFSETS[] = {
    0x68F0, 0x68F8, 0x6900, 0x6920,
};

using EffectCreateFn = void*(__fastcall*)(void*, int, int, void*, int, int,
    void*, void*, int, void*, int, uint32_t, void*);

static constexpr int MAX_GENERATOR_NODES = 256;

static EffectCreateFn oEffectCreate = nullptr;
static void(__fastcall* oDrStrangeUpdateSummons)(void*) = nullptr;
static void(__fastcall* oEffectDraw)(void*, void*) = nullptr;
static void(__fastcall* oEffectMoveAfter)(void*) = nullptr;
static void(__fastcall* oEffectDtorRelease)(void*) = nullptr;
static void(__fastcall* oEffectVdtor)(void*, uint32_t) = nullptr;
static void(__fastcall* oParticleManagerReleaseSubBuffer)(void*) = nullptr;
static void(__fastcall* oGameEffectFlushPending)(void*) = nullptr;
static void(__fastcall* oKirigakureEffectUpdate)(void*) = nullptr;
static std::vector<RollbackEffectRecipe> g_liveEffectRecipes;
static uint64_t g_effectSpawnSequence = 0;

static bool IsActiveUnit(void* unit)
{
    if (!unit) return false;
    uint32_t state = *(uint32_t*)((uint8_t*)unit + 0x10);
    return (state & 7u) < 3u && (state & 0x3F8u) != 0x3F8u;
}

static bool IsPersistentCharacterEffect(void* effect)
{
    uint64_t sCharacter = *(uint64_t*)_addr(PTR_SCHARACTER);
    if (!sCharacter || !effect) return false;

    for (uint32_t teamOffset : TEAM_OFFSETS) {
        uint64_t node = sCharacter + teamOffset + 0x08;
        for (int safety = 8; node && safety-- > 0;) {
            if (!IsAccessibleRange(node, 0x18)) break;
            uint64_t character = *(uint64_t*)(node + 0x08);
            if (character && IsAccessibleRange(character, 0x6928)) {
                // Character effect factories establish native ownership with
                // MtObject__setField_640(effect, character). This also covers
                // hyper effects retained in the character's indexed slots,
                // which are not represented by the fixed tail handles below.
                if (IsAccessibleRange((uint64_t)effect, 0x288) &&
                    *(void**)((uint8_t*)effect + 0x280) == (void*)character) {
                    return true;
                }
                for (uint32_t slotOffset : CHARACTER_EFFECT_SLOT_OFFSETS) {
                    if (*(void**)(character + slotOffset) == effect)
                        return true;
                }
            }
            node = *(uint64_t*)(node + 0x10);
        }
    }
    return false;
}

static void DetachEffectOwnedParticleState(void* effect)
{
    const uint32_t count = *(uint16_t*)((uint8_t*)effect + 0x208);
    const uint64_t managers = *(uint64_t*)((uint8_t*)effect + 0x220);
    bool validArray = managers && count <= MAX_PARTICLE_MANAGERS &&
        IsAccessibleRange(managers, count == 0 ? 1 :
                          (size_t)count * PARTICLE_MANAGER_STRIDE);
    for (uint32_t index = 0; validArray && index < count; ++index) {
        uint8_t* manager = (uint8_t*)managers +
            (size_t)index * PARTICLE_MANAGER_STRIDE;
        uint64_t vtable = *(uint64_t*)manager;
        validArray = IsAccessibleRange(vtable, 0x28) &&
            IsExecutableAddress(*(uint64_t*)(vtable + 0x20));
    }
    if (validArray) {
        for (uint32_t index = 0; index < count; ++index) {
            uint8_t* manager = (uint8_t*)managers +
                (size_t)index * PARTICLE_MANAGER_STRIDE;
            // Keep the manager array owned by native destruction, but detach
            // every buffer so releaseSubBuffer cannot return stale ownership
            // to the allocator. This avoids leaking the array itself.
            std::memset(manager + PARTICLE_MANAGER_BUFFER_OFFSET, 0, 0x18);
        }
        *(uint64_t*)((uint8_t*)effect + 0x218) = 0;
        *(uint16_t*)((uint8_t*)effect + 0x208) = 0;
        *(uint64_t*)((uint8_t*)effect + 0x228) = 0;
        return;
    }

    *(uint64_t*)((uint8_t*)effect + 0x218) = 0;
    *(uint16_t*)((uint8_t*)effect + 0x208) = 0;
    *(uint64_t*)((uint8_t*)effect + 0x220) = 0;
    *(uint64_t*)((uint8_t*)effect + 0x228) = 0;
}

static bool HasInvalidParticleManagerState(void* effect)
{
    if (!IsAccessibleRange((uint64_t)effect, 0x230)) return true;

    const uint32_t count = *(uint16_t*)((uint8_t*)effect + 0x208);
    const uint64_t managers = *(uint64_t*)((uint8_t*)effect + 0x220);
    if (!managers) return count != 0;
    if (count > MAX_PARTICLE_MANAGERS || !IsAccessibleRange(managers, 1))
        return true;
    if (count == 0) return false;
    if (!IsAccessibleRange(managers,
                           (size_t)count * PARTICLE_MANAGER_STRIDE)) {
        return true;
    }

    for (uint32_t index = 0; index < count; ++index) {
        uint8_t* manager = (uint8_t*)managers +
            (size_t)index * PARTICLE_MANAGER_STRIDE;
        uint64_t vtable = *(uint64_t*)manager;
        if (!IsAccessibleRange(vtable, 0x28) ||
            !IsExecutableAddress(*(uint64_t*)(vtable + 0x20))) {
            return true;
        }

        uint64_t buffer = *(uint64_t*)(manager + PARTICLE_MANAGER_BUFFER_OFFSET);
        if (buffer && !IsAccessiblePointer(buffer)) return true;
    }
    return false;
}

static void SanitizeEffectForDestruction(void* effect)
{
    if (!effect || !IsAccessibleRange((uint64_t)effect, 0x230)) return;
    if (g_activeRollbackProfile)
        ++g_activeRollbackProfile->restoreEffectSanitizeCalls;

    {
        RollbackHookProfileScope profile(RollbackHookProfileId::UnitRemoveEffectResource);
        uint64_t resource = *(uint64_t*)((uint8_t*)effect + 0x108);
        if (resource != 0 &&
            (!IsAccessibleAddress(resource + 0x4C) ||
             !HasCallableDtiVirtual((void*)resource))) {
            *(uint64_t*)((uint8_t*)effect + 0x108) = 0;
        }
    }

    {
        RollbackHookProfileScope profile(RollbackHookProfileId::UnitRemoveEffectGenerator);
        uint64_t generator = *(uint64_t*)((uint8_t*)effect + 0x218);
        uint64_t node = generator;
        uint64_t cycleCheckpoint = generator;
        uint32_t cyclePower = 1;
        uint32_t cycleLength = 0;
        bool validGeneratorChain = true;
        const uint64_t cAtkCtrlCreateProperty =
            (uint64_t)_addr(FN_CATKCTRL_CREATE_PROPERTY);
        for (int count = 0; node != 0 && count < MAX_GENERATOR_NODES; ++count) {
            if (g_activeRollbackProfile)
                ++g_activeRollbackProfile->restoreEffectGeneratorNodes;
            if (!IsAccessibleRange(node, 0x20)) {
                validGeneratorChain = false;
                break;
            }
            uint64_t vtable = *(uint64_t*)node;
            if (!IsAccessibleRange(vtable, 0x90) ||
                !IsExecutableAddress(*(uint64_t*)vtable) ||
                !IsExecutableAddress(*(uint64_t*)(vtable + 0x88)) ||
                *(uint64_t*)(vtable + 0x88) == cAtkCtrlCreateProperty) {
                validGeneratorChain = false;
                break;
            }
            uint64_t next = *(uint64_t*)(node + 0x18);
            ++cycleLength;
            if (next != 0 && next == cycleCheckpoint) {
                validGeneratorChain = false;
                break;
            }
            if (cycleLength == cyclePower) {
                cycleCheckpoint = next;
                cyclePower *= 2;
                cycleLength = 0;
            }
            node = next;
            if (count == MAX_GENERATOR_NODES - 1 && node != 0)
                validGeneratorChain = false;
        }
        if (!validGeneratorChain) {
            // A stale generator can leave a mapped but invalid particle-manager
            // allocation behind. Native destruction would pass its sub-buffer
            // pointers to the allocator and walk corrupted heap metadata.
            DetachEffectOwnedParticleState(effect);
        }
    }

    {
        RollbackHookProfileScope profile(RollbackHookProfileId::UnitRemoveEffectSubBuffers);
        if (HasInvalidParticleManagerState(effect))
            DetachEffectOwnedParticleState(effect);
    }
}

static void DetachExAtkSpaceChildren()
{
    uint64_t sMvc3Mgr = *(uint64_t*)_addr(PTR_SMVC3MGR);
    if (!sMvc3Mgr || !IsAccessibleAddress(sMvc3Mgr + SMVC3MGR_ACTION_OFFSET))
        return;
    uint64_t sAction = *(uint64_t*)(sMvc3Mgr + SMVC3MGR_ACTION_OFFSET);
    if (!sAction || !IsAccessibleAddress(sAction + 0x148)) return;
    uint64_t exAtkSpace = *(uint64_t*)(sAction + 0x148);
    if (!exAtkSpace || !HasCallableDtiVirtual((void*)exAtkSpace) ||
        !IsAccessibleAddress(exAtkSpace + 0x1D30)) return;

    std::memset((void*)(exAtkSpace + 0x1D18), 0, 4 * sizeof(uint64_t));
}

static void SanitizeStageEffectReferences(void* retiringEffect = nullptr)
{
    uint64_t sStage = *(uint64_t*)_addr(PTR_SSTAGE);
    if (!sStage || !IsAccessibleRange(sStage, 0x2060)) return;

    for (uint32_t offset : SSTAGE_EFFECT_OFFSETS) {
        void** slot = (void**)(sStage + offset);
        void* effect = *slot;
        if (!effect) continue;
        if (effect == retiringEffect || !IsEffectObject(effect) ||
            !IsActiveUnit(effect)) {
            *slot = nullptr;
        }
    }
}

static void* __fastcall hkEffectCreate(void* manager, int effectId, int a3,
                                       void* attachTo, int a5, int a6, void* a7,
                                       void* a8, int a9, void* a10, int a11,
                                       uint32_t a12, void* a13)
{
    RollbackHookProfileScope profile(RollbackHookProfileId::EffectCreate);
    void* effect = oEffectCreate(manager, effectId, a3, attachTo, a5, a6, a7,
                                 a8, a9, a10, a11, a12, a13);
    InvalidateMemoryQueryCache();
    if (effect) {
        RollbackEffectRecipe recipe = {
            effect, effect, manager, effectId, a3, attachTo, a5, a6, a7, a8,
            a9, a10, a11, a12, a13
        };
        recipe.spawnSequence = ++g_effectSpawnSequence;
        auto existing = std::find_if(g_liveEffectRecipes.begin(),
                                     g_liveEffectRecipes.end(),
                                     [effect](const RollbackEffectRecipe& live) {
                                         return live.restoredPtr == effect;
                                     });
        if (existing != g_liveEffectRecipes.end())
            *existing = recipe;
        else
            g_liveEffectRecipes.push_back(recipe);
    }
    return effect;
}

static void __fastcall hkDrStrangeUpdateSummons(void* character)
{
    // These summons are presentation-only and their allocations are not part
    // of rollback snapshots. Do not create or destroy them on a replayed frame.
    if (!g_isResimulating)
        oDrStrangeUpdateSummons(character);
}

static void __fastcall hkEffectDraw(void* effect, void* camera)
{
    if (!IsAccessibleRange((uint64_t)effect, 0x220))
        return;

    uint64_t resource = *(uint64_t*)((uint8_t*)effect + 0x108);
    if (resource && !HasCallableDtiVirtual((void*)resource))
        return;

    uint64_t generator = *(uint64_t*)((uint8_t*)effect + 0x218);
    if (generator && !HasCallableDtiVirtual((void*)generator))
        return;

    oEffectDraw(effect, camera);
}

static bool HasStaleGeneratorDispatch(void* effect)
{
    if (!IsAccessibleRange((uint64_t)effect, 0x220)) return true;

    const uint64_t cAtkCtrlCreateProperty =
        (uint64_t)_addr(FN_CATKCTRL_CREATE_PROPERTY);
    uint64_t node = *(uint64_t*)((uint8_t*)effect + 0x218);
    for (int count = 0; node != 0 && count < MAX_GENERATOR_NODES; ++count) {
        if (!IsAccessibleRange(node, 0x20)) return true;
        uint64_t vtable = *(uint64_t*)node;
        if (!IsAccessibleRange(vtable, 0x90) ||
            !IsExecutableAddress(*(uint64_t*)(vtable + 0x88)) ||
            *(uint64_t*)(vtable + 0x88) == cAtkCtrlCreateProperty) {
            return true;
        }
        node = *(uint64_t*)(node + 0x18);
    }
    return node != 0;
}

static void __fastcall hkEffectMoveAfter(void* effect)
{
    if (!g_isResimulating || !HasStaleGeneratorDispatch(effect)) {
        oEffectMoveAfter(effect);
        return;
    }
    if (!IsAccessibleRange((uint64_t)effect, 0x80)) return;

    // A recycled generator can dispatch cAtkCtrl::createProperty through the
    // particle callback slot. Mark the effect retired directly; invoking the
    // owner callback here can re-enter sUnit while its late pass is active.
    Effect_PrepareUnitRemoval(effect);
    *(uint16_t*)((uint8_t*)effect + 0x206) = 0;
    *(uint32_t*)((uint8_t*)effect + 0x100) &= ~0x20u;
    uint32_t state = *(uint32_t*)((uint8_t*)effect + 0x10);
    *(uint32_t*)((uint8_t*)effect + 0x10) = (state & ~7u) | 3u;
}

static void __fastcall hkEffectDtorRelease(void* effect)
{
    if (g_isResimulating)
        Effect_PrepareUnitRemoval(effect);
    oEffectDtorRelease(effect);
}

static void __fastcall hkEffectVdtor(void* effect, uint32_t flags)
{
    if (g_isResimulating)
        Effect_PrepareUnitRemoval(effect);
    oEffectVdtor(effect, flags);
}

static void __fastcall hkParticleManagerReleaseSubBuffer(void* manager)
{
    if (g_isResimulating && IsAccessibleRange(
            (uint64_t)manager, PARTICLE_MANAGER_STRIDE)) {
        uint64_t buffer = *(uint64_t*)((uint8_t*)manager +
                                      PARTICLE_MANAGER_BUFFER_OFFSET);
        if (buffer && !IsAccessiblePointer(buffer)) {
            // Native assumes this is an allocator-owned particle buffer. A
            // stale rollback graph can retain a readable manager with a dead
            // buffer pointer; skip only that invalid free and let the effect
            // destructor release the manager array itself.
            std::memset((uint8_t*)manager + PARTICLE_MANAGER_BUFFER_OFFSET,
                        0, 0x18);
            return;
        }
    }
    oParticleManagerReleaseSubBuffer(manager);
}

static void __fastcall hkGameEffectFlushPending(void* manager)
{
    // Native isAlive checks only state flags. A retired effect can therefore
    // survive in this queue with a zeroed vtable and crash at virtual +0x78.
    uint32_t count = *(uint32_t*)((uint8_t*)manager + 0x128);
    void** entries = *(void***)((uint8_t*)manager + 0x138);
    if (count <= 4096 && (!count || IsAccessibleRange(
            (uint64_t)entries, (size_t)count * sizeof(void*)))) {
        for (uint32_t i = 0; i < count; ++i) {
            void* effect = entries[i];
            if (!effect) continue;
            if (!IsAccessibleRange((uint64_t)effect, sizeof(uint64_t))) {
                entries[i] = nullptr;
                continue;
            }
            uint64_t vtable = *(uint64_t*)effect;
            if (!IsAccessibleAddress(vtable + 0x78) ||
                !IsExecutableAddress(*(uint64_t*)(vtable + 0x78))) {
                entries[i] = nullptr;
            }
        }
    }
    oGameEffectFlushPending(manager);
}

static void __fastcall hkKirigakureEffectUpdate(void* effect)
{
    RollbackHookProfileScope profile(RollbackHookProfileId::KirigakureEffectUpdate);
    uint64_t* attachment = (uint64_t*)((uint8_t*)effect + 0x40);
    if (*attachment && !HasCallableDtiVirtual((void*)*attachment))
        *attachment = 0;
    oKirigakureEffectUpdate(effect);
}

void Effect_CaptureRecipes(RollbackSnapshotSlot& slot)
{
    slot.effectRecipes.clear();
    slot.effectRecipes.reserve(g_liveEffectRecipes.size());
    for (const RollbackEffectRecipe& recipe : g_liveEffectRecipes) {
        if (!IsActiveUnit(recipe.restoredPtr)) continue;
        RollbackEffectRecipe saved = recipe;
        saved.savedPtr = recipe.restoredPtr;
        slot.effectRecipes.push_back(saved);
    }
}

void Effect_PrepareUnitRemoval(void* unit)
{
    bool isEffect = false;
    {
        RollbackHookProfileScope profile(RollbackHookProfileId::UnitRemoveEffectCheck);
        isEffect = IsEffectObject(unit);
    }
    if (isEffect) {
        RollbackHookProfileScope profile(RollbackHookProfileId::UnitRemoveEffectSanitize);
        SanitizeEffectForDestruction(unit);
    }
}

void Effect_FinishUnitRemoval(void* unit)
{
    g_liveEffectRecipes.erase(
        std::remove_if(g_liveEffectRecipes.begin(), g_liveEffectRecipes.end(),
                       [unit](const RollbackEffectRecipe& recipe) {
                           return recipe.restoredPtr == unit;
                       }),
        g_liveEffectRecipes.end());
}

int Effect_RetireCurrent(void* sUnit)
{
    DetachExAtkSpaceChildren();
    SanitizeStageEffectReferences();
    int retired = 0;
    uint32_t categoryCount = *(uint32_t*)((uint8_t*)sUnit + SUNIT_CATEGORY_COUNT_OFFSET);
    for (uint32_t category = 13; category <= 16 && category < categoryCount; ++category) {
        void* effect = *(void**)((uint8_t*)sUnit + SUNIT_CATEGORY_LISTS_OFFSET +
                                 SUNIT_CATEGORY_LIST_STRIDE * category);
        for (int safety = 4096; effect && safety-- > 0;) {
            if (g_activeRollbackProfile) ++g_activeRollbackProfile->restoreEffectVisits;
            void* next = *(void**)((uint8_t*)effect + 0x18);
            uint32_t* state = (uint32_t*)((uint8_t*)effect + 0x10);
            if (IsEffectObject(effect) && (*state & 7u) < 3u &&
                !IsPersistentCharacterEffect(effect)) {
                SanitizeStageEffectReferences(effect);
                *state = (*state & ~7u) | 3u;
                ++retired;
            }
            effect = next;
        }
    }
    return retired;
}

void Effect_SanitizeRetired(void* sUnit)
{
    uint32_t categoryCount = *(uint32_t*)((uint8_t*)sUnit + SUNIT_CATEGORY_COUNT_OFFSET);
    for (uint32_t category = 13; category <= 16 && category < categoryCount; ++category) {
        void* effect = *(void**)((uint8_t*)sUnit + SUNIT_CATEGORY_LISTS_OFFSET +
                                 SUNIT_CATEGORY_LIST_STRIDE * category);
        for (int safety = 4096; effect && safety-- > 0;) {
            if (g_activeRollbackProfile) ++g_activeRollbackProfile->restoreEffectVisits;
            void* next = *(void**)((uint8_t*)effect + 0x18);
            if (IsEffectObject(effect) && !IsPersistentCharacterEffect(effect))
                SanitizeEffectForDestruction(effect);
            effect = next;
        }
    }
}

void Effect_Reset()
{
    g_liveEffectRecipes.clear();
}

void Effect_InitHooks()
{
    MH_STATUS status = MH_CreateHook((LPVOID)_addr(FN_SGAMEEFFECT_FLUSH_PENDING),
                                     (LPVOID)&hkGameEffectFlushPending,
                                     (LPVOID*)&oGameEffectFlushPending);
    if (status == MH_OK) {
        MH_EnableHook((LPVOID)_addr(FN_SGAMEEFFECT_FLUSH_PENDING));
        HookLog("Rollback: guarded sGameEffect pending-effect flush\n");
    } else {
        HookLog("Rollback: failed to guard sGameEffect pending-effect flush (%d)\n",
                status);
    }

    status = MH_CreateHook((LPVOID)_addr(FN_UGAMEEFFECT_CREATE),
                                     (LPVOID)&hkEffectCreate,
                                     (LPVOID*)&oEffectCreate);
    if (status == MH_OK) {
        MH_EnableHook((LPVOID)_addr(FN_UGAMEEFFECT_CREATE));
        HookLog("Rollback: hooked uGameEffect factory\n");
    } else {
        HookLog("Rollback: failed to hook uGameEffect factory (%d)\n", status);
    }

    status = MH_CreateHook((LPVOID)_addr(FN_DRSTRANGE_UPDATE_SUMMONS),
                           (LPVOID)&hkDrStrangeUpdateSummons,
                           (LPVOID*)&oDrStrangeUpdateSummons);
    if (status == MH_OK) {
        MH_EnableHook((LPVOID)_addr(FN_DRSTRANGE_UPDATE_SUMMONS));
        HookLog("Rollback: guarded Dr Strange headless summon lifecycle\n");
    } else {
        HookLog("Rollback: failed to guard Dr Strange summon lifecycle (%d)\n",
                status);
    }

    status = MH_CreateHook((LPVOID)_addr(FN_EFFECT_DRAW),
                           (LPVOID)&hkEffectDraw,
                           (LPVOID*)&oEffectDraw);
    if (status == MH_OK) {
        MH_EnableHook((LPVOID)_addr(FN_EFFECT_DRAW));
        HookLog("Rollback: guarded stale effect draw resources\n");
    } else {
        HookLog("Rollback: failed to guard stale effect draw resources (%d)\n",
                status);
    }

    status = MH_CreateHook((LPVOID)_addr(FN_EFFECT_MOVE_AFTER),
                           (LPVOID)&hkEffectMoveAfter,
                           (LPVOID*)&oEffectMoveAfter);
    if (status == MH_OK) {
        MH_EnableHook((LPVOID)_addr(FN_EFFECT_MOVE_AFTER));
        HookLog("Rollback: guarded stale effect generator dispatch\n");
    } else {
        HookLog("Rollback: failed to guard stale effect generator dispatch (%d)\n",
                status);
    }

    status = MH_CreateHook((LPVOID)_addr(FN_EFFECT_DTOR_RELEASE),
                           (LPVOID)&hkEffectDtorRelease,
                           (LPVOID*)&oEffectDtorRelease);
    if (status == MH_OK) {
        MH_EnableHook((LPVOID)_addr(FN_EFFECT_DTOR_RELEASE));
        HookLog("Rollback: guarded effect destructor allocator release\n");
    } else {
        HookLog("Rollback: failed to guard effect destructor allocator release (%d)\n",
                status);
    }

    status = MH_CreateHook((LPVOID)_addr(FN_EFFECT_VDTOR),
                           (LPVOID)&hkEffectVdtor,
                           (LPVOID*)&oEffectVdtor);
    if (status == MH_OK) {
        MH_EnableHook((LPVOID)_addr(FN_EFFECT_VDTOR));
        HookLog("Rollback: guarded direct uEffect virtual destructor\n");
    } else {
        HookLog("Rollback: failed to guard direct uEffect virtual destructor (%d)\n",
                status);
    }

    status = MH_CreateHook((LPVOID)_addr(FN_PARTICLE_MANAGER_RELEASE_SUBBUFFER),
                           (LPVOID)&hkParticleManagerReleaseSubBuffer,
                           (LPVOID*)&oParticleManagerReleaseSubBuffer);
    if (status == MH_OK) {
        MH_EnableHook((LPVOID)_addr(FN_PARTICLE_MANAGER_RELEASE_SUBBUFFER));
        HookLog("Rollback: guarded invalid particle sub-buffer release\n");
    } else {
        HookLog("Rollback: failed to guard particle sub-buffer release (%d)\n",
                status);
    }

    status = MH_CreateHook((LPVOID)_addr(FN_KIRIGAKURE_EFFECT_UPDATE),
                           (LPVOID)&hkKirigakureEffectUpdate,
                           (LPVOID*)&oKirigakureEffectUpdate);
    if (status == MH_OK) {
        MH_EnableHook((LPVOID)_addr(FN_KIRIGAKURE_EFFECT_UPDATE));
        HookLog("Rollback: guarded uKirigakureEffect weak attachment\n");
    } else {
        HookLog("Rollback: failed to guard uKirigakureEffect attachment (%d)\n", status);
    }
}
