#include "../pch.h"

#include "rollback_effect.h"
#include "effect_pending_retention.h"

#include "mk10utils.h"
#include "rollback.h"
#include "rollback_memory.h"
#include "rollback_profile.h"
#include "rollback_snapshot.h"
#include "../kiero/minhook/include/MinHook.h"

#include <algorithm>
#include <array>
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
static constexpr uintptr_t FN_SGAMEEFFECT_SPAWN_SHORT = 0x140131660;
static constexpr uintptr_t FN_SUNIT_GET_INSTANCE = 0x140003160;
static constexpr uintptr_t FN_SUNIT_ADD_BOTTOM = 0x14051A6B0;
static constexpr uintptr_t FN_UGAMEEFFECT_OPERATOR_NEW = 0x140139E00;
static constexpr uintptr_t FN_DRSTRANGE_UPDATE_SUMMONS = 0x14007B9E0;
static constexpr uintptr_t FN_EFFECT_DRAW = 0x1405E4E70;
static constexpr uintptr_t FN_EFFECT_DTOR_RELEASE = 0x1405E07F0;
static constexpr uintptr_t FN_EFFECT_VDTOR = 0x1405E08C0;
static constexpr uintptr_t FN_EFFECT_CREATE_GENERATOR = 0x1405E27C0;
static constexpr uintptr_t FN_EFFECT_RELEASE_GENERATOR = 0x1405E6890;
static constexpr uintptr_t FN_PARTICLE_MANAGER_RELEASE_SUBBUFFER = 0x14080C120;
static constexpr uintptr_t FN_GAME_EFFECT_ATTACHMENT_UPDATE = 0x14013B050;
static constexpr uintptr_t FN_EXATKSPACE_SYNC_JOB = 0x140138250;
static constexpr uintptr_t FN_MTARRAY_DELETE_ALL = 0x1400112D0;
static constexpr uintptr_t FN_SUNIT_REMOVE_UNIT = 0x14051B9F0;
static constexpr uintptr_t FN_SUNIT_REMOVE_FROM_MULTI_MASK = 0x14051BAF0;
static constexpr uintptr_t FN_MTOBJECT_IS_ALIVE = 0x140001B80;
static constexpr uintptr_t DTI_UGAME_EFFECT_MODEL = 0x140D46ED0;
static constexpr uintptr_t VTABLE_URROCKET_RACCOON = 0x140A7DF30;
static constexpr uintptr_t SMVC3MGR_ACTION_OFFSET = 0x48;
static constexpr uint32_t SUNIT_CATEGORY_COUNT_OFFSET = 0xC38;
static constexpr uint32_t SUNIT_CATEGORY_LISTS_OFFSET = 0x50;
static constexpr uint32_t SUNIT_CATEGORY_LIST_STRIDE = 0x30;
static constexpr uint32_t SSTAGE_BODY_OFFSETS[] = {0x2050, 0x2058};
static constexpr uint32_t PARTICLE_MANAGER_STRIDE = 0x150;
static constexpr uint32_t PARTICLE_MANAGER_BUFFER_OFFSET = 0x130;
static constexpr uint32_t MAX_PARTICLE_MANAGERS = 256;
static constexpr uint32_t SSHOT_CATEGORY = 10;
static constexpr uint32_t SSHOT_CALLBACK_EFFECTS_OFFSET = 0x1F78;
static constexpr uint32_t SSHOT_CALLBACK_EFFECT_COUNT = 13;
static constexpr uint32_t TEAM_OFFSETS[] = {0x40, 0x300};
static constexpr uint32_t ROCKET_JETPACK_OWNER_OFFSETS[] = {0x68F8, 0x6900, 0x6930};
static constexpr uint32_t CHARACTER_EFFECT_SLOT_OFFSETS[] = {
    0x68F0, 0x68F8, 0x6900, 0x6908, 0x6910, 0x6920,
};
static constexpr uint32_t CHARACTER_INDEXED_EFFECTS_OFFSET = 0x6748;
static constexpr uint32_t CHARACTER_INDEXED_EFFECT_COUNT = 21;

using EffectCreateFn = void*(__fastcall*)(void*, int, int, void*, int, int,
    void*, void*, int, void*, int, uint32_t, void*);
using EffectCreateShortFn = void*(__fastcall*)(void*, int, int, void*, int, int,
    int, void*);

static constexpr int MAX_GENERATOR_NODES = 256;

static EffectCreateFn oEffectCreate = nullptr;
static EffectCreateShortFn oEffectCreateShort = nullptr;
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
static void(__fastcall* oGameEffectAttachmentUpdate)(void*) = nullptr;
static std::vector<RollbackEffectRecipe> g_liveEffectRecipes;
static uint64_t g_effectSpawnSequence = 0;

static std::vector<void*> g_resimEffects;
static thread_local bool g_inGameEffectFlushHitMarks = false;

struct EffectTraceEntry {
    uint64_t sequence;
    uint64_t object;
    uint64_t vtable;
    uint64_t parent;
    uint64_t resource;
    uint64_t arg0;
    uint64_t arg1;
    int32_t frame;
    uint32_t thread;
    const char* event;
};

static constexpr uint32_t EFFECT_TRACE_CAPACITY = 512;
static EffectTraceEntry g_effectTrace[EFFECT_TRACE_CAPACITY] = {};
static volatile LONG64 g_effectTraceSequence = 0;

static void TraceEffect(const char* event, void* object,
                        uint64_t arg0 = 0, uint64_t arg1 = 0)
{
    const uint64_t sequence = (uint64_t)InterlockedIncrement64(
        &g_effectTraceSequence);
    EffectTraceEntry& entry = g_effectTrace[
        sequence % EFFECT_TRACE_CAPACITY];
    entry.sequence = 0;
    entry.object = (uint64_t)object;
    const bool readable = object && IsAccessibleRange((uint64_t)object, 0x110);
    entry.vtable = readable ? *(uint64_t*)object : 0;
    entry.parent = readable ? *(uint64_t*)((uint8_t*)object + 0x40) : 0;
    entry.resource = readable ? *(uint64_t*)((uint8_t*)object + 0x108) : 0;
    entry.arg0 = arg0;
    entry.arg1 = arg1;
    entry.frame = g_resimulateCurrentFrame;
    entry.thread = GetCurrentThreadId();
    entry.event = event;
    MemoryBarrier();
    entry.sequence = sequence;
}

bool Effect_DumpCrashTrace(FILE* file, uint64_t object)
{
    if (!file || !object) return false;
    const uint64_t latest = (uint64_t)g_effectTraceSequence;
    const uint64_t first = latest > EFFECT_TRACE_CAPACITY
        ? latest - EFFECT_TRACE_CAPACITY + 1 : 1;
    bool wroteHeader = false;
    for (uint64_t sequence = first; sequence <= latest; ++sequence) {
        const EffectTraceEntry& entry = g_effectTrace[
            sequence % EFFECT_TRACE_CAPACITY];
        if (entry.sequence != sequence || entry.object != object) continue;
        if (!wroteHeader) {
            std::fprintf(file, "effect lifecycle trace for 0x%016llX:\n",
                         (unsigned long long)object);
            wroteHeader = true;
        }
        std::fprintf(file,
            "  seq=%llu event=%s frame=%d thread=%u vtable=0x%llX "
            "parent=0x%llX resource=0x%llX arg0=0x%llX arg1=0x%llX\n",
            (unsigned long long)entry.sequence,
            entry.event ? entry.event : "?", entry.frame, entry.thread,
            (unsigned long long)entry.vtable,
            (unsigned long long)entry.parent,
            (unsigned long long)entry.resource,
            (unsigned long long)entry.arg0,
            (unsigned long long)entry.arg1);
    }
    return wroteHeader;
}

static void ForgetResimEffect(void* effect)
{
    g_resimEffects.erase(
        std::remove(g_resimEffects.begin(), g_resimEffects.end(), effect),
        g_resimEffects.end());
}

static bool IsActiveUnit(void* unit)
{
    if (!unit) return false;
    uint32_t state = *(uint32_t*)((uint8_t*)unit + 0x10);
    return (state & 7u) < 3u && (state & 0x3F8u) != 0x3F8u;
}

static bool IsRollbackEffectCategory(void* effect)
{
    if (!effect) return false;
    const uint32_t category =
        (*(uint32_t*)((uint8_t*)effect + 0x10) >> 3) & 0x7Fu;
    return category >= 13 && category <= 16;
}

static bool IsUsableEffectParent(void* parent)
{
    if (!parent || !HasCallableDtiVirtual(parent)) return false;
    const uint64_t vtable = *(uint64_t*)parent;
    return IsAccessibleRange(vtable + 0xA0, sizeof(uint64_t)) &&
        IsExecutableAddress(*(uint64_t*)(vtable + 0xA0));
}

static bool IsPersistentCharacterEffect(void* effect)
{
    uint64_t sCharacter = *(uint64_t*)_addr(PTR_SCHARACTER);
    if (!sCharacter || !effect) return false;

    for (uint32_t teamOffset : TEAM_OFFSETS) {
        uint64_t node = sCharacter + teamOffset + 0x08;
        for (int safety = 8; node && safety-- > 0;) {
            uint64_t character = *(uint64_t*)(node + 0x08);
            if (character) {
                // Only a live character slot establishes retention. Child
                // effects still need native retirement to release their units.
                for (uint32_t slotOffset : CHARACTER_EFFECT_SLOT_OFFSETS) {
                    if (oEffectCreateShort && *(uint64_t*)character ==
                            (uint64_t)_addr(VTABLE_URROCKET_RACCOON) &&
                        (slotOffset == 0x68F8 || slotOffset == 0x6900))
                        continue;
                    if (*(void**)(character + slotOffset) == effect)
                        return true;
                }
                for (uint32_t index = 0;
                     index < CHARACTER_INDEXED_EFFECT_COUNT; ++index) {
                    if (*(void**)(character + CHARACTER_INDEXED_EFFECTS_OFFSET +
                                  index * sizeof(void*)) == effect) {
                        return true;
                    }
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
    // uShot setup binds owned uGameEffects through uEffect::mParent (+0x40),
    // uGameEffect::mpOwner (+0x280), and sometimes mpRequestUnit (+0x288).
    // Clear both sides before freeing the effect. Otherwise the later shot
    // destructor can release the same allocation through its callback array.
    // If the shot allocator reuses that address, delayed cleanup must also not
    // clear the fresh shot's newly-created callback slots.
    for (uint32_t offset : {0x40u, 0x280u, 0x288u}) {
        uint64_t* reference = (uint64_t*)((uint8_t*)effect + offset);
        if (!IsShotUnitReference(*reference)) continue;

        uint8_t* shot = (uint8_t*)*reference;
        for (uint32_t index = 0; index < SSHOT_CALLBACK_EFFECT_COUNT; ++index) {
            void** slot = (void**)(shot + SSHOT_CALLBACK_EFFECTS_OFFSET +
                                   index * sizeof(void*));
            if (*slot == effect) *slot = nullptr;
        }
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
    const uint32_t count = *(uint16_t*)((uint8_t*)effect + 0x208);
    const uint64_t managers = *(uint64_t*)((uint8_t*)effect + 0x220);
    if (!managers) return count != 0;
    if (count > MAX_PARTICLE_MANAGERS) return true;
    if (count == 0) return !IsAccessibleRange(managers, 1);
    if (!IsAccessibleRange(managers,
                           (size_t)count * PARTICLE_MANAGER_STRIDE)) {
        return true;
    }

    for (uint32_t index = 0; index < count; ++index) {
        uint8_t* manager = (uint8_t*)managers +
            (size_t)index * PARTICLE_MANAGER_STRIDE;
        uint64_t buffer = *(uint64_t*)(manager + PARTICLE_MANAGER_BUFFER_OFFSET);
        if (buffer && !IsAccessiblePointer(buffer)) return true;
    }
    return false;
}

static void SanitizeEffectForDestruction(void* effect)
{
    if (!effect) return;
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

bool Effect_IsExAtkSpaceUnit(const void* unit)
{
    // All eight levels and the base class share this +0x50 lifecycle method.
    // Called with a validated unit; avoid another DTI walk in headless dispatch.
    return unit && *(const uint64_t*)(*(const uint64_t*)unit + 0x50) ==
        (uint64_t)_addr(FN_EXATKSPACE_SYNC_JOB);
}

bool Effect_IsExAtkSpaceTransitionEffect(const void* effect, bool validateResource)
{
    if (!effect) return false;
    const uint32_t flags = *(const uint32_t*)((const uint8_t*)effect + 0x10);
    if ((flags & 7u) - 1u >= 2u || (flags & 0x3F8u) == 0x3F8u) return false;

    // Native low-ID effect dispatch creates tear variants 0102/0107 together
    // with companion 0106, and registers them in sGameEffect's tracked array.
    // None belongs to the four ExAtkSpace child slots. These are full native
    // (DTI.id << 32) | MtCRC(path) IDs; see doc/super_presentation_resim.md.
    const uint64_t resource = *(const uint64_t*)((const uint8_t*)effect + 0x108);
    // Restore/capture inspect potentially stale tracked objects. Native
    // headless dispatch already has a live effect owning its resource, so its
    // per-phase check needs only pointer loads and an integer comparison.
    if (!resource || (validateResource && !IsAccessibleRange(resource, 0x68)))
        return false;
    const uint64_t id = *(const uint64_t*)(resource + 0x60);
    return id == 0x6D5AE854FF040E06ull || // eft\cmn\efl\0102: page tear
           id == 0x6D5AE854F869CA1Full || // eft\cmn\efl\0106: transition companion
           id == 0x6D5AE8548F6EFA89ull;   // eft\cmn\efl\0107: circular tear
}

void Effect_FlushTrackedForRestore(void* manager)
{
    if (!manager) return;
    // Restore runs at the main-move boundary, before ordinary/headless unit
    // jobs, just like the surrounding shot and effect teardown. This helper
    // must not be used as a replacement for concurrent native script flushes.
    auto* bytes = (uint8_t*)manager;
    uint32_t& count = *(uint32_t*)(bytes + 0x128);
    void**& entries = *(void***)(bytes + 0x138);
    if (count > 4096 || (count && (!entries || !IsAccessibleRange(
            (uint64_t)entries, (size_t)count * sizeof(void*))))) {
        // Keep the existing native/hooked fallback for malformed headers.
        ((void(__fastcall*)(void*))_addr(FN_SGAMEEFFECT_FLUSH_PENDING))(manager);
        return;
    }

    // This is a kill-tracking array, not merely a list of unregistered spawns.
    // The native flush would kill the live tears BEFORE category retirement
    // could protect them. Keep their entries and allocation for ordinary script
    // cleanup; retire the rest through the same native virtual +0x78.
    EffectPending::FlushRetaining(entries, count,
        [](void* effect) {
            // Reject unrelated resources before the virtual DTI/hierarchy walk.
            // Validate the fields read by the resource predicate first, since
            // the tracking array may contain stale entries at this boundary.
            return IsAccessibleRange((uint64_t)effect, 0x110) &&
                Effect_IsExAtkSpaceTransitionEffect(effect) &&
                IsEffectObject(effect);
        },
        [](void* effect) {
            if (!IsAccessibleRange((uint64_t)effect, 0x14) ||
                ((*(uint32_t*)((uint8_t*)effect + 0x10) & 7u) - 1u >= 2u))
                return;
            const uint64_t vtable = *(uint64_t*)effect;
            if (IsAccessibleRange(vtable, 0x80) &&
                IsExecutableAddress(*(uint64_t*)(vtable + 0x78))) {
                ((void(__fastcall*)(void*))*(uint64_t*)(vtable + 0x78))(effect);
            }
        });
    if (count == 0) {
        ((void(__fastcall*)(void*, uint8_t))_addr(FN_MTARRAY_DELETE_ALL))(
            bytes + 0x120, 1);
    }
}

static std::array<void*, 4> GetRetainedExAtkSpaceChildren(bool detachUnretained = false)
{
    std::array<void*, 4> children{};
    uint64_t sMvc3Mgr = *(uint64_t*)_addr(PTR_SMVC3MGR);
    if (!sMvc3Mgr) return children;
    uint64_t sAction = *(uint64_t*)(sMvc3Mgr + SMVC3MGR_ACTION_OFFSET);
    if (!sAction) return children;
    uint64_t exAtkSpace = *(uint64_t*)(sAction + 0x148);
    if (!exAtkSpace || !IsAccessibleRange(exAtkSpace, 0x1D38)) return children;
    const uint64_t vtable = *(uint64_t*)exAtkSpace;
    if (!IsAccessibleRange(vtable, 0x58) ||
        !Effect_IsExAtkSpaceUnit((void*)exAtkSpace)) return children;

    const uint32_t ownerState = *(uint32_t*)(exAtkSpace + 0x10) & 7u;
    void** slots = (void**)(exAtkSpace + 0x1D18);
    for (size_t index = 0; index < children.size(); ++index) {
        void* child = slots[index];
        if (ownerState - 1u < 2u && child &&
            IsAccessibleRange((uint64_t)child, 0x14) &&
            ((*(uint32_t*)((uint8_t*)child + 0x10) & 7u) - 1u < 2u) &&
            IsActiveUnit(child) && IsEffectObject(child)) {
            children[index] = child;
        } else if (detachUnretained) {
            // Anything retirement may free must be detached first. Retaining
            // stale/dead child pointers would reintroduce the +0x78 crash in
            // uExAtkSpace__finalizeOwnedEffects (0x140137A10).
            slots[index] = nullptr;
        }
    }
    return children;
}

static void* __fastcall hkEffectCreate(void* manager, int effectId, int a3,
                                       void* attachTo, int a5, int a6, void* a7,
                                       void* a8, int a9, void* a10, int a11,
                                       uint32_t a12, void* a13)
{
    RollbackHookProfileScope profile(RollbackHookProfileId::EffectCreate);
    void* effect = oEffectCreate(manager, effectId, a3, attachTo, a5, a6, a7,
                                 a8, a9, a10, a11, a12, a13);
    if constexpr (EFFECT_LIFECYCLE_TRACE_ENABLED)
        TraceEffect("factory", effect, (uint64_t)a8, (uint64_t)attachTo);
    InvalidateMemoryQueryCache();
    // Rollback retirement owns only gameplay effect categories 13-16. Tracking
    // HUD effects from categories 29-31 would recreate them without retiring
    // their originals, splitting UI model and particle ownership on restart.
    if (effect && IsRollbackEffectCategory(effect)) {
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

static bool IsRocketShortEffectReferenced(const RollbackEffectRecipe& recipe)
{
    if (!recipe.rocketShort || !recipe.attachTo ||
        *(uint64_t*)recipe.attachTo != (uint64_t)_addr(VTABLE_URROCKET_RACCOON))
        return false;
    for (uint32_t offset : ROCKET_JETPACK_OWNER_OFFSETS)
        if (*(void**)((uint8_t*)recipe.attachTo + offset) == recipe.restoredPtr)
            return true;
    return false;
}

static void* __fastcall hkEffectCreateShort(void* manager, int effectId, int resourceId,
                                          void* owner, int flags, int index,
                                          int volume, void* type)
{
    void* effect = oEffectCreateShort(manager, effectId, resourceId, owner,
                                     flags, index, volume, type);
    if (!effect || effectId != 100 || (resourceId != 0x17 && resourceId != 0x29) ||
        !owner || *(uint64_t*)owner != (uint64_t)_addr(VTABLE_URROCKET_RACCOON))
        return effect;

    RollbackEffectRecipe recipe{};
    recipe.savedPtr = recipe.restoredPtr = effect;
    recipe.sGameEffect = manager;
    recipe.effectId = effectId;
    recipe.a3 = resourceId;
    recipe.attachTo = owner;
    recipe.a5 = flags;
    recipe.a6 = index;
    recipe.a9 = volume;
    recipe.a13 = type;
    recipe.rocketShort = true;
    recipe.spawnSequence = ++g_effectSpawnSequence;
    g_liveEffectRecipes.push_back(std::move(recipe));
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
    if constexpr (EFFECT_LIFECYCLE_TRACE_ENABLED)
        TraceEffect("dtor-release", effect);
    ForgetResimEffect(effect);
    Effect_FinishUnitRemoval(effect);
    if (g_isResimulating)
        Effect_PrepareUnitRemoval(effect);
    oEffectDtorRelease(effect);
}

static void __fastcall hkEffectVdtor(void* effect, uint32_t flags)
{
    if constexpr (EFFECT_LIFECYCLE_TRACE_ENABLED)
        TraceEffect("vdtor", effect, flags);
    ForgetResimEffect(effect);
    Effect_FinishUnitRemoval(effect);
    if (g_isResimulating)
        Effect_PrepareUnitRemoval(effect);
    oEffectVdtor(effect, flags);
}

static uint64_t __fastcall hkEffectCreateGenerator(void* effect)
{
    if constexpr (EFFECT_LIFECYCLE_TRACE_ENABLED)
        TraceEffect("generator-enter", effect);
    uint64_t result = oEffectCreateGenerator(effect);
    if constexpr (EFFECT_LIFECYCLE_TRACE_ENABLED)
        TraceEffect("generator-exit", effect, result);
    if (g_isResimulating && IsRollbackEffectCategory(effect) &&
        std::find(g_resimEffects.begin(), g_resimEffects.end(), effect) ==
            g_resimEffects.end()) {
        g_resimEffects.push_back(effect);
    }
    if ((uint8_t)result == 0 && effect) {
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
    if (g_isResimulating && manager) {
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

static bool IsValidCharacterIndexedEffect(void* object)
{
    if (!object || !IsEffectObject(object)) return false;
    uint64_t vtable = *(uint64_t*)object;
    return IsAccessibleRange(vtable + 0x78, sizeof(uint64_t)) &&
           IsExecutableAddress(*(uint64_t*)(vtable + 0x78));
}

static void ClearInvalidCharacterIndexedEffects(uint64_t character)
{
    if (!character) return;

    for (uint32_t index = 0; index < CHARACTER_INDEXED_EFFECT_COUNT; ++index) {
        uint64_t* slot = (uint64_t*)(character +
                                    CHARACTER_INDEXED_EFFECTS_OFFSET +
                                    index * sizeof(uint64_t));
        uint64_t object = *slot;
        if (!object) continue;
        if (!IsValidCharacterIndexedEffect((void*)object))
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
            uint64_t character = *(uint64_t*)(node + 0x08);
            ClearInvalidCharacterIndexedEffects(character);
            node = *(uint64_t*)(node + 0x10);
        }
    }
}

static void __fastcall hkGameEffectFlushHitMarks(void* manager)
{
    // A replayed effect can be destroyed during resimulation while its
    // character slot is consumed by the outer live tick immediately afterward.
    if (g_isResimulating || g_postResimFrame >= 0) {
        ClearInvalidCharacterIndexedEffects();
        // Hit-mark commands can target assist/proxy characters not reachable
        // through the short team-list scan above. Native command dispatch reads
        // their indexed effect slots and calls isKindOf without validating the
        // stored object's vtable.
        if (manager) {
            uint32_t count = *(uint32_t*)((uint8_t*)manager + 0x1940);
            if (count <= 128) {
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
    const bool wasFlushing = g_inGameEffectFlushHitMarks;
    g_inGameEffectFlushHitMarks = true;
    oGameEffectFlushHitMarks(manager);
    g_inGameEffectFlushHitMarks = wasFlushing;
}

static uint8_t __fastcall hkMtObjectIsKindOf(void* object, void* dti)
{
    if (g_isResimulating || g_postResimFrame >= 0 ||
        g_inGameEffectFlushHitMarks) {
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
    if ((g_isResimulating || g_postResimFrame >= 0 ||
         g_inGameEffectFlushHitMarks) &&
        owner && index < CHARACTER_INDEXED_EFFECT_COUNT) {
        void** slot = (void**)((uint8_t*)owner +
                               CHARACTER_INDEXED_EFFECTS_OFFSET +
                               index * sizeof(void*));
        if (*slot && !IsValidCharacterIndexedEffect(*slot))
            *slot = nullptr;
    }
    oMtObjectReplaceIndexedSlot(owner, index, replacement);
}

static void __fastcall hkGameEffectAttachmentUpdate(void* effect)
{
    RollbackHookProfileScope profile(RollbackHookProfileId::GameEffectAttachmentUpdate);
    uint64_t* attachment = (uint64_t*)((uint8_t*)effect + 0x40);
    const bool hadAttachment = *attachment != 0;

        //Removed dumb crash guard check if needed:
    //if (*attachment && !HasCallableDtiVirtual((void*)*attachment))
    //    *attachment = 0;
    
    // Attached trails need current transforms before recording resim history.
    // Keep the native update after sanitizing a stale parent as well.
    if (g_isResimulating && !hadAttachment)
        return;
    oGameEffectAttachmentUpdate(effect);
}

void Effect_CaptureRecipes(RollbackSnapshotSlot& slot)
{
    slot.effectRecipes.clear();
    slot.characterEffectStates.clear();
    slot.characterEffectReferences.clear();
    slot.effectRecipes.reserve(g_liveEffectRecipes.size());
    const auto exAtkChildren = GetRetainedExAtkSpaceChildren();
    for (const RollbackEffectRecipe& recipe : g_liveEffectRecipes) {
        const bool rocketOwner = IsRocketShortEffectReferenced(recipe);
        // Native cleanup finishes emission and clears the owner before the unit
        // dies. Recreating that unowned tail can restart a looping jetpack graph.
        if (recipe.rocketShort && !rocketOwner) continue;
        if (!recipe.restoredPtr ||
            std::find(exAtkChildren.begin(), exAtkChildren.end(), recipe.restoredPtr) !=
                exAtkChildren.end() ||
            !IsEffectObject(recipe.restoredPtr) ||
            (!IsActiveUnit(recipe.restoredPtr) && !rocketOwner) ||
            !IsRollbackEffectCategory(recipe.restoredPtr) ||
            Effect_IsExAtkSpaceTransitionEffect(recipe.restoredPtr) ||
            IsPersistentCharacterEffect(recipe.restoredPtr)) {
            continue;
        }

        // sGameEffect__spawn_Ex passes a8 to uEffect__setEffectParent. The
        // original spawn argument can become stale when native code reparents
        // the live effect, so capture the authoritative current mParent.
        RollbackEffectRecipe saved = recipe;
        saved.a8 = *(void**)((uint8_t*)recipe.restoredPtr + 0x40);
        saved.attachTo = *(void**)((uint8_t*)recipe.restoredPtr + 0x288);

        saved.savedPtr = recipe.restoredPtr;
        ConcreteEffectSnapshot_Capture(recipe.restoredPtr, saved.state);
        if (saved.rocketShort) {
            saved.shortParentNo = *(uint32_t*)((uint8_t*)recipe.restoredPtr + 0x48);
            saved.shortControlState = *(uint64_t*)((uint8_t*)recipe.restoredPtr + 0x290);
        }
        slot.effectRecipes.push_back(std::move(saved));
    }

    // If the short hook is unavailable, retain the previous in-place timing
    // policy. Otherwise these owners resolve through stable short recipes.
    uint64_t sCharacter = *(uint64_t*)_addr(PTR_SCHARACTER);
    for (uint32_t teamOffset : TEAM_OFFSETS) {
        uint64_t node = sCharacter ? sCharacter + teamOffset + 0x08 : 0;
        for (int safety = 8; node && safety-- > 0;) {
            void* character = *(void**)(node + 0x08);
            if (character &&
                *(uint64_t*)character ==
                    (uint64_t)_addr(VTABLE_URROCKET_RACCOON)) {
                for (uint32_t offset : {0x68F8u, 0x6900u}) {
                    if (oEffectCreateShort) continue;
                    void* effect = *(void**)((uint8_t*)character + offset);
                    if (!effect || !IsEffectObject(effect)) continue;
                    RollbackCharacterEffectState saved;
                    saved.character = character;
                    saved.effect = effect;
                    saved.expectedEffectVtable = *(uint64_t*)effect;
                    saved.ownerOffset = offset;
                    ConcreteEffectSnapshot_Capture(effect, saved.state);
                    slot.characterEffectStates.push_back(std::move(saved));
                }

                const uint64_t referenceCaptureStart = Profile_Now();
                for (uint32_t offset : {0x68F0u, 0x68F8u, 0x6900u, 0x6930u}) {
                    void* effect = *(void**)((uint8_t*)character + offset);
                    RollbackCharacterEffectReference reference{};
                    reference.character = character;
                    reference.expectedCharacterVtable = *(uint64_t*)character;
                    reference.offset = offset;

                    auto recipe = std::find_if(
                        slot.effectRecipes.begin(), slot.effectRecipes.end(),
                        [effect](const RollbackEffectRecipe& saved) {
                            return effect && saved.savedPtr == effect;
                        });
                    if (recipe != slot.effectRecipes.end()) {
                        reference.kind =
                            RollbackCharacterEffectReferenceKind::Reconstructed;
                        reference.effectSpawnSequence = recipe->spawnSequence;
                    } else {
                        auto retained = std::find_if(
                            slot.characterEffectStates.begin(),
                            slot.characterEffectStates.end(),
                            [character, offset](
                                const RollbackCharacterEffectState& saved) {
                                return saved.character == character &&
                                       saved.ownerOffset == offset;
                            });
                        if (retained != slot.characterEffectStates.end()) {
                            reference.kind =
                                RollbackCharacterEffectReferenceKind::Retained;
                            reference.retainedEffect = retained->effect;
                            reference.expectedEffectVtable =
                                retained->expectedEffectVtable;
                        }
                    }
                    slot.characterEffectReferences.push_back(reference);
                }
                Profile_AddElapsed(
                    &RollbackProfile::saveCharacterEffectReferences,
                    referenceCaptureStart);
            }
            node = *(uint64_t*)(node + 0x10);
        }
    }
    if constexpr (ROLLBACK_PROFILING_ENABLED) {
        if (g_activeRollbackProfile) {
            g_activeRollbackProfile->snapshotCharacterEffectReferenceCount +=
                (uint32_t)slot.characterEffectReferences.size();
            for (const RollbackCharacterEffectReference& reference :
                 slot.characterEffectReferences) {
                if (reference.kind ==
                    RollbackCharacterEffectReferenceKind::Reconstructed) {
                    ++g_activeRollbackProfile->
                        snapshotCharacterEffectReferenceReconstructedCount;
                } else if (reference.kind ==
                           RollbackCharacterEffectReferenceKind::Retained) {
                    ++g_activeRollbackProfile->
                        snapshotCharacterEffectReferenceRetainedCount;
                } else {
                    ++g_activeRollbackProfile->
                        snapshotCharacterEffectReferenceNoneCount;
                }
            }
        }
    }
}

void Effect_RestoreCharacterReferences(RollbackSnapshotSlot& slot)
{
    for (const RollbackCharacterEffectReference& saved :
         slot.characterEffectReferences) {
        if (!saved.character ||
            *(uint64_t*)saved.character != saved.expectedCharacterVtable) {
            continue;
        }

        void* effect = nullptr;
        if (saved.kind ==
            RollbackCharacterEffectReferenceKind::Reconstructed) {
            auto recipe = std::find_if(
                slot.effectRecipes.begin(), slot.effectRecipes.end(),
                [&](const RollbackEffectRecipe& candidate) {
                    return candidate.spawnSequence == saved.effectSpawnSequence;
                });
            if (recipe != slot.effectRecipes.end()) effect = recipe->restoredPtr;
        } else if (saved.kind ==
                   RollbackCharacterEffectReferenceKind::Retained) {
            if (saved.retainedEffect &&
                *(uint64_t*)saved.retainedEffect == saved.expectedEffectVtable) {
                effect = saved.retainedEffect;
            }
        }
        *(void**)((uint8_t*)saved.character + saved.offset) = effect;
    }
}

void Effect_RestoreCharacterStates(RollbackSnapshotSlot& slot)
{
    for (const RollbackCharacterEffectState& saved :
         slot.characterEffectStates) {
        if (!saved.character ||
            *(uint64_t*)saved.character !=
                (uint64_t)_addr(VTABLE_URROCKET_RACCOON)) {
            continue;
        }
        if (saved.effect &&
            *(uint64_t*)saved.effect == saved.expectedEffectVtable) {
            ConcreteEffectSnapshot_Restore(saved.effect, saved.state, false);
        }
    }
}

int Effect_RestoreRecipes(RollbackSnapshotSlot& slot)
{
    if (slot.effectRecipes.empty()) return 0;
    const auto exAtkChildren = GetRetainedExAtkSpaceChildren();
    int restored = 0;
    int failed = 0;
    // A shot callback may also be tracked as a general effect. Reuse the shot's
    // fresh callback instead of creating a second independent copy of its burst.
    std::vector<std::pair<uint64_t, void*>> callbacks;
    callbacks.reserve(slot.shotRecipes.size() * SSHOT_CALLBACK_EFFECT_COUNT);
    for (const auto& shot : slot.shotRecipes) {
        for (uint32_t index = 0; index < SSHOT_CALLBACK_EFFECT_COUNT; ++index) {
            void* old = shot.concrete.callbackEffects[index].savedEffect;
            if (!old) continue;
            void* live = shot.restoredPtr ? *(void**)((uint8_t*)shot.restoredPtr +
                SSHOT_CALLBACK_EFFECTS_OFFSET + index * sizeof(void*)) : nullptr;
            callbacks.emplace_back((uint64_t)old, live);
        }
    }
    std::sort(callbacks.begin(), callbacks.end(), [](const auto& a, const auto& b) {
        return a.first < b.first;
    });
    for (RollbackEffectRecipe& saved : slot.effectRecipes)
        saved.restoredPtr = nullptr;

    for (RollbackEffectRecipe& saved : slot.effectRecipes) {
        if (saved.savedPtr &&
            std::find(exAtkChildren.begin(), exAtkChildren.end(), saved.savedPtr) !=
                exAtkChildren.end()) {
            // The native owner still has this complete presentation graph.
            // Do not rewind it or create an independently owned duplicate.
            saved.restoredPtr = saved.savedPtr;
            continue;
        }
        auto callback = std::lower_bound(callbacks.begin(), callbacks.end(),
            (uint64_t)saved.savedPtr, [](const auto& entry, uint64_t pointer) {
                return entry.first < pointer;
            });
        if (callback != callbacks.end() && callback->first == (uint64_t)saved.savedPtr) {
            saved.restoredPtr = callback->second;
            if (saved.restoredPtr && IsActiveUnit(saved.restoredPtr)) {
                RollbackEffectRecipe live = saved;
                live.savedPtr = saved.restoredPtr;
                auto existing = std::find_if(g_liveEffectRecipes.begin(), g_liveEffectRecipes.end(),
                    [&](const auto& recipe) { return recipe.restoredPtr == saved.restoredPtr; });
                if (existing != g_liveEffectRecipes.end()) *existing = std::move(live);
                else g_liveEffectRecipes.push_back(std::move(live));
                ++restored;
            }
            continue;
        }
        // Independent effects already inactive at capture must not return
        // to their factory's first emission on every rollback. Live shot bodies
        // were handled above and have a distinct missing-particle repair path.
        if (saved.state.CompletedIndependent() && !saved.rocketShort) continue;
        void* sGameEffect = saved.sGameEffect;
        if (!sGameEffect || (saved.rocketShort ? !oEffectCreateShort : !oEffectCreate) ||
            (saved.a8 && !IsUsableEffectParent(saved.a8)) ||
            (saved.attachTo && IsLikelyPointer((uint64_t)saved.attachTo) &&
             !HasCallableDtiVirtual(saved.attachTo))) {
            ++failed;
            continue;
        }

        void* attachTo = saved.attachTo;
        if (saved.rocketShort) {
            void* sUnit = ((void*(__fastcall*)())_addr(FN_SUNIT_GET_INSTANCE))();
            if (!sUnit) { ++failed; continue; }
            void* effect = oEffectCreateShort(sGameEffect, saved.effectId, saved.a3,
                                              attachTo, saved.a5, saved.a6,
                                              saved.a9, saved.a13);
            if (!effect) { ++failed; continue; }
            InvalidateMemoryQueryCache();
            *(void**)((uint8_t*)effect + 0x280) = attachTo;
            if (saved.a8) {
                uint64_t vtable = *(uint64_t*)effect;
                ((void(__fastcall*)(void*, void*, uint32_t))
                    *(uint64_t*)(vtable + 0x88))(effect, saved.a8, saved.shortParentNo);
            }
            *(uint64_t*)((uint8_t*)effect + 0x290) = saved.shortControlState;
            ((void(__fastcall*)(void*, uint32_t, void*, void*))_addr(
                FN_SUNIT_ADD_BOTTOM))(sUnit, 13, effect, nullptr);
            if (saved.state.CompletedIndependent()) {
                // Keep a dead-but-occupied owner until Rocket's native cleanup
                // consumes it. Clearing it early skips gravity/latch changes.
                uint64_t vtable = *(uint64_t*)effect;
                ((void(__fastcall*)(void*))*(uint64_t*)(vtable + 0x78))(effect);
            } else if (!ConcreteEffectSnapshot_Restore(effect, saved.state)) {
                HookLog("Rollback: Rocket short effect timeline restore failed (id=%d)\n",
                        saved.a3);
            }
            saved.restoredPtr = effect;
            RollbackEffectRecipe live = saved;
            live.savedPtr = effect;
            g_liveEffectRecipes.push_back(std::move(live));
            ++restored;
            continue;
        }
        if constexpr (EFFECT_LIFECYCLE_TRACE_ENABLED) {
            if (saved.a8) {
                TraceEffect("restore-parent", saved.a8,
                            (uint64_t)saved.savedPtr,
                            (uint32_t)saved.effectId);
            }
        }
        void* effect = oEffectCreate(sGameEffect, saved.effectId, saved.a3,
                                     attachTo, saved.a5, saved.a6, saved.a7,
                                     saved.a8, saved.a9, saved.a10, saved.a11,
                                     saved.a12, saved.a13);
        if constexpr (EFFECT_LIFECYCLE_TRACE_ENABLED)
            TraceEffect("restore-factory", effect, (uint64_t)saved.a8,
                        (uint64_t)attachTo);
        if (!effect) {
            ++failed;
            continue;
        }
        saved.restoredPtr = effect;
        ConcreteEffectSnapshot_Restore(effect, saved.state);
        RollbackEffectRecipe live = saved;
        live.savedPtr = effect;
        live.restoredPtr = effect;
        g_liveEffectRecipes.push_back(std::move(live));
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
    if (!unit) return;
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
    if (!effect) return;
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

    uint64_t vtable = *(uint64_t*)effect;
    ((void(__fastcall*)(void*, uint32_t))*(uint64_t*)vtable)(effect, 1);
    return true;
}

int Effect_RetireCurrent(void* sUnit)
{
    if (!sUnit) return 0;
    const auto exAtkChildren = GetRetainedExAtkSpaceChildren(true);
    void* stageBodies[2] = {};
    uint64_t sStage = *(uint64_t*)_addr(PTR_SSTAGE);
    if (sStage) {
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
            if (std::find(exAtkChildren.begin(), exAtkChildren.end(), effect) ==
                    exAtkChildren.end() &&
                IsEffectObject(effect) && !Effect_IsExAtkSpaceTransitionEffect(effect) &&
                !IsPersistentCharacterEffect(effect) &&
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
        uint64_t vtable = *(uint64_t*)effect;
        auto destroy = (void(__fastcall*)(void*, uint32_t))*(uint64_t*)vtable;
        destroy(effect, 1);
        ++destroyed;
    }

    // Native finalization can unlink a resimulation-created effect before its
    // deferred scalar destruction. A subsequent restore cannot discover that
    // detached object through sUnit, so reclaim tracked detached effects here.
    std::vector<void*> detached;
    for (void* effect : g_resimEffects) {
        if (!effect ||
            std::find(exAtkChildren.begin(), exAtkChildren.end(), effect) !=
                exAtkChildren.end() ||
            !IsAccessibleRange((uint64_t)effect, 0x14) ||
            (*(uint32_t*)((uint8_t*)effect + 0x10) & 0x3F8u) != 0x3F8u ||
            !IsEffectObject(effect) || IsPersistentCharacterEffect(effect) ||
            effect == stageBodies[0] || effect == stageBodies[1]) {
            continue;
        }
        detached.push_back(effect);
    }
    for (void* effect : detached) {
        if (Effect_RetireOne(sUnit, effect)) {
            ++destroyed;
        }
    }
    g_liveEffectRecipes.clear();
    return destroyed;
}

bool Effect_RetireGameEffectModel(void* sUnit, void* effect)
{
    if (!sUnit || !effect ||
        GetObjectDti((uint64_t)effect) !=
            (uint64_t)_addr(DTI_UGAME_EFFECT_MODEL)) {
        return false;
    }

    // uGameEffectModel is scheduler-owned but is not a uEffect, so the generic
    // retirement pass never reaches it. Mirror sGameEffect's native teardown.
    if (((uint8_t(__fastcall*)(void*))_addr(FN_MTOBJECT_IS_ALIVE))(effect)) {
        uint64_t vtable = *(uint64_t*)effect;
        uint64_t finalize = *(uint64_t*)(vtable + 0x78);
        if (!finalize) return false;
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
    uint64_t vtable = *(uint64_t*)effect;
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
    g_resimEffects.clear();
}

void Effect_InitHooks()
{
    MH_STATUS status = MH_CreateHook((LPVOID)_addr(FN_SGAMEEFFECT_SPAWN_SHORT),
                                     (LPVOID)&hkEffectCreateShort,
                                     (LPVOID*)&oEffectCreateShort);
    if (status == MH_OK)
        status = MH_EnableHook((LPVOID)_addr(FN_SGAMEEFFECT_SPAWN_SHORT));
    if (status == MH_OK) {
        HookLog("Rollback: tracking Rocket jetpack short effects\n");
    } else {
        oEffectCreateShort = nullptr;
        HookLog("Rollback: failed to track Rocket short effects (%d)\n", status);
    }

    status = MH_CreateHook((LPVOID)_addr(FN_SGAMEEFFECT_FLUSH_PENDING),
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

    status = MH_CreateHook((LPVOID)_addr(FN_EFFECT_CREATE_GENERATOR),
                            (LPVOID)&hkEffectCreateGenerator,
                            (LPVOID*)&oEffectCreateGenerator);
    if (status == MH_OK) {
        MH_EnableHook((LPVOID)_addr(FN_EFFECT_CREATE_GENERATOR));
        HookLog("Rollback: tracking resimulation-created effects\n");
    } else {
        HookLog("Rollback: failed to track resimulation-created effects (%d)\n",
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

    status = MH_CreateHook((LPVOID)_addr(FN_GAME_EFFECT_ATTACHMENT_UPDATE),
                           (LPVOID)&hkGameEffectAttachmentUpdate,
                           (LPVOID*)&oGameEffectAttachmentUpdate);
    if (status == MH_OK) {
        MH_EnableHook((LPVOID)_addr(FN_GAME_EFFECT_ATTACHMENT_UPDATE));
        HookLog("Rollback: hooked uGameEffect attachment update\n");
    } else {
        HookLog("Rollback: failed to hook uGameEffect attachment (%d)\n", status);
    }
}
