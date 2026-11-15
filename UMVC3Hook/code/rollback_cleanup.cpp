#include "../pch.h"

#include "rollback_cleanup.h"
#include "rollback_memory.h"
#include "rollback_snapshot.h"
#include "mk10utils.h"

#include <cstring>
#include <vector>

extern void HookLog(const char* fmt, ...);

static constexpr uintptr_t PTR_SCHARACTER = 0x140d44a70;
static constexpr uintptr_t PTR_SGAMEUI = 0x140d53a18;
static constexpr uintptr_t PTR_SGAMEEFFECT = 0x140d46470;
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

// Life-gauge member order.
//
// sGameUi+0x5960[team] is a one-shot tag request, not a cached member id:
// cChrTeam__commitPendingCharacterSwap writes the incoming member's old slot
// (1/2) and uBtGaugeLife consumes it once (0x1402F48A0), swapping
// mChrList[0] <-> mChrList[slot] and writing 0 back. The gauge UI does not
// tick during resimulation, so a tag replayed or undone there leaves the
// order permanently wrong. Every swap mirrors the member-id swap, so the
// canonical order is mChrList[k] = default member id of the character whose
// current member id is k. Rebuild it after resim and drop the stale request.
static constexpr uintptr_t VTABLE_UBTGAUGELIFE = 0x140B1BF90;
static constexpr uint32_t UBTGAUGELIFE_TEAM_OFFSET = 0xB0;       // mId
static constexpr uint32_t UBTGAUGELIFE_CHR_LIST_OFFSET = 0xB4;   // mChrList[3]
static constexpr uint32_t UBTGAUGELIFE_ARCADE_BOSS_OFFSET = 0xC9; // mIsArcadeBoss
static constexpr uint32_t CCHR_DEFAULT_MEMBER_ID_OFFSET = 0x2DAC;
static constexpr uint32_t CCHR_MEMBER_ID_OFFSET = 0x2DB0;
static constexpr uint32_t SUNIT_BATTLE_UI_CHILD_CATEGORY = 0x1D;

static bool BuildGaugeMemberOrder(uint64_t sCharacter, int teamIndex, int32_t (&order)[3])
{
    uint64_t node = sCharacter + (teamIndex == 0 ? TEAM0_OFFSET : TEAM1_OFFSET) +
        TEAM_FIRST_NODE_OFFSET;
    bool found[3] = {};
    for (int member = 0; node && member < ROLLBACK_MAX_TEAM_NODES;
         ++member, node = *(uint64_t*)(node + NODE_NEXT_OFFSET)) {
        uint64_t character = *(uint64_t*)(node + NODE_CHR_OFFSET);
        if (!character) continue;
        int32_t memberId = *(int32_t*)(character + CCHR_MEMBER_ID_OFFSET);
        int32_t defaultId = *(int32_t*)(character + CCHR_DEFAULT_MEMBER_ID_OFFSET);
        // First match, as cChrTeam__findChrByMemberId.
        if (memberId < 0 || memberId > 2 || found[memberId]) continue;
        if (defaultId < 0 || defaultId > 2) return false;
        order[memberId] = defaultId;
        found[memberId] = true;
    }
    if (!found[0] || !found[1] || !found[2]) return false;
    return order[0] != order[1] && order[0] != order[2] && order[1] != order[2];
}

void Rollback_SyncActiveCharacterUi()
{
    uint64_t sCharacter = *(uint64_t*)_addr(PTR_SCHARACTER);
    uint64_t sGameUi = *(uint64_t*)_addr(PTR_SGAMEUI);
    uint64_t sUnit = *(uint64_t*)_addr(PTR_SUNIT);
    if (!sCharacter || !sGameUi || !sUnit) return;

    using SetTeamCharacterId = void(__fastcall*)(uint64_t, int, int);
    auto setTeamCharacterId = (SetTeamCharacterId)_addr(FN_GAMEUI_SET_TEAM_CHARACTER_ID);

    int32_t order[2][3];
    bool valid[2];
    for (int teamIndex = 0; teamIndex < 2; ++teamIndex) {
        valid[teamIndex] = BuildGaugeMemberOrder(sCharacter, teamIndex, order[teamIndex]);
        // Only clear a request the rebuild below supersedes.
        if (valid[teamIndex] &&
            *(int32_t*)(sGameUi + SGAMEUI_TEAM_CHARACTER_ID_OFFSET + teamIndex * 4) != 0)
            setTeamCharacterId(sGameUi, teamIndex, 0);
    }

    const uint64_t gaugeVtable = _addr(VTABLE_UBTGAUGELIFE);
    int gaugesFound = 0;
    auto scanCategory = [&](uint32_t category) {
        uint64_t unit = *(uint64_t*)(sUnit + SUNIT_CATEGORY_LISTS_OFFSET +
            category * SUNIT_CATEGORY_LIST_STRIDE);
        for (int safety = 512; unit && gaugesFound < 2 && safety-- > 0;
             unit = *(uint64_t*)(unit + 0x18)) {
            if (*(uint64_t*)unit != gaugeVtable) continue;
            ++gaugesFound;
            uint32_t team = *(uint32_t*)(unit + UBTGAUGELIFE_TEAM_OFFSET);
            // The arcade boss gauge never consumes swap requests.
            if (team > 1 || !valid[team] || *(uint8_t*)(unit + UBTGAUGELIFE_ARCADE_BOSS_OFFSET))
                continue;
            int32_t* chrList = (int32_t*)(unit + UBTGAUGELIFE_CHR_LIST_OFFSET);
            if (std::memcmp(chrList, order[team], sizeof(order[team])) == 0) continue;
            HookLog("Rollback: life gauge %u order %d,%d,%d -> %d,%d,%d\n", team,
                    chrList[0], chrList[1], chrList[2],
                    order[team][0], order[team][1], order[team][2]);
            std::memcpy(chrList, order[team], sizeof(order[team]));
        }
    };

    uint32_t categoryCount = *(uint32_t*)(sUnit + SUNIT_CATEGORY_COUNT_OFFSET);
    if (categoryCount > 64) categoryCount = 64;
    // uBattleUi__setup adds its child gauges at its own move line + 1.
    if (SUNIT_BATTLE_UI_CHILD_CATEGORY < categoryCount)
        scanCategory(SUNIT_BATTLE_UI_CHILD_CATEGORY);
    for (uint32_t category = 0; category < categoryCount && gaugesFound < 2; ++category) {
        if (category != SUNIT_BATTLE_UI_CHILD_CATEGORY) scanCategory(category);
    }
}

// ---------------------------------------------------------------------------
// uCutIn rollback
//
// Live cut-ins are registered in the MtArray at sGameUI+0x5670 by the uCutIn
// ctor/dtor (sGameUI__registerCutin 0x1402BFCF0). uCutIn runs in unit category
// 3, so it ticks during headless resimulation, but it is not part of the
// snapshot regions. Without rewinding, a rollback inside a cut-in replays its
// timer on top of the live value (it ends early), and its render-mode flag
// stays set, so the fade-in that hides participants and moves them to the
// cut-in layer never reapplies. A cut-in that ended inside the rolled-back
// window cannot be revived: its participants come back from the snapshot still
// hidden on the cut-in layer and nothing ever restores them (uMayoi is a
// participant of Wright's hypers but never the point character).
// ---------------------------------------------------------------------------

static constexpr uint32_t SGAMEUI_CUTIN_COUNT_OFFSET = 0x5678;
static constexpr uint32_t SGAMEUI_CUTIN_DATA_OFFSET = 0x5688;
static constexpr uint32_t UCUTIN_PARTICIPANT_COUNT_OFFSET = 0x48;
static constexpr uint32_t UCUTIN_PARTICIPANT_DATA_OFFSET = 0x58;
static constexpr uint32_t UCUTIN_DESTROY_VFUNC = 0x78;
// Offsets inside RollbackCutinState::state (based at uCutIn+0x750).
static constexpr uint32_t CUTIN_STATE_SAVED_VISIBLE = 0x758 - ROLLBACK_CUTIN_STATE_OFFSET;
static constexpr uint32_t CUTIN_STATE_RENDER_MODE = 0x7A4 - ROLLBACK_CUTIN_STATE_OFFSET;
static constexpr uint32_t MAX_REGISTERED_CUTINS = 16;

static bool IsAliveState(uint64_t object)
{
    return ((*(uint32_t*)(object + 0x10) & 7u) - 1u) < 2u;
}

static bool IsAliveCutin(uint64_t cutin)
{
    return cutin && HasDtiVirtual((void*)cutin) &&
        GetObjectDti(cutin) == (uint64_t)_addr(DTI_UCUTIN) && IsAliveState(cutin);
}

static uint32_t CollectAliveCutins(uint64_t (&out)[MAX_REGISTERED_CUTINS])
{
    uint64_t sGameUi = *(uint64_t*)_addr(PTR_SGAMEUI);
    if (!sGameUi) return 0;
    uint32_t count = *(uint32_t*)(sGameUi + SGAMEUI_CUTIN_COUNT_OFFSET);
    uint64_t data = *(uint64_t*)(sGameUi + SGAMEUI_CUTIN_DATA_OFFSET);
    if (!data || count > 256) return 0;
    uint32_t alive = 0;
    for (uint32_t i = 0; i < count && alive < MAX_REGISTERED_CUTINS; ++i) {
        uint64_t cutin = *(uint64_t*)(data + (uint64_t)i * sizeof(uint64_t));
        if (IsAliveCutin(cutin)) out[alive++] = cutin;
    }
    return alive;
}

static uint32_t ReadParticipants(uint64_t cutin, void* (&out)[ROLLBACK_CUTIN_MAX_PARTICIPANTS])
{
    uint32_t count = *(uint32_t*)(cutin + UCUTIN_PARTICIPANT_COUNT_OFFSET);
    uint64_t data = *(uint64_t*)(cutin + UCUTIN_PARTICIPANT_DATA_OFFSET);
    if (count > ROLLBACK_CUTIN_MAX_PARTICIPANTS || (count && !data)) return 0;
    for (uint32_t i = 0; i < count; ++i)
        out[i] = *(void**)(data + (uint64_t)i * sizeof(void*));
    return count;
}

static bool IsParticipantOfAliveCutin(uint64_t character)
{
    uint64_t cutins[MAX_REGISTERED_CUTINS];
    uint32_t count = CollectAliveCutins(cutins);
    for (uint32_t c = 0; c < count; ++c) {
        void* participants[ROLLBACK_CUTIN_MAX_PARTICIPANTS];
        uint32_t participantCount = ReadParticipants(cutins[c], participants);
        for (uint32_t i = 0; i < participantCount; ++i) {
            if ((uint64_t)participants[i] == character) return true;
        }
    }
    return false;
}

bool Rollback_HasActiveCutin()
{
    uint64_t cutins[MAX_REGISTERED_CUTINS];
    return CollectAliveCutins(cutins) != 0;
}

void Rollback_CaptureCutins(RollbackSnapshotSlot& slot)
{
    slot.cutins.clear();
    uint64_t cutins[MAX_REGISTERED_CUTINS];
    uint32_t count = CollectAliveCutins(cutins);
    for (uint32_t c = 0; c < count; ++c) {
        RollbackCutinState saved = {};
        saved.cutin = (void*)cutins[c];
        saved.vtable = *(uint64_t*)cutins[c];
        std::memcpy(saved.state, (void*)(cutins[c] + ROLLBACK_CUTIN_STATE_OFFSET),
                    sizeof(saved.state));
        saved.participantCount = ReadParticipants(cutins[c], saved.participants);
        slot.cutins.push_back(saved);
    }
}

static const RollbackCutinState* FindSavedCutin(const RollbackSnapshotSlot& slot,
                                                uint64_t cutin)
{
    for (const RollbackCutinState& saved : slot.cutins) {
        if ((uint64_t)saved.cutin != cutin || *(uint64_t*)cutin != saved.vtable)
            continue;
        // The participant list is fixed at spawn; a mismatch means the address
        // was reused by a different cut-in.
        void* participants[ROLLBACK_CUTIN_MAX_PARTICIPANTS];
        uint32_t participantCount = ReadParticipants(cutin, participants);
        if (participantCount != saved.participantCount ||
            std::memcmp(participants, saved.participants,
                        participantCount * sizeof(void*)) != 0) {
            return nullptr;
        }
        return &saved;
    }
    return nullptr;
}

void Rollback_RewindCutins(const RollbackSnapshotSlot& slot)
{
    uint64_t cutins[MAX_REGISTERED_CUTINS];
    uint32_t count = CollectAliveCutins(cutins);
    for (uint32_t c = 0; c < count; ++c) {
        const RollbackCutinState* saved = FindSavedCutin(slot, cutins[c]);
        if (saved) {
            // Timer, duration, saved participant visibility, fades, render-mode
            // flag and motion cache. The uCutInCam (category 4) does not tick
            // in resim, so it is left at its live frame, which the rewound
            // timer reaches again at the end of the replay.
            std::memcpy((void*)(cutins[c] + ROLLBACK_CUTIN_STATE_OFFSET), saved->state,
                        sizeof(saved->state));
            continue;
        }
        // Spawned after the restore frame: the replay spawns it again if the
        // same anmchr command runs. Native destroy restores its participants'
        // render state; the character render-state restore that follows then
        // overwrites them with the snapshot values.
        HookLog("Rollback: killing cut-in 0x%llX spawned after frame %d\n",
                cutins[c], slot.frame);
        auto destroy = (void(__fastcall*)(uint64_t))(
            *(uint64_t*)(*(uint64_t*)cutins[c] + UCUTIN_DESTROY_VFUNC));
        destroy(cutins[c]);
    }
}

// Mirrors uCutIn__setCutinRenderMode(this, 0) for one participant.
static void EndCutinRenderState(uint64_t character, int visible)
{
    using SetStencilRefFn = void(__fastcall*)(void*, int, uint8_t);
    auto setStencilRef = (SetStencilRefFn)_addr(FN_MODEL_SET_CHILD_MATERIAL_MODE);

    if (visible >= 0) {
        uint32_t& flags = *(uint32_t*)(character + 0x10);
        flags = visible ? (flags | MTOBJECT_VISIBLE_FLAG) : (flags & ~MTOBJECT_VISIBLE_FLAG);
    }
    uint32_t& drawLayer = *(uint32_t*)(character + CUNIT_DRAW_LAYER_MASK_OFFSET);
    drawLayer = (drawLayer & ~CUTIN_DRAW_LAYER_BITS) | NORMAL_DRAW_LAYER_BIT;
    const uint32_t layer = drawLayer;
    setStencilRef((void*)character, -1, 3);

    uint32_t weaponCount = *(uint32_t*)(character + CCHR_WEAPON_COUNT_OFFSET);
    uint64_t weapons = *(uint64_t*)(character + CCHR_WEAPON_ARRAY_OFFSET);
    if (!weapons || weaponCount > 256) return;
    for (uint32_t i = 0; i < weaponCount; ++i) {
        uint64_t weapon = *(uint64_t*)(weapons + (uint64_t)i * sizeof(uint64_t));
        if (!weapon) continue;
        setStencilRef((void*)weapon, -1, 3);
        *(uint32_t*)(weapon + CUNIT_DRAW_LAYER_MASK_OFFSET) = layer;
    }
}

// Visibility a dead snapshot cut-in saved for this participant at fade-in, or
// -1 when no snapshot cut-in hid it.
static int SavedCutinVisibility(const RollbackSnapshotSlot* slot, uint64_t character)
{
    if (!slot) return -1;
    for (const RollbackCutinState& saved : slot->cutins) {
        if (!saved.state[CUTIN_STATE_RENDER_MODE]) continue;
        for (uint32_t i = 0; i < saved.participantCount; ++i) {
            if ((uint64_t)saved.participants[i] != character) continue;
            uint32_t value;
            std::memcpy(&value, saved.state + CUTIN_STATE_SAVED_VISIBLE + i * 4, sizeof(value));
            return value == 1 ? 1 : 0;
        }
    }
    return -1;
}

// End the cut-in render state of every team character still on a cut-in layer
// that no alive cut-in owns.
static void RepairOrphanedCutinParticipants(const RollbackSnapshotSlot* slot,
                                            bool forceVisibleWithoutRecord)
{
    uint64_t sCharacter = *(uint64_t*)_addr(PTR_SCHARACTER);
    if (!sCharacter) return;
    for (int teamIndex = 0; teamIndex < 2; ++teamIndex) {
        uint64_t node = sCharacter + (teamIndex == 0 ? TEAM0_OFFSET : TEAM1_OFFSET) +
            TEAM_FIRST_NODE_OFFSET;
        for (int member = 0; node && member < ROLLBACK_MAX_TEAM_NODES;
             ++member, node = *(uint64_t*)(node + NODE_NEXT_OFFSET)) {
            uint64_t character = *(uint64_t*)(node + NODE_CHR_OFFSET);
            if (!character || !IsAliveState(character)) continue;
            if ((*(uint32_t*)(character + CUNIT_DRAW_LAYER_MASK_OFFSET) &
                 CUTIN_DRAW_LAYER_BITS) == 0) {
                continue;
            }
            if (IsParticipantOfAliveCutin(character)) continue;
            int visible = SavedCutinVisibility(slot, character);
            if (visible < 0 && forceVisibleWithoutRecord) visible = 1;
            HookLog("Rollback: ended orphaned cut-in render state chr=0x%llX visible=%d\n",
                    character, visible);
            EndCutinRenderState(character, visible);
        }
    }
}

void Rollback_RepairCutinRenderState(const RollbackSnapshotSlot& slot)
{
    RepairOrphanedCutinParticipants(&slot, true);
}

void Rollback_SyncActiveCharacterVisibility()
{
    // Replayed cut-ins end through native destroy, so this only catches state
    // the restore pass could not attribute. Visibility of non-point members is
    // left alone: without a saved value it cannot be known.
    RepairOrphanedCutinParticipants(nullptr, false);

    uint64_t sCharacter = *(uint64_t*)_addr(PTR_SCHARACTER);
    if (!sCharacter) return;

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

    // A rollback can begin after cChrTeam::active changed but before the model
    // callback made the incoming point visible. Normalize both point characters
    // unless an alive cut-in owns their render state.
    for (int teamIndex = 0; teamIndex < 2; ++teamIndex) {
        uint64_t team = sCharacter +
            (teamIndex == 0 ? TEAM0_OFFSET : TEAM1_OFFSET);
        uint64_t character = *(uint64_t*)(team + 0x48);
        if (!character) continue;
        if (IsParticipantOfAliveCutin(character)) continue;

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
