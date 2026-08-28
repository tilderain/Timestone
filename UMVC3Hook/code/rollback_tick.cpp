#include "../pch.h"

#include "rollback.h"
#include "rollback_profile.h"
#include "rollback_memory.h"
#include "rollback_diagnostics.h"
#include "rollback_tick.h"
#include "mk10utils.h"

#include <cstring>
#include <algorithm>
#include <array>
#include <utility>

extern void HookLog(const char* fmt, ...);

static constexpr uintptr_t FN_SMAIN_MOVE              = 0x140521df0;
static constexpr uintptr_t FN_UPDATE_WINDOW_FOCUS     = 0x140258540;
static constexpr uintptr_t FN_NETPAD_PROCESS_INPUT    = 0x140289c30;
static constexpr uintptr_t FN_COLLISION_BEGIN_FRAME   = 0x1405bb710;
static constexpr uintptr_t FN_MVC3MGR_GETINSTANCE     = 0x140001af0;
static constexpr uintptr_t FN_GET_GAME_CONFIG         = 0x1401bcc50;
static constexpr uintptr_t FN_RENDER_GETINSTANCE      = 0x1400047a0;
static constexpr uintptr_t FN_RENDER_SET_ASPECT       = 0x14053de60;
static constexpr uintptr_t FN_RENDER_GET_VIEWPORT     = 0x14020c7d0;
static constexpr uintptr_t FN_PRIMITIVE_GETINSTANCE   = 0x1401e0ca0;
static constexpr uintptr_t FN_PRIMITIVE_SET_WIDTH     = 0x140932790;
static constexpr uintptr_t FN_PRIMITIVE_SET_HEIGHT    = 0x140447110;
static constexpr uintptr_t FN_RENDER_BEGIN_FRAME      = 0x140537080;
static constexpr uintptr_t FN_MVC3MGR_GETACTION       = 0x1409629d0;
static constexpr uintptr_t FN_ACTION_SUBSTEP          = 0x14014f410;
static constexpr uintptr_t FN_GET_GAME_UI             = 0x14097d370;
static constexpr uintptr_t FN_GET_SHOT                = 0x140683730;
static constexpr uintptr_t FN_GET_GAME_EFFECT         = 0x140015690;
static constexpr uintptr_t FN_GET_HIT_SOLVER          = 0x140056130;
static constexpr uintptr_t FN_AI_SUBSTEP              = 0x140647910;
static constexpr uintptr_t FN_GET_CHARACTER           = 0x1407bcd80;
static constexpr uintptr_t FN_GET_STAGE               = 0x140259460;
static constexpr uintptr_t FN_RESET_HIT_NODES         = 0x140259e90;
static constexpr uintptr_t FN_GET_REPLAY              = 0x14013c430;
static constexpr uintptr_t FN_GET_GSOUND              = 0x140259270;
static constexpr uintptr_t FN_GET_BATTLE_SETTING      = 0x1408f1f10;
static constexpr uintptr_t FN_GET_MVC3_SETTING        = 0x14004b000;
static constexpr uintptr_t FN_GET_NETWORK             = 0x1407cda50;
static constexpr uintptr_t FN_GET_LICENSE             = 0x14010a200;
static constexpr uintptr_t FN_GET_RANKING             = 0x140703370;
static constexpr uintptr_t FN_GET_NETWORK_ROOM        = 0x14030e6c0;
static constexpr uintptr_t FN_GET_EARTH_ATTACK        = 0x140327a10;
static constexpr uintptr_t FN_UPDATE_RENDER_STATE     = 0x140259ea0;
static constexpr uintptr_t FN_RENDER_END_FRAME        = 0x14053a250;
static constexpr uintptr_t FN_PAD_GETINSTANCE         = 0x140001b00;
static constexpr uintptr_t FN_PAD_KEY_HELD            = 0x1402b3d00;
static constexpr uintptr_t FN_PAD_KEY_TRIGGERED       = 0x1402b3dc0;
static constexpr uintptr_t FN_SMAIN_GETINSTANCE       = 0x140003150;
static constexpr uintptr_t FN_SMAIN_EXIT              = 0x1405213a0;
static constexpr uintptr_t FN_SYNC_FENCE_BEGIN        = 0x14079a020;
static constexpr uintptr_t FN_SYNC_FENCE_END          = 0x14079a050;

static constexpr uint32_t SUNIT_CATEGORY_COUNT_OFFSET = 0xc38;
static constexpr uint32_t SUNIT_MOVE_CATEGORY_VTABLE  = 0x48;
static constexpr uint32_t SUNIT_SYNC_JOB_VTABLE       = 0x60;
static constexpr uint32_t SUNIT_SYNC_SAVE_VTABLE      = 0x50;
static constexpr uint32_t SUNIT_CATEGORY_LISTS_OFFSET = 0x50;
static constexpr uint32_t SUNIT_CATEGORY_LIST_STRIDE  = 0x30;
static constexpr uintptr_t FN_SUNIT_REMOVE_MULTI_MASK = 0x14051baf0;
static constexpr uintptr_t FN_SUNIT_REMOVE_UNIT       = 0x14051b9f0;
static constexpr uintptr_t FN_SMAIN_EXECUTE_JOB       = 0x140521230;
static constexpr uintptr_t FN_MTOBJECT_IS_ALIVE       = 0x140001b80;
static constexpr uintptr_t FN_UCHRCPU_RESET_MODE      = 0x140120a50;
static constexpr uintptr_t DTI_UCHRCHAIN              = 0x140D44920;
static constexpr uintptr_t DTI_UCHRCPU                = 0x140D463B0;

static bool IsHeadlessGameplayUnitCategory(uint32_t category)
{
    //Check doc/sunit_categories.md for info

    switch (category) {
        case 3:  //Camera, needed to prevent screen jitter
    
        //case 4:  //SystemEffect, uCutIn::uCutInCam, unsure if needed
    
        case 7:  // Character, shot, and game-effect preprocessing.
        case 8:  // Fighters and character-owned helpers.
        case 9:  // Character auxiliary units such as Morrigan shadows.
        case 10: // Projectiles.
        case 11: // Projectiles?
        case 12: // Projectiles?

        case 13: // Character chains and attack spaces; also contains some effects.
        case 18: // Character post-processing.
        case 21: // Character CPU state.
            return true;
        default:
            return false;
    }
}

static bool IsHeadlessGameplaySyncCategory(uint32_t category)
{
    // Keep sync filtering independent from move filtering so categories can be
    // omitted experimentally without suppressing their main gameplay update.
    switch (category) {
        case 3:  // Camera, needed to prevent screen jitter.
        case 7:  // Character, shot, and game-effect preprocessing.
        case 8:  // Fighters and character-owned helpers.
        case 9:  // Character auxiliary units such as Morrigan shadows.
        case 10: // Projectiles.
        case 11: // Data-selected projectile/gameplay lane.
        case 12: // Data-selected projectile/gameplay lane.
        case 13: // Character chains and attack spaces; also contains some effects.
        case 18: // Character post-processing.
        case 21: // Character CPU state.
            return true;
        default:
            return false;
    }
}

static bool IsHeadlessGameplayUnitObject(uint64_t dti)
{
    return dti != (uint64_t)_addr(DTI_UCHRCHAIN);
}

static constexpr uintptr_t SLEEP_CALL_FIXED  = 0x1405220b2;
static constexpr uintptr_t SLEEP_CALL_VAR    = 0x140521f93;
static constexpr uintptr_t PRESENT_CALL_SITE = 0x14053709c;
static constexpr uintptr_t SWAPCHAIN_CALL_A  = 0x1402598c4;
static constexpr uintptr_t SWAPCHAIN_CALL_B  = 0x1402598d1;
static constexpr uintptr_t SWAPCHAIN_CALL_C  = 0x140537113;

static uint8_t g_savedSleep[2][6] = {};
static uint8_t g_savedPresent[3] = {};
static uint8_t g_savedSwapchain[3][3] = {};
static bool g_sleepPatched = false;
static bool g_presentPatched = false;
static bool g_swapchainPatched = false;

using ObjectFn = void(__fastcall*)(uint64_t);
using GetterFn = uint64_t(__fastcall*)(uint64_t);

static void VTableCall(uint64_t object, uint32_t vtableOffset);

static const char* WatchdogPhaseName(RollbackUnitProfilePhase phase)
{
    switch (phase) {
    case RollbackUnitProfilePhase::Move: return "headless-unit-move";
    case RollbackUnitProfilePhase::SyncJob: return "headless-unit-sync-job";
    case RollbackUnitProfilePhase::SyncSave: return "headless-unit-sync-save";
    default: return "headless-unit-unknown";
    }
}

static bool RunDisabledCpuPostMove(uint64_t unit)
{
    if (*(uint32_t*)(unit + 0x6c) != 0) return false;

    auto isAlive = (bool(__fastcall*)(uint64_t))_addr(FN_MTOBJECT_IS_ALIVE);
    static constexpr uint32_t referenceOffsets[] = {0x58, 0x60, 0x1b08, 0x1ad0};
    for (uint32_t offset : referenceOffsets) {
        uint64_t& reference = *(uint64_t*)(unit + offset);
        if (reference && !isAlive(reference)) reference = 0;
    }

    // Invalid core bindings take the native teardown path at vtable +0x78.
    if (!*(uint64_t*)(unit + 0x40) || !*(uint64_t*)(unit + 0x48) ||
        !*(uint64_t*)(unit + 0x58) || !*(uint64_t*)(unit + 0x60)) {
        return false;
    }

    // Preserve the mode-transition reset, but omit busy-state and nearest-
    // threat work that only feeds the disabled CPU script path.
    ((ObjectFn)_addr(FN_UCHRCPU_RESET_MODE))(unit);
    return true;
}

static void __fastcall DisabledCpuPostMove(uint64_t unit)
{
    if (!RunDisabledCpuPostMove(unit)) VTableCall(unit, 0x48);
}

static void VTableCall(uint64_t object, uint32_t vtableOffset)
{
    uint64_t vtable = *(uint64_t*)object;
    (*(ObjectFn*)(vtable + vtableOffset))(object);
}

static void VTableCall(uint64_t object, uint32_t vtableOffset, int value)
{
    uint64_t vtable = *(uint64_t*)object;
    (*(void(__fastcall**)(uint64_t, int))(vtable + vtableOffset))(object, value);
}

static uint64_t MainMember(uint64_t sMain, uint32_t memberOffset)
{
    return *(uint64_t*)(sMain + memberOffset);
}

static uint64_t ManagerSubsystem(uintptr_t getterAddress)
{
    uint64_t manager = ((uint64_t(__fastcall*)())_addr(FN_MVC3MGR_GETINSTANCE))();
    return ((GetterFn)_addr(getterAddress))(manager);
}

template <size_t Category>
static void __fastcall ProfiledUnitObjectMove(uint64_t unit)
{
    const uint64_t dti = GetObjectDti(unit);
    const uint64_t start = Profile_NowEnabled();
    VTableCall(unit, 0x40);
    Profile_RecordUnitObject(RollbackUnitProfilePhase::Move, (uint32_t)Category,
                             dti, Profile_NowEnabled() - start);
}

template <RollbackUnitProfilePhase Phase, size_t Category, uint32_t VtableOffset>
static void __fastcall ProfiledUnitObjectPostMove(uint64_t unit)
{
    const uint64_t dti = GetObjectDti(unit);
    const uint64_t start = Profile_NowEnabled();
    if constexpr (Phase == RollbackUnitProfilePhase::SyncSave && Category == 21) {
        if (dti != (uint64_t)_addr(DTI_UCHRCPU) || !RunDisabledCpuPostMove(unit))
            VTableCall(unit, VtableOffset);
    } else {
        VTableCall(unit, VtableOffset);
    }
    Profile_RecordUnitObject(Phase, (uint32_t)Category, dti,
                             Profile_NowEnabled() - start);
}

template <size_t... Categories>
static constexpr auto MakeProfiledUnitObjectMoves(std::index_sequence<Categories...>)
{
    return std::array<ObjectFn, sizeof...(Categories)>{
        &ProfiledUnitObjectMove<Categories>...
    };
}

static constexpr auto PROFILED_UNIT_OBJECT_MOVES = MakeProfiledUnitObjectMoves(
    std::make_index_sequence<ROLLBACK_UNIT_CATEGORY_COUNT>{});

template <size_t... Categories>
static constexpr auto MakeProfiledUnitObjectSyncJobs(std::index_sequence<Categories...>)
{
    return std::array<ObjectFn, sizeof...(Categories)>{
        &ProfiledUnitObjectPostMove<RollbackUnitProfilePhase::SyncJob, Categories, 0x50>...
    };
}

template <size_t... Categories>
static constexpr auto MakeProfiledUnitObjectSyncSaves(std::index_sequence<Categories...>)
{
    return std::array<ObjectFn, sizeof...(Categories)>{
        &ProfiledUnitObjectPostMove<RollbackUnitProfilePhase::SyncSave, Categories, 0x48>...
    };
}

static constexpr auto PROFILED_UNIT_OBJECT_SYNC_JOBS = MakeProfiledUnitObjectSyncJobs(
    std::make_index_sequence<ROLLBACK_UNIT_CATEGORY_COUNT>{});
static constexpr auto PROFILED_UNIT_OBJECT_SYNC_SAVES = MakeProfiledUnitObjectSyncSaves(
    std::make_index_sequence<ROLLBACK_UNIT_CATEGORY_COUNT>{});

template <bool ProfilingEnabled>
static void HeadlessUnitCategoryMove(uint64_t sMain, uint64_t sUnit,
                                     uint32_t category)
{
    const uint64_t categoryBase = sUnit + SUNIT_CATEGORY_LISTS_OFFSET +
        SUNIT_CATEGORY_LIST_STRIDE * category;
    if ((*(uint32_t*)(categoryBase - 8) & 2u) != 0) return;

    uint64_t unit = *(uint64_t*)categoryBase;
    if (!unit) return;

    const bool useJobs = (*(uint8_t*)(categoryBase - 8) & 1u) != 0;
    VTableCall(sUnit, 0x78, (int)category);
    const uint64_t enabledMask = ~*(uint64_t*)(sUnit + 0x1848);
    const float delta = *(float*)(categoryBase + 0x10) * *(float*)(sMain + 0x80);

    while (unit) {
        const uint64_t next = *(uint64_t*)(unit + 0x18);
        const uint64_t dti = GetObjectDti(unit);
        if (!dti) {
            HookLog("Rollback: detached malformed unit category=%u unit=0x%llX "
                    "vtable=0x%llX\n", category, unit, *(uint64_t*)unit);
            uint32_t stateFlags = *(uint32_t*)(unit + 0x10);
            *(uint32_t*)(unit + 0x10) = (stateFlags & 0xfffffffcu) | 4u;
            ((void(__fastcall*)(uint64_t, uint64_t))
                _addr(FN_SUNIT_REMOVE_MULTI_MASK))(sUnit, unit);
            *(uint32_t*)(unit + 0x10) = stateFlags & 0xfffffff8u;
            ((void(__fastcall*)(uint64_t, uint64_t))
                _addr(FN_SUNIT_REMOVE_UNIT))(sUnit, unit);
            unit = next;
            continue;
        }
        if constexpr (ProfilingEnabled)
            Profile_RegisterUnitObjectType(RollbackUnitProfilePhase::Move, category, dti);
        if ((*(uint64_t*)(unit + 0x30) & enabledMask) == 0) {
            uint32_t stateFlags = *(uint32_t*)(unit + 0x10);
            *(float*)(unit + 0x28) = delta;
            const uint32_t state = stateFlags & 7u;
            if (state == 1) {
                *(uint32_t*)(unit + 0x10) = (stateFlags & 0xfffffffau) | 2u;
                VTableCall(unit, 0x28);
                stateFlags = *(uint32_t*)(unit + 0x10);
            } else if (state == 3) {
                *(uint32_t*)(unit + 0x10) = (stateFlags & 0xfffffffcu) | 4u;
                ((void(__fastcall*)(uint64_t, uint64_t))
                    _addr(FN_SUNIT_REMOVE_MULTI_MASK))(sUnit, unit);
                unit = next;
                continue;
            } else if (state == 4) {
                *(uint32_t*)(unit + 0x10) = stateFlags & 0xfffffff8u;
                ((void(__fastcall*)(uint64_t, uint64_t))
                    _addr(FN_SUNIT_REMOVE_UNIT))(sUnit, unit);
                const uint64_t vtable = *(uint64_t*)unit;
                ((void(__fastcall*)(uint64_t, int))(*(uint64_t*)vtable))(unit, 1);
                unit = next;
                continue;
            }

            if ((stateFlags & 7u) == 2 && (stateFlags & 0x400u) != 0 &&
                IsHeadlessGameplayUnitObject(dti)) {
                Rollback_FreezeWatchdogUnit("headless-unit-move", category,
                                            unit, dti, g_resimulateCurrentFrame);
                if (!useJobs) {
                    if constexpr (ProfilingEnabled)
                        PROFILED_UNIT_OBJECT_MOVES[category](unit);
                    else
                        VTableCall(unit, 0x40);
                } else {
                    const uint32_t job = *(uint32_t*)(sMain + 0x98);
                    *(uint32_t*)(sMain + 0x98) = job + 1;
                    *(uint64_t*)(sMain + 0xc8 + (uint64_t)job * 0x10) = unit;
                    *(uint64_t*)(sMain + 0xd0 + (uint64_t)job * 0x10) =
                        ProfilingEnabled
                            ? (uint64_t)PROFILED_UNIT_OBJECT_MOVES[category]
                            : *(uint64_t*)(*(uint64_t*)unit + 0x40);
                }
            }
        }
        unit = next;
    }

    if (useJobs) {
        ((void(__fastcall*)(uint64_t, int))_addr(FN_SMAIN_EXECUTE_JOB))(sMain, 0);
    }
}

template <bool ProfilingEnabled>
static void HeadlessUnitCategoryCleanup(uint64_t sMain, uint64_t sUnit,
                                        uint32_t category)
{
    const uint64_t categoryBase = sUnit + SUNIT_CATEGORY_LISTS_OFFSET +
        SUNIT_CATEGORY_LIST_STRIDE * category;
    if ((*(uint32_t*)(categoryBase - 8) & 2u) != 0) return;

    uint64_t unit = *(uint64_t*)categoryBase;
    if (!unit) return;

    VTableCall(sUnit, 0x78, (int)category);
    const uint64_t enabledMask = ~*(uint64_t*)(sUnit + 0x1848);
    const float delta = *(float*)(categoryBase + 0x10) * *(float*)(sMain + 0x80);
    while (unit) {
        const uint64_t next = *(uint64_t*)(unit + 0x18);
        if ((*(uint64_t*)(unit + 0x30) & enabledMask) == 0) {
            *(float*)(unit + 0x28) = delta;
            uint32_t stateFlags = *(uint32_t*)(unit + 0x10);
            const uint32_t state = stateFlags & 7u;
            if (state == 3 || state == 4) {
                uint64_t dti = 0;
                uint64_t start = 0;
                if constexpr (ProfilingEnabled) {
                    dti = GetObjectDti(unit);
                    Profile_RegisterUnitObjectType(
                        RollbackUnitProfilePhase::Move, category, dti);
                    start = Profile_NowEnabled();
                }
                if (state == 3) {
                    *(uint32_t*)(unit + 0x10) =
                        (stateFlags & 0xfffffffcu) | 4u;
                    ((void(__fastcall*)(uint64_t, uint64_t))
                        _addr(FN_SUNIT_REMOVE_MULTI_MASK))(sUnit, unit);
                } else {
                    *(uint32_t*)(unit + 0x10) = stateFlags & 0xfffffff8u;
                    ((void(__fastcall*)(uint64_t, uint64_t))
                        _addr(FN_SUNIT_REMOVE_UNIT))(sUnit, unit);
                    const uint64_t vtable = *(uint64_t*)unit;
                    ((void(__fastcall*)(uint64_t, int))(*(uint64_t*)vtable))(unit, 1);
                }
                if constexpr (ProfilingEnabled) {
                    Profile_RecordUnitObject(
                        RollbackUnitProfilePhase::Move, category, dti,
                        Profile_NowEnabled() - start);
                }
            }
        }
        unit = next;
    }
}


// Replicates the per-category late-pass body used by sUnit__load
// (0x14051BF10, phase 2) and sUnit__loadCategory (0x14051B6F0, phase 3).
template <bool ProfilingEnabled>
static void HeadlessUnitCategoryPostMove(uint64_t sMain, uint64_t sUnit,
                                         RollbackUnitProfilePhase phase,
                                         uint32_t category)
{
    const uint64_t categoryBase = sUnit + SUNIT_CATEGORY_LISTS_OFFSET +
        SUNIT_CATEGORY_LIST_STRIDE * category;
    const uint32_t categoryFlags = *(uint32_t*)(categoryBase - 8);
    if ((categoryFlags & 2u) != 0) return;

    uint64_t unit = *(uint64_t*)categoryBase;
    if (!unit) return;

    const bool useJobs = phase == RollbackUnitProfilePhase::SyncSave &&
        (categoryFlags & 1u) != 0;
    VTableCall(sUnit, 0x78, (int)category);
    const uint64_t enabledMask = ~*(uint64_t*)(sUnit + 0x1848);

    while (unit) {
        const uint64_t next = *(uint64_t*)(unit + 0x18);
        const uint64_t dti = GetObjectDti(unit);
        if constexpr (ProfilingEnabled)
            Profile_RegisterUnitObjectType(phase, category, dti);
        if ((*(uint64_t*)(unit + 0x30) & enabledMask) == 0) {
            uint32_t stateFlags = *(uint32_t*)(unit + 0x10);
            bool initialized = false;
            if ((stateFlags & 7u) == 1) {
                *(uint32_t*)(unit + 0x10) = (stateFlags & 0xfffffffau) | 2u;
                VTableCall(unit, 0x28);
                stateFlags = *(uint32_t*)(unit + 0x10);
                initialized = true;
            }

            if ((stateFlags & 7u) == 2 && (stateFlags & 0x400u) != 0 &&
                IsHeadlessGameplayUnitObject(dti)) {
                Rollback_FreezeWatchdogUnit(WatchdogPhaseName(phase), category,
                                            unit, dti, g_resimulateCurrentFrame);
                if (initialized) {
                    VTableCall(unit, 0x40);
                    if (phase == RollbackUnitProfilePhase::SyncSave)
                        VTableCall(unit, 0x50);
                }

                ObjectFn callback;
                if constexpr (ProfilingEnabled) {
                    callback = phase == RollbackUnitProfilePhase::SyncJob
                        ? PROFILED_UNIT_OBJECT_SYNC_JOBS[category]
                        : PROFILED_UNIT_OBJECT_SYNC_SAVES[category];
                } else {
                    const uint32_t offset = phase == RollbackUnitProfilePhase::SyncJob
                        ? 0x50 : 0x48;
                    callback = phase == RollbackUnitProfilePhase::SyncSave &&
                            category == 21 && dti == (uint64_t)_addr(DTI_UCHRCPU)
                        ? DisabledCpuPostMove
                        : *(ObjectFn*)(*(uint64_t*)unit + offset);
                }
                if (!useJobs) {
                    callback(unit);
                } else {
                    const uint32_t job = *(uint32_t*)(sMain + 0x98);
                    *(uint32_t*)(sMain + 0x98) = job + 1;
                    *(uint64_t*)(sMain + 0xc8 + (uint64_t)job * 0x10) = unit;
                    *(uint64_t*)(sMain + 0xd0 + (uint64_t)job * 0x10) =
                        (uint64_t)callback;
                }
            }
        }
        unit = next;
    }

    if (useJobs)
        ((void(__fastcall*)(uint64_t, int))_addr(FN_SMAIN_EXECUTE_JOB))(sMain, 0);
}

// Replicates the native sUnit phase wrapper: sUnit__save_jobwrap
// (0x14051BFF0, fence 2) and sUnit__save (0x14051B850, fence 3).
template <bool ProfilingEnabled>
static void HeadlessUnitPass(uint64_t sMain, uint64_t sUnit,
                             RollbackUnitProfilePhase phase, int fenceState,
                             uint32_t categoryVtableOffset)
{
    uint32_t syncPoint = UINT32_MAX;
    ((void(__fastcall*)(uint32_t*, int))_addr(FN_SYNC_FENCE_BEGIN))(
        &syncPoint, fenceState);

    if (*(int32_t*)(sMain + 0x78) != 0 || *(uint8_t*)(sMain + 0x7c) == 0) {
        const uint32_t categoryCount = std::min(
            *(uint32_t*)(sUnit + SUNIT_CATEGORY_COUNT_OFFSET),
            (uint32_t)ROLLBACK_UNIT_CATEGORY_COUNT);
        const uint64_t vtable = *(uint64_t*)sUnit;
        const auto categoryFn = *(void(__fastcall**)(uint64_t, int))(
            vtable + categoryVtableOffset);

        for (uint32_t category = 0; category < categoryCount; ++category) {
            const bool enabled = phase == RollbackUnitProfilePhase::Move
                ? IsHeadlessGameplayUnitCategory(category)
                : IsHeadlessGameplaySyncCategory(category);
            if (!enabled) {
                if (phase == RollbackUnitProfilePhase::Move) {
                    if constexpr (ProfilingEnabled) {
                        const uint64_t start = Profile_NowEnabled();
                        HeadlessUnitCategoryCleanup<true>(sMain, sUnit, category);
                        Profile_RecordUnitCategory(
                            phase, category, Profile_NowEnabled() - start);
                    } else {
                        HeadlessUnitCategoryCleanup<false>(sMain, sUnit, category);
                    }
                }
                continue;
            }
            if constexpr (ProfilingEnabled) {
                const uint64_t start = Profile_NowEnabled();
                if (phase == RollbackUnitProfilePhase::Move) {
                    HeadlessUnitCategoryMove<true>(sMain, sUnit, category);
                } else {
                    HeadlessUnitCategoryPostMove<true>(sMain, sUnit, phase, category);
                }
                Profile_RecordUnitCategory(phase, category,
                                           Profile_NowEnabled() - start);
            } else if (category == 13) {
                if (phase == RollbackUnitProfilePhase::Move) {
                    HeadlessUnitCategoryMove<false>(sMain, sUnit, category);
                } else {
                    HeadlessUnitCategoryPostMove<false>(sMain, sUnit, phase, category);
                }
            } else {
                categoryFn(sUnit, (int)category);
            }
        }
    }

    ((void(__fastcall*)(uint32_t*))_addr(FN_SYNC_FENCE_END))(&syncPoint);
}

void Rollback_ResyncMainTiming(uint64_t sMain)
{
    if (!sMain) return;

    LARGE_INTEGER qpc;
    QueryPerformanceCounter(&qpc);
    *(uint64_t*)(sMain + 0x40) = qpc.QuadPart;
    *(uint64_t*)(sMain + 0x40048) = qpc.QuadPart;
    *(uint64_t*)(sMain + 0x40078) = qpc.QuadPart;
    *(uint64_t*)(sMain + 0x70) = qpc.QuadPart;
    *(uint32_t*)(sMain + 0x38) = 16;
    *(float*)(sMain + 0x80) = 1.0f;
    *(uint64_t*)(sMain + 0x60) = 0;
    *(uint64_t*)(sMain + 0x68) = 0;
    *(uint32_t*)(sMain + 0x48) = 1;
}

void Rollback_PatchSleep(bool enable)
{
    return;
    static constexpr uintptr_t sites[] = {SLEEP_CALL_FIXED, SLEEP_CALL_VAR};
    if (enable == g_sleepPatched) return;

    for (int i = 0; i < 2; ++i) {
        DWORD old = 0;
        VirtualProtect((LPVOID)_addr(sites[i]), 6, PAGE_EXECUTE_READWRITE, &old);
        if (enable) {
            memcpy(g_savedSleep[i], (void*)_addr(sites[i]), 6);
            memset((void*)_addr(sites[i]), 0x90, 6);
        } else {
            memcpy((void*)_addr(sites[i]), g_savedSleep[i], 6);
        }
        VirtualProtect((LPVOID)_addr(sites[i]), 6, old, &old);
    }
    g_sleepPatched = enable;
}

void Rollback_PatchPresent(bool enable)
{
    return;
    if (enable == g_presentPatched) return;

    DWORD old = 0;
    VirtualProtect((LPVOID)_addr(PRESENT_CALL_SITE), 3, PAGE_EXECUTE_READWRITE, &old);
    if (enable) {
        memcpy(g_savedPresent, (void*)_addr(PRESENT_CALL_SITE), 3);
        memset((void*)_addr(PRESENT_CALL_SITE), 0x90, 3);
    } else {
        memcpy((void*)_addr(PRESENT_CALL_SITE), g_savedPresent, 3);
    }
    VirtualProtect((LPVOID)_addr(PRESENT_CALL_SITE), 3, old, &old);
    g_presentPatched = enable;
}

void Rollback_PatchSwapchain(bool enable)
{
    return;
    static constexpr uintptr_t sites[] = {
        SWAPCHAIN_CALL_A, SWAPCHAIN_CALL_B, SWAPCHAIN_CALL_C,
    };
    if (enable == g_swapchainPatched) return;

    for (int i = 0; i < 3; ++i) {
        DWORD old = 0;
        VirtualProtect((LPVOID)_addr(sites[i]), 3, PAGE_EXECUTE_READWRITE, &old);
        if (enable) {
            memcpy(g_savedSwapchain[i], (void*)_addr(sites[i]), 3);
            memset((void*)_addr(sites[i]), 0x90, 3);
        } else {
            memcpy((void*)_addr(sites[i]), g_savedSwapchain[i], 3);
        }
        VirtualProtect((LPVOID)_addr(sites[i]), 3, old, &old);
    }
    g_swapchainPatched = enable;
}

void Rollback_PatchDrawCalls(bool enable)
{
    return;
    static constexpr struct Site { uintptr_t address; int size; } sites[] = {
        {0x1402594d9, 3}, {0x1402594e6, 3}, {0x1402594f3, 3},
        {0x140259515, 3}, {0x140259568, 3}, {0x140259575, 3},
        {0x140259896, 3}, {0x140259525, 5}, {0x140259537, 5},
        {0x140259548, 5}, {0x140259559, 5}, {0x1402598ab, 3},
    };
    static uint8_t saved[sizeof(sites) / sizeof(sites[0])][6] = {};
    static bool patched = false;
    if (enable == patched) return;

    for (size_t i = 0; i < sizeof(sites) / sizeof(sites[0]); ++i) {
        DWORD old = 0;
        VirtualProtect((LPVOID)_addr(sites[i].address), sites[i].size,
                       PAGE_EXECUTE_READWRITE, &old);
        if (enable) {
            memcpy(saved[i], (void*)_addr(sites[i].address), sites[i].size);
            memset((void*)_addr(sites[i].address), 0x90, sites[i].size);
        } else {
            memcpy((void*)_addr(sites[i].address), saved[i], sites[i].size);
        }
        VirtualProtect((LPVOID)_addr(sites[i].address), sites[i].size, old, &old);
    }
    patched = enable;
}


// Replicates the gameplay portion of sMain__move (0x140521DF0) for rollback
// re-simulation, with rendering, pacing, and non-deterministic services removed.
void Rollback_HeadlessTick(uint64_t sMain)
{
    RollbackHeadlessProfileTimer profile;
    Rollback_FreezeWatchdogHeartbeat("headless-begin", g_resimulateCurrentFrame);

    // Characters cache this value as the identity of shots spawned this frame.
    // Native sMain derives it from local wall time, which is not peer-stable.
    *(uint32_t*)(sMain + 0x48) = (uint32_t)g_resimulateCurrentFrame;

    //((ObjectFn)_addr(FN_SMAIN_MOVE))(sMain); // Advance base timing, pause, and frame pacing state.
    //((ObjectFn)_addr(FN_UPDATE_WINDOW_FOCUS))(sMain); // Refresh iconified, foreground, and focus flags.
    //((ObjectFn)_addr(FN_NETPAD_PROCESS_INPUT))(MainMember(sMain, 0x401c0)); // Poll/process online pad input.
    //VTableCall(MainMember(sMain, 0x40260), 0x30); // mpPlatformUtil::move: service platform/async work.

        profile.Add(&RollbackProfile::headlessTimingInput,
        &RollbackProfile::headlessTimingInputMax);

    //VTableCall(MainMember(sMain, 0x401a8), 0x48); // mpResource::draw.
    //VTableCall(MainMember(sMain, 0x40198), 0x48); // mpTool::draw.
    //VTableCall(MainMember(sMain, 0x401d8), 0x48); // mpEffect::draw.
    ((ObjectFn)_addr(FN_COLLISION_BEGIN_FRAME))(MainMember(sMain, 0x401e0)); // Drain deferred collision state.
    //VTableCall(ManagerSubsystem(FN_GET_GAME_CONFIG), 0x48); // sGameConfig::draw.
        profile.Add(&RollbackProfile::headlessEarlyDrawCollision,
        &RollbackProfile::headlessEarlyDrawCollisionMax);

    // Copy the current render viewport dimensions into the primitive renderer.
    //uint64_t render = ((uint64_t(__fastcall*)())_addr(FN_RENDER_GETINSTANCE))();
    //((void(__fastcall*)(uint64_t, uint32_t))_addr(FN_RENDER_SET_ASPECT))(render, 2); // Select aspect mode 2.
    //uint32_t viewport[2];
    //render = ((uint64_t(__fastcall*)())_addr(FN_RENDER_GETINSTANCE))();
    //((void(__fastcall*)(uint64_t, uint32_t*))_addr(FN_RENDER_GET_VIEWPORT))(render, viewport); // Read width and height.
    //uint64_t primitive = ((uint64_t(__fastcall*)())_addr(FN_PRIMITIVE_GETINSTANCE))();
    //((void(__fastcall*)(uint64_t, uint32_t))_addr(FN_PRIMITIVE_SET_WIDTH))(primitive, viewport[0]); // Set field +0x40.
    //primitive = ((uint64_t(__fastcall*)())_addr(FN_PRIMITIVE_GETINSTANCE))();
    //((void(__fastcall*)(uint64_t, uint32_t))_addr(FN_PRIMITIVE_SET_HEIGHT))(primitive, viewport[1]); // Set field +0x44.

        profile.Add(&RollbackProfile::headlessViewportSetup,
        &RollbackProfile::headlessViewportSetupMax);

    //VTableCall(MainMember(sMain, 0x40270), 0x48); // mpPrimitive::draw.
    //VTableCall(MainMember(sMain, 0x40208), 0x48); // mpGpuParticle::draw.
    //((ObjectFn)_addr(FN_RENDER_BEGIN_FRAME))(MainMember(sMain, 0x40190)); // Begin/submit the render frame.
    Rollback_FreezeWatchdogHeartbeat("headless-area", g_resimulateCurrentFrame);
    VTableCall(MainMember(sMain, 0x40200), 0x30); // mpArea::move.

        profile.Add(&RollbackProfile::headlessRenderBeginArea,
        &RollbackProfile::headlessRenderBeginAreaMax);

    // sAction is registered as a gameplay unit and is dispatched by the
    // headless sUnit pass above. This helper is only the native action cleanup
    // pass; calling the fighting dispatcher here would advance action timers
    // twice.
    uint64_t manager = ((uint64_t(__fastcall*)())_addr(FN_MVC3MGR_GETINSTANCE))();
    uint64_t action = ((GetterFn)_addr(FN_MVC3MGR_GETACTION))(manager); // Resolve the active sAction.
    Rollback_FreezeWatchdogHeartbeat("headless-action", g_resimulateCurrentFrame);
    ((ObjectFn)_addr(FN_ACTION_SUBSTEP))(action); // Run the action substep and character moves.

        profile.Add(&RollbackProfile::headlessAction,
        &RollbackProfile::headlessActionMax);

    //VTableCall(MainMember(sMain, 0x40230), 0x30); // mpVibration::move.
    //VTableCall(MainMember(sMain, 0x401c0), 0x30); // mpNetPad::move.
    //VTableCall(MainMember(sMain, 0x40210), 0x30); // mpMouse::move.
    //VTableCall(MainMember(sMain, 0x40228), 0x30); // mpId::move: advance unique-ID allocator state.
    //VTableCall(MainMember(sMain, 0x40218), 0x30); // mpKeyboard::move: update held/triggered/released maps.
    //VTableCall(MainMember(sMain, 0x401c0), 0x50); // mpNetPad::postFrame.
    
        profile.Add(&RollbackProfile::headlessInputDevices,
        &RollbackProfile::headlessInputDevicesMax);

    //VTableCall(MainMember(sMain, 0x401e8), 0x30); // mpShader2::move.
    //VTableCall(MainMember(sMain, 0x40240), 0x30); // mpAI::move.
    //VTableCall(ManagerSubsystem(FN_GET_GAME_CONFIG), 0x30); // sGameConfig::move.
    //VTableCall(ManagerSubsystem(FN_GET_GAME_UI), 0x30); // sGameUi::move.
    //VTableCall(ManagerSubsystem(FN_GET_SHOT), 0x48); // sShot::draw.
    //VTableCall(ManagerSubsystem(FN_GET_GAME_EFFECT), 0x48); // sGameEffect::draw.

        profile.Add(&RollbackProfile::headlessGameplaySetup,
        &RollbackProfile::headlessGameplaySetupMax);

    const uint64_t sUnit = MainMember(sMain, 0x401a0);
    Rollback_FreezeWatchdogHeartbeat("headless-unit-move-pass", g_resimulateCurrentFrame);
        HeadlessUnitPass<ROLLBACK_PROFILING_ENABLED>(
        sMain, sUnit, RollbackUnitProfilePhase::Move, 1,
        SUNIT_MOVE_CATEGORY_VTABLE);

        profile.Add(&RollbackProfile::headlessUnitMove,
        &RollbackProfile::headlessUnitMoveMax);

    Rollback_FreezeWatchdogHeartbeat("headless-collision", g_resimulateCurrentFrame);
    {
        RollbackHookProfileScope collisionProfile(RollbackHookProfileId::CollisionMove);
        VTableCall(MainMember(sMain, 0x401e0), 0x30); // mpCollision base cSystem::move.
    }
    const uint64_t hitSolver = ManagerSubsystem(FN_GET_HIT_SOLVER);
    if constexpr (ROLLBACK_PROFILING_ENABLED) {
        const uint32_t inputNodes = *(uint32_t*)(hitSolver + 0x40);
        g_activeRollbackProfile->hitSolverInputNodes += inputNodes;
        g_activeRollbackProfile->hitSolverInputNodesMax =
            std::max(g_activeRollbackProfile->hitSolverInputNodesMax, inputNodes);
        ++g_activeRollbackProfile->hitSolverFrames;
    }
    {
        RollbackHookProfileScope hitSolverProfile(RollbackHookProfileId::HitSolverMove);
        VTableCall(hitSolver, 0x30); // sHitSolver::move.
    }
    if constexpr (ROLLBACK_PROFILING_ENABLED) {
        const uint32_t outputNodes = *(uint32_t*)(hitSolver + 0x40);
        g_activeRollbackProfile->hitSolverOutputNodes += outputNodes;
        g_activeRollbackProfile->hitSolverOutputNodesMax =
            std::max(g_activeRollbackProfile->hitSolverOutputNodesMax, outputNodes);
    }

        profile.Add(&RollbackProfile::headlessCollisionHit,
        &RollbackProfile::headlessCollisionHitMax);

    // sUnit vtable slot 13 (0x68): sUnit__save_jobwrap. Begins sync phase 2,
    // iterates every active category, and dispatches that category through
    // sUnit vtable slot 12. The category/node pass performs the per-node
    // save/load transition work and synchronizes any queued unit jobs.

    //VTableCall(MainMember(sMain, 0x401a0), 0x68); //REQUIRED
    Rollback_FreezeWatchdogHeartbeat("headless-sync-job-pass", g_resimulateCurrentFrame);
    HeadlessUnitPass<ROLLBACK_PROFILING_ENABLED>(
        sMain, sUnit, RollbackUnitProfilePhase::SyncJob, 2,
        SUNIT_SYNC_JOB_VTABLE); // REQUIRED
    // sUnit vtable slot 11 (0x58): sUnit__save. Begins sync phase 3 and
    // iterates every active category through sUnit vtable slot 10. This is
    // the normal non-job-wrapper category synchronization pass.
    
    //VTableCall(MainMember(sMain, 0x401a0), 0x58); //REQUIRED
    Rollback_FreezeWatchdogHeartbeat("headless-sync-save-pass", g_resimulateCurrentFrame);
    HeadlessUnitPass<ROLLBACK_PROFILING_ENABLED>(
        sMain, sUnit, RollbackUnitProfilePhase::SyncSave, 3,
        SUNIT_SYNC_SAVE_VTABLE); // REQUIRED

        profile.Add(&RollbackProfile::headlessUnitSync,
        &RollbackProfile::headlessUnitSyncMax);

    //((ObjectFn)_addr(FN_AI_SUBSTEP))(MainMember(sMain, 0x40240)); // Run the AI substep helper.
    //VTableCall(MainMember(sMain, 0x401d8), 0x30); // mpEffect::move.
    //VTableCall(MainMember(sMain, 0x40208), 0x30); // mpGpuParticle::move.
    VTableCall(MainMember(sMain, 0x401b0), 0x30); // mpCamera::move.     ////Check if this is needed and the perf cost
    //VTableCall(MainMember(sMain, 0x401f0), 0x30); // mpSound::move.
    //VTableCall(MainMember(sMain, 0x40248), 0x30); // mpScene::move.
    //VTableCall(MainMember(sMain, 0x40258), 0x30); // mpShadow::move.

        profile.Add(&RollbackProfile::headlessSimulationTail,
        &RollbackProfile::headlessSimulationTailMax);

    Rollback_FreezeWatchdogHeartbeat("headless-manager-tail", g_resimulateCurrentFrame);
    manager = ((uint64_t(__fastcall*)())_addr(FN_MVC3MGR_GETINSTANCE))();
    VTableCall(manager, 0x30); // sMvc3Manager::move.
    VTableCall(ManagerSubsystem(FN_GET_CHARACTER), 0x30); // sCharacter::move.
    //VTableCall(ManagerSubsystem(FN_GET_STAGE), 0x30); // sStage::move.
    VTableCall(ManagerSubsystem(FN_GET_SHOT), 0x30); // sShot::move.
    VTableCall(ManagerSubsystem(FN_GET_GAME_EFFECT), 0x30); // sGameEffect::move. sGameEffect__tickAndPruneDead at 0x140130EA0??
    ((ObjectFn)_addr(FN_RESET_HIT_NODES))(ManagerSubsystem(FN_GET_HIT_SOLVER)); // Clear active hit-solver nodes.
    //VTableCall(ManagerSubsystem(FN_GET_REPLAY), 0x30); // sReplay::move.
    //VTableCall(ManagerSubsystem(FN_GET_GSOUND), 0x30); // sGSound::move.
    //VTableCall(ManagerSubsystem(FN_GET_BATTLE_SETTING), 0x30); // sBattleSetting::move.
    //VTableCall(ManagerSubsystem(FN_GET_MVC3_SETTING), 0x30); // sMvc3Setting::move.

        profile.Add(&RollbackProfile::headlessManagerCore,
        &RollbackProfile::headlessManagerCoreMax);

    //VTableCall(ManagerSubsystem(FN_GET_NETWORK), 0x30); // Manager-owned sNetwork::move.
    //VTableCall(ManagerSubsystem(FN_GET_LICENSE), 0x30); // sLicense::move.
    //VTableCall(MainMember(sMain, 0x401b8), 0x30); // mpNetwork::move.
    //VTableCall(MainMember(sMain, 0x401c8), 0x30); // mpAgent::move.
    //VTableCall(MainMember(sMain, 0x401d0), 0x30); // mpGuide::move.
    //VTableCall(ManagerSubsystem(FN_GET_RANKING), 0x30); // sRanking::move.
    //VTableCall(ManagerSubsystem(FN_GET_NETWORK_ROOM), 0x30); // sNetworkRoom::move.
    //VTableCall(ManagerSubsystem(FN_GET_EARTH_ATTACK), 0x30); // sEarthAttack::move.
    //VTableCall(MainMember(sMain, 0x40198), 0x30); // mpTool::move.

        profile.Add(&RollbackProfile::headlessNetworkServices,
        &RollbackProfile::headlessNetworkServicesMax);

    //VTableCall(MainMember(sMain, 0x40270), 0x30); // mpPrimitive::move.
    //VTableCall(MainMember(sMain, 0x401e0), 0x48); // mpCollision base cSystem::draw.
    //((ObjectFn)_addr(FN_UPDATE_RENDER_STATE))(sMain); // Update sMvc3Main render options:- Resolution- FullScreen- AltAntiAlias- TextureDetail- EffectVolume
    //VTableCall(MainMember(sMain, 0x40190), 0x30); // mpRender::move.
    //((ObjectFn)_addr(FN_RENDER_END_FRAME))(MainMember(sMain, 0x40190)); // End and present the render frame.

        profile.Add(&RollbackProfile::headlessRender,
        &RollbackProfile::headlessRenderMax);

    //VTableCall(MainMember(sMain, 0x40208), 0x50); // mpGpuParticle::postFrame.
    //VTableCall(MainMember(sMain, 0x40270), 0x50); // mpPrimitive::postFrame.
    //VTableCall(MainMember(sMain, 0x401f8), 0x30); // mpCapture::move.

        profile.Add(&RollbackProfile::headlessPostFrame,
        &RollbackProfile::headlessPostFrameMax);

    // Alt+F4 bypasses the ordinary sPad keyboard-input suppression gates.
    //using KeyQueryFn = bool(__fastcall*)(uint64_t, uint32_t, bool);
    //auto getPad = (uint64_t(__fastcall*)())_addr(FN_PAD_GETINSTANCE);
    //bool leftAlt = ((KeyQueryFn)_addr(FN_PAD_KEY_HELD))(getPad(), 0xa4, true); // Query VK_LMENU held.
    //bool rightAlt = ((KeyQueryFn)_addr(FN_PAD_KEY_HELD))(getPad(), 0xa5, true); // Query VK_RMENU held.
    //if (rightAlt || leftAlt) {
        //bool f4 = ((KeyQueryFn)_addr(FN_PAD_KEY_TRIGGERED))(getPad(), 0x73, true); // Query new VK_F4 press.
        //if (f4) {
          //  uint64_t main = ((uint64_t(__fastcall*)())_addr(FN_SMAIN_GETINSTANCE))();
            //((ObjectFn)_addr(FN_SMAIN_EXIT))(main); // Request orderly game shutdown.
        //}
    //}
        profile.Add(&RollbackProfile::headlessAltF4,
        &RollbackProfile::headlessAltF4Max);
        Rollback_FreezeWatchdogHeartbeat("headless-complete", g_resimulateCurrentFrame);
        profile.Finish();
}
