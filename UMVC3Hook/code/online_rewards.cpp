// Native online rewards for netplay matches, and Steam achievement delivery.
// Addresses and behaviour: doc/online_rewards.md (Ghidra, 2026-11-18).

#include "online_rewards.h"

#include "netplay.h"
#include "spectate.h"
#include "mk10utils.h"
#include "../kiero/minhook/include/MinHook.h"

#include <Windows.h>
#include <cstdint>
#include <cstdio>

extern void HookLog(const char* fmt, ...);

namespace {

// ---------------------------------------------------------------------------
// Steam achievements
// ---------------------------------------------------------------------------
// The game queues an unlock with nNet::sGuide::awardAchievement and completes
// it from MtNetCore::move, which only runs under nNet::sComponent::move. The
// plugin removes that call (native_network_disable.md), so the request would
// wait for ever. The hook below makes the Steam calls directly instead.

constexpr uintptr_t FN_GUIDE_AWARD_ACHIEVEMENT = 0x140458F80;
constexpr size_t GUIDE_AWARD_COUNT = 0x300;      // uint16, awards handed over
constexpr uint32_t kAchievementCount = 47;       // ACHIEVEMENT_00..46

using HSteamUser = int32_t;
using HSteamPipe = int32_t;

struct StatsApi {
    void* (*GetClient)() = nullptr;
    HSteamUser (*GetUser)() = nullptr;
    HSteamPipe (*GetPipe)() = nullptr;
    void* (*GetUserStats)(void*, HSteamUser, HSteamPipe, const char*) = nullptr;
    bool (*RequestCurrentStats)(void*) = nullptr;
    bool (*GetAchievement)(void*, const char*, bool*) = nullptr;
    bool (*SetAchievement)(void*, const char*) = nullptr;
    bool (*StoreStats)(void*) = nullptr;
};

StatsApi g_stats;
void* g_userStats = nullptr;
bool g_statsRequested = false;
bool g_storePending = false;

template <typename T>
bool ResolveExport(HMODULE module, T& target, const char* name)
{
    target = reinterpret_cast<T>(GetProcAddress(module, name));
    return target != nullptr;
}

// The game has already initialised Steam; this only looks its interface up.
void* UserStats()
{
    if (g_userStats) return g_userStats;
    const HMODULE module = GetModuleHandleA("steam_api64.dll");
    if (!module) return nullptr;
    StatsApi api;
    if (!ResolveExport(module, api.GetClient, "SteamClient") ||
        !ResolveExport(module, api.GetUser, "SteamAPI_GetHSteamUser") ||
        !ResolveExport(module, api.GetPipe, "SteamAPI_GetHSteamPipe") ||
        !ResolveExport(module, api.GetUserStats, "SteamAPI_ISteamClient_GetISteamUserStats") ||
        !ResolveExport(module, api.RequestCurrentStats, "SteamAPI_ISteamUserStats_RequestCurrentStats") ||
        !ResolveExport(module, api.GetAchievement, "SteamAPI_ISteamUserStats_GetAchievement") ||
        !ResolveExport(module, api.SetAchievement, "SteamAPI_ISteamUserStats_SetAchievement") ||
        !ResolveExport(module, api.StoreStats, "SteamAPI_ISteamUserStats_StoreStats")) {
        return nullptr;
    }
    void* client = api.GetClient();
    const HSteamPipe pipe = api.GetPipe();
    if (!client || !pipe) return nullptr;
    // The interface version the game itself asks for.
    void* stats = api.GetUserStats(client, api.GetUser(), pipe,
                                   "STEAMUSERSTATS_INTERFACE_VERSION011");
    if (!stats) return nullptr;
    g_stats = api;
    g_userStats = stats;
    return stats;
}

// True once Steam has the achievement. False asks the game to try again; it
// does, ten seconds later, for as long as the request is pending.
bool AwardSteamAchievement(uint32_t id)
{
    if (id >= kAchievementCount) return false;
    void* stats = UserStats();
    if (!stats) return false;
    // This interface version only answers once the user's stats have been
    // requested. The game's own request was in the update that is removed.
    if (!g_statsRequested) g_statsRequested = g_stats.RequestCurrentStats(stats);
    if (g_storePending && g_stats.StoreStats(stats)) g_storePending = false;

    char name[24];
    snprintf(name, sizeof(name), "ACHIEVEMENT_%02u", id);
    bool achieved = false;
    if (g_stats.GetAchievement(stats, name, &achieved) && achieved) return true;
    if (!g_stats.SetAchievement(stats, name)) {
        static uint64_t reported = 0;
        if (!(reported & (1ull << id))) {
            reported |= 1ull << id;
            HookLog("OnlineRewards: Steam did not take %s yet; will retry\n", name);
        }
        return false;
    }
    if (!g_stats.StoreStats(stats)) g_storePending = true;
    HookLog("OnlineRewards: unlocked Steam achievement %s\n", name);
    return true;
}

using GuideAwardAchievementFn = bool(__fastcall*)(uint64_t guide, uint32_t unused, uint32_t id);
GuideAwardAchievementFn g_originalGuideAward = nullptr;

bool __fastcall Hook_GuideAwardAchievement(uint64_t guide, uint32_t, uint32_t id)
{
    if (!AwardSteamAchievement(id)) return false;
    ++*reinterpret_cast<uint16_t*>(guide + GUIDE_AWARD_COUNT);
    return true;
}

// ---------------------------------------------------------------------------
// Match rewards
// ---------------------------------------------------------------------------
// aAction::commitBattleRewards pays Player Points and updates play data,
// titles and achievement requests once per battle. A plugin match is battle
// type 0 (offline versus), which rewards team 0 with the versus amounts and
// counts nothing as online. For the length of that one call the hook shows the
// game what a native lobby match looks like, so every native online branch
// runs as it would there, for the local player's team.

constexpr uintptr_t FN_COMMIT_BATTLE_REWARDS = 0x1402052F0;
constexpr uintptr_t FN_ACTION_GET_INSTANCE = 0x140001A90;
constexpr uintptr_t FN_GAME_UI_GET_INSTANCE = 0x140001AC0;
constexpr uintptr_t FN_BATTLE_SETTING_GET_INSTANCE = 0x140004700;
constexpr uintptr_t FN_LICENSE_GET_INSTANCE = 0x140004750;
constexpr uintptr_t FN_NETWORK_ROOM_GET_INSTANCE = 0x140004790;

constexpr size_t ACTION_REWARDS_COMMITTED = 0x172;   // uint8
constexpr size_t BATTLE_SETTING_BATTLE_TYPE = 0x34C; // int32
constexpr size_t GAME_UI_CSS_STATE = 0x5B74;         // int32, selects the PP formula
constexpr size_t NETWORK_ROOM_TYPE = 0x38;           // int32
constexpr size_t NETWORK_ROOM_MY_NET_TEAM = 0xBE8;   // int32
constexpr size_t LICENSE_PLAY_POINT = 0x74;          // int32, PP total (inline, not behind a pointer)

constexpr int32_t kBattleTypeLiveOnline = 3;
// Room type 3 is the 8-player lobby: 0 and 1 are ranked (rank points, ranked
// win counts), 2 is player match, 4 is Heroes & Heralds.
constexpr int32_t kRoomTypeLobby = 3;
// Css state 8 is the lobby's: 448 PP plus a minute bonus, win or lose.
constexpr int32_t kCssStateLobby = 8;
// A rollback rewinds sAction, and with it the "committed" flag, to a frame
// before the commit; the frames are replayed and the commit asked for again.
// The rollback window is 8 frames, and no match ends within seconds of the
// last one, so a second request this soon is that replay.
constexpr uint64_t kCommitReplayGuardMs = 5000;

using GetInstanceFn = uint64_t(__fastcall*)();
using CommitBattleRewardsFn = void(__fastcall*)(uint64_t area);
CommitBattleRewardsFn g_originalCommitBattleRewards = nullptr;
uint64_t g_lastCommitTick = 0;

uint64_t Instance(uintptr_t getter)
{
    return reinterpret_cast<GetInstanceFn>(_addr(getter))();
}

int32_t PlayPointTotal()
{
    const uint64_t license = Instance(FN_LICENSE_GET_INSTANCE);
    return license ? *reinterpret_cast<int32_t*>(license + LICENSE_PLAY_POINT) : 0;
}

void __fastcall Hook_CommitBattleRewards(uint64_t area)
{
    if (!Netplay_IsActive()) {
        g_originalCommitBattleRewards(area);
        return;
    }
    // A native spectator earns nothing either.
    if (Netplay_IsSpectator() || Spectate_IsViewing()) return;

    const uint64_t action = Instance(FN_ACTION_GET_INSTANCE);
    const uint64_t setting = Instance(FN_BATTLE_SETTING_GET_INSTANCE);
    const uint64_t room = Instance(FN_NETWORK_ROOM_GET_INSTANCE);
    const uint64_t gameUi = Instance(FN_GAME_UI_GET_INSTANCE);
    if (!action || !setting || !room || !gameUi) {
        g_originalCommitBattleRewards(area);
        return;
    }

    uint8_t& committed = *reinterpret_cast<uint8_t*>(action + ACTION_REWARDS_COMMITTED);
    const uint64_t now = GetTickCount64();
    if (g_lastCommitTick && now - g_lastCommitTick < kCommitReplayGuardMs) {
        committed = 1;
        return;
    }

    int32_t& battleType = *reinterpret_cast<int32_t*>(setting + BATTLE_SETTING_BATTLE_TYPE);
    int32_t& roomType = *reinterpret_cast<int32_t*>(room + NETWORK_ROOM_TYPE);
    int32_t& myNetTeam = *reinterpret_cast<int32_t*>(room + NETWORK_ROOM_MY_NET_TEAM);
    int32_t& cssState = *reinterpret_cast<int32_t*>(gameUi + GAME_UI_CSS_STATE);
    const int32_t savedBattleType = battleType;
    const int32_t savedRoomType = roomType;
    const int32_t savedMyNetTeam = myNetTeam;
    const int32_t savedCssState = cssState;
    const bool wasCommitted = committed != 0;
    const int32_t pointsBefore = PlayPointTotal();
    const int32_t localTeam = Netplay_IsHost() ? 0 : 1;

    battleType = kBattleTypeLiveOnline;
    roomType = kRoomTypeLobby;
    myNetTeam = localTeam;
    cssState = kCssStateLobby;
    g_originalCommitBattleRewards(area);
    battleType = savedBattleType;
    roomType = savedRoomType;
    myNetTeam = savedMyNetTeam;
    cssState = savedCssState;

    if (!wasCommitted && committed) {
        g_lastCommitTick = now;
        HookLog("OnlineRewards: match rewards committed for team %d (+%d PP)\n",
                localTeam, PlayPointTotal() - pointsBefore);
    }
}

bool Install(uintptr_t address, void* detour, void** original)
{
    void* target = reinterpret_cast<void*>(_addr(address));
    return MH_CreateHook(target, detour, original) == MH_OK && MH_EnableHook(target) == MH_OK;
}

} // namespace

void OnlineRewards_InstallHooks()
{
    // Steam achievements are delivered by the plugin.
    if (!Install(FN_GUIDE_AWARD_ACHIEVEMENT, reinterpret_cast<void*>(&Hook_GuideAwardAchievement),
                 reinterpret_cast<void**>(&g_originalGuideAward)))
        HookLog("OnlineRewards: FAILED to hook achievement delivery\n");

    // Netplay matches pay the native online lobby rewards.
    if (!Install(FN_COMMIT_BATTLE_REWARDS, reinterpret_cast<void*>(&Hook_CommitBattleRewards),
                 reinterpret_cast<void**>(&g_originalCommitBattleRewards)))
        HookLog("OnlineRewards: FAILED to hook the battle reward commit\n");
}
