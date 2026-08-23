#include "../pch.h"

#include "rollback.h"
#include "rollback_memory.h"
#include "rollback_diagnostics.h"
#include "rollback_profile.h"
#include "mk10utils.h"
#include "netplay.h"
#include "../kiero/minhook/include/MinHook.h"

extern void HookLog(const char* fmt, ...);

static constexpr uintptr_t FN_IS_ONLINE_BATTLE = 0x140006AF0;
static constexpr uintptr_t FN_NDRAW_MATERIAL_RELEASE = 0x14076F760;
static constexpr uintptr_t VTABLE_MTOBJECT = 0x140A6A510;
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
    0x140203869, // aAction__init deterministic setup
};
using IsOnlineBattleFn = bool(__fastcall*)(uint64_t battleSetting);
static IsOnlineBattleFn g_originalIsOnlineBattle = nullptr;

using MtObjectSetFlagFn = void(__fastcall*)(uint64_t, uint8_t);
static MtObjectSetFlagFn g_originalMtObjectSetFlag = nullptr;
using MtObjectIsAliveFn = bool(__fastcall*)(uint64_t);
static MtObjectIsAliveFn g_originalMtObjectIsAlive = nullptr;
using RocketRaccoonPlaySfxFn = void(__fastcall*)(uint64_t);
static RocketRaccoonPlaySfxFn g_originalRocketRaccoonPlaySfx = nullptr;
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
using ParticleGeneratorModelUpdateFn = void(__fastcall*)(uint64_t);
static ParticleGeneratorModelUpdateFn g_originalParticleGeneratorModelUpdate = nullptr;
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
static PVOID g_materialCloneRecoveryHandler = nullptr;
static PVOID g_modelCommandAllocationRecoveryHandler = nullptr;
static PVOID g_schedulerAllocationRecoveryHandler = nullptr;
static PVOID g_resultScoreCursorRecoveryHandler = nullptr;
static uint64_t g_pendingMaterialRepairs[64] = {};
static size_t g_pendingMaterialRepairCount = 0;

static bool HasCallableVirtualMethod(uint64_t object, size_t offset);

static bool IsLiveCharacterModel(uint64_t model)
{
    const uint64_t sCharacter = *(uint64_t*)_addr(PTR_SCHARACTER);
    if (!model || !sCharacter)
        return false;

    for (int team = 0; team < 2; ++team) {
        uint64_t node = sCharacter + TEAM_OFFSETS[team] +
            TEAM_FIRST_NODE_OFFSET;
        for (int safety = 0; node && safety < 64; ++safety) {
            if (!IsAccessibleRange(node + NODE_CHARACTER_OFFSET,
                                   sizeof(uint64_t)) ||
                !IsAccessibleRange(node + NODE_NEXT_OFFSET,
                                   sizeof(uint64_t))) {
                break;
            }
            const uint64_t character =
                *(uint64_t*)(node + NODE_CHARACTER_OFFSET);
            if (character == model)
                return true;
            if (character && IsAccessibleRange(character + 0x3F38, 0x10)) {
                const uint32_t weaponCount = *(uint32_t*)(character + 0x3F38);
                const uint64_t weapons = *(uint64_t*)(character + 0x3F40);
                if (weapons && weaponCount <= 256 &&
                    IsAccessibleRange(weapons,
                        (size_t)weaponCount * sizeof(uint64_t))) {
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
    if (!material || !HasCallableVirtualMethod(material, 0x28))
        return false;
    const uint64_t vtable = *(uint64_t*)material;
    return *(uint64_t*)(vtable + 0x28) ==
        (uint64_t)_addr(FN_NDRAW_MATERIAL_RELEASE);
}

static bool IsMaterialCacheReady(uint64_t model)
{
    if (!model || !IsAccessibleRange(model + 0x100, 0x20))
        return false;
    const uint32_t count = *(uint32_t*)(model + 0x118);
    const uint64_t entries = *(uint64_t*)(model + 0x110);
    if (count == 0)
        return true;
    if (count > 4096 || !entries ||
        !IsAccessibleRange(entries, (size_t)count * sizeof(uint64_t))) {
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
    // Some restart-time UI units pass a null MtAnim frame here after their
    // resource lookup fails. There is no object state to update in that case.
    if (!object || !HasCallableDtiVirtual((void*)object) ||
        !g_originalMtObjectSetFlag) {
        return;
    }
    g_originalMtObjectSetFlag(object, enabled);
}

static bool __fastcall hkMtObjectIsAlive(uint64_t object)
{
    if ((uint64_t)__builtin_return_address(0) ==
        (uint64_t)_addr(0x14005629E)) {
        return g_originalMtObjectIsAlive(object);
    }
    // Native callers use this predicate as a vtable validity proof and then
    // dispatch another virtual method. A recycled pointer can still be mapped
    // and contain an alive-looking state word after its derived destructor has
    // replaced the vtable with the shorter MtObject base table.
    if (!object || !g_originalMtObjectIsAlive ||
        !IsAccessibleRange(object, sizeof(uint64_t))) {
        return false;
    }
    const uint64_t vtable = *(uint64_t*)object;
    if (vtable == (uint64_t)_addr(VTABLE_MTOBJECT)) {
        return false;
    }
    static thread_local uint64_t validatedVtables[256] = {};
    uint64_t& cached = validatedVtables[(vtable >> 3) & 0xFF];
    if (cached != vtable) {
        if (!IsAccessibleRange(vtable, 0x28)) return false;
        const uint64_t getDti = *(uint64_t*)(vtable + 0x20);
        if (!getDti || !IsExecutableAddress(getDti)) return false;
        cached = vtable;
    }
    return g_originalMtObjectIsAlive(object);
}

static void __fastcall hkRocketRaccoonPlaySfx(uint64_t character)
{
    // Native code treats isAlive as proof that these objects implement virtual
    // +0xD8. A retired child effect can retain state 1/2 after its derived
    // destructor has replaced the vtable with the shorter MtObject base table.
    static constexpr uint32_t effectOffsets[] = {0x68F8, 0x6900, 0x6930};
    for (uint32_t offset : effectOffsets) {
        uint64_t* slot = (uint64_t*)(character + offset);
        uint64_t object = *slot;
        if (!object || !IsAccessibleRange(object, sizeof(uint64_t))) continue;

        uint64_t vtable = *(uint64_t*)object;
        uint64_t callback = IsAccessibleRange(vtable + 0xD8, sizeof(uint64_t))
            ? *(uint64_t*)(vtable + 0xD8) : 0;
        if (!callback || !IsExecutableAddress(callback)) *slot = 0;
    }
    g_originalRocketRaccoonPlaySfx(character);
}

static void __fastcall hkMtAnimSetFrameIndex(uint64_t animation, uint32_t frame)
{
    // Several native UI setup routines call this after an allocation/constructor
    // failure. Keep the failed unit out of the animation helper without
    // changing valid frame-index writes.
    if (!animation || !IsAccessibleRange(animation + 0x10, sizeof(uint32_t)))
        return;
    g_originalMtAnimSetFrameIndex(animation, frame);
}

static void __fastcall hkCutInAddCharacter(uint64_t cutIn, uint64_t character)
{
    // The animation command creates a cut-in conditionally but calls this
    // helper unconditionally. A failed cut-in allocation has no participant
    // list to update.
    if (!cutIn || !character || !IsAccessibleRange(cutIn, 0x58) ||
        !IsAccessibleRange(character, sizeof(uint64_t)) ||
        !g_originalCutInAddCharacter) {
        return;
    }
    g_originalCutInAddCharacter(cutIn, character);
}

static void __fastcall hkCutInSetVariant(uint64_t cutIn, uint32_t variant)
{
    // The same failed cut-in path reaches the camera propagation call after
    // addCharacter. Leave the absent presentation object absent.
    if (!cutIn || !IsAccessibleRange(cutIn + 0x670, 0x130) ||
        !g_originalCutInSetVariant) {
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
        uint64_t source = 0;
        if (character <= UINT64_MAX - 0x6428 &&
            IsAccessibleRange(character + 0x6428, sizeof(uint64_t))) {
            source = *(uint64_t*)(character + 0x6428);
        }

        if (!source || source > UINT64_MAX - 0x5C ||
            !IsAccessibleRange(source + 0x50, sizeof(float) * 3)) {
            // FUN_1400E3020 has a separate stiff-damage side effect. Preserve
            // that scalar even when its optional position source was retired.
            if (damageCharacter && stiffDamage > 0.0f &&
                damageCharacter <= UINT64_MAX - 0x4154 &&
                IsAccessibleRange(damageCharacter + 0x4154, sizeof(float))) {
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

static LONG CALLBACK MaterialCloneRecoveryHandler(EXCEPTION_POINTERS* info)
{
    if (!info || !info->ExceptionRecord || !info->ContextRecord ||
        info->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION) {
        return EXCEPTION_CONTINUE_SEARCH;
    }

    CONTEXT* context = info->ContextRecord;
    if (context->Rip != (DWORD64)_addr(0x1405DCA7F) || context->Rax != 0 ||
        !IsAccessibleRange(context->Rsp + 0x48, sizeof(uint64_t))) {
        return EXCEPTION_CONTINUE_SEARCH;
    }

    // nDraw_Material__clone dereferences the result of its model clone before
    // returning. Its caller, uBaseModel__resetMaterials, already handles a
    // null clone through a fallback allocator. Resume at the clone epilogue so
    // that caller-side recovery can run instead of fabricating a model here.
    context->Rax = 0;
    context->Rdi = 0;
    context->Rip = (DWORD64)_addr(0x1405DCC8E);
    return EXCEPTION_CONTINUE_EXECUTION;
}
/// please check if this is necessary ////
static LONG CALLBACK ModelCommandAllocationRecoveryHandler(EXCEPTION_POINTERS* info)
{
    if (!info || !info->ExceptionRecord || !info->ContextRecord) {
        return EXCEPTION_CONTINUE_SEARCH;
    }

    const EXCEPTION_RECORD* exception = info->ExceptionRecord;
    CONTEXT* context = info->ContextRecord;
    if (exception->ExceptionCode != EXCEPTION_ACCESS_VIOLATION ||
        exception->NumberParameters < 2 ||
        exception->ExceptionInformation[0] != 1 ||
        exception->ExceptionInformation[1] != 0 ||
        context->Rbx > UINT64_MAX - 0x260 ||
        context->Rsp > UINT64_MAX - 0xB0 ||
        !IsAccessibleRange(context->Rbx + 0x28, 0x238) ||
        !IsAccessibleRange(context->Rsp + 0x60, 0x50)) {
        return EXCEPTION_CONTINUE_SEARCH;
    }

    if (context->Rip != (DWORD64)_addr(0x140640011) || context->Rcx != 0) {
        return EXCEPTION_CONTINUE_SEARCH;
    }

    const DWORD64 callerReturn = *(DWORD64*)(context->Rsp + 0x88);
    if (callerReturn != (DWORD64)_addr(0x140650A05)) {
        return EXCEPTION_CONTINUE_SEARCH;
    }

    // The render page allocator returned null while uModel__method_56C0 was
    // copying dirty command state. Restore FUN_14063f9b0's nonvolatile
    // registers and unwind it, then skip the caller's primitive submission.
    // Keep the model dirty so a later frame can retry after the page pool resets.
    *(uint64_t*)(context->Rbx + 0x28) = 0;
    *(uint64_t*)(context->Rbx + 0x30) = 0;
    *(uint64_t*)(context->Rbx + 0x258) = 0;
    *(uint32_t*)(context->Rbx + 0x130) |= 8;

    context->R14 = *(DWORD64*)(context->Rsp + 0x60);
    context->R15 = *(DWORD64*)(context->Rsp + 0x78);
    context->Rbx = *(DWORD64*)(context->Rsp + 0x80);
    context->Rsi = *(DWORD64*)(context->Rsp + 0xA0);
    context->Rdi = *(DWORD64*)(context->Rsp + 0xA8);
    context->Rsp += 0x90;
    context->Rax = 0;
    context->Rip = (DWORD64)_addr(0x140650BC5);
    return EXCEPTION_CONTINUE_EXECUTION;
}

static LONG CALLBACK SchedulerAllocationRecoveryHandler(EXCEPTION_POINTERS* info)
{
    if (!info || !info->ExceptionRecord || !info->ContextRecord) {
        return EXCEPTION_CONTINUE_SEARCH;
    }

    const EXCEPTION_RECORD* exception = info->ExceptionRecord;
    CONTEXT* context = info->ContextRecord;
    if (exception->ExceptionCode != EXCEPTION_ACCESS_VIOLATION ||
        exception->NumberParameters < 2 ||
        exception->ExceptionInformation[0] != 1 ||
        exception->ExceptionInformation[1] != 0 ||
        context->Rbx > UINT64_MAX - 0x70 ||
        !IsAccessibleRange(context->Rbx + 0x40, 0x30)) {
        return EXCEPTION_CONTINUE_SEARCH;
    }

    const DWORD64 firstEntriesWrite = (DWORD64)_addr(0x14050F0B5);
    const DWORD64 typeEntriesWrite = (DWORD64)_addr(0x14050F169);
    if (context->Rip == firstEntriesWrite && context->Rdx == 0) {
        // The native allocator returned null for count * 0x68 bytes. Publish an
        // empty scheduler and resume where RDI is restored before the epilogue.
        *(uint32_t*)(context->Rbx + 0x40) = 0;
        *(uint64_t*)(context->Rbx + 0x58) = 0;
        *(uint64_t*)(context->Rbx + 0x60) = 0;
        *(uint32_t*)(context->Rbx + 0x68) = 0;
        context->Rip = (DWORD64)_addr(0x14050F13A);
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    if (context->Rip == typeEntriesWrite && context->Rax == 0) {
        // The main entries remain valid; only the optional type-2 index failed.
        *(uint64_t*)(context->Rbx + 0x60) = 0;
        *(uint32_t*)(context->Rbx + 0x68) = 0;
        context->Rip = (DWORD64)_addr(0x14050F176);
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

static LONG CALLBACK ResultScoreCursorRecoveryHandler(EXCEPTION_POINTERS* info)
{
    if (!info || !info->ExceptionRecord || !info->ContextRecord) {
        return EXCEPTION_CONTINUE_SEARCH;
    }

    const EXCEPTION_RECORD* exception = info->ExceptionRecord;
    CONTEXT* context = info->ContextRecord;
    if (exception->ExceptionCode != EXCEPTION_ACCESS_VIOLATION ||
        exception->NumberParameters < 2 ||
        exception->ExceptionInformation[0] != 0 ||
        exception->ExceptionInformation[1] != 0 ||
        context->Rip != (DWORD64)_addr(0x14030A734) || context->Rcx != 0) {
        return EXCEPTION_CONTINUE_SEARCH;
    }

    // uBtResultScoreVersus setup stores a failed optional primitive lookup at
    // +0x248, then dispatches through it without a null check. Skip only that
    // cursor setup call and continue through the native epilogue.
    context->Rip = (DWORD64)_addr(0x14030A741);
    return EXCEPTION_CONTINUE_EXECUTION;
}

static bool HasCallableVirtualMethod(uint64_t object, size_t offset)
{
    if (!HasCallableDtiVirtual((void*)object)) return false;
    const uint64_t vtable = *(uint64_t*)object;
    if (vtable > UINT64_MAX - offset ||
        !IsAccessibleRange(vtable + offset, sizeof(uint64_t))) {
        return false;
    }
    return IsExecutableAddress(*(uint64_t*)(vtable + offset));
}

static void __fastcall hkReplaceActiveStage(uint64_t stageManager,
                                             uint64_t newStage)
{
    constexpr uint32_t ACTIVE_STAGES_OFFSET = 0x2060;
    constexpr uintptr_t FN_MTARRAY_GET_SIZE = 0x1400129C0;
    constexpr uintptr_t FN_MTARRAY_GET_ELEMENT = 0x140010E40;
    constexpr uintptr_t FN_MTARRAY_REMOVE_POINTER = 0x1400115D0;
    constexpr uintptr_t STAGE_LOCK = 0x140D4F728;

    if (stageManager &&
        IsAccessibleRange(stageManager + ACTIVE_STAGES_OFFSET, 0x20)) {
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
            if (oldStage && !HasCallableVirtualMethod(oldStage, 0xD8)) {
                HookLog("Rollback: removed invalid active stage entry "
                        "stage=0x%llX vtable=0x%llX\n",
                        oldStage,
                        IsAccessibleRange(oldStage, sizeof(uint64_t))
                            ? *(uint64_t*)oldStage : 0);
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
        // Material instances and their reference counts belong to the live
        // renderer. Replaying or batching action-change resets outside their
        // native order can release entries still referenced by another model.
        MarkMaterialRepairPending(model);
        return;
    }

    InvalidateMemoryQueryCache();
    if (!IsAccessibleRange(model, 0x120))
        return;

    const uint32_t count = *(uint32_t*)(model + 0x118);
    const uint64_t entries = *(uint64_t*)(model + 0x110);
    if (count != 0 && (count > 4096 || !entries ||
                       !IsAccessibleRange(entries,
                                           (size_t)count * sizeof(uint64_t)))) {
        *(uint32_t*)(model + 0x118) = 0;
        return;
    }

    uint32_t invalid = 0;
    for (uint32_t index = 0; index < count; ++index) {
        uint64_t* entry = (uint64_t*)(entries +
                                      (size_t)index * sizeof(uint64_t));
        const bool validMaterial = IsValidCachedMaterial(*entry);
        if (*entry && !validMaterial) {
            // The cached object has already been recycled into another MT
            // class. Native reset treats a null slot as missing and rebuilds
            // it from the current model resource without dispatching release
            // through the stale object's unrelated vtable.
            *entry = 0;
            ++invalid;
        }
    }
    Rollback_FreezeWatchdogMaterial(model, count, invalid,
                                    g_isResimulating ? g_resimulateCurrentFrame : -1);
    if (invalid != 0) {
        // A callable material vtable does not prove that another entry has not
        // already released the same allocation. Once any recycled member is
        // present, discard the complete derived cache and let native rebuild
        // it from the current resource without releasing stale aliases.
        memset((void*)entries, 0, (size_t)count * sizeof(uint64_t));
    }
    g_originalResetMaterials(model);
    Rollback_FreezeWatchdogHeartbeat("material-reset-complete",
                                     g_isResimulating ? g_resimulateCurrentFrame : -1);
    InvalidateMemoryQueryCache();

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

    InvalidateMemoryQueryCache();
    if (!IsMaterialCacheReady(model)) {
        hkResetMaterials(model);
        InvalidateMemoryQueryCache();
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
    // This virtual updates particle-model transforms and material bindings.
    // Its resources remain on the live presentation timeline and can have null
    // slots while a corrected effect timeline is replayed headlessly.
    if (!g_isResimulating && g_originalParticleGeneratorModelUpdate)
        g_originalParticleGeneratorModelUpdate(generator);
}

static void* WriteDefaultJointPosition(void* output)
{
    if (output)
        memcpy(output, (void*)_addr(0x140D2AAB0), 0x10);
    return output;
}

static void* __fastcall hkGetJointPos(uint64_t object, void* output)
{
    if (!g_isResimulating)
        return g_originalGetJointPos
            ? g_originalGetJointPos(object, output)
            : WriteDefaultJointPosition(output);

    if (!object || !output || !g_originalGetJointPos ||
        !IsAccessibleRange(object, 0x18)) {
        return WriteDefaultJointPosition(output);
    }

    const uint64_t owner = *(uint64_t*)(object + 0x08);
    if (!owner || !IsAccessibleRange(owner + 0x40, sizeof(uint64_t)))
        return g_originalGetJointPos(object, output);
    const uint64_t model = *(uint64_t*)(owner + 0x40);
    if (!model || !IsAccessibleRange(model + 0x110, 0x0C))
        return g_originalGetJointPos(object, output);

    const uint32_t count = *(uint32_t*)(model + 0x118);
    const uint64_t entries = *(uint64_t*)(model + 0x110);
    if (count > 4096 || (count && (!entries ||
        !IsAccessibleRange(entries, (size_t)count * sizeof(uint64_t))))) {
        return WriteDefaultJointPosition(output);
    }
    for (uint32_t i = 0; i < count; ++i) {
        const uint64_t entry =
            *(uint64_t*)(entries + (size_t)i * sizeof(uint64_t));
        if (!entry || !HasCallableVirtualMethod(entry, 0x48))
            return WriteDefaultJointPosition(output);
    }
    return g_originalGetJointPos(object, output);
}

static void __fastcall hkCharacterModelPostMove(uint64_t character)
{
    // uCharacter__moveAfter has completed its combat/status work before this
    // character-only thunk runs uToonModel material updates and cut-in light
    // propagation. Presentation resources stay on the live timeline.
    if (!g_isResimulating && g_originalCharacterModelPostMove)
        g_originalCharacterModelPostMove(character);
}

static void __fastcall hkModelBoundsFromJoints(uint64_t model)
{
    if (g_isResimulating && model &&
        IsAccessibleRange(model, sizeof(uint64_t))) {
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
            !IsAccessibleRange(entry.model, 0x220) ||
            *(uint64_t*)entry.model != entry.vtable ||
            ((*(uint32_t*)(entry.model + 0x10) & 7u) - 1u) >= 2u ||
            !HasCallableDtiVirtual((void*)entry.model)) {
            continue;
        }
        hkModelBoundsFromJoints(entry.model);
    }
}

static bool HasInvalidMtAnimTable(uint64_t animation)
{
    if (!IsAccessibleRange(animation, 0x70)) return true;

    const uint32_t count = *(uint32_t*)(animation + 0x68);
    const uint64_t entries = *(uint64_t*)(animation + 0x60);
    if (count == 0) return false;
    if (count > 4096 || !entries ||
        !IsAccessibleRange(entries, (size_t)count * sizeof(uint64_t))) {
        return true;
    }

    for (uint32_t index = 0; index < count; ++index) {
        const uint64_t entry = *(uint64_t*)(entries +
                                            (size_t)index * sizeof(uint64_t));
        if (!entry || !IsAccessibleRange(entry, sizeof(uint64_t))) return true;
        const uint64_t vtable = *(uint64_t*)entry;
        if (!vtable || !IsAccessibleAddress(vtable + 0x10)) return true;
    }
    return false;
}

static uint32_t __fastcall hkMtAnimUpdateFrame(uint64_t animation,
                                                uint64_t resource,
                                                uint64_t name)
{
    // Native returns UINT_MAX when no matching frame exists. Preserve that
    // result for a table containing a recycled/null animation entry instead of
    // dereferencing its invalid vtable.
    if (!resource || !name || !IsAccessibleRange(resource + 0x08, sizeof(uint64_t)) ||
        HasInvalidMtAnimTable(animation)) {
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
    MH_STATUS status = MH_CreateHook(
        (LPVOID)_addr(FN_IS_ONLINE_BATTLE),
        (LPVOID)&hkIsOnlineBattle,
        (LPVOID*)&g_originalIsOnlineBattle);
    if (status != MH_OK) {
        HookLog("Netplay: failed to hook g__isOnlineBattle (%d)\n", status);
        return;
    }

    status = MH_EnableHook((LPVOID)_addr(FN_IS_ONLINE_BATTLE));
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
    memcpy(target, replacement, N);
    FlushInstructionCache(GetCurrentProcess(), target, N);
    DWORD ignored = 0;
    VirtualProtect(target, N, oldProtect, &ignored);
    return true;
}

bool Rollback_PatchHitSolverAlwaysInline()
{
    // sHitSolver__move can send ordinary batches through two worker jobs.
    // Jump directly to its native in-place coalesce/sort path for every batch
    // so forward simulation and rollback use the same algorithm.
    // This changes spike performance of hitsolver from upwards of 3ms per frame, to 0.03ms per frame
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
    // uBattleUi__setup has the same allocation contract as uBattleUiHit: its
    // operator_new failure branches initialize a null unit and then continue
    // into sUnit/MtAnim/MtArray calls. Redirect each failed unit to the native
    // loop or block exit before those calls execute.
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

    // uBattleUiHit__setup checks model allocation, but its null branches still
    // add and initialize the null unit. Skip each failed iteration at its loop
    // increment instead of reaching MtAnim__setFrameIndex(NULL).
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
    const MH_STATUS modelBoundsStatus = MH_CreateHook(
        (LPVOID)_addr(0x140568F30), (LPVOID)&hkModelBoundsFromJoints,
        (LPVOID*)&g_originalModelBoundsFromJoints);
    if (modelBoundsStatus == MH_OK) {
        const MH_STATUS enableStatus =
            MH_EnableHook((LPVOID)_addr(0x140568F30));
        HookLog("Rollback: %s deferred model bounds filter\n",
                enableStatus == MH_OK ? "installed" : "failed to install");
    } else {
        HookLog("Rollback: failed to hook deferred model bounds filter (%d)\n",
                modelBoundsStatus);
    }

    const MH_STATUS characterModelStatus = MH_CreateHook(
        (LPVOID)_addr(0x1402021E0), (LPVOID)&hkCharacterModelPostMove,
        (LPVOID*)&g_originalCharacterModelPostMove);
    if (characterModelStatus == MH_OK &&
        MH_EnableHook((LPVOID)_addr(0x1402021E0)) == MH_OK) {
        HookLog("Rollback: installed headless character model post-move filter\n");
    }

    const MH_STATUS aliveStatus = MH_CreateHook(
        (LPVOID)_addr(0x140001B80), (LPVOID)&hkMtObjectIsAlive,
        (LPVOID*)&g_originalMtObjectIsAlive);
    if (aliveStatus == MH_OK) {
        const MH_STATUS enableStatus = MH_EnableHook((LPVOID)_addr(0x140001B80));
        HookLog("Rollback: %s stale MtObject alive-state guard\n",
                enableStatus == MH_OK ? "installed" : "failed to install");
    } else {
        HookLog("Rollback: failed to guard stale MtObject alive state (%d)\n",
                aliveStatus);
    }

    const MH_STATUS rocketEffectStatus = MH_CreateHook(
        (LPVOID)_addr(0x1400C56B0), (LPVOID)&hkRocketRaccoonPlaySfx,
        (LPVOID*)&g_originalRocketRaccoonPlaySfx);
    if (rocketEffectStatus == MH_OK) {
        const MH_STATUS enableStatus = MH_EnableHook((LPVOID)_addr(0x1400C56B0));
        HookLog("Rollback: %s stale Rocket Raccoon effect guard\n",
                enableStatus == MH_OK ? "installed" : "failed to install");
    } else {
        HookLog("Rollback: failed to guard stale Rocket Raccoon effects (%d)\n",
                rocketEffectStatus);
    }

    const MH_STATUS frameIndexStatus = MH_CreateHook(
        (LPVOID)_addr(0x1400F3150), (LPVOID)&hkMtAnimSetFrameIndex,
        (LPVOID*)&g_originalMtAnimSetFrameIndex);
    if (frameIndexStatus == MH_OK) {
        const MH_STATUS enableStatus = MH_EnableHook((LPVOID)_addr(0x1400F3150));
        HookLog("Rollback: %s null MtAnim frame-index guard\n",
                enableStatus == MH_OK ? "installed" : "failed to install");
    } else {
        HookLog("Rollback: failed to guard null MtAnim frame index (%d)\n",
                frameIndexStatus);
    }

    const MH_STATUS flagStatus = MH_CreateHook(
        (LPVOID)_addr(0x140002A70), (LPVOID)&hkMtObjectSetFlag,
        (LPVOID*)&g_originalMtObjectSetFlag);
    if (flagStatus != MH_OK) {
        HookLog("Rollback: failed to guard null MtObject flag target (%d)\n",
                flagStatus);
    } else {
        const MH_STATUS enableStatus = MH_EnableHook((LPVOID)_addr(0x140002A70));
        HookLog("Rollback: %s null MtObject flag target guard\n",
                enableStatus == MH_OK ? "installed" : "failed to install");
    }

    const MH_STATUS resetMaterialsStatus = MH_CreateHook(
        (LPVOID)_addr(0x14055FBC0), (LPVOID)&hkResetMaterials,
        (LPVOID*)&g_originalResetMaterials);
    if (resetMaterialsStatus == MH_OK) {
        const MH_STATUS enableStatus = MH_EnableHook((LPVOID)_addr(0x14055FBC0));
        HookLog("Rollback: %s stale material cache sanitizer\n",
                enableStatus == MH_OK ? "installed" : "failed to install");
    } else {
        HookLog("Rollback: failed to hook material cache reset (%d)\n",
                resetMaterialsStatus);
    }

    const MH_STATUS renderSetupStatus = MH_CreateHook(
        (LPVOID)_addr(0x14055FDC0), (LPVOID)&hkBaseModelRenderSetup,
        (LPVOID*)&g_originalBaseModelRenderSetup);
    if (renderSetupStatus == MH_OK &&
        MH_EnableHook((LPVOID)_addr(0x14055FDC0)) == MH_OK) {
        HookLog("Rollback: installed live material cache repair\n");
    }

    const MH_STATUS toonMaterialStatus = MH_CreateHook(
        (LPVOID)_addr(0x140564110), (LPVOID)&hkToonMaterialUpdate,
        (LPVOID*)&g_originalToonMaterialUpdate);
    if (toonMaterialStatus == MH_OK &&
        MH_EnableHook((LPVOID)_addr(0x140564110)) == MH_OK) {
        HookLog("Rollback: installed toon material cache repair\n");
    }

    const MH_STATUS particleModelStatus = MH_CreateHook(
        (LPVOID)_addr(0x140898730), (LPVOID)&hkParticleGeneratorModelUpdate,
        (LPVOID*)&g_originalParticleGeneratorModelUpdate);
    if (particleModelStatus == MH_OK &&
        MH_EnableHook((LPVOID)_addr(0x140898730)) == MH_OK) {
        HookLog("Rollback: installed headless particle-model update filter\n");
    }

    const MH_STATUS jointPosStatus = MH_CreateHook(
        (LPVOID)_addr(0x140549F50), (LPVOID)&hkGetJointPos,
        (LPVOID*)&g_originalGetJointPos);
    if (jointPosStatus == MH_OK &&
        MH_EnableHook((LPVOID)_addr(0x140549F50)) == MH_OK) {
        HookLog("Rollback: installed null model-joint entry guard\n");
    }

    const MH_STATUS animStatus = MH_CreateHook(
        (LPVOID)_addr(0x14050EE60), (LPVOID)&hkMtAnimUpdateFrame,
        (LPVOID*)&g_originalMtAnimUpdateFrame);
    if (animStatus != MH_OK) {
        HookLog("Rollback: failed to guard malformed MtAnim table (%d)\n",
                animStatus);
    } else {
        const MH_STATUS animEnableStatus = MH_EnableHook((LPVOID)_addr(0x14050EE60));
        HookLog("Rollback: %s malformed MtAnim table guard\n",
                animEnableStatus == MH_OK ? "installed" : "failed to install");
    }

    const MH_STATUS cutInAddStatus = MH_CreateHook(
        (LPVOID)_addr(0x1404490C0), (LPVOID)&hkCutInAddCharacter,
        (LPVOID*)&g_originalCutInAddCharacter);
    if (cutInAddStatus == MH_OK) {
        const MH_STATUS enableStatus = MH_EnableHook((LPVOID)_addr(0x1404490C0));
        HookLog("Rollback: %s null uCutIn participant guard\n",
                enableStatus == MH_OK ? "installed" : "failed to install");
    } else {
        HookLog("Rollback: failed to guard null uCutIn participant (%d)\n",
                cutInAddStatus);
    }

    const MH_STATUS cutInVariantStatus = MH_CreateHook(
        (LPVOID)_addr(0x14044B2D0), (LPVOID)&hkCutInSetVariant,
        (LPVOID*)&g_originalCutInSetVariant);
    if (cutInVariantStatus == MH_OK) {
        const MH_STATUS enableStatus = MH_EnableHook((LPVOID)_addr(0x14044B2D0));
        HookLog("Rollback: %s null uCutIn camera propagation guard\n",
                enableStatus == MH_OK ? "installed" : "failed to install");
    } else {
        HookLog("Rollback: failed to guard null uCutIn camera propagation (%d)\n",
                cutInVariantStatus);
    }

    const MH_STATUS copyPositionStatus = MH_CreateHook(
        (LPVOID)_addr(0x1400E3020), (LPVOID)&hkCopyCharacterPosition,
        (LPVOID*)&g_originalCopyCharacterPosition);
    if (copyPositionStatus == MH_OK) {
        const MH_STATUS enableStatus = MH_EnableHook((LPVOID)_addr(0x1400E3020));
        HookLog("Rollback: %s null cChr+0x6428 position guard\n",
                enableStatus == MH_OK ? "installed" : "failed to install");
    } else {
        HookLog("Rollback: failed to guard null cChr+0x6428 position (%d)\n",
                copyPositionStatus);
    }

    const MH_STATUS replaceStageStatus = MH_CreateHook(
        (LPVOID)_addr(0x1401F02D0), (LPVOID)&hkReplaceActiveStage,
        (LPVOID*)&g_originalReplaceActiveStage);
    if (replaceStageStatus == MH_OK) {
        const MH_STATUS enableStatus = MH_EnableHook((LPVOID)_addr(0x1401F02D0));
        HookLog("Rollback: %s invalid active-stage replacement guard\n",
                enableStatus == MH_OK ? "installed" : "failed to install");
    } else {
        HookLog("Rollback: failed to guard active-stage replacement (%d)\n",
                replaceStageStatus);
    }

    if (!g_materialCloneRecoveryHandler) {
        g_materialCloneRecoveryHandler = AddVectoredExceptionHandler(
            1, &MaterialCloneRecoveryHandler);
        HookLog("Rollback: material clone recovery %s\n",
                 g_materialCloneRecoveryHandler ? "installed" : "failed to install");
    }

    if (!g_modelCommandAllocationRecoveryHandler) {
        static constexpr uint8_t EXPECTED_NULL_COMMAND_WRITE[] = {
            0x48, 0x89, 0x01,
        };
        const void* fault = (const void*)_addr(0x140640011);
        if (memcmp(fault, EXPECTED_NULL_COMMAND_WRITE,
                   sizeof(EXPECTED_NULL_COMMAND_WRITE)) == 0) {
            g_modelCommandAllocationRecoveryHandler = AddVectoredExceptionHandler(
                1, &ModelCommandAllocationRecoveryHandler);
        }
        HookLog("Rollback: model command allocation recovery %s\n",
                g_modelCommandAllocationRecoveryHandler
                    ? "installed" : "failed to install");
    }

    if (!g_schedulerAllocationRecoveryHandler) {
        g_schedulerAllocationRecoveryHandler = AddVectoredExceptionHandler(
            1, &SchedulerAllocationRecoveryHandler);
        HookLog("Rollback: scheduler allocation recovery %s\n",
                g_schedulerAllocationRecoveryHandler
                     ? "installed" : "failed to install");
    }

    if (!g_resultScoreCursorRecoveryHandler) {
        static constexpr uint8_t EXPECTED_CURSOR_DEREFERENCE[] = {
            0x48, 0x8B, 0x01,
        };
        const void* fault = (const void*)_addr(0x14030A734);
        if (memcmp(fault, EXPECTED_CURSOR_DEREFERENCE,
                   sizeof(EXPECTED_CURSOR_DEREFERENCE)) == 0) {
            g_resultScoreCursorRecoveryHandler = AddVectoredExceptionHandler(
                1, &ResultScoreCursorRecoveryHandler);
        }
        HookLog("Rollback: result score cursor recovery %s\n",
                 g_resultScoreCursorRecoveryHandler
                     ? "installed" : "failed to install");
    }

}

bool Rollback_PatchNativeNetplayDeterminism()
{
    // aAction__init normally reaches its serialization setup only when replay
    // playback or g__isOnlineBattle is true. Custom ENet play intentionally
    // keeps an offline BattleType, so force only this branch rather than the
    // global online predicate, whose other callers expect native netplay state.
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

void Rollback_ApplyNetplayDeterminism()
{
    // Called from HookedMove on the game thread. Several native setters below
    // rewrite timing or subsystem state and are unsafe during parallel startup.
    using SetFrameWaitFn = void(__fastcall*)(uint64_t, bool);
    using SetMoveLineParallelFn = void(__fastcall*)(uint64_t, int, bool);
    using GetCollisionMoveMatrixFn = uint64_t(__fastcall*)(uint64_t);
    using GetInstanceFn = uint64_t(__fastcall*)();
    using SetBoolFn = void(__fastcall*)(uint64_t, bool);

    uint64_t sMain = *(uint64_t*)_addr(0x140e177e8);
    uint64_t sUnit = *(uint64_t*)_addr(0x140e17698);
    uint64_t sCollision = *(uint64_t*)_addr(0x140e18250);
    uint64_t sHitSolver = *(uint64_t*)_addr(0x140d44060);
    uint64_t battleRuntime = ((GetInstanceFn)_addr(0x140004730))();

    if (sMain)
        ((SetFrameWaitFn)_addr(0x140522420))(sMain, true);
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

    HookLog("Netplay: applied native gameplay parallelism settings\n");
}
