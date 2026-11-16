#include "../pch.h"
#include "lobby_flow.h"

#include "LaunchMenu.h"
#include "assist_names.h"
#include "connect_roster.h"
#include "controller.h"
#include "framestepper.h"
#include "launch_music.h"
#include "launchmenu_utils.h"
#include "lobby_native.h"
#include "lobby_room.h"
#include "lobby_ui.h"
#include "mk10utils.h"
#include "netplay.h"
#include "savedata.h"
#include "steam_lobby.h"
#include "steam_transport.h"
#include "trainingboot.h"
#include "../IniSafe.h"
#include "../imgui/imgui.h"
#include "../kiero/minhook/include/MinHook.h"

#include <Windows.h>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>

extern void HookLog(const char* fmt, ...);
extern void HookLogFlush();

using namespace lobby;

namespace {

// ---------------------------------------------------------------------------
// Native addresses (see doc/area.md and doc/native_lobby_ui.md)
// ---------------------------------------------------------------------------
constexpr uintptr_t SAREA_INSTANCE_PTR = 0x140E17A80;
constexpr uintptr_t SAREA_VT_REQUEST_TRANSITION = 0x50;
constexpr uintptr_t ATITLE_DTI = 0x140D506E8;
constexpr size_t ATITLE_TITLE_MENU = 0x90; // uMenuTitle*
constexpr size_t ATITLE_MAIN_MENU = 0x98;  // uMenuMain*
constexpr size_t ATITLE_PRESENTATION_MODE = 0xA0;
// (aTitle*, mode): mode 1 spawns uMenuMain. It is what the press-start
// screen invokes to open the main menu; it does nothing if the mode matches.
constexpr uintptr_t ATITLE_SET_PRESENTATION_MODE = 0x140216600;
// What uMenuTitle::move (0x1403F7DB0) sets up when Start is pressed, which the
// lobby skips by retiring that unit: the menu-controlling pad for player 0
// (sMvc3Manager+0x140), two sGameUi bytes it clears, and (doc/pad.md) the
// sPad keyboard-block bytes that otherwise keep the keyboard out of menus.
constexpr uintptr_t SMVC3MANAGER_INSTANCE_PTR = 0x140D510A0;
constexpr size_t MANAGER_PAD_ASSIGN = 0x140; // int32_t[4]
constexpr uintptr_t SPAD_INSTANCE_PTR = 0x140E174B8;
constexpr size_t SPAD_KEYBOARD_BLOCKED_1 = 0xC20;
constexpr size_t SPAD_KEYBOARD_BLOCKED_2 = 0xC30;
constexpr uintptr_t SGAMEUI_GET_INSTANCE = 0x140001AC0;
// The main menu greys out and skips "Online" while the game's own Steam
// session is not ready, which it never is here: the plugin switches native
// networking off (doc/native_network_disable.md). Three main-menu call sites
// ask through this thunk; the navigation state (shared by every menu) reads
// the cursor at +0x78 and writes 1 to its out parameter on decide.
constexpr uintptr_t NET_SESSION_READY_THUNK = 0x14045CFF0;
constexpr uintptr_t MENU_FLOW_NAVIGATION = 0x1403E9D90;       // (menu, int32_t* out)
constexpr uintptr_t MAINMENU_MOVE_READY_RETURN = 0x1403E7514; // in uMenuMain's move
constexpr uintptr_t MAINMENU_INITIAL_SELECT_RETURN = 0x1403E7274;
constexpr size_t MENU_CURSOR = 0x78;
constexpr int32_t MAINMENU_ITEM_ONLINE = 1;
constexpr uintptr_t UMENUMAIN_DTI = 0x140D6AE60;
constexpr uintptr_t MTOBJECT_VT_GET_DTI = 0x20;
// uMenuMain keeps its layout (ma_menu_main) in a unit reference at +0xD0;
// the getter returns the uScheduler. uMenuMain::kill only takes it down in
// some states, so the lobby kills it explicitly.
constexpr size_t UMENUMAIN_LAYOUT_REF = 0xD0;
constexpr uintptr_t UNIT_REF_GET = 0x1402B6D80;
constexpr ULONGLONG kOnlineFadeOutMs = 200;
constexpr ULONGLONG kOnlineFadeInMs = 250;
constexpr ULONGLONG kOnlineFadeLimitMs = 3000; // never leave the screen black
constexpr uint32_t CUNIT_FIX = 0x2000; // uMenuMain::kill keeps its layout alive while set
constexpr size_t SGAMEUI_TITLE_BYTE_1 = 0x5C40;
constexpr size_t SGAMEUI_TITLE_BYTE_2 = 0x5EA8;
constexpr size_t CUNIT_FLAGS = 0x10;
constexpr uintptr_t CUNIT_VT_KILL = 0x78;

constexpr ULONGLONG kConnectTimeoutMs = 30000;
constexpr ULONGLONG kHandshakeTimeoutMs = 15000;
constexpr ULONGLONG kPingSettleMs = 1500;
constexpr ULONGLONG kPingWaitLimitMs = 4500;

enum class Phase {
    Lobby,      // screens are up
    Connecting, // opening the match connection to the paired opponent
    Handshake,  // exchanging settings and rosters over the fresh connection
    Launching,  // lobby torn down, BootIntoTraining in progress
    InMatch,    // battle running
    Returning,  // aTitle requested after a match
};

bool g_enabled = false;
Phase g_phase = Phase::Lobby;
bool g_titleNeedsSetup = true;
bool g_titleRequested = false;
// The player left the lobby for the native main menu (game thread only).
bool g_releasedToMainMenu = false;
std::atomic<bool> g_reopenRequested{false};
std::atomic<ULONGLONG> g_releasedTitleTick{0};
// Main menu "Online" transition: black fades out over the menu, holds until
// the lobby can draw, then fades in. Read by the overlay (render thread).
std::atomic<ULONGLONG> g_fadeOutStart{0}, g_fadeInStart{0};
std::atomic<bool> g_fadeHold{false};

std::unique_ptr<SteamRoomService> g_service;
std::unique_ptr<LobbyUi> g_ui;
ConnectRoster g_rosterPrefs;
SavedRoster g_presets[3] = {};
size_t g_presetCount = 0;
bool g_presetsLoaded = false;

MatchPairing g_pairing = {};
ENetHost* g_matchHost = nullptr;
ENetPeer* g_matchPeer = nullptr;
bool g_matchIsHost = false;
ULONGLONG g_phaseStartTick = 0;
bool g_launchSent = false;

bool g_musicWanted = false;
// Frames the lobby has been waiting for the native backend to come up.
std::atomic<uint32_t> g_nativeWaitFrames{0};
constexpr uint32_t kNativeSlowFrames = 180;    // show "starting" after 3 s
constexpr uint32_t kNativeFailedFrames = 900;  // give up reporting after 15 s

std::atomic<bool> g_returnRequested{false};
char g_returnReason[160] = {};
char g_pendingNotice[160] = {};

// ---------------------------------------------------------------------------
// Roster data for the team screen
// ---------------------------------------------------------------------------
int RosterCharacterCount() { return 50; }
int32_t RosterCharacterId(int index) { return index + 1; }

const char* RosterCharacterName(int32_t id)
{
    return id >= 1 && id < kCharNameCount ? kCharNames[id] : "?";
}

const char* RosterAssistName(int32_t id, int assist)
{
    const int count = static_cast<int>(sizeof(kAssistMoveNames) / sizeof(kAssistMoveNames[0]));
    if (id < 0 || id >= count || assist < 0 || assist > 2) return "";
    return kAssistMoveNames[id].names[assist];
}

int RosterColorCount(int32_t id)
{
    // Same archive-variant counts the launcher validates against.
    if (id == 3 || id == 5 || id == 35 || id == 39) return 9;
    return id >= 1 && id <= 50 ? 8 : 1;
}

void LoadPresets()
{
    if (g_presetsLoaded) return;
    g_presetsLoaded = true;
    g_presetCount = Savedata_LoadRosters(g_presets, 3);
    HookLog("LobbyFlow: loaded %u saved roster presets\n", static_cast<unsigned>(g_presetCount));
}

int RosterPresetCount()
{
    LoadPresets();
    return static_cast<int>(g_presetCount);
}

bool RosterPreset(int index, Team* team)
{
    LoadPresets();
    if (!team || index < 0 || static_cast<size_t>(index) >= g_presetCount) return false;
    for (int i = 0; i < 3; ++i) {
        team->chars[i] = g_presets[index].chars[i];
        team->assists[i] = g_presets[index].assists[i];
        team->colors[i] = g_presets[index].colors[i];
        if (team->chars[i] < 1 || team->chars[i] > 50) return false;
    }
    return true;
}

const RosterSource kRosterSource = {
    RosterCharacterCount, RosterCharacterId, RosterCharacterName, RosterAssistName,
    RosterColorCount,     RosterPresetCount, RosterPreset,
};

// ---------------------------------------------------------------------------
// Preferences (Timestone.ini)
// ---------------------------------------------------------------------------
void SettingsPath(char* path, size_t size)
{
    LaunchMenuUtils::GetSettingsPath(path, size);
}

UiPrefs LoadPrefs()
{
    char path[MAX_PATH] = {};
    SettingsPath(path, sizeof(path));
    g_rosterPrefs = ConnectRoster_Load(path, ConnectRoster{});

    UiPrefs prefs = {};
    for (int i = 0; i < 3; ++i) {
        prefs.team.chars[i] = g_rosterPrefs.chars[i];
        prefs.team.assists[i] = g_rosterPrefs.assists[i];
        prefs.team.colors[i] = g_rosterPrefs.colors[i];
    }
    bool valid = true;
    for (int i = 0; i < 3; ++i) {
        valid &= prefs.team.chars[i] >= 1 && prefs.team.chars[i] <= 50 &&
                 prefs.team.assists[i] >= 0 && prefs.team.assists[i] <= 2 &&
                 prefs.team.colors[i] >= 0 &&
                 prefs.team.colors[i] < RosterColorCount(prefs.team.chars[i]);
    }
    if (!valid) prefs.team = Team{{CHR_RYU, CHR_WOLVERINE, CHR_DANTE}, {0, 0, 0}, {0, 0, 0}};

    prefs.entryMode = GetPrivateProfileIntA("Lobby", "EntryMode", 0, path) == 1
        ? EntryMode::CharacterSelect : EntryMode::Preset;
    prefs.presetIndex = GetPrivateProfileIntA("Lobby", "Preset", -1, path);
    if (prefs.presetIndex < -1 || prefs.presetIndex > 2) prefs.presetIndex = -1;
    GetPrivateProfileStringA("Lobby", "RoomName", "", prefs.create.name,
                             sizeof(prefs.create.name), path);
    prefs.create.maxMembers = GetPrivateProfileIntA("Lobby", "RoomSize", kMaxMembers, path);
    prefs.create.slotCount = GetPrivateProfileIntA("Lobby", "RoomSlots", 2, path);
    const int visibility = GetPrivateProfileIntA("Lobby", "RoomVisibility", 0, path);
    prefs.create.visibility = visibility == 1 ? Visibility::FriendsOnly
                            : visibility == 2 ? Visibility::Private : Visibility::Public;
    return prefs;
}

void SavePrefs(const UiPrefs& prefs)
{
    char path[MAX_PATH] = {};
    SettingsPath(path, sizeof(path));
    for (int i = 0; i < 3; ++i) {
        g_rosterPrefs.chars[i] = prefs.team.chars[i];
        g_rosterPrefs.assists[i] = prefs.team.assists[i];
        g_rosterPrefs.colors[i] = prefs.team.colors[i];
    }
    ConnectRoster_Save(path, g_rosterPrefs);
    const IniSafe_Entry entries[] = {
        {"Lobby", "EntryMode", prefs.entryMode == EntryMode::CharacterSelect ? "1" : "0"},
        {"Lobby", "Preset", std::to_string(prefs.presetIndex)},
        {"Lobby", "RoomName", prefs.create.name},
        {"Lobby", "RoomSize", std::to_string(prefs.create.maxMembers)},
        {"Lobby", "RoomSlots", std::to_string(prefs.create.slotCount)},
        {"Lobby", "RoomVisibility", std::to_string(static_cast<int>(prefs.create.visibility))},
    };
    IniSafe_WriteBatch(entries, sizeof(entries) / sizeof(entries[0]), path);
}

// ---------------------------------------------------------------------------
// Native helpers
// ---------------------------------------------------------------------------
bool RequestTitleArea()
{
    const uint64_t area = *reinterpret_cast<uint64_t*>(_addr(SAREA_INSTANCE_PTR));
    if (!area) return false;
    using RequestTransition = void(__fastcall*)(uint64_t, void*);
    const RequestTransition request = *reinterpret_cast<RequestTransition*>(
        *reinterpret_cast<uint64_t*>(area) + SAREA_VT_REQUEST_TRANSITION);
    request(area, reinterpret_cast<void*>(_addr(ATITLE_DTI)));
    return true;
}

// aTitle::init spawns the press-start screen or the main menu. The lobby
// replaces both, so retire whichever one this entry created.
void RetireNativeTitleUnits(uint64_t titleArea)
{
    for (const size_t offset : {ATITLE_TITLE_MENU, ATITLE_MAIN_MENU}) {
        void*& unit = *reinterpret_cast<void**>(titleArea + offset);
        if (!unit) continue;
        uint32_t& flags = *reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(unit) + CUNIT_FLAGS);
        const uint32_t state = flags & 7u;
        if (state == 1 || state == 2) {
            // A menu that is mid-transition is "fixed", and its kill then
            // leaves the layout scheduler running; it must go with the menu.
            flags &= ~CUNIT_FIX;
            using Kill = void(__fastcall*)(void*);
            void* layout = nullptr;
            if (offset == ATITLE_MAIN_MENU) {
                using RefGet = void*(__fastcall*)(void*);
                layout = reinterpret_cast<RefGet>(_addr(UNIT_REF_GET))(
                    static_cast<uint8_t*>(unit) + UMENUMAIN_LAYOUT_REF);
            }
            (*reinterpret_cast<Kill*>(*reinterpret_cast<uint8_t**>(unit) + CUNIT_VT_KILL))(unit);
            if (layout) {
                const uint32_t layoutState = *reinterpret_cast<uint32_t*>(
                    static_cast<uint8_t*>(layout) + CUNIT_FLAGS) & 7u;
                if (layoutState == 1 || layoutState == 2) {
                    (*reinterpret_cast<Kill*>(*reinterpret_cast<uint8_t**>(layout) + CUNIT_VT_KILL))(layout);
                    HookLog("LobbyFlow: killed the main menu layout left behind by its menu\n");
                }
            }
            HookLog("LobbyFlow: retired native title unit at aTitle+0x%X (state %u -> %u)\n",
                    static_cast<unsigned>(offset), state, flags & 7u);
        }
        unit = nullptr;
    }
}


// ---------------------------------------------------------------------------
// Main menu "Online" -> lobby
// ---------------------------------------------------------------------------
using SessionReady = bool(__fastcall*)(void*);
using MenuNavigation = uint64_t(__fastcall*)(void*, int32_t*);
SessionReady g_originalSessionReady = nullptr;
MenuNavigation g_originalMenuNavigation = nullptr;
bool g_menuHooksInstalled = false;
bool g_inMainMenuNavigation = false; // game thread only
uint64_t g_releasedTitleArea = 0;

bool TitleAreaIsRunning()
{
    return GetTickCount64() - g_releasedTitleTick.load(std::memory_order_relaxed) < 500;
}

// Any live uMenuMain counts, not only the one aTitle tracks at +0x98: the
// navigation state is shared by every menu class and must be told apart by
// class, and the title area can respawn its menu on its own.
bool IsReleasedMainMenu(void* menu)
{
    if (!g_releasedToMainMenu || !menu || !TitleAreaIsRunning()) return false;
    using GetDti = void*(__fastcall*)(void*);
    const GetDti getDti = *reinterpret_cast<GetDti*>(
        *reinterpret_cast<uint8_t**>(menu) + MTOBJECT_VT_GET_DTI);
    return getDti(menu) == reinterpret_cast<void*>(_addr(UMENUMAIN_DTI));
}

__attribute__((noinline)) bool __fastcall Hook_SessionReady(void* library)
{
    if (g_releasedToMainMenu) {
        const uintptr_t caller = reinterpret_cast<uintptr_t>(__builtin_return_address(0));
        if (g_inMainMenuNavigation || caller == _addr(MAINMENU_MOVE_READY_RETURN) ||
            caller == _addr(MAINMENU_INITIAL_SELECT_RETURN))
            return true;
    }
    return g_originalSessionReady(library);
}

uint64_t __fastcall Hook_MenuNavigation(void* menu, int32_t* out)
{
    if (!IsReleasedMainMenu(menu)) return g_originalMenuNavigation(menu, out);

    // The menu is frozen while the transition to the lobby plays.
    if (g_fadeHold.load(std::memory_order_relaxed)) return 1;

    // Take the decide on Online before the menu sees it: its own handler
    // starts the native network entry, which leaves the menu half torn down.
    Input input = {};
    Native_ReadInput(input, false);
    if (input.confirm &&
        *reinterpret_cast<int32_t*>(static_cast<uint8_t*>(menu) + MENU_CURSOR) ==
            MAINMENU_ITEM_ONLINE) {
        HookLog("LobbyFlow: Online chosen on the main menu\n");
        Native_PlaySfx(SFX_CONFIRM);
        g_fadeInStart.store(0, std::memory_order_relaxed);
        g_fadeOutStart.store(GetTickCount64(), std::memory_order_relaxed);
        g_fadeHold.store(true, std::memory_order_release);
        return 1;
    }

    g_inMainMenuNavigation = true;
    const uint64_t result = g_originalMenuNavigation(menu, out);
    g_inMainMenuNavigation = false;
    // A decide that did not come through the pad or keyboard (a mouse click)
    // has already been taken by the menu, which played its own sound.
    if (out && *out == 1 &&
        *reinterpret_cast<int32_t*>(static_cast<uint8_t*>(menu) + MENU_CURSOR) ==
            MAINMENU_ITEM_ONLINE) {
        HookLog("LobbyFlow: Online clicked on the main menu\n");
        g_fadeInStart.store(0, std::memory_order_relaxed);
        g_fadeOutStart.store(GetTickCount64(), std::memory_order_relaxed);
        g_fadeHold.store(true, std::memory_order_release);
    }
    return result;
}

void InstallMainMenuHooks()
{
    if (g_menuHooksInstalled) return;
    g_menuHooksInstalled = true;
    void* ready = reinterpret_cast<void*>(_addr(NET_SESSION_READY_THUNK));
    void* navigation = reinterpret_cast<void*>(_addr(MENU_FLOW_NAVIGATION));
    const MH_STATUS a = MH_CreateHook(ready, reinterpret_cast<void*>(&Hook_SessionReady),
                                      reinterpret_cast<void**>(&g_originalSessionReady));
    const MH_STATUS b = MH_CreateHook(navigation, reinterpret_cast<void*>(&Hook_MenuNavigation),
                                      reinterpret_cast<void**>(&g_originalMenuNavigation));
    if (a != MH_OK || b != MH_OK || MH_EnableHook(navigation) != MH_OK ||
        MH_EnableHook(ready) != MH_OK)
        HookLog("LobbyFlow: main menu Online hooks failed (%d, %d); use the Esc menu\n",
                static_cast<int>(a), static_cast<int>(b));
}

void ApplyPressStartState()
{
    if (const uint64_t manager = *reinterpret_cast<uint64_t*>(_addr(SMVC3MANAGER_INSTANCE_PTR))) {
        int32_t* assign = reinterpret_cast<int32_t*>(manager + MANAGER_PAD_ASSIGN);
        // Keyboard input is routed through whichever slot player 0 maps to.
        if (assign[0] < 0 || assign[0] > 3) {
            for (int i = 1; i < 4; ++i)
                if (assign[i] == 0) assign[i] = assign[0];
            assign[0] = 0;
        }
    }
    if (const uint64_t pad = *reinterpret_cast<uint64_t*>(_addr(SPAD_INSTANCE_PTR))) {
        *reinterpret_cast<uint8_t*>(pad + SPAD_KEYBOARD_BLOCKED_1) = 0;
        *reinterpret_cast<uint8_t*>(pad + SPAD_KEYBOARD_BLOCKED_2) = 0;
    }
    using GetInstance = uint64_t(__fastcall*)();
    if (const uint64_t gameUi = reinterpret_cast<GetInstance>(_addr(SGAMEUI_GET_INSTANCE))()) {
        *reinterpret_cast<uint8_t*>(gameUi + SGAMEUI_TITLE_BYTE_1) = 0;
        *reinterpret_cast<uint8_t*>(gameUi + SGAMEUI_TITLE_BYTE_2) = 0;
    }
}

// Hands the title area back to the game with its main menu open.
void ReleaseToMainMenu(uint64_t titleArea)
{
    HookLog("LobbyFlow: leaving the lobby for the main menu\n");
    if (g_service && g_service->Room().inRoom) g_service->Leave();
    LaunchMusic_BeginFadeOut();
    g_musicWanted = false;
    Native_Shutdown();
    RetireNativeTitleUnits(titleArea);
    ApplyPressStartState();
    InstallMainMenuHooks();
    g_releasedTitleArea = titleArea;
    g_releasedTitleTick.store(GetTickCount64(), std::memory_order_relaxed);
    *reinterpret_cast<int32_t*>(titleArea + ATITLE_PRESENTATION_MODE) = -1;
    using SetPresentationMode = void(__fastcall*)(uint64_t, int32_t);
    reinterpret_cast<SetPresentationMode>(_addr(ATITLE_SET_PRESENTATION_MODE))(titleArea, 1);
    g_reopenRequested.store(false, std::memory_order_relaxed);
    g_releasedToMainMenu = true;
}

// ---------------------------------------------------------------------------
// Match start
// ---------------------------------------------------------------------------
void SetPhase(Phase phase)
{
    g_phase = phase;
    g_phaseStartTick = GetTickCount64();
}

void DropMatchConnection()
{
    // Before Netplay_Init the game-side ENet host is still ours to destroy.
    if (g_matchHost && !Netplay_IsActive()) enet_host_destroy(g_matchHost);
    g_matchHost = nullptr;
    g_matchPeer = nullptr;
    SteamRoom_DisconnectPeer();
}

void AbortMatchStart(const char* notice)
{
    HookLog("LobbyFlow: match start aborted: %s\n", notice ? notice : "(cancelled)");
    DropMatchConnection();
    if (g_service) g_service->FinishMatch(false, false);
    if (g_ui) {
        g_ui->SetBusy(nullptr);
        if (notice) g_ui->ShowNotice(notice);
    }
    SetPhase(Phase::Lobby);
}

void FillBootConfig(TrainingBootConfig& config, const Team& p1, const Team& p2, int32_t stageId)
{
    config = TrainingBoot_GetDefaultConfig();
    for (int i = 0; i < 3; ++i) {
        config.p1Chars[i] = p1.chars[i];
        config.p1Colors[i] = p1.colors[i];
        config.p1Assists[i] = p1.assists[i];
        config.p2Chars[i] = p2.chars[i];
        config.p2Colors[i] = p2.colors[i];
        config.p2Assists[i] = p2.assists[i];
    }
    config.simpleMode[0] = false;
    config.simpleMode[1] = false;
    config.stageId = stageId;
    config.timerValue = 99;
}

// Player 1 owns the match parameters: netplay settings from measured ping,
// both rosters as published in the room, and the stage. This mirrors what the
// launcher's host sends before it starts the game.
bool SendLaunchAsHost()
{
    int32_t delay = 3, rollback = 7;
    const uint32_t rtt = SteamTransport_GetRTT();
    LaunchMenu_GetAutomaticNetplaySettings(rtt / 2, &delay, &rollback);
    Controller_SetDelayFrames(delay);
    Controller_SetRollbackFrames(rollback);

    DelayPacket settings = {};
    settings.type = PACKET_DELAY;
    settings.delay = delay;
    settings.rollbackFrames = rollback;
    settings.effective_frame = -1;

    LaunchTrainingPacket launch = {};
    launch.type = PACKET_LAUNCH_TRAINING;
    for (int i = 0; i < 3; ++i) {
        launch.p1Chars[i] = g_pairing.localTeam.chars[i];
        launch.p1Colors[i] = g_pairing.localTeam.colors[i];
        launch.p1Assists[i] = g_pairing.localTeam.assists[i];
        launch.p2Chars[i] = g_pairing.opponentTeam.chars[i];
        launch.p2Colors[i] = g_pairing.opponentTeam.colors[i];
        launch.p2Assists[i] = g_pairing.opponentTeam.assists[i];
    }
    launch.stageId = g_rosterPrefs.stageId;

    ENetPacket* settingsPacket =
        enet_packet_create(&settings, sizeof(settings), ENET_PACKET_FLAG_RELIABLE);
    ENetPacket* launchPacket =
        enet_packet_create(&launch, sizeof(launch), ENET_PACKET_FLAG_RELIABLE);
    if (!settingsPacket || !launchPacket ||
        enet_peer_send(g_matchPeer, 0, settingsPacket) != 0) {
        if (settingsPacket) enet_packet_destroy(settingsPacket);
        if (launchPacket) enet_packet_destroy(launchPacket);
        return false;
    }
    if (enet_peer_send(g_matchPeer, 0, launchPacket) != 0) {
        enet_packet_destroy(launchPacket);
        return false;
    }
    enet_host_flush(g_matchHost);

    // Same match options the receiving side derives from the packet.
    TrainingBoot_SetCpuMode(TRAINING_CPU_NONE);
    TrainingBoot_SetFiveBarStart(false);
    TrainingBootConfig config;
    FillBootConfig(config, g_pairing.localTeam, g_pairing.opponentTeam, launch.stageId);
    TrainingBoot_SetConfig(config);
    HookLog("LobbyFlow: sent launch as P1 ping=%u ms settings=%dD/%dR stage=%d\n", rtt / 2, delay,
            rollback, launch.stageId);
    return true;
}

void BeginLaunch()
{
    // The lobby stays on screen (busy overlay) until the boot commits; see
    // LobbyFlow_OnBattleBootCommitted.
    TrainingBoot_PrepareNextBoot(true);
    SetNetworkHandles(g_matchHost, g_matchPeer);
    FrameStepper_InitNetplay();
    g_matchHost = nullptr; // owned by netplay from here on
    g_matchPeer = nullptr;
    if (g_service) g_service->SetLocalStatus(MemberStatus::Fighting);
    SetPhase(Phase::Launching);
    HookLog("LobbyFlow: launching match vs %s as P%d\n", g_pairing.opponentName,
            g_pairing.localSide + 1);
}

// Services the fresh connection until the launch parameters are exchanged.
// Returns true when the match can launch.
bool PumpHandshake()
{
    const ULONGLONG elapsed = GetTickCount64() - g_phaseStartTick;
    if (elapsed > kHandshakeTimeoutMs) {
        AbortMatchStart("Could not properly sync with the other player.");
        return false;
    }

    if (g_matchIsHost) {
        // Let Steam's route settle so the automatic settings see a real ping.
        const bool pingKnown = SteamTransport_GetRTT() != 0;
        if (elapsed < kPingSettleMs || (!pingKnown && elapsed < kPingWaitLimitMs)) {
            ENetEvent event;
            while (enet_host_service(g_matchHost, &event, 0) > 0) {
                if (event.type == ENET_EVENT_TYPE_RECEIVE) {
                    enet_packet_destroy(event.packet);
                } else if (event.type == ENET_EVENT_TYPE_DISCONNECT) {
                    AbortMatchStart("Connection to opponent has been lost.");
                    return false;
                }
            }
            return false;
        }
        if (!SendLaunchAsHost()) {
            AbortMatchStart("Failed to create game session.");
            return false;
        }
        return true;
    }

    ENetEvent event;
    while (enet_host_service(g_matchHost, &event, 0) > 0) {
        if (event.type == ENET_EVENT_TYPE_DISCONNECT) {
            AbortMatchStart("Connection to opponent has been lost.");
            return false;
        }
        if (event.type != ENET_EVENT_TYPE_RECEIVE) continue;
        const uint32_t type = event.packet->dataLength >= sizeof(uint32_t)
            ? *reinterpret_cast<uint32_t*>(event.packet->data) : UINT32_MAX;
        bool launch = false;
        if (type == PACKET_DELAY && event.packet->dataLength == sizeof(DelayPacket)) {
            const auto& settings = *reinterpret_cast<const DelayPacket*>(event.packet->data);
            if (settings.delay >= 0 && settings.delay <= 15 && settings.rollbackFrames >= 0 &&
                settings.rollbackFrames <= 16 &&
                (settings.delay != 0 || settings.rollbackFrames != 0)) {
                Controller_SetDelayFrames(settings.delay);
                Controller_SetRollbackFrames(settings.rollbackFrames);
                HookLog("LobbyFlow: applied P1 netplay settings %dD/%dR\n", settings.delay,
                        settings.rollbackFrames);
            }
        } else if (type == PACKET_LAUNCH_TRAINING &&
                   event.packet->dataLength == sizeof(LaunchTrainingPacket)) {
            // Validates the rosters and installs the boot configuration.
            launch = ApplyTrainingLaunchPacket(
                *reinterpret_cast<const LaunchTrainingPacket*>(event.packet->data));
            if (!launch) {
                enet_packet_destroy(event.packet);
                AbortMatchStart("Could not properly sync with the other player.");
                return false;
            }
        }
        enet_packet_destroy(event.packet);
        // Stop reading immediately: everything after the launch packet belongs
        // to the netplay service thread.
        if (launch) return true;
    }
    return false;
}

void UpdateMatchStart(bool cancelPressed)
{
    if (g_phase == Phase::Lobby) {
        MatchPairing pairing;
        if (!g_service->FindPairing(pairing)) return;
        g_pairing = pairing;
        g_matchIsHost = pairing.localSide == 0;
        g_launchSent = false;
        if (!SteamRoom_ConnectPeer(pairing.opponentId, g_matchIsHost)) {
            g_service->FinishMatch(false, false);
            g_ui->ShowNotice("Failed to create game session.");
            return;
        }
        char busy[128];
        snprintf(busy, sizeof(busy), "Connecting to %.48s...", pairing.opponentName);
        g_ui->SetBusy(busy);
        SetPhase(Phase::Connecting);
        HookLog("LobbyFlow: pairing found slot=%d side=%d opponent=%llu\n", pairing.slot,
                pairing.localSide, static_cast<unsigned long long>(pairing.opponentId));
        return;
    }

    if (g_phase == Phase::Connecting) {
        MatchPairing current;
        const bool stillPaired = g_service->FindPairing(current) &&
                                 current.opponentId == g_pairing.opponentId;
        if (!stillPaired) {
            AbortMatchStart("The other player is no longer ready.");
            return;
        }
        if (cancelPressed) {
            // Backing out while the connection is still being made.
            AbortMatchStart(nullptr);
            return;
        }
        // Teams may still change until the connection is up.
        g_pairing.localTeam = current.localTeam;
        g_pairing.opponentTeam = current.opponentTeam;
        if (SteamRoom_TakePeerFailed() ||
            GetTickCount64() - g_phaseStartTick > kConnectTimeoutMs) {
            AbortMatchStart("Connection to battle could not be made.");
            return;
        }
        bool isHost = false;
        if (SteamLobby_TakeNetworkHandles(&g_matchHost, &g_matchPeer, &isHost)) {
            g_matchIsHost = isHost;
            g_ui->SetBusy("Preparing the battle...");
            SetPhase(Phase::Handshake);
            HookLog("LobbyFlow: match connection established (%s)\n", isHost ? "P1" : "P2");
        }
        return;
    }

    if (g_phase == Phase::Handshake && PumpHandshake()) BeginLaunch();
}

// ---------------------------------------------------------------------------
// Title hold
// ---------------------------------------------------------------------------
void EnsureObjects()
{
    if (!g_service) {
        g_service.reset(new SteamRoomService());
        // Launched from a Steam invite ("+connect_lobby <id>"): go straight in.
        if (const char* invite = strstr(GetCommandLineA(), "+connect_lobby")) {
            const uint64_t lobbyId = strtoull(invite + strlen("+connect_lobby"), nullptr, 10);
            if (lobbyId) {
                HookLog("LobbyFlow: joining invited lobby %llu\n",
                        static_cast<unsigned long long>(lobbyId));
                g_service->Join(lobbyId);
            }
        }
    }
    if (!g_ui) g_ui.reset(new LobbyUi(*g_service, kRosterSource, LoadPrefs()));
}

void RunLobbyFrame(uint64_t titleArea)
{
    g_service->Pump();

    Input input = {};
    Native_ReadInput(input, g_ui->TextEntryActive());
    g_ui->Update(input);
    if (g_ui->TakePrefsDirty()) SavePrefs(g_ui->Prefs());
    Native_PlaySfx(g_ui->TakeSfx());

    if (g_ui->TakeMainMenuRequested()) {
        ReleaseToMainMenu(titleArea);
        return;
    }

    UpdateMatchStart(input.cancel);

    // Connection music while in a room, as in the launcher's battle setup.
    const bool inRoom = g_service->Room().inRoom;
    if (inRoom && !g_musicWanted && g_phase == Phase::Lobby) LaunchMusic_Start();
    if (!inRoom && g_musicWanted) LaunchMusic_BeginFadeOut();
    g_musicWanted = inRoom;

    if (Canvas* canvas = Native_BeginFrame()) {
        g_ui->Draw(*canvas);
        Native_EndFrame();
        g_nativeWaitFrames.store(0, std::memory_order_relaxed);
        // The lobby is on screen: lift the black left by the Online transition.
        if (g_fadeHold.exchange(false, std::memory_order_acq_rel))
            g_fadeInStart.store(GetTickCount64(), std::memory_order_release);
    } else {
        const uint32_t waited = g_nativeWaitFrames.fetch_add(1, std::memory_order_relaxed) + 1;
        if (waited == kNativeFailedFrames)
            HookLog("LobbyFlow: native lobby UI did not start within %u frames\n", waited);
    }
}

} // namespace

// ---------------------------------------------------------------------------
// Public interface
// ---------------------------------------------------------------------------
void LobbyFlow_Enable()
{
    g_enabled = true;
    HookLog("LobbyFlow: in-game lobby enabled\n");
}

bool LobbyFlow_IsEnabled()
{
    return g_enabled;
}

bool LobbyFlow_IsLobbyVisible()
{
    // If the native UI never came up, stop claiming the screen so the plugin's
    // own pause menu (and its Exit Game button) is reachable again.
    if (g_nativeWaitFrames.load(std::memory_order_relaxed) >= kNativeFailedFrames) return false;
    return g_enabled && !g_releasedToMainMenu &&
           (g_phase == Phase::Lobby || g_phase == Phase::Connecting || g_phase == Phase::Handshake);
}

void LobbyFlow_DrawOverlay()
{
    if (!g_enabled) return;

    const ULONGLONG now = GetTickCount64();
    float black = 0.0f;
    if (g_fadeHold.load(std::memory_order_acquire)) {
        const ULONGLONG elapsed = now - g_fadeOutStart.load(std::memory_order_relaxed);
        black = elapsed >= kOnlineFadeOutMs ? 1.0f
                                            : static_cast<float>(elapsed) / kOnlineFadeOutMs;
        if (elapsed > kOnlineFadeLimitMs) g_fadeHold.store(false, std::memory_order_release);
    } else if (const ULONGLONG fadeIn = g_fadeInStart.load(std::memory_order_acquire)) {
        const ULONGLONG elapsed = now - fadeIn;
        if (elapsed >= kOnlineFadeInMs)
            g_fadeInStart.store(0, std::memory_order_relaxed);
        else
            black = 1.0f - static_cast<float>(elapsed) / kOnlineFadeInMs;
    }
    if (black > 0.0f)
        ImGui::GetBackgroundDrawList()->AddRectFilled(
            ImVec2(0.0f, 0.0f), ImGui::GetIO().DisplaySize,
            IM_COL32(0, 0, 0, static_cast<int>(black * 255.0f + 0.5f)));

    const uint32_t waited = g_nativeWaitFrames.load(std::memory_order_relaxed);
    if (waited < kNativeSlowFrames) return;
    const bool failed = waited >= kNativeFailedFrames;
    const ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f),
                            ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::Begin("##LobbyStartup", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                 ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize |
                 ImGuiWindowFlags_NoInputs);
    if (failed) {
        ImGui::TextUnformatted("The in-game lobby could not start.");
        ImGui::TextUnformatted("Details are in Timestone.log. Press Esc for the menu,");
        ImGui::TextUnformatted("or launch with -launcher to use the launcher.");
    } else {
        ImGui::TextUnformatted("Starting lobby...");
    }
    ImGui::End();
}

bool LobbyFlow_InRoomMatch()
{
    return g_enabled && (g_phase == Phase::Launching || g_phase == Phase::InMatch);
}

void LobbyFlow_EnterTitleFromLogo()
{
    if (g_titleRequested) return;
    if (RequestTitleArea()) {
        g_titleRequested = true;
        HookLog("LobbyFlow: requested the title area from the logo sequence\n");
    }
}

bool LobbyFlow_TitleTick(uint64_t titleArea)
{
    if (g_phase == Phase::Launching) {
        if (TrainingBoot_HasNetworkFailed()) {
            // The opponent vanished between the handshake and the battle boot.
            HookLog("LobbyFlow: battle boot failed; staying in the lobby\n");
            FrameStepper_EndNetplaySession();
            SetNetworkHandles(nullptr, nullptr);
            TrainingBoot_PrepareNextBoot(false);
            g_service->FinishMatch(false, false);
            g_ui->SetBusy(nullptr);
            g_ui->ShowNotice("Connection to opponent has been lost.");
            SetPhase(Phase::Lobby);
            return false;
        }
        // Keep the room on screen under the busy overlay while the boot waits
        // for the native config load and the initial settings exchange.
        g_service->Pump();
        g_ui->Update(Input{});
        if (Canvas* canvas = Native_BeginFrame()) {
            g_ui->Draw(*canvas);
            Native_EndFrame();
        }
        return true; // keep driving BootIntoTraining until it commits
    }

    if (g_titleNeedsSetup) {
        // Everything from a previous visit died with the area change.
        g_titleNeedsSetup = false;
        Native_Invalidate();
        RetireNativeTitleUnits(titleArea);
        EnsureObjects();
        if (g_phase == Phase::Returning || g_phase == Phase::InMatch) {
            g_service->FinishMatch(false, false);
            g_ui->ReturnToRoom();
            SetPhase(Phase::Lobby);
        }
        if (g_pendingNotice[0]) {
            g_ui->ShowNotice(g_pendingNotice);
            g_pendingNotice[0] = '\0';
        }
        HookLog("LobbyFlow: title area ready for the lobby (area=%llx)\n",
                static_cast<unsigned long long>(titleArea));
    }
    // The area can respawn its menu units on a later frame of the same visit.
    RetireNativeTitleUnits(titleArea);

    RunLobbyFrame(titleArea);
    return g_phase == Phase::Launching;
}

bool LobbyFlow_IsReleasedToMainMenu()
{
    return g_enabled && g_releasedToMainMenu;
}

bool LobbyFlow_CanReopen()
{
    return LobbyFlow_IsReleasedToMainMenu() && TitleAreaIsRunning();
}

void LobbyFlow_RequestReopen()
{
    g_reopenRequested.store(true, std::memory_order_release);
}

bool LobbyFlow_ReleasedTitleTick(uint64_t titleArea)
{
    g_releasedTitleArea = titleArea;
    g_releasedTitleTick.store(GetTickCount64(), std::memory_order_relaxed);
    // "Online" on the main menu: switch once the fade has covered the screen.
    if (g_fadeHold.load(std::memory_order_relaxed) && !g_fadeInStart.load(std::memory_order_relaxed) &&
        GetTickCount64() - g_fadeOutStart.load(std::memory_order_relaxed) >= kOnlineFadeOutMs)
        g_reopenRequested.store(true, std::memory_order_release);
    if (!g_reopenRequested.exchange(false, std::memory_order_acq_rel)) return false;
    HookLog("LobbyFlow: reopening the lobby from the main menu\n");
    g_releasedToMainMenu = false;
    g_titleNeedsSetup = true;
    SetPhase(Phase::Lobby);
    LobbyFlow_TitleTick(titleArea);
    return true;
}

void LobbyFlow_OnBattleBootCommitted()
{
    if (!g_enabled) return;
    Native_Shutdown();
    g_titleNeedsSetup = true;
    if (g_ui) g_ui->SetBusy(nullptr);
    LaunchMusic_BeginFadeOut();
    g_musicWanted = false;
    if (g_phase == Phase::Launching) SetPhase(Phase::InMatch);
    HookLog("LobbyFlow: battle boot committed; lobby units released\n");
}

void LobbyFlow_GameTick()
{
    if (!g_enabled) return;

    if (g_phase == Phase::InMatch || g_phase == Phase::Returning) {
        // Keep the room alive (membership, chat backlog) while fighting, at a
        // rate that stays out of the match's way.
        static uint32_t tick = 0;
        if (g_service && (g_phase == Phase::Returning || ++tick % 20 == 0)) g_service->Pump();
    }
    if (g_phase != Phase::InMatch) {
        g_returnRequested.store(false, std::memory_order_relaxed);
        return;
    }

    if (Netplay_TakePeerDisconnected())
        LobbyFlow_RequestReturnToLobby("Connection to opponent has been lost.");
    if (!g_returnRequested.exchange(false, std::memory_order_acq_rel)) return;

    HookLog("LobbyFlow: leaving the match (%s)\n", g_returnReason[0] ? g_returnReason : "requested");
    FrameStepper_EndNetplaySession();
    // Netplay destroyed the ENet host; never leave the launch config pointing at it.
    SetNetworkHandles(nullptr, nullptr);
    TrainingBoot_PrepareNextBoot(false);
    snprintf(g_pendingNotice, sizeof(g_pendingNotice), "%s", g_returnReason);
    g_returnReason[0] = '\0';
    g_titleNeedsSetup = true;
    if (!RequestTitleArea()) HookLog("LobbyFlow: sArea is unavailable; cannot return to title\n");
    SetPhase(Phase::Returning);
}

void LobbyFlow_RequestReturnToLobby(const char* reason)
{
    if (!LobbyFlow_InRoomMatch()) return;
    if (!g_returnRequested.load(std::memory_order_relaxed))
        snprintf(g_returnReason, sizeof(g_returnReason), "%s", reason ? reason : "");
    g_returnRequested.store(true, std::memory_order_release);
}

bool LobbyFlow_WantsText()
{
    return LobbyFlow_IsLobbyVisible() && g_ui && g_ui->TextEntryActive();
}

void LobbyFlow_OnChar(uint32_t utf16)
{
    Native_OnChar(utf16);
}
