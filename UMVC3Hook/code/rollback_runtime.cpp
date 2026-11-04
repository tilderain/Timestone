#include "../pch.h"

#include "rollback.h"
#include "rollback_memory.h"
#include "rollback_diagnostics.h"
#include "rollback_profile.h"
#include "mk10utils.h"
#include "netplay.h"
#include "gameplay_hooks.h"

extern void HookLog(const char* fmt, ...);

static constexpr uintptr_t FN_IS_ONLINE_BATTLE = 0x140006AF0;
static constexpr uintptr_t FN_NDRAW_MATERIAL_RELEASE = 0x14076F760;
static constexpr uintptr_t FN_UMODEL_MOTION_DTOR = 0x140526000;
static constexpr uintptr_t FN_PARTICLE_FILTER_UPDATE = 0x1408D61C0;
static constexpr uintptr_t PTR_SCHARACTER = 0x140D44A70;
static constexpr uintptr_t TEAM_OFFSETS[] = {0x40, 0x300};
static constexpr uintptr_t TEAM_FIRST_NODE_OFFSET = 0x08;
static constexpr uintptr_t NODE_CHARACTER_OFFSET = 0x08;
static constexpr uintptr_t NODE_NEXT_OFFSET = 0x10;
static constexpr uintptr_t ONLINE_CALLER_RETURNS[] = {
    0x14003F083, // cChr__resolveActionTransitionState
    0x14003F251, // cChr__resolveAdvancedGuardTransition
    0x14004026A, // cChr__resolveDamageReactionTransition
    0x1400409D6, // cChr__tryStartThrowTransition
    0x140040F41, // cChr__tryStartVariableCounterTransition
    0x1400417BF, // cChr__resolveDamageReactionAction
    0x140048CBD, // cChr__populateHitReactionResult
    0x140203869, // aAction__init online/replay timing and job setup
};
using IsOnlineBattleFn = bool(__fastcall*)(uint64_t battleSetting);
static IsOnlineBattleFn g_originalIsOnlineBattle = nullptr;

using MtObjectSetFlagFn = void(__fastcall*)(uint64_t, uint8_t);
static MtObjectSetFlagFn g_originalMtObjectSetFlag = nullptr;
using MtAnimSetFrameIndexFn = void(__fastcall*)(uint64_t, uint32_t);
static MtAnimSetFrameIndexFn g_originalMtAnimSetFrameIndex = nullptr;
using MtAnimUpdateFrameFn = uint32_t(__fastcall*)(uint64_t, uint64_t, uint64_t);
static MtAnimUpdateFrameFn g_originalMtAnimUpdateFrame = nullptr;
using ResetMaterialsFn = void(__fastcall*)(uint64_t);
static ResetMaterialsFn g_originalResetMaterials = nullptr;
using BaseModelRenderSetupFn = void(__fastcall*)(uint64_t, uint64_t);
static BaseModelRenderSetupFn g_originalBaseModelRenderSetup = nullptr;
using ToonMaterialUpdateFn = void(__fastcall*)(uint64_t);
static ToonMaterialUpdateFn g_originalToonMaterialUpdate = nullptr;
using ParticleGeneratorUpdateFn = void(__fastcall*)(uint64_t);
static ParticleGeneratorUpdateFn g_originalParticleGeneratorModelUpdate = nullptr;
static ParticleGeneratorUpdateFn g_originalParticleGeneratorFilterUpdate = nullptr;
using GetJointPosFn = void*(__fastcall*)(uint64_t, void*);
static GetJointPosFn g_originalGetJointPos = nullptr;
using CharacterModelPostMoveFn = void(__fastcall*)(uint64_t);
static CharacterModelPostMoveFn g_originalCharacterModelPostMove = nullptr;
using ModelBoundsFromJointsFn = void(__fastcall*)(uint64_t);
static ModelBoundsFromJointsFn g_originalModelBoundsFromJoints = nullptr;
struct DeferredModelBounds {
    uint64_t model;
    uint64_t vtable;
};
static constexpr size_t DEFERRED_MODEL_BOUNDS_CAPACITY = 512;
static DeferredModelBounds
    g_deferredModelBounds[DEFERRED_MODEL_BOUNDS_CAPACITY] = {};
static size_t g_deferredModelBoundsCount = 0;
using CutInAddCharacterFn = void(__fastcall*)(uint64_t, uint64_t);
static CutInAddCharacterFn g_originalCutInAddCharacter = nullptr;
using CutInSetVariantFn = void(__fastcall*)(uint64_t, uint32_t);
static CutInSetVariantFn g_originalCutInSetVariant = nullptr;
using CopyCharacterPositionFn = uint64_t(__fastcall*)(uint64_t, int, float, uint64_t);
static CopyCharacterPositionFn g_originalCopyCharacterPosition = nullptr;
using ReplaceActiveStageFn = void(__fastcall*)(uint64_t, uint64_t);
static ReplaceActiveStageFn g_originalReplaceActiveStage = nullptr;
using UModelMotionDtorFn = void(__fastcall*)(uint64_t);
static UModelMotionDtorFn g_originalUModelMotionDtor = nullptr;
static LONG g_invalidMotionFreeCount = 0;
static uint64_t g_pendingMaterialRepairs[64] = {};
static size_t g_pendingMaterialRepairCount = 0;

static bool HasVirtualMethod(uint64_t object, size_t offset);

static bool HasMotionAllocationOwner(uint64_t allocation,
                                     uint64_t& block,
                                     uint64_t& owner)
{
    block = 0;
    owner = 0;
    if (!allocation)
        return true;

    const uint64_t headerOffset = *(uint64_t*)(allocation - sizeof(uint64_t));
    block = allocation - headerOffset;
    owner = *(uint64_t*)(block + sizeof(uint64_t));
    return owner && *(uint32_t*)(owner + 0x10) != 0;
}

static void __fastcall hkUModelMotionDtor(uint64_t motion)
{
    if (!motion || !g_originalUModelMotionDtor)
        return;

    uint64_t* allocationSlot = (uint64_t*)(motion + 0x160);
    const uint64_t allocation = *allocationSlot;
    uint64_t block = 0;
    uint64_t owner = 0;
    if (!HasMotionAllocationOwner(allocation, block, owner)) {
        const LONG count = InterlockedIncrement(&g_invalidMotionFreeCount);
        if (count <= 16) {
            HookLog("Rollback: suppressed stale uModel::Motion free "
                    "motion=0x%llX allocation=0x%llX block=0x%llX owner=0x%llX "
                    "caller=0x%llX count=%ld\n",
                    motion, allocation, block, owner,
                    (uint64_t)__builtin_return_address(0), count);
        }
        *allocationSlot = 0;
    }

    g_originalUModelMotionDtor(motion);
    // Native does not clear +0x160 after freeing it. Avoid a repeated free.
    *allocationSlot = 0;
}

static bool IsLiveCharacterModel(uint64_t model)
{
    const uint64_t sCharacter = *(uint64_t*)_addr(PTR_SCHARACTER);
    if (!model || !sCharacter)
        return false;

    for (int team = 0; team < 2; ++team) {
        uint64_t node = sCharacter + TEAM_OFFSETS[team] +
            TEAM_FIRST_NODE_OFFSET;
        for (int safety = 0; node && safety < 64; ++safety) {
            const uint64_t character =
                *(uint64_t*)(node + NODE_CHARACTER_OFFSET);
            if (character == model)
                return true;
            if (character) {
                const uint32_t weaponCount = *(uint32_t*)(character + 0x3F38);
                const uint64_t weapons = *(uint64_t*)(character + 0x3F40);
                if (weapons && weaponCount <= 256) {
                    for (uint32_t i = 0; i < weaponCount; ++i) {
                        if (*(uint64_t*)(weapons + (size_t)i * sizeof(uint64_t)) ==
                            model) {
                            return true;
                        }
                    }
                }
            }
            node = *(uint64_t*)(node + NODE_NEXT_OFFSET);
        }
    }
    return false;
}

static bool IsValidCachedMaterial(uint64_t material)
{
    if (!material || !HasVirtualMethod(material, 0x28))
        return false;
    const uint64_t vtable = *(uint64_t*)material;
    return *(uint64_t*)(vtable + 0x28) ==
        (uint64_t)_addr(FN_NDRAW_MATERIAL_RELEASE);
}

static bool IsMaterialCacheReady(uint64_t model)
{
    if (!model)
        return false;
    const uint32_t count = *(uint32_t*)(model + 0x118);
    const uint64_t entries = *(uint64_t*)(model + 0x110);
    if (count == 0)
        return true;
    if (count > 4096 || !entries) {
        return false;
    }
    for (uint32_t index = 0; index < count; ++index) {
        if (!IsValidCachedMaterial(*(uint64_t*)(entries +
                (size_t)index * sizeof(uint64_t)))) {
            return false;
        }
    }
    return true;
}

static void MarkMaterialRepairPending(uint64_t model)
{
    for (size_t i = 0; i < g_pendingMaterialRepairCount; ++i) {
        if (g_pendingMaterialRepairs[i] == model)
            return;
    }
    if (g_pendingMaterialRepairCount <
        sizeof(g_pendingMaterialRepairs) / sizeof(g_pendingMaterialRepairs[0]))
        g_pendingMaterialRepairs[g_pendingMaterialRepairCount++] = model;
}

static size_t FindPendingMaterialRepair(uint64_t model)
{
    for (size_t i = 0; i < g_pendingMaterialRepairCount; ++i) {
        if (g_pendingMaterialRepairs[i] == model)
            return i;
    }
    return SIZE_MAX;
}

static void ClearPendingMaterialRepair(size_t index)
{
    if (index >= g_pendingMaterialRepairCount)
        return;
    --g_pendingMaterialRepairCount;
    g_pendingMaterialRepairs[index] =
        g_pendingMaterialRepairs[g_pendingMaterialRepairCount];
}

static void __fastcall hkMtObjectSetFlag(uint64_t object, uint8_t enabled)
{
    // UI setup can reach this flag setter with a missing object.
    if (!object || !g_originalMtObjectSetFlag) {
        return;
    }
    g_originalMtObjectSetFlag(object, enabled);
}

static void __fastcall hkMtAnimSetFrameIndex(uint64_t animation, uint32_t frame)
{
    // UI setup can pass null after allocation failure; native dereferences it.
    if (!animation)
        return;
    g_originalMtAnimSetFrameIndex(animation, frame);
}

static void __fastcall hkCutInAddCharacter(uint64_t cutIn, uint64_t character)
{
    // Skip participant setup when the optional cut-in or character is absent.
    if (!cutIn || !character || !g_originalCutInAddCharacter) {
        return;
    }
    g_originalCutInAddCharacter(cutIn, character);
}

static void __fastcall hkCutInSetVariant(uint64_t cutIn, uint32_t variant)
{
    // Skip camera propagation when the optional cut-in is absent.
    if (!cutIn || !g_originalCutInSetVariant) {
        return;
    }
    g_originalCutInSetVariant(cutIn, variant);
}

static uint64_t __fastcall hkCopyCharacterPosition(uint64_t character,
                                                    int mode,
                                                    float stiffDamage,
                                                    uint64_t damageCharacter)
{
    if (mode == 0) {
        uint64_t source = character ? *(uint64_t*)(character + 0x6428) : 0;

        if (!source) {
            // Native also adds stiff damage. Preserve it when position copy fails.
            if (damageCharacter && stiffDamage > 0.0f) {
                using GetStiffDamageFn = float(__fastcall*)(uint64_t);
                using SetStiffDamageFn = void(__fastcall*)(uint64_t, float);
                auto getStiffDamage = (GetStiffDamageFn)_addr(0x140032F50);
                auto setStiffDamage = (SetStiffDamageFn)_addr(0x1400AEE10);
                setStiffDamage(damageCharacter,
                               getStiffDamage(damageCharacter) + stiffDamage);
            }

            static uint32_t logged = 0;
            if (logged++ < 8) {
                HookLog("Rollback: skipped null cChr+0x6428 position source "
                        "chr=0x%llX source=0x%llX\n", character, source);
            }
            return 0;
        }
    }

    return g_originalCopyCharacterPosition
        ? g_originalCopyCharacterPosition(character, mode, stiffDamage,
                                           damageCharacter)
        : 0;
}

static bool HasVirtualMethod(uint64_t object, size_t offset)
{
    if (!object) return false;
    const uint64_t vtable = *(uint64_t*)object;
    return vtable && *(uint64_t*)(vtable + offset) != 0;
}

static void __fastcall hkReplaceActiveStage(uint64_t stageManager,
                                             uint64_t newStage)
{
    constexpr uint32_t ACTIVE_STAGES_OFFSET = 0x2060;
    constexpr uintptr_t FN_MTARRAY_GET_SIZE = 0x1400129C0;
    constexpr uintptr_t FN_MTARRAY_GET_ELEMENT = 0x140010E40;
    constexpr uintptr_t FN_MTARRAY_REMOVE_POINTER = 0x1400115D0;
    constexpr uintptr_t STAGE_LOCK = 0x140D4F728;

    if (stageManager) {
        uint64_t activeStages = stageManager + ACTIVE_STAGES_OFFSET;
        auto getSize = (int(__fastcall*)(uint64_t))_addr(FN_MTARRAY_GET_SIZE);
        auto getElement =
            (uint64_t*(__fastcall*)(uint64_t, uint32_t))_addr(FN_MTARRAY_GET_ELEMENT);
        auto removePointer =
            (void(__fastcall*)(uint64_t, uint64_t))_addr(FN_MTARRAY_REMOVE_POINTER);

        EnterCriticalSection((LPCRITICAL_SECTION)_addr(STAGE_LOCK));
        if (getSize(activeStages) > 0) {
            uint64_t* first = getElement(activeStages, 0);
            uint64_t oldStage = first ? *first : 0;
            if (oldStage && !HasVirtualMethod(oldStage, 0xD8)) {
                HookLog("Rollback: removed invalid active stage entry "
                        "stage=0x%llX vtable=0x%llX\n",
                        oldStage,
                        *(uint64_t*)oldStage);
                removePointer(activeStages, oldStage);
            }
        }
        LeaveCriticalSection((LPCRITICAL_SECTION)_addr(STAGE_LOCK));
    }

    if (g_originalReplaceActiveStage)
        g_originalReplaceActiveStage(stageManager, newStage);
}

static void __fastcall hkResetMaterials(uint64_t model)
{
    if (!model || !g_originalResetMaterials) {
        return;
    }

    if (g_isResimulating && IsLiveCharacterModel(model)) {
        // Defer live fighter/weapon material validation instead of replaying
        // renderer releases and clones during resimulation.
        MarkMaterialRepairPending(model);
        return;
    }

    const uint32_t count = *(uint32_t*)(model + 0x118);
    const uint64_t entries = *(uint64_t*)(model + 0x110);
    if (count != 0 && (count > 4096 || !entries)) {
        *(uint32_t*)(model + 0x118) = 0;
        return;
    }

    uint32_t invalid = 0;
    for (uint32_t index = 0; index < count; ++index) {
        uint64_t* entry = (uint64_t*)(entries +
                                      (size_t)index * sizeof(uint64_t));
        const bool validMaterial = IsValidCachedMaterial(*entry);
        if (*entry && !validMaterial) {
            // A failed release-slot check does not prove recycling. Clear the
            // suspect pointer so native reset rebuilds it without calling release.
            *entry = 0;
            ++invalid;
        }
    }
    Rollback_FreezeWatchdogMaterial(model, count, invalid,
                                    g_isResimulating ? g_resimulateCurrentFrame : -1);
    if (invalid != 0) {
        // Other slots may alias stale objects. Discard all entries before
        // native rebuild; avoiding uncertain releases can leak valid materials.
        memset((void*)entries, 0, (size_t)count * sizeof(uint64_t));
    }
    g_originalResetMaterials(model);
    Rollback_FreezeWatchdogHeartbeat("material-reset-complete",
                                     g_isResimulating ? g_resimulateCurrentFrame : -1);

    if (!g_isResimulating) {
        const size_t pending = FindPendingMaterialRepair(model);
        if (pending != SIZE_MAX && IsMaterialCacheReady(model))
            ClearPendingMaterialRepair(pending);
    }
}

static bool RepairPendingMaterialCache(uint64_t model)
{
    const size_t pending = FindPendingMaterialRepair(model);
    if (pending == SIZE_MAX)
        return true;

    if (!IsMaterialCacheReady(model)) {
        hkResetMaterials(model);
    }
    if (!IsMaterialCacheReady(model))
        return false;

    const size_t remaining = FindPendingMaterialRepair(model);
    if (remaining != SIZE_MAX)
        ClearPendingMaterialRepair(remaining);
    return true;
}

static void __fastcall hkBaseModelRenderSetup(uint64_t model,
                                               uint64_t renderContext)
{
    if (!model || !g_originalBaseModelRenderSetup)
        return;

    if (!RepairPendingMaterialCache(model)) {
        return;
    }
    g_originalBaseModelRenderSetup(model, renderContext);
}

static void __fastcall hkToonMaterialUpdate(uint64_t model)
{
    if (!model || !g_originalToonMaterialUpdate)
        return;

    if (!RepairPendingMaterialCache(model)) {
        return;
    }
    g_originalToonMaterialUpdate(model);
}

static void __fastcall hkParticleGeneratorModelUpdate(uint64_t generator)
{
    // Keep particle-model updates on the live timeline. Native also changes
    // flags and can close a particle, so this skip is not side-effect-free.
    if (!g_isResimulating && g_originalParticleGeneratorModelUpdate)
        g_originalParticleGeneratorModelUpdate(generator);
}

static void __fastcall hkParticleGeneratorFilterUpdate(uint64_t generator)
{
    // Skip the native filter-mode dispatch during headless resimulation.
    if (g_isResimulating)
        return;
    g_originalParticleGeneratorFilterUpdate(generator);
}

static void* WriteDefaultJointPosition(void* output)
{
    if (output)
        memcpy(output, (void*)_addr(0x140D2AAB0), 0x10);
    return output;
}

static void* __fastcall hkGetJointPos(uint64_t object, void* output)
{
    // Historical name: 0x140549F50 reads a material base-map factor, not a joint.
    // Invalid resimulation entries use the native default (1, 1, 1, 1).
    if (!g_isResimulating)
        return g_originalGetJointPos
            ? g_originalGetJointPos(object, output)
            : WriteDefaultJointPosition(output);

    if (!object || !output || !g_originalGetJointPos) {
        return WriteDefaultJointPosition(output);
    }

    const uint64_t owner = *(uint64_t*)(object + 0x08);
    if (!owner)
        return g_originalGetJointPos(object, output);
    const uint64_t model = *(uint64_t*)(owner + 0x40);
    if (!model)
        return g_originalGetJointPos(object, output);

    const uint32_t count = *(uint32_t*)(model + 0x118);
    const uint64_t entries = *(uint64_t*)(model + 0x110);
    if (count > 4096 || (count && !entries)) {
        return WriteDefaultJointPosition(output);
    }
    for (uint32_t i = 0; i < count; ++i) {
        const uint64_t entry =
            *(uint64_t*)(entries + (size_t)i * sizeof(uint64_t));
        if (!entry || !HasVirtualMethod(entry, 0x48))
            return WriteDefaultJointPosition(output);
    }
    return g_originalGetJointPos(object, output);
}

static void __fastcall hkCharacterModelPostMove(uint64_t character)
{
    // The thunk runs uToonModel material updates, then virtual +0x230.
    // Keep both on the live timeline; the indirect call is not proven lighting-only.
    if (!g_isResimulating && g_originalCharacterModelPostMove)
        g_originalCharacterModelPostMove(character);
}

static void __fastcall hkModelBoundsFromJoints(uint64_t model)
{
    if (g_isResimulating && model) {
        for (size_t i = 0; i < g_deferredModelBoundsCount; ++i) {
            if (g_deferredModelBounds[i].model == model) {
                g_deferredModelBounds[i].vtable = *(uint64_t*)model;
                return;
            }
        }
        if (g_deferredModelBoundsCount < DEFERRED_MODEL_BOUNDS_CAPACITY) {
            g_deferredModelBounds[g_deferredModelBoundsCount++] = {
                model, *(uint64_t*)model
            };
            return;
        }
    }

    RollbackHookProfileScope profile(
        RollbackHookProfileId::ModelBoundsFromJoints);
    if (g_originalModelBoundsFromJoints)
        g_originalModelBoundsFromJoints(model);
}

void Rollback_ClearDeferredModelBounds()
{
    g_deferredModelBoundsCount = 0;
}

void Rollback_FlushDeferredModelBounds()
{
    const size_t count = g_deferredModelBoundsCount;
    g_deferredModelBoundsCount = 0;
    for (size_t i = 0; i < count; ++i) {
        const DeferredModelBounds& entry = g_deferredModelBounds[i];
        if (!entry.model || !entry.vtable ||
            *(uint64_t*)entry.model != entry.vtable ||
            ((*(uint32_t*)(entry.model + 0x10) & 7u) - 1u) >= 2u) {
            continue;
        }
        hkModelBoundsFromJoints(entry.model);
    }
}

static bool HasInvalidMtAnimTable(uint64_t animation)
{
    if (!animation) return true;

    const uint32_t count = *(uint32_t*)(animation + 0x68);
    const uint64_t entries = *(uint64_t*)(animation + 0x60);
    if (count == 0) return false;
    if (count > 4096 || !entries) {
        return true;
    }

    for (uint32_t index = 0; index < count; ++index) {
        const uint64_t entry = *(uint64_t*)(entries +
                                            (size_t)index * sizeof(uint64_t));
        if (!entry) return true;
        const uint64_t vtable = *(uint64_t*)entry;
        if (!vtable) return true;
    }
    return false;
}

static uint32_t __fastcall hkMtAnimUpdateFrame(uint64_t animation,
                                                uint64_t resource,
                                                uint64_t name)
{
    // Native resolves a named unit index, returning UINT_MAX on a miss.
    // Use that sentinel for missing inputs or a malformed scheduler work array.
    if (!resource || !name || HasInvalidMtAnimTable(animation)) {
        return 0xffffffffu;
    }
    return g_originalMtAnimUpdateFrame(animation, resource, name);
}

static bool __fastcall hkIsOnlineBattle(uint64_t battleSetting)
{
    if (Netplay_IsActive()) {
        uintptr_t caller = (uintptr_t)__builtin_return_address(0);
        for (uintptr_t address : ONLINE_CALLER_RETURNS) {
            if (caller == (uintptr_t)_addr(address))
                return true;
        }
    }

    return g_originalIsOnlineBattle && g_originalIsOnlineBattle(battleSetting);
}

void Rollback_InitOnlineBattleHook()
{
    MH_STATUS status = GameplayHooks_Create(
        (LPVOID)_addr(FN_IS_ONLINE_BATTLE),
        (LPVOID)&hkIsOnlineBattle,
        (LPVOID*)&g_originalIsOnlineBattle);
    if (status != MH_OK) {
        HookLog("Netplay: failed to hook g__isOnlineBattle (%d)\n", status);
        return;
    }

    status = GameplayHooks_QueueEnable((LPVOID)_addr(FN_IS_ONLINE_BATTLE));
    if (status == MH_OK)
        HookLog("Netplay: enabled scoped native online combat behavior\n");
    else
        HookLog("Netplay: failed to enable g__isOnlineBattle hook (%d)\n", status);
}

template <size_t N>
static bool PatchBytes(uintptr_t address, const uint8_t (&expected)[N],
                       const uint8_t (&replacement)[N])
{
    uint8_t* target = (uint8_t*)_addr(address);
    if (memcmp(target, replacement, N) == 0)
        return true;
    if (memcmp(target, expected, N) != 0)
        return false;

    DWORD oldProtect = 0;
    if (!VirtualProtect(target, N, PAGE_EXECUTE_READWRITE, &oldProtect))
        return false;
    GameplayHooks_RecordPatch(target, N);
    memcpy(target, replacement, N);
    FlushInstructionCache(GetCurrentProcess(), target, N);
    DWORD ignored = 0;
    VirtualProtect(target, N, oldProtect, &ignored);
    return true;
}

bool Rollback_PatchGameConfigRefresh()
{
    // Skip per-frame native session/name maintenance, retaining both dead-object
    // checks and the original epilogue. Explicit title/Guide refreshes stay intact.
    static constexpr uint8_t CHECK_SESSION[] = {0x83, 0xBB, 0x44, 0x04, 0, 0, 0};
    static constexpr uint8_t SKIP_SESSION[] = {0xE9, 0x26, 0, 0, 0, 0x90, 0x90};
    static constexpr uint8_t LOAD_NAME[] = {0x48, 0x8D, 0xB3, 0x38, 0x03, 0, 0};
    static constexpr uint8_t SKIP_NAME[] = {0xE9, 0x97, 0, 0, 0, 0x90, 0x90};
    const bool patched =
        PatchBytes(0x1402534BD, CHECK_SESSION, SKIP_SESSION) &&
        PatchBytes(0x140253508, LOAD_NAME, SKIP_NAME);
    HookLog("GameConfig: per-frame session/Steam name refresh %s\n",
            patched ? "disabled" : "patch failed");
    return patched;
}

bool Rollback_PatchHitSolverAlwaysInline()
{
    // Force native in-place coalesce/sort instead of the two-bucket worker path.
    // This selects the same path in forward simulation and resimulation.
    static constexpr uint8_t SELECT_WORKER_OR_INLINE[] = {
        0x0F, 0x84, 0xB5, 0x01, 0x00, 0x00, // JZ 0x140011BFD
    };
    static constexpr uint8_t ALWAYS_INLINE[] = {
        0xE9, 0xB6, 0x01, 0x00, 0x00, 0x90, // JMP 0x140011BFD; NOP
    };

    const bool patched = PatchBytes(0x140011A42, SELECT_WORKER_OR_INLINE,
                                    ALWAYS_INLINE);
    HookLog("Rollback: HitSolver always-inline patch %s\n",
            patched ? "installed" : "failed");
    return patched;
}

void Rollback_PatchBattleUiAllocationFailure()
{
    // Native failure branches still reach unit setup with null allocations.
    // Redirect each to its loop increment or block exit.
    static constexpr uint8_t NULL_GAUGE_LIFE[] = {0x74, 0x0F};
    static constexpr uint8_t SKIP_GAUGE_LIFE[] = {0x74, 0x4B};
    static constexpr uint8_t NULL_GAUGE_HC[] = {0x74, 0x0F};
    static constexpr uint8_t SKIP_GAUGE_HC[] = {0x74, 0x4B};
    static constexpr uint8_t NULL_BATTLE_TIME[] = {0x74, 0x0D};
    static constexpr uint8_t SKIP_BATTLE_TIME[] = {0x74, 0x49};
    static constexpr uint8_t NULL_CARD_ICON[] = {0x74, 0x0D};
    static constexpr uint8_t SKIP_CARD_ICON[] = {0x74, 0x3C};
    static constexpr uint8_t NULL_KEY_DISPLAY[] = {0x74, 0x13};
    static constexpr uint8_t SKIP_KEY_DISPLAY[] = {0x74, 0x42};
    static constexpr uint8_t NULL_SCORE[] = {0x74, 0x0D};
    static constexpr uint8_t SKIP_SCORE[] = {0x74, 0x33};
    static constexpr uint8_t NULL_ROOM_WATCH[] = {0x74, 0x0D};
    static constexpr uint8_t SKIP_ROOM_WATCH[] = {0x74, 0x3C};
    static constexpr uint8_t NULL_AMBIENT_LIGHT[] = {0x74, 0x0B};
    static constexpr uint8_t RETURN_NO_AMBIENT_LIGHT[] = {0x74, 0x48};

    const bool battleUi =
        PatchBytes(0x1402D2A32, NULL_GAUGE_LIFE, SKIP_GAUGE_LIFE) &&
        PatchBytes(0x1402D2AA2, NULL_GAUGE_HC, SKIP_GAUGE_HC) &&
        PatchBytes(0x1402D2B19, NULL_BATTLE_TIME, SKIP_BATTLE_TIME) &&
        PatchBytes(0x1402D2BAA, NULL_CARD_ICON, SKIP_CARD_ICON) &&
        PatchBytes(0x1402D2C79, NULL_KEY_DISPLAY, SKIP_KEY_DISPLAY) &&
        PatchBytes(0x1402D2EB1, NULL_SCORE, SKIP_SCORE) &&
        PatchBytes(0x1402D2F09, NULL_ROOM_WATCH, SKIP_ROOM_WATCH) &&
        PatchBytes(0x1402D3146, NULL_AMBIENT_LIGHT,
                   RETURN_NO_AMBIENT_LIGHT);
    HookLog("Rollback: %s null uBattleUi allocation guards\n",
            battleUi ? "installed" : "partially installed");

    // Skip failed hit-model allocations before native adds/initializes the unit.
    static constexpr uint8_t NULL_HIT_MODEL_FIRST[] = {0x74, 0x0F};
    static constexpr uint8_t SKIP_NULL_HIT_MODEL_FIRST[] = {0x74, 0x59};
    static constexpr uint8_t NULL_HIT_MODEL_SECOND[] = {0x74, 0x12};
    static constexpr uint8_t SKIP_NULL_HIT_MODEL_SECOND[] = {0x74, 0x69};
    static constexpr uint8_t NULL_HIT_MODEL_THIRD[] = {0x74, 0x0F};
    static constexpr uint8_t SKIP_NULL_HIT_MODEL_THIRD[] = {0x74, 0x59};
    const bool first = PatchBytes(0x1402D88F3, NULL_HIT_MODEL_FIRST,
                                  SKIP_NULL_HIT_MODEL_FIRST);
    const bool second = PatchBytes(0x1402D8972, NULL_HIT_MODEL_SECOND,
                                   SKIP_NULL_HIT_MODEL_SECOND);
    const bool third = PatchBytes(0x1402D8A42, NULL_HIT_MODEL_THIRD,
                                  SKIP_NULL_HIT_MODEL_THIRD);
    HookLog("Rollback: %s null uBattleUiHit model allocation guards\n",
            first && second && third ? "installed" : "partially installed");
}

void Rollback_InitNativeNullGuards()
{
    const MH_STATUS motionDtorStatus = GameplayHooks_Create(
        (LPVOID)_addr(FN_UMODEL_MOTION_DTOR), (LPVOID)&hkUModelMotionDtor,
        (LPVOID*)&g_originalUModelMotionDtor);
    if (motionDtorStatus == MH_OK) {
        const MH_STATUS enableStatus =
            GameplayHooks_QueueEnable((LPVOID)_addr(FN_UMODEL_MOTION_DTOR));
        HookLog("Rollback: %s stale uModel::Motion free guard\n",
                enableStatus == MH_OK ? "installed" : "failed to install");
    } else {
        HookLog("Rollback: failed to hook uModel::Motion destructor (%d)\n",
                motionDtorStatus);
    }

    const MH_STATUS modelBoundsStatus = GameplayHooks_Create(
        (LPVOID)_addr(0x140568F30), (LPVOID)&hkModelBoundsFromJoints,
        (LPVOID*)&g_originalModelBoundsFromJoints);
    if (modelBoundsStatus == MH_OK) {
        const MH_STATUS enableStatus =
            GameplayHooks_QueueEnable((LPVOID)_addr(0x140568F30));
        HookLog("Rollback: %s deferred model bounds filter\n",
                enableStatus == MH_OK ? "installed" : "failed to install");
    } else {
        HookLog("Rollback: failed to hook deferred model bounds filter (%d)\n",
                modelBoundsStatus);
    }

    const MH_STATUS characterModelStatus = GameplayHooks_Create(
        (LPVOID)_addr(0x1402021E0), (LPVOID)&hkCharacterModelPostMove,
        (LPVOID*)&g_originalCharacterModelPostMove);
    if (characterModelStatus == MH_OK &&
        GameplayHooks_QueueEnable((LPVOID)_addr(0x1402021E0)) == MH_OK) {
        HookLog("Rollback: installed headless character model post-move filter\n");
    }

    const MH_STATUS frameIndexStatus = GameplayHooks_Create(
        (LPVOID)_addr(0x1400F3150), (LPVOID)&hkMtAnimSetFrameIndex,
        (LPVOID*)&g_originalMtAnimSetFrameIndex);
    if (frameIndexStatus == MH_OK) {
        const MH_STATUS enableStatus = GameplayHooks_QueueEnable((LPVOID)_addr(0x1400F3150));
        HookLog("Rollback: %s null MtAnim frame-index guard\n",
                enableStatus == MH_OK ? "installed" : "failed to install");
    } else {
        HookLog("Rollback: failed to guard null MtAnim frame index (%d)\n",
                frameIndexStatus);
    }

    const MH_STATUS flagStatus = GameplayHooks_Create(
        (LPVOID)_addr(0x140002A70), (LPVOID)&hkMtObjectSetFlag,
        (LPVOID*)&g_originalMtObjectSetFlag);
    if (flagStatus != MH_OK) {
        HookLog("Rollback: failed to guard null MtObject flag target (%d)\n",
                flagStatus);
    } else {
        const MH_STATUS enableStatus = GameplayHooks_QueueEnable((LPVOID)_addr(0x140002A70));
        HookLog("Rollback: %s null MtObject flag target guard\n",
                enableStatus == MH_OK ? "installed" : "failed to install");
    }

    const MH_STATUS resetMaterialsStatus = GameplayHooks_Create(
        (LPVOID)_addr(0x14055FBC0), (LPVOID)&hkResetMaterials,
        (LPVOID*)&g_originalResetMaterials);
    if (resetMaterialsStatus == MH_OK) {
        const MH_STATUS enableStatus = GameplayHooks_QueueEnable((LPVOID)_addr(0x14055FBC0));
        HookLog("Rollback: %s stale material cache sanitizer\n",
                enableStatus == MH_OK ? "installed" : "failed to install");
    } else {
        HookLog("Rollback: failed to hook material cache reset (%d)\n",
                resetMaterialsStatus);
    }

    const MH_STATUS renderSetupStatus = GameplayHooks_Create(
        (LPVOID)_addr(0x14055FDC0), (LPVOID)&hkBaseModelRenderSetup,
        (LPVOID*)&g_originalBaseModelRenderSetup);
    if (renderSetupStatus == MH_OK &&
        GameplayHooks_QueueEnable((LPVOID)_addr(0x14055FDC0)) == MH_OK) {
        HookLog("Rollback: installed live material cache repair\n");
    }

    const MH_STATUS toonMaterialStatus = GameplayHooks_Create(
        (LPVOID)_addr(0x140564110), (LPVOID)&hkToonMaterialUpdate,
        (LPVOID*)&g_originalToonMaterialUpdate);
    if (toonMaterialStatus == MH_OK &&
        GameplayHooks_QueueEnable((LPVOID)_addr(0x140564110)) == MH_OK) {
        HookLog("Rollback: installed toon material cache repair\n");
    }

    const MH_STATUS particleModelStatus = GameplayHooks_Create(
        (LPVOID)_addr(0x140898730), (LPVOID)&hkParticleGeneratorModelUpdate,
        (LPVOID*)&g_originalParticleGeneratorModelUpdate);
    if (particleModelStatus == MH_OK &&
        GameplayHooks_QueueEnable((LPVOID)_addr(0x140898730)) == MH_OK) {
        HookLog("Rollback: installed headless particle-model update filter\n");
    }

    MH_STATUS particleFilterStatus = GameplayHooks_Create(
        (LPVOID)_addr(FN_PARTICLE_FILTER_UPDATE),
        (LPVOID)&hkParticleGeneratorFilterUpdate,
        (LPVOID*)&g_originalParticleGeneratorFilterUpdate);
    if (particleFilterStatus == MH_OK)
        particleFilterStatus = GameplayHooks_QueueEnable((LPVOID)_addr(FN_PARTICLE_FILTER_UPDATE));
    if (particleFilterStatus == MH_OK) {
        HookLog("Rollback: installed headless Filter submission skip\n");
    } else {
        HookLog("Rollback: failed to install effect Filter submission skip (%d)\n",
                particleFilterStatus);
    }

    const MH_STATUS jointPosStatus = GameplayHooks_Create(
        (LPVOID)_addr(0x140549F50), (LPVOID)&hkGetJointPos,
        (LPVOID*)&g_originalGetJointPos);
    if (jointPosStatus == MH_OK &&
        GameplayHooks_QueueEnable((LPVOID)_addr(0x140549F50)) == MH_OK) {
        HookLog("Rollback: installed null model-joint entry guard\n");
    }

    const MH_STATUS animStatus = GameplayHooks_Create(
        (LPVOID)_addr(0x14050EE60), (LPVOID)&hkMtAnimUpdateFrame,
        (LPVOID*)&g_originalMtAnimUpdateFrame);
    if (animStatus != MH_OK) {
        HookLog("Rollback: failed to guard malformed MtAnim table (%d)\n",
                animStatus);
    } else {
        const MH_STATUS animEnableStatus = GameplayHooks_QueueEnable((LPVOID)_addr(0x14050EE60));
        HookLog("Rollback: %s malformed MtAnim table guard\n",
                animEnableStatus == MH_OK ? "installed" : "failed to install");
    }

    const MH_STATUS cutInAddStatus = GameplayHooks_Create(
        (LPVOID)_addr(0x1404490C0), (LPVOID)&hkCutInAddCharacter,
        (LPVOID*)&g_originalCutInAddCharacter);
    if (cutInAddStatus == MH_OK) {
        const MH_STATUS enableStatus = GameplayHooks_QueueEnable((LPVOID)_addr(0x1404490C0));
        HookLog("Rollback: %s null uCutIn participant guard\n",
                enableStatus == MH_OK ? "installed" : "failed to install");
    } else {
        HookLog("Rollback: failed to guard null uCutIn participant (%d)\n",
                cutInAddStatus);
    }

    const MH_STATUS cutInVariantStatus = GameplayHooks_Create(
        (LPVOID)_addr(0x14044B2D0), (LPVOID)&hkCutInSetVariant,
        (LPVOID*)&g_originalCutInSetVariant);
    if (cutInVariantStatus == MH_OK) {
        const MH_STATUS enableStatus = GameplayHooks_QueueEnable((LPVOID)_addr(0x14044B2D0));
        HookLog("Rollback: %s null uCutIn camera propagation guard\n",
                enableStatus == MH_OK ? "installed" : "failed to install");
    } else {
        HookLog("Rollback: failed to guard null uCutIn camera propagation (%d)\n",
                cutInVariantStatus);
    }

    const MH_STATUS copyPositionStatus = GameplayHooks_Create(
        (LPVOID)_addr(0x1400E3020), (LPVOID)&hkCopyCharacterPosition,
        (LPVOID*)&g_originalCopyCharacterPosition);
    if (copyPositionStatus == MH_OK) {
        const MH_STATUS enableStatus = GameplayHooks_QueueEnable((LPVOID)_addr(0x1400E3020));
        HookLog("Rollback: %s null cChr+0x6428 position guard\n",
                enableStatus == MH_OK ? "installed" : "failed to install");
    } else {
        HookLog("Rollback: failed to guard null cChr+0x6428 position (%d)\n",
                copyPositionStatus);
    }

    const MH_STATUS replaceStageStatus = GameplayHooks_Create(
        (LPVOID)_addr(0x1401F02D0), (LPVOID)&hkReplaceActiveStage,
        (LPVOID*)&g_originalReplaceActiveStage);
    if (replaceStageStatus == MH_OK) {
        const MH_STATUS enableStatus = GameplayHooks_QueueEnable((LPVOID)_addr(0x1401F02D0));
        HookLog("Rollback: %s invalid active-stage replacement guard\n",
                enableStatus == MH_OK ? "installed" : "failed to install");
    } else {
        HookLog("Rollback: failed to guard active-stage replacement (%d)\n",
                replaceStageStatus);
    }

}

bool Rollback_PatchNativeNetplayDeterminism()
{
    // Force aAction's online/replay fixed-timing and job-control setup.
    // Leave the global online predicate unchanged for unrelated native callers.
    static constexpr uintptr_t DETERMINISM_BRANCH = 0x14020385f;
    uint8_t* branch = (uint8_t*)_addr(DETERMINISM_BRANCH);
    if (branch[0] == 0xeb && branch[1] == 0x12)
        return true;
    if (branch[0] != 0x75 || branch[1] != 0x12) {
        HookLog("Netplay: native determinism branch has unexpected bytes %02X %02X\n",
                branch[0], branch[1]);
        return false;
    }

    DWORD oldProtect = 0;
    if (!VirtualProtect(branch, 1, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        HookLog("Netplay: failed to make native determinism branch writable\n");
        return false;
    }
    branch[0] = 0xeb; // JNZ -> JMP into the native online/replay setup block.
    FlushInstructionCache(GetCurrentProcess(), branch, 1);
    DWORD ignored = 0;
    VirtualProtect(branch, 1, oldProtect, &ignored);

    HookLog("Netplay: forced aAction native online determinism branch\n");
    return true;
}

void Rollback_DisableGameplayParallelism()
{
    // Apply the native online/replay job-control settings to all 64 unit lanes,
    // collision, HitSolver, and battle runtime. HitSolver still needs its inline
    // patch: setting its control byte to false does not disable worker dispatch.
    using SetMoveLineParallelFn = void(__fastcall*)(uint64_t, int, bool);
    using GetCollisionMoveMatrixFn = uint64_t(__fastcall*)(uint64_t);
    using GetInstanceFn = uint64_t(__fastcall*)();
    using SetBoolFn = void(__fastcall*)(uint64_t, bool);

    uint64_t sUnit = *(uint64_t*)_addr(0x140e17698);
    uint64_t sCollision = *(uint64_t*)_addr(0x140e18250);
    uint64_t sHitSolver = *(uint64_t*)_addr(0x140d44060);
    uint64_t battleRuntime = ((GetInstanceFn)_addr(0x140004730))();

    if (sUnit) {
        auto setMoveLineParallel = (SetMoveLineParallelFn)_addr(0x140207b10);
        for (int category = 0; category < 64; ++category)
            setMoveLineParallel(sUnit, category, false);
    }
    if (sCollision) {
        uint64_t moveMatrix = ((GetCollisionMoveMatrixFn)_addr(0x1402033d0))(sCollision);
        if (moveMatrix)
            ((SetBoolFn)_addr(0x1405b7bf0))(moveMatrix, false);
    }
    if (sHitSolver)
        ((SetBoolFn)_addr(0x140207660))(sHitSolver, false);
    if (battleRuntime)
        ((SetBoolFn)_addr(0x140207c30))(battleRuntime, false);

    HookLog("Rollback: disabled native gameplay parallelism\n");
}

void Rollback_ApplyNetplayDeterminism()
{
    // Apply fixed timing at the game-thread netplay boundary, not during startup.
    using SetFrameWaitFn = void(__fastcall*)(uint64_t, bool);
    uint64_t sMain = *(uint64_t*)_addr(0x140e177e8);
    if (sMain)
        ((SetFrameWaitFn)_addr(0x140522420))(sMain, true);

    Rollback_DisableGameplayParallelism();
    HookLog("Netplay: enabled fixed frame timing\n");
}
