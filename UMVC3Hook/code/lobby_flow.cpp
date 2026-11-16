#include "../pch.h"
#include "lobby_flow.h"
#include "lobby_local.h"
#include "lobby_music.h"

#include "LaunchMenu.h"
#include "assist_names.h"
#include "connect_roster.h"
#include "controller.h"
#include "framestepper.h"
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
#include <algorithm>
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
// Menu chrome that outlives uMenuMain: the guide/status bar mode that
// uMenuMainBase::setup selects (sMvc3Guide+0x4B0, 0 = none) and the license
// card hotkey, which the menu's navigation state arms every frame
// (sLicense LicAttr bit 1) and only an explicit call disarms.
constexpr uintptr_t SMVC3GUIDE_GET_INSTANCE = 0x140001AE0;
constexpr uintptr_t SMVC3GUIDE_SET_UI_GUIDE_MODE = 0x1402837C0; // (this, mode)
constexpr uintptr_t SLICENSE_GET_INSTANCE = 0x140004750;
constexpr uintptr_t SLICENSE_DISARM_CARD = 0x140279680;
// aTitle::init switches sMain's frame-wait mode off, and in that mode the
// title area is only paced by VSync: with VSync off it runs at hundreds of
// frames per second. Frame-wait mode sleeps to the fixed 60 Hz step.
constexpr uintptr_t SMAIN_INSTANCE_PTR = 0x140E177E8;
constexpr size_t SMAIN_FRAME_WAIT = 0x58;
constexpr uintptr_t SMAIN_SET_FRAME_WAIT = 0x140522420; // (this, bool)
// Menu music, played the way the game does (sGSound system cues): the main
// menu theme is cues 13 and 14 (aTitle::init, uMenuTitle on Start) and the
// native lobby room is cue 16 (uMenuNetRoomMain__playRoomTransitionCue
// 0x14042F870). Before starting the menu theme aTitle::init fades whatever is
// playing with the call at 0x1402B0D20 (2000 ms there).
constexpr uintptr_t GSOUND_GET_INSTANCE = 0x140003AA0;
// (this, cRef* out, cue, variant, release, restart). `variant` picks the mix
// of a stage cue (71..80), `release` starts a cue that would otherwise only
// be queued (character and stage themes), `restart` replays one already up.
constexpr uintptr_t GSOUND_PLAY_SYSTEM_CUE = 0x1402AC5C0;
constexpr uintptr_t GSOUND_FADE_OUT_BGM = 0x1402B0D20;    // (this, milliseconds)
constexpr uint32_t kCueMenuThemeA = 13;
constexpr uint32_t kCueMenuThemeB = 14;
constexpr int32_t kBgmSwitchFadeMs = 600;
constexpr uintptr_t SGAMECONFIG_GET_INSTANCE = 0x140001AB0;
constexpr ULONGLONG kOnlineFadeOutMs = 200;
constexpr ULONGLONG kOnlineFadeInMs = 250;
constexpr ULONGLONG kOnlineFadeLimitMs = 6000; // never leave the screen black
// sGameUi boot state selects what aTitle::init builds: 0 the press-start
// screen, 1 the main menu with its music and camera.
constexpr size_t SGAMEUI_BOOT_STATE = 0x5798;
// Saved cursor of the main menu (sGameUi::mMenuCursorPos[0]), read by
// uMenuMain__getInitialSelection.
constexpr size_t SGAMEUI_MAINMENU_CURSOR = 0x57B0;
constexpr int32_t kBootStateTitle = 0;
constexpr int32_t kBootStateMainMenu = 1;
// Written to the outgoing aTitle's mPresentationMode while a reload is queued.
// aTitle::init sets that field to 0 or 1, so the first tick that does not see
// the marker belongs to the rebuilt area (which may reuse the same address).
constexpr int32_t kReloadMarker = 0x5A5A0001;
constexpr ULONGLONG kReloadTimeoutMs = 8000;
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
// Game thread only.
bool g_titleReloadPending = false;
int32_t g_titleReloadSavedMode = 0;
ULONGLONG g_titleReloadStart = 0;
bool g_onlineChosen = false;        // main menu: Online picked, fading out
bool g_releaseNeedsInput = false;   // main menu: input state owed after the reload
ULONGLONG g_mainMenuReleaseAt = 0;  // lobby: MAIN MENU picked, fading out
bool g_entrancePending = true;      // lobby: slide-in owed once it can draw
constexpr uint32_t kEntranceFadeDelay = 8; // frames of the 250 ms fade to sit out

std::unique_ptr<SteamRoomService> g_service;
std::unique_ptr<LobbyUi> g_ui;
ConnectRoster g_rosterPrefs;
SavedRoster g_presets[3] = {};
size_t g_presetCount = 0;
bool g_presetsLoaded = false;

MatchPairing g_pairing = {};
ENetHost* g_matchHost = nullptr;
ENetPeer* g_matchPeer = nullptr;
ULONGLONG g_bothReadyTick = 0; // when both players finished the pre-match steps
// Set by "Return to Lobby": back in the room the player is stood up.
std::atomic<bool> g_leaveSeatOnReturn{false};
bool g_matchIsHost = false;
ULONGLONG g_phaseStartTick = 0;
bool g_launchSent = false;

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

    prefs.presetIndex = GetPrivateProfileIntA("Lobby", "Preset", -1, path);
    if (prefs.presetIndex < -1 || prefs.presetIndex > 2) prefs.presetIndex = -1;
    GetPrivateProfileStringA("Lobby", "RoomName", "", prefs.create.name,
                             sizeof(prefs.create.name), path);
    prefs.create.maxMembers = GetPrivateProfileIntA("Lobby", "RoomSize", kMaxMembers, path);
    prefs.create.slotCount = GetPrivateProfileIntA("Lobby", "RoomSlots", 2, path);
    const int visibility = GetPrivateProfileIntA("Lobby", "RoomVisibility", 0, path);
    prefs.create.visibility = visibility == 1 ? Visibility::FriendsOnly
                            : visibility == 2 ? Visibility::Private : Visibility::Public;
    prefs.create.firstTo = GetPrivateProfileIntA("Lobby", "RoomFirstTo", 1, path);
    prefs.create.rotation = GetPrivateProfileIntA("Lobby", "RoomRotation", 1, path) == 2
        ? Rotation::WinnerLeaves : Rotation::WinnerStays;
    prefs.create.music = GetPrivateProfileIntA("Lobby", "RoomMusic", lobby::kMusicDefault, path);
    if (lobby::FindMusicTrack(prefs.create.music) < 0) prefs.create.music = lobby::kMusicDefault;
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
        {"Lobby", "Preset", std::to_string(prefs.presetIndex)},
        {"Lobby", "RoomName", prefs.create.name},
        {"Lobby", "RoomSize", std::to_string(prefs.create.maxMembers)},
        {"Lobby", "RoomSlots", std::to_string(prefs.create.slotCount)},
        {"Lobby", "RoomVisibility", std::to_string(static_cast<int>(prefs.create.visibility))},
        {"Lobby", "RoomMusic", std::to_string(prefs.create.music)},
        {"Lobby", "RoomFirstTo", std::to_string(prefs.create.firstTo)},
        {"Lobby", "RoomRotation", std::to_string(static_cast<int>(prefs.create.rotation))},
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


// Called every title-area frame: each reload of the area clears the mode.
void EnsureFramePacing()
{
    Native_EnsurePadSlotsActive();
    const uint64_t main = *reinterpret_cast<uint64_t*>(_addr(SMAIN_INSTANCE_PTR));
    if (!main || *reinterpret_cast<uint8_t*>(main + SMAIN_FRAME_WAIT)) return;
    using SetFrameWait = void(__fastcall*)(uint64_t, bool);
    reinterpret_cast<SetFrameWait>(_addr(SMAIN_SET_FRAME_WAIT))(main, true);
    HookLog("LobbyFlow: enabled sMain frame-wait pacing for the title area\n");
}

// The menu theme is two synchronised layers (cue 13 -> bgm_gm_035, cue 14 ->
// bgm_gm_036, stream type 10 in sound\bgm\bgm). Requesting the cues only
// queues them: the main menu itself (uMenuMain vfn38, 0x1403E89A0) then mutes
// the layer the player's BGM setting does not use and releases both, and
// without that step they never become audible.
constexpr uintptr_t SGAMECONFIG_GET_BGM_STYLE = 0x140251640;   // (config) -> 0 or not
constexpr uintptr_t GSOUND_RESOLVE_SOUND_REF = 0x1402AA2B0;    // (this, cRef* out, index)
constexpr uintptr_t GSOUND_SET_CUE_CHANNEL_VOLUME = 0x1402AF010; // (this, ref, cue, 0, channel, vol)
constexpr uintptr_t GSOUND_RELEASE_CUE = 0x1402AEAE0;          // (this, cRef* out, cue)
constexpr uint32_t kAllChannels = 0xFFFFFFFFu;
// Set while the title area reloads with the menu theme already playing, in
// either direction between the lobby and the real main menu. The game would
// otherwise fade it (the dying main menu) or restart it (aTitle::init for the
// main-menu boot state); both are swallowed so the music runs straight through.
bool g_keepMenuTheme = false;
uint32_t g_menuThemeKickFrames = 0; // repeat the release once, a little later

// --- Set in progress (first to N) ---------------------------------------------
// A room match is a set: games are counted from the result phase the game
// reaches on both peers (the simulation is shared, so both count the same),
// rematches run until someone has the room's "first to" number, then both
// players return to the room on their own.
constexpr uint32_t kResultCountTicks = 30;       // result phase must hold this long
constexpr uint32_t kResultGoneTicks = 120;       // ...and be gone this long to re-arm
constexpr uint32_t kSetEndDelayTicks = 150;      // let the win pose play before leaving
int g_setFirstTo = 1;
int g_setWins[2] = {0, 0}; // [0] local, [1] opponent
bool g_setOver = false;
uint32_t g_setOverTicks = 0;
bool g_resultSeen = false;    // set by the result hooks, consumed each game tick
int32_t g_resultWinTeam = -1;
uint32_t g_resultTicks = 0;
uint32_t g_resultGoneTicks = 0;
bool g_gameCounted = false;

void ResetSet(int firstTo)
{
    g_setFirstTo = firstTo < 0 ? 1 : firstTo; // 0: endless, no set to win
    g_setWins[0] = g_setWins[1] = 0;
    g_setOver = false;
    g_setOverTicks = 0;
    g_resultSeen = false;
    g_resultTicks = 0;
    g_resultGoneTicks = 0;
    g_gameCounted = false;
}

enum class Bgm { None, MainMenu, Room };
Bgm g_bgm = Bgm::None; // what the lobby believes is playing (game thread)
int32_t g_roomTrack = lobby::kMusicDefault; // lobby_music.h id, while g_bgm is Room

// What should be playing right now: the menu theme on the lobby's own menus,
// and in a room whatever its host chose (the native room track by default).
// A host who picks the menu theme gets the same two-layer playback as the
// menus, so it also carries on unbroken when the room is entered or left.
struct WantedBgm {
    Bgm bgm;
    int32_t track;
};
WantedBgm CurrentWantedBgm()
{
    const bool inRoom = g_service && g_service->Room().inRoom;
    // Outside a room the only other music is the track being auditioned in
    // the create screen's list; closing the list returns to the menu theme.
    const int32_t preview = !inRoom && g_ui ? g_ui->PreviewMusic() : -1;
    if (!inRoom && preview < 0) return {Bgm::MainMenu, lobby::kMusicDefault};
    const int32_t track = inRoom ? g_service->Room().music : preview;
    if (lobby::MusicCue(track) == lobby::kMusicCueMainMenu)
        return {Bgm::MainMenu, lobby::kMusicDefault};
    return {Bgm::Room, track};
}

void PlayRoomTrack(void* sound, int32_t track)
{
    using PlayCue = void*(__fastcall*)(void*, void*, uint32_t, int32_t, char, char);
    alignas(16) uint8_t handle[32] = {};
    reinterpret_cast<PlayCue>(_addr(GSOUND_PLAY_SYSTEM_CUE))(
        sound, handle, static_cast<uint32_t>(lobby::MusicCue(track)), lobby::MusicVariant(track),
        1, 1);
    g_roomTrack = track;
    HookLog("LobbyFlow: room music cue %d variant %d (started=%d)\n", lobby::MusicCue(track),
            lobby::MusicVariant(track),
            *reinterpret_cast<uint64_t*>(handle + 8) != UINT64_MAX ? 1 : 0);
}

// Mutes the unused layer and releases both, as uMenuMain does. Returns false
// if the theme's cue is not queued or playing at all.
bool ReleaseMenuThemeLayers(void* sound)
{
    using GetConfig = void*(__fastcall*)();
    using GetStyle = int32_t(__fastcall*)(void*);
    using ResolveRef = void*(__fastcall*)(void*, void*, uint32_t);
    using SetChannelVolume = void(__fastcall*)(void*, void*, uint32_t, uint64_t, uint32_t, float);
    using ReleaseCue = void*(__fastcall*)(void*, void*, uint32_t);
    void* config = reinterpret_cast<GetConfig>(_addr(SGAMECONFIG_GET_INSTANCE))();
    const int32_t style =
        config ? reinterpret_cast<GetStyle>(_addr(SGAMECONFIG_GET_BGM_STYLE))(config) : 0;
    alignas(16) uint8_t ref[32] = {};
    void* bank = reinterpret_cast<ResolveRef>(_addr(GSOUND_RESOLVE_SOUND_REF))(sound, ref, 0);
    reinterpret_cast<SetChannelVolume>(_addr(GSOUND_SET_CUE_CHANNEL_VOLUME))(
        sound, bank, style == 0 ? kCueMenuThemeB : kCueMenuThemeA, 0, kAllChannels, 0.0f);
    alignas(16) uint8_t handle[32] = {};
    const ReleaseCue release = reinterpret_cast<ReleaseCue>(_addr(GSOUND_RELEASE_CUE));
    release(sound, handle, kCueMenuThemeA);
    const bool present = *reinterpret_cast<uint64_t*>(handle + 8) != UINT64_MAX;
    release(sound, handle, kCueMenuThemeB);
    return present;
}

// Starts the menu theme, or lets it carry on if it is already there.
void PlayMenuTheme(void* sound, bool restart)
{
    using PlayCue = void*(__fastcall*)(void*, void*, uint32_t, int32_t, char, char);
    const PlayCue play = reinterpret_cast<PlayCue>(_addr(GSOUND_PLAY_SYSTEM_CUE));
    if (!restart && ReleaseMenuThemeLayers(sound)) {
        HookLog("LobbyFlow: menu theme already present; kept\n");
        return;
    }
    alignas(16) uint8_t handle[32] = {};
    play(sound, handle, kCueMenuThemeA, 0, 0, 1);
    play(sound, handle, kCueMenuThemeB, 0, 0, 1);
    const bool present = ReleaseMenuThemeLayers(sound);
    g_menuThemeKickFrames = 20;
    HookLog("LobbyFlow: menu theme started (queued=%d)\n", present ? 1 : 0);
}

void SetBgm(Bgm wanted, int32_t track = lobby::kMusicDefault)
{
    if (wanted == g_bgm && (wanted != Bgm::Room || track == g_roomTrack)) return;
    using GetInstance = void*(__fastcall*)();
    void* sound = reinterpret_cast<GetInstance>(_addr(GSOUND_GET_INSTANCE))();
    if (!sound) return;
    using FadeOut = void(__fastcall*)(void*, int32_t);
    if (g_bgm != Bgm::None)
        reinterpret_cast<FadeOut>(_addr(GSOUND_FADE_OUT_BGM))(sound, kBgmSwitchFadeMs);
    if (wanted == Bgm::MainMenu) {
        PlayMenuTheme(sound, true);
    } else if (wanted == Bgm::Room) {
        PlayRoomTrack(sound, track);
    }
    HookLog("LobbyFlow: music %d -> %d\n", static_cast<int>(g_bgm), static_cast<int>(wanted));
    g_bgm = wanted;
}

// The menu theme only played when it was started where the game starts it:
// at the end of aTitle::init, before the area's load phase. Requested later
// from the running title area (boot state 0) it stayed silent, so the lobby
// starts its music from a hook on aTitle::init, with the same fade-then-play
// sequence init uses for its main-menu boot states.
constexpr uintptr_t ATITLE_INIT = 0x140216940;
using TitleInit = void(__fastcall*)(uint64_t, uint64_t, uint64_t, uint64_t);
TitleInit g_originalTitleInit = nullptr;
bool g_titleInitHookInstalled = false;

void __fastcall Hook_TitleInit(uint64_t area, uint64_t a, uint64_t b, uint64_t c)
{
    g_originalTitleInit(area, a, b, c);
    // A reload into the real main menu starts its own theme in init.
    if (!g_enabled || g_releasedToMainMenu) return;
    // Booting a battle also passes through the title area; no menu music then.
    if (g_phase == Phase::Launching || g_phase == Phase::InMatch) return;
    using GetInstance = void*(__fastcall*)();
    void* sound = reinterpret_cast<GetInstance>(_addr(GSOUND_GET_INSTANCE))();
    if (!sound) return;
    using FadeOut = void(__fastcall*)(void*, int32_t);
    const WantedBgm wanted = CurrentWantedBgm();
    const bool inRoom = wanted.bgm == Bgm::Room;
    if (inRoom) {
        reinterpret_cast<FadeOut>(_addr(GSOUND_FADE_OUT_BGM))(sound, 2000);
        PlayRoomTrack(sound, wanted.track);
    } else if (g_bgm == Bgm::MainMenu) {
        // Coming from the real main menu: let its theme carry on if it
        // survived the reload, otherwise start it again.
        PlayMenuTheme(sound, false);
    } else {
        reinterpret_cast<FadeOut>(_addr(GSOUND_FADE_OUT_BGM))(sound, 2000);
        PlayMenuTheme(sound, true);
    }
    g_bgm = inRoom ? Bgm::Room : Bgm::MainMenu;
    HookLog("LobbyFlow: music started from aTitle::init (%s)\n", inRoom ? "room" : "menu theme");
}

using FadeOutBgm = void(__fastcall*)(void*, int32_t);
FadeOutBgm g_originalFadeOutBgm = nullptr;

void __fastcall Hook_FadeOutBgm(void* sound, int32_t milliseconds)
{
    if (g_keepMenuTheme) return;
    g_originalFadeOutBgm(sound, milliseconds);
}

void InstallTitleInitHook()
{
    if (g_titleInitHookInstalled) return;
    g_titleInitHookInstalled = true;
    void* fade = reinterpret_cast<void*>(_addr(GSOUND_FADE_OUT_BGM));
    if (MH_CreateHook(fade, reinterpret_cast<void*>(&Hook_FadeOutBgm),
                      reinterpret_cast<void**>(&g_originalFadeOutBgm)) != MH_OK ||
        MH_EnableHook(fade) != MH_OK)
        HookLog("LobbyFlow: music fade hook failed; the menu theme may restart on switches\n");
    void* target = reinterpret_cast<void*>(_addr(ATITLE_INIT));
    MH_STATUS status = MH_CreateHook(target, reinterpret_cast<void*>(&Hook_TitleInit),
                                     reinterpret_cast<void**>(&g_originalTitleInit));
    if (status == MH_OK) status = MH_EnableHook(target);
    if (status != MH_OK)
        HookLog("LobbyFlow: aTitle::init hook failed (%d); menu music may be missing\n",
                static_cast<int>(status));
}

void StartBlack()
{
    if (g_fadeHold.load(std::memory_order_relaxed)) return;
    g_fadeInStart.store(0, std::memory_order_relaxed);
    g_fadeOutStart.store(GetTickCount64(), std::memory_order_relaxed);
    g_fadeHold.store(true, std::memory_order_release);
}

void LiftBlack()
{
    if (g_fadeHold.exchange(false, std::memory_order_acq_rel))
        g_fadeInStart.store(GetTickCount64(), std::memory_order_release);
}

// Rebuilds the title area the way the game does: aTitle::init kills every
// unit and builds the screen the boot state asks for. Killing or spawning
// menu units by hand left layouts, the status bar and duplicate menus behind.
void BeginTitleReload(uint64_t titleArea, int32_t bootState, bool keepMenuTheme)
{
    g_keepMenuTheme = keepMenuTheme;
    using GetInstance = uint64_t(__fastcall*)();
    if (const uint64_t gameUi = reinterpret_cast<GetInstance>(_addr(SGAMEUI_GET_INSTANCE))())
        *reinterpret_cast<int32_t*>(gameUi + SGAMEUI_BOOT_STATE) = bootState;
    int32_t& mode = *reinterpret_cast<int32_t*>(titleArea + ATITLE_PRESENTATION_MODE);
    if (!g_titleReloadPending) g_titleReloadSavedMode = mode;
    StartBlack();
    if (!RequestTitleArea()) {
        HookLog("LobbyFlow: sArea is unavailable; cannot reload the title area\n");
        return;
    }
    mode = kReloadMarker;
    g_titleReloadPending = true;
    g_titleReloadStart = GetTickCount64();
}

// True while `titleArea` is still the outgoing area of a queued reload.
bool TitleReloadPending(uint64_t titleArea)
{
    if (!g_titleReloadPending) return false;
    int32_t& mode = *reinterpret_cast<int32_t*>(titleArea + ATITLE_PRESENTATION_MODE);
    if (mode == kReloadMarker) {
        if (GetTickCount64() - g_titleReloadStart < kReloadTimeoutMs) return true;
        HookLog("LobbyFlow: title area reload did not happen; carrying on in place\n");
        mode = g_titleReloadSavedMode;
    }
    g_titleReloadPending = false;
    g_keepMenuTheme = false;
    return false;
}

// The lobby has no native menu under it, so nothing the main menu switched on
// may stay: the status bar and the Ctrl license card would draw over it.
void HideNativeMenuChrome()
{
    using GetInstance = void*(__fastcall*)();
    if (void* guide = reinterpret_cast<GetInstance>(_addr(SMVC3GUIDE_GET_INSTANCE))()) {
        using SetUiGuideMode = void(__fastcall*)(void*, uint64_t);
        reinterpret_cast<SetUiGuideMode>(_addr(SMVC3GUIDE_SET_UI_GUIDE_MODE))(guide, 0);
    }
    if (void* license = reinterpret_cast<GetInstance>(_addr(SLICENSE_GET_INSTANCE))()) {
        using Disarm = void(__fastcall*)(void*);
        reinterpret_cast<Disarm>(_addr(SLICENSE_DISARM_CARD))(license);
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
    // No menu takes input while the title area is being rebuilt.
    if (g_titleReloadPending) return 1;
    if (!IsReleasedMainMenu(menu)) return g_originalMenuNavigation(menu, out);
    if (g_onlineChosen) return 1;

    // Take the decide on Online before the menu sees it: its own handler
    // would start the native network entry.
    Input input = {};
    Native_ReadInput(input, false);
    if (input.confirm &&
        *reinterpret_cast<int32_t*>(static_cast<uint8_t*>(menu) + MENU_CURSOR) ==
            MAINMENU_ITEM_ONLINE) {
        HookLog("LobbyFlow: Online chosen on the main menu\n");
        Native_PlaySfx(SFX_CONFIRM);
        g_onlineChosen = true;
        StartBlack();
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
        g_onlineChosen = true;
        StartBlack();
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

// On the main menu the game only reads the hardware slot that player 0 is
// mapped to, and only switches from keyboard to pad when that slot reports a
// press (0x1402B42C0). The press-start screen normally maps player 0 to
// whichever pad pressed Start; the lobby skips it, so do the same here for
// whichever pad is being used.
void FollowActivePad()
{
    const uint64_t manager = *reinterpret_cast<uint64_t*>(_addr(SMVC3MANAGER_INSTANCE_PTR));
    const uint64_t pad = *reinterpret_cast<uint64_t*>(_addr(SPAD_INSTANCE_PTR));
    if (!manager || !pad) return;
    int32_t* assign = reinterpret_cast<int32_t*>(manager + MANAGER_PAD_ASSIGN);
    for (int32_t slot = 0; slot < 4; ++slot) {
        const uint8_t* slotObject = reinterpret_cast<const uint8_t*>(pad + 0x58 + slot * 0x2E0);
        const bool active = slotObject[0x08] != 0;
        const uint32_t pressed = *reinterpret_cast<const uint32_t*>(slotObject + 0x16C);
        if (!active || !pressed || assign[0] == slot) continue;
        // Same exchange as the game's own assignment (0x14025B090).
        for (int i = 1; i < 4; ++i)
            if (assign[i] == slot) assign[i] = assign[0];
        HookLog("LobbyFlow: player 1 menu input moved from pad slot %d to %d\n", assign[0], slot);
        assign[0] = slot;
        return;
    }
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
    // The rebuilt title area starts the main menu theme itself; if it is
    // already playing it carries straight on.
    const bool themePlaying = g_bgm == Bgm::MainMenu;
    if (g_bgm == Bgm::Room) SetBgm(Bgm::None);
    g_bgm = Bgm::MainMenu;
    Native_Shutdown();
    InstallMainMenuHooks();
    g_releasedTitleArea = titleArea;
    g_releasedTitleTick.store(GetTickCount64(), std::memory_order_relaxed);
    g_reopenRequested.store(false, std::memory_order_relaxed);
    g_onlineChosen = false;
    g_releaseNeedsInput = true;
    g_releasedToMainMenu = true;
    // The first visit of a session lands on Online, the way back to the
    // lobby; later visits keep wherever the player left the cursor.
    static bool firstVisit = true;
    if (firstVisit) {
        firstVisit = false;
        using GetInstance = uint64_t(__fastcall*)();
        if (const uint64_t gameUi = reinterpret_cast<GetInstance>(_addr(SGAMEUI_GET_INSTANCE))())
            *reinterpret_cast<int32_t*>(gameUi + SGAMEUI_MAINMENU_CURSOR) = MAINMENU_ITEM_ONLINE;
    }
    BeginTitleReload(titleArea, kBootStateMainMenu, themePlaying);
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
    if (g_service) {
        g_service->FinishMatch(false, false);
        // A failure with both players still seated would pair them again on
        // the next frame and fail the same way; the seat is given up instead.
        if (notice) g_service->Stand();
    }
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
    // "Auto" is the ping measured on this connection, which has been up since
    // both players accepted the match: the same figure the prompt showed.
    const uint32_t rtt = SteamTransport_GetRTT();
    LaunchMenu_GetAutomaticNetplaySettings(rtt / 2, &delay, &rollback);
    // The players' own delay picks. Both run the same delay, so the higher
    // pick is used (a player who asked for more must not get less); "Auto"
    // stands for the value measured from the ping. Moving away from the
    // automatic delay keeps the same total budget by adjusting rollback.
    const auto wanted = [&](int pick) { return pick > kDelayPickAuto ? pick - 1 : delay; };
    const int32_t chosen = std::max(wanted(g_pairing.localDelayPick),
                                    wanted(g_pairing.opponentDelayPick));
    if (chosen != delay) {
        const int32_t budget = delay + rollback;
        delay = chosen;
        rollback = std::max(rollback, budget - delay);
        if (rollback > 16) rollback = 16;
    }
    if (delay == 0 && rollback == 0) rollback = 1;
    HookLog("LobbyFlow: delay picks local=%d opponent=%d measured ping=%u ms -> %dD/%dR\n",
            g_pairing.localDelayPick, g_pairing.opponentDelayPick, rtt / 2, delay, rollback);
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
    g_leaveSeatOnReturn.store(false, std::memory_order_relaxed);
    if (g_service) {
        ResetSet(g_service->Room().settings.firstTo);
        g_service->SetLocalStatus(MemberStatus::Fighting);
    }
    SetPhase(Phase::Launching);
    HookLog("LobbyFlow: launching match vs %s as P%d\n", g_pairing.opponentName,
            g_pairing.localSide + 1);
}

// Services the fresh connection until the launch parameters are exchanged.
// Returns true when the match can launch.
bool PumpHandshake(bool bothReady)
{
    const ULONGLONG now = GetTickCount64();
    // The connection is up while the players are still answering the
    // pre-match prompts; the exchange is only on the clock once both are done.
    if (!bothReady) g_bothReadyTick = 0;
    else if (!g_bothReadyTick) g_bothReadyTick = now;
    if (g_bothReadyTick && now - g_bothReadyTick > kHandshakeTimeoutMs) {
        AbortMatchStart("Could not properly sync with the other player.");
        return false;
    }
    const ULONGLONG elapsed = now - g_phaseStartTick; // age of the connection

    if (g_matchIsHost) {
        // Let Steam's route settle so the automatic settings see a real ping,
        // and hold the launch until both players have finished their prompts.
        const bool pingKnown = SteamTransport_GetRTT() != 0;
        if (!bothReady || elapsed < kPingSettleMs || (!pingKnown && elapsed < kPingWaitLimitMs)) {
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
            g_service->Stand(); // do not retry every frame
            g_ui->ShowNotice("Failed to create game session.");
            return;
        }
        // No overlay: the connection is made behind the pre-match prompts so
        // they can show the ping actually measured between the two players.
        g_bothReadyTick = 0;
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
            // Someone gave their seat up; the prompts already say so.
            AbortMatchStart(nullptr);
            return;
        }
        if (current.bothReady) {
            // Both got through the prompts before the connection was up.
            char busy[128];
            snprintf(busy, sizeof(busy), "Connecting to %.48s...", g_pairing.opponentName);
            g_ui->SetBusy(busy);
        }
        if (SteamRoom_TakePeerFailed() ||
            GetTickCount64() - g_phaseStartTick > kConnectTimeoutMs) {
            AbortMatchStart("Connection to battle could not be made.");
            return;
        }
        bool isHost = false;
        if (SteamLobby_TakeNetworkHandles(&g_matchHost, &g_matchPeer, &isHost)) {
            g_matchIsHost = isHost;
            SetPhase(Phase::Handshake);
            HookLog("LobbyFlow: match connection established (%s)\n", isHost ? "P1" : "P2");
        }
        return;
    }

    if (g_phase != Phase::Handshake) return;
    MatchPairing current;
    if (!g_service->FindPairing(current) || current.opponentId != g_pairing.opponentId) {
        AbortMatchStart(nullptr);
        return;
    }
    // Teams and delay picks are chosen in the prompts, after the connection.
    g_pairing.localTeam = current.localTeam;
    g_pairing.opponentTeam = current.opponentTeam;
    g_pairing.localDelayPick = current.localDelayPick;
    g_pairing.opponentDelayPick = current.opponentDelayPick;
    // The prompts show this in place of the room's estimate, and "Auto" is
    // worked out from it on both sides.
    if (const uint32_t rtt = SteamTransport_GetRTT())
        g_service->SetMeasuredPing(g_pairing.opponentId, static_cast<int>(rtt / 2));
    if (current.bothReady && !g_bothReadyTick) g_ui->SetBusy("Preparing the battle...");
    // Player 2 always listens: player 1 launches as soon as it sees both
    // ready, which can be a moment before this side's view of the room agrees.
    if (PumpHandshake(current.bothReady)) BeginLaunch();
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
    // Lobby test build: bring up the second player's game now that this one has
    // finished starting (no-op otherwise, and only ever once).
    LocalLobby_SpawnSecondInstance();
}

void RunLobbyFrame(uint64_t titleArea)
{
    g_service->Pump();

    Input input = {};
    Native_ReadInput(input, g_ui->TextEntryActive());
    // MAIN MENU was picked: the lobby stays up, inert, until black covers it.
    if (g_mainMenuReleaseAt) input = Input{};
    g_ui->Update(input);
    if (g_ui->TakePrefsDirty()) SavePrefs(g_ui->Prefs());
    const uint32_t sfx = g_ui->TakeSfx();
    if (sfx & SFX_MATCH_FOUND) Native_FocusGameWindow();
    Native_PlaySfx(sfx);

    if (g_ui->TakeMainMenuRequested() && !g_mainMenuReleaseAt) {
        StartBlack();
        g_mainMenuReleaseAt = GetTickCount64() + kOnlineFadeOutMs;
    }
    if (g_mainMenuReleaseAt && GetTickCount64() >= g_mainMenuReleaseAt) {
        g_mainMenuReleaseAt = 0;
        ReleaseToMainMenu(titleArea);
        return;
    }

    UpdateMatchStart(input.cancel);


    if (Canvas* canvas = Native_BeginFrame()) {
        if (g_entrancePending) {
            // First frame the lobby can be seen on this visit: play the
            // screen's slide-in now, after most of any fade from black.
            g_entrancePending = false;
            g_ui->PlayEntrance(g_fadeHold.load(std::memory_order_relaxed) ? kEntranceFadeDelay : 0);
        }
        g_ui->Draw(*canvas);
        Native_EndFrame();
        g_nativeWaitFrames.store(0, std::memory_order_relaxed);
        // The game's own music: the main menu theme on the lobby's menus,
        // the native lobby track once inside a room. Started only once the
        // lobby is on screen, well after the title area's sound has loaded.
        if (g_phase != Phase::Launching) {
            const WantedBgm wanted = CurrentWantedBgm();
            SetBgm(wanted.bgm, wanted.track);
        }
        if (g_menuThemeKickFrames && --g_menuThemeKickFrames == 0 && g_bgm == Bgm::MainMenu) {
            using GetInstance = void*(__fastcall*)();
            if (void* sound = reinterpret_cast<GetInstance>(_addr(GSOUND_GET_INSTANCE))())
                ReleaseMenuThemeLayers(sound);
        }
        // The lobby is on screen: lift the black left by the transition into
        // it (not the one that is covering it on the way out).
        if (!g_mainMenuReleaseAt) LiftBlack();
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

    // Set score during a room match, top centre between the health bars.
    if (g_phase == Phase::InMatch && g_setFirstTo != 1) {
        char score[48], format[12] = "";
        if (g_setFirstTo > 0) snprintf(format, sizeof(format), "FT%d    ", g_setFirstTo);
        snprintf(score, sizeof(score), "%s%d - %d", format,
                 g_setWins[g_pairing.localSide == 0 ? 0 : 1],
                 g_setWins[g_pairing.localSide == 0 ? 1 : 0]);
        const ImVec2 display = ImGui::GetIO().DisplaySize;
        ImFont* font = ImGui::GetFont();
        const float size = display.y * 0.03f;
        const ImVec2 extent = font->CalcTextSizeA(size, 100000.0f, 0.0f, score);
        const ImVec2 at((display.x - extent.x) * 0.5f, display.y * 0.012f);
        ImDrawList* draw = ImGui::GetBackgroundDrawList();
        draw->AddText(font, size, ImVec2(at.x + 2.0f, at.y + 2.0f), IM_COL32(0, 0, 0, 220), score);
        draw->AddText(font, size, at, IM_COL32(255, 255, 255, 255), score);
    }

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
    InstallTitleInitHook();
    if (RequestTitleArea()) {
        g_titleRequested = true;
        HookLog("LobbyFlow: requested the title area from the logo sequence\n");
    }
}

bool LobbyFlow_TitleTick(uint64_t titleArea)
{
    EnsureFramePacing();
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

    // Reopening from the main menu: wait for the rebuilt title area.
    if (TitleReloadPending(titleArea)) return false;

    if (g_titleNeedsSetup) {
        // Everything from a previous visit died with the area change.
        g_titleNeedsSetup = false;
        g_entrancePending = true;
        Native_Invalidate();
        RetireNativeTitleUnits(titleArea);
        ApplyPressStartState();
        EnsureObjects();
        if (g_phase == Phase::Returning || g_phase == Phase::InMatch) {
            // Only a set played to its end goes on the room record.
            g_service->FinishMatch(g_setOver, g_setWins[0] > g_setWins[1]);
            if (g_leaveSeatOnReturn.exchange(false, std::memory_order_acq_rel)) {
                // Walking out of a match is walking away from the slot: no
                // seat, and no place in its queue either.
                g_service->Stand();
                g_service->LeaveQueue();
                HookLog("LobbyFlow: left the match by choice; seat given up\n");
            }
            ResetSet(1);
            g_ui->ReturnToRoom();
            g_entrancePending = false; // back from a match: the room is simply there
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
    HideNativeMenuChrome();

    RunLobbyFrame(titleArea);
    return g_phase == Phase::Launching;
}

bool LobbyFlow_ShouldSkipSystemCue(uint32_t cue)
{
    return g_keepMenuTheme && (cue == kCueMenuThemeA || cue == kCueMenuThemeB);
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
    EnsureFramePacing();
    g_releasedTitleArea = titleArea;
    g_releasedTitleTick.store(GetTickCount64(), std::memory_order_relaxed);
    // Leaving the lobby: the outgoing area idles until the rebuilt one runs.
    if (TitleReloadPending(titleArea)) return true;
    if (g_releaseNeedsInput) {
        // First frame of the rebuilt area, with the main menu in place.
        g_releaseNeedsInput = false;
        ApplyPressStartState();
        LiftBlack();
        HookLog("LobbyFlow: main menu ready (area=%llx)\n",
                static_cast<unsigned long long>(titleArea));
    }

    FollowActivePad();

    // "Online" on the main menu: switch once the fade has covered the screen.
    if (g_onlineChosen &&
        GetTickCount64() - g_fadeOutStart.load(std::memory_order_relaxed) >= kOnlineFadeOutMs)
        g_reopenRequested.store(true, std::memory_order_release);
    if (!g_reopenRequested.exchange(false, std::memory_order_acq_rel)) return false;

    HookLog("LobbyFlow: reopening the lobby from the main menu\n");
    g_onlineChosen = false;
    g_releasedToMainMenu = false;
    g_titleNeedsSetup = true;
    SetPhase(Phase::Lobby);
    BeginTitleReload(titleArea, kBootStateTitle, g_bgm == Bgm::MainMenu);
    return true;
}

void LobbyFlow_OnBattleBootCommitted()
{
    if (!g_enabled) return;
    Native_Shutdown();
    g_titleNeedsSetup = true;
    if (g_ui) g_ui->SetBusy(nullptr);
    SetBgm(Bgm::None); // the battle brings its own music
    if (g_phase == Phase::Launching) SetPhase(Phase::InMatch);
    HookLog("LobbyFlow: battle boot committed; lobby units released\n");
}

const char* SetResultText()
{
    static char text[96];
    const bool won = g_setWins[0] > g_setWins[1];
    if (g_setFirstTo <= 1)
        snprintf(text, sizeof(text), won ? "You won the match." : "You lost the match.");
    else
        snprintf(text, sizeof(text), "You %s the set %d - %d.", won ? "won" : "lost", g_setWins[0],
                 g_setWins[1]);
    return text;
}

// Once per game tick while a room match runs: count a finished game, and end
// the set when it is decided.
void UpdateSet()
{
    if (g_resultSeen) {
        g_resultSeen = false;
        g_resultGoneTicks = 0;
        if (!g_gameCounted && ++g_resultTicks >= kResultCountTicks) {
            // Held long enough that no rollback can still undo the finish.
            g_gameCounted = true;
            const int32_t winTeam = g_resultWinTeam;
            if (winTeam == 0 || winTeam == 1) {
                ++g_setWins[winTeam == g_pairing.localSide ? 0 : 1];
                if (g_service) g_service->ReportSetScore(g_setWins[0]);
            }
            g_setOver = g_setFirstTo > 0 &&
                        (g_setWins[0] >= g_setFirstTo || g_setWins[1] >= g_setFirstTo);
            HookLog("LobbyFlow: game finished winTeam=%d (local side %d) set %d-%d of FT%d%s\n",
                    winTeam, g_pairing.localSide, g_setWins[0], g_setWins[1], g_setFirstTo,
                    g_setOver ? " (set over)" : "");
        }
    } else if (++g_resultGoneTicks >= kResultGoneTicks) {
        // A skipped frame or two must not re-arm the count: only a result
        // phase that has really gone (the rematch is under way) does.
        g_resultTicks = 0;
        g_gameCounted = false;
    }
    if (g_setOver && ++g_setOverTicks == kSetEndDelayTicks)
        LobbyFlow_RequestReturnToLobby(SetResultText());
}

// The local team's life, for the room's match slot. Read straight from the
// characters (life ratio of cChrStatus at uCharacter+0x1530), rounded to a
// few steps so a match does not publish hundreds of member-data updates.
constexpr uintptr_t PTR_SCHARACTER_INSTANCE = 0x140D44A70;
constexpr uintptr_t CCHRSTATUS_GET_LIFE_RATIO = 0x1400315C0; // (cChrStatus*) -> float
constexpr uint32_t kHealthSampleTicks = 180; // every 3 seconds
constexpr int kHealthStep = 4; // percent

void SampleTeamHealth()
{
    static uint32_t tick = 0;
    if (++tick % kHealthSampleTicks != 0 || !g_service) return;
    const uint64_t characters = *reinterpret_cast<uint64_t*>(_addr(PTR_SCHARACTER_INSTANCE));
    if (!characters) return;
    uint8_t percent[3] = {lobby::kHealthUnknown, lobby::kHealthUnknown, lobby::kHealthUnknown};
    using GetLifeRatio = float(__fastcall*)(uint64_t);
    const GetLifeRatio getRatio = reinterpret_cast<GetLifeRatio>(_addr(CCHRSTATUS_GET_LIFE_RATIO));
    // Team list as the plugin's checksum walks it: sCharacter+0x40 / +0x300,
    // nodes linked at +0x10 with the character at +0x08.
    uint64_t node = characters + (g_pairing.localSide == 0 ? 0x40 : 0x300) + 0x08;
    for (int guard = 0; node && guard < 16; ++guard) {
        const uint64_t character = *reinterpret_cast<uint64_t*>(node + 0x08);
        node = *reinterpret_cast<uint64_t*>(node + 0x10);
        if (!character) continue;
        // Helpers (Frank's zombies and the like) hang off a parent character.
        if (*reinterpret_cast<uint64_t*>(character + 0x6698)) continue;
        const int32_t member = *reinterpret_cast<int32_t*>(character + 0x2DAC); // mDefMemberId
        if (member < 0 || member > 2) continue;
        float ratio = getRatio(character + 0x1530);
        if (!(ratio > 0.0f)) ratio = 0.0f;
        if (ratio > 1.0f) ratio = 1.0f;
        int value = static_cast<int>(ratio * 100.0f + 0.5f);
        // Round up to the step, so only a dead character reads as zero.
        value = value <= 0 ? 0 : ((value + kHealthStep - 1) / kHealthStep) * kHealthStep;
        percent[member] = static_cast<uint8_t>(value > 100 ? 100 : value);
    }
    g_service->ReportHealth(percent);
}

void LobbyFlow_OnResultPhase(int32_t winTeam)
{
    if (!g_enabled || g_phase != Phase::InMatch) return;
    g_resultSeen = true;
    g_resultWinTeam = winTeam;
}

bool LobbyFlow_AllowResultRestart()
{
    if (!g_enabled || g_phase != Phase::InMatch) return true;
    return g_gameCounted && !g_setOver;
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

    UpdateSet();
    SampleTeamHealth();
    if (Netplay_TakePeerDisconnected())
        LobbyFlow_RequestReturnToLobby("Connection to opponent has been lost.");
    if (!g_returnRequested.exchange(false, std::memory_order_acq_rel)) return;
    // After a decided set the other player leaving is the normal ending, and
    // both players leave at almost the same moment: whoever is a frame behind
    // sees a disconnect first. Whatever asked for the return (netplay reports
    // a lost peer through more than one path), it is not an error. The result
    // goes in the room's chat; no modal stands between the players and the
    // next match's prompt.
    if (g_setOver) {
        g_returnReason[0] = '\0';
        if (g_service) g_service->PostSystemLine(SetResultText());
    }

    HookLog("LobbyFlow: leaving the match (%s)\n", g_returnReason[0] ? g_returnReason : "requested");
    FrameStepper_EndNetplaySession();
    // Netplay destroyed the ENet host; never leave the launch config pointing at it.
    SetNetworkHandles(nullptr, nullptr);
    TrainingBoot_PrepareNextBoot(false);
    snprintf(g_pendingNotice, sizeof(g_pendingNotice), "%s", g_returnReason);
    g_returnReason[0] = '\0';
    g_titleNeedsSetup = true;
    // Come back to the bare title screen, the state the lobby is known to
    // take over cleanly, not to a main menu that would have to be removed.
    using GetInstance = uint64_t(__fastcall*)();
    if (const uint64_t gameUi = reinterpret_cast<GetInstance>(_addr(SGAMEUI_GET_INSTANCE))())
        *reinterpret_cast<int32_t*>(gameUi + SGAMEUI_BOOT_STATE) = kBootStateTitle;
    if (!RequestTitleArea()) HookLog("LobbyFlow: sArea is unavailable; cannot return to title\n");
    SetPhase(Phase::Returning);
}

void LobbyFlow_RequestLeaveMatch()
{
    if (!LobbyFlow_InRoomMatch()) return;
    g_leaveSeatOnReturn.store(true, std::memory_order_release);
    LobbyFlow_RequestReturnToLobby("");
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
