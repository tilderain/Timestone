// In-match roster selector. All selector protocol, UI, preload, and native
// roster mutation lives here; netplay only transports opaque extension packets.
#include "../pch.h"
#include "../imgui/imgui.h"
#include "../kiero/minhook/include/MinHook.h"
#include "LaunchMenu.h"
#include "assist_names.h"
#include "framestepper.h"
#include "mk10utils.h"
#include "netplay.h"
#include "trainingboot.h"
#include <Windows.h>
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>

extern void HookLog(const char* fmt, ...);
extern "C" int32_t Controller_GetNetFrameCount();

namespace {

constexpr uint32_t CHRSEL_MAGIC = 0x31534C43; // "CLS1"

enum ChrSelAction : uint32_t {
    CHRSEL_OPEN = 1,
    CHRSEL_UPDATE = 2,
    CHRSEL_CANCEL = 3,
    CHRSEL_COMMIT = 4,
};

struct RosterConfig {
    int32_t chars[2][3];
    int32_t colors[2][3];
    int32_t assists[2][3];
};

#pragma pack(push, 1)
struct ChrSelPacket {
    uint32_t type;
    uint32_t magic;
    uint32_t action;
    uint32_t generation;
    uint32_t session;
    uint32_t revision;
    uint32_t readyMask;
    int32_t targetFrame;
    uint32_t roundSeed;
    RosterConfig roster;
};
#pragma pack(pop)

static_assert(sizeof(ChrSelPacket) <= 256);

bool g_open = false;
bool g_pendingRoster = false;
bool g_archiveRequestsPending = false;
uint32_t g_session = 0;
uint32_t g_revision = 0;
uint32_t g_remoteRevision = 0;
int32_t g_pendingTargetFrame = -1;
RosterConfig g_roster = {};
RosterConfig g_pendingConfig = {};
char g_status[160] = {};

constexpr uintptr_t BATTLE_SETTING_GET_INSTANCE = 0x140004700;
constexpr uintptr_t CHARACTER_GET_INSTANCE = 0x140001AA0;
constexpr uintptr_t GAME_UI_GET_INSTANCE = 0x140001AC0;
constexpr uintptr_t SLOT_SET_CHAR_ID = 0x140146560;
constexpr uintptr_t SLOT_SET_DRAW_MODE = 0x1401461E0;
constexpr uintptr_t SLOT_SET_ASSIST = 0x140024D80;
constexpr uintptr_t CHARACTER_REQUEST_ARCHIVES = 0x140059600;
constexpr uintptr_t CHARACTER_RELEASE_ARCHIVES = 0x140059BC0;
constexpr uintptr_t CHARACTER_ARCHIVES_READY = 0x1400592F0;
constexpr uintptr_t CHARACTER_ARCHIVE_LOAD_STATE = 0x140058F40;
constexpr uintptr_t CHARACTER_UPDATE_ARCHIVES = 0x14001F2E0;
constexpr uintptr_t CHARACTER_COLLECT_TEAM_SOUND_SLOTS = 0x140059180;
constexpr uintptr_t CHARACTER_REMAP_TEAM_SOUND = 0x140059210;
constexpr uintptr_t GAME_CONFIG_GET_INSTANCE = 0x140001AB0;
constexpr uintptr_t GAME_CONFIG_GET_LANGUAGE = 0x140004840;
constexpr uintptr_t MAIN_GET_INSTANCE = 0x140003150;
constexpr uintptr_t MAIN_IS_PAUSED_THIS_FRAME = 0x140006B50;
constexpr uintptr_t STAGE_GET_INSTANCE = 0x1400047E0;
constexpr uintptr_t STAGE_HAS_LIVE_BODY = 0x1401F0950;
constexpr uintptr_t GAME_UI_SET_CHAR = 0x14024E160;
constexpr uintptr_t GAME_UI_SET_COLOR = 0x14024E120;
constexpr uintptr_t GAME_UI_SET_ASSIST = 0x14024E100;
constexpr uintptr_t GAME_UI_IS_CHAR_VARIANT_SELECTABLE = 0x1402BC990;
constexpr uintptr_t ACTION_GET_INSTANCE = 0x140001A90;
constexpr uintptr_t ACTION_OR_GAME_FLAG = 0x14000ADD0;

using ArchivesReadyFn = uint64_t(__fastcall*)(uint64_t, int32_t, uint32_t,
                                               uint32_t, int32_t, int32_t);
ArchivesReadyFn g_originalArchivesReady = nullptr;
bool g_archiveReadyHookAttempted = false;
bool g_resetDiagnosticsActive = false;

uint64_t __fastcall HookArchivesReady(uint64_t characters, int32_t character,
                                      uint32_t color, uint32_t soundVariant,
                                      int32_t teamSoundA, int32_t teamSoundB)
{
    // Native aggregate readiness treats a missing cache record as ready. Keep
    // reset step 3 waiting until every replacement request owns a cache slot.
    if (g_archiveRequestsPending)
        return 0;
    return g_originalArchivesReady(characters, character, color, soundVariant,
                                   teamSoundA, teamSoundB);
}

void EnsureArchiveReadyHook()
{
    if (g_archiveReadyHookAttempted)
        return;
    g_archiveReadyHookAttempted = true;
    MH_STATUS status = MH_Initialize();
    if (status != MH_OK && status != MH_ERROR_ALREADY_INITIALIZED) {
        HookLog("ChrSelImGui: failed to initialize archive readiness hook (%d)\n", status);
        return;
    }
    status = MH_CreateHook((void*)_addr(CHARACTER_ARCHIVES_READY),
                           (void*)&HookArchivesReady,
                           (void**)&g_originalArchivesReady);
    if (status != MH_OK) {
        HookLog("ChrSelImGui: failed to create archive readiness hook (%d)\n", status);
        return;
    }
    status = MH_EnableHook((void*)_addr(CHARACTER_ARCHIVES_READY));
    if (status != MH_OK && status != MH_ERROR_ENABLED) {
        HookLog("ChrSelImGui: failed to enable archive readiness hook (%d)\n", status);
        return;
    }
    HookLog("ChrSelImGui: installed replacement archive readiness gate\n");
}

void LogResetState()
{
    if (!g_resetDiagnosticsActive)
        return;

    using GetInstanceFn = uint64_t(__fastcall*)();
    using LoadStateFn = uint32_t(__fastcall*)(uint64_t, uint32_t, uint32_t, uint32_t);
    using StageReadyFn = bool(__fastcall*)(uint64_t);
    const uint64_t action = ((GetInstanceFn)_addr(ACTION_GET_INSTANCE))();
    if (!action)
        return;

    const int32_t step = *(int32_t*)(action + 0xD8);
    const int32_t order = *(int32_t*)(action + 0xDC);
    const int32_t stepFrame = *(int32_t*)(action + 0xF0);
    bool paused = false;
    if (step == 12 && stepFrame == 3) {
        const uint64_t main = ((GetInstanceFn)_addr(MAIN_GET_INSTANCE))();
        using IsPausedFn = bool(__fastcall*)(uint64_t);
        paused = main && ((IsPausedFn)_addr(MAIN_IS_PAUSED_THIS_FRAME))(main);
        const uint64_t characters = ((GetInstanceFn)_addr(CHARACTER_GET_INSTANCE))();
        if (characters) {
            // sCharacter::move is not dispatched while the training-reset
            // state machine waits here, so advance its archive table directly.
            using UpdateArchivesFn = void(__fastcall*)(uint64_t);
            ((UpdateArchivesFn)_addr(CHARACTER_UPDATE_ARCHIVES))(characters + 0x600);
        }
    }
    static int32_t lastStep = -1;
    static int32_t lastStepFrame = -1;
    static uint32_t unchangedFrames = 0;
    const bool changed = step != lastStep || stepFrame != lastStepFrame;
    if (changed || ++unchangedFrames >= 120) {
        unchangedFrames = 0;
        HookLog("ChrSelImGui: reset state step=%d order=%d subframe=%d restartMode=%d flags=0x%08X paused=%d\n",
                step, order, stepFrame, *(int32_t*)(action + 0x118),
                *(uint32_t*)(action + 0xC4), paused);
    }
    lastStep = step;
    lastStepFrame = stepFrame;

    if (step == 12 && stepFrame == 3 && unchangedFrames == 0) {
        const uint64_t characters = ((GetInstanceFn)_addr(CHARACTER_GET_INSTANCE))();
        const uint64_t stage = ((GetInstanceFn)_addr(STAGE_GET_INSTANCE))();
        const bool archivesReady = characters && g_originalArchivesReady &&
            g_originalArchivesReady(characters, -1, 0, 0, 0, 0);
        const bool stageReady = stage &&
            ((StageReadyFn)_addr(STAGE_HAS_LIVE_BODY))(stage);
        HookLog("ChrSelImGui: reset wait stageReady=%d archivesReady=%d rebuildArcOrder=%d pendingRequests=%d\n",
                stageReady, archivesReady,
                characters ? *(int32_t*)(characters + 0x1210) : -999,
                g_archiveRequestsPending);
        if (characters) {
            LoadStateFn loadState = (LoadStateFn)_addr(CHARACTER_ARCHIVE_LOAD_STATE);
            for (int team = 0; team < 2; ++team) {
                for (int member = 0; member < 3; ++member) {
                    const int character = g_pendingConfig.chars[team][member];
                    const int color = g_pendingConfig.colors[team][member];
                    HookLog("ChrSelImGui: archive state team=%d member=%d char=%d c1=%u c2=%u c3=%u c4=%u\n",
                            team, member, character,
                            loadState(characters, 1, character, color),
                            loadState(characters, 2, character, color),
                            loadState(characters, 3, character, color),
                            loadState(characters, 4, character, 1));
                }
            }
        }
    }

    if (step == 5 && !FrameStepper_IsMatchRestartPending()) {
        g_resetDiagnosticsActive = false;
        HookLog("ChrSelImGui: roster reset returned to fighting\n");
    }
}

int VariantCount(int characterId)
{
    if (characterId == 51) return 1;
    if (characterId == 3 || characterId == 5 ||
        characterId == 35 || characterId == 39) return 9;
    return characterId > 0 && characterId < kCharNameCount ? 8 : 1;
}

bool IsColorSelectable(int characterId, int color)
{
    if (characterId <= 0 || characterId >= kCharNameCount || color < 0)
        return false;

    using GetInstanceFn = uint64_t(__fastcall*)();
    using IsSelectableFn = bool(__fastcall*)(uint64_t, uint32_t, int32_t);
    const uint64_t gameUi = ((GetInstanceFn)_addr(GAME_UI_GET_INSTANCE))();
    return gameUi && ((IsSelectableFn)_addr(GAME_UI_IS_CHAR_VARIANT_SELECTABLE))(
        gameUi, static_cast<uint32_t>(characterId), color);
}

int NormalizeColor(int characterId, int color)
{
    if (IsColorSelectable(characterId, color))
        return color;

    for (int candidate = 0; candidate < VariantCount(characterId); ++candidate) {
        if (IsColorSelectable(characterId, candidate))
            return candidate;
    }
    return 0;
}

bool ValidateRoster(const RosterConfig& roster)
{
    for (int team = 0; team < 2; ++team) {
        for (int member = 0; member < 3; ++member) {
            const int character = roster.chars[team][member];
            if (character <= 0 || character >= kCharNameCount ||
                roster.colors[team][member] < 0 ||
                roster.colors[team][member] >= VariantCount(character) ||
                !IsColorSelectable(character, roster.colors[team][member]) ||
                roster.assists[team][member] < 0 ||
                roster.assists[team][member] > 2)
                return false;
        }
    }
    return true;
}

uint64_t GetBattleSetting()
{
    using GetInstanceFn = uint64_t(__fastcall*)();
    return ((GetInstanceFn)_addr(BATTLE_SETTING_GET_INSTANCE))();
}

bool ReadCurrentRoster(RosterConfig& roster)
{
    const uint64_t battleSetting = GetBattleSetting();
    if (!battleSetting)
        return false;

    int counts[2] = {};
    for (int index = 0; index < 6; ++index) {
        const uint64_t slot = battleSetting + 0x38 + index * 0x58;
        int team = *(int32_t*)(slot + 0x08);
        const int character = *(int32_t*)(slot + 0x0C);
        HookLog("ChrSelImGui: live slot[%d] team=%d char=%d body=%d assist=%d\n",
                index, team, character, *(int32_t*)(slot + 0x14),
                *(int32_t*)(slot + 0x1C));
        if (team < 0 || team > 1)
            team = index / 3;
        if (character <= 0 || character >= kCharNameCount || counts[team] >= 3)
            continue;
        const int member = counts[team]++;
        roster.chars[team][member] = character;
        roster.colors[team][member] = NormalizeColor(
            character, std::clamp(*(int32_t*)(slot + 0x14),
                                  0, VariantCount(character) - 1));
        roster.assists[team][member] =
            std::clamp(*(int32_t*)(slot + 0x1C), 0, 2);
    }
    return counts[0] == 3 && counts[1] == 3 && ValidateRoster(roster);
}

void ReadFallbackRoster(RosterConfig& roster)
{
    const LaunchDialogConfig& config = GetLaunchDialogConfig();
    for (int member = 0; member < 3; ++member) {
        roster.chars[0][member] = config.p1Chars[member];
        roster.chars[1][member] = config.p2Chars[member];
        roster.colors[0][member] = config.p1Colors[member];
        roster.colors[1][member] = config.p2Colors[member];
        roster.assists[0][member] = config.p1Assists[member];
        roster.assists[1][member] = config.p2Assists[member];
    }
}

void NormalizeRoster(RosterConfig& roster)
{
    const TrainingBootConfig& defaults = TrainingBoot_GetDefaultConfig();
    for (int team = 0; team < 2; ++team) {
        for (int member = 0; member < 3; ++member) {
            int& character = roster.chars[team][member];
            if (character <= 0 || character >= kCharNameCount) {
                character = team == 0
                    ? defaults.p1Chars[member] : defaults.p2Chars[member];
            }
            if (character <= 0 || character >= kCharNameCount)
                character = CHR_RYU;
            roster.colors[team][member] = NormalizeColor(
                character, std::clamp(roster.colors[team][member],
                                     0, VariantCount(character) - 1));
            roster.assists[team][member] = std::clamp(
                roster.assists[team][member], 0, 2);
        }
    }
}

bool CanOpenSelector()
{
    if (GetLaunchOption() != LAUNCH_TRAINING &&
        GetLaunchOption() != LAUNCH_DUAL_TRAINING)
        return false;
    if (g_pendingRoster || FrameStepper_IsMatchRestartPending())
        return false;
    using GetInstanceFn = uint64_t(__fastcall*)();
    const uint64_t action = ((GetInstanceFn)_addr(ACTION_GET_INSTANCE))();
    if (!action || *(int32_t*)(action + 0xD8) != 5)
        return false;
    return !Netplay_IsActive() || FrameStepper_IsBattleSynchronized();
}

void SetStatus(const char* text)
{
    std::snprintf(g_status, sizeof(g_status), "%s", text ? text : "");
}

ChrSelPacket MakePacket(ChrSelAction action)
{
    ChrSelPacket packet = {};
    packet.type = PACKET_EXTENSION;
    packet.magic = CHRSEL_MAGIC;
    packet.action = action;
    packet.generation = FrameStepper_GetBattleGeneration();
    packet.session = g_session;
    packet.revision = g_revision;
    packet.readyMask = 0;
    packet.targetFrame = g_pendingTargetFrame;
    packet.roster = g_roster;
    return packet;
}

bool SendPacket(const ChrSelPacket& packet)
{
    if (!Netplay_IsActive())
        return true;
    if (Netplay_SendExtensionPacket(&packet, sizeof(packet)))
        return true;
    SetStatus("Network control queue is full; try again");
    return false;
}

void BeginSynchronizedSession()
{
    RosterConfig current = {};
    if (!CanOpenSelector()) {
        HookLog("ChrSelImGui: F8 open rejected (not synchronized fighting or restart pending)\n");
        SetStatus("Roster selector is only available during synchronized fighting");
        return;
    }
    if (!ReadCurrentRoster(current)) {
        ReadFallbackRoster(current);
        HookLog("ChrSelImGui: live roster read failed; using launch configuration\n");
    }
    NormalizeRoster(current);
    if (!ValidateRoster(current)) {
        HookLog("ChrSelImGui: roster normalization failed unexpectedly\n");
        return;
    }
    g_roster = current;
    ++g_session;
    if (g_session == 0) ++g_session;
    g_revision = 1;
    g_remoteRevision = 0;
    g_open = true;
    SetStatus("Either player can commit the synchronized roster");
    SendPacket(MakePacket(CHRSEL_OPEN));
}

void SendLocalUpdate()
{
    if (!g_open || !ValidateRoster(g_roster))
        return;
    ++g_revision;
    SendPacket(MakePacket(CHRSEL_UPDATE));
}

void CopyRemoteOwnedTeam(const ChrSelPacket& packet)
{
    const int remoteTeam = Netplay_IsHost() ? 1 : 0;
    for (int member = 0; member < 3; ++member) {
        g_roster.chars[remoteTeam][member] = packet.roster.chars[remoteTeam][member];
        g_roster.colors[remoteTeam][member] = packet.roster.colors[remoteTeam][member];
        g_roster.assists[remoteTeam][member] = packet.roster.assists[remoteTeam][member];
    }
}

bool RequestRosterArchives(const RosterConfig& roster, bool includeTeamSounds)
{
    using GetInstanceFn = uint64_t(__fastcall*)();
    using RequestFn = bool(__fastcall*)(uint64_t, uint32_t, uint32_t,
                                         uint32_t, int32_t, int32_t, uint32_t);
    using CollectTeamSoundsFn = void(__fastcall*)(int32_t, int32_t,
                                                   int32_t*, int32_t*);
    using RemapTeamSoundFn = int32_t(__fastcall*)(int32_t, int32_t);
    using GetLanguageFn = int32_t(__fastcall*)(uint64_t);
    const uint64_t characters = ((GetInstanceFn)_addr(CHARACTER_GET_INSTANCE))();
    if (!characters)
        return false;
    RequestFn request = (RequestFn)_addr(CHARACTER_REQUEST_ARCHIVES);
    CollectTeamSoundsFn collectTeamSounds =
        (CollectTeamSoundsFn)_addr(CHARACTER_COLLECT_TEAM_SOUND_SLOTS);
    RemapTeamSoundFn remapTeamSound =
        (RemapTeamSoundFn)_addr(CHARACTER_REMAP_TEAM_SOUND);
    const uint64_t gameConfig = ((GetInstanceFn)_addr(GAME_CONFIG_GET_INSTANCE))();
    const int32_t language = gameConfig
        ? ((GetLanguageFn)_addr(GAME_CONFIG_GET_LANGUAGE))(gameConfig) : 0;
    bool accepted = true;
    for (int team = 0; team < 2; ++team) {
        for (int member = 0; member < 3; ++member) {
            int32_t teamSoundA = 0;
            int32_t teamSoundB = 0;
            if (includeTeamSounds) {
                int32_t otherA = 0;
                int32_t otherB = 0;
                collectTeamSounds(team, member, &otherA, &otherB);
                teamSoundA = remapTeamSound(otherA, language);
                teamSoundB = remapTeamSound(otherB, language);
            }
            if (!request(characters, roster.chars[team][member],
                         roster.colors[team][member], 1,
                         teamSoundA, teamSoundB, 0x22))
                accepted = false;
        }
    }
    return accepted;
}

bool AcceptCommit(const ChrSelPacket& packet)
{
    if (!ValidateRoster(packet.roster) || packet.targetFrame < 0)
        return false;
    if (Netplay_IsActive() &&
        packet.targetFrame < Controller_GetNetFrameCount() + 3)
        return false;

    g_pendingConfig = packet.roster;
    g_pendingTargetFrame = packet.targetFrame;
    g_pendingRoster = true;
    // The active battle slots still describe the old team here, so preload
    // character-local archives now and pairwise team audio after the barrier.
    g_archiveRequestsPending = !RequestRosterArchives(g_pendingConfig, false);
    if (!g_archiveRequestsPending)
        HookLog("ChrSelImGui: replacement archive requests accepted before barrier\n");
    if (!FrameStepper_ScheduleRosterRestart(packet.targetFrame, packet.roundSeed)) {
        g_pendingRoster = false;
        g_archiveRequestsPending = false;
        g_pendingTargetFrame = -1;
        return false;
    }
    g_open = false;
    SetStatus("Roster committed; waiting for synchronized reset");
    HookLog("ChrSelImGui: roster restart scheduled frame=%d session=%u\n",
            packet.targetFrame, packet.session);
    return true;
}

void HandlePacket(const ChrSelPacket& packet)
{
    if (packet.magic != CHRSEL_MAGIC ||
        packet.generation != FrameStepper_GetBattleGeneration())
        return;

    if (packet.action == CHRSEL_OPEN) {
        if (!ValidateRoster(packet.roster) || !CanOpenSelector())
            return;
        if (!g_open || packet.session != g_session) {
            g_session = packet.session;
            g_revision = 1;
            g_remoteRevision = packet.revision;
            g_roster = packet.roster;
            g_open = true;
        } else {
            CopyRemoteOwnedTeam(packet);
            g_remoteRevision = std::max(g_remoteRevision, packet.revision);
        }
        SetStatus(Netplay_IsHost()
            ? "Select P1; either player can commit"
            : "Select P2; either player can commit");
        SendLocalUpdate();
        return;
    }
    if (packet.session != g_session)
        return;
    if (packet.action == CHRSEL_UPDATE) {
        if (!g_open || packet.revision <= g_remoteRevision ||
            !ValidateRoster(packet.roster))
            return;
        g_remoteRevision = packet.revision;
        CopyRemoteOwnedTeam(packet);
        return;
    }
    if (packet.action == CHRSEL_CANCEL) {
        g_open = false;
        SetStatus("Roster change cancelled by peer");
        return;
    }
    if (packet.action == CHRSEL_COMMIT) {
        if (!AcceptCommit(packet))
            SetStatus("Rejected stale or invalid roster commit");
    }
}

void RequestOpen()
{
    if (!CanOpenSelector()) {
        SetStatus("Roster selector is only available during synchronized fighting");
        return;
    }
    BeginSynchronizedSession();
}

void CancelSelection()
{
    if (Netplay_IsActive())
        SendPacket(MakePacket(CHRSEL_CANCEL));
    g_open = false;
    SetStatus("Roster change cancelled");
}

void CommitSelection()
{
    ChrSelPacket packet = MakePacket(CHRSEL_COMMIT);
    const int32_t currentFrame = Controller_GetNetFrameCount();
    int32_t lead = 45;
    if (Netplay_IsActive())
        lead = std::max(lead, (int32_t)(Netplay_GetRTT() * 60 / 1000) + 15);
    packet.targetFrame = Netplay_IsActive() ? currentFrame + lead : currentFrame;
    uint64_t replay = *(uint64_t*)_addr(0x140D4D9D8);
    packet.roundSeed = replay ? *(uint32_t*)(replay + 0x58) : 0x13579BDF;
    g_pendingTargetFrame = packet.targetFrame;

    if (!AcceptCommit(packet)) {
        SetStatus("A restart is already pending");
        return;
    }
    if (Netplay_IsActive())
        SendPacket(packet);
}

bool DrawTeam(int team, bool editable)
{
    bool changed = false;
    ImGui::PushID(team);
    ImGui::TextColored(team == 0 ? ImVec4(0.35f, 0.75f, 1.0f, 1.0f)
                                 : ImVec4(1.0f, 0.45f, 0.55f, 1.0f),
                       "P%d TEAM", team + 1);
    if (!editable) ImGui::BeginDisabled();
    for (int member = 0; member < 3; ++member) {
        ImGui::PushID(member);
        ImGui::Separator();
        ImGui::TextUnformatted(member == 0 ? "Point" : member == 1 ? "Second" : "Anchor");

        int selected = g_roster.chars[team][member];
        if (ImGui::BeginCombo("Character", kCharNames[selected])) {
            if (ImGui::IsKeyPressed(ImGui::GetKeyIndex(ImGuiKey_UpArrow), true)) {
                selected = selected > 1 ? selected - 1 : kCharNameCount - 1;
                changed = true;
            }
            if (ImGui::IsKeyPressed(ImGui::GetKeyIndex(ImGuiKey_DownArrow), true)) {
                selected = selected + 1 < kCharNameCount ? selected + 1 : 1;
                changed = true;
            }
            for (int character = 1; character < kCharNameCount; ++character) {
                const bool isSelected = character == selected;
                if (ImGui::Selectable(kCharNames[character], isSelected)) {
                    selected = character;
                    changed = true;
                }
                if (isSelected)
                    ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
        if (selected != g_roster.chars[team][member]) {
            g_roster.chars[team][member] = selected;
            g_roster.colors[team][member] = NormalizeColor(
                selected, g_roster.colors[team][member]);
        }

        const int variantCount = VariantCount(g_roster.chars[team][member]);
        char colorPreview[24];
        std::snprintf(colorPreview, sizeof(colorPreview), "Color %d",
                      g_roster.colors[team][member] + 1);
        if (ImGui::BeginCombo("Color", colorPreview)) {
            for (int color = 0; color < variantCount; ++color) {
                if (!IsColorSelectable(g_roster.chars[team][member], color))
                    continue;
                char label[24];
                std::snprintf(label, sizeof(label), "Color %d", color + 1);
                if (ImGui::Selectable(label,
                        color == g_roster.colors[team][member])) {
                    g_roster.colors[team][member] = color;
                    changed = true;
                }
            }
            ImGui::EndCombo();
        }

        const int character = g_roster.chars[team][member];
        const char* assistPreview = kAssistMoveNames[character].names[
            g_roster.assists[team][member]];
        if (ImGui::BeginCombo("Assist", assistPreview)) {
            for (int assist = 0; assist < 3; ++assist) {
                if (ImGui::Selectable(kAssistMoveNames[character].names[assist],
                        assist == g_roster.assists[team][member])) {
                    g_roster.assists[team][member] = assist;
                    changed = true;
                }
            }
            ImGui::EndCombo();
        }
        ImGui::PopID();
    }
    if (!editable) ImGui::EndDisabled();
    ImGui::PopID();
    return changed;
}

void WriteRosterAtBarrier()
{
    using GetInstanceFn = uint64_t(__fastcall*)();
    using SlotSetterFn = void(__fastcall*)(uint64_t, int32_t);
    using ReleaseFn = void(__fastcall*)(uint64_t, int32_t, uint32_t,
                                         uint32_t, int32_t, int32_t);
    using CollectTeamSoundsFn = void(__fastcall*)(int32_t, int32_t,
                                                   int32_t*, int32_t*);
    using RemapTeamSoundFn = int32_t(__fastcall*)(int32_t, int32_t);
    using GetLanguageFn = int32_t(__fastcall*)(uint64_t);
    using UiSetterFn = void(__fastcall*)(uint64_t, int32_t, int32_t, int32_t);
    using OrGameFlagFn = void(__fastcall*)(uint64_t, uint32_t);

    const uint64_t battleSetting = GetBattleSetting();
    const uint64_t characters = ((GetInstanceFn)_addr(CHARACTER_GET_INSTANCE))();
    const uint64_t gameUi = ((GetInstanceFn)_addr(GAME_UI_GET_INSTANCE))();
    if (!battleSetting || !characters)
        return;

    const uint64_t gameConfig = ((GetInstanceFn)_addr(GAME_CONFIG_GET_INSTANCE))();
    const int32_t language = gameConfig
        ? ((GetLanguageFn)_addr(GAME_CONFIG_GET_LANGUAGE))(gameConfig) : 0;
    CollectTeamSoundsFn collectTeamSounds =
        (CollectTeamSoundsFn)_addr(CHARACTER_COLLECT_TEAM_SOUND_SLOTS);
    RemapTeamSoundFn remapTeamSound =
        (RemapTeamSoundFn)_addr(CHARACTER_REMAP_TEAM_SOUND);
    ReleaseFn release = (ReleaseFn)_addr(CHARACTER_RELEASE_ARCHIVES);
    for (int index = 0; index < 6; ++index) {
        const uint64_t slot = battleSetting + 0x38 + index * 0x58;
        const int oldCharacter = *(int32_t*)(slot + 0x0C);
        if (oldCharacter <= 0 || oldCharacter >= kCharNameCount)
            continue;
        int32_t otherA = 0;
        int32_t otherB = 0;
        collectTeamSounds(index / 3, index % 3, &otherA, &otherB);
        release(characters, oldCharacter,
                *(uint32_t*)(slot + 0x14), *(uint32_t*)(slot + 0x44),
                remapTeamSound(otherA, language),
                remapTeamSound(otherB, language));
    }

    SlotSetterFn setCharacter = (SlotSetterFn)_addr(SLOT_SET_CHAR_ID);
    SlotSetterFn setColor = (SlotSetterFn)_addr(SLOT_SET_DRAW_MODE);
    SlotSetterFn setAssist = (SlotSetterFn)_addr(SLOT_SET_ASSIST);
    UiSetterFn setUiCharacter = (UiSetterFn)_addr(GAME_UI_SET_CHAR);
    UiSetterFn setUiColor = (UiSetterFn)_addr(GAME_UI_SET_COLOR);
    UiSetterFn setUiAssist = (UiSetterFn)_addr(GAME_UI_SET_ASSIST);

    for (int team = 0; team < 2; ++team) {
        for (int member = 0; member < 3; ++member) {
            const int index = team * 3 + member;
            const int character = g_pendingConfig.chars[team][member];
            const int color = g_pendingConfig.colors[team][member];
            const int assist = g_pendingConfig.assists[team][member];
            for (uintptr_t base : {uintptr_t(0x38), uintptr_t(0x378)}) {
                const uint64_t slot = battleSetting + base + index * 0x58;
                *(int32_t*)(slot + 0x08) = team;
                setCharacter(slot, character);
                setColor(slot, color);
                setAssist(slot, assist);
                // uCharacter__setup_base reads this as its character-sound
                // selector. Retaining the outgoing value mixes paths such as
                // Chris/chr_cc_jp with Dante's dan.srqr.
                *(int32_t*)(slot + 0x30) = 1;
                // Native archive requests read slot +0x44 for the category-4
                // character sound variant. Normal initialized slots use 1.
                *(int32_t*)(slot + 0x44) = 1;
            }
            if (gameUi) {
                setUiCharacter(gameUi, team, member, character);
                setUiColor(gameUi, team, member, color);
                setUiAssist(gameUi, team, member, assist);
            }
            HookLog("ChrSelImGui: barrier slot[%d] team=%d char=%d color=%d assist=%d\n",
                    index, team, character, color, assist);
        }
    }

    // Cache entries released above remain occupied until sCharacter::move
    // retires them. Keep retrying from ChrSelImGui_Update while reset advances.
    g_archiveRequestsPending = !RequestRosterArchives(g_pendingConfig, true);
    if (g_archiveRequestsPending)
        HookLog("ChrSelImGui: archive cache busy at barrier; retrying during reset\n");

    const uint64_t action = ((GetInstanceFn)_addr(ACTION_GET_INSTANCE))();
    if (action) {
        ((OrGameFlagFn)_addr(ACTION_OR_GAME_FLAG))(action, 0x3000);
        HookLog("ChrSelImGui: armed both team rebuild flags actionFlags=0x%08X\n",
                *(uint32_t*)(action + 0xC4));
    } else {
        HookLog("ChrSelImGui: WARNING sAction unavailable while arming team rebuild\n");
    }
}

} // namespace

void ChrSelImGui_Update()
{
    EnsureArchiveReadyHook();
    LogResetState();

    if (g_archiveRequestsPending) {
        static uint32_t retryFrames = 0;
        if (RequestRosterArchives(g_pendingConfig, true)) {
            g_archiveRequestsPending = false;
            retryFrames = 0;
            HookLog("ChrSelImGui: all replacement archive requests accepted\n");
        } else if (++retryFrames % 120 == 0) {
            HookLog("ChrSelImGui: replacement archives still waiting for cache slots\n");
        }
    }

    static bool previousF8 = false;
    const bool f8 = (GetAsyncKeyState(VK_F8) & 0x8000) != 0;
    if (f8 && !previousF8 && !g_open)
        RequestOpen();
    previousF8 = f8;

    ChrSelPacket packet = {};
    size_t packetSize = 0;
    while (Netplay_GetExtensionPacket(&packet, sizeof(packet), packetSize)) {
        if (packetSize == sizeof(packet))
            HandlePacket(packet);
    }
}

void ChrSelImGui_Draw()
{
    if (!g_open)
        return;

    const ImVec2 display = ImGui::GetIO().DisplaySize;
    ImGui::SetNextWindowPos(ImVec2(display.x * 0.5f, display.y * 0.5f),
                            ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(std::min(900.0f, display.x - 40.0f),
                                    std::min(650.0f, display.y - 40.0f)),
                             ImGuiCond_Always);
    ImGui::Begin("Change Roster", nullptr,
                 ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings);
    ImGui::Separator();

    const int ownedTeam = !Netplay_IsActive() ? -1 : Netplay_IsHost() ? 0 : 1;
    bool changed = false;
    if (ImGui::BeginTable("##RosterTeams", 2,
                          ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingStretchSame)) {
        for (int team = 0; team < 2; ++team) {
            ImGui::TableNextColumn();
            changed |= DrawTeam(team, ownedTeam < 0 || ownedTeam == team);
        }
        ImGui::EndTable();
    }

    if (changed) {
        SendLocalUpdate();
    }

    ImGui::Separator();
    if (ImGui::Button(Netplay_IsActive() ? "Apply Roster" : "Apply Roster")) {
        CommitSelection();
    }

    ImGui::SameLine();
    if (ImGui::Button("Cancel"))
        CancelSelection();
    if (g_status[0])
        ImGui::TextWrapped("%s", g_status);
    ImGui::End();
}

bool ChrSelImGui_IsOpen()
{
    return g_open;
}

bool ChrSelImGui_Open()
{
    if (g_open)
        return true;
    if (!CanOpenSelector()) {
        SetStatus("Roster selector is only available during synchronized fighting");
        return false;
    }
    BeginSynchronizedSession();
    return g_open;
}

bool ChrSelImGui_ApplyPendingRoster(int32_t targetFrame)
{
    if (!g_pendingRoster)
        return true;
    if (targetFrame != g_pendingTargetFrame || !ValidateRoster(g_pendingConfig))
        return false;

    WriteRosterAtBarrier();
    g_resetDiagnosticsActive = true;
    HookLog("ChrSelImGui: applied roster at synchronized frame %d\n", targetFrame);
    g_pendingRoster = false;
    g_pendingTargetFrame = -1;
    return true;
}
