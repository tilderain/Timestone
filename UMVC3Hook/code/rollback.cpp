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
static constexpr uintptr_t PTR_ANEXTBATTLE  = 0x140d47e68;
static constexpr uintptr_t PTR_SGAMEUI      = 0x140d53a18;
static constexpr uintptr_t PTR_SMAIN        = 0x140e177e8;
static constexpr uintptr_t PTR_SGAMEEFFECT  = 0x140d46470;
static constexpr uintptr_t PTR_SMVC3MGR     = 0x140d510a0;
static constexpr uintptr_t PTR_SUNIT        = 0x140e17698;
static constexpr uintptr_t PTR_SCAMERA      = 0x140e17930;
static constexpr uintptr_t PTR_SCOLLISION   = 0x140e18250;

// sMvc3Main__move — the full per-frame update pipeline
static constexpr uintptr_t FN_SMVC3MAIN_MOVE = 0x1402594A0;
static constexpr uintptr_t FN_CCHR_SET_POS    = 0x140002AF0;
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
static constexpr uintptr_t FN_RCHRCPU_CLASS_FUNC = 0x140117280;
static constexpr uintptr_t FN_RNG_RANDOM_INT = 0x14014EB30;
static constexpr uintptr_t FN_HSIENKO_SELECT_ANKI = 0x1400A1260;
static constexpr uintptr_t FN_SET_RESERVED_ANKI = 0x1400D4D90;
static constexpr uintptr_t FN_BUILD_ATTACH_MATRIX = 0x14014F960;
static constexpr uintptr_t FN_KIRIGAKURE_EFFECT_UPDATE = 0x14013B050;
static constexpr uintptr_t DTI_UEFFECT = 0x140E18698;
static constexpr uintptr_t DTI_UBASEMODEL = 0x140E17D48;

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
static constexpr uint32_t SCOLLISION_BODY_START   = 0x38;
static constexpr uint32_t SCOLLISION_ALLOC_START  = 0x23970;
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
static uint64_t GetObjectDti(uint64_t objPtr) {
    uint64_t vtable = *(uint64_t*)objPtr;
    if (vtable && vtable >= 0x140000000) {
        auto fn = (uint64_t(__fastcall*)(uint64_t))(*(uint64_t*)(vtable + 0x20));
        if (fn && (uint64_t)fn >= 0x140000000) {
            return fn(objPtr);
        }
    }
    return 0;
}
static uint32_t DecodeDtiSize(uint64_t dtiPtr) {
    if (!dtiPtr || dtiPtr < 0x140000000) return 0;
    return ((*(uint32_t*)(dtiPtr + 0x30) & 0x7FFFFF) << 2);
}

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
static constexpr uintptr_t DTI_CUNIT                  = 0x140E173D0;
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

static bool IsLikelyPointer(uint64_t value)
{
    return value >= 0x10000 && value < 0x0000800000000000ull && (value & 7) == 0;
}

static bool IsAccessiblePointer(uint64_t value)
{
    if (!IsLikelyPointer(value)) return false;
    MEMORY_BASIC_INFORMATION mbi = {};
    if (VirtualQuery((void*)value, &mbi, sizeof(mbi)) == 0) return false;
    if (mbi.State != MEM_COMMIT) return false;
    return (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) == 0;
}

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

static bool HasCallableDtiVirtual(const void* object)
{
    uint64_t address = (uint64_t)object;
    if (!IsAccessiblePointer(address)) return false;

    uint64_t vtable = *(const uint64_t*)object;
    if (!IsAccessibleAddress(vtable + 0x20)) return false;

    uint64_t getDti = *(const uint64_t*)(vtable + 0x20);
    if (!getDti || !IsAccessibleAddress(getDti)) return false;

    MEMORY_BASIC_INFORMATION mbi = {};
    if (VirtualQuery((void*)getDti, &mbi, sizeof(mbi)) == 0) return false;
    const DWORD executable = PAGE_EXECUTE | PAGE_EXECUTE_READ |
        PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    return (mbi.Protect & executable) != 0;
}

static bool IsUnitObject(const void* object)
{
    if (!HasCallableDtiVirtual(object)) return false;

    uint64_t dti = GetObjectDti((uint64_t)object);
    const uint64_t unitDti = _addr(DTI_CUNIT);
    for (int depth = 0; dti && depth < 128; ++depth) {
        if (dti == unitDti) return true;
        if (!IsAccessibleAddress(dti + 0x20)) return false;
        uint64_t parent = *(const uint64_t*)(dti + 0x20);
        if (parent == dti) return false;
        dti = parent;
    }
    return false;
}

static bool IsEffectObject(const void* object)
{
    if (!HasCallableDtiVirtual(object)) return false;

    uint64_t dti = GetObjectDti((uint64_t)object);
    const uint64_t effectDti = _addr(DTI_UEFFECT);
    for (int depth = 0; dti && depth < 128; ++depth) {
        if (dti == effectDti) return true;
        if (!IsAccessibleAddress(dti + 0x20)) return false;
        uint64_t parent = *(const uint64_t*)(dti + 0x20);
        if (parent == dti) return false;
        dti = parent;
    }
    return false;
}

static bool IsBaseModelObject(const void* object)
{
    if (!HasCallableDtiVirtual(object)) return false;

    uint64_t dti = GetObjectDti((uint64_t)object);
    const uint64_t baseModelDti = _addr(DTI_UBASEMODEL);
    for (int depth = 0; dti && depth < 128; ++depth) {
        if (dti == baseModelDti) return true;
        if (!IsAccessibleAddress(dti + 0x20)) return false;
        uint64_t parent = *(const uint64_t*)(dti + 0x20);
        if (parent == dti) return false;
        dti = parent;
    }
    return false;
}

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
            bool validUnit = IsUnitObject(node);
            if (validUnit) {
                uint32_t state = *(uint32_t*)((uint8_t*)node + 0x10);
                if ((state & 7u) == 2u && (state & 0x400u) != 0) {
                    uint64_t vtable = *(uint64_t*)node;
                    validUnit = IsAccessibleAddress(vtable + 0x40) &&
                                IsExecutableAddress(*(uint64_t*)(vtable + 0x40));
                }
            }
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

static void RestoreSCollisionBody(void* destination, const uint8_t* source, size_t size)
{
    if (size < SCOLLISION_SIZE) return;

//    HookLog("Rollback: restoring inline sCollision body @ 0x%llX size=%zu\n",
            //(uint64_t)destination, size);

    struct LivePointerField { uint32_t offset; uint64_t value; };
    std::vector<LivePointerField> livePointers;
    for (uint32_t offset = SCOLLISION_BODY_START;
         offset + sizeof(uint64_t) <= SCOLLISION_ALLOC_START;
         offset += sizeof(uint64_t)) {
        uint64_t liveValue = *(uint64_t*)((uint8_t*)destination + offset);
        uint64_t savedValue = *(const uint64_t*)(source + offset);
        if (IsAccessiblePointer(liveValue) || IsAccessiblePointer(savedValue)) {
            livePointers.push_back({offset, liveValue});
        }
    }

    // Rewind the complete inline collision state, but not cSystem's base or
    // mPrivateAllocator. Heap ownership behind the preserved pointers remains
    // on the live timeline until those allocations are captured explicitly.
    std::memcpy((uint8_t*)destination + SCOLLISION_BODY_START,
                source + SCOLLISION_BODY_START,
                SCOLLISION_ALLOC_START - SCOLLISION_BODY_START);
    for (const LivePointerField& field : livePointers) {
        *(uint64_t*)((uint8_t*)destination + field.offset) = field.value;
    }
}

static void RestoreExternalAnimBlock(void* dst, const uint8_t* src, size_t size)
{
    if (size != CCHR_EXTERNAL_ANIM_SIZE) return;

    // cChrEffectSlot is an sCollision::Node, not a plain animation buffer.
    // +0x00..+0x7f owns its live vtable, geometry MtArray, child records and
    // bounding topology. Rewinding that graph inserts unrelated live objects
    // into collision jobs and eventually calls draw methods without an
    // sRender context. The owner and setup callback at +0x80/+0x88 are not
    // topology: they are the one-shot setup binding and must match the saved
    // attack. A completed single-hit attack can clear this binding; starting
    // another attack repopulates it, which previously made restore appear to
    // work only after another attack was started.
    // Restore active flag and owner/callback, but CLEAR the setup latch so
    // setupOnce fires again after restore. The latch is the one-shot gate:
    // setupOnce only invokes the callback when +0x74 is zero. Restoring a
    // post-setup latch=1 prevents multi-hit attacks from rebuilding geometry.
    std::memcpy((uint8_t*)dst + 0x08, src + 0x08, 0x01);
    *((uint8_t*)dst + 0x74) = 0; // clear setup latch to allow callback re-execution
    std::memcpy((uint8_t*)dst + 0x80, src + 0x80, 0x10); // setup owner and callback
    std::memcpy((uint8_t*)dst + 0x90, src + 0x90, 0x28); // group, attr, move, user u32
    std::memcpy((uint8_t*)dst + 0xC8, src + 0xC8, 0x18); // derived slot scalar tail
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
bool    g_isResimulating         = false;
int32_t g_resimulateCurrentFrame = 0;
int32_t g_postResimFrame         = -1;
int32_t g_resimReentranceDepth   = 0; // tracks nested re-sim calls for diagnostics

// Pending re-sim request from FrameStepper_Process (HookUpdate thread).
// hkMvc3Main_move on the main game thread checks/resets these so the
// actual oMvc3Main_move calls run on the correct thread (avoiding D3D
// thread-safety issues in the render path).
int32_t g_resimRequestedStart   = -1;
int32_t g_resimRequestedEnd     = -1;
int32_t g_restoreRequestedFrame = -1;

int     g_f7State        = 0;
int32_t g_f7StartFrame   = 0;
int     g_f7Remaining    = 0;
int     g_f7DiagSession  = -1;

// Code-only diagnostic toggle. Keep false during normal play: NDJSON capture
// serializes full character, hitbox, projectile, and memory state per frame.
bool    g_ndjsonLoggingEnabled      = true;
bool    g_ndjsonIncludeDirHistory   = false;

// Rollback request from controller.cpp — late input arrived that differs
// from what was predicted.  Start/end inclusive frame range.
// __ImageBase gives us the DLL's base address (used with GetModuleFileNameA
// to resolve paths relative to the DLL's directory instead of the CWD, which
// games often change after loading plugins).
EXTERN_C IMAGE_DOS_HEADER __ImageBase;

// Resolve a relative path to one rooted at the DLL's own directory.
// Returns the number of chars written (like snprintf), or negative on error.
// |dst| is always null-terminated.
static int ResolveDllPath(char* dst, size_t dstLen, const char* relative) {
    if (!dst || dstLen == 0) return -1;
    char modPath[260];
    DWORD ret = GetModuleFileNameA((HINSTANCE)&__ImageBase, modPath, sizeof(modPath));
    if (ret == 0 || ret >= sizeof(modPath)) { dst[0] = '\0'; return -1; }
    char* sep = strrchr(modPath, '\\');
    if (sep) *(sep + 1) = '\0'; else modPath[0] = '\0';
    return snprintf(dst, dstLen, "%s%s", modPath, relative);
}

static int32_t g_rollbackReqStart = -1;
static int32_t g_rollbackReqEnd   = -1;
static uint64_t g_sMvc3Main      = 0;
static void(__fastcall* oMvc3Main_move)(uint64_t param_1) = nullptr;
using CChrSetPos_t = void(__fastcall*)(uint64_t character, const float* position);
static CChrSetPos_t oCChrSetPos = nullptr;

static void __fastcall hkCChrSetPos(uint64_t character, const float* position)
{
    if (g_isResimulating && character && position) {
        const float* oldPosition = (const float*)(character + 0x50);
        float deltaX = position[0] - oldPosition[0];
        if (std::isfinite(deltaX) && std::fabs(deltaX) >= 0.001f) {
            HookLog("[ResimSetPos] frame=%d chr=0x%llX caller=0x%llX "
                    "old=[%.9g,%.9g,%.9g] src=[%.9g,%.9g,%.9g] dx=%.9g\n",
                    g_resimulateCurrentFrame, character,
                    (uint64_t)__builtin_return_address(0),
                    oldPosition[0], oldPosition[1], oldPosition[2],
                    position[0], position[1], position[2], deltaX);
        }
    }
    oCChrSetPos(character, position);
}
// Trampoline for the render/present call — NOP'd during re-simulation
static void(__fastcall* oCallSRender)(uint64_t param_1) = nullptr;
// Trampoline for sMain__move (frame-timing / QPC / Sleep / busy-wait)
static void(__fastcall* oMain_move)(uint64_t param_1) = nullptr;
// Trampoline for sSound__move. Audio scene state is not rollback gameplay state.
static void(__fastcall* oSound_move)(uint64_t param_1) = nullptr;
// Trampoline for sGameUI__move. UI scheduler state is not rollback gameplay state.
static void(__fastcall* oGameUi_move)(uint64_t param_1) = nullptr;
// Trampoline for resource-backed scheduler setup. Its arrays are allocator-owned.
static void(__fastcall* oSchedulerRebuild)(uint64_t scheduler) = nullptr;
static bool(__fastcall* oRChrCpuClassFunc)(void* cpu, void* object) = nullptr;
// Trampoline for the fighter pushbox collision callback.
static void(__fastcall* oApplyCollisionPushCorrection)(
    uint64_t character,
    int callbackPhase,
    uint64_t collisionObject,
    uint64_t otherCharacter,
    uint64_t collisionResult,
    uint32_t collisionSlot) = nullptr;

// ===========================================================================
// Resim diagnostic dumper — binary + text state capture
// ===========================================================================
static int    g_diagSessionId  = 0;
static char   g_diagDir[260]   = {};
static int    g_diagWriteCount = 0;
static FILE*  g_diagNdjsonFile    = nullptr;
static DWORD  g_diagNdjsonFlush   = 0;

// Netplay per-frame live NDJSON logging
static bool   g_netplayLiveInit   = false;
static char   g_netplayLivePath[260] = {};
static int    g_netplayLiveReset   = 0;

static std::string g_pendingNPLiveJSON[512];
static int32_t     g_pendingNPLiveFrame[512];
static constexpr int NETPLAY_LIVE_HISTORY = 512;
static std::string g_netplayLiveHistory[NETPLAY_LIVE_HISTORY];
static int32_t     g_netplayLiveHistoryFrame[NETPLAY_LIVE_HISTORY];
static bool        g_netplayLiveDesyncFlushed = false;

// Ground-truth verification: locally queued re-simulation states, paired with
// remote ground truth data upon receipt of a GroundTruthPacket.
struct PendingResimDump {
    int32_t frame;
    ResimCharState p1Post;
    ResimCharState p2Post;
};
static constexpr int PENDING_RESIM_DUMP_RING = 256;
static PendingResimDump g_pendingResimDumps[PENDING_RESIM_DUMP_RING] = {};
static bool             g_pendingResimDumpValid[PENDING_RESIM_DUMP_RING] = {};

// ===========================================================================
// Diag Snapshot Accessors — the ONLY place that should read g_resimFrameSnap
// / g_liveFrameSnap. Centralizes the frame-stamp check that was previously
// missing at three separate call sites.
// ===========================================================================
static const FrameDiagSnapshot* Rollback_GetResimSnapshot(int32_t frame) {
    uint32_t idx = (uint32_t)(frame >= 0 ? frame : 0) % ROLLBACK_WINDOW_DIAG;
    const FrameDiagSnapshot& snap = g_resimFrameSnap[idx];
    if (snap.frame != frame) {
        HookLog("Rollback: stale resim diag slot for frame %d (slot holds frame %d) — refusing read\n",
                frame, snap.frame);
        return nullptr;
    }
    return &snap;
}

static const FrameDiagSnapshot* Rollback_GetLiveSnapshot(int32_t frame) {
    uint32_t idx = (uint32_t)(frame >= 0 ? frame : 0) % ROLLBACK_WINDOW_DIAG;
    const FrameDiagSnapshot& snap = g_liveFrameSnap[idx];
    if (snap.frame != frame) {
        HookLog("Rollback: stale live diag slot for frame %d (slot holds frame %d) — refusing read\n",
                frame, snap.frame);
        return nullptr;
    }
    return &snap;
}

// Pulls P1/P2 point-character post-state out of a snapshot. Returns false
// if either character isn't present (so callers don't have to repeat the
// teamIdx/charSlot scan loop).
static bool Rollback_FindP1P2Post(const FrameDiagSnapshot& snap,
                                   const CharDiagSnapshot** p1Post,
                                   const CharDiagSnapshot** p2Post)
{
    *p1Post = nullptr;
    *p2Post = nullptr;
    for (int ci = 0; ci < snap.charCount && ci < 6; ci++) {
        uint32_t teamIdx = snap.chars[ci].post.teamIdx;
        uint32_t slot    = snap.chars[ci].post.charSlot;
        if (teamIdx == 0 && slot == 0) *p1Post = &snap.chars[ci].post;
        if (teamIdx == 1 && slot == 0) *p2Post = &snap.chars[ci].post;
    }
    return *p1Post && *p2Post;
}

static void Rollback_FillResimCharState(ResimCharState& dst, const CharDiagSnapshot& s)
{
    dst.actionId = s.actionId;
    dst.posX     = s.posX;
    dst.posY     = s.posY;
    dst.posZ     = s.posZ;
    dst.flags    = s.flags14F0;
    dst.mHeld    = *(uint32_t*)(s.cchr + 0x14);
    dst.frameCnt = *(uint64_t*)(s.cchr + 0x18);
    for (int ri = 0; ri < 4; ri++)
        dst.rng[ri] = s.xorShift[ri];
}

static void Rollback_NetplayLiveInit() {
    if (!g_ndjsonLoggingEnabled) return;
    g_netplayLiveInit = false;
    g_netplayLivePath[0] = '\0';

    char dir[260];
    SYSTEMTIME st;
    GetLocalTime(&st);
    char rel[80];
    
    // Append the local netplay role as a suffix to the directory name
    const char* sideSuffix = Netplay_IsActive() ? (Netplay_IsHost() ? "_host" : "_client") : "_offline";
    if (g_netplayLiveReset > 0) {
        snprintf(rel, sizeof(rel), "nplive_%04d%02d%02d_%02d%02d%02d%s_r%d",
                 st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond,
                 sideSuffix, g_netplayLiveReset);
    } else {
        snprintf(rel, sizeof(rel), "nplive_%04d%02d%02d_%02d%02d%02d%s",
                 st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond,
                 sideSuffix);
    }
             
    ResolveDllPath(dir, sizeof(dir), rel);
    CreateDirectoryA(dir, NULL);

    snprintf(g_netplayLivePath, sizeof(g_netplayLivePath), "%s/frames.ndjson", dir);
    FILE* f = fopen(g_netplayLivePath, "w");
    if (f) fclose(f);

    char logPath[260];
    const char* logName = Netplay_IsHost() ? "UMVC3Hook_host.log" : "UMVC3Hook_client.log";
    snprintf(logPath, sizeof(logPath), "%s/%s", dir, logName);
    HookLogSetFile(logPath);

    for (int i = 0; i < 256; i++) {
        g_pendingNPLiveFrame[i] = -1;
        g_pendingNPLiveJSON[i].clear();
    }
    for (int i = 0; i < NETPLAY_LIVE_HISTORY; i++) {
        g_netplayLiveHistoryFrame[i] = -1;
        g_netplayLiveHistory[i].clear();
    }
    g_netplayLiveDesyncFlushed = false;

    HookLog("NetplayLive: NDJSON log -> %s\n", g_netplayLivePath);
    g_netplayLiveInit = true;
}

bool Rollback_GetNetplayLiveDirectory(char* path, size_t pathSize)
{
    if (!path || pathSize == 0 || !g_netplayLiveInit || !g_netplayLivePath[0])
        return false;

    const char* slash = strrchr(g_netplayLivePath, '/');
    const char* backslash = strrchr(g_netplayLivePath, '\\');
    if (!slash || (backslash && backslash > slash))
        slash = backslash;
    if (!slash)
        return false;

    const size_t directoryLength = static_cast<size_t>(slash - g_netplayLivePath);
    if (directoryLength + 1 > pathSize)
        return false;

    memcpy(path, g_netplayLivePath, directoryLength);
    path[directoryLength] = '\0';
    return true;
}

void Rollback_ResetNetplayLiveLog()
{
    if (!g_ndjsonLoggingEnabled) return;
    ++g_netplayLiveReset;
    Rollback_NetplayLiveInit();
}

void Rollback_ApplyNetplayDeterminism()
{
    using SetFrameWaitFn = void(__fastcall*)(uint64_t, bool);
    using SetMoveLineParallelFn = void(__fastcall*)(uint64_t, int, bool);
    using GetCollisionMoveMatrixFn = uint64_t(__fastcall*)(uint64_t);
    using GetInstanceFn = uint64_t(__fastcall*)();
    using SetBoolFn = void(__fastcall*)(uint64_t, bool);

    uint64_t sMain = *(uint64_t*)_addr(0x140e177e8);
    uint64_t sUnit = *(uint64_t*)_addr(PTR_SUNIT);
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

    HookLog("Netplay: applied native battle determinism settings\n");
}

static std::string Rollback_GenerateHitboxJSON(uint64_t chr) {
    static constexpr uint32_t capsuleBase = 0x4210;
    static constexpr uint32_t capsuleStride = 0xB0;
    static constexpr uint32_t sphereBases[] = {0x4E20, 0x5530, 0x5C40};
    static constexpr uint32_t sphereStride = 0x70;

    std::string out = "[";
    bool first = true;
    char entry[384];

    for (uint32_t index = 0; index < 16; ++index) {
        uint64_t primitive = chr + capsuleBase + index * capsuleStride;
        if (*(uint8_t*)(primitive + 0x08) == 0) continue;

        float x0 = *(float*)(primitive + 0x20);
        float y0 = *(float*)(primitive + 0x24);
        float z0 = *(float*)(primitive + 0x28);
        float x1 = *(float*)(primitive + 0x30);
        float y1 = *(float*)(primitive + 0x34);
        float z1 = *(float*)(primitive + 0x38);
        float radius = *(float*)(primitive + 0x40);
        float dx = x1 - x0;
        float dy = y1 - y0;
        float dz = z1 - z0;
        float segmentLength = std::sqrt(dx * dx + dy * dy + dz * dz);
        if (!std::isfinite(x0) || !std::isfinite(y0) || !std::isfinite(z0) ||
            !std::isfinite(x1) || !std::isfinite(y1) || !std::isfinite(z1) ||
            !std::isfinite(radius) || !std::isfinite(segmentLength)) {
            continue;
        }

        snprintf(entry, sizeof(entry),
                 "%s{\"bank\":\"capsule\",\"index\":%u,\"shape\":\"capsule\",\"damaging\":true,"
                 "\"pos\":[%.9g,%.9g,%.9g],"
                 "\"size\":[%.9g,%.9g,%.9g],\"radius\":%.9g,\"length\":%.9g}",
                 first ? "" : ",", index,
                 (x0 + x1) * 0.5f, (y0 + y1) * 0.5f, (z0 + z1) * 0.5f,
                 std::fabs(dx) + radius * 2.0f,
                 std::fabs(dy) + radius * 2.0f,
                 std::fabs(dz) + radius * 2.0f,
                 radius, segmentLength + radius * 2.0f);
        out += entry;
        first = false;
    }

    for (uint32_t bank = 0; bank < 3; ++bank) {
        for (uint32_t index = 0; index < 16; ++index) {
            uint64_t primitive = chr + sphereBases[bank] + index * sphereStride;
            if (*(uint8_t*)(primitive + 0x08) == 0) continue;

            float x = *(float*)(primitive + 0x20);
            float y = *(float*)(primitive + 0x24);
            float z = *(float*)(primitive + 0x28);
            float radius = *(float*)(primitive + 0x2C);
            int32_t parent = *(int32_t*)(primitive + 0x40);
            float localX = *(float*)(primitive + 0x50);
            float localY = *(float*)(primitive + 0x54);
            float localZ = *(float*)(primitive + 0x58);
            if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z) ||
                !std::isfinite(radius) || !std::isfinite(localX) ||
                !std::isfinite(localY) || !std::isfinite(localZ)) {
                continue;
            }

            snprintf(entry, sizeof(entry),
                     "%s{\"bank\":\"sphere%u\",\"index\":%u,\"shape\":\"sphere\",\"damaging\":false,"
                     "\"pos\":[%.9g,%.9g,%.9g],"
                     "\"size\":[%.9g,%.9g,%.9g],\"radius\":%.9g,"
                     "\"parent\":%d,\"local\":[%.9g,%.9g,%.9g]}",
                     first ? "" : ",", bank, index, x, y, z,
                     radius * 2.0f, radius * 2.0f, radius * 2.0f, radius,
                     parent, localX, localY, localZ);
            out += entry;
            first = false;
        }
    }

    out += "]";
    return out;
}

static std::string Rollback_GenerateShotJSON();

std::string Rollback_GenerateNetplayLiveJSON(int32_t frame, bool isResim,
                                             const char* phase = nullptr,
                                             int32_t rollbackStart = -1,
                                             int32_t rollbackEnd = -1) {
    if (!g_ndjsonLoggingEnabled) return "";
    if (frame == 0) return "";
    char tsBuf[32] = {};
    SYSTEMTIME st;
    GetLocalTime(&st);
    snprintf(tsBuf, sizeof(tsBuf), "%04d-%02d-%02dT%02d:%02d:%02d",
             st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);

    std::string sideStr = Netplay_IsHost() ? "host" : "client";
    if (phase) {
        sideStr += "_";
        sideStr += phase;
    } else if (isResim) {
        sideStr += "_resim";
    }
    const char* side = sideStr.c_str();

    // sMain+0x80 = mDeltaTime: wall-clock QPC-derived frame delta consumed by
    // the dash double-tap accumulators in cChrInput_inputMaskPostProcess
    // (acc += this value each frame).  Logged at full precision to detect
    // cross-machine timing jitter baked into dash detection state.
    float mainDt = 0.0f;
    {
        uint64_t sMain = *(uint64_t*)_addr(PTR_SMAIN);
        if (sMain) mainDt = *(float*)(sMain + 0x80);
    }

    char header[512];
    if (rollbackStart >= 0 && rollbackEnd >= rollbackStart) {
        snprintf(header, sizeof(header),
                 "{\"v\":1,\"ts\":\"%s\",\"frame\":%d,\"side\":\"%s\","
                 "\"rollback_start\":%d,\"rollback_end\":%d,\"dt\":%.9g,\"chars\":[",
                 tsBuf, frame, side, rollbackStart, rollbackEnd, mainDt);
    } else {
        snprintf(header, sizeof(header),
                 "{\"v\":1,\"ts\":\"%s\",\"frame\":%d,\"side\":\"%s\",\"dt\":%.9g,"
                 "\"chars\":[",
                 tsBuf, frame, side, mainDt);
    }

    std::string out = header;

    uint64_t sCharacter = *(uint64_t*)_addr(PTR_SCHARACTER);
    bool firstChar = true;
    if (sCharacter) {
        for (int ti = 0; ti < 2; ++ti) {
            uint64_t team = sCharacter + (ti == 0 ? TEAM0_OFFSET : TEAM1_OFFSET);
            uint64_t node = team + TEAM_FIRST_NODE_OFFSET;
            int ci = 0;
            while (node) {
                uint64_t chr = *(uint64_t*)(node + NODE_CHR_OFFSET);
                if (chr) {
                    CharDiagSnapshot snap;
                    CaptureCharSnapshot(chr, ti, ci, snap);

                    if (!firstChar) out += ",";
                    firstChar = false;

                    char charBuf[4096];
                    int32_t memberId = *(int32_t*)(chr + 0x2DB0);
                    int32_t plrId = *(int32_t*)(chr + 0x2DA0);
                    int32_t assistType = *(int32_t*)(chr + 0x2DBC);
                    std::string hitboxes = Rollback_GenerateHitboxJSON(chr);

                    // Dash/double-tap detection state (cChrInput_inputMaskPostProcess
                    // @0x14002E5A0): per-bit elapsed-frame timers and tap counters.
                    //   +0x260: float[32] frame timer since last tap (+= sMain+0x80/frame)
                    //   +0x2e0: int[32]   consecutive tap count (>=3 sets mask +0x238)
                    //   +0x130: float[32] frame timer (2.0/3.0 threshold windows)
                    // Mask words: +0x12c, +0x230, +0x238, +0x250.
                    // Only nonzero entries are emitted as "i:value" pairs.
                    std::string dashState;
                    {
                        char tmp[128];
                        dashState = "\"acc260\":{";
                        bool first = true;
                        for (int bi = 0; bi < 32; bi++) {
                            float v = *(float*)(snap.cchr + 0x260 + bi * 4);
                            if (v != 0.0f) {
                                if (!first) dashState += ",";
                                snprintf(tmp, sizeof(tmp), "\"%d\":%.9g", bi, v);
                                dashState += tmp; first = false;
                            }
                        }
                        dashState += "},\"tap2e0\":{";
                        first = true;
                        for (int bi = 0; bi < 32; bi++) {
                            int32_t v = *(int32_t*)(snap.cchr + 0x2e0 + bi * 4);
                            if (v != 0) {
                                if (!first) dashState += ",";
                                snprintf(tmp, sizeof(tmp), "\"%d\":%d", bi, v);
                                dashState += tmp; first = false;
                            }
                        }
                        dashState += "},\"acc130\":{";
                        first = true;
                        for (int bi = 0; bi < 32; bi++) {
                            float v = *(float*)(snap.cchr + 0x130 + bi * 4);
                            if (v != 0.0f) {
                                if (!first) dashState += ",";
                                snprintf(tmp, sizeof(tmp), "\"%d\":%.9g", bi, v);
                                dashState += tmp; first = false;
                            }
                        }
                        snprintf(tmp, sizeof(tmp),
                                 "},\"m12c\":\"0x%08X\",\"m230\":\"0x%08X\","
                                 "\"m238\":\"0x%08X\",\"m250\":\"0x%08X\"",
                                 *(uint32_t*)(snap.cchr + 0x12c),
                                 *(uint32_t*)(snap.cchr + 0x230),
                                 *(uint32_t*)(snap.cchr + 0x238),
                                 *(uint32_t*)(snap.cchr + 0x250));
                        dashState += tmp;
                    }

                    if (g_ndjsonIncludeDirHistory) {
                        uint64_t dirFrameCnt = *(uint64_t*)(snap.cchr + 0x18);
                        uint32_t dirSlot = dirFrameCnt & 0x3F;
                        uint32_t dir0   = *(uint32_t*)(snap.cchr + 0x1B0);
                        uint32_t dir4   = *(uint32_t*)(snap.cchr + 0x1B4);
                        uint32_t h590   = *(uint32_t*)(snap.cchr + 0x590);
                        uint32_t h594   = *(uint32_t*)(snap.cchr + 0x594);

                        snprintf(charBuf, sizeof(charBuf),
                                "{\"team\":%d,\"slot\":%d,\"mid\":%d,"
                                "\"plr\":%d,\"ast\":%d,"
                                "\"pos\":[%.9g,%.9g,%.9g],"
                                 "\"act\":\"0x%08X\",\"mChrF\":\"0x%08X\","
                                 "\"mAnmF\":\"0x%08X\",\"mPrgF\":\"0x%08X\","
                                "\"mHeld\":\"0x%08X\",\"fcnt\":%llu,"
                                "\"mOld\":\"0x%08X\","
                                "\"mPressed\":\"0x%08X\",\"mReleased\":\"0x%08X\","
                                "\"mMask\":\"0x%08X\",\"mProcessed\":\"0x%08X\","
                                 "\"dirSlot\":%u,"
                                 "\"dir0\":\"0x%08X\",\"dir4\":\"0x%08X\","
                                 "\"h590\":\"0x%08X\",\"h594\":\"0x%08X\","
                                 "\"rng\":[\"0x%08X\",\"0x%08X\",\"0x%08X\",\"0x%08X\"]",
                                ti, ci, memberId,
                                plrId, assistType,
                                snap.posX, snap.posY, snap.posZ,
                                snap.actionId, snap.flags14F0,
                                snap.flags14F4, snap.flags14F8,
                                *(uint32_t*)(snap.cchr + 0x14),
                                dirFrameCnt,
                                *(uint32_t*)(snap.cchr + 0x10),
                                *(uint32_t*)(snap.cchr + 0x20),
                                *(uint32_t*)(snap.cchr + 0x24),
                                *(uint32_t*)(snap.cchr + 0x568),
                                *(uint32_t*)(snap.cchr + 0x56C),
                                 dirSlot,
                                 dir0, dir4,
                                 h590, h594,
                                 snap.xorShift[0], snap.xorShift[1],
                                snap.xorShift[2], snap.xorShift[3]);
                        out += charBuf;
                        out += ",";
                        out += dashState;
                        out += ",\"hitboxes\":";
                        out += hitboxes;
                        out += "}";
                    } else {
                        snprintf(charBuf, sizeof(charBuf),
                                "{\"team\":%d,\"slot\":%d,\"mid\":%d,"
                                "\"plr\":%d,\"ast\":%d,"
                                "\"pos\":[%.9g,%.9g,%.9g],"
                                 "\"act\":\"0x%08X\",\"mChrF\":\"0x%08X\","
                                 "\"mAnmF\":\"0x%08X\",\"mPrgF\":\"0x%08X\","
                                "\"mHeld\":\"0x%08X\",\"fcnt\":%llu,"
                                "\"mOld\":\"0x%08X\","
                                "\"mPressed\":\"0x%08X\",\"mReleased\":\"0x%08X\","
                                "\"mMask\":\"0x%08X\",\"mProcessed\":\"0x%08X\","
                                "\"rng\":[\"0x%08X\",\"0x%08X\",\"0x%08X\",\"0x%08X\"]",
                                ti, ci, memberId,
                                plrId, assistType,
                                snap.posX, snap.posY, snap.posZ,
                                snap.actionId, snap.flags14F0,
                                snap.flags14F4, snap.flags14F8,
                                *(uint32_t*)(snap.cchr + 0x14),
                                *(uint64_t*)(snap.cchr + 0x18),
                                *(uint32_t*)(snap.cchr + 0x10),
                                *(uint32_t*)(snap.cchr + 0x20),
                                *(uint32_t*)(snap.cchr + 0x24),
                                *(uint32_t*)(snap.cchr + 0x568),
                                *(uint32_t*)(snap.cchr + 0x56C),
                                snap.xorShift[0], snap.xorShift[1],
                                snap.xorShift[2], snap.xorShift[3]);
                        out += charBuf;
                        out += ",";
                        out += dashState;
                        out += ",\"hitboxes\":";
                        out += hitboxes;
                        out += "}";
                    }
                    ci++;
                }
                node = *(uint64_t*)(node + NODE_NEXT_OFFSET);
            }
        }
    }

    out += "],\"shots\":";
    out += Rollback_GenerateShotJSON();
    out += "}\n";
    return out;
}

static void Rollback_WriteNetplayLiveJSON(const std::string& json) {
    if (!g_ndjsonLoggingEnabled) return;
    if (!g_netplayLiveInit) Rollback_NetplayLiveInit();
    if (!g_netplayLivePath[0] || g_netplayLiveDesyncFlushed || json.empty()) return;

    int32_t frame = -1;
    const char* frameKey = std::strstr(json.c_str(), "\"frame\":");
    if (frameKey) frame = (int32_t)std::strtol(frameKey + 8, nullptr, 10);
    if (frame < 0) return;

    int idx = (uint32_t)frame % NETPLAY_LIVE_HISTORY;
    if (g_netplayLiveHistoryFrame[idx] != frame) {
        g_netplayLiveHistoryFrame[idx] = frame;
        g_netplayLiveHistory[idx].clear();
    }
    g_netplayLiveHistory[idx] += json;
}

extern "C" void Rollback_NotifyDesyncTriggered(int32_t frame)
{
    if (!g_ndjsonLoggingEnabled) return;
    if (g_netplayLiveDesyncFlushed) return;
    if (!g_netplayLiveInit) Rollback_NetplayLiveInit();
    if (!g_netplayLivePath[0]) return;

    // Move still-predicted records into the history before flushing it.
    for (int i = 0; i < 256; i++) {
        if (g_pendingNPLiveFrame[i] >= 0) {
            Rollback_WriteNetplayLiveJSON(g_pendingNPLiveJSON[i]);
            g_pendingNPLiveFrame[i] = -1;
        }
    }

    FILE* f = fopen(g_netplayLivePath, "a");
    if (!f) return;

    int32_t firstFrame = frame - NETPLAY_LIVE_HISTORY + 1;
    for (int32_t current = firstFrame; current <= frame; current++) {
        int idx = (uint32_t)current % NETPLAY_LIVE_HISTORY;
        if (g_netplayLiveHistoryFrame[idx] == current)
            fputs(g_netplayLiveHistory[idx].c_str(), f);
    }
    fclose(f);
    g_netplayLiveDesyncFlushed = true;
    HookLog("NetplayLive: flushed last %d frames through desync trigger frame %d\n",
            NETPLAY_LIVE_HISTORY, frame);
}

extern "C" void Rollback_FlushNetplayLive(int32_t frame)
{
    if (!g_ndjsonLoggingEnabled) return;
    if (!g_netplayLiveInit) Rollback_NetplayLiveInit();
    if (!g_netplayLivePath[0] || g_netplayLiveDesyncFlushed) return;

    // A clean disconnect can happen before either side reaches the local
    // desync threshold. Preserve the same rolling window in that case.
    for (int i = 0; i < 256; i++) {
        if (g_pendingNPLiveFrame[i] >= 0) {
            Rollback_WriteNetplayLiveJSON(g_pendingNPLiveJSON[i]);
            g_pendingNPLiveFrame[i] = -1;
        }
    }

    int32_t newestFrame = frame;
    if (newestFrame < 0) {
        for (int i = 0; i < NETPLAY_LIVE_HISTORY; i++)
            if (g_netplayLiveHistoryFrame[i] > newestFrame)
                newestFrame = g_netplayLiveHistoryFrame[i];
    }
    if (newestFrame < 0) return;

    FILE* f = fopen(g_netplayLivePath, "a");
    if (!f) return;

    int32_t firstFrame = newestFrame - NETPLAY_LIVE_HISTORY + 1;
    for (int32_t current = firstFrame; current <= newestFrame; current++) {
        int idx = (uint32_t)current % NETPLAY_LIVE_HISTORY;
        if (g_netplayLiveHistoryFrame[idx] == current)
            fputs(g_netplayLiveHistory[idx].c_str(), f);
    }
    fclose(f);
    g_netplayLiveDesyncFlushed = true;
    HookLog("NetplayLive: final flush of last %d frames through frame %d\n",
            NETPLAY_LIVE_HISTORY, newestFrame);
}

bool Rollback_FlushNDJSONOnCrash()
{
    bool flushed = false;

    // rdiag records are written immediately but its stdio buffer is normally
    // flushed only once per second.
    if (g_diagNdjsonFile && fflush(g_diagNdjsonFile) == 0)
        flushed = true;

    if (!g_ndjsonLoggingEnabled || !g_netplayLiveInit ||
        !g_netplayLivePath[0] || g_netplayLiveDesyncFlushed) {
        return flushed;
    }

    int32_t newestFrame = -1;
    for (int i = 0; i < NETPLAY_LIVE_HISTORY; ++i) {
        if (g_netplayLiveHistoryFrame[i] > newestFrame)
            newestFrame = g_netplayLiveHistoryFrame[i];
    }
    for (int i = 0; i < 256; ++i) {
        if (g_pendingNPLiveFrame[i] > newestFrame)
            newestFrame = g_pendingNPLiveFrame[i];
    }
    if (newestFrame < 0)
        return flushed;

    FILE* file = fopen(g_netplayLivePath, "a");
    if (!file)
        return flushed;

    const int32_t firstFrame = newestFrame - NETPLAY_LIVE_HISTORY + 1;
    for (int32_t frame = firstFrame; frame <= newestFrame; ++frame) {
        const int historyIndex = (uint32_t)frame % NETPLAY_LIVE_HISTORY;
        if (g_netplayLiveHistoryFrame[historyIndex] == frame) {
            const std::string& json = g_netplayLiveHistory[historyIndex];
            fwrite(json.data(), 1, json.size(), file);
        }

        const int pendingIndex = (uint32_t)frame % 256;
        if (g_pendingNPLiveFrame[pendingIndex] == frame) {
            const std::string& json = g_pendingNPLiveJSON[pendingIndex];
            fwrite(json.data(), 1, json.size(), file);
        }
    }

    flushed = fflush(file) == 0 || flushed;
    fclose(file);
    return flushed;
}

extern "C" void Rollback_CommitPendingNetplayLive(int32_t frame) {
    if (!g_ndjsonLoggingEnabled) return;
    int idx = (uint32_t)frame % 256;
    if (g_pendingNPLiveFrame[idx] == frame) {
        Rollback_WriteNetplayLiveJSON(g_pendingNPLiveJSON[idx]);
        g_pendingNPLiveFrame[idx] = -1;
    }
}

extern "C" void Rollback_DiscardPendingNetplayLive(int32_t frame) {
    if (!g_ndjsonLoggingEnabled) return;
    int idx = (uint32_t)frame % 256;
    if (g_pendingNPLiveFrame[idx] == frame) {
        g_pendingNPLiveFrame[idx] = -1;
    }
}

static void Rollback_DumpNetplayLive(int32_t frame) {
    if (!g_ndjsonLoggingEnabled) return;
    if (!g_netplayLiveInit) Rollback_NetplayLiveInit();
    if (!g_netplayLivePath[0]) return;

    std::string json = Rollback_GenerateNetplayLiveJSON(frame, false);
    if (json.empty()) return;

    if (frame < 12) {
        Rollback_WriteNetplayLiveJSON(json);
        return;
    }

    int idx = (uint32_t)frame % 256;
    if (Controller_IsFrameConfirmed(frame)) {
        Rollback_WriteNetplayLiveJSON(json);
        g_pendingNPLiveFrame[idx] = -1;
    } else {
        g_pendingNPLiveJSON[idx] = json;
        g_pendingNPLiveFrame[idx] = frame;
    }
}

// Captures character state into the diag ring buffer.
// If postOnly is false (default): captures both pre and post from the current
//   memory state (used for live frames and initial resim pre-capture).
// If postOnly is true: captures only the post-state, leaving pre untouched
//   (used for resim post-capture after the frame tick to preserve the pre-tick
//    state that was captured before oMvc3Main_move).
static void Rollback_CaptureFrameDiagSnapshot(int32_t frame, bool isResim, bool postOnly = false)
{
    if (!g_ndjsonLoggingEnabled) return;
    FrameDiagSnapshot& dst = isResim ? g_resimFrameSnap[(uint32_t)(frame >= 0 ? frame : 0) % ROLLBACK_WINDOW_DIAG]
                                      : g_liveFrameSnap[(uint32_t)(frame >= 0 ? frame : 0) % ROLLBACK_WINDOW_DIAG];
    if (!postOnly) {
        dst.charCount = 0;
        dst.frame = frame;
    }

    uint64_t sCharacter = *(uint64_t*)_addr(PTR_SCHARACTER);
    if (!sCharacter) return;

    int ci = 0;
    for (int ti = 0; ti < 2; ++ti) {
        uint64_t team = sCharacter + (ti == 0 ? TEAM0_OFFSET : TEAM1_OFFSET);
        uint64_t node = team + TEAM_FIRST_NODE_OFFSET;
        int slotInTeam = 0;
        while (node && (postOnly ? (ci < dst.charCount) : (dst.charCount < 6))) {
            uint64_t chr = *(uint64_t*)(node + NODE_CHR_OFFSET);
            if (chr) {
                CharDiagSnapshot snap;
                CaptureCharSnapshot(chr, ti, slotInTeam, snap);
                if (postOnly) {
                    dst.chars[ci].post = snap;
                } else {
                    dst.chars[dst.charCount].pre  = snap;
                    dst.chars[dst.charCount].post = snap;
                    dst.charCount++;
                }
                ci++;
                slotInTeam++;
            }
            node = *(uint64_t*)(node + NODE_NEXT_OFFSET);
        }
    }
}

static void Rollback_DiagInit();

// ---------------------------------------------------------------------------
// Rollback_LogResimNotifyNDJSON — called on the RECEIVER side when a resim
// notify packet arrives from the remote peer.  Writes the sender's pre-state
// as side="ground_truth" and post-state as side="resim" to both the nplive
// NDJSON and the rdiag.ndjson (when available).
// ---------------------------------------------------------------------------
void Rollback_LogResimNotifyNDJSON(const ResimNotifyPacket& pkt)
{
    if (!g_ndjsonLoggingEnabled) return;
    char tsBuf[32] = {};
    SYSTEMTIME st;
    GetLocalTime(&st);
    snprintf(tsBuf, sizeof(tsBuf), "%04d-%02d-%02dT%02d:%02d:%02d",
             st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);

    const char* senderSide = Netplay_IsHost() ? "host" : "client";

    auto writeEntry = [&](FILE* f, int32_t frame, const char* side,
                          const ResimCharState& p1, const ResimCharState& p2)
    {
        fprintf(f, "{\"v\":1,\"ts\":\"%s\",\"frame\":%d,"
                   "\"side\":\"%s\",\"resim_notify\":true,"
                   "\"senderSide\":\"%s\",\"correctedTeam\":%u,"
                   "\"corrected_frames\":[%d,%d],"
                   "\"chars\":[",
                tsBuf, frame, side, senderSide, pkt.correctedTeam,
                pkt.resimStartFrame, pkt.resimEndFrame);
        // P1 point
        fprintf(f, "{\"team\":0,\"slot\":0,"
                   "\"pos\":[%.4f,%.4f,%.4f],"
                             "\"act\":\"0x%08X\",\"mChrF\":\"0x%08X\","
                   "\"mHeld\":\"0x%08X\",\"fcnt\":%llu,"
                   "\"rng\":[\"0x%08X\",\"0x%08X\",\"0x%08X\",\"0x%08X\"]}",
                p1.posX, p1.posY, p1.posZ,
                 p1.actionId, p1.flags,
                p1.mHeld, (unsigned long long)p1.frameCnt,
                p1.rng[0], p1.rng[1], p1.rng[2], p1.rng[3]);
        // P2 point
        fprintf(f, ",{\"team\":1,\"slot\":0,"
                   "\"pos\":[%.4f,%.4f,%.4f],"
                             "\"act\":\"0x%08X\",\"mChrF\":\"0x%08X\","
                   "\"mHeld\":\"0x%08X\",\"fcnt\":%llu,"
                   "\"rng\":[\"0x%08X\",\"0x%08X\",\"0x%08X\",\"0x%08X\"]}",
                p2.posX, p2.posY, p2.posZ,
                p2.actionId, p2.flags,
                p2.mHeld, (unsigned long long)p2.frameCnt,
                p2.rng[0], p2.rng[1], p2.rng[2], p2.rng[3]);
        fprintf(f, "]}\n");
    };

    // --- Write to rdiag.ndjson (pre-state as ground_truth, post-state as resim) ---
    // Initialize the diag directory if this side hasn't created one yet
    // (e.g. receiver that never ran Rollback_ReSimulate locally).
    // Remote notify packets are kept out of nplive to ensure nplive only logs local simulation.
    Rollback_DiagInit();
    if (g_diagNdjsonFile) {
        for (uint32_t fi = 0; fi < pkt.frameCount; fi++) {
            const auto& rf = pkt.frames[fi];
            writeEntry(g_diagNdjsonFile, rf.frame, "ground_truth", rf.p1Pre, rf.p2Pre);
            writeEntry(g_diagNdjsonFile, rf.frame, "resim", rf.p1Post, rf.p2Post);
        }
        DWORD now = GetTickCount();
        if (now - g_diagNdjsonFlush >= 1000) {
            fflush(g_diagNdjsonFile);
            g_diagNdjsonFlush = now;
        }
    }
}

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
    if (!effect || !IsAccessiblePointer((uint64_t)effect + 0x230)) return;

    // uEffect::~uEffect releases the cResource at +0x108. A torn effect can
    // retain a small scalar here (0x120 in the 04:06 crash), which makes
    // sResource::release decrement memory at scalar+0x4c. Keep valid native
    // resources; suppress only values that cannot be resource objects.
    uint64_t resource = *(uint64_t*)((uint8_t*)effect + 0x108);
    if (resource != 0 &&
        (!IsAccessibleAddress(resource + 0x4C) ||
         !HasCallableDtiVirtual((void*)resource))) {
        *(uint64_t*)((uint8_t*)effect + 0x108) = 0;
    }

    // uEffect::~uEffect deletes the generator chain at +0x218 and releases the
    // particle sub-buffer array at +0x220. Prediction-only effects can outlive
    // the graph that populated these fields until sUnit's deferred cleanup.
    // Clear only malformed ownership; valid native allocations remain intact.
    uint64_t generator = *(uint64_t*)((uint8_t*)effect + 0x218);
    uint64_t node = generator;
    std::unordered_set<uint64_t> visited;
    bool validGeneratorChain = true;
    for (int count = 0; node != 0 && count < 4096; ++count) {
        if (!IsAccessiblePointer(node + 0x18) || !visited.insert(node).second) {
            validGeneratorChain = false;
            break;
        }
        uint64_t vtable = *(uint64_t*)node;
        static constexpr uint32_t requiredVirtuals[] = {
            0x00, // deleting destructor
            0x38, 0x40, 0x50, 0x70, 0x88, // setup/update/finish path
            0xA0, 0xA8              // draw path
        };
        if (!IsAccessibleAddress(vtable + 0xA8)) {
            validGeneratorChain = false;
            break;
        }
        for (uint32_t offset : requiredVirtuals) {
            if (!IsExecutableAddress(*(uint64_t*)(vtable + offset))) {
                validGeneratorChain = false;
                break;
            }
        }
        if (!validGeneratorChain) break;
        node = *(uint64_t*)(node + 0x18);
        if (count == 4095 && node != 0) validGeneratorChain = false;
    }
    if (!validGeneratorChain) {
        *(uint64_t*)((uint8_t*)effect + 0x218) = 0;
    }

    uint64_t subBuffers = *(uint64_t*)((uint8_t*)effect + 0x220);
    if (subBuffers != 0 && !IsAccessiblePointer(subBuffers)) {
        *(uint64_t*)((uint8_t*)effect + 0x220) = 0;
        *(uint16_t*)((uint8_t*)effect + 0x208) = 0;
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
    auto shotRecipe = g_liveShotRecipes.find(unit);
    if (shotRecipe != g_liveShotRecipes.end() &&
        shotRecipe->second.reconstructed) {
        // Natural category cleanup calls removeUnit immediately before the
        // concrete shot destructor. Gameplay can repopulate an inline particle
        // node after reconstruction, so detach it at this final ownership
        // boundary as well as during rollback's explicit retirement.
        DetachEmbeddedShotEffect(unit);
    }
    if (IsEffectObject(unit)) {
        SanitizeEffectForDestruction(unit);
    }

    char result = oUnitRemove(manager, unit);
    if (result && unit) {
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
    oShotUpdateHitboxes(shot);

    auto recipe = g_liveShotRecipes.find(shot);
    if (recipe == g_liveShotRecipes.end() || !recipe->second.reconstructed)
        return;

    ConcreteShotSnapshot_ReapplyCapsules(shot, recipe->second.concrete);
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

static void Rollback_AppendShotHitboxesJSON(std::string& json, void* shot)
{
    json += ",\"hitboxes\":[";
    bool firstHitbox = true;
    uint8_t* shotBytes = (uint8_t*)shot;
    using GetPrimitiveFn = uint64_t(__fastcall*)(uint64_t, uint32_t);
    auto getMetadata = (uint64_t(__fastcall*)(uint64_t, uint32_t))_addr(0x1405AA900);
    uint32_t hitboxIndex = 0;

    // uShot__forEachExtraSlot_dispatchCollision enumerates all four embedded
    // collision slots beginning at +0x2058. The slot's MtArray is at +0x18;
    // its count is at +0x20 and entries are obtained through vtable +0x88.
    for (uint32_t slotIndex = 0; slotIndex < 4; ++slotIndex) {
        uint64_t collisionSlot = *(uint64_t*)(shotBytes + 0x2058 + slotIndex * 8);
        if (!collisionSlot || !IsAccessiblePointer(collisionSlot)) continue;

        uint64_t vtable = *(uint64_t*)collisionSlot;
        if (!vtable || !IsAccessiblePointer(vtable) ||
            !IsAccessibleAddress(vtable + 0x88)) continue;
        GetPrimitiveFn getPrimitive = *(GetPrimitiveFn*)(vtable + 0x88);
        if (!getPrimitive || !IsAccessibleAddress((uint64_t)getPrimitive)) continue;

        uint32_t count = *(uint32_t*)(collisionSlot + 0x20);
        if (count > 128) count = 128;
        for (uint32_t index = 0; index < count; ++index) {
            uint64_t primitive = getPrimitive(collisionSlot, index);
            uint64_t metadata = getMetadata(collisionSlot, index);
            if (!primitive || !metadata || !IsAccessiblePointer(primitive) ||
                !IsAccessiblePointer(metadata)) {
                continue;
            }

            uint32_t type = *(uint32_t*)(metadata + 0x14);
            if (type != 0 && type != 1) continue;

            float x = *(float*)(primitive + 0x20);
            float y = *(float*)(primitive + 0x24);
            float z = *(float*)(primitive + 0x28);
            float radius = *(float*)(primitive + (type == 0 ? 0x2C : 0x40));
            float x2 = 0.0f;
            float y2 = 0.0f;
            float z2 = 0.0f;
            if (type == 1) {
                x2 = *(float*)(primitive + 0x30);
                y2 = *(float*)(primitive + 0x34);
                z2 = *(float*)(primitive + 0x38);
            }
            if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z) ||
                !std::isfinite(x2) || !std::isfinite(y2) || !std::isfinite(z2) ||
                !std::isfinite(radius) || radius <= 0.0f || radius > 1000.0f) {
                continue;
            }

            char hitbox[512];
            if (type == 1) {
                snprintf(hitbox, sizeof(hitbox),
                         "%s{\"bank\":\"shot\",\"index\":%u,\"slot\":%u,\"shape\":\"capsule\","
                         "\"pos\":[%.9g,%.9g,%.9g],\"endPos\":[%.9g,%.9g,%.9g],"
                         "\"size\":[%.9g,%.9g,%.9g],\"radius\":%.9g}",
                         firstHitbox ? "" : ",", hitboxIndex++, slotIndex,
                         x, y, z, x2, y2, z2,
                         radius * 2.0f, radius * 2.0f, radius * 2.0f, radius);
            } else {
                snprintf(hitbox, sizeof(hitbox),
                         "%s{\"bank\":\"shot\",\"index\":%u,\"slot\":%u,\"shape\":\"sphere\","
                         "\"pos\":[%.9g,%.9g,%.9g],\"size\":[%.9g,%.9g,%.9g],"
                         "\"radius\":%.9g}",
                         firstHitbox ? "" : ",", hitboxIndex++, slotIndex,
                         x, y, z, radius * 2.0f, radius * 2.0f, radius * 2.0f, radius);
            }
            json += hitbox;
            firstHitbox = false;
        }
    }
    json += "]";
}

static void Rollback_AppendShotCollisionOriginJSON(std::string& json, void* shot)
{
    uint64_t shotAddress = (uint64_t)shot;
    uint64_t vtable = shotAddress ? *(uint64_t*)shotAddress : 0;
    if (!vtable || !IsAccessiblePointer(vtable) || !IsAccessibleAddress(vtable + 0xA0)) return;

    using GetMatrixFn = uint64_t(__fastcall*)(uint64_t, int32_t);
    GetMatrixFn getMatrix = *(GetMatrixFn*)(vtable + 0xA0);
    if (!getMatrix || !IsAccessibleAddress((uint64_t)getMatrix)) return;
    uint64_t matrix = getMatrix(shotAddress, -1);
    if (!matrix || !IsAccessiblePointer(matrix)) return;

    float values[16];
    for (int i = 0; i < 16; ++i) {
        values[i] = *(float*)(matrix + 0x20 + i * sizeof(float));
        if (!std::isfinite(values[i])) return;
    }
    char origin[640];
    snprintf(origin, sizeof(origin),
             ",\"collisionMatrix\":["
             "%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,"
             "%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g]",
             values[0], values[1], values[2], values[3],
             values[4], values[5], values[6], values[7],
             values[8], values[9], values[10], values[11],
             values[12], values[13], values[14], values[15]);
    json += origin;
}

static std::string Rollback_GenerateShotJSON()
{
    std::string out = "[";
    bool first = true;
    char entry[1024];

    for (int list = 0; list < SSHOT_SENTINEL_COUNT; ++list) {
        uint64_t sentinel = (uint64_t)_addr(0x140d47f90) + list * 0x28;
        uint64_t sentinelRef = sentinel + 0x08;
        uint64_t nodeRef = *(uint64_t*)(sentinel + 0x10);
        int safety = DYNAMIC_SAFETY;
        while (nodeRef && nodeRef != sentinelRef && safety-- > 0) {
            void* shot = (void*)(nodeRef - SSHOT_NODE_OFFSET);
            uint8_t* shotBytes = (uint8_t*)shot;
            auto recipe = g_liveShotRecipes.find(shot);

            uint32_t slotId = 0;
            uint64_t initParam = 0;
            uint64_t ownerChr = 0;
            uint64_t shotElement = 0;
            const char* kind = "unknown";
            if (recipe != g_liveShotRecipes.end()) {
                slotId = recipe->second.slotId;
                initParam = recipe->second.initParam;
                ownerChr = (uint64_t)recipe->second.ownerChr;
                shotElement = (uint64_t)recipe->second.shotElement;
                kind = recipe->second.kind == ShotSpawnKind::Extended ? "extended" : "standard";
            }
            
            snprintf(entry, sizeof(entry),
                     "%s{\"list\":%d,\"addr\":\"0x%llX\","
                     "\"slot\":%u,\"init\":%llu,\"kind\":\"%s\","
                     "\"owner\":\"0x%llX\",\"element\":\"0x%llX\","
                     "\"state\":\"0x%08X\","
                     "\"pos\":[%.9g,%.9g,%.9g],"
                     "\"runtimePos\":[%.9g,%.9g,%.9g],"
                     "\"lifeFrame\":%.9g,"
                     "\"velocity\":[%.9g,%.9g,%.9g],"
                     "\"speed\":%.9g,\"step\":%.9g,"
                     "\"timer\":%.9g",
                     first ? "" : ",", list, (uint64_t)shot,
                      slotId, (unsigned long long)initParam, kind, ownerChr, shotElement,
                      *(uint32_t*)(shotBytes + 0x10),
                      *(float*)(shotBytes + 0x50),
                      *(float*)(shotBytes + 0x54),
                      *(float*)(shotBytes + 0x58),
                      *(float*)(shotBytes + 0x418),
                      *(float*)(shotBytes + 0x41C),
                      *(float*)(shotBytes + 0x420),
                      *(float*)(shotBytes + 0x2078),
                      *(float*)(shotBytes + 0x20C0),
                      *(float*)(shotBytes + 0x20C4),
                      *(float*)(shotBytes + 0x20C8),
                      *(float*)(shotBytes + 0x20D0),
                      *(float*)(shotBytes + 0x212C),
                      *(float*)(shotBytes + 0x1F60));
               std::string shotEntry(entry);
               Rollback_AppendShotCollisionOriginJSON(shotEntry, shot);
               Rollback_AppendShotHitboxesJSON(shotEntry, shot);
             shotEntry += "}";
             out += shotEntry;
            first = false;
            nodeRef = *(uint64_t*)(nodeRef + SSHOT_NODE_NEXT);
        }
    }

    out += "]";
    return out;
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

    int removedInvalidUnits = SanitizeUnitCategoryLists(sUnit);
    if (removedInvalidUnits != 0) {
        HookLog("Rollback: removed %d invalid sUnit category entries before restore\n",
                removedInvalidUnits);
    }

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
            void* next = *(void**)((uint8_t*)effect + 0x18);
            uint32_t* state = (uint32_t*)((uint8_t*)effect + 0x10);
            if (IsEffectObject(effect) && (*state & 7u) < 3u) {
                *state = (*state & ~7u) | 3u;
                ++retiredEffects;
            }
            effect = next;
        }
    }

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

    // Concrete shot destructors can release attached uGameEffect objects
    // directly. Those effects may still be present in sUnit's primary and
    // multi-mask lists with a zeroed vtable, causing the next parallel move
    // job to dispatch through null at vtable+0x40. Remove such dead entries
    // after all shot-owned teardown has completed and before reconstruction.
    int removedShotOwnedEffects = SanitizeUnitCategoryLists(sUnit);
    if (removedShotOwnedEffects != 0) {
        HookLog("Rollback: removed %d dead sUnit entries after shot destruction\n",
                removedShotOwnedEffects);
    }
    for (uint32_t category = 13; category <= 16 && category < categoryCount; ++category) {
        void* effect = *(void**)((uint8_t*)sUnit + SUNIT_CATEGORY_LISTS_OFFSET +
                                 SUNIT_CATEGORY_LIST_STRIDE * category);
        int safety = 4096;
        while (effect && safety-- > 0) {
            void* next = *(void**)((uint8_t*)effect + 0x18);
            if (IsEffectObject(effect))
                SanitizeEffectForDestruction(effect);
            effect = next;
        }
    }

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

    for (ShotRecipe& saved : slot.shotRecipes) {
        RestorePreSpawnRng(saved);
        void* objectRef1 = ValidateShotObjectReference(
            saved.objectRef1, saved.objectRef1Dti,
            saved.objectRef1WasActiveUnit);
        void* objectRef2 = ValidateShotObjectReference(
            saved.objectRef2, saved.objectRef2Dti,
            saved.objectRef2WasActiveUnit);
        void* fresh = saved.kind == ShotSpawnKind::Extended
            ? oShotSpawnEx(sShot, saved.shotElement, saved.initParam, saved.posData,
                           saved.ownerChr, objectRef1, saved.slotId,
                           objectRef2, saved.exA9, saved.exA10,
                           saved.exA11, saved.exA12)
            : oShotSpawn(sShot, saved.shotElement, saved.initParam, saved.posData,
                         saved.ownerChr, objectRef1, saved.slotId, objectRef2);
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
        ReleaseFreshEmbeddedShotEffectGraph(fresh);
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
        RestoreKingKobunHelper(fresh, saved);
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
        respawnedShots++;
    }

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

    // ---- 4. aNextBattle — battle state machine (static manager) ----
    {
        uint64_t aNextBattle = *(uint64_t*)_addr(PTR_ANEXTBATTLE);
        add((void*)aNextBattle, 0x20000);
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

// ===========================================================================
// Sleep-Patch & Headless Tick — game-logic-only re-simulation stepping
// ===========================================================================
static constexpr uintptr_t FN_SMAIN_MOVE            = 0x140521df0;
static constexpr uintptr_t FN_UPDATE_MANAGERS        = 0x140258540;
static constexpr uintptr_t FN_PAD_PROCESS            = 0x140289c30;
static constexpr uintptr_t FN_MVC3MGR_GETINSTANCE    = 0x140001af0;
static constexpr uintptr_t FN_MVC3MGR_GETACTION      = 0x1409629d0;
static constexpr uintptr_t FN_AFTER_GETACTION        = 0x14014f410;
static constexpr uintptr_t FN_COLLISION_BEGIN_FRAME  = 0x1405bb710;  // sCollision__beginFrameAndDrainDeferredState
static constexpr uintptr_t FN_CALL_AI                = 0x140647910;  // direct call on sMain+0x40240 (AI)
static constexpr uintptr_t FN_UPDATE_RENDER_STATE    = 0x140259ea0;  // sMvc3Main__updateRenderState
static constexpr uintptr_t FN_RENDER_END_FRAME       = 0x14053a250;  // sRender__endFrameAndPresent
static constexpr uintptr_t FN_SSOUND_MOVE             = 0x1405CC040;
static constexpr uintptr_t FN_SGAMEUI_MOVE            = 0x1402BE610;
static constexpr uintptr_t FN_SCHEDULER_REBUILD       = 0x14050F010;
static constexpr uintptr_t FN_APPLY_COLLISION_PUSH   = 0x14004ad20;  // fighter pushbox response callback
static constexpr uintptr_t FN_RESET_HITNODES         = 0x140259e90;  // j_sHitSolver__resetActiveNodes
static constexpr uintptr_t FN_PAD_METHOD_3D00        = 0x1402a3d00;  // sPad method
static constexpr uintptr_t FN_PAD_METHOD_3DC0        = 0x1402a3dc0;  // sPad method

// sMvc3Manager subsystem getters (for MgrSubsysV30 helper)
static constexpr uintptr_t FN_GETGAMECONFIG          = 0x1401bcc50;
static constexpr uintptr_t FN_GETGAMEUI              = 0x14097d370;
static constexpr uintptr_t FN_GETSHOT                = 0x140683730;
static constexpr uintptr_t FN_GETGAMEEFFECT          = 0x140015690;
static constexpr uintptr_t FN_GETHITSOLVER           = 0x140056130;
static constexpr uintptr_t FN_GETCHARACTER           = 0x1407bcd80;
static constexpr uintptr_t FN_GETSTAGE               = 0x140259460;
static constexpr uintptr_t FN_GETREPLAY              = 0x14013c430;
static constexpr uintptr_t FN_GETBATTLESET           = 0x1408f1f10;
static constexpr uintptr_t FN_GETMVC3SET             = 0x14004b000;
static constexpr uintptr_t FN_GETNETWORK             = 0x1407cda50;
static constexpr uintptr_t FN_GETLICENSE             = 0x14010a200;
static constexpr uintptr_t FN_GETRANKING             = 0x140703370;
static constexpr uintptr_t FN_GETNETROOM             = 0x14030e6c0;
static constexpr uintptr_t FN_GETEARTHATK            = 0x140327a10;

// Sleep-patch state: the two Sleep() call sites inside sMain__move
// (0x140521df0).  Each is a 6-byte "CALL [RIP+xxx]" through the IAT.
static constexpr uintptr_t SLEEP_CALL_FIXED = 0x1405220b2;  // Sleep(dt-1)
static constexpr uintptr_t SLEEP_CALL_VAR   = 0x140521f93;  // Sleep(1)
static constexpr uintptr_t PRESENT_CALL_SITE = 0x14053709c; // CALL [RAX+0x48] inside sRender__beginFrame
static constexpr uintptr_t SWAPCHAIN_CALL_A  = 0x1402598c4; // vtable+0x50 on sMain+0x40208
static constexpr uintptr_t SWAPCHAIN_CALL_B  = 0x1402598d1; // vtable+0x50 on sMain+0x40270
static constexpr uintptr_t SWAPCHAIN_CALL_C  = 0x140537113; // vtable+0x50 in sRender__beginFrame
static uint8_t g_savedSleep[2][6] = {};
static uint8_t g_savedPresent[3] = {};
static uint8_t g_savedSwapchain[3][3] = {};
static bool    g_sleepPatched = false;
static bool    g_presentPatched = false;
static bool    g_swapchainPatched = false;

// Helper: call a vtable method on an object pointer stored at |offset| in
// sMvc3Main.  vtable_offset is the byte offset into the vtable (e.g. 0x30).
static inline void VTableCall(uint64_t sMain, uint32_t memberOffset, uint32_t vtableOffset)
{
    uint64_t obj = *(uint64_t*)(sMain + memberOffset);
    if (!obj) return;
    uint64_t vtable = *(uint64_t*)obj;
    if (!vtable) return;
    auto fn = *(void(__fastcall**)(uint64_t))(vtable + vtableOffset);
    if (fn) fn(obj);
}

// Helper: call a non-vtable direct function with an object from sMain member.
static inline void DirectMemberCall(uint64_t sMain, uint32_t memberOffset, uintptr_t funcAddr)
{
    uint64_t obj = *(uint64_t*)(sMain + memberOffset);
    if (!obj) return;
    ((void(__fastcall*)(uint64_t))_addr(funcAddr))(obj);
}

// Helper: sMvc3Manager::getInstance() → getter(instance) → vtable(vfnOffset) on result.
static inline void MgrSubsysCall(uintptr_t getterAddr, uint32_t vfnOffset)
{
    uint64_t inst = ((uint64_t(__fastcall*)())_addr(FN_MVC3MGR_GETINSTANCE))();
    if (!inst) return;
    uint64_t obj = ((uint64_t(__fastcall*)(uint64_t))_addr(getterAddr))(inst);
    if (!obj) return;
    uint64_t vt = *(uint64_t*)obj;
    if (!vt) return;
    auto fn = *(void(__fastcall**)(uint64_t))(vt + vfnOffset);
    if (fn) fn(obj);
}

// Helper: Aligns restored sMain structures with actual performance counters
// to prevent the game loop from computing a negative/excessive frame-time
// delta after re-simulation, which would cause a freeze in the busy-wait
// or in the Sleep-based frame-pacing loop.
static void Rollback_ResyncMainTiming(uint64_t sMain)
{
    if (!sMain) return;

    LARGE_INTEGER qpc;
    QueryPerformanceCounter(&qpc);

    // Sync high-resolution performance-counter samples with physical counter.
    // sMain+0x40 is the "base QPC" sampled at the PREVIOUS frame boundary;
    // sMain__move computes the frame delta as (current_QPC - base) * 1000 / freq.
    // Without a valid base the first post-re-sim delta will be enormous and
    // the frame-pacer and/or tick-count multiplier will produce wrong results.
    *(uint64_t*)(sMain + 0x40)     = qpc.QuadPart;  // base QPC for delta
    *(uint64_t*)(sMain + 0x40048)  = qpc.QuadPart;
    *(uint64_t*)(sMain + 0x40078)  = qpc.QuadPart;
    *(uint64_t*)(sMain + 0x70)     = qpc.QuadPart;

    // Reset timing deltas to 60 FPS nominal values so the next real tick
    // sees a well-formed 16 ms frame delta.
    *(uint32_t*)(sMain + 0x38)     = 16;           // ~16ms frame delta
    // mDeltaTime (+0x80) is in FRAME units (~1.0), not seconds — sMain__move
    // sets it to 1.0f in the vsync path and multiplies by globalSpeed (+0x8c).
    // The dash double-tap timers integrate this value per frame.
    *(float*)(sMain + 0x80)        = 1.0f;         // 1 frame of game time
    *(uint32_t*)(sMain + 0xA8)     = 16;           // current delta
    *(uint32_t*)(sMain + 0xA4)     = 16;           // shifted delta

    // Zero accumulated sleep/deadline calculations so they recompute fresh.
    // These are the "accumulated time" (sMain+0x60) and "deadline" (sMain+0x68)
    // used by the frame-pacing busy-wait logic.  If left stale the game may
    // spin forever waiting for sMain+0x60 < sMain+0x68 to become false.
    *(uint64_t*)(sMain + 0x60)     = 0;
    *(uint64_t*)(sMain + 0x68)     = 0;

    // Set tick count so the next real frame advances by exactly 1 step.
    // (Without this the restored frame count may jump or stall.)
    *(uint32_t*)(sMain + 0x48)     = 1;
}

void Rollback_PatchSleep(bool enable)
{
    if (enable && !g_sleepPatched) {
        DWORD old = 0;
        static const uintptr_t sites[2] = { SLEEP_CALL_FIXED, SLEEP_CALL_VAR };
        for (int i = 0; i < 2; ++i) {
            VirtualProtect((LPVOID)_addr(sites[i]), 6, PAGE_EXECUTE_READWRITE, &old);
            memcpy(g_savedSleep[i], (void*)_addr(sites[i]), 6);
            memset((void*)_addr(sites[i]), 0x90, 6);
            VirtualProtect((LPVOID)_addr(sites[i]), 6, old, &old);
        }
        g_sleepPatched = true;
    } else if (!enable && g_sleepPatched) {
        DWORD old = 0;
        static const uintptr_t sites[2] = { SLEEP_CALL_FIXED, SLEEP_CALL_VAR };
        for (int i = 0; i < 2; ++i) {
            VirtualProtect((LPVOID)_addr(sites[i]), 6, PAGE_EXECUTE_READWRITE, &old);
            memcpy((void*)_addr(sites[i]), g_savedSleep[i], 6);
            VirtualProtect((LPVOID)_addr(sites[i]), 6, old, &old);
        }
        g_sleepPatched = false;
    }
}

void Rollback_PatchPresent(bool enable)
{
    // NOP / restore the 3-byte CALL [RAX+0x48] inside sRender__beginFrame.
    // This Present call blocks for ~16ms per frame under DXVK; NOPping it
    // during re-sim lets the rest of the render-sync function run normally
    // (setting render-done flags, advancing frame counter, etc.) without
    // the blocking Present.  Required because the game crashes if we skip
    // sRender__beginFrame entirely (downstream game logic depends on its state).
    if (enable && !g_presentPatched) {
        DWORD old = 0;
        VirtualProtect((LPVOID)_addr(PRESENT_CALL_SITE), 3,
                       PAGE_EXECUTE_READWRITE, &old);
        memcpy(g_savedPresent, (void*)_addr(PRESENT_CALL_SITE), 3);
        memset((void*)_addr(PRESENT_CALL_SITE), 0x90, 3);
        VirtualProtect((LPVOID)_addr(PRESENT_CALL_SITE), 3, old, &old);
        g_presentPatched = true;
    } else if (!enable && g_presentPatched) {
        DWORD old = 0;
        VirtualProtect((LPVOID)_addr(PRESENT_CALL_SITE), 3,
                       PAGE_EXECUTE_READWRITE, &old);
        memcpy((void*)_addr(PRESENT_CALL_SITE), g_savedPresent, 3);
        VirtualProtect((LPVOID)_addr(PRESENT_CALL_SITE), 3, old, &old);
        g_presentPatched = false;
    }
}

void Rollback_PatchSwapchain(bool enable)
{
    // NOP / restore three vtable+0x50 call sites that are part of the
    // swapchain/end-frame path in sMvc3Main__move and sRender__beginFrame.
    // Each instruction is "CALL [RAX+0x50]" = FF 50 50 (3 bytes).
    //
    // These swapchain operations expect a Present to have been submitted;
    // with the Present NOP'd during re-sim they may do unnecessary work or
    // block.  NOPping them eliminates any remaining D3D9/Vulkan overhead
    // from the re-sim path.
    static const uintptr_t sites[3] = {
        SWAPCHAIN_CALL_A,
        SWAPCHAIN_CALL_B,
        SWAPCHAIN_CALL_C,
    };
    if (enable && !g_swapchainPatched) {
        for (int i = 0; i < 3; ++i) {
            DWORD old = 0;
            VirtualProtect((LPVOID)_addr(sites[i]), 3,
                           PAGE_EXECUTE_READWRITE, &old);
            memcpy(g_savedSwapchain[i], (void*)_addr(sites[i]), 3);
            memset((void*)_addr(sites[i]), 0x90, 3);
            VirtualProtect((LPVOID)_addr(sites[i]), 3, old, &old);
        }
        g_swapchainPatched = true;
    } else if (!enable && g_swapchainPatched) {
        for (int i = 0; i < 3; ++i) {
            DWORD old = 0;
            VirtualProtect((LPVOID)_addr(sites[i]), 3,
                           PAGE_EXECUTE_READWRITE, &old);
            memcpy((void*)_addr(sites[i]), g_savedSwapchain[i], 3);
            VirtualProtect((LPVOID)_addr(sites[i]), 3, old, &old);
        }
        g_swapchainPatched = false;
    }
}

// ===========================================================================
// Rollback_PatchDrawCalls — NOP all remaining render/draw call sites
//
// Every render-related CALL inside sMvc3Main__move that is still running
// during re-sim.  Comment out individual entries in the `sites` array
// below to test whether skipping that call site crashes the game.
//
// Sites organised by category:
//   A–G:  vtable+0x48 indirect draw calls (3 bytes each)
//   H–K:  Direct render-function CALLs (5 bytes each)
//   L:    vtable+0x30 render-logic update (3 bytes)
// ===========================================================================
void Rollback_PatchDrawCalls(bool enable)
{
    static struct { uintptr_t addr; int n; const char* label; } const sites[] = {
        // --- vtable+0x48 DRAW calls (FF 50 48 / FF 52 48) ---
        { 0x1402594d9, 3, "draw_401a8"       },
        { 0x1402594e6, 3, "draw_40198"       },
        { 0x1402594f3, 3, "draw_401d8"       },
        { 0x140259515, 3, "draw_gameConfig"  },
        { 0x140259568, 3, "draw_40270"       },
        { 0x140259575, 3, "draw_40208"       },
        { 0x140259896, 3, "draw_401e0"       },
        // --- Direct render-function CALLs (E8 rel32 = 5 bytes) ---
        { 0x140259525, 5, "setRenderTargetAspect" },
        { 0x140259537, 5, "util_C7D0"             },
        { 0x140259548, 5, "setField_40"            },
        { 0x140259559, 5, "getter_7110"            },
        // --- vtable+0x30 render-logic update ---
        { 0x1402598ab, 3, "sRender_vtable+0x30"   },
    };
    static constexpr int N = sizeof(sites) / sizeof(sites[0]);

    // Saved bytes storage — indexed identically to sites[]
    static uint8_t saved[N][6] = {};   // max 6 bytes per site
    static bool    patched = false;

    if (enable && !patched) {
        for (int i = 0; i < N; ++i) {
            DWORD old = 0;
            VirtualProtect((LPVOID)_addr(sites[i].addr), sites[i].n,
                           PAGE_EXECUTE_READWRITE, &old);
            memcpy(saved[i], (void*)_addr(sites[i].addr), sites[i].n);
            memset((void*)_addr(sites[i].addr), 0x90, sites[i].n);
            VirtualProtect((LPVOID)_addr(sites[i].addr), sites[i].n, old, &old);
            //HookLog("Rollback: NOP'd draw call %s @ 0x%llX (%d bytes)\n",
            //        sites[i].label, (uint64_t)_addr(sites[i].addr), sites[i].n);
        }
        patched = true;
    } else if (!enable && patched) {
        for (int i = 0; i < N; ++i) {
            DWORD old = 0;
            VirtualProtect((LPVOID)_addr(sites[i].addr), sites[i].n,
                           PAGE_EXECUTE_READWRITE, &old);
            memcpy((void*)_addr(sites[i].addr), saved[i], sites[i].n);
            VirtualProtect((LPVOID)_addr(sites[i].addr), sites[i].n, old, &old);
        }
        patched = false;
    } else if (!enable && !patched) {
        return; // nothing to do
    } else {
        //HookLog("Rollback: draw calls already patched, skipping\n");
    }
}

//THIS IS NOT USED
void Rollback_HeadlessTick(uint64_t sMain)
{
//THIS IS NOT USED
return;
    if (!sMain) 
    {
        HookLog("Rollbackheadlesstick: smain is bad?:? :>\n");
        return;
    }

    HookLog("Rollback_HeadlessTick: begin\n");

    // ---- Disable collision sound-event updates during re-sim ----
    // sCollision__move checks the flag at collisionObj+0x73a8 before
    // calling sCollision__cSbcMoveMatrix__dispatchPostMoveJobs. During re-sim the
    // sound slot-index increments past the per-entry capacity, hitting a
    // null-pointer bug in the iterator.  Sound doesn't matter for headless
    // re-simulation; zeroing the flag safely skips the crashing call.
    //
    // (We also hook the post-move dispatcher via MinHook below as a redundant
    //  guard — the flag can be re-enabled on worker threads.)

    // ==================================================================
    // Phase 1 — Frame timing + Manager state + Input (always needed)
    // ==================================================================
    // 1. Frame timing — SKIP sMain__move during re-simulation.
    //    sMain__move (0x140521df0) reads real QPC, computes frame deltas,
    //    runs Sleep()/busy-wait for frame pacing, and updates sMain timing
    //    fields (0x38, 0x48, 0x60, 0x68, 0x80, etc.).  Calling it during
    //    re-sim corrupts those fields (it samples real QPC across the re-sim
    //    window) and the first post-re-sim real frame computes a stale delta
    //    that confuses the frame-pacer — causing a permanent hang.
    //
    //    Instead we just set the tick count (sMain+0x48) to 1 so any game
    //    logic that checks it sees a valid advance.  The real sMain__move
    //    runs on the next natural frame boundary after re-sim ends.
    HookLog("Rollback_HeadlessTick: Phase1 step1 — Frame timing (skipping sMain__move)\n");
    *(uint32_t*)(sMain + 0x48) = 1;
    // 2. Window-focus/manager state
    HookLog("Rollback_HeadlessTick: Phase1 step2 — Window-focus/manager state\n");
    ((void(__fastcall*)(uint64_t))_addr(FN_UPDATE_MANAGERS))(sMain);
    // 3. Input processing (CRITICAL — gathers pad / applies historical inputs)
    HookLog("Rollback_HeadlessTick: Phase1 step3 — Input processing\n");
    uint64_t syncPad = *(uint64_t*)(sMain + 0x401c0);
    if (syncPad)
        ((void(__fastcall*)(uint64_t))_addr(FN_PAD_PROCESS))(syncPad);
    // 4. Async loader update (vtable+0x30)
    HookLog("Rollback_HeadlessTick: Phase1 step4 — Async loader update\n");
    VTableCall(sMain, 0x40260, 0x30);

    // ==================================================================
    // [SKIP] vtable+0x48 draw calls on [0x401a8], [0x40198], [0x401d8]
    // [SKIP] sCollision__beginFrameAndDrainDeferredState on [0x401e0]
    // [SKIP] sRender/sPrimitive render-setup block
    // [SKIP] vtable+0x48 on [0x40270], [0x40208]
    // [SKIP] sRender__beginFrame (Present path) - would crash through kiero hooks
    // ==================================================================

    // ==================================================================
    // Phase 2 — Post-render game-logic updates (mirrors original order)
    // ==================================================================
    HookLog("Rollback_HeadlessTick: Phase2 begin — Post-render game-logic updates\n");
    // 5. sMvc3Area update (vtable+0x30)
    VTableCall(sMain, 0x40200, 0x30);

    // 6. sMvc3Manager singleton chain (getAction + afterAction)
    HookLog("Rollback_HeadlessTick: Phase2 step6 — sMvc3Manager singleton chain\n");
    {
        uint64_t mgr = ((uint64_t(__fastcall*)())_addr(FN_MVC3MGR_GETINSTANCE))();
        if (mgr) {
            uint64_t action = ((uint64_t(__fastcall*)(uint64_t))_addr(FN_MVC3MGR_GETACTION))(mgr);
            if (action)
                ((void(__fastcall*)(uint64_t))_addr(FN_AFTER_GETACTION))(action);
        }
    }

    // 7. Post-render vtable+0x30 updates (game-logic state)
    HookLog("Rollback_HeadlessTick: Phase2 step7 — Post-render vtable+0x30 updates\n");
    VTableCall(sMain, 0x40230, 0x30);  // sMvc3Vibration
    VTableCall(sMain, 0x401c0, 0x30);  // sMvc3SyncPad
    VTableCall(sMain, 0x40210, 0x30);  // sMouse
    VTableCall(sMain, 0x40228, 0x30);  // sId
    VTableCall(sMain, 0x40218, 0x30);  // sKeyboard
    VTableCall(sMain, 0x401c0, 0x50);  // sMvc3SyncPad vfn+0x50
    VTableCall(sMain, 0x401e8, 0x30);  // sMvc3Shader2
    VTableCall(sMain, 0x40240, 0x30);  // sAI

    // ---- Manager subsystems (vtable+0x30 updates) ----
    HookLog("Rollback_HeadlessTick: Phase2 — Manager subsystems\n");
    MgrSubsysCall(FN_GETGAMECONFIG, 0x30);   // sGameConfig
    // sGameUI is presentation state and is intentionally excluded from the
    // snapshot. Updating it during re-simulation can load uCmbText resources
    // and rebuild uScheduler arrays whose allocator-owned storage is not
    // rewound, eventually reaching native setup with a failed allocation.
    // Leave UI animation/resource state on the live timeline.

    // [SKIP] getShot->vtable+0x48 (draw)
    // [SKIP] getGameEffect->vtable+0x48 (draw)

    VTableCall(sMain, 0x401a0, 0x30);  // collision (sCollision__move)
    VTableCall(sMain, 0x401e0, 0x30);  // mpCollision second call

    MgrSubsysCall(FN_GETHITSOLVER, 0x30);    // sHitSolver

    VTableCall(sMain, 0x401a0, 0x68);  // physics vfn+0x68
    VTableCall(sMain, 0x401a0, 0x58);  // physics vfn+0x58

    // 8. Direct AI call (separate from vtable+0x30 above)
    HookLog("Rollback_HeadlessTick: Phase2 step8 — Direct AI call\n");
    DirectMemberCall(sMain, 0x40240, FN_CALL_AI);

    // 9. Remaining vtable+0x30 member updates
    HookLog("Rollback_HeadlessTick: Phase2 step9 — Remaining vtable+0x30 updates\n");
    VTableCall(sMain, 0x401d8, 0x30);
    VTableCall(sMain, 0x40208, 0x30);
    VTableCall(sMain, 0x401b0, 0x30);
    VTableCall(sMain, 0x401f0, 0x30);
    VTableCall(sMain, 0x40248, 0x30);
    VTableCall(sMain, 0x40258, 0x30);

    // ==================================================================
    // Phase 3 — Manager subsystem update loop (all vtable+0x30)
    //
    // NOTE: The function at 0x140259270 was previously identified as a
    // manager subsystem getter, but Ghidra analysis revealed it is actually
    // Concurrency::details::VirtualProcessor::GetOwningRoot — a ConcRT
    // thread-pool internal. Calling it with vtable+0x30 corrupts the
    // dispatch table across all 16 worker threads, causing crashes at
    // address 0x100000000.  It is intentionally excluded below.
    // ==================================================================
    HookLog("Rollback_HeadlessTick: Phase3 begin — Manager subsystem update loop\n");
    // getInstance -> vtable+0x30 (manager itself)
    HookLog("Rollback_HeadlessTick: Phase3 — sMvc3Manager vtable+0x30\n");
    {
        uint64_t mgr = ((uint64_t(__fastcall*)())_addr(FN_MVC3MGR_GETINSTANCE))();
        if (mgr) {
            uint64_t vt = *(uint64_t*)mgr;
            if (vt) {
                auto fn = *(void(__fastcall**)(uint64_t))(vt + 0x30);
                if (fn) fn(mgr);
            }
        }
    }

    HookLog("Rollback_HeadlessTick: Phase3 — Character/Stage/Shot/GameEffect vtable+0x30\n");
    MgrSubsysCall(FN_GETCHARACTER, 0x30);    // sCharacter
    MgrSubsysCall(FN_GETSTAGE, 0x30);        // sStage
    MgrSubsysCall(FN_GETSHOT, 0x30);         // sShot (update, NOT draw)
    MgrSubsysCall(FN_GETGAMEEFFECT, 0x30);   // sGameEffect (update, NOT draw)
    // getHitSolver -> j_sHitSolver__resetActiveNodes (direct, not vtable)
    HookLog("Rollback_HeadlessTick: Phase3 — Hit solver reset\n");
    {
        uint64_t inst = ((uint64_t(__fastcall*)())_addr(FN_MVC3MGR_GETINSTANCE))();
        if (inst) {
            uint64_t hs = ((uint64_t(__fastcall*)(uint64_t))_addr(FN_GETHITSOLVER))(inst);
            if (hs)
                ((void(__fastcall*)(uint64_t))_addr(FN_RESET_HITNODES))(hs);
        }
    }

    HookLog("Rollback_HeadlessTick: Phase3 — Replay/Battle/Network/License vtable+0x30\n");
    MgrSubsysCall(FN_GETREPLAY, 0x30);       // sReplay
    // FN_MGR_GETSOMETHING (0x140259270) intentionally excluded — see note above.
    MgrSubsysCall(FN_GETBATTLESET, 0x30);    // sBattleSetting
    MgrSubsysCall(FN_GETMVC3SET, 0x30);      // sMvc3Setting
    MgrSubsysCall(FN_GETNETWORK, 0x30);      // sNetwork
    MgrSubsysCall(FN_GETLICENSE, 0x30);      // sLicense

    HookLog("Rollback_HeadlessTick: Phase3 — Remaining member vtable+0x30\n");
    VTableCall(sMain, 0x401b8, 0x30);
    VTableCall(sMain, 0x401c8, 0x30);
    VTableCall(sMain, 0x401d0, 0x30);

    HookLog("Rollback_HeadlessTick: Phase3 — Ranking/NetRoom/EarthAtk vtable+0x30\n");
    MgrSubsysCall(FN_GETRANKING, 0x30);      // sRanking
    MgrSubsysCall(FN_GETNETROOM, 0x30);      // sNetworkRoom
    MgrSubsysCall(FN_GETEARTHATK, 0x30);     // sEarthAttack

    // ==================================================================
    // Phase 4 — Tail + finalize
    // ==================================================================
    HookLog("Rollback_HeadlessTick: Phase4 begin — Tail + finalize\n");
    VTableCall(sMain, 0x40198, 0x30);
    VTableCall(sMain, 0x40270, 0x30);

    // [SKIP] vtable+0x48 on [0x401e0] (draw)

    // Tail function
    HookLog("Rollback_HeadlessTick: Phase4 — sMain tail\n");
    ((void(__fastcall*)(uint64_t))_addr(FN_UPDATE_RENDER_STATE))(sMain);

    // [SKIP] sRender game-logic state update (vtable+0x30) — handled inside
    //       FN_SMAIN_MOVE, and the internal allocator isn't snapshot-safe.
    // [SKIP] Direct sRender method — rendering is meaningless during headless re-sim.
    //[SKIP] vtable+0x50 on [0x40208] (render)
    //[SKIP] vtable+0x50 on [0x40270] (render)

    HookLog("Rollback_HeadlessTick: Phase4 — Final vtable+0x30\n");
    VTableCall(sMain, 0x401f8, 0x30);

    // ==================================================================
    // Phase 5 — Pad methods + exit check (SKIP exit during re-sim)
    // ==================================================================
    HookLog("Rollback_HeadlessTick: Phase5 begin — Pad methods\n");
    // sPad::method_3D00(0xa4) and sPad::method_3D00(0xa5) — input state flush
    {
        uint64_t pad = ((uint64_t(__fastcall*)())_addr(0x140001b00))();
        if (pad)
            ((uint8_t(__fastcall*)(uint64_t, uint32_t, uint8_t))_addr(FN_PAD_METHOD_3D00))(pad, 0xa4, 1);
    }
    HookLog("Rollback_HeadlessTick: Phase5 — Pad method 0xa5\n");
    {
        uint64_t pad = ((uint64_t(__fastcall*)())_addr(0x140001b00))();
        if (pad)
            ((uint8_t(__fastcall*)(uint64_t, uint32_t, uint8_t))_addr(FN_PAD_METHOD_3D00))(pad, 0xa5, 1);
    }
    // [SKIP] The ALT+F4 exit check (sMain__exit call) — we NEVER want to
    // exit during re-simulation. The original checks if F4+Enter was pressed
    // and calls sMain__exit. Skip this entirely.

    HookLog("Rollback_HeadlessTick: end\n");
}

// Forward declarations
static void Rollback_ClearStalePointers();
static void Rollback_ClearGameEffectSlots();
static void Rollback_SyncActiveCharacterUi();

// ===========================================================================
// Public API — Save / Restore / ReSimulate
// ===========================================================================

void Rollback_SaveSnapshot(int32_t frame)
{
    Region regions[MAX_REGIONS];
    int count = BuildRegions(regions, MAX_REGIONS);
    if (count == 0) return;

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

    // Native clones keep reference-managed geometry alive after attack-end
    // clears the live node's child array.
    CaptureCollisionNodeClones(slot);
    CaptureShotAndEffectRecipes(slot);

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
            // Throw root motion reads this transient sAction payload. Native
            // action processing can replace its allocation after capture, so
            // rewind the saved scalar data into the current allocation while
            // retaining the current allocator header and pointer identity.
            std::memcpy((uint8_t*)liveModelJointAllocation +
                            SACTION_MODEL_JOINT_DATA_OFFSET,
                        slot.data + offset + SACTION_MODEL_JOINT_DATA_OFFSET,
                        copySize - SACTION_MODEL_JOINT_DATA_OFFSET);
        } else if (!ownerStillMatches[i] || !objectStillMatches) {
            // The external blend/animation allocation was replaced after the
            // snapshot, or a helper cChr was destroyed/reused. Never write old
            // bytes into a freed or repurposed allocation.
        } else if (slot.regions[i].ownerSlot != 0 &&
                   slot.regions[i].size == CCHR_EXTERNAL_ANIM_SIZE) {
            RestoreExternalAnimBlock(slot.regions[i].ptr,
                                     slot.data + offset,
                                     copySize);
        } else if ((uint64_t)slot.regions[i].ptr == sCollision &&
                   slot.regions[i].size == SCOLLISION_SIZE) {
            ++collisionRestoreCount;
            collisionRestoreBytes += copySize;
            RestoreSCollisionBody(slot.regions[i].ptr, slot.data + offset, copySize);
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

    // Full cChr body restore also rewinds its external pointer fields. If an
    // allocation changed, put the current pointer back after skipping its old
    // buffer so the live character does not retain a stale address.
    for (int i = 0; i < slot.regionCount; ++i) {
        if (slot.regions[i].ownerSlot != 0 && !ownerStillMatches[i]) {
            *(uint64_t*)slot.regions[i].ownerSlot = liveOwnerPointers[i];
        }
    }

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

    // Shot construction registers owned effects through sGameEffect and scans
    // its tracked-object slots with MtObject::isKindOf. Clear references to
    // effects retired by this restore before the first factory call.
    Rollback_ClearGameEffectSlots();
    RestoreShotAndEffectRecipes(slot);

    // Character slot tables contain live model/resource references. Clearing
    // all 64 entries makes restored dead characters invisible.
    Rollback_SyncActiveCharacterUi();

    HookLog("Rollback: restored frame %d with %d regions, total size %zu bytes, "
            "sCollision inline regions=%d bytes=%zu\n",
            frame, slot.regionCount, offset, collisionRestoreCount, collisionRestoreBytes);
}

// ===========================================================================
// Diag dumpers — capture full snapshot state + character text for resim debugging
// ===========================================================================

static uint64_t fnv1a_64(const void* data, size_t len) {
    uint64_t h = 0xCBF29CE484222325ULL;
    const uint8_t* p = (const uint8_t*)data;
    for (size_t i = 0; i < len; i++) {
        h ^= p[i];
        h *= 0x100000001B3ULL;
    }
    return h;
}

static void Rollback_DiagInit() {
    if (!g_ndjsonLoggingEnabled) return;
    if (g_diagDir[0]) return;
    SYSTEMTIME st;
    GetLocalTime(&st);
    char rel[80];

    // Append the local netplay role as a suffix to the directory name
    const char* sideSuffix = Netplay_IsActive() ? (Netplay_IsHost() ? "_host" : "_client") : "_offline";
    snprintf(rel, sizeof(rel), "rdiag_%04d%02d%02d_%02d%02d%02d%s",
             st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, sideSuffix);

    ResolveDllPath(g_diagDir, sizeof(g_diagDir), rel);
    CreateDirectoryA(g_diagDir, NULL);
    HookLog("Rollback: diag dir: %s\n", g_diagDir);
    char ndjsonPath[260];
    snprintf(ndjsonPath, sizeof(ndjsonPath), "%s/rdiag.ndjson", g_diagDir);
    g_diagNdjsonFile = fopen(ndjsonPath, "a");
    if (g_diagNdjsonFile)
        g_diagNdjsonFlush = GetTickCount();
}

// Binary: one file per session with [header + N entries].
// Header:  magic(4)="RBSD"  ver(u32)  sessionId(u32)  entryCount(u32)
// Entry:   frame(i32) side(u8) regionCount(u32) dataSize(u32)
//          regions[ regionSize(u32) regionHash(u64) regionData(bytes) ]
// v1 → v2: per-region FNV-1a hash added after size for region-level divergence detection.
static void Rollback_DumpBinary(int session, int32_t frame, uint8_t side) {
    if (!g_diagDir[0]) return;

    Region regions[MAX_REGIONS];
    int count = BuildRegions(regions, MAX_REGIONS);
    if (count == 0) return;

    char path[260];
    snprintf(path, sizeof(path), "%s/session_%d.bin", g_diagDir, session);

    bool isNew = false;
    FILE* f = fopen(path, "rb");
    if (!f) {
        f = fopen(path, "wb");
        if (!f) return;
        isNew = true;
        uint32_t magic = 0x44534252;
        uint32_t ver = 2;
        uint32_t sid = (uint32_t)session;
        uint32_t ec = 0;
        fwrite(&magic, 4, 1, f);
        fwrite(&ver, 4, 1, f);
        fwrite(&sid, 4, 1, f);
        fwrite(&ec, 4, 1, f);
    } else {
        fclose(f);
        f = fopen(path, "ab");
        if (!f) return;
    }

    g_diagWriteCount++;

    int32_t binFrame = side == 0 ? frame : frame + 1;
    fwrite(&binFrame, sizeof(binFrame), 1, f);
    fwrite(&side, 1, 1, f);
    uint32_t cc = (uint32_t)count;
    fwrite(&cc, sizeof(cc), 1, f);

    uint32_t totalSize = 0;
    for (int i = 0; i < count; i++)
        totalSize += (uint32_t)regions[i].size;
    fwrite(&totalSize, sizeof(totalSize), 1, f);

    for (int i = 0; i < count; i++) {
        uint32_t sz = (uint32_t)regions[i].size;
        uint64_t hash = fnv1a_64(regions[i].ptr, regions[i].size);
        fwrite(&sz, sizeof(sz), 1, f);
        fwrite(&hash, sizeof(hash), 1, f);
        fwrite(regions[i].ptr, 1, sz, f);
    }

    fclose(f);

    if (isNew) {
        uint32_t ec = (uint32_t)g_diagWriteCount;
        f = fopen(path, "r+b");
        if (f) {
            fseek(f, 12, SEEK_SET);
            fwrite(&ec, 4, 1, f);
            fclose(f);
        }
    }
}

// Text: character state per frame — appended to session text log
// RESIM dump labeling convention:
//   side=0 (LIVE): frame = startFrame of the resim window.  The actual captured
//     state is the current live frame (endFrame), NOT startFrame.  The label uses
//     startFrame because call-site changes broke the resim — the user must
//     mentally map: LIVE frame=N → state at the CURRENT live frame, not frame N.
//   side=1 (RESIM): frame = the frame that was just ticked.  The captured state
//     is at frame+1 (the start of the next frame).  We display frame+1 here so
//     the output frame number matches the actual state.
static void Rollback_DumpCharText(int session, int32_t frame, uint8_t side) {
    if (!g_diagDir[0]) return;

    char path[260];
    snprintf(path, sizeof(path), "%s/session_%d.txt", g_diagDir, session);

    FILE* f = fopen(path, "a");
    if (!f) return;

    int32_t displayFrame = side == 0 ? frame : frame + 1;
    fprintf(f, "=== %s frame=%d ===\n", side == 0 ? "LIVE" : "RESIM", displayFrame);
    if (side == 0) {
        fprintf(f, "# NOTE: LIVE frame=%d labels the resim window start, actual state is at current live frame\n", frame);
    }

    uint64_t sCharacter = *(uint64_t*)_addr(PTR_SCHARACTER);
    if (!sCharacter) { fprintf(f, "  (no sCharacter)\n\n"); fclose(f); return; }

    for (int ti = 0; ti < 2; ++ti) {
        uint64_t team = sCharacter + (ti == 0 ? TEAM0_OFFSET : TEAM1_OFFSET);
        uint64_t node = team + TEAM_FIRST_NODE_OFFSET;
        int ci = 0;
        while (node) {
            uint64_t chr = *(uint64_t*)(node + NODE_CHR_OFFSET);
            if (chr) {
                CharDiagSnapshot snap;
                CaptureCharSnapshot(chr, ti, ci, snap);

                uint64_t fcnt = *(uint64_t*)(snap.cchr + 0x18);
                uint32_t mHeld = *(uint32_t*)(snap.cchr + 0x14);
                uint32_t mRemap = *(uint32_t*)(snap.cchr + 0x560);
                uint32_t mMask  = *(uint32_t*)(snap.cchr + 0x568);
                uint32_t mProc  = *(uint32_t*)(snap.cchr + 0x56C);
                uint32_t mOld   = *(uint32_t*)(snap.cchr + 0x10);
                uint32_t h590   = *(uint32_t*)(snap.cchr + 0x590);
                uint32_t h594   = *(uint32_t*)(snap.cchr + 0x594);
                uint64_t heldR0 = *(uint64_t*)(snap.cchr + 0x28);
                uint32_t relHead= *(uint32_t*)(snap.cchr + 0x0A8);
                uint64_t relR0  = *(uint64_t*)(snap.cchr + 0x0AC);
                uint64_t prsR0  = *(uint64_t*)(snap.cchr + 0x130);
                uint32_t dir0   = *(uint32_t*)(snap.cchr + 0x1B0);
                uint32_t dir4   = *(uint32_t*)(snap.cchr + 0x1B4);
                uint32_t dirSlot= (uint32_t)(fcnt & 0x3F);
                uint64_t dirRing0  = *(uint64_t*)(snap.cchr + 0x360);
                uint64_t dirRingSlot=*(uint64_t*)(snap.cchr + 0x360 + dirSlot * 8);

                fprintf(f, "  Team%d Char%d @ 0x%llX\n", ti, ci, (uint64_t)chr);
                fprintf(f, "    pos=(%+.4f,%+.4f,%+.4f) act=0x%08X fl=0x%08X\n",
                        snap.posX, snap.posY, snap.posZ, snap.actionId, snap.flags14F0);
                fprintf(f, "    cci: mOld=0x%08X mHeld=0x%08X remap=0x%08X mask=0x%08X proc=0x%08X\n",
                        mOld, mHeld, mRemap, mMask, mProc);
                fprintf(f, "    cci: cntr=%llu h590=0x%08X h594=0x%08X\n", fcnt, h590, h594);
                fprintf(f, "    cci: heldR0=0x%016llX relHead=0x%08X relR0=0x%016llX prsR0=0x%016llX\n",
                        heldR0, relHead, relR0, prsR0);
                fprintf(f, "    cci: dir0=0x%08X dir4=0x%08X dirRing[0]=0x%016llX [%u]=0x%016llX\n",
                        dir0, dir4, dirRing0, dirSlot, dirRingSlot);
                fprintf(f, "    rng: 0x%08X 0x%08X 0x%08X 0x%08X\n",
                        snap.xorShift[0], snap.xorShift[1], snap.xorShift[2], snap.xorShift[3]);
                ci++;
            }
            node = *(uint64_t*)(node + NODE_NEXT_OFFSET);
        }
    }
    fprintf(f, "\n");
    fclose(f);
}

// Manifest: per-region FNV-1a hash summary appended to session text log.
// Quick visual scan shows which regions diverge between LIVE and RESIM.
static void Rollback_DumpManifest(int session, int32_t frame, uint8_t side) {
    if (!g_diagDir[0]) return;

    Region regions[MAX_REGIONS];
    int count = BuildRegions(regions, MAX_REGIONS);
    if (count == 0) return;

    char path[260];
    snprintf(path, sizeof(path), "%s/session_%d.txt", g_diagDir, session);

    FILE* f = fopen(path, "a");
    if (!f) return;

    int32_t mf = side == 0 ? frame : frame + 1;
    fprintf(f, "=== MANIFEST %s frame=%d regions=%d ===\n",
            side == 0 ? "LIVE" : "RESIM", mf, count);
    for (int i = 0; i < count; i++) {
        uint64_t hash = fnv1a_64(regions[i].ptr, regions[i].size);
        fprintf(f, "  [%3d] addr=0x%016llX size=%-8u hash=0x%016llX\n",
                i, (uint64_t)regions[i].ptr, (uint32_t)regions[i].size, hash);
    }
    fprintf(f, "\n");
    fclose(f);
}

// NDJSON: one JSON line per dump frame for script/AI consumption.
// All entries go into a single rdiag.ndjson file (no separate sessions).
// side=0 → "ground_truth" (pre-resim state), side=1 → "resim" (post-resim state)
// Schema:  v, ts, frame, side, regions[{idx,addr,size,hash}], chars[{...}], shots[{...}]
static void Rollback_DumpNDJSON(int32_t frame, uint8_t side) {
    if (!g_ndjsonLoggingEnabled) return;
    if (!g_diagDir[0]) return;

    Region regions[MAX_REGIONS];
    int count = BuildRegions(regions, MAX_REGIONS);
    if (count == 0) return;

    if (!g_diagNdjsonFile) return;

    // Build ISO timestamp
    char tsBuf[32] = {};
    SYSTEMTIME st;
    GetLocalTime(&st);
    snprintf(tsBuf, sizeof(tsBuf), "%04d-%02d-%02dT%02d:%02d:%02d",
             st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);

    int32_t nf = side == 0 ? frame : frame + 1;
    fprintf(g_diagNdjsonFile, "{\"v\":1,\"ts\":\"%s\",\"frame\":%d,\"side\":\"%s\",\"regions\":[",
            tsBuf, nf, side == 0 ? "ground_truth" : "resim");

    for (int i = 0; i < count; i++) {
        uint64_t rh = fnv1a_64(regions[i].ptr, regions[i].size);
        if (i > 0) fprintf(g_diagNdjsonFile, ",");
        fprintf(g_diagNdjsonFile, "{\"idx\":%d,\"addr\":\"0x%llX\",\"size\":%llu,\"hash\":\"0x%016llX\"}",
                i, (uint64_t)regions[i].ptr, (uint64_t)regions[i].size, rh);
    }

    fprintf(g_diagNdjsonFile, "],\"chars\":[");

    uint64_t sCharacter = *(uint64_t*)_addr(PTR_SCHARACTER);
    bool firstChar = true;
    if (sCharacter) {
        for (int ti = 0; ti < 2; ++ti) {
            uint64_t team = sCharacter + (ti == 0 ? TEAM0_OFFSET : TEAM1_OFFSET);
            uint64_t node = team + TEAM_FIRST_NODE_OFFSET;
            int ci = 0;
            while (node) {
                uint64_t chr = *(uint64_t*)(node + NODE_CHR_OFFSET);
                if (chr) {
                    CharDiagSnapshot snap;
                    CaptureCharSnapshot(chr, ti, ci, snap);
                    uint64_t fcnt = *(uint64_t*)(snap.cchr + 0x18);
                    uint32_t mHeld = *(uint32_t*)(snap.cchr + 0x14);
                    uint32_t mOld = *(uint32_t*)(snap.cchr + 0x10);
                    uint32_t mPressed = *(uint32_t*)(snap.cchr + 0x20);
                    uint32_t mReleased = *(uint32_t*)(snap.cchr + 0x24);
                    uint32_t mMask = *(uint32_t*)(snap.cchr + 0x568);
                    uint32_t mProcessed = *(uint32_t*)(snap.cchr + 0x56C);

                    if (!firstChar) fprintf(g_diagNdjsonFile, ",");
                    firstChar = false;
                    fprintf(g_diagNdjsonFile, "{\"team\":%d,\"slot\":%d,"
                            "\"pos\":[%.4f,%.4f,%.4f],"
                             "\"act\":\"0x%08X\",\"mChrF\":\"0x%08X\","
                             "\"mAnmF\":\"0x%08X\",\"mPrgF\":\"0x%08X\","
                            "\"mHeld\":\"0x%08X\",\"fcnt\":%llu,"
                            "\"mOld\":\"0x%08X\","
                            "\"mPressed\":\"0x%08X\",\"mReleased\":\"0x%08X\","
                            "\"mMask\":\"0x%08X\",\"mProcessed\":\"0x%08X\","
                            "\"rng\":[\"0x%08X\",\"0x%08X\",\"0x%08X\",\"0x%08X\"]}",
                            ti, ci,
                            snap.posX, snap.posY, snap.posZ,
                             snap.actionId, snap.flags14F0, snap.flags14F4, snap.flags14F8,
                            mHeld, fcnt,
                            mOld, mPressed, mReleased, mMask, mProcessed,
                            snap.xorShift[0], snap.xorShift[1],
                            snap.xorShift[2], snap.xorShift[3]);
                    ci++;
                }
                node = *(uint64_t*)(node + NODE_NEXT_OFFSET);
            }
        }
    }

    fprintf(g_diagNdjsonFile, "],\"shots\":[");

    // Dump the live projectile list separately from character state.  The
    // rollback snapshot already captures these fields, so keeping the same
    // representation here makes shot divergence easy to compare.
    bool firstShot = true;
    for (int list = 0; list < SSHOT_SENTINEL_COUNT; ++list) {
        uint64_t sentinel = (uint64_t)_addr(0x140d47f90) + list * 0x28;
        uint64_t sentinelRef = sentinel + 0x08;
        uint64_t nodeRef = *(uint64_t*)(sentinel + 0x10);
        int safety = DYNAMIC_SAFETY;
        while (nodeRef && nodeRef != sentinelRef && safety-- > 0) {
            void* shot = (void*)(nodeRef - SSHOT_NODE_OFFSET);
            uint8_t* shotBytes = (uint8_t*)shot;
            auto recipe = g_liveShotRecipes.find(shot);

            uint32_t slotId = 0;
            uint64_t initParam = 0;
            uint64_t ownerChr = 0;
            uint64_t shotElement = 0;
            if (recipe != g_liveShotRecipes.end()) {
                slotId = recipe->second.slotId;
                initParam = recipe->second.initParam;
                ownerChr = (uint64_t)recipe->second.ownerChr;
                shotElement = (uint64_t)recipe->second.shotElement;
            }

            if (!firstShot) fprintf(g_diagNdjsonFile, ",");
            firstShot = false;
            fprintf(g_diagNdjsonFile,
                    "{\"list\":%d,\"addr\":\"0x%llX\","
                    "\"slot\":%u,\"init\":%llu,"
                    "\"owner\":\"0x%llX\",\"element\":\"0x%llX\","
                    "\"state\":\"0x%08X\","
                    "\"pos\":[%.4f,%.4f,%.4f],"
                    "\"runtimePos\":[%.4f,%.4f,%.4f],"
                    "\"lifeFrame\":%.4f,"
                    "\"velocity\":[%.4f,%.4f,%.4f],"
                    "\"speed\":%.4f,\"step\":%.4f,"
                    "\"timer\":%.4f",
                    list, (uint64_t)shot, slotId, (unsigned long long)initParam,
                    ownerChr, shotElement,
                     *(uint32_t*)(shotBytes + 0x10),
                     *(float*)(shotBytes + 0x50),
                     *(float*)(shotBytes + 0x54),
                     *(float*)(shotBytes + 0x58),
                     *(float*)(shotBytes + 0x418),
                     *(float*)(shotBytes + 0x41C),
                     *(float*)(shotBytes + 0x420),
                     *(float*)(shotBytes + 0x2078),
                     *(float*)(shotBytes + 0x20C0),
                     *(float*)(shotBytes + 0x20C4),
                     *(float*)(shotBytes + 0x20C8),
                     *(float*)(shotBytes + 0x20D0),
                     *(float*)(shotBytes + 0x212C),
                     *(float*)(shotBytes + 0x1F60));
            std::string shotHitboxes;
            Rollback_AppendShotCollisionOriginJSON(shotHitboxes, shot);
            Rollback_AppendShotHitboxesJSON(shotHitboxes, shot);
            fputs(shotHitboxes.c_str(), g_diagNdjsonFile);
            fputs("}", g_diagNdjsonFile);

            nodeRef = *(uint64_t*)(nodeRef + SSHOT_NODE_NEXT);
        }
    }

    fprintf(g_diagNdjsonFile, "]}\n");
    DWORD now = GetTickCount();
    if (now - g_diagNdjsonFlush >= 1000) {
        fflush(g_diagNdjsonFile);
        g_diagNdjsonFlush = now;
    }
}

void Rollback_QueuePendingResimDump(int32_t frame)
{
    if (!g_ndjsonLoggingEnabled) return;
    if (!Netplay_IsActive())
    {
        return;
    }

    const FrameDiagSnapshot* snap = Rollback_GetResimSnapshot(frame);
    if (!snap) return;

    const CharDiagSnapshot* p1Post = nullptr;
    const CharDiagSnapshot* p2Post = nullptr;
    if (!Rollback_FindP1P2Post(*snap, &p1Post, &p2Post)) return;

    PendingResimDump dump;
    dump.frame = frame;
    Rollback_FillResimCharState(dump.p1Post, *p1Post);
    Rollback_FillResimCharState(dump.p2Post, *p2Post);

    uint32_t idx = (uint32_t)(frame >= 0 ? frame : 0) % PENDING_RESIM_DUMP_RING;
    g_pendingResimDumps[idx] = dump;
    g_pendingResimDumpValid[idx] = true;
}

extern "C" void Rollback_SendGroundTruth(int32_t startFrame, int32_t endFrame)
{
    if (!g_ndjsonLoggingEnabled) return;
    if (startFrame >= endFrame) return;

    GroundTruthPacket gtPkt = {};
    gtPkt.type = PACKET_GROUND_TRUTH;
    gtPkt.startFrame = startFrame;
    gtPkt.endFrame = endFrame;

    int32_t frameCount = 0;
    for (int32_t f = startFrame; f < endFrame && frameCount < MAX_RESIM_FRAMES; f++)
    {
        uint32_t liveIdx = (uint32_t)f % 256;
        auto& p1Live = g_liveRef[0][liveIdx];
        auto& p2Live = g_liveRef[1][liveIdx];

        auto& rf = gtPkt.frames[frameCount];
        rf.frame = f;

        auto fillState = [](ResimCharState& dst, const LiveFrameRecord& src) {
            dst.actionId = src.actionId;
            dst.posX     = src.posX;
            dst.posY     = src.posY;
            dst.posZ     = src.posZ;
            dst.flags    = src.flags14F0;
            dst.mHeld    = src.mHeld;
            dst.frameCnt = src.frameCnt;
            for (int ri = 0; ri < 4; ri++)
                dst.rng[ri] = src.rng[ri];
        };

        fillState(rf.p1Live, p1Live);
        fillState(rf.p2Live, p2Live);
        frameCount++;
    }
    gtPkt.frameCount = (uint32_t)frameCount;
    Netplay_SendGroundTruth(gtPkt);
}

extern "C" void Rollback_ProcessInboundGroundTruth()
{
    GroundTruthPacket gtPkt;
    while (Netplay_GetInboundGroundTruth(gtPkt))
    {
        if (!g_ndjsonLoggingEnabled) continue;
        Rollback_DiagInit();
        if (!g_diagNdjsonFile) continue;

        char tsBuf[32] = {};
        SYSTEMTIME st;
        GetLocalTime(&st);
        snprintf(tsBuf, sizeof(tsBuf), "%04d-%02d-%02dT%02d:%02d:%02d",
                 st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);

        for (uint32_t fi = 0; fi < gtPkt.frameCount; fi++)
        {
            auto& rf = gtPkt.frames[fi];

            uint32_t pidx = (uint32_t)(rf.frame >= 0 ? rf.frame : 0) % PENDING_RESIM_DUMP_RING;
            bool found = g_pendingResimDumpValid[pidx] && g_pendingResimDumps[pidx].frame == rf.frame;
            PendingResimDump resimDump = found ? g_pendingResimDumps[pidx] : PendingResimDump{};
            if (found) g_pendingResimDumpValid[pidx] = false;

            if (!found) continue;

            // Ground truth (received remote data)
            fprintf(g_diagNdjsonFile, "{\"v\":1,\"ts\":\"%s\",\"frame\":%d,\"side\":\"ground_truth\","
                       "\"chars\":[",
                    tsBuf, rf.frame);
            fprintf(g_diagNdjsonFile, "{\"team\":0,\"slot\":0,\"pos\":[%.4f,%.4f,%.4f],\"act\":\"0x%08X\",\"fl\":\"0x%08X\",\"mHeld\":\"0x%08X\",\"fcnt\":%llu,\"rng\":[\"0x%08X\",\"0x%08X\",\"0x%08X\",\"0x%08X\"]}",
                    rf.p1Live.posX, rf.p1Live.posY, rf.p1Live.posZ, rf.p1Live.actionId, rf.p1Live.flags, rf.p1Live.mHeld, (unsigned long long)rf.p1Live.frameCnt,
                    rf.p1Live.rng[0], rf.p1Live.rng[1], rf.p1Live.rng[2], rf.p1Live.rng[3]);
            fprintf(g_diagNdjsonFile, ",{\"team\":1,\"slot\":0,\"pos\":[%.4f,%.4f,%.4f],\"act\":\"0x%08X\",\"fl\":\"0x%08X\",\"mHeld\":\"0x%08X\",\"fcnt\":%llu,\"rng\":[\"0x%08X\",\"0x%08X\",\"0x%08X\",\"0x%08X\"]}",
                    rf.p2Live.posX, rf.p2Live.posY, rf.p2Live.posZ, rf.p2Live.actionId, rf.p2Live.flags, rf.p2Live.mHeld, (unsigned long long)rf.p2Live.frameCnt,
                    rf.p2Live.rng[0], rf.p2Live.rng[1], rf.p2Live.rng[2], rf.p2Live.rng[3]);
            fprintf(g_diagNdjsonFile, "]}\n");

            // Resim (local corrected data)
            fprintf(g_diagNdjsonFile, "{\"v\":1,\"ts\":\"%s\",\"frame\":%d,\"side\":\"resim\","
                       "\"chars\":[",
                    tsBuf, rf.frame);
            fprintf(g_diagNdjsonFile, "{\"team\":0,\"slot\":0,\"pos\":[%.4f,%.4f,%.4f],\"act\":\"0x%08X\",\"fl\":\"0x%08X\",\"mHeld\":\"0x%08X\",\"fcnt\":%llu,\"rng\":[\"0x%08X\",\"0x%08X\",\"0x%08X\",\"0x%08X\"]}",
                    resimDump.p1Post.posX, resimDump.p1Post.posY, resimDump.p1Post.posZ, resimDump.p1Post.actionId, resimDump.p1Post.flags, resimDump.p1Post.mHeld, (unsigned long long)resimDump.p1Post.frameCnt,
                    resimDump.p1Post.rng[0], resimDump.p1Post.rng[1], resimDump.p1Post.rng[2], resimDump.p1Post.rng[3]);
            fprintf(g_diagNdjsonFile, ",{\"team\":1,\"slot\":0,\"pos\":[%.4f,%.4f,%.4f],\"act\":\"0x%08X\",\"fl\":\"0x%08X\",\"mHeld\":\"0x%08X\",\"fcnt\":%llu,\"rng\":[\"0x%08X\",\"0x%08X\",\"0x%08X\",\"0x%08X\"]}",
                    resimDump.p2Post.posX, resimDump.p2Post.posY, resimDump.p2Post.posZ, resimDump.p2Post.actionId, resimDump.p2Post.flags, resimDump.p2Post.mHeld, (unsigned long long)resimDump.p2Post.frameCnt,
                    resimDump.p2Post.rng[0], resimDump.p2Post.rng[1], resimDump.p2Post.rng[2], resimDump.p2Post.rng[3]);
            fprintf(g_diagNdjsonFile, "]}\n");
        }
        DWORD now = GetTickCount();
        if (now - g_diagNdjsonFlush >= 1000) {
            fflush(g_diagNdjsonFile);
            g_diagNdjsonFlush = now;
        }
    }
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

    g_isResimulating = true;

    //HookLog("Rollback: [%s] re-simulating frames %d \u2192 %d\n",
            //Netplay_IsHost() ? "host" : "client", startFrame, endFrame);

    //Are these really necessary? Might as well leave them in

    Rollback_PatchSleep(true); // NOP Sleep calls so frames run at full speed
    Rollback_PatchPresent(true); // NOP Present inside post-render sync
    Rollback_PatchSwapchain(true); // NOP swapchain vtable+0x50 calls
    Rollback_PatchDrawCalls(true); // NOP remaining render/draw calls

    // Save the live input history before rewinding.  The re-sim loop calls
    // SilentFrameStart/End which shifts g_oldInputs with zeros (re-sim
    // bypasses the accumulation).  Restoring after the loop prevents the
    // outbound packet from carrying a corrupted timeline.
    //HookLog("[ReSim] ~~~~~~~~~~ g_oldInputs BEFORE re-sim loop ~~~~~~~~~~\n");
    Controller_SaveInputHistory();
    int silentFrameEndCount = 0;

    // Diagnostic dump: capture ground truth BEFORE rewinding.
    // Gives the live state of all regions + character state to compare
    // against what the re-simulation produces.
    // When g_f7DiagSession is set (F7 mode), skip the ground truth dump —
    // the F7 state machine already captured per-frame live dumps during
    // the recording phase using that session.  We just reuse the session
    // ID so resim dumps land alongside the live dumps.
    {
        int diagSid;
        if (g_f7DiagSession >= 0) {
            diagSid = g_f7DiagSession;
            HookLog("Rollback: diag session %d (F7, reuse) for frames %d\u2192%d\n",
                    diagSid, startFrame, endFrame);
        } else {
            Rollback_DiagInit();
            g_diagSessionId++;
            diagSid = g_diagSessionId;
            uint64_t sr = *(uint64_t*)_addr(PTR_SREPLAY);
            int32_t liveFrame = sr ? *(int32_t*)(sr + 0x54) : startFrame;
            g_diagWriteCount = 0;
            //HookLog("Rollback: diag session %d for frames %d\u2192%d (live=%d)\n",
                    //diagSid, startFrame, endFrame, liveFrame);
            // session_* files disabled — use rdiag.ndjson instead
            //Rollback_DumpBinary(diagSid, liveFrame, 0);
            //Rollback_DumpCharText(diagSid, liveFrame, 0);
            //Rollback_DumpManifest(diagSid, liveFrame, 0);
        }
        // Sync the global so the resim loop writes to the same session
        g_diagSessionId = diagSid;
    }

    // Capture the final predicted live state before restoring it. This is a
    // separate diagnostic entry because it belongs to the discarded timeline,
    // not to the corrected frame sequence.
    int32_t liveFrameBeforeRestore = -1;
    {
        uint64_t sReplay = *(uint64_t*)_addr(PTR_SREPLAY);
        if (sReplay) liveFrameBeforeRestore = *(int32_t*)(sReplay + 0x54);
    }
    if (liveFrameBeforeRestore >= 0 && Netplay_IsActive()) {
        std::string json = Rollback_GenerateNetplayLiveJSON(
            liveFrameBeforeRestore, false, "pre_restore_prediction", startFrame, endFrame);
        if (!json.empty()) Rollback_WriteNetplayLiveJSON(json);
    }

    // 1. Rewind to the starting frame boundary
    Rollback_RestoreSnapshot(startFrame);

    // Diagnostic: log whether we actually have received input for every frame
    // in the resim range before we start stepping.
    Controller_LogReceivedInputAvailability(startFrame, endFrame);

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
        // Capture PRE-state before the tick
        Rollback_CaptureFrameDiagSnapshot(f, /*isResim=*/true, /*postOnly=*/false);

        // Pre-tick NDJSON logging for both active and inactive netplay sessions
        if (Netplay_IsActive()) {
            Rollback_DiscardPendingNetplayLive(f);
            std::string json = Rollback_GenerateNetplayLiveJSON(f, true);
            if (!json.empty())
                Rollback_WriteNetplayLiveJSON(json);
        } else {
            Rollback_DumpNDJSON(f, 1);
        }

        // Multi-resim may correct a still-predicted frame again, so preserve a
        // corrected restore point. Non-multi waits for a definitive batch and
        // never starts another rollback inside this replayed range.
        if (g_multiResimEnabled) {
            Rollback_SaveSnapshot(f);
            Rollback_UpdateSnapshotInputs(f, resimLocalInput, resimRemoteInput);
        }

        //Todo: replace with headless tick for better perf
        oMvc3Main_move(g_sMvc3Main);
        Controller_SilentFrameEnd();
        silentFrameEndCount++;
        Controller_LogReSimFrame(f);

        // Capture POST-state after the tick (preserves the pre-tick state captured above)
        Rollback_CaptureFrameDiagSnapshot(f, /*isResim=*/true, /*postOnly=*/true);

        Rollback_QueuePendingResimDump(f);
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
    Controller_RestoreInputHistory();

    // [EXPERIMENTAL] mJobThreadNumInit restore — see mvc3/rollback R3-SIMLOOP
    // if (pJobThreadNumInit) {
    //     *pJobThreadNumInit = saved_job_thread_num_init;
    // }

    // 3. Resynchronize performance counter state inside sMain to match
    //    current system time, preventing the game loop from seeing a
    //    negative or excessive frame-time delta that would cause a freeze.
    uint64_t sMain = *(uint64_t*)_addr(PTR_SMAIN);
    if (sMain) {
        //Same with this, unsure if this is really needed, might cause lag if disabled
        Rollback_ResyncMainTiming(sMain);
    }

    g_isResimulating = false;
    if (g_resimReentranceDepth > 0) {
        HookLog("[ReSim] WARNING: re-entrance depth was %d during re-sim (nested calls detected)\n",
                g_resimReentranceDepth);
    }
    Rollback_PatchSleep(false); // restore Sleep calls
    Rollback_PatchPresent(false); // restore Present call
    Rollback_PatchSwapchain(false); // restore swapchain calls
    Rollback_PatchDrawCalls(false); // restore render/draw calls
    //HookLog("Rollback: re-simulation complete (%d frames)\n",
            //endFrame - startFrame);
}

void Rollback_RequestReSimulate(int32_t startFrame, int32_t endFrame)
{
    g_rollbackReqStart = startFrame;
    g_rollbackReqEnd   = endFrame;
}

// ===========================================================================
// Post-Restore Cleanup — clear stale pointer/slot tables after snapshot restore
//
// The flat-memcpy restore of character objects brings back slot arrays at
// offset +0x6748 that reference heap objects (effects, projectiles, etc.)
// which were NOT saved in the snapshot.  Any subsequent call to
// MtObject__replaceSlot_6F30 on these slots will try to call vtable[0x78]
// (release) on a stale/corrupted object → page fault at NULL.
//
// We walk the team character lists (same traversal as BuildRegions) and clear
// only the slot entries confirmed to hold stale references. Do not clear the
// whole allocation: this range overlaps character-specific state.
//
// Slot array base: character + 0x6748.
// Confirmed stale-reference indices: 1, 14, 15.
//
// uBaseModel also owns a material-instance cache through +0x110, with its
// entry count at +0x118. Character action changes release every cached entry
// before rebuilding it. The cache allocation itself remains live across a
// restore, but entries retired on the prediction timeline can be dangling.
// ===========================================================================
static void ClearModelMaterialCache(uint64_t model)
{
    if (!HasCallableDtiVirtual((void*)model)) return;

    uint64_t entries = *(uint64_t*)(model + 0x110);
    uint32_t count = *(uint32_t*)(model + 0x118);
    if (!entries || count > 4096 || !IsAccessibleAddress(entries)) return;

    for (uint32_t i = 0; i < count; ++i) {
        uint64_t entry = entries + (uint64_t)i * sizeof(uint64_t);
        if (!IsAccessibleAddress(entry)) break;
        uint64_t material = *(uint64_t*)entry;
        if (material && !HasCallableDtiVirtual((void*)material))
            *(uint64_t*)entry = 0;
    }
}

static void Rollback_ClearStalePointers()
{
    uint64_t sCharacter = *(uint64_t*)_addr(PTR_SCHARACTER);
    if (!sCharacter) return;

    static constexpr uintptr_t SLOT_ARRAY_OFFSET = 0x6748;
    static constexpr uint32_t STALE_SLOT_INDICES[] = {1, 14, 15};

    for (int ti = 0; ti < 2; ++ti) {
        uint64_t team = sCharacter + (ti == 0 ? TEAM0_OFFSET : TEAM1_OFFSET);
        uint64_t node = team + TEAM_FIRST_NODE_OFFSET;

        while (node) {
            uint64_t character = *(uint64_t*)(node + NODE_CHR_OFFSET);
            if (character) {
                ClearModelMaterialCache(character);

                // cChr owns an array of character-specific weapon models.
                // Hsien-Ko, for example, resets weapon index 12 on every
                // action change and follows that weapon's material cache.
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
                    uint64_t slotAddress = character + SLOT_ARRAY_OFFSET +
                                           (uint64_t)index * sizeof(uint64_t);
                    if (IsAccessibleAddress(slotAddress))
                        *(uint64_t*)slotAddress = 0;
                }
            }
            node = *(uint64_t*)(node + NODE_NEXT_OFFSET);
        }
    }

//HookLog("Rollback: cleared character slot arrays after restore\n");
}

static void Rollback_SyncActiveCharacterUi()
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

        // Match cChrTeam::commitPendingCharacterSwap: the UI table is keyed by
        // player/team id and stores the active member id.
        int playerId = *(int*)(active + 0x2DA0);
        int memberId = *(int*)(active + 0x2DB0);
        setTeamCharacterId(sGameUi, playerId, memberId);
    }
}

// ===========================================================================
// Post-Restore Cleanup — clear sGameEffect slot tables after snapshot restore
//
// The sGameEffect manager maintains two slot tables with MtObject* weak
// references to active effect objects:
//   generic slots — 10 entries × 8 bytes at +0x40
//   character slots — 6 records × 0x18 bytes at +0x90; each record contains
//                     a three-entry effect pointer array
//
// These entries point to heap-allocated effect nodes that were NOT saved in
// the snapshot (sUnit effect category traversal is skipped due to pool
// allocator corruption).  After restore any stale pointers in these tables
// would be dereferenced by the game's effect update path, causing crashes
// or visual corruption (broken animation positions).  Zeroing them lets the
// game allocate fresh effect slots on the next tick.
// ===========================================================================
static void Rollback_ClearGameEffectSlots()
{
    uint64_t sGameEffect = *(uint64_t*)_addr(PTR_SGAMEEFFECT);
    if (!sGameEffect) return;

    static constexpr size_t GENERIC_SLOTS_OFFSET = 0x40;
    static constexpr size_t GENERIC_SLOTS_SIZE   = 10 * sizeof(uint64_t);
    static constexpr size_t CHARACTER_SLOTS_OFFSET = 0x90;
    static constexpr size_t CHARACTER_SLOTS_SIZE   = 6 * 0x18;

    memset((void*)(sGameEffect + GENERIC_SLOTS_OFFSET), 0, GENERIC_SLOTS_SIZE);
    memset((void*)(sGameEffect + CHARACTER_SLOTS_OFFSET), 0, CHARACTER_SLOTS_SIZE);

//    HookLog("Rollback: cleared sGameEffect slot tables after restore\n");
}

void Rollback_FreeSnapshots()
{
    // Make sure patched calls are restored before tearing down
    if (g_sleepPatched)
        Rollback_PatchSleep(false);
    if (g_presentPatched)
        Rollback_PatchPresent(false);
    if (g_swapchainPatched)
        Rollback_PatchSwapchain(false);
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
// Hook Setup — intercept sMvc3Main__move
//
// We hook the top-level frame update function (0x1402594A0) so we can:
//   1. Capture the sMvc3Main instance pointer for re-simulation calls
//   2. (Future) Gate execution during re-simulation or inject pre-frame logic
// ===========================================================================

static void __fastcall hkMvc3Main_move(uint64_t param_1)
{
    // Log EVERY call so we can tell if the game loop keeps running after re-sim
    static int32_t frameCounter = 0;
    frameCounter++;
   // HookLog("hkMvc3Main_move: frame=%d g_isResimulating=%d\n",
   //         frameCounter, (int)g_isResimulating);

    // Capture the main manager pointer on first call
    if (!g_sMvc3Main) {
        g_sMvc3Main = param_1;
        //HookLog("Rollback: captured sMvc3Main @ 0x%llX\n", param_1);
    }

    int32_t curFrame = (int32_t)Controller_GetNetFrameCount();

    // 1. Process pending snapshot-restore request from FrameStepper_Process (F6 key).
    // Execute at frame boundary (BEFORE oMvc3Main_move) to avoid mid-frame
    // memory corruption when restoring shared game state.
    static bool s_postRestoreTickDump = false;
    if (g_restoreRequestedFrame >= 0) {
        int32_t rf = g_restoreRequestedFrame;
        g_restoreRequestedFrame = -1;
        HookLog("Rollback: executing deferred restore to frame %d\n", rf);
        Rollback_RestoreSnapshot(rf);
        s_postRestoreTickDump = true;
    }

    // 2. Process pending re-sim request from HookUpdate thread (FrameStepper_Process).
    // Execute it HERE on the main game thread so sRender path runs safely.
    if (g_resimRequestedStart >= 0) {
        int32_t rs = g_resimRequestedStart;
        int32_t re = g_resimRequestedEnd;
        g_resimRequestedStart = -1;
        g_resimRequestedEnd   = -1;
        Rollback_ReSimulate(rs, re);
        // After re-sim, let the normal frame proceed
    }

    // 3. Process F7 re-sim test: save snapshot, record N frames of normal play, restore
    // and re-sim with the captured inputs (exercises the full rollback path).
    if (g_f7State == 1) {
        g_f7StartFrame = (int32_t)Controller_GetNetFrameCount();
        Rollback_SaveSnapshot(g_f7StartFrame);
        // Initialize diag session so Rollback_ReSimulate reuses the same one
        // (live per-frame dumps during recording + resim dumps side by side).
        Rollback_DiagInit();
        g_f7DiagSession = g_diagSessionId;
        g_diagSessionId++;
        g_f7State = 2;
        HookLog("F7: snapshot saved at frame %d, recording %d frames\n",
                g_f7StartFrame, g_f7Remaining);
    }
    if (g_f7State == 2) {
        g_f7Remaining--;
        if (g_f7Remaining <= 0) {
            // Last recording frame — capture live state before re-sim (Pre-Tick)
            if (g_f7DiagSession >= 0) {
                Rollback_DumpNDJSON(curFrame, 0);
            }
            g_f7State = 0;
            HookLog("F7: %d frames recorded, restoring frame %d and re-simulating\n",
                    8, g_f7StartFrame);
            Rollback_ReSimulate(g_f7StartFrame, g_f7StartFrame + 8);
            g_f7DiagSession = -1;

            int32_t updatedFrame = (int32_t)Controller_GetNetFrameCount();
            if (updatedFrame >= 0) {
                Rollback_SaveSnapshot(updatedFrame);
                g_postResimFrame = updatedFrame;
            }
        }
    }

    // ==================================================================
    // 4. Auto-snapshot + rollback for netplay (only on main game thread,
    // not during re-sim).  Only active when rollback frames > 0.
    // ==================================================================
    if (Netplay_IsActive() && FrameStepper_IsBattleSynchronized() &&
        !g_isResimulating && Controller_IsRollbackEnabled())
    {
        // Only snapshot + rollback when in-game (aNextBattle step >= 2,
        // i.e. countdown or fighting).  Menus/loading screens don't need
        // rollback state and the frame counter jumps around there.
        typedef uint64_t(__fastcall* GetAmaterasu_t)();
        static GetAmaterasu_t getAmaterasu = (GetAmaterasu_t)_addr(0x140001A90);
        uint64_t aNextBattle = getAmaterasu();
        int step = aNextBattle ? *(int*)(aNextBattle + 0xD8) : 0;
        if (step >= 0)
        {
            // Clear post-resim boundary flag at start of each frame.
            // This ensures the flag is active for exactly one live tick
            // (the one immediately after Rollback_ReSimulate returns).
            if (!g_isResimulating)
                g_postResimFrame = -1;

            // Save snapshot for the current frame BEFORE the game tick
            Rollback_SaveSnapshot(curFrame);

            // Consume any pending rollback request from controller.cpp
            if (g_rollbackReqStart >= 0)
            {
                int32_t rs = g_rollbackReqStart;
                int32_t re = g_rollbackReqEnd;
                g_rollbackReqStart = -1;
                g_rollbackReqEnd   = -1;
                Rollback_ReSimulate(rs, re);

                // Notify the remote peer which frames we just re-simulated,
                // including per-frame resim results for BOTH teams' point
                // characters so their NDJSON log shows both sides' states.
                if (g_ndjsonLoggingEnabled) {
                    ResimNotifyPacket rnPkt = {};
                    rnPkt.type = PACKET_RESIM_NOTIFY;
                    rnPkt.resimStartFrame = rs;
                    rnPkt.resimEndFrame = re;
                    rnPkt.correctedTeam = Netplay_IsHost() ? 1 : 0;
                    int32_t notifyStart = rs - 2;
                    if (notifyStart < 0) notifyStart = 0;
                    int32_t notifyEnd = re + 2;
                    int32_t frameCount = 0;
                    for (int32_t f = notifyStart; f < notifyEnd && frameCount < MAX_RESIM_FRAMES; f++)
                    {
                        auto& rf = rnPkt.frames[frameCount];
                        rf.frame = f;

                        auto fillLive = [](ResimCharState& dst, const LiveFrameRecord& src) {
                        dst.actionId = src.actionId;
                        dst.posX     = src.posX;
                        dst.posY     = src.posY;
                        dst.posZ     = src.posZ;
                        dst.flags    = src.flags14F0;
                        dst.mHeld    = src.mHeld;
                        dst.frameCnt = src.frameCnt;
                        for (int ri = 0; ri < 4; ri++)
                            dst.rng[ri] = src.rng[ri];
                    };

                    bool isResimFrame = (f >= rs && f < re);
                    if (isResimFrame)
                    {
                        // PRE state from g_liveRef (live state recorded during gameplay)
                        uint32_t lrIdx = (uint32_t)(f >= 0 ? f : 0) % 256;
                        fillLive(rf.p1Pre, g_liveRef[0][lrIdx]);
                        fillLive(rf.p2Pre, g_liveRef[1][lrIdx]);

                        // POST state from pending resim dump (resimulation output)
                        uint32_t pidx = (uint32_t)(f >= 0 ? f : 0) % PENDING_RESIM_DUMP_RING;
                        if (g_pendingResimDumpValid[pidx] && g_pendingResimDumps[pidx].frame == f) {
                            rf.p1Post = g_pendingResimDumps[pidx].p1Post;
                            rf.p2Post = g_pendingResimDumps[pidx].p2Post;
                        } else {
                            // Fallback: post == pre if dump not available
                            rf.p1Post = rf.p1Pre;
                            rf.p2Post = rf.p2Pre;
                        }
                    }
                    else
                    {
                        // Context frame: pre == post == live state from g_liveRef
                        uint32_t liveIdx = (uint32_t)(f >= 0 ? f : 0) % 256;
                        fillLive(rf.p1Pre,  g_liveRef[0][liveIdx]);
                        fillLive(rf.p1Post, g_liveRef[0][liveIdx]);
                        fillLive(rf.p2Pre,  g_liveRef[1][liveIdx]);
                        fillLive(rf.p2Post, g_liveRef[1][liveIdx]);
                    }

                        rf.usedInput = Controller_GetUsedRemoteInput(f);
                        rf.recvInput = Controller_GetReceivedRemoteInput(f);
                        frameCount++;
                    }
                    rnPkt.frameCount = (uint32_t)frameCount;
                    Netplay_SendResimNotify(rnPkt);
                }

                // Re-resolve the current frame's remote input — packets that
                // arrived during the resim may now provide real data where we
                // only had a prediction before.
                Controller_ResolveRemoteInput(curFrame);
                //HookLog("Rollback: re-resolved remote input for frame %d\n", curFrame);

                // After re-sim, re-save the current frame's snapshot so it
                // reflects the corrected state at start of curFrame.
                Rollback_SaveSnapshot(curFrame);

                // Flag the first post-resim live tick for diagnostic logging.
                g_postResimFrame = curFrame;
                //HookLog("Rollback: resim complete, live tick frame=%d (post-resim boundary)\n",
                        //curFrame);
            }
        }
    }

    // 5. Early return if we are in the middle of re-simulation
    if (g_isResimulating) {
        oMvc3Main_move(param_1);
        return;
    }

    // Update curFrame to ensure we log/capture using the corrected frame timeline
    curFrame = (int32_t)Controller_GetNetFrameCount();

    // 6. Unconditional Netplay Live NDJSON logging at the start of the frame (Pre-Tick)
    // Runs only after any pending rollbacks/re-simulations have stabilized the frame state
    if (Netplay_IsActive() && curFrame >= 0) {
        Rollback_DumpNetplayLive(curFrame);
    }

    // 7. Capture corrected PRE-state before the tick
    if (curFrame >= 0) {
        Rollback_CaptureFrameDiagSnapshot(curFrame, /*isResim=*/false, /*postOnly=*/false);
    }

    oMvc3Main_move(param_1);

    // 8. Capture POST-state after the tick (preserves the pre-state captured above)
    if (curFrame >= 0) {
        Rollback_CaptureFrameDiagSnapshot(curFrame, /*isResim=*/false, /*postOnly=*/true);
    }

    // F7 live capture during recording for frames 1-7
    if (g_f7DiagSession >= 0 && g_f7State == 2) {
        Rollback_DumpNDJSON(curFrame, 0);
    }
}

// Keep the callback's phase-1 collision-node state transition, but suppress
// phase 0, which accumulates the player-to-player separation correction.
static void __fastcall hkApplyCollisionPushCorrection(
    uint64_t character,
    int callbackPhase,
    uint64_t collisionObject,
    uint64_t otherCharacter,
    uint64_t collisionResult,
    uint32_t collisionSlot)
{
    if (callbackPhase != 0 && oApplyCollisionPushCorrection) {
        oApplyCollisionPushCorrection(
            character,
            callbackPhase,
            collisionObject,
            otherCharacter,
            collisionResult,
            collisionSlot);
    }
}

// ===========================================================================
// Hook - sRender__endFrameAndPresent
//
// Called at the end of sMvc3Main__move as the last operation on the render
// manager (sMain+0x40190).  Performs frame-rate checking, Present(), and
// swap-chain management.
//
// During re-sim (g_isResimulating) we skip this entirely to avoid the
// ~16ms VSYNC wait per frame.  The re-sim now runs on the main game thread
// (via g_resimRequestedStart/End consumed in hkMvc3Main_move), so skipping
// this is safe — no thread-safety issue with the render path.
// ===========================================================================
static void __fastcall hkCallSRender(uint64_t param_1)
{
    if (g_isResimulating) {
        return;
    }
    oCallSRender(param_1);
}

static void __fastcall hkSound_move(uint64_t param_1)
{
    // Sound scenes contain live emitter/model pointers and transient matrix
    // indices. They are cosmetic and are not part of the rollback snapshot, so
    // advancing them during headless ticks can dereference abandoned objects.
    if (g_isResimulating)
        return;

    // sScene__updateSoundEnv calls uBaseModel's matrix virtual at +0xA0 without
    // validating the attachment. A rollback can destroy or reuse a transient
    // model while its sound scene remains live. The reused address may still be
    // a valid object with an executable +0xA0 slot, so require model ancestry.
    constexpr uint32_t SOUND_SCENE_COUNT_OFFSET = 0x1CF50;
    constexpr uint32_t SOUND_SCENE_FIRST_OFFSET = 0x70;
    constexpr uint32_t SOUND_SCENE_STRIDE = 0xB50;
    constexpr uint32_t SOUND_SCENE_MODEL_OFFSET = 0x80;
    constexpr uint32_t SOUND_SCENE_MAX_COUNT = 40;

    uint32_t sceneCount = *(uint32_t*)(param_1 + SOUND_SCENE_COUNT_OFFSET);
    if (sceneCount <= SOUND_SCENE_MAX_COUNT) {
        for (uint32_t i = 0; i < sceneCount; ++i) {
            uint64_t scene = param_1 + SOUND_SCENE_FIRST_OFFSET +
                (uint64_t)i * SOUND_SCENE_STRIDE;
            uint64_t* modelSlot = (uint64_t*)(scene + SOUND_SCENE_MODEL_OFFSET);
            uint64_t model = *modelSlot;
            if (!model)
                continue;

            bool valid = IsBaseModelObject((void*)model);
            uint64_t vtable = valid ? *(uint64_t*)model : 0;
            valid = valid && IsAccessiblePointer(vtable) &&
                IsAccessibleAddress(vtable + 0xA0);
            uint64_t getMatrix = valid ? *(uint64_t*)(vtable + 0xA0) : 0;
            if (!valid || !IsExecutableAddress(getMatrix)) {
                HookLog("Rollback: detached stale sound scene model scene=%u model=0x%llX vtable=0x%llX\n",
                        i, model, vtable);
                *modelSlot = 0;
            }
        }
    }

    oSound_move(param_1);
}

static void __fastcall hkGameUi_move(uint64_t param_1)
{
    // Re-simulation currently runs the full native sMvc3Main__move rather than
    // Rollback_HeadlessTick. Keep unsnapshotted UI resources and scheduler
    // arrays on the final presentation timeline only.
    if (g_isResimulating)
        return;

    oGameUi_move(param_1);
}

static void __fastcall hkSchedulerRebuild(uint64_t scheduler)
{
    // UI and battle-message units can both be created through sUnit while
    // replaying frames. Merely skipping the allocation leaves the new unit in
    // active state 2 with an empty scheduler, so it survives onto the final
    // timeline and repeated rollbacks accumulate presentation objects. Retire
    // it through sUnit's normal state-3 two-phase destruction path instead.
    if (g_isResimulating) {
        uint32_t* state = (uint32_t*)(scheduler + 0x10);
        *state = (*state & ~7u) | 3u;
        return;
    }

    oSchedulerRebuild(scheduler);
}

static bool __fastcall hkRChrCpuClassFunc(void* cpu, void* object)
{
    // This opcode pops a 64-bit object from two 32-bit VM stack entries and
    // immediately calls object->vtable[0x20]. A rewound script can reach the
    // opcode with an ordinary packed script value (the observed crash used
    // 0x0000008400000084). Native behavior for null/type mismatch is false, so
    // reject values that cannot be safely dispatched as MtObject instances.
    uint64_t address = (uint64_t)object;
    if (!IsAccessiblePointer(address))
        return false;

    uint64_t vtable = *(uint64_t*)address;
    if (!IsAccessiblePointer(vtable) || !IsAccessibleAddress(vtable + 0x20))
        return false;

    uint64_t getDti = *(uint64_t*)(vtable + 0x20);
    if (!IsExecutableAddress(getDti))
        return false;

    return oRChrCpuClassFunc(cpu, object);
}

// ===========================================================================
// Hook — sMain__move (frame timing / Sleep / busy-wait)
//
// Reads QPC, computes frame deltas, NOPs Sleep calls, and spins in a
// busy-wait for frame pacing.  During re-simulation we skip it entirely
// (just set tick count to 1) because:
//   1. Sleep is already NOP'd by Rollback_PatchSleep, but the busy-wait
//      still spins for the full frame time (~16ms @ 60fps).
//   2. Accumulated/deadline timing values drift across calls, causing
//      progressively longer busy-waits that eventually hang forever.
//   3. We sync QPC state via Rollback_ResyncMainTiming after the loop.
// ===========================================================================
static void __fastcall hkMain_move(uint64_t param_1)
{
    if (g_isResimulating) {
        *(uint32_t*)(param_1 + 0xA8) = 1;
        *(uint32_t*)(param_1 + 0xA4) = 1;
        // Force tick=0 so the game's dispatcher loop won't call
        // sMvc3Main__move again.  Setting tick=1 was the root cause
        // of the infinite loop: the dispatcher checked tick, saw 1,
        // called sMvc3Main__move which hit hkMvc3Main_move (SKIPPED),
        // tick was never consumed, so the dispatcher looped forever.
        *(uint32_t*)(param_1 + 0x48) = 0;
        // Deterministic frame delta during re-sim: sMain__move is skipped so
        // mDeltaTime (+0x80) would stay frozen at the snapshot value.  The
        // dash double-tap timers in cChrInput_inputMaskPostProcess do
        // acc += mDeltaTime each frame (thresholds 2.0/3.0/8.0/10.0 frames),
        // so it must be exactly 1.0 per simulated frame.
        *(float*)(param_1 + 0x80) = 1.0f;
        return;
    }

    // Eliminate job scheduling as a source of peer-dependent floating-point
    // ordering in constraint and character-push work. sMain::move copies the
    // desired count at +0xA8 into the active count at +0xA4, so set both before
    // the call. Keep this active from loading through fighting so pre-fight
    // collision caches are constructed under the same execution mode.
    if (Netplay_IsActive()) {
        static bool loggedSingleThread = false;
        *(uint32_t*)(param_1 + 0xA8) = 1;
        *(uint32_t*)(param_1 + 0xA4) = 1;
        if (!loggedSingleThread) {
            HookLog("Netplay: forcing deterministic single-thread job execution\n");
            loggedSingleThread = true;
        }
    }

    oMain_move(param_1);

    // Netplay determinism: mDeltaTime (+0x80) is QPC-derived wall-clock
    // jitter (≈1.0 but different on each machine every frame).  The dash
    // double-tap window timers integrate it, so host and client drift apart
    // and eventually detect dashes on different frames (frame-227 desync).
    // Force exactly 1 frame of game time per tick on both machines.
    if (Netplay_IsActive()) {
        *(float*)(param_1 + 0x80) = 1.0f;
        // Defend against runtime settings code changing the desired count
        // between ticks. The next sMain::move will copy this value to +0xA4.
        *(uint32_t*)(param_1 + 0xA8) = 1;
        *(uint32_t*)(param_1 + 0xA4) = 1;
    }
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

    MH_STATUS s = MH_CreateHook(
        (LPVOID)_addr(FN_SMVC3MAIN_MOVE),
        (LPVOID)&hkMvc3Main_move,
        (LPVOID*)&oMvc3Main_move
    );

    if (s == MH_OK) {
        MH_EnableHook((LPVOID)_addr(FN_SMVC3MAIN_MOVE));
        HookLog("Rollback: hooked sMvc3Main__move (0x%llX)\n",
                (uint64_t)_addr(FN_SMVC3MAIN_MOVE));
    } else {
        HookLog("Rollback: failed to hook sMvc3Main__move (%d)\n", s);
    }

    s = MH_CreateHook((LPVOID)_addr(FN_CCHR_SET_POS),
                      (LPVOID)&hkCChrSetPos, (LPVOID*)&oCChrSetPos);
    if (s == MH_OK) {
        //MH_EnableHook((LPVOID)_addr(FN_CCHR_SET_POS));
        HookLog("Rollback: hooked cChr__setPos for resim diagnostics\n");
    } else {
        HookLog("Rollback: failed to hook cChr__setPos (%d)\n", s);
    }

    s = MH_CreateHook((LPVOID)_addr(FN_SSHOT_SPAWN),
                      (LPVOID)&hkShotSpawn, (LPVOID*)&oShotSpawn);
    if (s == MH_OK) {
        MH_EnableHook((LPVOID)_addr(FN_SSHOT_SPAWN));
        HookLog("Rollback: hooked sShot__spawnShot\n");
    } else {
        HookLog("Rollback: failed to hook sShot__spawnShot (%d)\n", s);
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

    s = MH_CreateHook(
        (LPVOID)_addr(FN_APPLY_COLLISION_PUSH),
        (LPVOID)&hkApplyCollisionPushCorrection,
        (LPVOID*)&oApplyCollisionPushCorrection
    );
    if (s == MH_OK) {
    //    MH_EnableHook((LPVOID)_addr(FN_APPLY_COLLISION_PUSH));
    //    HookLog("Rollback: player collision pushboxes disabled (0x%llX)\n",
    //            (uint64_t)_addr(FN_APPLY_COLLISION_PUSH));
    } else {
        HookLog("Rollback: failed to disable player collision pushboxes (%d)\n", s);
    }

    // Render/present function — NOP during re-sim so the full game tick
    // (oMvc3Main_move) can be called from Rollback_ReSimulate without
    // actually drawing to the screen.
    s = MH_CreateHook(
        (LPVOID)_addr(FN_RENDER_END_FRAME),
        (LPVOID)&hkCallSRender,
        (LPVOID*)&oCallSRender
    );
    if (s == MH_OK) {
        MH_EnableHook((LPVOID)_addr(FN_RENDER_END_FRAME));
        HookLog("Rollback: hooked sRender call (0x%llX)\n",
                (uint64_t)_addr(FN_RENDER_END_FRAME));
    } else {
        HookLog("Rollback: failed to hook sRender call (%d)\n", s);
    }

    s = MH_CreateHook(
        (LPVOID)_addr(FN_SSOUND_MOVE),
        (LPVOID)&hkSound_move,
        (LPVOID*)&oSound_move
    );
    if (s == MH_OK) {
        MH_EnableHook((LPVOID)_addr(FN_SSOUND_MOVE));
        HookLog("Rollback: hooked sSound__move for headless re-simulation\n");
    } else {
        HookLog("Rollback: failed to hook sSound__move (%d)\n", s);
    }

    s = MH_CreateHook(
        (LPVOID)_addr(FN_SGAMEUI_MOVE),
        (LPVOID)&hkGameUi_move,
        (LPVOID*)&oGameUi_move
    );
    if (s == MH_OK) {
        MH_EnableHook((LPVOID)_addr(FN_SGAMEUI_MOVE));
        HookLog("Rollback: hooked sGameUI__move for re-simulation suppression\n");
    } else {
        HookLog("Rollback: failed to hook sGameUI__move (%d)\n", s);
    }

    s = MH_CreateHook(
        (LPVOID)_addr(FN_SCHEDULER_REBUILD),
        (LPVOID)&hkSchedulerRebuild,
        (LPVOID*)&oSchedulerRebuild
    );
    if (s == MH_OK) {
        MH_EnableHook((LPVOID)_addr(FN_SCHEDULER_REBUILD));
        HookLog("Rollback: hooked uScheduler rebuild for re-simulation suppression\n");
    } else {
        HookLog("Rollback: failed to hook uScheduler rebuild (%d)\n", s);
    }

    s = MH_CreateHook(
        (LPVOID)_addr(FN_RCHRCPU_CLASS_FUNC),
        (LPVOID)&hkRChrCpuClassFunc,
        (LPVOID*)&oRChrCpuClassFunc
    );
    if (s == MH_OK) {
        MH_EnableHook((LPVOID)_addr(FN_RCHRCPU_CLASS_FUNC));
        HookLog("Rollback: guarded rChrCpu script object cast\n");
    } else {
        HookLog("Rollback: failed to guard rChrCpu script object cast (%d)\n", s);
    }

    // sMain__move (frame-timing / QPC / Sleep / busy-wait) — NOP during re-sim
    // so the full game tick runs at full speed without frame-pacing waits.
    s = MH_CreateHook(
        (LPVOID)_addr(FN_SMAIN_MOVE),
        (LPVOID)&hkMain_move,
        (LPVOID*)&oMain_move
    );
    if (s == MH_OK) {
        MH_EnableHook((LPVOID)_addr(FN_SMAIN_MOVE));
        HookLog("Rollback: hooked sMain__move (0x%llX)\n",
                (uint64_t)_addr(FN_SMAIN_MOVE));
    } else {
        HookLog("Rollback: failed to hook sMain__move (%d)\n", s);
    }

}
