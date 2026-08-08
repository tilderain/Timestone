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
static constexpr uintptr_t FN_SGAMEEFFECT_FLUSH_HIT_MARKS = 0x140131070;
static constexpr uintptr_t FN_MTOBJECT_IS_KIND_OF = 0x14004F0B0;
static constexpr uintptr_t FN_MTOBJECT_REPLACE_INDEXED_SLOT = 0x140056F30;
static constexpr uintptr_t FN_UGAMEEFFECT_CREATE = 0x140131410;
static constexpr uintptr_t FN_UGAMEEFFECT_OPERATOR_NEW = 0x140139E00;
static constexpr uintptr_t FN_DRSTRANGE_UPDATE_SUMMONS = 0x14007B9E0;
static constexpr uintptr_t FN_EFFECT_DRAW = 0x1405E4E70;
static constexpr uintptr_t FN_EFFECT_DTOR_RELEASE = 0x1405E07F0;
static constexpr uintptr_t FN_EFFECT_VDTOR = 0x1405E08C0;
static constexpr uintptr_t FN_EFFECT_CREATE_GENERATOR = 0x1405E27C0;
static constexpr uintptr_t FN_EFFECT_RELEASE_GENERATOR = 0x1405E6890;
static constexpr uintptr_t FN_PARTICLE_MANAGER_RELEASE_SUBBUFFER = 0x14080C120;
static constexpr uintptr_t FN_KIRIGAKURE_EFFECT_UPDATE = 0x14013B050;
static constexpr uintptr_t FN_SUNIT_REMOVE_UNIT = 0x14051B9F0;
static constexpr uintptr_t FN_SUNIT_REMOVE_FROM_MULTI_MASK = 0x14051BAF0;
static constexpr uintptr_t FN_MTOBJECT_IS_ALIVE = 0x140001B80;
static constexpr uintptr_t DTI_UGAME_EFFECT_MODEL = 0x140D46ED0;
static constexpr uintptr_t SMVC3MGR_ACTION_OFFSET = 0x48;
static constexpr uint32_t SUNIT_CATEGORY_COUNT_OFFSET = 0xC38;
static constexpr uint32_t SUNIT_CATEGORY_LISTS_OFFSET = 0x50;
static constexpr uint32_t SUNIT_CATEGORY_LIST_STRIDE = 0x30;
static constexpr uint32_t SSTAGE_BODY_OFFSETS[] = {0x2050, 0x2058};
static constexpr uint32_t PARTICLE_MANAGER_STRIDE = 0x150;
static constexpr uint32_t PARTICLE_MANAGER_BUFFER_OFFSET = 0x130;
static constexpr uint32_t MAX_PARTICLE_MANAGERS = 256;
static constexpr uint32_t SSHOT_CATEGORY = 10;
static constexpr uint32_t TEAM_OFFSETS[] = {0x40, 0x300};
static constexpr uint32_t CHARACTER_EFFECT_SLOT_OFFSETS[] = {
    0x68F0, 0x68F8, 0x6900, 0x6920,
};

using EffectCreateFn = void*(__fastcall*)(void*, int, int, void*, int, int,
    void*, void*, int, void*, int, uint32_t, void*);

static constexpr int MAX_GENERATOR_NODES = 256;

static EffectCreateFn oEffectCreate = nullptr;
static void*(__fastcall* oGameEffectOperatorNew)(uint64_t, uint32_t) = nullptr;
static void(__fastcall* oDrStrangeUpdateSummons)(void*) = nullptr;
static void(__fastcall* oEffectDraw)(void*, void*) = nullptr;
static void(__fastcall* oEffectDtorRelease)(void*) = nullptr;
static void(__fastcall* oEffectVdtor)(void*, uint32_t) = nullptr;
static uint64_t(__fastcall* oEffectCreateGenerator)(void*) = nullptr;
static void(__fastcall* oParticleManagerReleaseSubBuffer)(void*) = nullptr;
static void(__fastcall* oGameEffectFlushPending)(void*) = nullptr;
static void(__fastcall* oGameEffectFlushHitMarks)(void*) = nullptr;
static uint8_t(__fastcall* oMtObjectIsKindOf)(void*, void*) = nullptr;
static void(__fastcall* oMtObjectReplaceIndexedSlot)(void*, uint32_t, void*) =
    nullptr;
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

static bool IsShotUnitReference(uint64_t object)
{
    if (!object || !IsAccessibleRange(object, 0x14) ||
        !IsUnitObject((void*)object)) {
        return false;
    }
    return (*(uint32_t*)(object + 0x10) & 0x3F8u) ==
        (SSHOT_CATEGORY << 3);
}

static void DetachRetiringEffectFromShot(void* effect)
{
    if (!IsAccessibleRange((uint64_t)effect, 0x290)) return;

    // uShot setup binds owned uGameEffects through uEffect::mParent (+0x40),
    // uGameEffect::mpOwner (+0x280), and sometimes mpRequestUnit (+0x288).
    // A retired effect can outlive synchronous shot destruction; if the shot
    // allocator reuses that address, delayed cleanup could act on the fresh
    // shot and clear its newly-created callback slots.
    for (uint32_t offset : {0x40u, 0x280u, 0x288u}) {
        uint64_t* reference = (uint64_t*)((uint8_t*)effect + offset);
        if (IsShotUnitReference(*reference))
            *reference = 0;
    }
}

static void SanitizeEffectOwnedParticleState(void* effect)
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
            uint64_t buffer = *(uint64_t*)(
                manager + PARTICLE_MANAGER_BUFFER_OFFSET);
            if (buffer && !IsAccessiblePointer(buffer)) {
                // Suppress only the invalid free. uEffect__releaseGenerator
                // must retain the valid buffers, count, and manager array so
                // native teardown can return their allocations.
                std::memset(manager + PARTICLE_MANAGER_BUFFER_OFFSET, 0, 0x18);
            }
        }
        return;
    }

    *(uint16_t*)((uint8_t*)effect + 0x208) = 0;
    // Even when manager records are malformed, preserve a readable array
    // pointer. Native teardown skips the zero-count loop and still frees the
    // containing allocation.
    if (!managers || !IsAccessibleRange(managers, 1))
        *(uint64_t*)((uint8_t*)effect + 0x220) = 0;
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

    DetachRetiringEffectFromShot(effect);

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
        uint64_t previous = 0;
        uint64_t cycleCheckpoint = generator;
        uint32_t cyclePower = 1;
        uint32_t cycleLength = 0;
        bool validGeneratorChain = true;
        for (int count = 0; node != 0 && count < MAX_GENERATOR_NODES; ++count) {
            if (g_activeRollbackProfile)
                ++g_activeRollbackProfile->restoreEffectGeneratorNodes;
            if (!IsAccessibleRange(node, 0x20)) {
                validGeneratorChain = false;
                break;
            }
            uint64_t vtable = *(uint64_t*)node;
            // uEffect__releaseGenerator reads only node+0x18 and invokes the
            // deleting destructor in vtable slot 0. Requiring unrelated move
            // callbacks here rejected valid generators and detached the whole
            // chain, leaking every generator allocation on each restore.
            if (!IsAccessibleRange(vtable, sizeof(uint64_t)) ||
                !IsExecutableAddress(*(uint64_t*)vtable)) {
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
            previous = node;
            node = next;
            if (count == MAX_GENERATOR_NODES - 1 && node != 0)
                validGeneratorChain = false;
        }
        if (!validGeneratorChain) {
            // Keep every validated prefix reclaimable. If the first node is
            // already corrupt there is no safe native object to destroy;
            // otherwise terminate the chain before the bad suffix and let the
            // normal uEffect destructor release all validated generators.
            if (previous)
                *(uint64_t*)(previous + 0x18) = 0;
            else
                *(uint64_t*)((uint8_t*)effect + 0x218) = 0;
            SanitizeEffectOwnedParticleState(effect);
        }
    }

    {
        RollbackHookProfileScope profile(RollbackHookProfileId::UnitRemoveEffectSubBuffers);
        if (HasInvalidParticleManagerState(effect))
            SanitizeEffectOwnedParticleState(effect);
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

static void* __fastcall hkGameEffectOperatorNew(uint64_t size, uint32_t alignment)
{
    void* effect = oGameEffectOperatorNew(size, alignment);
    if (!effect) {
        uint64_t replay = *(uint64_t*)_addr(0x140D4D9D8);
        int32_t frame = replay ? *(int32_t*)(replay + 0x54) : -1;
        HookLog("UGAMEEFFECTALLOCFAIL frame=%d resim=%d caller=0x%llX "
                "size=%llu alignment=%u\n",
                frame, g_isResimulating ? 1 : 0,
                (uint64_t)__builtin_return_address(0), size, alignment);
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

static uint64_t __fastcall hkEffectCreateGenerator(void* effect)
{
    uint64_t result = oEffectCreateGenerator(effect);
    if ((uint8_t)result == 0 && effect &&
        IsAccessibleRange((uint64_t)effect, 0x230)) {
        uint64_t resource = *(uint64_t*)((uint8_t*)effect + 0x108);
        uint16_t expectedManagers = resource &&
            IsAccessibleRange(resource + 0x90, sizeof(uint16_t))
            ? *(uint16_t*)(resource + 0x90) : 0;
        HookLog("EFFECTALLOCFAIL effect=0x%llX resource=0x%llX expected=%u "
                "generators=0x%llX managers=0x%llX count=%u flags=0x%08X\n",
                (uint64_t)effect, resource, expectedManagers,
                *(uint64_t*)((uint8_t*)effect + 0x218),
                *(uint64_t*)((uint8_t*)effect + 0x220),
                *(uint16_t*)((uint8_t*)effect + 0x208),
                *(uint32_t*)((uint8_t*)effect + 0x100));
    }
    return result;
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

static void ClearInvalidCharacterIndexedEffects(uint64_t character)
{
    constexpr uint32_t slotArrayOffset = 0x6748;
    constexpr uint32_t slotCount = 21;
    if (!character || !IsAccessibleRange(
            character + slotArrayOffset, slotCount * sizeof(uint64_t))) {
        return;
    }

    for (uint32_t index = 0; index < slotCount; ++index) {
        uint64_t* slot = (uint64_t*)(character + slotArrayOffset +
                                    index * sizeof(uint64_t));
        uint64_t object = *slot;
        if (!object) continue;
        if (!HasCallableDtiVirtual((void*)object)) {
            *slot = 0;
            continue;
        }
        uint64_t dti = GetObjectDti(object);
        if (!dti || !IsAccessibleRange(dti, 0x28))
            *slot = 0;
    }
}

static void ClearInvalidCharacterIndexedEffects()
{
    uint64_t sCharacter = *(uint64_t*)_addr(PTR_SCHARACTER);
    if (!sCharacter) return;

    for (uint32_t teamOffset : TEAM_OFFSETS) {
        uint64_t node = sCharacter + teamOffset + 0x08;
        for (int safety = 8; node && safety-- > 0;) {
            if (!IsAccessibleRange(node, 0x18)) break;
            uint64_t character = *(uint64_t*)(node + 0x08);
            ClearInvalidCharacterIndexedEffects(character);
            node = *(uint64_t*)(node + 0x10);
        }
    }
}

static void __fastcall hkGameEffectFlushHitMarks(void* manager)
{
    if (g_isResimulating) {
        ClearInvalidCharacterIndexedEffects();
        // Hit-mark commands can target assist/proxy characters not reachable
        // through the short team-list scan above. Native command dispatch reads
        // their indexed effect slots and calls isKindOf without validating the
        // stored object's vtable.
        if (manager && IsAccessibleRange((uint64_t)manager, 0x1944)) {
            uint32_t count = *(uint32_t*)((uint8_t*)manager + 0x1940);
            if (count <= 128 && (!count || IsAccessibleRange(
                    (uint64_t)manager + 0x140, (size_t)count * 0x30))) {
                for (uint32_t index = 0; index < count; ++index) {
                    uint8_t* entry = (uint8_t*)manager + 0x140 + index * 0x30;
                    ClearInvalidCharacterIndexedEffects(
                        *(uint64_t*)(entry + 0x18));
                    ClearInvalidCharacterIndexedEffects(
                        *(uint64_t*)(entry + 0x20));
                }
            }
        }
    }
    oGameEffectFlushHitMarks(manager);
}

static uint8_t __fastcall hkMtObjectIsKindOf(void* object, void* dti)
{
    if (g_isResimulating) {
        if (!object || !IsAccessibleRange((uint64_t)object, sizeof(uint64_t)))
            return 0;
        uint64_t vtable = *(uint64_t*)object;
        if (!vtable || !IsAccessibleRange(vtable + 0x20, sizeof(uint64_t)) ||
            !IsExecutableAddress(*(uint64_t*)(vtable + 0x20))) {
            return 0;
        }
    }
    return oMtObjectIsKindOf(object, dti);
}

static void __fastcall hkMtObjectReplaceIndexedSlot(
    void* owner, uint32_t index, void* replacement)
{
    if (g_isResimulating && owner && index < 21 && IsAccessibleRange(
            (uint64_t)owner + 0x6748 + index * sizeof(void*), sizeof(void*))) {
        void** slot = (void**)((uint8_t*)owner + 0x6748 +
                               index * sizeof(void*));
        if (*slot && !HasCallableDtiVirtual(*slot))
            *slot = nullptr;
    }
    oMtObjectReplaceIndexedSlot(owner, index, replacement);
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
        if (!recipe.restoredPtr ||
            !IsAccessibleRange((uint64_t)recipe.restoredPtr, 0x290) ||
            !IsEffectObject(recipe.restoredPtr) ||
            !IsActiveUnit(recipe.restoredPtr) ||
            IsPersistentCharacterEffect(recipe.restoredPtr)) {
            continue;
        }
        RollbackEffectRecipe saved = recipe;
        saved.savedPtr = recipe.restoredPtr;
        ConcreteEffectSnapshot_Capture(recipe.restoredPtr, saved.state);
        slot.effectRecipes.push_back(saved);
    }
}

int Effect_RestoreRecipes(RollbackSnapshotSlot& slot)
{
    int restored = 0;
    int failed = 0;
    for (RollbackEffectRecipe& saved : slot.effectRecipes)
        saved.restoredPtr = nullptr;

    for (RollbackEffectRecipe& saved : slot.effectRecipes) {
        void* sGameEffect = saved.sGameEffect;
        if (!sGameEffect || !oEffectCreate) {
            ++failed;
            continue;
        }

        void* attachTo = saved.attachTo;
        void* effect = oEffectCreate(sGameEffect, saved.effectId, saved.a3,
                                     attachTo, saved.a5, saved.a6, saved.a7,
                                     saved.a8, saved.a9, saved.a10, saved.a11,
                                     saved.a12, saved.a13);
        if (!effect) {
            ++failed;
            continue;
        }
        saved.restoredPtr = effect;
        ConcreteEffectSnapshot_Restore(effect, saved.state);
        RollbackEffectRecipe live = saved;
        live.savedPtr = effect;
        live.restoredPtr = effect;
        g_liveEffectRecipes.push_back(live);
        ++restored;
    }
    if (failed != 0) {
        HookLog("Rollback: effect recipes restored=%d failed=%d\n",
                restored, failed);
    }
    return restored;
}

void Effect_PrepareUnitRemoval(void* unit)
{
    if (!unit || !IsAccessibleRange((uint64_t)unit, 0x14)) return;
    const uint32_t category =
        (*(uint32_t*)((uint8_t*)unit + 0x10) >> 3) & 0x7Fu;
    if (category < 13 || category > 16) return;

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

void Effect_ReleaseOwnedAllocations(void* effect)
{
    if (!effect || !IsAccessibleRange((uint64_t)effect, 0x230)) return;
    SanitizeEffectForDestruction(effect);
    ((void(__fastcall*)(void*))_addr(FN_EFFECT_RELEASE_GENERATOR))(effect);
}

bool Effect_RetireOne(void* sUnit, void* effect)
{
    if (!sUnit || !effect || !IsEffectObject(effect)) return false;

    SanitizeEffectForDestruction(effect);
    ((void(__fastcall*)(void*, void*))_addr(
        FN_SUNIT_REMOVE_FROM_MULTI_MASK))(sUnit, effect);
    ((char(__fastcall*)(void*, void*))_addr(FN_SUNIT_REMOVE_UNIT))(
        sUnit, effect);
    Effect_FinishUnitRemoval(effect);

    if (!IsAccessibleRange((uint64_t)effect, sizeof(uint64_t))) return false;
    uint64_t vtable = *(uint64_t*)effect;
    if (!IsAccessibleRange(vtable, sizeof(uint64_t)) ||
        !IsExecutableAddress(*(uint64_t*)vtable)) {
        return false;
    }
    ((void(__fastcall*)(void*, uint32_t))*(uint64_t*)vtable)(effect, 1);
    return true;
}

int Effect_RetireCurrent(void* sUnit)
{
    if (!sUnit) return 0;
    DetachExAtkSpaceChildren();
    void* stageBodies[2] = {};
    uint64_t sStage = *(uint64_t*)_addr(PTR_SSTAGE);
    if (sStage && IsAccessibleRange(sStage, 0x2060)) {
        for (size_t index = 0; index < std::size(SSTAGE_BODY_OFFSETS); ++index) {
            stageBodies[index] =
                *(void**)(sStage + SSTAGE_BODY_OFFSETS[index]);
        }
    }
    std::vector<void*> abandoned;
    uint32_t categoryCount = *(uint32_t*)((uint8_t*)sUnit + SUNIT_CATEGORY_COUNT_OFFSET);
    for (uint32_t category = 13; category <= 16 && category < categoryCount; ++category) {
        void* effect = *(void**)((uint8_t*)sUnit + SUNIT_CATEGORY_LISTS_OFFSET +
                                 SUNIT_CATEGORY_LIST_STRIDE * category);
        for (int safety = 4096; effect && safety-- > 0;) {
            if (g_activeRollbackProfile) ++g_activeRollbackProfile->restoreEffectVisits;
            void* next = *(void**)((uint8_t*)effect + 0x18);
            if (IsEffectObject(effect) && !IsPersistentCharacterEffect(effect) &&
                effect != stageBodies[0] && effect != stageBodies[1] &&
                std::find(abandoned.begin(), abandoned.end(), effect) == abandoned.end())
                abandoned.push_back(effect);
            effect = next;
        }
    }

    auto removeFromMultiMask =
        (void(__fastcall*)(void*, void*))_addr(FN_SUNIT_REMOVE_FROM_MULTI_MASK);
    auto removeUnit =
        (char(__fastcall*)(void*, void*))_addr(FN_SUNIT_REMOVE_UNIT);
    int destroyed = 0;
    for (void* effect : abandoned) {
        // State 3/4 effects from an earlier restore are still allocations and
        // must not be skipped. Native deferred retirement needs two category
        // ticks; repeated resims can otherwise allocate replacements faster
        // than those ticks run and exhaust the effect heap.
        SanitizeEffectForDestruction(effect);
        removeFromMultiMask(sUnit, effect);
        removeUnit(sUnit, effect);
        if (!IsAccessibleRange((uint64_t)effect, sizeof(uint64_t))) continue;
        uint64_t vtable = *(uint64_t*)effect;
        if (!IsAccessibleRange(vtable, sizeof(uint64_t)) ||
            !IsExecutableAddress(*(uint64_t*)vtable)) continue;
        auto destroy = (void(__fastcall*)(void*, uint32_t))*(uint64_t*)vtable;
        destroy(effect, 1);
        ++destroyed;
    }
    g_liveEffectRecipes.clear();
    return destroyed;
}

bool Effect_RetireGameEffectModel(void* sUnit, void* effect)
{
    if (!sUnit || !effect || !HasCallableDtiVirtual(effect) ||
        GetObjectDti((uint64_t)effect) !=
            (uint64_t)_addr(DTI_UGAME_EFFECT_MODEL)) {
        return false;
    }

    // uGameEffectModel is scheduler-owned but is not a uEffect, so the generic
    // retirement pass never reaches it. Mirror sGameEffect's native teardown.
    if (((uint8_t(__fastcall*)(void*))_addr(FN_MTOBJECT_IS_ALIVE))(effect)) {
        uint64_t vtable = *(uint64_t*)effect;
        uint64_t finalize = IsAccessibleRange(vtable + 0x78, 8)
            ? *(uint64_t*)(vtable + 0x78) : 0;
        if (!finalize || !IsExecutableAddress(finalize)) return false;
        ((void(__fastcall*)(void*))finalize)(effect);
    }

    uint32_t* stateFlags = (uint32_t*)((uint8_t*)effect + 0x10);
    // Native retirement moves category state 3 -> 4 while unlinking secondary
    // masks, then 4 -> 0 before removing the primary sUnit link.
    if ((*stateFlags & 7u) == 3) {
        *stateFlags = (*stateFlags & 0xFFFFFFFCu) | 4u;
        ((void(__fastcall*)(void*, void*))_addr(
            FN_SUNIT_REMOVE_FROM_MULTI_MASK))(sUnit, effect);
    }
    if ((*stateFlags & 7u) != 4) return false;

    *stateFlags &= 0xFFFFFFF8u;
    ((char(__fastcall*)(void*, void*))_addr(FN_SUNIT_REMOVE_UNIT))(
        sUnit, effect);
    // List removal does not free the object; virtual slot zero is the scalar
    // deleting destructor and flag 1 requests storage release.
    if (!IsAccessibleRange((uint64_t)effect, sizeof(uint64_t))) return false;
    uint64_t vtable = *(uint64_t*)effect;
    if (!IsAccessibleRange(vtable, sizeof(uint64_t)) ||
        !IsExecutableAddress(*(uint64_t*)vtable)) {
        return false;
    }
    ((void(__fastcall*)(void*, uint32_t))*(uint64_t*)vtable)(effect, 1);
    return true;
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

    status = MH_CreateHook((LPVOID)_addr(FN_SGAMEEFFECT_FLUSH_HIT_MARKS),
                           (LPVOID)&hkGameEffectFlushHitMarks,
                           (LPVOID*)&oGameEffectFlushHitMarks);
    if (status == MH_OK) {
        MH_EnableHook((LPVOID)_addr(FN_SGAMEEFFECT_FLUSH_HIT_MARKS));
        HookLog("Rollback: guarded sGameEffect hit-mark effect slots\n");
    } else {
        HookLog("Rollback: failed to guard hit-mark effect slots (%d)\n",
                status);
    }

    status = MH_CreateHook((LPVOID)_addr(FN_MTOBJECT_IS_KIND_OF),
                           (LPVOID)&hkMtObjectIsKindOf,
                           (LPVOID*)&oMtObjectIsKindOf);
    if (status == MH_OK) {
        MH_EnableHook((LPVOID)_addr(FN_MTOBJECT_IS_KIND_OF));
        HookLog("Rollback: guarded stale MtObject type checks\n");
    } else {
        HookLog("Rollback: failed to guard MtObject type checks (%d)\n", status);
    }

    status = MH_CreateHook((LPVOID)_addr(FN_MTOBJECT_REPLACE_INDEXED_SLOT),
                           (LPVOID)&hkMtObjectReplaceIndexedSlot,
                           (LPVOID*)&oMtObjectReplaceIndexedSlot);
    if (status == MH_OK) {
        MH_EnableHook((LPVOID)_addr(FN_MTOBJECT_REPLACE_INDEXED_SLOT));
        HookLog("Rollback: guarded stale indexed effect replacement\n");
    } else {
        HookLog("Rollback: failed to guard indexed effect replacement (%d)\n",
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

    /*status = MH_CreateHook((LPVOID)_addr(FN_UGAMEEFFECT_OPERATOR_NEW),
                           (LPVOID)&hkGameEffectOperatorNew,
                           (LPVOID*)&oGameEffectOperatorNew);
    if (status == MH_OK) {
        MH_EnableHook((LPVOID)_addr(FN_UGAMEEFFECT_OPERATOR_NEW));
        HookLog("Rollback: tracing direct uGameEffect allocation failures\n");
    } else {
        HookLog("Rollback: failed to trace direct uGameEffect allocation (%d)\n",
                status);
    }*/

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

    /*status = MH_CreateHook((LPVOID)_addr(FN_EFFECT_CREATE_GENERATOR),
                           (LPVOID)&hkEffectCreateGenerator,
                           (LPVOID*)&oEffectCreateGenerator);
    if (status == MH_OK) {
        MH_EnableHook((LPVOID)_addr(FN_EFFECT_CREATE_GENERATOR));
        HookLog("Rollback: tracing effect generator allocation failures\n");
    } else {
        HookLog("Rollback: failed to trace effect generator allocation (%d)\n",
                status);
    }*/

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
