// rollback.cpp
// Frame-indexed snapshot/restore for deterministic rollback netcode.
//
// Architecture:
//   A ring buffer holds up to ROLLBACK_WINDOW frame snapshots.  Each snapshot
//   is a flat memcpy dump of all MUST-snapshot regions followed by per-region
//   metadata (address + size) so that restore uses the EXACT addresses that
//   were saved — avoiding the stale-pointer problem with dynamically allocated
//   objects (projectiles, effects).
//
//   SaveSnapshot() is called at the frame boundary BEFORE the game tick.
//   RestoreSnapshot(targetFrame) rewinds to that frame for re-simulation.
//   ReSimulate(start, end) chains restore + tick + save for each frame.
//
//   Physics state (~11424 bytes) is embedded inside the sMain manager at
//   offset ~0x31A78.  Capturing the full sMain (0x40080 bytes) covers it.
//
//   Dynamic projectile/effect objects are traversed via intrusive linked
//   lists, with DTI-based sizing for each node.
//
// See doc/rollback.md for the full singleton catalog and per-field analysis.
#include "../pch.h"

#include "rollback.h"
#include "rollback_internal.h"
#include "rollback_cleanup.h"
#include "rollback_diagnostics.h"
#include "rollback_hooks.h"
#include "rollback_memory.h"
#include "rollback_profile.h"
#include "rollback_restore.h"
#include "rollback_tick.h"
#include "snapshot_ndjson.h"
#include "concrete_snapshot.h"
#include "controller.h"
#include "mk10utils.h"
#include "netplay.h"
#include "diag.h"
#include "../kiero/minhook/include/MinHook.h"
#include <cstring>
#include <cmath>
#include <new>
#include <cstdio>
#include <vector>
#include <algorithm>
#include <string>
#include <unordered_map>
#include <unordered_set>

extern void HookLog(const char* fmt, ...);
extern void HookLogSetFile(const char* filename);

// ===========================================================================
// Addresses — absolute VAs, converted at runtime via _addr()
// ===========================================================================
static constexpr uintptr_t PTR_SREPLAY      = 0x140d4d9d8;
static constexpr uintptr_t PTR_SCHARACTER   = 0x140d44a70;
static constexpr uintptr_t PTR_SSHOT        = 0x140d47f40;
static constexpr uintptr_t PTR_SHITSOLVER   = 0x140d44060;
static constexpr uintptr_t PTR_SACTION      = 0x140d47e68;
static constexpr uintptr_t PTR_SGAMEUI      = 0x140d53a18;
static constexpr uintptr_t PTR_SMAIN        = 0x140e177e8;
static constexpr uintptr_t PTR_SGAMEEFFECT  = 0x140d46470;
static constexpr uintptr_t PTR_SMVC3MGR     = 0x140d510a0;
static constexpr uintptr_t PTR_SUNIT        = 0x140e17698;
static constexpr uintptr_t PTR_SCAMERA      = 0x140e17930;
static constexpr uintptr_t PTR_SCOLLISION   = 0x140e18250;

// sMvc3Main__move — the full per-frame update pipeline
static constexpr uintptr_t FN_GAMEUI_SET_TEAM_CHARACTER_ID = 0x140022610;
static constexpr uintptr_t FN_SSHOT_SPAWN = 0x140150E00;
static constexpr uintptr_t FN_SSHOT_SPAWN_EX = 0x140150F40;
static constexpr uintptr_t FN_SSHOT_ENQUEUE_UNIT = 0x1401512A0;
static constexpr uintptr_t FN_SSHOT_FLUSH_PENDING_UNITS = 0x1401517A0;
static constexpr uintptr_t FN_USHOT_UPDATE_HITBOXES = 0x1401C9000;
static constexpr uintptr_t VTABLE_USHOT_VULCAN_ZIREITOU = 0x140AC0E20;
static constexpr uintptr_t FN_SUNIT_ADD_UNIT = 0x14051A6B0;
static constexpr uintptr_t FN_SUNIT_REMOVE_UNIT = 0x14051B9F0;
static constexpr uintptr_t FN_SUNIT_REMOVE_MULTI_MASK = 0x14051BAF0;
static constexpr uintptr_t FN_SUNIT_REGISTER_MULTI_MASK = 0x14051BDE0;
static constexpr uintptr_t FN_UGAMEEFFECT_CREATE = 0x140131410;
static constexpr uintptr_t FN_RNG_RANDOM_INT = 0x14014EB30;
static constexpr uintptr_t FN_HSIENKO_SELECT_ANKI = 0x1400A1260;
static constexpr uintptr_t FN_SET_RESERVED_ANKI = 0x1400D4D90;
static constexpr uintptr_t FN_BUILD_ATTACH_MATRIX = 0x14014F960;
static constexpr uintptr_t FN_KIRIGAKURE_EFFECT_UPDATE = 0x14013B050;
static constexpr uintptr_t FN_RESET_HITNODES = 0x140259E90;

// ===========================================================================
// Offsets & Struct Constants
// ===========================================================================
static constexpr uintptr_t SMVC3MGR_ACTION_OFFSET = 0x48;
static constexpr uint32_t SACTION_MODEL_JOINT_WRAPPER_OFFSET = 0x120;
static constexpr uint32_t SACTION_MODEL_JOINT_ALLOCATION_OFFSET = 0x70;
static constexpr uint32_t SACTION_MODEL_JOINT_DATA_OFFSET = 0x10;
static constexpr size_t SACTION_MODEL_JOINT_DATA_SIZE = 0x70;
static constexpr size_t SACTION_MODEL_JOINT_ALLOCATION_SIZE =
    SACTION_MODEL_JOINT_DATA_OFFSET + SACTION_MODEL_JOINT_DATA_SIZE;
static constexpr uintptr_t TEAM0_OFFSET           = 0x40;
static constexpr uintptr_t TEAM1_OFFSET           = 0x300;
static constexpr size_t    TEAM_SIZE              = 0x2C0;
static constexpr size_t    SCOLLISION_SIZE        = 0x239E0;
static constexpr size_t    SCOLLISION_BVH_COUNT   = 0x1F;
static constexpr size_t    SCOLLISION_NODE_SIZE   = 0x40;

static constexpr uintptr_t NODE_CHR_OFFSET        = 0x08;
static constexpr uintptr_t NODE_NEXT_OFFSET       = 0x10;
static constexpr uintptr_t TEAM_FIRST_NODE_OFFSET = 0x08;

// DTI object size is encoded at +0x30: lower 23 bits = (actual_size >> 2).
// Decode: ((*(uint32_t*)(dti + 0x30) & 0x7FFFFF) << 2).
// Do NOT read +0x34 — that field is the CRC32 class-name hash, not the size.
// IMPORTANT: The DTI pointer is NOT at object+0x08. Per MtObject__isKindOf, it's
// accessed via vtable[4] = *(void**)(*(uint64_t*)object + 0x20).
static constexpr size_t MAX_CHAR_SIZE        = 0x40000;
static constexpr size_t CCHR_NOMINAL_SIZE    = 0x6A60;
static constexpr size_t CCHR_MAX_SIZE        = 30000;
static constexpr uint32_t CCHR_NUM_JOINTS_OFFSET = 0x530;
static constexpr uint32_t CCHR_JOINT_STATE_PTR_OFFSET = 0x538;
static constexpr uint32_t CCHR_JOINT_INDEX_PTR_OFFSET = 0x548;
static constexpr uint32_t CCHR_MOTION_COUNT_OFFSET = 0x554;
static constexpr uint32_t CCHR_MOTION_BASE       = 0x560;
static constexpr uint32_t CCHR_MOTION_STRIDE     = 0x180;
static constexpr uint32_t CCHR_BLEND_PTR_OFFSET  = 0x160;
static constexpr size_t CCHR_BLEND_BYTES_PER_JOINT = 104;
static constexpr uint32_t CCHR_MOTION_CHANNELS_PTR_OFFSET = 0x6D8;
static constexpr uint32_t CCHR_BLENDED_JOINT_PTR_OFFSET = 0x11B0;
static constexpr uint32_t CCHR_MOTION_CHANNEL_COUNT_OFFSET = 0x1238;
static constexpr size_t CCHR_MOTION_CHANNEL_STRIDE = 0x0C;
static constexpr size_t CCHR_JOINT_STATE_STRIDE = 0xC0;
static constexpr size_t CCHR_BLENDED_JOINT_STRIDE = 0x40;
static constexpr uint32_t CCHR_EXTERNAL_ANIM_OFFSETS[] = {
    0x4200, 0x4E10, 0x5520, 0x5C30
};
static constexpr size_t CCHR_EXTERNAL_ANIM_SIZE = 0xE0;
static constexpr uintptr_t UCHRCPU_VTABLE = 0x140A87B08;
static constexpr uint32_t UCHRCPU_VM_STATE_OFFSET = 0xD0;
static constexpr uint32_t UCHRCPU_VM_STATE_SIZE = 0x1580; // through +0x164f
static constexpr int CCHR_MOTION_COUNT = 8;
static constexpr uint32_t CCHR_DRAW_STATE_START = 0x80;
static constexpr uint32_t CCHR_DRAW_STATE_END   = CCHR_MOTION_BASE;
static constexpr uint32_t CCHR_MODEL_TAIL_START = 0x1160;
static constexpr uint32_t CCHR_MODEL_TAIL_END   = 0x1300;
static constexpr uint32_t SUNIT_CATEGORY_COUNT_OFFSET = 0xC38;
static constexpr uint32_t SUNIT_CATEGORY_LISTS_OFFSET = 0x50;
static constexpr uint32_t SUNIT_CATEGORY_LIST_STRIDE  = 0x30;
static constexpr uint32_t SUNIT_CHARACTER_CATEGORY    = 8;
static constexpr uint32_t SUNIT_MAX_CATEGORIES        = 64;
static constexpr uint32_t SUNIT_MULTI_MASK_COUNT_OFFSET = 0x1840;
static constexpr uint32_t SUNIT_MULTI_MASK_ARRAYS_OFFSET = 0xC50;
static constexpr uint32_t CUNIT_LIVE_STATE_OFFSETS[] = {0x10, 0x18, 0x20, 0x38};
static constexpr size_t SMAIN_SIZE = 0x40080;
static constexpr int SSHOT_CATEGORY = 10;

struct SnapshotRange {
    uint32_t offset;
    uint32_t size;
};

static constexpr SnapshotRange CCHR_SCALAR_RESTORE_RANGES[] = {
    // cUnit::getTimeScale reads +0x28 directly. The generic pointer-preserve
    // pass can mistake a pair of packed float values here for an address,
    // leaving a post-rollback hitstop/time-scale value live.
    {0x28, 0x08},
    {0x48, 0x38},    // parent flags and world position/orientation/scale
    // These pointer-free local/cached/world matrices are derived from the
    // restored position. Reapply them after preserving live model state so
    // root-level collision primitives do not read abandoned prediction data.
    {0x80, 0x100},
    {0xA8, 0x04},    // action event/frame progression counter
    // uBaseModel::PartsDisp is a 512-bit command/part availability bitset.
    // Character logic consumes these bits as attacks progress; Morrigan tests
    // bit 0 every frame before remapping attack commands. The broad live draw-
    // state preservation below must not retain the exhausted post-attack bits.
    {0x12C, 0x40},
    // uModel motion-evaluation aggregate state immediately before Motion[0].
    // +0x558/+0x55C are consumed/accumulated layer flags that select the
    // layered versus simple joint path. They must rewind with Motion[8].
    {0x550, 0x10},
    // Assist/DHC and action-state flags used by character transitions.
    {0x29E, 0x12},
    {0x1300, 0x08},  // character action-time coefficient and local step
    {0x1308, 0x40},  // action descriptor and actor animation state
    // cActorGameModelAnm is embedded at +0x1348. Keep its vtable live, but
    // rewind its script IDs, owner, command blocks, key and loop registers.
    {0x1350, 0x98},
    {0x14F0, 0x28},  // cChr gameplay flags, including mAnmF airborne bit
    // cChrStatus is embedded at +0x1530. Preserve its live vtable and
    // internal references, but restore max/current/start/red life scalars.
    {0x154C, 0x14},
    // cChrAbility contains six inline 0x80-byte Prim records at +0x1570.
    // Preserve each vtable at record +0x00 and the live sound cookie at +0x78,
    // but rewind the scalar gameplay payload at +0x08..+0x77.
    {0x1578, 0x70},
    {0x15F8, 0x70},
    {0x1678, 0x70},
    {0x16F8, 0x70},
    {0x1778, 0x70},
    {0x17F8, 0x70},
    {0x1870, 0x08},  // cChrAbility scalar tail before cAtkCtrl
    {0x1644, 0x04},  // class-opcode action frame counter
    // cAtkCtrl starts at +0x1878. Restore mpResource, mAtkInfoId, flags,
    // timers and the full scalar range through mAnmNo so multi-hit attacks
    // have correct attack definition, lifetime state and animation binding.
    // mpUnit at +0x1890 remains live (owner character reference).
    {0x1898, 0x30},  // stop before mTInfo at cAtkCtrl+0x50
    // Character/team/member identity is rewritten during death and tag swaps.
    // Restore it so pending swap logic resolves the saved roster member.
    {0x2D9C, 0x24},
    // MtXorShift state is four uint32 values. Restore it explicitly because
    // the generic 8-byte pointer heuristic can classify an aligned pair of
    // RNG words as a live pointer and otherwise retain the prediction state.
    {0x2D80, 0x10},
    // Rewind the complete success window for both embedded cChrCmd objects.
    // mpSucLine points into the separately restored line buffer, and the cached
    // outputs remain gameplay-relevant until native mSucDelay expiration.
    {0x28B8, 0x20}, // cChr+0x2738, cChrCmd+0x180..+0x19F
    {0x2AD0, 0x20}, // cChr+0x2950, cChrCmd+0x180..+0x19F
    {0x3F48, 0x18C}, // action requests and movement state before the Y-motion block
    // uZeroSh__positionUpdate_DE80/FUN_14004EF80 reads the cached Y reference
    // and vertical velocity from this block before deciding the landing/gravity
    // path. Keep it explicit so rollback cannot retain one prediction tick.
    {0x40D4, 0x4C},  // mMoveAux.y, mOtherYSpd, mYSpd and related Y-motion state
    {0x4120, 0xD8},  // remaining velocity/gravity and air counters
    // Combat counters block at +0x4168 contains hit tracking, air action limits,
    // block timing windows, hitstun state and super armor. Essential for multi-hit
    // attacks, air combo limits, and defensive state determinism.
    {0x4168, 0x90},
    // Inline per-primitive world-position cache. The hitbox records above
    // contain shape/local data, but collision setup also reads these sixteen
    // Vec3 entries when committing primitive positions. Keeping this live can
    // leave one sphere on the previous timeline and later compound into a
    // whole-model root-position offset.
    {0x4D10, 0x100},
    // Pending fighter-separation correction written by
    // uCharacter__applyCollisionPushCorrection. Its packed float bits can look
    // like an aligned pointer, so explicitly rewind it after the pointer guard.
    {0x6340, 0x10},
    // Pointer-free combat tail. +0x6360..+0x636f selects the current attack
    // data for each collision group before geometry is populated. It must be
    // rewound along with combo rates, last-hit state, damage, armor and guard
    // state through +0x6427. The MtArray header at +0x6350 stays live.
    {0x6360, 0xC8},
    // Pointer-free cChr tail containing reflected charge/stiffness scalars and
    // the model/attachment vector written before action dispatch at +0x68E0.
    {0x68D0, 0x20},
};

// sMain is almost entirely live worker infrastructure. Only timing state is
// rollback-relevant; callback queues, ring buffers, handles and worker records
// must remain on the current timeline.
static constexpr SnapshotRange SMAIN_RESTORE_RANGES[] = {
    {0x30, 0x64},       // init flag and primary timing through +0x93
    {0xA4, 0x1C},       // timing tail; excludes queue header +0x98..+0xA3
    {0x40010, 0x10},    // secondary timing before mpJobSys1
    {0x40028, 0x18},    // secondary timing between embedded vtable pointers
    {0x40048, 0x14},    // secondary timing tail
};

// sAction owns resources, arrays and transient effect/ExAtkSpace objects in the
// same 0x300-byte body as its match state. Rewind only confirmed scalar state;
// in particular, +0x148 is a live uExAtkSpace reference released through
// vtable+0x78 and must never be imported from an older timeline.
static constexpr SnapshotRange SACTION_RESTORE_RANGES[] = {
    {0x28, 0xE8},  // cActionTime and match/round/step gameplay state
    {0x140, 0x08}, // current ExAtkSpace type and request state before owner ptr
    {0x150, 0x08}, // ExAtkSpace destroy flag and scalar state
    {0x168, 0x09}, // remaining ExAtkSpace scalar state through request flag
    {0x218, 0x04}, // clear-character type
    {0x250, 0x10}, // restart and damage scaling
    {0x268, 0x34}, // combat tuning and stage-range scalar state
};

// cChrTeam embeds its intrusive cChrNode/list head at +0x08. Restore only
// gameplay/team-selection fields; the live list topology must not be rewound.
static constexpr SnapshotRange CCHR_TEAM_RESTORE_RANGES[] = {
    {0x50, 0xC0},  // timing, health, meter, tag/X-Factor and cached active flags
    {0x110, 0x1B0}, // cached character state and play data
};

static bool IsActiveUnit(void* unit);

static void* ValidateShotObjectReference(void* reference, uint64_t expectedDti,
                                         bool expectedActiveUnit)
{
    if (!reference) return nullptr;
    if (!expectedDti || !HasCallableDtiVirtual(reference) ||
        GetObjectDti((uint64_t)reference) != expectedDti) {
        return nullptr;
    }
    if (expectedActiveUnit && !IsActiveUnit(reference)) return nullptr;
    return reference;
}

static int SanitizeUnitCategoryLists(void* sUnit)
{
    if (!sUnit || !IsAccessiblePointer((uint64_t)sUnit)) return 0;

    struct ValidatedUnit {
        void* unit;
        bool valid;
    };
    std::vector<ValidatedUnit> validatedUnits;
    validatedUnits.reserve(128);

    auto isValidUnit = [&](void* unit) {
        for (const ValidatedUnit& cached : validatedUnits) {
            if (cached.unit == unit) return cached.valid;
        }

        bool valid = IsUnitObject(unit);
        if (valid) {
            uint32_t state = *(uint32_t*)((uint8_t*)unit + 0x10);
            if ((state & 7u) == 2u && (state & 0x400u) != 0) {
                uint64_t vtable = *(uint64_t*)unit;
                valid = IsAccessibleAddress(vtable + 0x40) &&
                        IsExecutableAddress(*(uint64_t*)(vtable + 0x40));
            }
        }
        validatedUnits.push_back({unit, valid});
        return valid;
    };

    uint32_t categoryCount =
        *(uint32_t*)((uint8_t*)sUnit + SUNIT_CATEGORY_COUNT_OFFSET);
    if (categoryCount > SUNIT_MAX_CATEGORIES)
        categoryCount = SUNIT_MAX_CATEGORIES;

    uint32_t maskCount =
        *(uint32_t*)((uint8_t*)sUnit + SUNIT_MULTI_MASK_COUNT_OFFSET);
    if (maskCount > SUNIT_MAX_CATEGORIES)
        maskCount = SUNIT_MAX_CATEGORIES;

    int removed = 0;
    for (uint32_t category = 0; category < categoryCount; ++category) {
        auto* head = (void**)((uint8_t*)sUnit + SUNIT_CATEGORY_LISTS_OFFSET +
                              SUNIT_CATEGORY_LIST_STRIDE * category);
        auto* tail = head + 1;
        void* previous = nullptr;
        void* node = *head;
        int safety = 4096;

        while (node && safety-- > 0) {
            if (!IsAccessibleAddress((uint64_t)node + 0x20)) {
                if (previous)
                    *(void**)((uint8_t*)previous + 0x18) = nullptr;
                else
                    *head = nullptr;
                *tail = previous;
                ++removed;
                break;
            }

            void* next = *(void**)((uint8_t*)node + 0x18);
            bool validUnit = isValidUnit(node);
            if (validUnit) {
                previous = node;
                node = next;
                continue;
            }

            // The pending-shot queue can register scalar or unrelated object
            // storage as a unit when its count overruns the 512-entry buffer.
            // Remove the foreign pointer without reading its category or mask.
            for (uint32_t mask = 0; mask < maskCount; ++mask) {
                uint8_t* array = (uint8_t*)sUnit + SUNIT_MULTI_MASK_ARRAYS_OFFSET +
                                 SUNIT_CATEGORY_LIST_STRIDE * mask;
                uint32_t* count = (uint32_t*)(array + 0x08);
                uint32_t capacity = *(uint32_t*)(array + 0x0C);
                void** entries = *(void***)(array + 0x18);
                if (*count > capacity || !entries ||
                    !IsAccessibleAddress((uint64_t)entries +
                                         (size_t)capacity * sizeof(void*)))
                    continue;

                for (uint32_t i = 0; i < *count;) {
                    if (entries[i] != node) {
                        ++i;
                        continue;
                    }
                    for (uint32_t j = i + 1; j < *count; ++j)
                        entries[j - 1] = entries[j];
                    entries[--*count] = nullptr;
                }
            }

            if (previous)
                *(void**)((uint8_t*)previous + 0x18) = next;
            else
                *head = next;

            if (next && IsAccessibleAddress((uint64_t)next + 0x20))
                *(void**)((uint8_t*)next + 0x20) = previous;
            else if (next) {
                next = nullptr;
                if (previous)
                    *(void**)((uint8_t*)previous + 0x18) = nullptr;
                else
                    *head = nullptr;
            }

            if (*tail == node || !next)
                *tail = previous;

            ++removed;
            node = next;
        }

        if (node && safety <= 0) {
            if (previous)
                *(void**)((uint8_t*)previous + 0x18) = nullptr;
            else
                *head = nullptr;
            *tail = previous;
        }
    }
    return removed;
}

static void RemoveUnitFromAllMultiMaskArrays(void* sUnit, void* unit)
{
    if (!sUnit || !unit) return;

    uint32_t maskCount =
        *(uint32_t*)((uint8_t*)sUnit + SUNIT_MULTI_MASK_COUNT_OFFSET);
    if (maskCount > SUNIT_MAX_CATEGORIES)
        maskCount = SUNIT_MAX_CATEGORIES;

    for (uint32_t mask = 0; mask < maskCount; ++mask) {
        uint8_t* array = (uint8_t*)sUnit + SUNIT_MULTI_MASK_ARRAYS_OFFSET +
                         SUNIT_CATEGORY_LIST_STRIDE * mask;
        uint32_t* count = (uint32_t*)(array + 0x08);
        uint32_t capacity = *(uint32_t*)(array + 0x0C);
        void** entries = *(void***)(array + 0x18);
        if (*count > capacity || !entries ||
            !IsAccessibleAddress((uint64_t)entries +
                                 (size_t)capacity * sizeof(void*)))
            continue;

        for (uint32_t i = 0; i < *count;) {
            if (entries[i] != unit) {
                ++i;
                continue;
            }
            for (uint32_t j = i + 1; j < *count; ++j)
                entries[j - 1] = entries[j];
            entries[--*count] = nullptr;
        }
    }

    *(uint64_t*)((uint8_t*)unit + 0x30) = 0;
}

static constexpr int    MAX_REGIONS          = 2048;
static constexpr int    DYNAMIC_SAFETY       = 128; // max projectile/effect nodes
static constexpr int    SSHOT_SENTINEL_COUNT = 10;  // 10 sentinel slots starting at 0x140d47f90
static constexpr size_t SSHOT_NODE_OFFSET    = 0x1450; // uShot->cChrNode offset
static constexpr size_t SSHOT_NODE_NEXT      = 0x08; // cChrNode forward link
static constexpr size_t SHITSOLVER_SIZE      = 0x3808C;
static constexpr size_t SHITSOLVER_DATA_OFF  = 0x58;   // skip vtable, CS, flags, MtArray header
static constexpr size_t SHITSOLVER_ARRAY_OFF = 0x38;
static constexpr size_t SHITSOLVER_POOL_OFF  = 0x60;
static constexpr size_t SHITSOLVER_NODE_SIZE = 0x60;
static constexpr size_t SHITSOLVER_COUNT_OFF = 0x30060;
static constexpr uintptr_t FN_COLLISION_NODE_OBJECT_CREATE = 0x1405AD2A0;
static constexpr uintptr_t FN_COLLISION_NODE_COPY          = 0x1405A4DA0;

// ===========================================================================
// Global Re-Simulation Indicators & Hook State
// ===========================================================================
// Pending re-sim request from FrameStepper_Process (HookUpdate thread).
// hkMvc3Main_move on the main game thread checks/resets these so the
// actual oMvc3Main_move calls run on the correct thread (avoiding D3D
// thread-safety issues in the render path).
// Code-only diagnostic toggle. Keep false during normal play: NDJSON capture
// serializes full character, hitbox, projectile, and memory state per frame.
// Rollback request from controller.cpp — late input arrived that differs
// from what was predicted.  Start/end inclusive frame range.
// __ImageBase gives us the DLL's base address (used with GetModuleFileNameA
// to resolve paths relative to the DLL's directory instead of the CWD, which
// games often change after loading plugins).
static int32_t g_rollbackReqStart = -1;
static int32_t g_rollbackReqEnd   = -1;
static uint64_t g_sMvc3Main      = 0;
static void(__fastcall* oMvc3Main_move)(uint64_t param_1) = nullptr;

 // static decltype(&QueryPerformanceCounter) g_real_qpc = nullptr;
// static uint64_t g_virtual_qpc = 0;
// static DWORD g_main_thread_id = 0;
// static uint64_t g_qpc_history[ROLLBACK_WINDOW] = {};
// 
// static BOOL WINAPI hooked_qpc(LARGE_INTEGER* out) {
//     if (g_isResimulating && GetCurrentThreadId() == g_main_thread_id) {
//         if (out) out->QuadPart = (LONGLONG)g_virtual_qpc;
//         return TRUE;
//     }
//     if (!g_real_qpc) return FALSE;
//     return g_real_qpc(out);
// }

// Frame counter / rollback state from controller.cpp
extern "C" int32_t Controller_GetNetFrameCount();
extern "C" bool    Controller_IsRollbackEnabled();
extern "C" bool    FrameStepper_IsBattleSynchronized();
extern "C" uint32_t Controller_GetCurrentFrameInput();
extern "C" void    Controller_InjectLocalInput(int32_t frame, uint32_t input);
extern "C" void    Controller_InjectRemoteInput(int32_t frame, uint32_t input);
extern "C" void    Controller_SilentFrameStart();
extern "C" void    Controller_SilentFrameEnd();
extern "C" void    Controller_SaveInputHistory();
extern "C" void    Controller_RestoreInputHistory();
extern "C" void    Controller_DumpOldInputs(const char* label);
extern "C" void    Controller_ResolveRemoteInput(int32_t frame);

extern bool g_multiResimEnabled;

// ===========================================================================
// Snapshot Region Descriptor & Ring Buffer
// ===========================================================================
struct Region {
    void*    ptr;       // source/destination address in game memory
    size_t   size;      // number of bytes
    uint64_t ownerSlot; // pointer field that must still refer to ptr on restore
    uint64_t expectedVtable; // nonzero for cChr bodies; validates helper reuse
    bool     skipVtable;
};

struct ShotFlightState {
    uint32_t unitState;             // +0x10 cUnit lifecycle/gameplay flags
    // uModel motion playback advances by cUnit delta-time at +0x28. Restoring
    // Motion[] without this pair can leave reconstructed animations frozen.
    uint8_t timeScaleState[0x08];   // +0x28..+0x2F
    // uModel+0x100 is a live owned pointer, so only restore the contiguous
    // position/quaternion/matrix scalar block preceding it.
    uint8_t transformState[0xB0];  // +0x50..+0xFF
    // uShot::move reads this scalar block for lifetime, active/end flags,
    // velocity and movement state. The factory initializes it, but restoring
    // only the transform and attack blocks leaves a respawned shot at the
    // current timeline's runtime phase.
    uint8_t runtimeState[0x50];     // +0x3E8..+0x437
    // mBaseMatrix, mOldWorldMatrix, mAtkMatrix and mAtkBaseMatrix.
    uint8_t shotMatrices[0x100];   // +0x1E50..+0x1F4F    // Test if necessary on Hado Shoryu
    float timerThresh;
    float timer;
    float delayTime;
    float autoAddDivStartTime;
    uint8_t resourceScalars[0x90]; // +0x1F70..+0x1FFF
    uint8_t attackScalars[0x20];   // +0x2038..+0x2057
    uint8_t movementScalars[0x60]; // +0x2078..+0x20D7
    uint8_t animationScalars[0x14];// +0x2118..+0x212B
    uint32_t step;                  // +0x212C, read by uShot::move
    // These are per-shot dispatch fields. Ghidra's
    // sMvc3Manager_updateRandomSelection name describes the dispatch case, but
    // its this pointer is the active uShot/rChrCpu object, not the singleton.
    uint32_t randomProgress;        // +0xA8
    uint32_t randomSelection;       // +0xC4
    uint32_t randomFlags;            // +0xC8
    uint32_t randomState;            // +0x1B58
    bool hasZireitouState;
    uint32_t zireitouState[4];      // +0x2210..+0x221F phase/timing scalars
};

enum class ShotSpawnKind : uint8_t { Standard, Extended };

struct ShotRecipe {
    void* savedPtr;
    void* restoredPtr;
    void* shotElement;
    uint64_t initParam;
    void* posData;
    void* ownerChr;
    void* objectRef1;
    uint64_t objectRef1Dti;
    bool objectRef1WasActiveUnit;
    uint32_t slotId;
    void* objectRef2;
    uint64_t objectRef2Dti;
    bool objectRef2WasActiveUnit;
    ShotSpawnKind kind;
    int exA9;
    void* exA10;
    int exA11;
    void* exA12;
    ShotFlightState flight;
    ConcreteShotSnapshot concrete;
    uint64_t spawnSequence = 0;
    uint8_t preSpawnGlobalRng[16] = {};
    uint8_t preSpawnEffectRng[16] = {};
    uint8_t preSpawnOwnerRng[16] = {};
    bool hasPreSpawnRng = false;
    bool reconstructed = false;
};

struct EffectRecipe {
    void* savedPtr;
    void* restoredPtr;
    void* sGameEffect;
    int effectId;
    int a3;
    void* attachTo;
    int a5;
    int a6;
    void* a7;
    void* a8;
    int a9;
    void* a10;
    int a11;
    uint32_t a12;
    void* a13;
    uint64_t spawnSequence = 0;
};

struct CollisionNodeClone {
    uint64_t ownerSlot;
    void* savedPtr;
    void* clone;
};

struct SnapshotSlot {
    int32_t  frame       = -1;    // game frame this slot holds (-1 = invalid)
    uint8_t* data        = nullptr; // compact linear memory pool
    size_t   size        = 0;       // total bytes of data written
    size_t   capacity    = 0;       // allocated capacity of data[]
    Region   regions[MAX_REGIONS];  // exact addresses captured at save time
    int      regionCount = 0;       // number of populated regions[]
    uint32_t localInput  = 0;      // local player's delayed input for this frame
    uint32_t remoteInput = 0;      // best-known remote input for this frame
    std::vector<ShotRecipe> shotRecipes;
    std::vector<EffectRecipe> effectRecipes;
    std::vector<CollisionNodeClone> collisionNodeClones;
};

static SnapshotSlot g_slots[ROLLBACK_WINDOW];
static std::unordered_map<void*, ShotRecipe> g_liveShotRecipes;
static std::unordered_map<void*, EffectRecipe> g_liveEffectRecipes;
static uint64_t g_shotSpawnSequence = 0;
static uint64_t g_effectSpawnSequence = 0;

using ShotSpawnFn = void*(__fastcall*)(void*, void*, uint64_t, void*, void*, void*, uint32_t, void*);
using ShotSpawnExFn = void*(__fastcall*)(void*, void*, uint64_t, void*, void*, void*, uint32_t,
                                         void*, int, void*, int, void*);
using ShotEnqueueFn = char(__fastcall*)(void*, void*, int);
using UnitAddFn = char(__fastcall*)(void*, int, void*, void*);
using UnitRemoveFn = char(__fastcall*)(void*, void*);
using UnitRemoveMultiMaskFn = void(__fastcall*)(void*, void*);
using EffectCreateFn = void*(__fastcall*)(void*, int, int, void*, int, int, void*, void*,
                                          int, void*, int, uint32_t, void*);

static ShotSpawnFn oShotSpawn = nullptr;
static ShotSpawnExFn oShotSpawnEx = nullptr;
static UnitRemoveFn oUnitRemove = nullptr;
static void(__fastcall* oKirigakureEffectUpdate)(void* effect) = nullptr;
static EffectCreateFn oEffectCreate = nullptr;
using BuildAttachMatrixFn = void(__fastcall*)(void*, void*, char);
static BuildAttachMatrixFn oBuildAttachMatrix = nullptr;
using ShotUpdateHitboxesFn = void(__fastcall*)(void*);
static ShotUpdateHitboxesFn oShotUpdateHitboxes = nullptr;
using KingKobunMoveFn = void(__fastcall*)(void*);
static KingKobunMoveFn oKingKobunMove = nullptr;
static int g_kingKobunTraceCount = 0;
using HsienKoSelectAnkiFn = int(__fastcall*)(void*);
static HsienKoSelectAnkiFn oHsienKoSelectAnki = nullptr;
using SetReservedAnkiFn = void(__fastcall*)(void*, int);
static SetReservedAnkiFn oSetReservedAnki = nullptr;

using RandomIntRangeFn = int(__fastcall*)(void*, int, int, void*);
static RandomIntRangeFn oRandomIntRange = nullptr;
static int g_rngTraceFrame = -1;
static int g_rngTraceCount = 0;
static int g_ankiTraceFrame = -1;
static int g_ankiTraceCount = 0;
static int g_ankiReserveTraceFrame = -1;
static int g_ankiReserveTraceCount = 0;

static int RngTraceFrame()
{
    uint64_t replay = *(uint64_t*)_addr(PTR_SREPLAY);
    return replay ? *(int32_t*)(replay + 0x54) : -1;
}

static bool IsAnkihouRngCaller(uint64_t caller, void* rngOwner)
{
    // Return addresses immediately following the random calls in Bomb setup,
    // Bomb's direction callback, and Rolling setup/landing. Keep the filter
    // tight so a full gameplay round does not flood the hook log.
    if (rngOwner && GetObjectDti((uint64_t)rngOwner) == (uint64_t)_addr(0x140D455F8))
        return true;
    return caller == (uint64_t)_addr(0x1400A1289) ||
           caller == (uint64_t)_addr(0x14018BAF1) ||
           caller == (uint64_t)_addr(0x14018B5C2) ||
           caller == (uint64_t)_addr(0x14018D245 + 5) ||
           caller == (uint64_t)_addr(0x14018D299 + 5);
}

static int __fastcall hkRandomIntRange(void* stateArg, int low, int high,
                                       void* rngOwner)
{
    uint64_t caller = (uint64_t)__builtin_return_address(0);
    if (!IsAnkihouRngCaller(caller, rngOwner))
        return oRandomIntRange(stateArg, low, high, rngOwner);

    int frame = RngTraceFrame();
    if (frame != g_rngTraceFrame) {
        g_rngTraceFrame = frame;
        g_rngTraceCount = 0;
    }

    uint32_t globalBefore[4] = {};
    uint32_t ownerBefore[4] = {};
    std::memcpy(globalBefore, (void*)_addr(0x140D765D8), sizeof(globalBefore));
    if (rngOwner)
        std::memcpy(ownerBefore, (uint8_t*)rngOwner + 0x2D80, sizeof(ownerBefore));

    int result = oRandomIntRange(stateArg, low, high, rngOwner);

    uint32_t globalAfter[4] = {};
    uint32_t ownerAfter[4] = {};
    std::memcpy(globalAfter, (void*)_addr(0x140D765D8), sizeof(globalAfter));
    if (rngOwner)
        std::memcpy(ownerAfter, (uint8_t*)rngOwner + 0x2D80, sizeof(ownerAfter));

    if (g_rngTraceCount++ < 32) {
        HookLog("RNGTRACE frame=%d resim=%d caller=0x%llX stateArg=0x%llX owner=0x%llX "
                "range=[%d,%d] result=%d global=%08X,%08X,%08X,%08X->%08X,%08X,%08X,%08X "
                "ownerRng(+0x2D80)=%08X,%08X,%08X,%08X->%08X,%08X,%08X,%08X\n",
                frame, g_isResimulating ? 1 : 0, caller, (uint64_t)stateArg,
                (uint64_t)rngOwner, low, high, result,
                globalBefore[0], globalBefore[1], globalBefore[2], globalBefore[3],
                globalAfter[0], globalAfter[1], globalAfter[2], globalAfter[3],
                ownerBefore[0], ownerBefore[1], ownerBefore[2], ownerBefore[3],
                ownerAfter[0], ownerAfter[1], ownerAfter[2], ownerAfter[3]);
    }
    return result;
}

static int __fastcall hkHsienKoSelectAnki(void* character)
{
    if (!character)
        return oHsienKoSelectAnki(character);

    int frame = RngTraceFrame();
    if (frame != g_ankiTraceFrame) {
        g_ankiTraceFrame = frame;
        g_ankiTraceCount = 0;
    }

    uint32_t reservedBefore = *(uint32_t*)((uint8_t*)character + 0x68FC);
    uint32_t commandBefore = *(uint32_t*)((uint8_t*)character + 0x13C4);
    uint32_t actionId = *(uint32_t*)((uint8_t*)character + 0x1310);
    uint32_t rngBefore[4] = {};
    std::memcpy(rngBefore, (uint8_t*)character + 0x2D80, sizeof(rngBefore));

    int result = oHsienKoSelectAnki(character);

    uint32_t rngAfter[4] = {};
    std::memcpy(rngAfter, (uint8_t*)character + 0x2D80, sizeof(rngAfter));
    uint64_t caller = (uint64_t)__builtin_return_address(0);

    if (g_ankiTraceCount++ < 64) {
        HookLog("ANKITRACE frame=%d resim=%d caller=0x%llX char=0x%llX action=0x%08X "
                "reserved=%d commandBefore(+0x13C4)=%08X callbackResult=%d "
                "rng(+0x2D80)=%08X,%08X,%08X,%08X->%08X,%08X,%08X,%08X\n",
                frame, g_isResimulating ? 1 : 0, caller, (uint64_t)character,
                actionId, (int32_t)reservedBefore, commandBefore, result,
                rngBefore[0], rngBefore[1], rngBefore[2], rngBefore[3],
                rngAfter[0], rngAfter[1], rngAfter[2], rngAfter[3]);
    }
    return result;
}

static void __fastcall hkSetReservedAnki(void* character, int value)
{
    if (!character)
        return oSetReservedAnki(character, value);

    int frame = RngTraceFrame();
    if (frame != g_ankiReserveTraceFrame) {
        g_ankiReserveTraceFrame = frame;
        g_ankiReserveTraceCount = 0;
    }

    uint32_t before = *(uint32_t*)((uint8_t*)character + 0x68FC);
    uint32_t actionId = *(uint32_t*)((uint8_t*)character + 0x1310);
    uint64_t caller = (uint64_t)__builtin_return_address(0);
    oSetReservedAnki(character, value);

    if (g_ankiReserveTraceCount++ < 32) {
        HookLog("ANKIRESERVE frame=%d resim=%d caller=0x%llX object=0x%llX "
                "action=0x%08X before=%d after=%d\n",
                frame, g_isResimulating ? 1 : 0, caller, (uint64_t)character,
                actionId, (int32_t)before, value);
    }
}

static bool IsLinkedShot(void* shot);

static void CapturePreSpawnRng(ShotRecipe& recipe, void* ownerChr)
{
    std::memcpy(recipe.preSpawnGlobalRng, (void*)_addr(0x140D765D8), 16);
    uint64_t sEffect = *(uint64_t*)_addr(0x140E17A38);
    if (sEffect)
        std::memcpy(recipe.preSpawnEffectRng, (uint8_t*)sEffect + 0x3C, 16);
    if (ownerChr)
        std::memcpy(recipe.preSpawnOwnerRng, (uint8_t*)ownerChr + 0x2D80, 16);
    recipe.hasPreSpawnRng = true;
}

static void RestorePreSpawnRng(const ShotRecipe& recipe)
{
    if (!recipe.hasPreSpawnRng) return;
    std::memcpy((void*)_addr(0x140D765D8), recipe.preSpawnGlobalRng, 16);
    uint64_t sEffect = *(uint64_t*)_addr(0x140E17A38);
    if (sEffect)
        std::memcpy((uint8_t*)sEffect + 0x3C, recipe.preSpawnEffectRng, 16);
    if (recipe.ownerChr)
        std::memcpy((uint8_t*)recipe.ownerChr + 0x2D80,
                    recipe.preSpawnOwnerRng, 16);
}

static bool IsZireitouShot(void* shot)
{
    return shot && *(uintptr_t*)shot == (uintptr_t)_addr(VTABLE_USHOT_VULCAN_ZIREITOU);
}

static void DestroyCollisionNodeClones(SnapshotSlot& slot)
{
    for (const CollisionNodeClone& entry : slot.collisionNodeClones) {
        if (!entry.clone || !HasCallableDtiVirtual(entry.clone)) continue;
        uint64_t vtable = *(uint64_t*)entry.clone;
        auto destroy = (void(__fastcall*)(void*, uint32_t))*(uint64_t*)vtable;
        if (destroy && IsAccessibleAddress((uint64_t)destroy))
            destroy(entry.clone, 1);
    }
    slot.collisionNodeClones.clear();
}

static void CaptureCollisionNodeClones(SnapshotSlot& slot)
{
    auto createNode =
        (void*(__fastcall*)(void*))_addr(FN_COLLISION_NODE_OBJECT_CREATE);
    auto copyNode =
        (void(__fastcall*)(void*, void*, bool, bool))_addr(FN_COLLISION_NODE_COPY);

    for (int i = 0; i < slot.regionCount; ++i) {
        const Region& region = slot.regions[i];
        if (!region.ownerSlot || region.size != CCHR_EXTERNAL_ANIM_SIZE) continue;
        if (!HasCallableDtiVirtual(region.ptr)) continue;

        void* clone = createNode(nullptr);
        if (!clone) continue;
        // Match the engine's copy thunk: inherit the source child factory, then
        // deep-copy each child through that compatible factory.
        copyNode(clone, region.ptr, false, true);
        slot.collisionNodeClones.push_back({region.ownerSlot, region.ptr, clone});
    }
}

static void CaptureShotFlight(void* shot, ShotFlightState& out)
{
    auto* p = (uint8_t*)shot;
    std::memcpy(&out.unitState, p + 0x10, sizeof(out.unitState));
    std::memcpy(out.timeScaleState, p + 0x28, sizeof(out.timeScaleState));
    std::memcpy(out.transformState, p + 0x50, sizeof(out.transformState));
    std::memcpy(out.runtimeState, p + 0x3E8, sizeof(out.runtimeState));
    std::memcpy(out.shotMatrices, p + 0x1E50, sizeof(out.shotMatrices));  // Test if necessary on Hado Shoryu
    std::memcpy(&out.timerThresh, p + 0x1F58, sizeof(out.timerThresh));
    std::memcpy(&out.timer, p + 0x1F60, sizeof(out.timer));
    std::memcpy(&out.delayTime, p + 0x1F5C, sizeof(out.delayTime));
    std::memcpy(&out.autoAddDivStartTime, p + 0x1F64, sizeof(out.autoAddDivStartTime));
    std::memcpy(out.resourceScalars, p + 0x1F70, sizeof(out.resourceScalars));
    std::memcpy(out.attackScalars, p + 0x2038, sizeof(out.attackScalars));
    std::memcpy(out.movementScalars, p + 0x2078, sizeof(out.movementScalars));
    std::memcpy(out.animationScalars, p + 0x2118, sizeof(out.animationScalars));
    std::memcpy(&out.step, p + 0x212C, sizeof(out.step));
    std::memcpy(&out.randomProgress, p + 0xA8, sizeof(out.randomProgress));
    std::memcpy(&out.randomSelection, p + 0xC4, sizeof(out.randomSelection));
    std::memcpy(&out.randomFlags, p + 0xC8, sizeof(out.randomFlags));
    std::memcpy(&out.randomState, p + 0x1B58, sizeof(out.randomState));
    out.hasZireitouState = IsZireitouShot(shot);
    if (out.hasZireitouState) {
        std::memcpy(out.zireitouState, p + 0x2210, sizeof(out.zireitouState));
    }
}

static void ApplyShotFlight(void* shot, const ShotFlightState& in)
{
    auto* p = (uint8_t*)shot;
    // sUnit::addUnit rewrites only category bits 3..9. Preserve the factory's
    // current category sentinel until registration, but restore the saved
    // lifecycle state so an active shot does not run setup a second time.
    uint32_t unitState = *(uint32_t*)(p + 0x10);
    unitState = (in.unitState & ~0x3F8u) | (unitState & 0x3F8u);
    std::memcpy(p + 0x10, &unitState, sizeof(unitState));
    std::memcpy(p + 0x28, in.timeScaleState, sizeof(in.timeScaleState));
    std::memcpy(p + 0x50, in.transformState, sizeof(in.transformState));
    std::memcpy(p + 0x3E8, in.runtimeState, sizeof(in.runtimeState));
    std::memcpy(p + 0x1E50, in.shotMatrices, sizeof(in.shotMatrices));  // Test if necessary on Hado Shoryu
    std::memcpy(p + 0x1F58, &in.timerThresh, sizeof(in.timerThresh));
    std::memcpy(p + 0x1F60, &in.timer, sizeof(in.timer));
    std::memcpy(p + 0x1F5C, &in.delayTime, sizeof(in.delayTime));
    std::memcpy(p + 0x1F64, &in.autoAddDivStartTime, sizeof(in.autoAddDivStartTime));
    std::memcpy(p + 0x1F70, in.resourceScalars, sizeof(in.resourceScalars));  // Test if necessary on Hado Shoryu
    std::memcpy(p + 0x2038, in.attackScalars, sizeof(in.attackScalars));   // Test if necessary on Hado Shoryu
    std::memcpy(p + 0x2078, in.movementScalars, sizeof(in.movementScalars));
    std::memcpy(p + 0x2118, in.animationScalars, sizeof(in.animationScalars));
    std::memcpy(p + 0x212C, &in.step, sizeof(in.step));
    std::memcpy(p + 0xA8, &in.randomProgress, sizeof(in.randomProgress));
    std::memcpy(p + 0xC4, &in.randomSelection, sizeof(in.randomSelection));
    std::memcpy(p + 0xC8, &in.randomFlags, sizeof(in.randomFlags));
    std::memcpy(p + 0x1B58, &in.randomState, sizeof(in.randomState));
    if (in.hasZireitouState && IsZireitouShot(shot)) {
        std::memcpy(p + 0x2210, in.zireitouState, sizeof(in.zireitouState));
    }
}

static void RestoreShotArmState(void* shot, const ShotRecipe& saved)
{
    if (!shot || saved.concrete.body.size() < 0x1E50) return;

    uint8_t* arm = (uint8_t*)shot + 0x1DC0;
    const uint8_t* savedArm = saved.concrete.body.data() + 0x1DC0;
    void* shootParent = *(void**)(arm + 0x78);
    void* shootWayParent = *(void**)(arm + 0x80);

    // cArm is inline native state. Restore its pointer-free shoot position,
    // direction, offsets and error vectors, but keep the fresh object's vtable
    // and rebind its owner graph below.
    std::memcpy(arm + 0x10, savedArm + 0x10, 0x60);
    std::memcpy(arm + 0x88, savedArm + 0x88, 0x08);

    // mpSelfShot is the reconstructed shot. Retain the parent links established
    // by the native factory (Ryu for his hyper laser), not historical objects.
    *(void**)(arm + 0x70) = shot;
    *(void**)(arm + 0x78) = shootParent;
    *(void**)(arm + 0x80) = shootWayParent;
}

static void* __fastcall hkShotSpawn(void* manager, void* element, uint64_t initParam,
                                    void* posData, void* ownerChr, void* objectRef1,
                                    uint32_t slotId, void* objectRef2)
{
    RollbackHookProfileScope profile(RollbackHookProfileId::ShotSpawn);
    ShotRecipe recipe{};
    recipe.shotElement = element;
    recipe.initParam = initParam;
    recipe.posData = posData;
    recipe.ownerChr = ownerChr;
    recipe.objectRef1 = objectRef1;
    recipe.objectRef1Dti = objectRef1 && HasCallableDtiVirtual(objectRef1)
        ? GetObjectDti((uint64_t)objectRef1) : 0;
    recipe.objectRef1WasActiveUnit = objectRef1 && IsUnitObject(objectRef1) &&
        IsActiveUnit(objectRef1);
    recipe.slotId = slotId;
    recipe.objectRef2 = objectRef2;
    recipe.objectRef2Dti = objectRef2 && HasCallableDtiVirtual(objectRef2)
        ? GetObjectDti((uint64_t)objectRef2) : 0;
    recipe.objectRef2WasActiveUnit = objectRef2 && IsUnitObject(objectRef2) &&
        IsActiveUnit(objectRef2);
    recipe.kind = ShotSpawnKind::Standard;
    recipe.spawnSequence = ++g_shotSpawnSequence;
    // Capture before the native factory. Hsien-Ko Ankihou setup consumes the
    // owner's dispatcher RNG while choosing its concrete effect variant.
    CapturePreSpawnRng(recipe, ownerChr);
    void* shot = oShotSpawn(manager, element, initParam, posData, ownerChr,
                            objectRef1, slotId, objectRef2);
    if (shot) {
        recipe.savedPtr = shot;
        recipe.restoredPtr = shot;
        g_liveShotRecipes[shot] = recipe;
    }
    return shot;
}

static void* __fastcall hkShotSpawnEx(void* manager, void* element, uint64_t initParam,
                                      void* posData, void* ownerChr, void* objectRef1,
                                      uint32_t slotId, void* objectRef2, int a9,
                                      void* a10, int a11, void* a12)
{
    RollbackHookProfileScope profile(RollbackHookProfileId::ShotSpawnEx);
    ShotRecipe recipe{};
    recipe.shotElement = element;
    recipe.initParam = initParam;
    recipe.posData = posData;
    recipe.ownerChr = ownerChr;
    recipe.objectRef1 = objectRef1;
    recipe.objectRef1Dti = objectRef1 && HasCallableDtiVirtual(objectRef1)
        ? GetObjectDti((uint64_t)objectRef1) : 0;
    recipe.objectRef1WasActiveUnit = objectRef1 && IsUnitObject(objectRef1) &&
        IsActiveUnit(objectRef1);
    recipe.slotId = slotId;
    recipe.objectRef2 = objectRef2;
    recipe.objectRef2Dti = objectRef2 && HasCallableDtiVirtual(objectRef2)
        ? GetObjectDti((uint64_t)objectRef2) : 0;
    recipe.objectRef2WasActiveUnit = objectRef2 && IsUnitObject(objectRef2) &&
        IsActiveUnit(objectRef2);
    recipe.kind = ShotSpawnKind::Extended;
    recipe.exA9 = a9;
    recipe.exA10 = a10;
    recipe.exA11 = a11;
    recipe.exA12 = a12;
    recipe.spawnSequence = ++g_shotSpawnSequence;
    CapturePreSpawnRng(recipe, ownerChr);
    void* shot = oShotSpawnEx(manager, element, initParam, posData, ownerChr,
                              objectRef1, slotId, objectRef2, a9, a10, a11, a12);
    if (shot) {
        recipe.savedPtr = shot;
        recipe.restoredPtr = shot;
        g_liveShotRecipes[shot] = recipe;
    }
    return shot;
}

static void* __fastcall hkEffectCreate(void* manager, int effectId, int a3,
                                       void* attachTo, int a5, int a6, void* a7,
                                       void* a8, int a9, void* a10, int a11,
                                       uint32_t a12, void* a13)
{
    RollbackHookProfileScope profile(RollbackHookProfileId::EffectCreate);
    void* effect = oEffectCreate(manager, effectId, a3, attachTo, a5, a6, a7,
                                 a8, a9, a10, a11, a12, a13);
    if (effect) {
        EffectRecipe recipe = {
            effect, effect, manager, effectId, a3, attachTo, a5, a6, a7, a8,
            a9, a10, a11, a12, a13
        };
        recipe.spawnSequence = ++g_effectSpawnSequence;
        g_liveEffectRecipes[effect] = recipe;
    }
    return effect;
}

static void DetachEmbeddedShotEffect(void* shot);

static void SanitizeEffectForDestruction(void* effect)
{
    if (!effect || !IsAccessibleRange((uint64_t)effect, 0x230)) return;
    if (g_activeRollbackProfile)
        ++g_activeRollbackProfile->restoreEffectSanitizeCalls;

    // uEffect::~uEffect releases the cResource at +0x108. A torn effect can
    // retain a small scalar here (0x120 in the 04:06 crash), which makes
    // sResource::release decrement memory at scalar+0x4c. Keep valid native
    // resources; suppress only values that cannot be resource objects.
    {
        RollbackHookProfileScope profile(RollbackHookProfileId::UnitRemoveEffectResource);
        uint64_t resource = *(uint64_t*)((uint8_t*)effect + 0x108);
        if (resource != 0 &&
            (!IsAccessibleAddress(resource + 0x4C) ||
             !HasCallableDtiVirtual((void*)resource))) {
            *(uint64_t*)((uint8_t*)effect + 0x108) = 0;
        }
    }

    // uEffect::~uEffect deletes the generator chain at +0x218 and releases the
    // particle sub-buffer array at +0x220. Prediction-only effects can outlive
    // the graph that populated these fields until sUnit's deferred cleanup.
    // Clear only malformed ownership; valid native allocations remain intact.
    {
        RollbackHookProfileScope profile(RollbackHookProfileId::UnitRemoveEffectGenerator);
        uint64_t generator = *(uint64_t*)((uint8_t*)effect + 0x218);
        uint64_t node = generator;
        uint64_t cycleCheckpoint = generator;
        uint32_t cyclePower = 1;
        uint32_t cycleLength = 0;
        bool validGeneratorChain = true;
        for (int count = 0; node != 0 && count < 4096; ++count) {
            if (g_activeRollbackProfile)
                ++g_activeRollbackProfile->restoreEffectGeneratorNodes;
            if (!IsAccessibleRange(node, 0x20)) {
                validGeneratorChain = false;
                break;
            }
            uint64_t vtable = *(uint64_t*)node;
            // uEffect::releaseGenerator only follows +0x18 and invokes the deleting
            // destructor at vtable slot 0. Other update and draw slots are not part
            // of destruction and made this guard perform eight VirtualQuery calls
            // for every node in every retired effect.
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
            node = next;
            if (count == 4095 && node != 0) validGeneratorChain = false;
        }
        if (!validGeneratorChain) {
            *(uint64_t*)((uint8_t*)effect + 0x218) = 0;
        }
    }

    {
        RollbackHookProfileScope profile(RollbackHookProfileId::UnitRemoveEffectSubBuffers);
        uint64_t subBuffers = *(uint64_t*)((uint8_t*)effect + 0x220);
        if (subBuffers != 0 && !IsAccessiblePointer(subBuffers)) {
            *(uint64_t*)((uint8_t*)effect + 0x220) = 0;
            *(uint16_t*)((uint8_t*)effect + 0x208) = 0;
        }
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
        !IsAccessibleAddress(exAtkSpace + 0x1D30)) {
        return;
    }

    // uExAtkSpace::setModel releases these four owned uEffect references via
    // virtual +0x78. Rollback retires effect categories independently, so the
    // children can be destroyed before the still-live ExAtkSpace is replaced.
    // Detach them while every object is still valid; native sUnit cleanup
    // remains responsible for retiring the child effects themselves.
    std::memset((void*)(exAtkSpace + 0x1D18), 0, 4 * sizeof(uint64_t));
}

static char __fastcall hkUnitRemove(void* manager, void* unit)
{
    RollbackHookProfileScope profile(RollbackHookProfileId::UnitRemove);
    decltype(g_liveShotRecipes)::iterator shotRecipe;
    {
        RollbackHookProfileScope child(RollbackHookProfileId::UnitRemoveRecipeLookup);
        shotRecipe = g_liveShotRecipes.find(unit);
    }
    if (shotRecipe != g_liveShotRecipes.end() &&
        shotRecipe->second.reconstructed) {
        // Natural category cleanup calls removeUnit immediately before the
        // concrete shot destructor. Gameplay can repopulate an inline particle
        // node after reconstruction, so detach it at this final ownership
        // boundary as well as during rollback's explicit retirement.
        RollbackHookProfileScope child(RollbackHookProfileId::UnitRemoveShotDetach);
        DetachEmbeddedShotEffect(unit);
    }
    bool isEffect = false;
    {
        RollbackHookProfileScope child(RollbackHookProfileId::UnitRemoveEffectCheck);
        isEffect = IsEffectObject(unit);
    }
    if (isEffect) {
        RollbackHookProfileScope child(RollbackHookProfileId::UnitRemoveEffectSanitize);
        SanitizeEffectForDestruction(unit);
    }

    char result;
    {
        RollbackHookProfileScope child(RollbackHookProfileId::UnitRemoveNative);
        result = oUnitRemove(manager, unit);
    }
    if (result && unit) {
        RollbackHookProfileScope child(RollbackHookProfileId::UnitRemoveRecipeCleanup);
        // A shot can leave sUnit while remaining linked in sShot until its
        // deferred list cleanup. Keep its spawn recipe through that interval;
        // hkShotSpawn replaces the entry if the allocator reuses the address.
        if (!IsLinkedShot(unit))
            g_liveShotRecipes.erase(unit);
        g_liveEffectRecipes.erase(unit);
    }
    return result;
}

static void __fastcall hkKirigakureEffectUpdate(void* effect)
{
    RollbackHookProfileScope profile(RollbackHookProfileId::KirigakureEffectUpdate);
    // This callback calls MtObject::isAlive directly on its weak attachment at
    // +0x40. Validate only at that dereference site instead of scanning every
    // sUnit category and performing DTI checks on every object each frame.
    uint64_t* attachment = (uint64_t*)((uint8_t*)effect + 0x40);
    if (*attachment && !HasCallableDtiVirtual((void*)*attachment))
        *attachment = 0;

    oKirigakureEffectUpdate(effect);
}

static bool IsLinkedShot(void* shot)
{
    for (int list = 0; list < SSHOT_SENTINEL_COUNT; ++list) {
        uint64_t sentinel = (uint64_t)_addr(0x140d47f90) + list * 0x28;
        uint64_t sentinelRef = sentinel + 0x08;
        uint64_t nodeRef = *(uint64_t*)(sentinel + 0x10);
        int safety = DYNAMIC_SAFETY;
        while (nodeRef && nodeRef != sentinelRef && safety-- > 0) {
            if ((void*)(nodeRef - SSHOT_NODE_OFFSET) == shot) return true;
            nodeRef = *(uint64_t*)(nodeRef + SSHOT_NODE_NEXT);
        }
    }
    return false;
}

static bool IsActiveUnit(void* unit)
{
    if (!unit) return false;
    uint32_t state = *(uint32_t*)((uint8_t*)unit + 0x10);
    return (state & 7u) < 3u && (state & 0x3F8u) != 0x3F8u;
}

static void RestoreKingKobunHelper(void* shot, const ShotRecipe& saved)
{
    constexpr uintptr_t KING_KOBUN_DTI = 0x140D4D2B0;
    constexpr uintptr_t KOBUN_DTI = 0x140D455B0;
    constexpr size_t HELPER_OFFSET = 0x2158;
    if (!shot || saved.concrete.dti != (uint64_t)_addr(KING_KOBUN_DTI) ||
        saved.concrete.body.size() < HELPER_OFFSET + sizeof(uint64_t)) {
        return;
    }

    void* savedHelper = *(void**)(saved.concrete.body.data() + HELPER_OFFSET);
    void* helper = ValidateShotObjectReference(
        savedHelper, (uint64_t)_addr(KOBUN_DTI), true);

    // King Servbot setup normally obtains this relationship from Tron's
    // transient action-side-camera state. That lookup is no longer populated
    // when a historical shot is reconstructed, so fall back to the unique live
    // uKobun helper in the character category.
    if (!helper) {
        uint64_t sUnit = *(uint64_t*)_addr(PTR_SUNIT);
        if (sUnit && *(uint32_t*)(sUnit + SUNIT_CATEGORY_COUNT_OFFSET) >
                         SUNIT_CHARACTER_CATEGORY) {
            uint64_t node = *(uint64_t*)(sUnit + SUNIT_CATEGORY_LISTS_OFFSET +
                SUNIT_CATEGORY_LIST_STRIDE * SUNIT_CHARACTER_CATEGORY);
            for (int safety = 64; node && safety-- > 0; node = *(uint64_t*)(node + 0x18)) {
                if (GetObjectDti(node) == (uint64_t)_addr(KOBUN_DTI) &&
                    IsActiveUnit((void*)node)) {
                    if (helper) return; // Ambiguous: do not bind another player's helper.
                    helper = (void*)node;
                }
            }
        }
    }

    *(void**)((uint8_t*)shot + HELPER_OFFSET) = helper;
}

static bool IsTerminalShot(void* shot)
{
    if (!shot || !IsActiveUnit(shot)) return false;

    // uShot::move dispatches step 3 to the concrete shot handler instead of
    // running the common lifetime path. A few handlers leave an expired shot
    // linked with lifeFrame=0 and a nonzero timer, causing rollback recipe
    // capture to resurrect the dead object forever. This is the terminal
    // pattern observed for the persistent list-0 shot in the 031454 capture.
    float lifeFrame = *(float*)((uint8_t*)shot + 0x2078);
    float timer = *(float*)((uint8_t*)shot + 0x1F60);
    uint32_t step = *(uint32_t*)((uint8_t*)shot + 0x212C);
    return std::isfinite(lifeFrame) && lifeFrame <= 0.0f &&
           std::isfinite(timer) && timer > 0.0f && step == 3;
}

static void DetachEmbeddedShotEffect(void* shot)
{
    if (!shot || !HasCallableDtiVirtual(shot)) return;

    constexpr uint32_t EMBEDDED_EFFECT_OFFSET = 0x2160;
    constexpr uint32_t EMBEDDED_EFFECT_SIZE = 0x230;
    uint32_t shotSize = DecodeDtiSize(GetObjectDti((uint64_t)shot));
    if (shotSize < EMBEDDED_EFFECT_OFFSET + EMBEDDED_EFFECT_SIZE) return;

    // The concrete laser destructors pass shot+0x2160 to particle unlink.
    // Ghidra renders this as field19_0x1350+0xE10, but field19 is inline
    // storage. Only concrete types whose DTI size contains the complete uEffect
    // may use this layout. Shorter lasers end near +0x2160; treating committed
    // memory after them as inline storage corrupts the next heap allocation.
    uint8_t* embeddedEffect = (uint8_t*)shot + EMBEDDED_EFFECT_OFFSET;
    if (!IsEffectObject(embeddedEffect)) return;
    *(uint64_t*)(embeddedEffect + 0x50) = 0;
    *(uint64_t*)(embeddedEffect + 0x58) = 0;
    *(uint64_t*)(embeddedEffect + 0x60) = 0;
    *(uint32_t*)(embeddedEffect + 0x70) = 0xFFFFFFFFu;

    // Reconstructed shots do not own a reconstructed particle graph. Ensure a
    // saved inline effect can never return historical storage to the allocator.
    *(uint64_t*)(embeddedEffect + 0x218) = 0;
    *(uint64_t*)(embeddedEffect + 0x220) = 0;
    *(uint64_t*)(embeddedEffect + 0x228) = 0;
    *(uint32_t*)(embeddedEffect + 0x208) = 0;
    *(uint32_t*)(embeddedEffect + 0x210) = 0;
}

static void ReleaseFreshEmbeddedShotEffectGraph(void* shot)
{
    if (!shot || !HasCallableDtiVirtual(shot)) return;

    constexpr uint32_t EMBEDDED_EFFECT_OFFSET = 0x2160;
    constexpr uint32_t EMBEDDED_EFFECT_SIZE = 0x230;
    uint32_t shotSize = DecodeDtiSize(GetObjectDti((uint64_t)shot));
    if (shotSize < EMBEDDED_EFFECT_OFFSET + EMBEDDED_EFFECT_SIZE) return;

    void* embeddedEffect = (uint8_t*)shot + EMBEDDED_EFFECT_OFFSET;
    if (!IsEffectObject(embeddedEffect)) return;

    // This graph was just created by the native shot factory, so its allocator
    // ownership is still authoritative. Release it before concrete restoration;
    // DetachEmbeddedShotEffect then keeps the reconstructed shot graph-free.
    ((void(__fastcall*)(void*))_addr(0x1405E6890))(embeddedEffect);
}

static bool ValidAttachDispatchObject(uint64_t object)
{
    if (!object || !IsAccessiblePointer(object) ||
        !HasCallableDtiVirtual((void*)object)) return false;

    uint64_t vtable = *(uint64_t*)object;
    return IsAccessibleAddress(vtable + 0x2C8) &&
           IsAccessibleAddress(vtable + 0x2D0) &&
           IsExecutableAddress(*(uint64_t*)(vtable + 0x2C8)) &&
           IsExecutableAddress(*(uint64_t*)(vtable + 0x2D0));
}

static bool ValidOptionalAttachObject(uint64_t object)
{
    return object == 0 || HasCallableDtiVirtual((void*)object);
}

static void __fastcall hkBuildAttachMatrix(void* arm, void* output, char reset)
{
    RollbackHookProfileScope profile(RollbackHookProfileId::BuildAttachMatrix);
    if (!arm || !output || !IsAccessiblePointer((uint64_t)arm + 0xD0)) {
        return;
    }

    void* shot = (uint8_t*)arm - 0x1DC0;
    auto recipe = g_liveShotRecipes.find(shot);
    if (recipe == g_liveShotRecipes.end() || !recipe->second.reconstructed) {
        oBuildAttachMatrix(arm, output, reset);
        return;
    }

    auto* bytes = (uint8_t*)arm;
    uint64_t primary = *(uint64_t*)(bytes + 0x70);
    uint64_t secondary = *(uint64_t*)(bytes + 0x78);
    uint64_t tertiary = *(uint64_t*)(bytes + 0x80);
    if (ValidAttachDispatchObject(primary) &&
        ValidOptionalAttachObject(secondary) &&
        ValidOptionalAttachObject(tertiary)) {
        oBuildAttachMatrix(arm, output, reset);
        return;
    }

    // A reconstructed owner can still contain a stale optional attachment or
    // primary dispatch link. Preserve the last native matrix for that case;
    // valid fresh owners must run natively so directional input can update the
    // laser quaternion during re-simulation.
    (void)reset;
    std::memcpy(output, bytes + 0x90, 0x40);
}

static void __fastcall hkShotUpdateHitboxes(void* shot)
{
    RollbackHookProfileScope profile(RollbackHookProfileId::ShotUpdateHitboxes);
    oShotUpdateHitboxes(shot);

    auto recipe = g_liveShotRecipes.find(shot);
    if (recipe == g_liveShotRecipes.end() || !recipe->second.reconstructed)
        return;

    ConcreteShotSnapshot_ReapplyCapsules(shot, recipe->second.concrete);
}

static void __fastcall hkKingKobunMove(void* shot)
{
    RollbackHookProfileScope profile(RollbackHookProfileId::KingKobunMove);
    bool trace = shot && GetObjectDti((uint64_t)shot) == _addr(0x140D4D2B0) &&
        g_kingKobunTraceCount < 128;
    uint32_t beforeMode = 0;
    uint32_t beforePhase = 0;
    float beforePos = 0.0f;
    if (trace) {
        beforeMode = *(uint32_t*)((uint8_t*)shot + 0x214C);
        beforePhase = *(uint32_t*)((uint8_t*)shot + 0x2140);
        beforePos = *(float*)((uint8_t*)shot + 0x50);
    }

    oKingKobunMove(shot);

    if (trace) {
        HookLog("KINGMOVE shot=0x%llX frame=%d resim=%d mode=%u->%u phase=%u->%u "
                "x=%.6g->%.6g speed=%.6g dt=%.6g facing=%u helper=0x%llX state=0x%08X\n",
                (uint64_t)shot, RngTraceFrame(), g_multiResimEnabled ? 1 : 0,
                beforeMode, *(uint32_t*)((uint8_t*)shot + 0x214C),
                beforePhase, *(uint32_t*)((uint8_t*)shot + 0x2140),
                beforePos, *(float*)((uint8_t*)shot + 0x50),
                *(float*)((uint8_t*)shot + 0x2150),
                *(float*)((uint8_t*)shot + 0x28),
                *(uint8_t*)((uint8_t*)shot + 0x2154),
                *(uint64_t*)((uint8_t*)shot + 0x2158),
                *(uint32_t*)((uint8_t*)shot + 0x10));
        ++g_kingKobunTraceCount;
    }
}

static int SanitizeShotCollisionSlotsForDestruction(void* shot)
{
    if (!shot || !IsAccessiblePointer((uint64_t)shot + 0x2078)) return 0;

    int sanitized = 0;
    auto* shotBytes = (uint8_t*)shot;
    for (uint32_t index = 0; index < 4; ++index) {
        uint64_t slot = *(uint64_t*)(shotBytes + 0x2058 + index * sizeof(uint64_t));
        if (!slot || !IsAccessiblePointer(slot + 0x38)) continue;

        // cCollisionNode's geometry MtArray owns its entries at +0x30. A
        // partially restored header can retain a scalar as its data pointer;
        // uShot__releaseAtkInfo then calls the slot destructor, which passes
        // that value to MtAllocator::free. Discard only malformed arrays: the
        // allocation cannot be reclaimed safely, but the shot can still run
        // its remaining native teardown.
        uint32_t count = *(uint32_t*)(slot + 0x20);
        uint32_t capacity = *(uint32_t*)(slot + 0x24);
        uint64_t entries = *(uint64_t*)(slot + 0x30);
        bool malformed = count > capacity || count > 128 ||
            (count != 0 && !IsAccessiblePointer(entries)) ||
            (entries != 0 && !IsAccessiblePointer(entries));
        if (!malformed) continue;

        std::memset((void*)(slot + 0x20), 0, 0x18);
        ++sanitized;
    }
    return sanitized;
}

static void CaptureShotAndEffectRecipes(SnapshotSlot& slot)
{
    slot.shotRecipes.clear();
    slot.effectRecipes.clear();

    // Only snapshot recipes for shots that are still linked in sShot. The
    // lifecycle hook may retain an entry until sUnit finishes its two-step
    // removal, so copying the entire map would resurrect dying projectiles.
    for (int list = 0; list < SSHOT_SENTINEL_COUNT; ++list) {
        uint64_t sentinel = (uint64_t)_addr(0x140d47f90) + list * 0x28;
        uint64_t sentinelRef = sentinel + 0x08;
        uint64_t nodeRef = *(uint64_t*)(sentinel + 0x10);
        int safety = DYNAMIC_SAFETY;
        while (nodeRef && nodeRef != sentinelRef && safety-- > 0) {
            void* shot = (void*)(nodeRef - SSHOT_NODE_OFFSET);
            auto recipe = g_liveShotRecipes.find(shot);
            if (recipe != g_liveShotRecipes.end()) {
                if (IsTerminalShot(shot)) {
                    nodeRef = *(uint64_t*)(nodeRef + SSHOT_NODE_NEXT);
                    continue;
                }
                ShotRecipe saved = recipe->second;
                saved.savedPtr = shot;
                CaptureShotFlight(shot, saved.flight);
                ConcreteShotSnapshot_Capture(shot, saved.concrete);
                slot.shotRecipes.push_back(saved);
            }
            nodeRef = *(uint64_t*)(nodeRef + SSHOT_NODE_NEXT);
        }
    }

    std::sort(slot.shotRecipes.begin(), slot.shotRecipes.end(),
              [](const ShotRecipe& a, const ShotRecipe& b) {
                  return a.spawnSequence < b.spawnSequence;
              });

    // uGameEffect creation is centralized at the hooked 13-argument factory.
    // Entries are removed when sUnit removes the corresponding object.
    slot.effectRecipes.reserve(g_liveEffectRecipes.size());
    for (const auto& entry : g_liveEffectRecipes) {
        if (!IsActiveUnit(entry.first)) continue;
        EffectRecipe saved = entry.second;
        saved.savedPtr = entry.first;
        saved.restoredPtr = entry.first;
        slot.effectRecipes.push_back(saved);
    }
}

static void RestoreShotAndEffectRecipes(SnapshotSlot& slot)
{
    if (!oShotSpawn || !oShotSpawnEx) {
        HookLog("Rollback: shot restore skipped because lifecycle hooks are unavailable\n");
        return;
    }

    void* sUnit = *(void**)_addr(PTR_SUNIT);
    auto removeUnit = (UnitRemoveFn)_addr(FN_SUNIT_REMOVE_UNIT);
    const uint64_t profileStart = Profile_Now();
    if (g_activeRollbackProfile)
        g_activeRollbackProfile->restoreSavedShots += (uint32_t)slot.shotRecipes.size();

    // Each live uShot owns the rShot resource stored in its recipe. Pin every
    // resource before destroying current shots so native reconstruction cannot
    // dereference attack/collision data released with the old object.
    auto addRef = (void(__fastcall*)(void*))_addr(0x14050D100);
    auto releaseRef = (void(__fastcall*)(void*))_addr(0x14050D5A0);
    std::vector<void*> pinnedShotResources;
    pinnedShotResources.reserve(slot.shotRecipes.size());
    for (const ShotRecipe& saved : slot.shotRecipes) {
        if (!saved.shotElement || !HasCallableDtiVirtual(saved.shotElement))
            continue;
        addRef(saved.shotElement);
        pinnedShotResources.push_back(saved.shotElement);
    }
    const uint64_t pinResourcesEnd = Profile_Now();

    int removedInvalidUnits = SanitizeUnitCategoryLists(sUnit);
    if (removedInvalidUnits != 0) {
        HookLog("Rollback: removed %d invalid sUnit category entries before restore\n",
                 removedInvalidUnits);
    }
    const uint64_t preSanitizeEnd = Profile_Now();

    // Effect bodies and generator graphs are not rewound. Retaining even a
    // snapshot-era effect lets its independently advanced graph execute after
    // restore; observed failures cover move, finish, draw, and destruction.
    // Retire all effects through native two-phase cleanup. hkUnitRemove
    // sanitizes owned resource/generator fields at the destruction boundary.
    DetachExAtkSpaceChildren();
    int retiredEffects = 0;
    uint32_t categoryCount = *(uint32_t*)((uint8_t*)sUnit + SUNIT_CATEGORY_COUNT_OFFSET);
    for (uint32_t category = 13; category <= 16 && category < categoryCount; ++category) {
        void* effect = *(void**)((uint8_t*)sUnit + SUNIT_CATEGORY_LISTS_OFFSET +
                                 SUNIT_CATEGORY_LIST_STRIDE * category);
        int safety = 4096;
        while (effect && safety-- > 0) {
            if (g_activeRollbackProfile)
                ++g_activeRollbackProfile->restoreEffectVisits;
            void* next = *(void**)((uint8_t*)effect + 0x18);
            uint32_t* state = (uint32_t*)((uint8_t*)effect + 0x10);
            if (IsEffectObject(effect) && (*state & 7u) < 3u) {
                *state = (*state & ~7u) | 3u;
                ++retiredEffects;
            }
            effect = next;
        }
    }
    const uint64_t retireEffectsEnd = Profile_Now();

    // A projectile that has already advanced cannot be made equivalent to its
    // saved state by patching trajectory fields: hit-consumption, collision,
    // callback, and subclass-tail state remain advanced. Remove every current
    // shot synchronously before recreating the saved set. Merely setting state
    // 3 leaves the object in both sUnit and sShot for two more moveCategory
    // passes, so rollback would briefly run the retired shot beside its clone.
    int retiredShots = 0;
    std::vector<void*> currentShots;
    currentShots.reserve(g_liveShotRecipes.size());
    for (const auto& entry : g_liveShotRecipes) {
        if (!IsLinkedShot(entry.first)) continue;
        currentShots.push_back(entry.first);
    }
    if (g_activeRollbackProfile)
        g_activeRollbackProfile->restoreCurrentShots += (uint32_t)currentShots.size();
    const uint64_t collectShotsEnd = Profile_Now();
    for (void* shot : currentShots) {
        // Some concrete shots embed an sEffect at +0x2140. Its destructor
        // unlinks through the owner and then walks owner+0xA8's child chain.
        // That graph is not reconstructed by the shot recipe path, so even a
        // readable owner can contain a stale child and crash in
        // sEffect__markChildrenDirtyForUnlink. Detach the embedded particle
        // links before destruction; the native factory creates fresh effects
        // for the restored shot when needed.
        DetachEmbeddedShotEffect(shot);
        int malformedCollisionSlots = SanitizeShotCollisionSlotsForDestruction(shot);
        if (malformedCollisionSlots != 0) {
            HookLog("Rollback: detached %d malformed collision arrays before destroying shot 0x%llX\n",
                    malformedCollisionSlots, (uint64_t)shot);
        }
        uint32_t* state = (uint32_t*)((uint8_t*)shot + 0x10);
        RemoveUnitFromAllMultiMaskArrays(sUnit, shot);
        removeUnit(sUnit, shot);
        *state &= ~7u;
        uint64_t vtable = *(uint64_t*)shot;
        auto destroy = (void(__fastcall*)(void*, uint32_t))*(uint64_t*)vtable;
        destroy(shot, 1);
        ++retiredShots;
    }
    const uint64_t destroyShotsEnd = Profile_Now();

    // Concrete shot destructors can release attached uGameEffect objects
    // directly. Those effects may still be present in sUnit's primary and
    // multi-mask lists with a zeroed vtable, causing the next parallel move
    // job to dispatch through null at vtable+0x40. Remove such dead entries
    // after all shot-owned teardown has completed and before reconstruction.
    // Only shot destructors can introduce the dead attached-effect entries this
    // second pass repairs. With no current shots, topology is unchanged since
    // the pre-restore sanitation pass, so rescanning every sUnit list is pure
    // overhead.
    int removedShotOwnedEffects = currentShots.empty()
        ? 0
        : SanitizeUnitCategoryLists(sUnit);
    if (removedShotOwnedEffects != 0) {
        HookLog("Rollback: removed %d dead sUnit entries after shot destruction\n",
                 removedShotOwnedEffects);
    }
    const uint64_t postSanitizeEnd = Profile_Now();
    for (uint32_t category = 13; category <= 16 && category < categoryCount; ++category) {
        void* effect = *(void**)((uint8_t*)sUnit + SUNIT_CATEGORY_LISTS_OFFSET +
                                 SUNIT_CATEGORY_LIST_STRIDE * category);
        int safety = 4096;
        while (effect && safety-- > 0) {
            if (g_activeRollbackProfile)
                ++g_activeRollbackProfile->restoreEffectVisits;
            void* next = *(void**)((uint8_t*)effect + 0x18);
            if (IsEffectObject(effect))
                SanitizeEffectForDestruction(effect);
            effect = next;
        }
    }
    const uint64_t sanitizeEffectsEnd = Profile_Now();

    void* sShot = *(void**)_addr(PTR_SSHOT);
    auto enqueueUnit = (ShotEnqueueFn)_addr(FN_SSHOT_ENQUEUE_UNIT);
    auto addUnit = (UnitAddFn)_addr(FN_SUNIT_ADD_UNIT);
    auto flushPendingUnits =
        (void(__fastcall*)(void*))_addr(FN_SSHOT_FLUSH_PENDING_UNITS);
    uint32_t* pendingCountPtr = (uint32_t*)((uint8_t*)sShot + 0x2448);
    if (*pendingCountPtr != 0) {
        HookLog("Rollback: discarding %u pre-restore pending-shot entries\n",
                *pendingCountPtr);
        *pendingCountPtr = 0;
    }
    int respawnedShots = 0;
    int failedShots = 0;

    // Native shot construction consumes action/global/effect RNG while
    // rebuilding resources and owned effects. Reconstruction is rollback
    // plumbing, not a simulated gameplay event, so it must not advance any
    // deterministic stream before the first re-simulated frame.
    uint8_t restoredGlobalRng[16] = {};
    uint8_t restoredEffectRng[16] = {};
    struct SavedCharacterRng {
        void* character;
        uint8_t state[16];
    };
    std::vector<SavedCharacterRng> restoredCharacterRngs;
    uint64_t sEffect = *(uint64_t*)_addr(0x140E17A38);
    std::memcpy(restoredGlobalRng, (void*)_addr(0x140d765d8), sizeof(restoredGlobalRng));
    if (sEffect) std::memcpy(restoredEffectRng, (void*)(sEffect + 0x3C), sizeof(restoredEffectRng));

    // Some shot setup methods dispatch RNG through their owning uCharacter.
    // Preserve each owner's stream so native reconstruction remains plumbing.
    for (const ShotRecipe& saved : slot.shotRecipes) {
        if (!saved.ownerChr) continue;
        bool alreadySaved = false;
        for (const SavedCharacterRng& characterRng : restoredCharacterRngs) {
            if (characterRng.character == saved.ownerChr) {
                alreadySaved = true;
                break;
            }
        }
        if (alreadySaved) continue;

        SavedCharacterRng characterRng{};
        characterRng.character = saved.ownerChr;
        std::memcpy(characterRng.state, (uint8_t*)saved.ownerChr + 0x2D80,
                    sizeof(characterRng.state));
        restoredCharacterRngs.push_back(characterRng);
    }
    const uint64_t respawnSetupEnd = Profile_Now();

    uint64_t respawnPrepareTicks = 0;
    uint64_t respawnFactoryTicks = 0;
    uint64_t respawnRegisterTicks = 0;
    uint64_t respawnReleaseEffectsTicks = 0;
    uint64_t respawnConcreteTicks = 0;
    uint64_t respawnOverlayTicks = 0;
    for (ShotRecipe& saved : slot.shotRecipes) {
        const uint64_t prepareStart = Profile_Now();
        RestorePreSpawnRng(saved);
        void* objectRef1 = ValidateShotObjectReference(
            saved.objectRef1, saved.objectRef1Dti,
            saved.objectRef1WasActiveUnit);
        void* objectRef2 = ValidateShotObjectReference(
            saved.objectRef2, saved.objectRef2Dti,
            saved.objectRef2WasActiveUnit);
        const uint64_t factoryStart = Profile_Now();
        respawnPrepareTicks += factoryStart - prepareStart;
        void* fresh = saved.kind == ShotSpawnKind::Extended
            ? oShotSpawnEx(sShot, saved.shotElement, saved.initParam, saved.posData,
                           saved.ownerChr, objectRef1, saved.slotId,
                           objectRef2, saved.exA9, saved.exA10,
                           saved.exA11, saved.exA12)
            : oShotSpawn(sShot, saved.shotElement, saved.initParam, saved.posData,
                         saved.ownerChr, objectRef1, saved.slotId, objectRef2);
        const uint64_t factoryEnd = Profile_Now();
        respawnFactoryTicks += factoryEnd - factoryStart;
        if (!fresh) {
            failedShots++;
            continue;
        }

        // The factory may create an inline effect with a valid vtable, but its
        // owner/list links still point at the old prediction graph. Detach it
        // before the broad concrete overlay can copy any stale relationship.
        ShotRecipe liveRecipe = saved;
        liveRecipe.savedPtr = fresh;
        liveRecipe.restoredPtr = fresh;
        liveRecipe.reconstructed = true;
        g_liveShotRecipes[fresh] = liveRecipe;
        saved.restoredPtr = fresh;
        // Rebuild registration through the same pending-unit path used by the
        // native shot callers. Drain it before resimulation so all restored
        // shots enter the category list in one native registration pass.
        // The native pending queue holds 0x200 entries. Its enqueue function
        // increments the counter even on overflow, which would make the later
        // flush walk beyond the queue buffer. Match the native caller fallback
        // without touching the counter once the queue is full.
        uint32_t pendingCount = *pendingCountPtr;
        if (pendingCount >= 0x200 || !enqueueUnit(sShot, fresh, SSHOT_CATEGORY)) {
            addUnit(sUnit, SSHOT_CATEGORY, fresh, nullptr);
        }
        const uint64_t registerEnd = Profile_Now();
        respawnRegisterTicks += registerEnd - factoryEnd;
        ReleaseFreshEmbeddedShotEffectGraph(fresh);
        const uint64_t releaseEffectsEnd = Profile_Now();
        respawnReleaseEffectsTicks += releaseEffectsEnd - registerEnd;
        const void* validTargets[6] = {};
        size_t validTargetCount = 0;
        uint64_t sCharacter = *(uint64_t*)_addr(PTR_SCHARACTER);
        for (int teamIndex = 0; sCharacter && teamIndex < 2; ++teamIndex) {
            uint64_t node = sCharacter +
                (teamIndex == 0 ? TEAM0_OFFSET : TEAM1_OFFSET) +
                TEAM_FIRST_NODE_OFFSET;
            for (int member = 0; node && member < 3; ++member) {
                void* character = *(void**)(node + NODE_CHR_OFFSET);
                if (character) validTargets[validTargetCount++] = character;
                node = *(uint64_t*)(node + NODE_NEXT_OFFSET);
            }
        }
        if (!ConcreteShotSnapshot_Restore(fresh, saved.concrete, validTargets,
                                          validTargetCount)) {
            HookLog("Rollback: restored shot 0x%llX has incomplete collision topology\n",
                     (uint64_t)fresh);
        }
        const uint64_t concreteEnd = Profile_Now();
        respawnConcreteTicks += concreteEnd - releaseEffectsEnd;
        RestoreKingKobunHelper(fresh, saved);
        if (saved.concrete.dti == (uint64_t)_addr(0x140D4D2B0) &&
            saved.concrete.body.size() >= 0x2164) {
            // These are the controller's scalar mode/phase fields. The broad
            // shot overlay deliberately preserves pointer-looking words, and
            // +0x2158 is fixed separately above; explicitly restore the
            // remaining King Servbot fields so reconstruction cannot leave the
            // fresh factory mode latched at its pre-move state.
            static constexpr size_t kingRanges[][2] = {
                {0x2140, 0x08},
                {0x214C, 0x04},
                {0x2150, 0x08},
                {0x2160, 0x04},
            };
            for (const auto& range : kingRanges) {
                std::memcpy((uint8_t*)fresh + range[0],
                            saved.concrete.body.data() + range[0], range[1]);
            }
        }
        // Concrete snapshots do not reconstruct the global particle-owner
        // graph referenced by inline sEffect objects. Leave that graph
        // detached so later native shot destruction cannot walk stale nodes.
        DetachEmbeddedShotEffect(fresh);

        // Reapply confirmed pointer-free common fields after the broad object
        // overlay so packed floats/flags that resemble addresses still rewind.
        RestoreShotArmState(fresh, saved);
        ApplyShotFlight(fresh, saved.flight);

        // ApplyShotFlight preserves the category assigned by addUnit while
        // restoring the saved gameplay lifecycle bits.
        uint32_t* unitState = (uint32_t*)((uint8_t*)fresh + 0x10);
        *unitState = (*unitState & ~0x3F8u) | (SSHOT_CATEGORY << 3);
        const uint64_t overlayEnd = Profile_Now();
        respawnOverlayTicks += overlayEnd - concreteEnd;
        respawnedShots++;
    }
    const uint64_t respawnEnd = Profile_Now();

    // Native enqueue increments the counter before rejecting an overflow. A
    // concurrent/native spawn during reconstruction can therefore poison it
    // again even though our own calls stop at capacity. Never let flush walk
    // beyond the 512-entry buffer into the activity array at +0x2450.
    if (*pendingCountPtr > 0x200) {
        HookLog("Rollback: clamping pending-shot count %u to 512 before flush\n",
                *pendingCountPtr);
        *pendingCountPtr = 0x200;
    }
    flushPendingUnits(sShot);

    std::memcpy((void*)_addr(0x140d765d8), restoredGlobalRng, sizeof(restoredGlobalRng));
    if (sEffect) std::memcpy((void*)(sEffect + 0x3C), restoredEffectRng, sizeof(restoredEffectRng));
    for (const SavedCharacterRng& characterRng : restoredCharacterRngs) {
        std::memcpy((uint8_t*)characterRng.character + 0x2D80, characterRng.state,
                    sizeof(characterRng.state));
    }
    for (void* resource : pinnedShotResources)
        releaseRef(resource);
    const uint64_t finalizeEnd = Profile_Now();

    if (g_activeRollbackProfile) {
        g_activeRollbackProfile->restoreShotPinResources += pinResourcesEnd - profileStart;
        g_activeRollbackProfile->restoreShotPreSanitize += preSanitizeEnd - pinResourcesEnd;
        g_activeRollbackProfile->restoreShotRetireEffects += retireEffectsEnd - preSanitizeEnd;
        g_activeRollbackProfile->restoreShotCollect += collectShotsEnd - retireEffectsEnd;
        g_activeRollbackProfile->restoreShotDestroy += destroyShotsEnd - collectShotsEnd;
        g_activeRollbackProfile->restoreShotPostSanitize += postSanitizeEnd - destroyShotsEnd;
        g_activeRollbackProfile->restoreShotSanitizeEffects += sanitizeEffectsEnd - postSanitizeEnd;
        g_activeRollbackProfile->restoreShotRespawnSetup += respawnSetupEnd - sanitizeEffectsEnd;
        g_activeRollbackProfile->restoreShotRespawn += respawnEnd - respawnSetupEnd;
        g_activeRollbackProfile->restoreShotRespawnPrepare += respawnPrepareTicks;
        g_activeRollbackProfile->restoreShotRespawnFactory += respawnFactoryTicks;
        g_activeRollbackProfile->restoreShotRespawnRegister += respawnRegisterTicks;
        g_activeRollbackProfile->restoreShotRespawnReleaseEffects += respawnReleaseEffectsTicks;
        g_activeRollbackProfile->restoreShotRespawnConcrete += respawnConcreteTicks;
        g_activeRollbackProfile->restoreShotRespawnOverlay += respawnOverlayTicks;
        g_activeRollbackProfile->restoreShotFinalize += finalizeEnd - respawnEnd;
    }

    HookLog("Rollback: retired shots=%d effects=%d; shot recipes respawned=%d failed=%d\n",
            retiredShots, retiredEffects, respawnedShots, failedShots);
}

// ===========================================================================
// Safe Dynamic List Traversal
//
// Walks an intrusive linked list starting at |listHead|, reading each node's
// DTI to determine object size.  Bounded by DYNAMIC_SAFETY to prevent
// infinite loops on corrupted lists.
// ===========================================================================
static void TraverseAndAddDynamicList(uint64_t listHead, size_t nextOffset,
                                      int& r, Region* regions, int maxRegions)
{
    uint64_t current = listHead;
    int safety = DYNAMIC_SAFETY;

    while (current && r < maxRegions && safety-- > 0) {
        // Reject obviously-bogus pointers
        if (current < 0x10000) break;

        // Read object size from DTI (accessed via vtable[4], not object+0x08)
        uint64_t dtiPtr = GetObjectDti(current);
        uint32_t objSize = DecodeDtiSize(dtiPtr);
        if (objSize == 0 || objSize > MAX_CHAR_SIZE) {
            objSize = 0x400; // conservative fallback for typical nodes
        }

        regions[r].ptr  = (void*)current;
        regions[r].size = objSize;
        regions[r].ownerSlot = 0;
        regions[r].expectedVtable = 0;
        regions[r].skipVtable = false;
        ++r;

        current = *(uint64_t*)(current + nextOffset);
    }
}

// ===========================================================================
// Region List Builder — enumerates all snapshot-worthy memory
// ===========================================================================
static int BuildRegions(Region* regions, int maxRegions)
{
    if (!regions || maxRegions < 1) return 0;

    int r = 0;
    auto add = [&](void* ptr, size_t sz, uint64_t ownerSlot = 0,
                   uint64_t expectedVtable = 0, bool skipVtable = false) {
        if (r < maxRegions && ptr && sz > 0) {
            regions[r].ptr  = ptr;
            regions[r].size = sz;
            regions[r].ownerSlot = ownerSlot;
            regions[r].expectedVtable = expectedVtable;
            regions[r].skipVtable = skipVtable;
            ++r;
        }
    };

    int collisionExternalRegions = 0;
    size_t collisionExternalBytes = 0;
    struct CollisionDiscoveryDiag {
        uint64_t sCollision = 0;
        uint64_t sbc = 0;
        uint64_t moveMatrix = 0;
        uint64_t activeData = 0;
        uint32_t activeCount = 0;
        uint32_t activeCapacity = 0;
        uint64_t moveCategoryArray = 0;
        uint32_t moveCategoryCount = 0;
        uint32_t bvhVisited = 0;
        uint32_t bvhPoolPresent = 0;
        uint32_t bvhPoolHeaderAccessible = 0;
        uint32_t bvhPoolCapacityValid = 0;
        uint32_t bvhFreePresent = 0;
        uint32_t bvhFreeHeaderValid = 0;
        uint32_t bvhReachableNodes = 0;
        uint32_t bvhStandaloneNodes = 0;
        uint32_t sbcQueueHeadersValid = 0;
        uint32_t sbcQueueDataPresent = 0;
        uint32_t moveQueuesPresent = 0;
        uint32_t moveThreadHeadersValid = 0;
    } collisionDiag;

    auto logOwnedBufferRejection = [&](const char* label, uint64_t ownerSlot,
                                       uint64_t ptr, size_t size) {
        return;
        static std::unordered_map<std::string, std::string> lastReasons;
        const char* reason = nullptr;
        if (!ownerSlot) reason = "null owner slot";
        else if (!ptr) reason = "null data pointer";
        else if (!size) reason = "zero size";
        else if (size > 0x1000000) reason = "size exceeds limit";
        else if (!IsAccessiblePointer(ptr)) reason = "start inaccessible";
        else if (!IsAccessibleAddress(ptr + size - 1)) reason = "end inaccessible";
        if (!reason) return;

        char state[192];
        std::snprintf(state, sizeof(state),
                      "%s owner=0x%llX ptr=0x%llX size=%zu",
                      reason, ownerSlot, ptr, size);
        std::string& previous = lastReasons[label];
        if (previous != state) {
            HookLog("Rollback: sCollision discovery reject %s: %s\n", label, state);
            previous = state;
        }
    };

    auto addOwnedBuffer = [&](uint64_t ownerSlot, uint64_t ptr, size_t size) {
        if (!ownerSlot || !ptr || !size || size > 0x1000000 ||
            !IsAccessiblePointer(ptr) || !IsAccessibleAddress(ptr + size - 1)) {
            return;
        }
        if (r >= maxRegions) return;
        add((void*)ptr, size, ownerSlot);
        ++collisionExternalRegions;
        collisionExternalBytes += size;
    };

    std::unordered_set<uint64_t> capturedStandaloneBvhNodes;
    auto addDynamicBvhBuffers = [&](uint64_t bvh, uint32_t bank, uint32_t index) {
        ++collisionDiag.bvhVisited;
        if (!bvh || !IsAccessiblePointer(bvh)) {
            if (index == 0)
                HookLog("Rollback: sCollision discovery BVH bank=0x%X base=0x%llX inaccessible\n",
                        bank, bvh);
            return;
        }

        // resizeNodePool allocates a 0x10-byte allocator header followed by
        // 0x40-byte nodes and stores the first node in +0x18. Do not rewind
        // allocator metadata; capture the node payload itself. The +0x20
        // array contains pointers into that pool and defines free-list order.
        uint64_t pool = *(uint64_t*)(bvh + 0x18);
        if (pool) ++collisionDiag.bvhPoolPresent;
        uint32_t poolCapacity = 0;
        if (pool && IsAccessiblePointer(pool - 0x10)) {
            ++collisionDiag.bvhPoolHeaderAccessible;
            poolCapacity = *(uint32_t*)(pool - 0x10);
        }
        if (poolCapacity > 0 && poolCapacity <= 0x10000) {
            ++collisionDiag.bvhPoolCapacityValid;
            addOwnedBuffer(bvh + 0x18, pool,
                           (size_t)poolCapacity * SCOLLISION_NODE_SIZE);
        } else if (pool && index == 0) {
            HookLog("Rollback: sCollision discovery BVH bank=0x%X pool=0x%llX capacity=%u headerAccessible=%d\n",
                    bank, pool, poolCapacity, IsAccessiblePointer(pool - 0x10));
        }

        uint32_t freeCount = *(uint32_t*)(bvh + 0x28);
        uint32_t freeCapacity = *(uint32_t*)(bvh + 0x2C);
        uint64_t freeNodes = *(uint64_t*)(bvh + 0x20);
        if (freeNodes) ++collisionDiag.bvhFreePresent;
        if (freeCount <= freeCapacity && freeCapacity <= 0x10000) {
            ++collisionDiag.bvhFreeHeaderValid;
            addOwnedBuffer(bvh + 0x20, freeNodes, (size_t)freeCapacity * 8);
        } else if (index == 0) {
            HookLog("Rollback: sCollision discovery BVH bank=0x%X freeNodes=0x%llX count=%u capacity=%u\n",
                    bank, freeNodes, freeCount, freeCapacity);
        }

        // A second MtArray at +0x30 tracks nodes not currently assigned to
        // the reserved pool. It has its own external pointer array at +0x48.
        uint32_t noUseCount = *(uint32_t*)(bvh + 0x38);
        uint32_t noUseCapacity = *(uint32_t*)(bvh + 0x3C);
        uint64_t noUseNodes = *(uint64_t*)(bvh + 0x48);
        if (noUseCount <= noUseCapacity && noUseCapacity <= 0x10000) {
            addOwnedBuffer(bvh + 0x48, noUseNodes,
                           (size_t)noUseCapacity * 8);
        }

        // popNodeOrAlloc falls back to individually allocated 0x40-byte nodes
        // after both pointer arrays are exhausted. Walk the live tree and
        // capture every reachable node outside this BVH's fixed pool. The
        // parent/root field is the owner slot, so restore is skipped if the
        // live topology no longer references the saved allocation.
        struct PendingBvhNode { uint64_t ownerSlot; uint64_t node; };
        std::vector<PendingBvhNode> pending;
        uint64_t root = *(uint64_t*)(bvh + 0x08);
        if (root) pending.push_back({bvh + 0x08, root});
        std::unordered_set<uint64_t> visited;
        while (!pending.empty() && visited.size() < 0x10000) {
            PendingBvhNode current = pending.back();
            pending.pop_back();
            if (!current.node || !IsAccessiblePointer(current.node) ||
                !visited.insert(current.node).second) {
                continue;
            }
            ++collisionDiag.bvhReachableNodes;

            bool inFixedPool = pool && poolCapacity > 0 &&
                current.node >= pool &&
                current.node < pool + (size_t)poolCapacity * SCOLLISION_NODE_SIZE &&
                ((current.node - pool) % SCOLLISION_NODE_SIZE) == 0;
            if (!inFixedPool && capturedStandaloneBvhNodes.insert(current.node).second) {
                addOwnedBuffer(current.ownerSlot, current.node, SCOLLISION_NODE_SIZE);
                ++collisionDiag.bvhStandaloneNodes;
            }

            uint64_t child0 = *(uint64_t*)(current.node + 0x10);
            uint64_t child1 = *(uint64_t*)(current.node + 0x18);
            if (child0) pending.push_back({current.node + 0x10, child0});
            if (child1) pending.push_back({current.node + 0x18, child1});
        }
    };

    auto addNodeListBuffers = [&](uint64_t nodeList) {
        if (!nodeList || !IsAccessiblePointer(nodeList)) return;

        // NodeList contains seven 41-entry MtArray groups, four singleton
        // MtArrays, and two embedded dynamic BVHs. The object itself is
        // captured separately; these are its heap-backed pointer arrays.
        static constexpr uint32_t arrayBases[] = {
            0x18, 0x538, 0xA58, 0x1028, 0x1548, 0x1A68, 0x1F88
        };
        for (uint32_t base : arrayBases) {
            for (uint32_t category = 0; category < 0x29; ++category) {
                uint64_t header = nodeList + base + category * 0x20;
                uint32_t count = *(uint32_t*)(header + 0x08);
                uint32_t capacity = *(uint32_t*)(header + 0x0C);
                uint64_t data = *(uint64_t*)(header + 0x18);
                if (count <= capacity && capacity <= 0x10000) {
                    addOwnedBuffer(header + 0x18, data, (size_t)capacity * 8);
                }
            }
        }

        for (uint32_t headerOffset : {0xF78u, 0xF98u, 0x24A8u, 0x24C8u}) {
            uint64_t header = nodeList + headerOffset;
            uint32_t count = *(uint32_t*)(header + 0x08);
            uint32_t capacity = *(uint32_t*)(header + 0x0C);
            uint64_t data = *(uint64_t*)(header + 0x18);
            if (count <= capacity && capacity <= 0x10000) {
                addOwnedBuffer(header + 0x18, data, (size_t)capacity * 8);
            }
        }

        addDynamicBvhBuffers(nodeList + 0xFC0, 0xFC0, 0);
        addDynamicBvhBuffers(nodeList + 0x24E8, 0x24E8, 0);
    };

    auto addSbcObjectBuffers = [&](uint64_t sbc) {
        collisionDiag.sbc = sbc;
        if (!sbc || !IsAccessiblePointer(sbc)) {
            HookLog("Rollback: sCollision discovery SbcObject=0x%llX inaccessible\n", sbc);
            return;
        }

        // Active registration array: MtArray header at +0x40, data at +0x58.
        uint32_t activeCount = *(uint32_t*)(sbc + 0x48);
        uint32_t activeCapacity = *(uint32_t*)(sbc + 0x4C);
        uint64_t activeData = *(uint64_t*)(sbc + 0x58);
        collisionDiag.activeCount = activeCount;
        collisionDiag.activeCapacity = activeCapacity;
        collisionDiag.activeData = activeData;
        if (activeCount <= activeCapacity && activeCapacity <= 0x10000) {
            addOwnedBuffer(sbc + 0x58, activeData,
                           (size_t)activeCapacity * 8);
            if (activeCapacity > 0)
                logOwnedBufferRejection("SbcObject.active", sbc + 0x58, activeData,
                                        (size_t)activeCapacity * 8);
        } else {
            HookLog("Rollback: sCollision discovery active invalid count=%u capacity=%u data=0x%llX\n",
                    activeCount, activeCapacity, activeData);
        }

        // Dirty and merge queues are 41 MtArray-derived NodeList headers with
        // a 0x20-byte stride. These offsets start at the count field: capacity
        // is +0x04, ownership is +0x08, and data is +0x10.
        for (uint32_t base : {0x70u, 0x590u}) {
            for (uint32_t category = 0; category < 0x29; ++category) {
                uint64_t header = sbc + base + category * 0x20;
                uint32_t count = *(uint32_t*)(header + 0x00);
                uint32_t capacity = *(uint32_t*)(header + 0x04);
                uint64_t data = *(uint64_t*)(header + 0x10);
                if (count <= capacity && capacity <= 0x10000) {
                    ++collisionDiag.sbcQueueHeadersValid;
                    if (data) ++collisionDiag.sbcQueueDataPresent;
                    addOwnedBuffer(header + 0x10, data, (size_t)capacity * 8);
                    if (capacity > 0 && category == 0)
                        logOwnedBufferRejection(base == 0x70 ? "SbcObject.dirty[0]" :
                                                 "SbcObject.merge[0]",
                                                 header + 0x10, data,
                                                 (size_t)capacity * 8);
                } else if (category == 0) {
                   //HookLog("Rollback: sCollision discovery queue base=0x%X count=%u capacity=%u data=0x%llX\n",
                            //base, count, capacity, data);
                }
                if (data && count <= capacity && capacity <= 0x10000) {
                    for (uint32_t entry = 0; entry < count; ++entry) {
                        uint64_t recordSlot = data + (size_t)entry * 8;
                        uint64_t record = *(uint64_t*)recordSlot;
                        addOwnedBuffer(recordSlot, record, 0x28);
                    }
                }
            }
        }
    };

    auto addMoveMatrixBuffers = [&](uint64_t moveMatrix) {
        collisionDiag.moveMatrix = moveMatrix;
        if (!moveMatrix || !IsAccessiblePointer(moveMatrix)) {
            //HookLog("Rollback: sCollision discovery moveMatrix=0x%llX inaccessible\n",
            //        moveMatrix);
            return;
        }
        uint32_t categoryCount = *(uint32_t*)(moveMatrix + 0x10);
        uint32_t categoryCapacity = *(uint32_t*)(moveMatrix + 0x14);
        uint64_t categoryArray = *(uint64_t*)(moveMatrix + 0x20);
        collisionDiag.moveCategoryCount = categoryCount;
        collisionDiag.moveCategoryArray = categoryArray;
        if (categoryCount > categoryCapacity || categoryCapacity > 0x40 ||
            !categoryArray ||
            !IsAccessiblePointer(categoryArray)) {
            //HookLog("Rollback: sCollision discovery move categories count=%u capacity=%u array=0x%llX accessible=%d\n",
            //        categoryCount, categoryCapacity, categoryArray,
            //        IsAccessiblePointer(categoryArray));
            return;
        }

        addOwnedBuffer(moveMatrix + 0x20, categoryArray,
                       (size_t)categoryCapacity * 8);

        // Each category points to an MtArray<NodeList*>. Its backing array
        // selects one full 0x2550-byte NodeList per frame/thread slot.
        for (uint32_t category = 0; category < categoryCount; ++category) {
            uint64_t categorySlot = categoryArray + (size_t)category * 8;
            uint64_t queue = *(uint64_t*)categorySlot;
            if (!queue || !IsAccessiblePointer(queue)) continue;
            ++collisionDiag.moveQueuesPresent;
            uint32_t threadCount = *(uint32_t*)(queue + 8);
            uint32_t threadCapacity = *(uint32_t*)(queue + 0xC);
            uint64_t threadData = *(uint64_t*)(queue + 0x18);
            if (threadCount > threadCapacity || threadCapacity > 0x40) {
                if (category == 0)
                    //HookLog("Rollback: sCollision discovery move queue=0x%llX threadCount=%u capacity=%u\n",
                    //        queue, threadCount, threadCapacity);
                continue;
            }
            addOwnedBuffer(queue + 0x18, threadData,
                           (size_t)threadCapacity * 8);
            for (uint32_t thread = 0; thread < threadCount; ++thread) {
                uint64_t nodeListSlot = threadData + (size_t)thread * 8;
                uint64_t nodeList = *(uint64_t*)nodeListSlot;
                if (!nodeList || !IsAccessiblePointer(nodeList)) continue;
                ++collisionDiag.moveThreadHeadersValid;
                addOwnedBuffer(nodeListSlot, nodeList, 0x2550);
                addNodeListBuffers(nodeList);
            }
        }
    };

    // ---- 1. sReplay: frame counter only (sReplay+0x54). Full replay buffer
    //     has internal pointers that go stale. ---
    uint64_t sReplay = *(uint64_t*)_addr(PTR_SREPLAY);
    if (sReplay) {
        add((void*)(sReplay + 0x54), 4); // just the frame counter
    }

    // ---- 1a. Global XorShift RNG state (16 bytes, 4 × uint32) ----
    //     Used by 64/73 call sites across gameplay (hit roll, damage variance,
    //     AI decisions, projectile patterns).  Without restoring this, re-simulation
    //     with different inputs consumes RNG differently, causing desyncs in all
    //     RNG-derived behavior between the original and re-simulated timelines.
    //     Address confirmed from LEA RAX,[0x140d765d8] in MtXorShift__getGlobalState.
    add((void*)_addr(0x140d765d8), 16);

    // ---- 1b. sEffect per-instance RNG (sEffect+0x3C, 16 bytes) ----
    //     Second within-round RNG source (rng_inventory.md §4.2/§5.1): seeded at
    //     sEffect ctor (seed 426772233) and reseeded by sReplay__onRoundStart from
    //     the round seed, then advances within the round.  g_sEffect__mpInstance
    //     global at 0x140E17A38 (returned by aNextBattle__createProperty_4730).
    //     Determinism harness invariant per rng_inventory.md §6.3: sEffect+0x3C
    //     must match saved value byte-for-byte after restore.
    {
        uint64_t sEffect = *(uint64_t*)_addr(0x140E17A38);
        if (sEffect) {
            add((void*)(sEffect + 0x3C), 16);
        }
    }

    // ---- 2. sShot manager header and shot ID pool ----
    // Keep intrusive sentinel topology live; recipes reconcile shot lifetime.
    uint64_t sShot = *(uint64_t*)_addr(PTR_SSHOT);
    if (sShot) {
        add((void*)sShot, 0x50);
    }

    // ---- 2a. Slot active flags (NOT in sShot — stored in sGameConfig+0x4A89C) ---
    //     sShot__setSlotActiveFlag writes to uHaggar__getter_49F0(sGameConfig) + 0x48A6c.
    //     uHaggar__getter_49F0 returns sGameConfig + 0x1E30, so the flags live at
    //     sGameConfig + 0x1E30 + 0x48A6c = sGameConfig + 0x4A89C.  10 slots × 1 byte.
    //     Without restoring these the game sees no active projectiles and frees them,
    //     causing stale-address corruption on the next restore.
    {
        uint64_t sGameConfig = *(uint64_t*)_addr(0x140D50F88);
        if (sGameConfig) {
            add((void*)(sGameConfig + 0x4A89C), 10);
        }
    }

    // uShot heap bodies are not copied. Their intrusive links and owned effect
    // pointers cannot safely survive object destruction; native spawn recipes
    // below reconstruct missing shots and their dependent objects.

    // ---- 3. sHitSolver — collision pairs + result buffer (static array) ----
    //     Full body is 0x3808C (sHitSolver_field_map.md §2): 192KB cHitNode pool,
    //     counters at +0x30060, SortBucketA @+0x30070, SortBucketB @+0x34078,
    //     mWorkCnt/B/Final tail @+0x38080.  The previous 0x34080 size truncated
    //     mid-SortBucketB, dropping ~16KB of bucket B and the work counters.
    {
        uint64_t sHitSolver = *(uint64_t*)_addr(PTR_SHITSOLVER);
        add((void*)sHitSolver, SHITSOLVER_SIZE);
    }

    // ---- 4. sAction — battle state machine (static manager) ----
    {
        uint64_t sAction = *(uint64_t*)_addr(PTR_SACTION);
        add((void*)sAction, 0x20000);
    }

    // ---- 5. sGameUI — SKIPPED: has internal ptrs (ui elements, text buffers) ---
    // {
    //     uint64_t sGameUI = *(uint64_t*)_addr(PTR_SGAMEUI);
    //     add((void*)sGameUI, 0x6000);
    // }

    // ---- 6. sMain — physics, timing, frame pacing (needed for re-sim consistency) ----
    {
        uint64_t sMain = *(uint64_t*)_addr(PTR_SMAIN);
        add((void*)sMain, SMAIN_SIZE);
    }

    // ---- 6a. sCollision — inline broadphase, move-matrix and geometry state ----
    // Restore has a dedicated path that retains cSystem synchronization,
    // mPrivateAllocator, and live pointers to externally owned allocations.
    {
        uint64_t sCollision = *(uint64_t*)_addr(PTR_SCOLLISION);
        collisionDiag.sCollision = sCollision;
        add((void*)sCollision, SCOLLISION_SIZE);
        if (sCollision) {
            // Ghidra's sCollision methods address these subsystems directly
            // from the complete singleton. The documented offsets already
            // include the 0x38-byte cSystem base.
            uint64_t sbc = sCollision + 0x43B8;
            uint64_t moveMatrix = sCollision + 0x7368;

            addSbcObjectBuffers(sbc);
            addMoveMatrixBuffers(moveMatrix);

            // cSbcArrayBP has five independent banks of 31 dynamic BVHs.
            // Their headers are inline in sCollision, but their node pools and
            // free-node arrays are allocated externally.
            for (uint32_t bank : {0x2B8u, 0xFB8u, 0x1CB8u, 0x29B8u, 0x36B8u}) {
                for (uint32_t index = 0; index < SCOLLISION_BVH_COUNT; ++index)
                    addDynamicBvhBuffers(sCollision + bank + index * 0x68, bank, index);
            }

            // cSbcObject owns two banks of 31 dynamic BVHs. The same helper
            // captures their node pools and free-node pointer arrays.
            for (uint32_t bank : {0xAA8u, 0x17A8u}) {
                for (uint32_t index = 0; index < SCOLLISION_BVH_COUNT; ++index)
                    addDynamicBvhBuffers(sbc + bank + index * 0x68, bank, index);
            }

            // Character collision attachment wrappers own dynamic BVHs and
            // NodeList-backed child state outside the singleton. Their child
            // graph is already cloned; capture the allocation's scalar image
            // as well when the live owner still points to it.
            uint64_t character = *(uint64_t*)_addr(PTR_SCHARACTER);
            if (character) {
                for (uint32_t teamOffset : {TEAM0_OFFSET, TEAM1_OFFSET}) {
                    uint64_t node = character + teamOffset + TEAM_FIRST_NODE_OFFSET;
                    for (uint32_t n = 0; node && n < 8; ++n) {
                        uint64_t chr = *(uint64_t*)(node + NODE_CHR_OFFSET);
                        if (chr) {
                            for (uint32_t offset : CCHR_EXTERNAL_ANIM_OFFSETS) {
                                uint64_t wrapper = *(uint64_t*)(chr + offset);
                                if (wrapper) {
                                    // Wrapper scalar state is already covered
                                    // by the 0xE0 region. Its child arrays are
                                    // intentionally left to the deep clone.
                                }
                            }
                        }
                        node = *(uint64_t*)(node + NODE_NEXT_OFFSET);
                    }
                }
            }
        }
        static int lastCollisionExternalRegions = -1;
        static size_t lastCollisionExternalBytes = 0;
        if (collisionExternalRegions != lastCollisionExternalRegions ||
            collisionExternalBytes != lastCollisionExternalBytes) {
            /*HookLog("Rollback: sCollision external discovery regions=%d bytes=%zu\n",
                    collisionExternalRegions, collisionExternalBytes);
            HookLog("Rollback: sCollision discovery summary singleton=0x%llX sbc=0x%llX "
                    "active=%u/%u@0x%llX queues=%u/%u bvh=%u nodes=%u standalone=%u pool=%u/%u/%u "
                    "free=%u/%u move=0x%llX categories=%u@0x%llX queues=%u threads=%u\n",
                    collisionDiag.sCollision, collisionDiag.sbc,
                    collisionDiag.activeCount, collisionDiag.activeCapacity,
                    collisionDiag.activeData,
                    collisionDiag.sbcQueueDataPresent,
                    collisionDiag.sbcQueueHeadersValid,
                    collisionDiag.bvhVisited,
                    collisionDiag.bvhReachableNodes,
                    collisionDiag.bvhStandaloneNodes,
                    collisionDiag.bvhPoolPresent,
                    collisionDiag.bvhPoolHeaderAccessible,
                    collisionDiag.bvhPoolCapacityValid,
                    collisionDiag.bvhFreePresent,
                    collisionDiag.bvhFreeHeaderValid,
                    collisionDiag.moveMatrix,
                    collisionDiag.moveCategoryCount,
                    collisionDiag.moveCategoryArray,
                    collisionDiag.moveQueuesPresent,
                    collisionDiag.moveThreadHeadersValid);*/
            lastCollisionExternalRegions = collisionExternalRegions;
            lastCollisionExternalBytes = collisionExternalBytes;
        }
    }

    // ---- 7. sCamera — view matrices, 8-slot camera stack, cutin state ----
    //     3456 B total; skip vtable @+0 .. +7 on restore (handled via flat
    //     memcpy — the vtable pointer is the same at save and restore time
    //     for a singleton, so restoring it is safe here).
    uint64_t camUnit = 0;
    {
        uint64_t sCamera = *(uint64_t*)_addr(PTR_SCAMERA);
        if (sCamera) {
            add((void*)sCamera, 0xD80);
            // First viewport slot at +0x50, stride 0x1A0, camera unit ptr at slot+8.
            // This is the uActionSideCamera gameplay anchor unit.
            camUnit = *(uint64_t*)(sCamera + 0x58);
        }
    }

    // ---- 7a. uActionSideCamera — gameplay camera anchor/smoothing unit ----
    //     cUnit-derived object (DTI 0x140d44268, size 0x590) that computes
    //     camera anchor position, smoothing trajectory, and screen-edge
    //     clamps each frame.  During host-only resim its internal state
    //     drifts from the live client, causing permanent character position
    //     drift (desync) that never self-corrects.  sCamera viewport[0]+8
    //     holds the pointer (confirmed via sCamera__refreshViewsForBoundary).
    {
        if (camUnit) {
            uint64_t dtiPtr  = GetObjectDti(camUnit);
            uint32_t camSize = DecodeDtiSize(dtiPtr);
            if (camSize < 0x100 || camSize > 0x2000) {
                camSize = 0x590; // known DTI size for uActionSideCamera
            }
            add((void*)camUnit, camSize);
        }
    }

    // ---- 8. sGameEffect — SKIPPED: individual effect nodes are captured ---
    //     via the sUnit category traversal (categories 13-16) below.  Flat-
    //     restoring sGameEffect would crash due to stale effect pointers.

    // ---- 10. sAction (fighting phase state machine) ----
    uint64_t sMvc3Mgr = *(uint64_t*)_addr(PTR_SMVC3MGR);
    if (sMvc3Mgr) {
        uint64_t sAction = *(uint64_t*)(sMvc3Mgr + SMVC3MGR_ACTION_OFFSET);
        add((void*)sAction, 0x300);
        if (sAction && IsAccessibleAddress(
                sAction + SACTION_MODEL_JOINT_WRAPPER_OFFSET)) {
            uint64_t modelJointWrapper = *(uint64_t*)(
                sAction + SACTION_MODEL_JOINT_WRAPPER_OFFSET);
            if (modelJointWrapper && IsAccessibleAddress(
                    modelJointWrapper + SACTION_MODEL_JOINT_ALLOCATION_OFFSET)) {
                uint64_t allocationSlot = modelJointWrapper +
                    SACTION_MODEL_JOINT_ALLOCATION_OFFSET;
                uint64_t allocation = *(uint64_t*)allocationSlot;
                if (allocation && IsAccessibleAddress(
                        allocation + SACTION_MODEL_JOINT_ALLOCATION_SIZE - 1)) {
                    // sAction__getModelJoint returns allocation+0x10. Capture
                    // the allocation for ownership validation; restore skips
                    // its header and applies only the scalar payload.
                    add((void*)allocation,
                        SACTION_MODEL_JOINT_ALLOCATION_SIZE,
                        allocationSlot);
                }
            }
        }
    }

    // ---- 11-16. Per-character uCharacter + 17-18. cChrTeam ----
    uint64_t sCharacter = *(uint64_t*)_addr(PTR_SCHARACTER);
    if (sCharacter) {
        uint64_t team0 = sCharacter + TEAM0_OFFSET;
        uint64_t team1 = sCharacter + TEAM1_OFFSET;
        add((void*)team0, TEAM_SIZE);
        add((void*)team1, TEAM_SIZE);

        uint64_t teamCharacters[6] = {};
        int teamCharacterCount = 0;
        for (int ti = 0; ti < 2; ++ti) {
            uint64_t team = sCharacter + (ti == 0 ? TEAM0_OFFSET : TEAM1_OFFSET);
            uint64_t node = team + TEAM_FIRST_NODE_OFFSET;

            while (node && r < maxRegions) {
                uint64_t character = *(uint64_t*)(node + NODE_CHR_OFFSET);
                if (character) {
                    if (teamCharacterCount < 6) {
                        teamCharacters[teamCharacterCount++] = character;
                    }
                    uint64_t vtable = *(uint64_t*)character;
                    uint64_t dtiFunc = (vtable >= 0x140000000) ? *(uint64_t*)(vtable + 0x20) : 0;
                    uint64_t dtiPtr = GetObjectDti(character);
                    uint32_t charSize = DecodeDtiSize(dtiPtr);
                    uint32_t finalSize = charSize;
                    if (finalSize < CCHR_NOMINAL_SIZE || finalSize > CCHR_MAX_SIZE) {
                        finalSize = CCHR_NOMINAL_SIZE;
                    }
                    uint32_t* animFields = (uint32_t*)(character + 0x1300);
                   // HookLog("DIAG Character[team %d]: addr=0x%llX vtable=0x%llX dtiFunc=0x%llX dtiPtr=0x%llX rawSize=%u finalSize=%u\n"
                 //           "DIAG   anim[+0x1300]: %08X %08X %08X %08X  %08X %08X %08X %08X  %08X %08X %08X %08X  %08X %08X %08X %08X\n",
                 //           ti, character, vtable, dtiFunc, dtiPtr, (unsigned)charSize, (unsigned)finalSize,
                 //           animFields[0], animFields[1], animFields[2], animFields[3],
                //            animFields[4], animFields[5], animFields[6], animFields[7],
                 //           animFields[8], animFields[9], animFields[10], animFields[11],
                 //           animFields[12], animFields[13], animFields[14], animFields[15]);
                    add((void*)character, finalSize, 0, vtable, true);

                    // processTarget1 indexes these seven heap-backed tables
                    // using the packed counts at +0x1440 and immediately calls
                    // virtual +0x40 on each entry. Rewind each active table
                    // with its owning slot so its entries stay synchronized
                    // with the restored count.
                    static constexpr uint32_t targetOffsets[] = {
                        0x1408, 0x1410, 0x1418, 0x1420,
                        0x1428, 0x1430, 0x1438,
                    };
                    static constexpr uint32_t targetStrides[] = {
                        0xBB0, 0xBB0, 0xB90, 0xB90,
                        0xBB0, 0xBB0, 0xB80,
                    };
                    uint32_t packedTargetCounts = *(uint32_t*)(character + 0x1440);
                    for (size_t target = 0;
                         target < sizeof(targetOffsets) / sizeof(targetOffsets[0]);
                         ++target) {
                        uint32_t count = (packedTargetCounts >> (target * 4)) & 0xF;
                        uint64_t ownerSlot = character + targetOffsets[target];
                        uint64_t table = *(uint64_t*)ownerSlot;
                        if (table && count > 0 && count <= 10) {
                            add((void*)table,
                                (size_t)count * targetStrides[target],
                                ownerSlot);
                        }
                    }

                    // Restore motion state and its external interpolation data
                    // together. Keeping one live while rewinding the other
                    // leaves animation/root-motion state split across frames.
                    uint32_t numJoints = *(uint32_t*)(character + CCHR_NUM_JOINTS_OFFSET);
                    if (numJoints <= 256) {
                        size_t blendSize = CCHR_BLEND_BYTES_PER_JOINT * numJoints;
                        for (int motion = 0; motion < CCHR_MOTION_COUNT; ++motion) {
                            uint64_t ownerSlot = character + CCHR_MOTION_BASE +
                                motion * CCHR_MOTION_STRIDE + CCHR_BLEND_PTR_OFFSET;
                            uint64_t blend = *(uint64_t*)ownerSlot;
                            if (blend && blendSize > 0) {
                                add((void*)blend, blendSize, ownerSlot);
                            }
                        }
                    }

                    // Motion+0x178 fields are slices into one allocation owned
                    // by uModel+0x6D8, not independent allocations. Rewind the
                    // complete allocation through its actual owner slot.
                    uint32_t channelCount = *(uint32_t*)(character +
                        CCHR_MOTION_CHANNEL_COUNT_OFFSET);
                    uint32_t motionCount = *(uint32_t*)(character +
                        CCHR_MOTION_COUNT_OFFSET);
                    if (channelCount <= 4096 && motionCount <= CCHR_MOTION_COUNT) {
                        size_t channelBytes = CCHR_MOTION_CHANNEL_STRIDE *
                            channelCount * motionCount;
                        uint64_t ownerSlot = character + CCHR_MOTION_CHANNELS_PTR_OFFSET;
                        uint64_t channels = *(uint64_t*)ownerSlot;
                        if (channels && channelBytes > 0) {
                            add((void*)channels, channelBytes, ownerSlot);
                        }
                    }

                    // These heap buffers are outputs and working state of the
                    // same motion evaluation pass. Leaving them ahead while
                    // rewinding Motion/channel state splits root and joint
                    // transforms across timelines.
                    if (numJoints <= 256 && numJoints > 0) {
                        uint64_t jointStateOwner = character + CCHR_JOINT_STATE_PTR_OFFSET;
                        uint64_t jointStates = *(uint64_t*)jointStateOwner;
                        if (jointStates) {
                            add((void*)jointStates,
                                CCHR_JOINT_STATE_STRIDE * numJoints,
                                jointStateOwner);
                        }

                        uint64_t jointIndexOwner = character + CCHR_JOINT_INDEX_PTR_OFFSET;
                        uint64_t jointIndices = *(uint64_t*)jointIndexOwner;
                        if (jointIndices) {
                            add((void*)jointIndices, numJoints, jointIndexOwner);
                        }

                        uint64_t blendedOwner = character + CCHR_BLENDED_JOINT_PTR_OFFSET;
                        uint64_t blendedJoints = *(uint64_t*)blendedOwner;
                        if (blendedJoints) {
                            add((void*)blendedJoints,
                                CCHR_BLENDED_JOINT_STRIDE * numJoints,
                                blendedOwner);
                        }
                    }

                    for (uint32_t animOffset : CCHR_EXTERNAL_ANIM_OFFSETS) {
                        uint64_t ownerSlot = character + animOffset;
                        uint64_t anim = *(uint64_t*)ownerSlot;
                        if (anim && HasCallableDtiVirtual((void*)anim)) {
                            add((void*)anim, CCHR_EXTERNAL_ANIM_SIZE, ownerSlot);
                        }
                    }

                    // cChrInput is embedded inside uCharacter at +0x21C8.
                    // The full cChrInput struct is ~0x5A0 bytes covering all
                    // ring buffers (mHeldRing, mReleasedRing, mPressedRing),
                    // direction history (512B at +0x360), final processed fields,
                    // and the +0x590/+0x594 fields read by getInputDirection.
                    // Snapshot the entire range so direction-history and
                    // ring-buffer-head state survive rollback restores.
                    uint64_t cChrInput = character + 0x21C8;
                    add((void*)cChrInput, 0x5A0);

                    // ---- cChrCmd line buffers (heap-allocated per-command match state) ----
                    // Two embedded cChrCmd objects at uCharacter+0x2738 and +0x2950.
                    // Each has a heap-allocated line array at cmd+0x178 of n×0x20 B entries
                    // (where n = cmd+0x170), with an 8-B count header at lines-8.
                    // Without this, double-tap dash / back-dash / charge / combo command
                    // matching progress is lost on snapshot restore → resim eats different
                    // action transitions.  Verified via Ghidra cross-refs from cChrCmd__setOwner.
                    for (uint64_t cmdOff : {0x2738ull, 0x2950ull}) {
                        uint64_t cmd   = character + cmdOff;
                        uint64_t lines = *(uint64_t*)(cmd + 0x178);
                        int      n     = *(int*)(cmd + 0x170);
                        if (lines && n > 0 && n < 0x1000)
                            add((void*)(lines - 8), (size_t)n * 0x20 + 8);
                    }
                }
                node = *(uint64_t*)(node + NODE_NEXT_OFFSET);
            }
        }

        // Spawned helper uCharacters live in sUnit category 8 but are not in
        // either team roster. Capture their DTI-sized bodies at saved addresses;
        // blend and external animation buffers remain team-fighter-only.
        uint64_t sUnit = *(uint64_t*)_addr(PTR_SUNIT);
        if (sUnit && *(uint32_t*)(sUnit + SUNIT_CATEGORY_COUNT_OFFSET) > SUNIT_CHARACTER_CATEGORY) {
            uint64_t helper = *(uint64_t*)(sUnit + SUNIT_CATEGORY_LISTS_OFFSET +
                SUNIT_CATEGORY_LIST_STRIDE * SUNIT_CHARACTER_CATEGORY);
            int safety = 64;
            while (helper && safety-- > 0 && r < maxRegions) {
                bool isTeamCharacter = false;
                for (int i = 0; i < teamCharacterCount; ++i) {
                    if (teamCharacters[i] == helper) {
                        isTeamCharacter = true;
                        break;
                    }
                }
                if (!isTeamCharacter) {
                    uint32_t helperSize = DecodeDtiSize(GetObjectDti(helper));
                    if (helperSize >= 8 && helperSize <= CCHR_MAX_SIZE) {
                        add((void*)helper, helperSize, 0, *(uint64_t*)helper, true);
                    }
                }
                helper = *(uint64_t*)(helper + 0x18);
            }
        }
    }

    // ---- 19-22. sUnit effect categories — SKIPPED (heap corruption) ---
    //     Walking effect categories 13-16 and restoring individual nodes at
    //     their heap addresses corrupts the pool allocator's free-list metadata
    //     when nodes were freed between save and restore (MtHeapAllocator crash).
    //     Instead, we zero the sGameEffect slot tables post-restore to allow
    //     fresh effect allocations without conflicting with old slots.

    return r;
}

void NDJSON_GetRegions(std::vector<NDJSON_Region>& regions)
{
    Region snapshotRegions[MAX_REGIONS];
    int count = BuildRegions(snapshotRegions, MAX_REGIONS);
    regions.clear();
    regions.reserve(count);
    for (int i = 0; i < count; ++i)
        regions.push_back({snapshotRegions[i].ptr, snapshotRegions[i].size});
}

bool NDJSON_GetShotRecipe(void* shot, NDJSON_ShotRecipe& recipe)
{
    auto found = g_liveShotRecipes.find(shot);
    if (found == g_liveShotRecipes.end()) return false;
    recipe.slotId = found->second.slotId;
    recipe.initParam = found->second.initParam;
    recipe.ownerChr = (uint64_t)found->second.ownerChr;
    recipe.shotElement = (uint64_t)found->second.shotElement;
    recipe.extended = found->second.kind == ShotSpawnKind::Extended;
    return true;
}

// ===========================================================================
// Public API — Save / Restore / ReSimulate
// ===========================================================================

void Rollback_SaveSnapshot(int32_t frame)
{
    const uint64_t profileStart = Profile_Now();
    Region regions[MAX_REGIONS];
    int count = BuildRegions(regions, MAX_REGIONS);
    if (count == 0) return;
    const uint64_t buildRegionsEnd = Profile_Now();

    size_t total = 0;
    for (int i = 0; i < count; ++i) {
        total += regions[i].size;
    }

    int slotIdx = frame % ROLLBACK_WINDOW;
    if (slotIdx < 0) slotIdx += ROLLBACK_WINDOW;
    SnapshotSlot& slot = g_slots[slotIdx];

    DestroyCollisionNodeClones(slot);

    if (slot.capacity < total) {
        delete[] slot.data;
        slot.data = new (std::nothrow) uint8_t[total];
        if (!slot.data) {
            HookLog("Rollback: failed to allocate %zu-byte snapshot for frame %d\n",
                    total, frame);
            slot.capacity    = 0;
            slot.size        = 0;
            slot.regionCount = 0;
            return;
        }
        slot.capacity = total;
    }
    const uint64_t allocationEnd = Profile_Now();

    // Copy data AND save per-region metadata for safe restore
    size_t offset = 0;
    slot.regionCount = count;
    slot.frame = frame;
    for (int i = 0; i < count; ++i) {
        slot.regions[i].ptr  = regions[i].ptr;
        slot.regions[i].size = regions[i].size;
        slot.regions[i].ownerSlot = regions[i].ownerSlot;
        slot.regions[i].expectedVtable = regions[i].expectedVtable;
        slot.regions[i].skipVtable = regions[i].skipVtable;
        std::memcpy(slot.data + offset, regions[i].ptr, regions[i].size);
        offset += regions[i].size;
    }
    slot.size = offset;
    const uint64_t copyRegionsEnd = Profile_Now();

    static int lastSnapshotRegionCount = -1;
    if (count != lastSnapshotRegionCount) {
        int collisionIndex = -1;
        size_t collisionSize = 0;
        for (int i = 0; i < count; ++i) {
            if ((uintptr_t)regions[i].ptr == *(uint64_t*)_addr(PTR_SCOLLISION)) {
                collisionIndex = i;
                collisionSize = regions[i].size;
                break;
            }
        }
        HookLog("Rollback: snapshot frame %d regions=%d bytes=%zu sCollisionIndex=%d sCollisionSize=%zu\n",
                frame, count, offset, collisionIndex, collisionSize);
        lastSnapshotRegionCount = count;
    }
    const uint64_t snapshotMetadataEnd = Profile_Now();

    // Native clones keep reference-managed geometry alive after attack-end
    // clears the live node's child array.
    CaptureCollisionNodeClones(slot);
    const uint64_t collisionClonesEnd = Profile_Now();
    CaptureShotAndEffectRecipes(slot);
    const uint64_t shotRecipesEnd = Profile_Now();

    Profile_RecordSnapshot(profileStart, buildRegionsEnd, allocationEnd, copyRegionsEnd,
                           snapshotMetadataEnd, collisionClonesEnd, shotRecipesEnd,
                           offset, (uint32_t)count);

    // [EXPERIMENTAL] QPC capture — see mvc3/rollback W3-C2
    // LARGE_INTEGER qpc_cap;
    // if (QueryPerformanceCounter(&qpc_cap)) {
    //     g_qpc_history[slotIdx] = qpc_cap.QuadPart;
    // }
    
    // Save the local player's input for this frame in the snapshot slot.
    // At this point g_oldInputs[MAX_HISTORY - g_delayFrames] holds the
    // delayed input that the imminent game tick will use.
    slot.localInput = Controller_GetCurrentFrameInput();

    // Controller_OnFrameStart resolves and republishes the exact input before
    // the tick. Initialize with the prior prediction rather than stale received
    // ring data in case this frame is not confirmed yet.
    slot.remoteInput = Controller_GetUsedRemoteInput(frame);
    
    //HookLog("Rollback: saved frame %d with %d regions, total size %zu bytes\n", frame, count, offset);
}

void Rollback_UpdateRemoteInput(int32_t frame, uint32_t input)
{
    int slotIdx = frame % ROLLBACK_WINDOW;
    if (slotIdx < 0) slotIdx += ROLLBACK_WINDOW;
    SnapshotSlot& slot = g_slots[slotIdx];
    if (slot.data && slot.size > 0 && slot.regionCount > 0 && slot.frame == frame)
    {
        slot.remoteInput = input;
    }
}

extern "C" void Rollback_UpdateSnapshotInputs(int32_t frame, uint32_t localInput, uint32_t remoteInput)
{
    int slotIdx = frame % ROLLBACK_WINDOW;
    if (slotIdx < 0) slotIdx += ROLLBACK_WINDOW;
    SnapshotSlot& slot = g_slots[slotIdx];

    if (slot.frame == frame && slot.data && slot.size > 0)
    {
        slot.localInput = localInput;
        slot.remoteInput = remoteInput;
    }
}

extern "C" bool Rollback_GetSnapshotInputs(int32_t frame, uint32_t* outLocal, uint32_t* outRemote)
{
    int slotIdx = frame % ROLLBACK_WINDOW;
    if (slotIdx < 0) slotIdx += ROLLBACK_WINDOW;
    const SnapshotSlot& slot = g_slots[slotIdx];
    
    if (slot.frame == frame && slot.data && slot.size > 0)
    {
        if (outLocal) *outLocal = slot.localInput;
        if (outRemote) *outRemote = slot.remoteInput;
        return true;
    }
    return false;
}

void Rollback_RestoreSnapshot(int32_t frame)
{
    const uint64_t profileStart = Profile_Now();
    int slotIdx = frame % ROLLBACK_WINDOW;
    if (slotIdx < 0) slotIdx += ROLLBACK_WINDOW;
    SnapshotSlot& slot = g_slots[slotIdx];

    // Verify the slot contains data for exactly this frame (ring-buffer safety)
    if (!slot.data || slot.size == 0 || slot.regionCount == 0 || slot.frame != frame) {
        HookLog("Rollback: no snapshot available for frame %d (slot has frame %d)\n",
                frame, slot.frame);
        return;
    }

    // Restore using SAVED region metadata (addresses + sizes), not re-resolved.
    // This ensures dynamic objects (projectiles, effects) map to the exact
    // same memory addresses that were captured, even if the game rebuilt its
    // internal lists between save and restore.
    uint64_t sHitSolver = *(uint64_t*)_addr(PTR_SHITSOLVER);
    uint64_t sCollision = *(uint64_t*)_addr(PTR_SCOLLISION);
    uint64_t sMvc3Mgr = *(uint64_t*)_addr(PTR_SMVC3MGR);
    uint64_t sAction = sMvc3Mgr ? *(uint64_t*)(sMvc3Mgr + SMVC3MGR_ACTION_OFFSET) : 0;
    uint64_t modelJointAllocationSlot = 0;
    uint64_t liveModelJointAllocation = 0;
    if (sAction && IsAccessibleAddress(
            sAction + SACTION_MODEL_JOINT_WRAPPER_OFFSET)) {
        uint64_t modelJointWrapper = *(uint64_t*)(
            sAction + SACTION_MODEL_JOINT_WRAPPER_OFFSET);
        if (modelJointWrapper && IsAccessibleAddress(
                modelJointWrapper + SACTION_MODEL_JOINT_ALLOCATION_OFFSET)) {
            modelJointAllocationSlot = modelJointWrapper +
                SACTION_MODEL_JOINT_ALLOCATION_OFFSET;
            liveModelJointAllocation = *(uint64_t*)modelJointAllocationSlot;
        }
    }
    uint64_t sCharacter = *(uint64_t*)_addr(PTR_SCHARACTER);
    uint64_t team0 = sCharacter ? sCharacter + TEAM0_OFFSET : 0;
    uint64_t team1 = sCharacter ? sCharacter + TEAM1_OFFSET : 0;
    auto isTeamCharacter = [sCharacter](uint64_t character) {
        if (!sCharacter || !character) return false;
        for (int teamIndex = 0; teamIndex < 2; ++teamIndex) {
            uint64_t node = sCharacter + (teamIndex == 0 ? TEAM0_OFFSET : TEAM1_OFFSET) +
                TEAM_FIRST_NODE_OFFSET;
            for (int n = 0; node && n < 8; ++n) {
                if (*(uint64_t*)(node + NODE_CHR_OFFSET) == character) return true;
                node = *(uint64_t*)(node + NODE_NEXT_OFFSET);
            }
        }
        return false;
    };
    bool ownerStillMatches[MAX_REGIONS] = {};
    uint64_t liveOwnerPointers[MAX_REGIONS] = {};
    for (int i = 0; i < slot.regionCount; ++i) {
        uint64_t ownerSlot = slot.regions[i].ownerSlot;
        if (ownerSlot == 0) {
            ownerStillMatches[i] = true;
            continue;
        }
        liveOwnerPointers[i] = *(uint64_t*)ownerSlot;
        ownerStillMatches[i] = liveOwnerPointers[i] == (uint64_t)slot.regions[i].ptr;
    }
    const uint64_t setupEnd = Profile_Now();

    size_t offset = 0;
    int collisionRestoreCount = 0;
    size_t collisionRestoreBytes = 0;
    for (int i = 0; i < slot.regionCount; ++i) {
        size_t copySize = slot.regions[i].size;
        if (offset + copySize > slot.size) {
            copySize = slot.size - offset;
        }
        bool objectStillMatches = slot.regions[i].expectedVtable == 0 ||
            *(uint64_t*)slot.regions[i].ptr == slot.regions[i].expectedVtable;
        if (slot.regions[i].ownerSlot == modelJointAllocationSlot &&
            slot.regions[i].size == SACTION_MODEL_JOINT_ALLOCATION_SIZE &&
            liveModelJointAllocation &&
            IsAccessibleAddress(liveModelJointAllocation +
                SACTION_MODEL_JOINT_ALLOCATION_SIZE - 1) &&
            copySize > SACTION_MODEL_JOINT_DATA_OFFSET) {
            // The payload is the evaluated action-model joint state. Throw
            // capture consumes its root-motion result as well as the limits at
            // +0x54/+0x58. Keeping the future payload makes the first restored
            // throw tick see already-consumed motion and produce no diagonal
            // displacement. Rewind the payload in the current allocation while
            // preserving the live allocator header and pointer identity.
            size_t payloadSize = copySize - SACTION_MODEL_JOINT_DATA_OFFSET;
            std::memcpy((uint8_t*)liveModelJointAllocation +
                            SACTION_MODEL_JOINT_DATA_OFFSET,
                        slot.data + offset + SACTION_MODEL_JOINT_DATA_OFFSET,
                        payloadSize);
        } else if (!ownerStillMatches[i] || !objectStillMatches) {
            // The external blend/animation allocation was replaced after the
            // snapshot, or a helper cChr was destroyed/reused. Never write old
            // bytes into a freed or repurposed allocation.
        } else if (slot.regions[i].ownerSlot != 0 &&
                   slot.regions[i].size == CCHR_EXTERNAL_ANIM_SIZE) {
            Rollback_RestoreExternalAnimBlock(slot.regions[i].ptr,
                                     slot.data + offset,
                                     copySize);
        } else if ((uint64_t)slot.regions[i].ptr == sCollision &&
                   slot.regions[i].size == SCOLLISION_SIZE) {
            ++collisionRestoreCount;
            collisionRestoreBytes += copySize;
            Rollback_RestoreCollisionBody(slot.regions[i].ptr, slot.data + offset, copySize);
        } else if ((uint64_t)slot.regions[i].ptr == *(uint64_t*)_addr(PTR_SMAIN) &&
                   slot.regions[i].size == SMAIN_SIZE) {
            for (const SnapshotRange& range : SMAIN_RESTORE_RANGES) {
                if ((size_t)range.offset + range.size <= copySize) {
                    std::memcpy((uint8_t*)slot.regions[i].ptr + range.offset,
                                slot.data + offset + range.offset,
                                range.size);
                }
            }
        } else if ((uint64_t)slot.regions[i].ptr == sAction &&
                   slot.regions[i].size == 0x300) {
            for (const SnapshotRange& range : SACTION_RESTORE_RANGES) {
                if ((size_t)range.offset + range.size <= copySize) {
                    std::memcpy((uint8_t*)slot.regions[i].ptr + range.offset,
                                slot.data + offset + range.offset,
                                range.size);
                }
            }
        } else if ((uint64_t)slot.regions[i].ptr == sHitSolver &&
            slot.regions[i].size == SHITSOLVER_SIZE &&
            copySize > SHITSOLVER_DATA_OFF) {
            // Preserve the live vtable, CRITICAL_SECTION, flags and the
            // heap-backed MtArray<cHitNode*> header.
            std::memcpy((void*)(sHitSolver + SHITSOLVER_DATA_OFF),
                        slot.data + offset + SHITSOLVER_DATA_OFF,
                        copySize - SHITSOLVER_DATA_OFF);
        } else if ((uint64_t)slot.regions[i].ptr == team0 ||
                   (uint64_t)slot.regions[i].ptr == team1) {
            // Keep the live cChrNode list head and vtable, but restore the
            // active-character reference and all team gameplay aggregates.
            uint64_t liveActive = *(uint64_t*)((uint8_t*)slot.regions[i].ptr + 0x48);
            for (const SnapshotRange& range : CCHR_TEAM_RESTORE_RANGES) {
                if ((size_t)range.offset + range.size <= copySize) {
                    std::memcpy((uint8_t*)slot.regions[i].ptr + range.offset,
                                slot.data + offset + range.offset,
                                range.size);
                }
            }
            std::memcpy((uint8_t*)slot.regions[i].ptr + 0x48,
                        slot.data + offset + 0x48, sizeof(uint64_t));
            // Do not leave a saved pointer to a character that no longer
            // belongs to the current live team list.
            uint64_t restoredActive = *(uint64_t*)((uint8_t*)slot.regions[i].ptr + 0x48);
            if (restoredActive == 0 || restoredActive != liveActive) {
                // The roster itself is stable for a snapshot restore; the
                // saved pointer is valid when it is one of the current nodes.
                // If it was replaced, retain the live selection instead.
                bool found = false;
                uint64_t node = (uint64_t)slot.regions[i].ptr + TEAM_FIRST_NODE_OFFSET;
                for (int n = 0; node && n < 8; ++n) {
                    if (*(uint64_t*)(node + NODE_CHR_OFFSET) == restoredActive) {
                        found = true;
                        break;
                    }
                    node = *(uint64_t*)(node + NODE_NEXT_OFFSET);
                }
                if (!found)
                    *(uint64_t*)((uint8_t*)slot.regions[i].ptr + 0x48) = liveActive;
            }
        } else if (slot.regions[i].skipVtable && copySize > sizeof(uint64_t)) {
            uint64_t liveUnitState[sizeof(CUNIT_LIVE_STATE_OFFSETS) / sizeof(CUNIT_LIVE_STATE_OFFSETS[0])];
            for (size_t field = 0;
                 field < sizeof(CUNIT_LIVE_STATE_OFFSETS) / sizeof(CUNIT_LIVE_STATE_OFFSETS[0]);
                 ++field) {
                liveUnitState[field] =
                    *(uint64_t*)((uint8_t*)slot.regions[i].ptr + CUNIT_LIVE_STATE_OFFSETS[field]);
            }
            uint8_t liveDrawState[CCHR_DRAW_STATE_END - CCHR_DRAW_STATE_START];
            std::memcpy(liveDrawState,
                        (uint8_t*)slot.regions[i].ptr + CCHR_DRAW_STATE_START,
                        sizeof(liveDrawState));
            uint8_t liveModelTail[CCHR_MODEL_TAIL_END - CCHR_MODEL_TAIL_START];
            std::memcpy(liveModelTail,
                        (uint8_t*)slot.regions[i].ptr + CCHR_MODEL_TAIL_START,
                        sizeof(liveModelTail));

            struct LivePointerField { uint32_t offset; uint64_t value; };
            LivePointerField livePointers[512];
            size_t livePointerCount = 0;
            for (size_t fieldOffset = sizeof(uint64_t);
                 fieldOffset + sizeof(uint64_t) <= copySize && livePointerCount < 512;
                 fieldOffset += sizeof(uint64_t)) {
                uint64_t liveValue = *(uint64_t*)((uint8_t*)slot.regions[i].ptr + fieldOffset);
                uint64_t savedValue = *(uint64_t*)(slot.data + offset + fieldOffset);
                if (IsLikelyPointer(liveValue) || IsLikelyPointer(savedValue)) {
                    livePointers[livePointerCount++] = {
                        (uint32_t)fieldOffset, liveValue
                    };
                }
            }
            std::memcpy((uint8_t*)slot.regions[i].ptr + sizeof(uint64_t),
                        slot.data + offset + sizeof(uint64_t),
                        copySize - sizeof(uint64_t));
            // cChr derives from cUnit. Preserve its live lifecycle state and
            // intrusive links: rewinding +0x10 can re-run setup/createProperty
            // for an already-live helper, while rewinding the links corrupts
            // sUnit traversal shared with objects outside this snapshot.
            for (size_t field = 0;
                 field < sizeof(CUNIT_LIVE_STATE_OFFSETS) / sizeof(CUNIT_LIVE_STATE_OFFSETS[0]);
                 ++field) {
                uint32_t fieldOffset = CUNIT_LIVE_STATE_OFFSETS[field];
                if (fieldOffset == 0x10 && isTeamCharacter((uint64_t)slot.regions[i].ptr))
                    continue; // restore the saved MtObject alive/lifecycle flags
                *(uint64_t*)((uint8_t*)slot.regions[i].ptr + fieldOffset) = liveUnitState[field];
            }
            // Keep live draw-list/resource ownership and uModel lifecycle state,
            // but rewind Motion[8] at +0x560..+0x115f. Those records must agree
            // with the separately restored blend buffers and animation blocks.
            std::memcpy((uint8_t*)slot.regions[i].ptr + CCHR_DRAW_STATE_START,
                        liveDrawState,
                        sizeof(liveDrawState));
            std::memcpy((uint8_t*)slot.regions[i].ptr + CCHR_MODEL_TAIL_START,
                        liveModelTail,
                        sizeof(liveModelTail));
            // Preserve every live pointer-like field in the remainder of cChr.
            // The legacy body contains weak shot/effect refs, resource refs and
            // subclass-tail pointers whose targets are not recreated by restore.
            for (size_t pointer = 0; pointer < livePointerCount; ++pointer) {
                *(uint64_t*)((uint8_t*)slot.regions[i].ptr + livePointers[pointer].offset) =
                    livePointers[pointer].value;
            }
            // The broad pointer guard can mistake packed flags and pairs of
            // floats for aligned addresses (for example speed + zero accel).
            // Reapply confirmed pointer-free gameplay ranges so grounded state,
            // gravity and velocity come from the restored frame.
            for (const SnapshotRange& range : CCHR_SCALAR_RESTORE_RANGES) {
                if ((size_t)range.offset + range.size <= copySize) {
                    std::memcpy((uint8_t*)slot.regions[i].ptr + range.offset,
                                slot.data + offset + range.offset,
                                range.size);
                }
            }

            uint64_t concreteDti = GetObjectDti((uint64_t)slot.regions[i].ptr);
            int32_t ankiReservedLive = 0;
            int32_t ankiReservedSaved = 0;
            bool traceAnkiRestore =
                concreteDti == (uint64_t)_addr(0x140D455F8) && copySize >= 0x6900;
            if (traceAnkiRestore) {
                ankiReservedLive = *(int32_t*)((uint8_t*)slot.regions[i].ptr + 0x68FC);
                ankiReservedSaved = *(int32_t*)(slot.data + offset + 0x68FC);
            }

            ConcreteCharacterSnapshot_RestoreFields(
                slot.regions[i].ptr,
                slot.data + offset,
                copySize,
                concreteDti);

            if (traceAnkiRestore) {
                int32_t ankiReservedAfter =
                    *(int32_t*)((uint8_t*)slot.regions[i].ptr + 0x68FC);
                //HookLog("ANKIRESTORE frame=%d object=0x%llX live=%d saved=%d after=%d\n",
                //        frame, (uint64_t)slot.regions[i].ptr, ankiReservedLive,
                //        ankiReservedSaved, ankiReservedAfter);
            }

            // uChrCpu's interpreter stack contains 32-bit script values, so
            // pairs of words can look like pointers to the generic guard. A
            // split restore of that stack and its cursor lets the VM combine
            // stale words into a bogus manager pointer during re-simulation.
            // Locate the embedded uChrCpu by its vtable and restore the VM
            // stack plus scalar execution state as one pointer-free block.
            const uint64_t cpuVtable = _addr(UCHRCPU_VTABLE);
            if (cpuVtable != 0) {
                for (size_t cpuOffset = 0;
                     cpuOffset + sizeof(uint64_t) <= copySize;
                     cpuOffset += sizeof(uint64_t)) {
                    uint64_t liveVtable = *(uint64_t*)((uint8_t*)slot.regions[i].ptr +
                                                       cpuOffset);
                    if (liveVtable != cpuVtable) continue;
                    if (cpuOffset + UCHRCPU_VM_STATE_OFFSET +
                            UCHRCPU_VM_STATE_SIZE > copySize)
                        break;

                    std::memcpy((uint8_t*)slot.regions[i].ptr + cpuOffset +
                                    UCHRCPU_VM_STATE_OFFSET,
                                slot.data + offset + cpuOffset +
                                    UCHRCPU_VM_STATE_OFFSET,
                                UCHRCPU_VM_STATE_SIZE);
                    break;
                }
            }

            // Motion slots contain many pairs of floats whose packed 64-bit
            // value passes IsLikelyPointer (for example {1.0f, 0.0f}). The
            // generic pointer guard above would therefore retain prediction-
            // timeline frame, blend and root-motion values. Rewind the known
            // scalar/vector portions while keeping the slot vtable and its
            // resource/allocation pointers at +0x120..+0x138, +0x160,
            // +0x170 and +0x178 live.
            for (int motion = 0; motion < CCHR_MOTION_COUNT; ++motion) {
                size_t motionBase = CCHR_MOTION_BASE + motion * CCHR_MOTION_STRIDE;
                static constexpr SnapshotRange motionRestoreRanges[] = {
                    {0x08, 0x118},
                    {0x140, 0x20},
                    {0x168, 0x08},
                };
                for (const SnapshotRange& range : motionRestoreRanges) {
                    size_t fieldOffset = motionBase + range.offset;
                    if (fieldOffset + range.size <= copySize) {
                        std::memcpy((uint8_t*)slot.regions[i].ptr + fieldOffset,
                                    slot.data + offset + fieldOffset,
                                    range.size);
                    }
                }
            }

            // Rewind all five cChrTargetInfo records at +0x6490..+0x670f.
            // They cache target, attacker, damage, throw and parent character
            // state. Keep each vtable live and validate its saved character
            // pointer against the current roster.
            static constexpr uint32_t targetInfoBase = 0x6490;
            static constexpr uint32_t targetInfoStride = 0x80;
            for (uint32_t targetInfo = 0; targetInfo < 5; ++targetInfo) {
                uint32_t record = targetInfoBase + targetInfo * targetInfoStride;
                uint32_t pointerField = record + 0x08;
                uint32_t dataField = record + 0x10;
                if ((size_t)dataField + 0x70 > copySize) continue;

                uint64_t savedTarget = *(const uint64_t*)(slot.data + offset + pointerField);
                uint64_t liveTarget = *(uint64_t*)((uint8_t*)slot.regions[i].ptr + pointerField);
                if (savedTarget == 0 || savedTarget == liveTarget || isTeamCharacter(savedTarget)) {
                    *(uint64_t*)((uint8_t*)slot.regions[i].ptr + pointerField) = savedTarget;
                    std::memcpy((uint8_t*)slot.regions[i].ptr + dataField,
                                slot.data + offset + dataField,
                                0x70);
                }
            }

            // The generic pointer guard above can mistake aligned primitive
            // flags, coordinates and IDs for addresses, retaining post-hit
            // disabled geometry. Rewind confirmed pointer-free fields while
            // preserving each primitive's vtable and live parent references.
            static constexpr uint32_t capsuleBase = 0x4210;
            static constexpr uint32_t capsuleStride = 0xB0;
            for (uint32_t primitive = 0; primitive < 16; ++primitive) {
                uint32_t entry = capsuleBase + primitive * capsuleStride;
                static constexpr SnapshotRange fields[] = {
                    {0x08, 0x01}, // inherited active flag
                    {0x10, 0x04}, // primitive kind
                    {0x20, 0x24}, // capsule endpoints and radius
                    {0x50, 0x04}, // active/attribute flags
                    {0x68, 0x0C}, // parent indices and size rate
                    {0x80, 0x20}, // local endpoint offsets
                };
                for (const SnapshotRange& field : fields) {
                    if ((size_t)entry + field.offset + field.size <= copySize) {
                        std::memcpy((uint8_t*)slot.regions[i].ptr + entry + field.offset,
                                    slot.data + offset + entry + field.offset,
                                    field.size);
                    }
                }
            }

            static constexpr uint32_t sphereBases[] = {0x4E20, 0x5530, 0x5C40};
            static constexpr uint32_t sphereStride = 0x70;
            for (uint32_t base : sphereBases) {
                for (uint32_t primitive = 0; primitive < 16; ++primitive) {
                    uint32_t entry = base + primitive * sphereStride;
                    static constexpr SnapshotRange fields[] = {
                        {0x08, 0x01}, // inherited active flag
                        {0x10, 0x04}, // primitive kind
                        {0x20, 0x10}, // sphere center and radius
                        {0x30, 0x04}, // active/attribute flags
                        {0x40, 0x08}, // parent index and size rate
                        {0x50, 0x10}, // local center offset
                    };
                    for (const SnapshotRange& field : fields) {
                        if ((size_t)entry + field.offset + field.size <= copySize) {
                            std::memcpy((uint8_t*)slot.regions[i].ptr + entry + field.offset,
                                        slot.data + offset + entry + field.offset,
                                        field.size);
                        }
                    }
                }
            }

            // cAtkCtrl::TargetInfo entries are 0x18 bytes: vtable at +0x00,
            // target reference at +0x08, then mTotalHit and mHitTime at
            // +0x10/+0x14. There are 32 inline entries in each table. The
            // target reference is part of the attack's
            // per-target consumed state. Preserving the post-hit live pointer
            // can make a restored pre-hit attack remain associated with the
            // target it already hit; starting another attack clears this,
            // which explains why the next attack works.
            static constexpr uint32_t targetTables[] = {0x18C8, 0x1BC8, 0x1EC8};
            for (uint32_t table : targetTables) {
                for (uint32_t target = 0; target < 32; ++target) {
                    uint32_t targetOffset = table + target * 0x18 + 0x08;
                    uint32_t field = table + target * 0x18 + 0x10;
                    if ((size_t)targetOffset + sizeof(uint64_t) <= copySize) {
                        uint64_t savedTarget = *(const uint64_t*)(slot.data + offset + targetOffset);
                        uint64_t liveTarget = *(uint64_t*)((uint8_t*)slot.regions[i].ptr + targetOffset);
                        if (savedTarget == 0 || savedTarget == liveTarget ||
                            isTeamCharacter(savedTarget)) {
                            *(uint64_t*)((uint8_t*)slot.regions[i].ptr + targetOffset) = savedTarget;
                        }
                    }
                    if ((size_t)field + 0x08 <= copySize) {
                        std::memcpy((uint8_t*)slot.regions[i].ptr + field,
                                    slot.data + offset + field,
                                    0x08);
                    }
                }
            }
        } else if (copySize > 0) {
            std::memcpy(slot.regions[i].ptr, slot.data + offset, copySize);
        }
        offset += slot.regions[i].size;
    }
    const uint64_t regionsEnd = Profile_Now();

    // Restore the saved child factory as well as its children. Keeping the
    // destination's generic factory creates objects with incompatible vtables.
    auto copyCollisionNode =
        (void(__fastcall*)(void*, void*, bool, bool))_addr(FN_COLLISION_NODE_COPY);
    for (const CollisionNodeClone& entry : slot.collisionNodeClones) {
        if (*(uint64_t*)entry.ownerSlot == (uint64_t)entry.savedPtr &&
            HasCallableDtiVirtual(entry.savedPtr) &&
            HasCallableDtiVirtual(entry.clone))
            copyCollisionNode(entry.savedPtr, entry.clone, false, true);
    }
    const uint64_t collisionClonesEnd = Profile_Now();

    // Full cChr body restore also rewinds its external pointer fields. If an
    // allocation changed, put the current pointer back after skipping its old
    // buffer so the live character does not retain a stale address.
    for (int i = 0; i < slot.regionCount; ++i) {
        if (slot.regions[i].ownerSlot != 0 && !ownerStillMatches[i]) {
            *(uint64_t*)slot.regions[i].ownerSlot = liveOwnerPointers[i];
        }
    }
    const uint64_t ownerPointersEnd = Profile_Now();

    // Active hit nodes are one-frame work items. Restoring them after a hit has
    // already been applied replays the resolver callback on every subsequent
    // restore, compounding launch/pushback (the effect is largest immediately
    // after a hit and decays once the native solver consumes the node).
    // Keep the live MtArray allocation and solver infrastructure, but discard
    // the transient pending-hit list.
    if (sHitSolver) {
        ((void(__fastcall*)(uint64_t))_addr(FN_RESET_HITNODES))(sHitSolver);
        *(uint32_t*)(sHitSolver + 0x30064) = 0; // mHitSolverCnt
        *(uint32_t*)(sHitSolver + 0x30070) = 0; // sort bucket A count
        *(uint32_t*)(sHitSolver + 0x34078) = 0; // sort bucket B count
    }
    const uint64_t hitSolverEnd = Profile_Now();

    // Log ALL heap regions with their sizes
    offset = 0;
    for (int i = 0; i < slot.regionCount; ++i) {
        uint64_t raddr = (uint64_t)slot.regions[i].ptr;
        size_t rsize = slot.regions[i].size;
     //   HookLog("DIAG Restore region[%d]: addr=0x%llX size=%zu\n", i, raddr, rsize);
        uint64_t maybeVt = (raddr >= 0x100000 && raddr < 0x140000000) ? *(uint64_t*)raddr : 0;
        if (maybeVt >= 0x140000000 && rsize >= 0x1000) {
            uint32_t* animAfter = (uint32_t*)(raddr + 0x1300);
     //       HookLog("DIAG Post-restore char@0x%llX region[%d]: anim[+0x1300]: %08X %08X %08X %08X  %08X %08X %08X %08X  %08X %08X %08X %08X  %08X %08X %08X %08X\n",
     //               raddr, i,
      //              animAfter[0], animAfter[1], animAfter[2], animAfter[3],
      //              animAfter[4], animAfter[5], animAfter[6], animAfter[7],
          //          animAfter[8], animAfter[9], animAfter[10], animAfter[11],
       //             animAfter[12], animAfter[13], animAfter[14], animAfter[15]);
        }
        offset += rsize;
    }

    // The character body restore includes slot-table pointers to heap objects
    // that are not part of the snapshot. Clear them before the next game tick
    // can release one through MtObject__replaceSlot_6F30.
    Rollback_ClearStalePointers();
    const uint64_t stalePointersEnd = Profile_Now();

    // Shot construction registers owned effects through sGameEffect and scans
    // its tracked-object slots with MtObject::isKindOf. Clear references to
    // effects retired by this restore before the first factory call.
    Rollback_ClearGameEffectSlots();
    const uint64_t effectSlotsEnd = Profile_Now();
    RestoreShotAndEffectRecipes(slot);
    const uint64_t shotsEnd = Profile_Now();

    // Character slot tables contain live model/resource references. Clearing
    // all 64 entries makes restored dead characters invisible.
    Rollback_SyncActiveCharacterUi();
    const uint64_t uiEnd = Profile_Now();

    if (g_activeRollbackProfile) {
        g_activeRollbackProfile->restoreTotal += uiEnd - profileStart;
        g_activeRollbackProfile->restoreSetup += setupEnd - profileStart;
        g_activeRollbackProfile->restoreRegions += regionsEnd - setupEnd;
        g_activeRollbackProfile->restoreCollisionClones += collisionClonesEnd - regionsEnd;
        g_activeRollbackProfile->restoreOwnerPointers += ownerPointersEnd - collisionClonesEnd;
        g_activeRollbackProfile->restoreHitSolver += hitSolverEnd - ownerPointersEnd;
        g_activeRollbackProfile->restoreStalePointers += stalePointersEnd - hitSolverEnd;
        g_activeRollbackProfile->restoreEffectSlots += effectSlotsEnd - stalePointersEnd;
        g_activeRollbackProfile->restoreShots += shotsEnd - effectSlotsEnd;
        g_activeRollbackProfile->restoreUi += uiEnd - shotsEnd;
    }

    HookLog("Rollback: restored frame %d with %d regions, total size %zu bytes, "
            "sCollision inline regions=%d bytes=%zu\n",
            frame, slot.regionCount, offset, collisionRestoreCount, collisionRestoreBytes);
}

 void Rollback_ReSimulate(int32_t startFrame, int32_t endFrame)
{
    if (startFrame >= endFrame) return;

    // Re-entry guard: if F7 is still held/read during re-sim, the game's
    // input processing will try to start another ReSimulate from within
    // oMvc3Main_move.  Just return — we're already in one.
    if (g_isResimulating) {
        g_resimReentranceDepth++;
        HookLog("Rollback: ReSimulate re-entered depth=%d — ignoring (already re-simulating)\n",
                g_resimReentranceDepth);
        return;
    }
    g_resimReentranceDepth = 0;

    if (!g_sMvc3Main) {
        HookLog("Rollback: cannot re-simulate — sMvc3Main pointer not captured\n");
        return;
    }

    RollbackProfile profile = {};
    profile.startFrame = startFrame;
    profile.endFrame = endFrame;
    const uint64_t profileStart = Profile_Now();
    if constexpr (ROLLBACK_PROFILING_ENABLED)
        g_activeRollbackProfile = &profile;
    g_isResimulating = true;

    //HookLog("Rollback: [%s] re-simulating frames %d \u2192 %d\n",
            //Netplay_IsHost() ? "host" : "client", startFrame, endFrame);

    //Are these really necessary? Might as well leave them in

    uint64_t phaseStart = Profile_Now();
    Rollback_PatchSleep(true); // NOP Sleep calls so frames run at full speed
    Rollback_PatchPresent(true); // NOP Present inside post-render sync
    Rollback_PatchSwapchain(true); // NOP swapchain vtable+0x50 calls
    Rollback_PatchDrawCalls(true); // NOP remaining render/draw calls
    profile.patchEnable += Profile_Now() - phaseStart;

    // Save the live input history before rewinding.  The re-sim loop calls
    // SilentFrameStart/End which shifts g_oldInputs with zeros (re-sim
    // bypasses the accumulation).  Restoring after the loop prevents the
    // outbound packet from carrying a corrupted timeline.
    //HookLog("[ReSim] ~~~~~~~~~~ g_oldInputs BEFORE re-sim loop ~~~~~~~~~~\n");
    phaseStart = Profile_Now();
    Controller_SaveInputHistory();
    profile.inputHistorySave += Profile_Now() - phaseStart;
    int silentFrameEndCount = 0;

    // Diagnostic dump: capture ground truth BEFORE rewinding.
    // Gives the live state of all regions + character state to compare
    // against what the re-simulation produces.
    // When g_f7DiagSession is set (F7 mode), skip the ground truth dump —
    // the F7 state machine already captured per-frame live dumps during
    // the recording phase using that session.  We just reuse the session
    // ID so resim dumps land alongside the live dumps.
    phaseStart = Profile_Now();
    {
        int diagSid;
        if (g_f7DiagSession >= 0) {
            diagSid = g_f7DiagSession;
            HookLog("Rollback: diag session %d (F7, reuse) for frames %d\u2192%d\n",
                    diagSid, startFrame, endFrame);
        } else {
            diagSid = NDJSON_BeginSession();
        }
        // Sync the global so the resim loop writes to the same session
        (void)diagSid;
    }
    profile.diagSetup += Profile_Now() - phaseStart;

    // Capture the final predicted live state before restoring it. This is a
    // separate diagnostic entry because it belongs to the discarded timeline,
    // not to the corrected frame sequence.
    int32_t liveFrameBeforeRestore = -1;
    {
        uint64_t sReplay = *(uint64_t*)_addr(PTR_SREPLAY);
        if (sReplay) liveFrameBeforeRestore = *(int32_t*)(sReplay + 0x54);
    }
    phaseStart = Profile_Now();
    if (liveFrameBeforeRestore >= 0 && Netplay_IsActive()) {
        std::string json = NDJSON_GenerateNetplayLive(
            liveFrameBeforeRestore, false, "pre_restore_prediction", startFrame, endFrame);
        if (!json.empty()) NDJSON_WriteNetplayLive(json);
    }
    profile.preRestoreJson += Profile_Now() - phaseStart;

    // 1. Rewind to the starting frame boundary
    Rollback_RestoreSnapshot(startFrame);

    // Diagnostic: log whether we actually have received input for every frame
    // in the resim range before we start stepping.
    phaseStart = Profile_Now();
    Controller_LogReceivedInputAvailability(startFrame, endFrame);
    profile.inputAvailability += Profile_Now() - phaseStart;

    // [EXPERIMENTAL] mJobThreadNumInit=1 — see mvc3/rollback R3-SIMLOOP
    // uint64_t sMain_job = *(uint64_t*)_addr(PTR_SMAIN);
    // uint32_t saved_job_thread_num_init = 0;
    // uint32_t* pJobThreadNumInit = nullptr;
    // if (sMain_job) {
    //     pJobThreadNumInit = (uint32_t*)(sMain_job + 0xA4); // mJobThreadNumInit at offset 164
    //     saved_job_thread_num_init = *pJobThreadNumInit;
    //     *pJobThreadNumInit = 1;
    // }

    // 2. Step forward (no snapshot saves — the re-simmed frames are now
    //    correct, and the next normal-frame save at curFrame covers us).
    //    sMain__move is hooked (skip frame-pacing) and the render function
    //    is hooked (skip Present), so the full oMvc3Main_move game tick
    //    runs at speed without timing waits or actual rendering.
    //    The hkMvc3Main_move hook returns early during re-sim so the
    //    game's own frame loop doesn't double-execute.
    //    Controller_SilentFrameStart/End maintains the local input shift
    //    buffer without draining packets or sending outbound data.  Local
    //    and remote input for each re-simmed frame are injected from the
    //    snapshot slot via Controller_InjectLocalInput and
    //    Controller_InjectRemoteInput before the game tick.
    for (int32_t f = startFrame; f < endFrame; ++f) {
        RollbackProfileTimer frameTimer;
        g_resimulateCurrentFrame = f;
        int si = f % ROLLBACK_WINDOW;
        if (si < 0) si += ROLLBACK_WINDOW;

        // Keep the corrected inputs attached to this frame while multi-resim
        // replaces its snapshot. SaveSnapshot normally samples the live input
        // shift buffer, which is not the timeline currently being replayed.
        const uint32_t resimLocalInput = g_slots[si].localInput;
        uint32_t resimRemoteInput = g_slots[si].remoteInput;

        // [EXPERIMENTAL] g_virtual_qpc = g_qpc_history[f % ROLLBACK_WINDOW];
        // Re-resolve remote input so re-sim uses the latest received data,
        // not the stale prediction that triggered the conflict.
        if (Netplay_IsActive()) {
            Controller_ResolveRemoteInput(f);
            resimRemoteInput = Controller_GetUsedRemoteInput(f);
        }
        // hkprocessFrameInput reads snapshot metadata before its ring-buffer
        // fallback, so publish the newly resolved input before running the tick.
        Rollback_UpdateSnapshotInputs(f, resimLocalInput, resimRemoteInput);
        Controller_SilentFrameStart();
        // Inject the local and remote inputs saved in this frame's
        // snapshot slot.  hkprocessFrameInput reads g_recordedGameInput
        // for the local team and g_usedRemoteInput for the remote team
        // during re-sim; pre-populating from the snapshot makes the hook
        // replay the correct historical inputs without relying on the
        // shift-buffer save/restore that was global-static-buffer-
        // vulnerable.
        {
            Controller_InjectLocalInput(f, resimLocalInput);
            if (!Netplay_IsActive()) {
                Controller_InjectRemoteInput(f, resimRemoteInput);
            }
        }
        frameTimer.Add(profile.frameInput);

        // Capture PRE-state before the tick
            NDJSON_CaptureFrame(f, /*isResim=*/true, /*postOnly=*/false);
        frameTimer.Add(profile.frameDiagPre);

        // Pre-tick NDJSON logging for both active and inactive netplay sessions
        if (Netplay_IsActive()) {
            NDJSON_DiscardPendingNetplayLive(f);
            std::string json = NDJSON_GenerateNetplayLive(f, true);
            if (!json.empty())
                NDJSON_WriteNetplayLive(json);
        } else {
            NDJSON_DumpFrame(f, 1);
        }
        frameTimer.Add(profile.frameJson);

        // Multi-resim may correct a still-predicted frame again, so preserve a
        // corrected restore point. Non-multi waits for a definitive batch and
        // never starts another rollback inside this replayed range.
        if (g_multiResimEnabled) {
            Rollback_SaveSnapshot(f);
            Rollback_UpdateSnapshotInputs(f, resimLocalInput, resimRemoteInput);
        }
        frameTimer.Add(profile.frameSnapshot);

        //Todo: replace with headless tick for better perf
        oMvc3Main_move(g_sMvc3Main);
        frameTimer.Add(profile.frameNativeTick, profile.frameNativeTickMax);

        Controller_SilentFrameEnd();
        silentFrameEndCount++;
        Controller_LogReSimFrame(f);
        frameTimer.Add(profile.frameInputEnd);

        // Capture POST-state after the tick (preserves the pre-tick state captured above)
        NDJSON_CaptureFrame(f, /*isResim=*/true, /*postOnly=*/true);

        NDJSON_QueuePendingResimDump(f);
        frameTimer.Add(profile.frameDiagPost);
        frameTimer.Finish(profile.frameTotal, profile.frameTotalMax);
    }

    // Verify SilentFrameEnd count matches expected
    int expectedFrameCount = endFrame - startFrame;
    //HookLog("[ReSim] SilentFrameEnd called %d times (expected=%d, match=%s)\n",
            //silentFrameEndCount, expectedFrameCount,
            //silentFrameEndCount == expectedFrameCount ? "YES" : "NO");

    //HookLog("[ReSim] ~~~~~~~~~~ g_oldInputs AFTER re-sim loop (before RestoreInputHistory) ~~~~~~~~~~\n");
    Controller_DumpOldInputs("post-loop-pre-restore");

    // Restore the local input history so the next outbound packet carries
    // the original timeline, preventing the remote side from seeing spurious
    // conflicts on already-reconciled frames.
    phaseStart = Profile_Now();
    Controller_RestoreInputHistory();
    profile.inputHistoryRestore += Profile_Now() - phaseStart;

    // [EXPERIMENTAL] mJobThreadNumInit restore — see mvc3/rollback R3-SIMLOOP
    // if (pJobThreadNumInit) {
    //     *pJobThreadNumInit = saved_job_thread_num_init;
    // }

    // 3. Resynchronize performance counter state inside sMain to match
    //    current system time, preventing the game loop from seeing a
    //    negative or excessive frame-time delta that would cause a freeze.
    uint64_t sMain = *(uint64_t*)_addr(PTR_SMAIN);
    phaseStart = Profile_Now();
    if (sMain) {
        //Same with this, unsure if this is really needed, might cause lag if disabled
        Rollback_ResyncMainTiming(sMain);
    }
    profile.timingResync += Profile_Now() - phaseStart;

    g_isResimulating = false;
    if (g_resimReentranceDepth > 0) {
        HookLog("[ReSim] WARNING: re-entrance depth was %d during re-sim (nested calls detected)\n",
                g_resimReentranceDepth);
    }
    phaseStart = Profile_Now();
    Rollback_PatchSleep(false); // restore Sleep calls
    Rollback_PatchPresent(false); // restore Present call
    Rollback_PatchSwapchain(false); // restore swapchain calls
    Rollback_PatchDrawCalls(false); // restore render/draw calls
    profile.patchDisable += Profile_Now() - phaseStart;
    profile.total = Profile_Now() - profileStart;
    if constexpr (ROLLBACK_PROFILING_ENABLED)
        g_activeRollbackProfile = nullptr;
    Profile_Dump(profile, g_ndjsonLoggingEnabled, g_multiResimEnabled);
    //HookLog("Rollback: re-simulation complete (%d frames)\n",
            //endFrame - startFrame);
}

void Rollback_RequestReSimulate(int32_t startFrame, int32_t endFrame)
{
    g_rollbackReqStart = startFrame;
    g_rollbackReqEnd   = endFrame;
}

bool Rollback_ConsumeReSimRequest(int32_t& startFrame, int32_t& endFrame)
{
    if (g_rollbackReqStart < 0)
        return false;

    startFrame = g_rollbackReqStart;
    endFrame = g_rollbackReqEnd;
    g_rollbackReqStart = -1;
    g_rollbackReqEnd = -1;
    return true;
}

void Rollback_SetMainInstance(uint64_t main)
{
    if (!g_sMvc3Main)
        g_sMvc3Main = main;
}

void Rollback_FreeSnapshots()
{
    // Make sure patched calls are restored before tearing down
    Rollback_PatchSleep(false);
    Rollback_PatchPresent(false);
    Rollback_PatchSwapchain(false);
    Rollback_PatchDrawCalls(false);
    Rollback_PatchDrawCalls(false); // restore draw calls (no-op if unpatched)

    for (int i = 0; i < ROLLBACK_WINDOW; ++i) {
        DestroyCollisionNodeClones(g_slots[i]);
        delete[] g_slots[i].data;
        g_slots[i].data        = nullptr;
        g_slots[i].size        = 0;
        g_slots[i].capacity    = 0;
        g_slots[i].regionCount = 0;
        g_slots[i].frame       = -1;
        g_slots[i].shotRecipes.clear();
        g_slots[i].effectRecipes.clear();
    }
    g_liveShotRecipes.clear();
    g_liveEffectRecipes.clear();
    g_shotSpawnSequence = 0;
    g_sMvc3Main = 0;
}

// ===========================================================================
// VTable patch — post-render swapchain operations (0x50 slot)
//
// The two vtable+0x50 calls at the end of sMvc3Main__move (on objects at
// sMain+0x40208 and sMain+0x40270) are swapchain / D3D operations that
// depend on the render function having set up the frame.  When we NOP the
// render function during re-sim these calls hang.  Since we can't easily
// MinHook each vtable entry, we temporarily replace them with a NOP stub
// for the duration of the re-sim loop.
// ===========================================================================
void Rollback_InitHooks()
{
    // [EXPERIMENTAL] QPC hook install — see mvc3/rollback W3-C2
    // g_main_thread_id = GetCurrentThreadId();
    // {
    //     HMODULE hKernel = GetModuleHandleA("kernel32.dll");
    //     void* qpc_target = (void*)GetProcAddress(hKernel, "QueryPerformanceCounter");
    //     if (qpc_target) {
    //         MH_STATUS sq = MH_CreateHook(qpc_target, (LPVOID)&hooked_qpc, (void**)&g_real_qpc);
    //         if (sq == MH_OK) {
    //             MH_EnableHook(qpc_target);
    //             HookLog("Rollback: QPC hook installed (main_tid=%lu)\n", g_main_thread_id);
    //         } else {
    //             HookLog("Rollback: WARN — QPC hook failed (%d), resim won't be deterministic\n", sq);
    //         }
    //     } else {
    //         HookLog("Rollback: WARN — could not resolve QueryPerformanceCounter\n");
    //     }
    // }

    Rollback_InitMainHooks(&oMvc3Main_move);

    MH_STATUS s;

    s = MH_CreateHook((LPVOID)_addr(FN_SSHOT_SPAWN),
                      (LPVOID)&hkShotSpawn, (LPVOID*)&oShotSpawn);
    if (s == MH_OK) {
        MH_EnableHook((LPVOID)_addr(FN_SSHOT_SPAWN));
        HookLog("Rollback: hooked sShot__spawnShot\n");
    } else {
        HookLog("Rollback: failed to hook sShot__spawnShot (%d)\n", s);
    }

    s = MH_CreateHook((LPVOID)_addr(0x1401BA2D0),
                      (LPVOID)&hkKingKobunMove, (LPVOID*)&oKingKobunMove);
    if (s == MH_OK) {
        MH_EnableHook((LPVOID)_addr(0x1401BA2D0));
        HookLog("Rollback: hooked uShotVulcanKingKobun::move trace\n");
    } else {
        HookLog("Rollback: failed to hook uShotVulcanKingKobun::move (%d)\n", s);
    }

    s = MH_CreateHook((LPVOID)_addr(FN_RNG_RANDOM_INT),
                      (LPVOID)&hkRandomIntRange,
                      (LPVOID*)&oRandomIntRange);
    if (s == MH_OK) {
        //MH_EnableHook((LPVOID)_addr(FN_RNG_RANDOM_INT));
        HookLog("Rollback: hooked Ankihou RNG trace at 0x%llX\n",
                (uint64_t)_addr(FN_RNG_RANDOM_INT));
    } else {
        HookLog("Rollback: failed to hook Ankihou RNG trace (%d)\n", s);
    }

    s = MH_CreateHook((LPVOID)_addr(FN_HSIENKO_SELECT_ANKI),
                      (LPVOID)&hkHsienKoSelectAnki,
                      (LPVOID*)&oHsienKoSelectAnki);
    if (s == MH_OK) {
        //MH_EnableHook((LPVOID)_addr(FN_HSIENKO_SELECT_ANKI));
        HookLog("Rollback: hooked Hsien-Ko Anki selector trace at 0x%llX\n",
                (uint64_t)_addr(FN_HSIENKO_SELECT_ANKI));
    } else {
        HookLog("Rollback: failed to hook Hsien-Ko Anki selector trace (%d)\n", s);
    }

    s = MH_CreateHook((LPVOID)_addr(FN_SET_RESERVED_ANKI),
                      (LPVOID)&hkSetReservedAnki,
                      (LPVOID*)&oSetReservedAnki);
    if (s == MH_OK) {
        //MH_EnableHook((LPVOID)_addr(FN_SET_RESERVED_ANKI));
        HookLog("Rollback: hooked Hsien-Ko Anki reserve trace at 0x%llX\n",
                (uint64_t)_addr(FN_SET_RESERVED_ANKI));
    } else {
        HookLog("Rollback: failed to hook Hsien-Ko Anki reserve trace (%d)\n", s);
    }

    s = MH_CreateHook((LPVOID)_addr(FN_SSHOT_SPAWN_EX),
                      (LPVOID)&hkShotSpawnEx, (LPVOID*)&oShotSpawnEx);
    if (s == MH_OK) {
        MH_EnableHook((LPVOID)_addr(FN_SSHOT_SPAWN_EX));
        HookLog("Rollback: hooked sShot__spawnShotEx\n");
    } else {
        HookLog("Rollback: failed to hook sShot__spawnShotEx (%d)\n", s);
    }

    s = MH_CreateHook((LPVOID)_addr(FN_UGAMEEFFECT_CREATE),
                      (LPVOID)&hkEffectCreate, (LPVOID*)&oEffectCreate);
    if (s == MH_OK) {
        MH_EnableHook((LPVOID)_addr(FN_UGAMEEFFECT_CREATE));
        HookLog("Rollback: hooked uGameEffect factory\n");
    } else {
        HookLog("Rollback: failed to hook uGameEffect factory (%d)\n", s);
    }

    s = MH_CreateHook((LPVOID)_addr(FN_BUILD_ATTACH_MATRIX),
                      (LPVOID)&hkBuildAttachMatrix,
                      (LPVOID*)&oBuildAttachMatrix);
    if (s == MH_OK) {
        MH_EnableHook((LPVOID)_addr(FN_BUILD_ATTACH_MATRIX));
        HookLog("Rollback: hooked uShot attachment transform guard\n");
    } else {
        HookLog("Rollback: failed to hook uShot attachment transform guard (%d)\n", s);
    }

    s = MH_CreateHook((LPVOID)_addr(FN_USHOT_UPDATE_HITBOXES),
                      (LPVOID)&hkShotUpdateHitboxes,
                      (LPVOID*)&oShotUpdateHitboxes);
    if (s == MH_OK) {
        MH_EnableHook((LPVOID)_addr(FN_USHOT_UPDATE_HITBOXES));
        HookLog("Rollback: hooked uShot hitbox update tail repair\n");
    } else {
        HookLog("Rollback: failed to hook uShot hitbox update tail repair (%d)\n", s);
    }

    s = MH_CreateHook((LPVOID)_addr(FN_SUNIT_REMOVE_UNIT),
                      (LPVOID)&hkUnitRemove, (LPVOID*)&oUnitRemove);
    if (s == MH_OK) {
        MH_EnableHook((LPVOID)_addr(FN_SUNIT_REMOVE_UNIT));
        HookLog("Rollback: hooked sUnit__removeUnit for recipe lifetime\n");
    } else {
        HookLog("Rollback: failed to hook sUnit__removeUnit (%d)\n", s);
    }

    s = MH_CreateHook((LPVOID)_addr(FN_KIRIGAKURE_EFFECT_UPDATE),
                      (LPVOID)&hkKirigakureEffectUpdate,
                      (LPVOID*)&oKirigakureEffectUpdate);
    if (s == MH_OK) {
        MH_EnableHook((LPVOID)_addr(FN_KIRIGAKURE_EFFECT_UPDATE));
        HookLog("Rollback: guarded uKirigakureEffect weak attachment\n");
    } else {
        HookLog("Rollback: failed to guard uKirigakureEffect attachment (%d)\n", s);
    }

}
