#include "../pch.h"
#include "lobby_flow.h"
#include "lobby_local.h"
#include "lobby_music.h"

#include "LaunchMenu.h"
#include "assist_names.h"
#include "chr_colors.h"
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
#include "spectate.h"
#include "steam_lobby.h"
#include "steam_transport.h"
#include "trainingboot.h"
#include "updater.h"
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
#include <vector>

extern void HookLog(const char* fmt, ...);
extern void HookLogFlush();
extern void HookLog_MarkGameLoaded();

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
// What that state checks before it reads any input: a transition timer, a
// child menu (gallery), the license screen laid over the menu, the screen
// fade and the guide overlay.
constexpr size_t MENU_TRANSITION_TIMER = 0x8C; // float, counts down
constexpr size_t MENU_CHILD = 0x58;            // cUnit*
constexpr size_t MENU_FADE_LIMIT = 0x80;       // float
constexpr uintptr_t SLICENSE_GET_SCREEN = 0x14027CC80;      // live license screen or 0
constexpr uintptr_t SMVC3GUIDE_BLOCKS_MENU_INPUT = 0x140282600;
constexpr uintptr_t SGAMEUI_GET_FADE = 0x1402BBF40;         // float
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
// Press Start's savedata step (uMenuTitle__move -> 0x140253380). It spawns
// the uUiPreMenu at sGameConfig+0x4ADF0, which loads savedata.bin, applies it
// and sets the "savedata loaded" byte, or puts up the native missing/corrupt
// save prompts. Until that byte is set, every aTitle::init resets the
// presets and settings to defaults, and any later save writes those defaults.
constexpr uintptr_t SGAMECONFIG_SPAWN_BOOT_PREMENU = 0x140253380;  // (config, 0)
constexpr uintptr_t SGAMECONFIG_PREMENU_ALIVE = 0x1402532F0;       // (config) -> bool
constexpr uintptr_t SGAMECONFIG_IS_STORAGE_ENABLED = 0x140253360;  // (config) -> bool
constexpr uintptr_t SGAMECONFIG_SET_STORAGE_ENABLED = 0x14032CE00; // (config, bool)
constexpr uintptr_t SGAMECONFIG_IS_SAVEDATA_LOADED = 0x140216F30;  // (config) -> bool
constexpr size_t SGAMECONFIG_PREMENU = 0x4ADF0;                    // uUiPreMenu*
// What a menu calls once it has changed something the save holds (the
// gallery, 0x14037EF81: mode 1, reserve the save, 0). It spawns the same
// pre-menu in its save states (0x20 on), which packs the live state, the
// license profile with the reserve units included, and writes savedata.bin.
// The pre-menu clears "savedata loaded" when built and sets it again when the
// save has gone through.
constexpr uintptr_t SGAMECONFIG_SPAWN_SAVE_PREMENU = 0x140253B80;  // (config, mode, reserve, 0)
constexpr size_t SGAMECONFIG_SAVEDATA_LOADED = 0x4ADEC;            // uint8
constexpr int32_t kPreMenuSaveMode = 1;
constexpr size_t PREMENU_STATE = 0x60;
constexpr uintptr_t PREMENU_CANCEL_MESSAGE = 0x14032C760;          // (premenu)
// After a good load the pre-menu checks native matchmaking (0x14032B5A0) and,
// with it unavailable, posts message 2900 (state 0x37, then 0x38 waits for a
// button). Timestone does not use native online, so the boot load ends where
// a ready network would have sent it: 0x39, terminate.
constexpr int32_t kPreMenuNetworkNotice = 0x37;
constexpr int32_t kPreMenuNetworkNoticeWait = 0x38;
constexpr int32_t kPreMenuTerminate = 0x39;
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
    WatchJoining, // finding a feed for a match to watch, waiting for its first game
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
// The boot savedata load, run once per process before the lobby first shows.
enum class SaveLoad { NotStarted, Running, Done };
SaveLoad g_saveLoad = SaveLoad::NotStarted;
constexpr uint32_t kEntranceFadeDelay = 8; // frames of the 250 ms fade to sit out

std::unique_ptr<SteamRoomService> g_service;
std::unique_ptr<LobbyUi> g_ui;
ConnectRoster g_rosterPrefs;
SavedRoster g_presets[lobby::kReserveUnits] = {};
bool g_presetValid[lobby::kReserveUnits] = {};
bool g_presetsLoaded = false;
bool g_licenseFound = false;
// A reserve unit was changed in the lobby and the game's save is owed.
bool g_reserveSavePending = false;
bool g_reserveSaveRunning = false;

// Watching someone's match instead of playing one (game thread, except the
// text the overlay copies).
bool g_spectating = false;
ULONGLONG g_watchIdleTick = 0;   // joining: when the slot stopped being in progress
ULONGLONG g_watchLiveTick = 0;   // joining: when its match was first seen running
ULONGLONG g_feedLostTick = 0;    // watching: when the feed went away
// The player chose to stop watching (Stop Watching in the pause menu, which
// runs on the render thread): the standing wish to watch is withdrawn on the
// next lobby frame.
std::atomic<bool> g_watchOptOutRequested{false};
char g_watchNames[2][kNameCapacity] = {};
char g_watchLine[160] = {};
// Spectating: who else watches, the setup's queue and where the feed comes
// from, as text for the Esc menu. Written on the game thread each tick.
SpectatorInfo g_watchInfo = {};

MatchPairing g_pairing = {};
ULONGLONG g_launchWaitTick = 0; // player 2: when player 1 was first seen fighting
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

// Timestone update offered from the online menu (updater.h does the work).
enum class UpdateFlow { Idle, Checking, Offered, Downloading, Installing };
UpdateFlow g_updateFlow = UpdateFlow::Idle;
bool g_updateOfferPending = false; // a fresh visit owes the offer once known

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
    const int count = ChrColors_Count(id);
    return count > 0 ? count : 1;
}

// The reserve units come from the game's live license profile, not from
// savedata.bin: the lobby only shows once the save has been applied to it,
// the license screen edits it in place, and reading it costs no file access.
// The profile is the 0x690-byte block the save's +0x20 is copied into
// (sLicense__copyIncoming, 0x140223240); table offsets as in
// doc/saved_roster_presets.md.
constexpr size_t SLICENSE_PROFILE = 0x40;
constexpr size_t PROFILE_PRESET_CHARS = 0xB8;   // int32_t[3][3]
constexpr size_t PROFILE_PRESET_ASSISTS = 0xDC;
constexpr size_t PROFILE_PRESET_COLORS = 0x100;

uint8_t* LicenseProfile()
{
    using GetInstance = void*(__fastcall*)();
    uint8_t* license = static_cast<uint8_t*>(
        reinterpret_cast<GetInstance>(_addr(SLICENSE_GET_INSTANCE))());
    return license ? license + SLICENSE_PROFILE : nullptr;
}

void LoadPresets()
{
    if (g_presetsLoaded) return;
    g_presetsLoaded = true;
    memset(g_presetValid, 0, sizeof(g_presetValid));
    const uint8_t* profile = LicenseProfile();
    g_licenseFound = profile != nullptr;
    if (!profile) return;
    const auto table = [profile](size_t offset, size_t index) {
        int32_t value = 0;
        memcpy(&value, profile + offset + index * sizeof(int32_t), sizeof(value));
        return value;
    };
    unsigned usable = 0;
    for (size_t preset = 0; preset < lobby::kReserveUnits; ++preset) {
        SavedRoster roster = {};
        bool valid = true;
        for (size_t slot = 0; slot < 3; ++slot) {
            const size_t index = preset * 3 + slot;
            roster.chars[slot] = table(PROFILE_PRESET_CHARS, index);
            roster.assists[slot] = table(PROFILE_PRESET_ASSISTS, index);
            roster.colors[slot] = table(PROFILE_PRESET_COLORS, index);
            valid &= roster.chars[slot] >= 1 && roster.chars[slot] <= 50;
            valid &= roster.assists[slot] >= 0 && roster.assists[slot] <= 2;
            valid &= roster.colors[slot] >= 0 && roster.colors[slot] <= 8;
        }
        g_presets[preset] = roster;
        g_presetValid[preset] = valid;
        usable += valid;
    }
    HookLog("LobbyFlow: read %u reserve units from the live profile\n", usable);
}

int RosterPresetCount()
{
    LoadPresets();
    return g_licenseFound ? lobby::kReserveUnits : 0;
}

bool RosterPreset(int index, Team* team)
{
    LoadPresets();
    if (!team || index < 0 || index >= lobby::kReserveUnits || !g_presetValid[index]) return false;
    for (int i = 0; i < 3; ++i) {
        team->chars[i] = g_presets[index].chars[i];
        team->assists[i] = g_presets[index].assists[i];
        team->colors[i] = g_presets[index].colors[i];
        // A reserve unit may hold a color this install cannot load.
        if (!ChrColors_Available(team->chars[i], team->colors[i])) team->colors[i] = 0;
    }
    return true;
}

// Writes a team into a reserve unit of the live profile, where the license
// screen edits them too. The save file follows from PumpReserveSave: the
// profile is the block sGameConfig__packLiveStateIntoSavePayload
// (0x14024F210) copies into the save at +0x20.
bool RosterSetPreset(int index, const Team& team)
{
    LoadPresets();
    uint8_t* profile = LicenseProfile();
    if (!profile || index < 0 || index >= lobby::kReserveUnits) return false;
    for (int i = 0; i < 3; ++i) {
        if (team.chars[i] < 1 || team.chars[i] > 50 || team.assists[i] < 0 ||
            team.assists[i] > 2 || team.colors[i] < 0 || team.colors[i] > 7)
            return false;
        // The game's own team editor never puts a character in twice.
        for (int j = 0; j < i; ++j)
            if (team.chars[j] == team.chars[i]) return false;
    }
    SavedRoster& roster = g_presets[index];
    bool changed = !g_presetValid[index];
    for (int i = 0; i < 3; ++i)
        changed |= roster.chars[i] != team.chars[i] || roster.assists[i] != team.assists[i] ||
                   roster.colors[i] != team.colors[i];
    if (!changed) return true;
    for (int i = 0; i < 3; ++i) {
        roster.chars[i] = team.chars[i];
        roster.assists[i] = team.assists[i];
        roster.colors[i] = team.colors[i];
        const size_t at = (static_cast<size_t>(index) * 3 + i) * sizeof(int32_t);
        memcpy(profile + PROFILE_PRESET_CHARS + at, &team.chars[i], sizeof(int32_t));
        memcpy(profile + PROFILE_PRESET_ASSISTS + at, &team.assists[i], sizeof(int32_t));
        memcpy(profile + PROFILE_PRESET_COLORS + at, &team.colors[i], sizeof(int32_t));
    }
    g_presetValid[index] = true;
    g_reserveSavePending = true;
    HookLog("LobbyFlow: reserve unit %c set to %d/%d/%d\n", 'A' + index, team.chars[0],
            team.chars[1], team.chars[2]);
    return true;
}

int RosterStageCount() { return kStageCount; }
int32_t RosterStageId(int index)
{
    return index >= 0 && index < kStageCount ? kStages[index].id : lobby::kStageAny;
}
const char* RosterStageName(int32_t id)
{
    for (int i = 0; i < kStageCount; ++i)
        if (kStages[i].id == id) return kStages[i].name;
    return "";
}

// The stage a match is fought on: one of the two players' preferred stages,
// picked at random by player 1. A player with no preference goes along with
// the other's; when neither has one it is any stage but the training room.
int32_t PickMatchStage(int localStage, int opponentStage)
{
    LARGE_INTEGER counter = {};
    QueryPerformanceCounter(&counter);
    const uint32_t roll = static_cast<uint32_t>(counter.QuadPart ^ (counter.QuadPart >> 17) ^
                                                GetTickCount64() * 2654435761u);
    const bool localSet = RosterStageName(localStage)[0] != '\0';
    const bool opponentSet = RosterStageName(opponentStage)[0] != '\0';
    if (localSet && opponentSet) return (roll >> 8) & 1 ? localStage : opponentStage;
    if (localSet) return localStage;
    if (opponentSet) return opponentStage;
    int32_t pool[32];
    int count = 0;
    for (int i = 0; i < kStageCount && count < 32; ++i)
        if (kStages[i].id != 0) pool[count++] = kStages[i].id;
    return count ? pool[(roll >> 8) % count] : g_rosterPrefs.stageId;
}

const RosterSource kRosterSource = {
    RosterCharacterCount, RosterCharacterId, RosterCharacterName, RosterAssistName,
    RosterColorCount,     RosterPresetCount, RosterPreset,
    RosterSetPreset,
    RosterStageCount,     RosterStageId,     RosterStageName,
};

// ---------------------------------------------------------------------------
// Preferences (Timestone.ini)
// ---------------------------------------------------------------------------
void SettingsPath(char* path, size_t size)
{
    LaunchMenuUtils::GetSettingsPath(path, size);
}

// Spectating: whether the names and feed state are drawn over the match.
std::atomic<bool> g_spectatorOverlay{true};

// A team read from the ini may hold a character twice (teams could before
// presets existed, and the file can be edited): the later one becomes the
// first character not on the team, in its first color.
void RemoveDuplicateCharacters(Team& team)
{
    for (int i = 1; i < 3; ++i) {
        bool repeated = false;
        for (int j = 0; j < i; ++j) repeated |= team.chars[j] == team.chars[i];
        if (!repeated) continue;
        for (int32_t id = 1; id <= 50; ++id) {
            if (id == team.chars[0] || id == team.chars[1] || id == team.chars[2]) continue;
            team.chars[i] = id;
            team.colors[i] = 0;
            break;
        }
    }
}

UiPrefs LoadPrefs()
{
    char path[MAX_PATH] = {};
    SettingsPath(path, sizeof(path));
    g_spectatorOverlay.store(GetPrivateProfileIntA("Lobby", "SpectatorOverlay", 1, path) != 0,
                             std::memory_order_relaxed);
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
                 prefs.team.assists[i] >= 0 && prefs.team.assists[i] <= 2;
    }
    if (!valid) prefs.team = Team{{CHR_RYU, CHR_WOLVERINE, CHR_DANTE}, {0, 0, 0}, {0, 0, 0}};
    // A color that cannot be loaded costs the color, not the whole team.
    for (int i = 0; i < 3; ++i) {
        if (!ChrColors_Available(prefs.team.chars[i], prefs.team.colors[i]))
            prefs.team.colors[i] = 0;
    }

    RemoveDuplicateCharacters(prefs.team);

    // The player's own teams: [LobbyPresets] NameN, and TeamN as nine numbers
    // (character, assist, color for each member). An empty TeamN is a free slot.
    for (int i = 0; i < lobby::kTeamPresets; ++i) {
        lobby::TeamPreset& preset = prefs.presets[i];
        char key[16], text[96] = {};
        snprintf(key, sizeof(key), "Team%d", i + 1);
        GetPrivateProfileStringA("LobbyPresets", key, "", text, sizeof(text), path);
        int v[9] = {};
        if (sscanf(text, "%d,%d,%d,%d,%d,%d,%d,%d,%d", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5],
                   &v[6], &v[7], &v[8]) != 9)
            continue;
        bool usable = true;
        for (int m = 0; m < 3; ++m) {
            preset.team.chars[m] = v[m * 3];
            preset.team.assists[m] = v[m * 3 + 1];
            preset.team.colors[m] = v[m * 3 + 2];
            usable &= preset.team.chars[m] >= 1 && preset.team.chars[m] <= 50 &&
                      preset.team.assists[m] >= 0 && preset.team.assists[m] <= 2;
            if (usable && !ChrColors_Available(preset.team.chars[m], preset.team.colors[m]))
                preset.team.colors[m] = 0;
        }
        if (!usable) continue;
        RemoveDuplicateCharacters(preset.team);
        preset.used = true;
        snprintf(key, sizeof(key), "Name%d", i + 1);
        GetPrivateProfileStringA("LobbyPresets", key, "", preset.name, sizeof(preset.name), path);
        if (!preset.name[0]) snprintf(preset.name, sizeof(preset.name), "PRESET %d", i + 1);
    }
    // The team in use: one of those, or a reserve unit (kTeamPresets + 0..2).
    // Before there were presets, [Lobby] Preset named a reserve unit or -1.
    prefs.slot = GetPrivateProfileIntA("Lobby", "TeamSlot", -1, path);
    if (prefs.slot < 0 || prefs.slot >= lobby::kTeamSlots) {
        const int reserve = GetPrivateProfileIntA("Lobby", "Preset", -1, path);
        prefs.slot = reserve >= 0 && reserve < lobby::kReserveUnits ? lobby::kTeamPresets + reserve
                                                                    : -1;
    }
    if (prefs.slot >= 0 && prefs.slot < lobby::kTeamPresets && !prefs.presets[prefs.slot].used)
        prefs.slot = -1;
    if (prefs.slot < 0) {
        // No team chosen yet: the first preset, made from the team so far if
        // there is none.
        for (int i = 0; i < lobby::kTeamPresets && prefs.slot < 0; ++i)
            if (prefs.presets[i].used) prefs.slot = i;
        if (prefs.slot < 0) {
            prefs.slot = 0;
            prefs.presets[0].used = true;
            prefs.presets[0].team = prefs.team;
            snprintf(prefs.presets[0].name, sizeof(prefs.presets[0].name), "PRESET 1");
        }
    }
    // A reserve unit's team is read from the game once the lobby shows
    // (LobbyUi::RefreshPreset); until then the team last used stands.
    if (prefs.slot < lobby::kTeamPresets) prefs.team = prefs.presets[prefs.slot].team;
    prefs.icon = GetPrivateProfileIntA("Lobby", "Icon", lobby::kIconSteam, path);
    if (prefs.icon < lobby::kIconSteam || prefs.icon > lobby::kIconMax) prefs.icon = lobby::kIconSteam;
    // The preferred stage starts as Bonne Wonderland.
    constexpr int kDefaultStage = 105;
    prefs.stage = GetPrivateProfileIntA("Lobby", "Stage", kDefaultStage, path);
    if (!RosterStageName(prefs.stage)[0]) prefs.stage = lobby::kStageAny;
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
    prefs.muteMusic = GetPrivateProfileIntA("Lobby", "MuteMusic", 0, path) != 0;
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
    std::vector<IniSafe_Entry> entries = {
        {"Lobby", "TeamSlot", std::to_string(prefs.slot)},
        {"Lobby", "Icon", std::to_string(prefs.icon)},
        {"Lobby", "Stage", std::to_string(prefs.stage)},
        {"Lobby", "RoomName", prefs.create.name},
        {"Lobby", "RoomSize", std::to_string(prefs.create.maxMembers)},
        {"Lobby", "RoomSlots", std::to_string(prefs.create.slotCount)},
        {"Lobby", "RoomVisibility", std::to_string(static_cast<int>(prefs.create.visibility))},
        {"Lobby", "RoomMusic", std::to_string(prefs.create.music)},
        {"Lobby", "MuteMusic", prefs.muteMusic ? "1" : "0"},
        {"Lobby", "RoomFirstTo", std::to_string(prefs.create.firstTo)},
        {"Lobby", "RoomRotation", std::to_string(static_cast<int>(prefs.create.rotation))},
    };
    for (int i = 0; i < lobby::kTeamPresets; ++i) {
        const lobby::TeamPreset& preset = prefs.presets[i];
        const std::string number = std::to_string(i + 1);
        std::string team;
        for (int m = 0; preset.used && m < 3; ++m)
            team += (m ? "," : "") + std::to_string(preset.team.chars[m]) + "," +
                    std::to_string(preset.team.assists[m]) + "," +
                    std::to_string(preset.team.colors[m]);
        entries.push_back({"LobbyPresets", "Name" + number, preset.used ? preset.name : ""});
        entries.push_back({"LobbyPresets", "Team" + number, team});
    }
    IniSafe_WriteBatch(entries.data(), entries.size(), path);
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
    if (track == lobby::kMusicNone) return {Bgm::None, lobby::kMusicDefault};
    if (inRoom && g_ui && g_ui->Prefs().muteMusic) {
        // A guest who muted the lobby's music; the host always hears their own.
        const RoomState& room = g_service->Room();
        bool isHost = false;
        for (int i = 0; i < room.memberCount; ++i)
            isHost |= room.members[i].isLocal && room.members[i].isHost;
        if (!isHost) return {Bgm::None, lobby::kMusicDefault};
    }
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
    if (wanted.bgm == Bgm::None) {
        // The host chose no music for this room.
        reinterpret_cast<FadeOut>(_addr(GSOUND_FADE_OUT_BGM))(sound, 2000);
        g_bgm = Bgm::None;
        HookLog("LobbyFlow: no music from aTitle::init (room has none)\n");
        return;
    }
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

// Whether the menu's navigation state would act on a press this frame. It
// runs every frame even with the license screen open on top of the menu, and
// returns early itself; the hook must not take a decide the menu would ignore.
bool MainMenuTakesInput(void* menu)
{
    const uint8_t* base = static_cast<const uint8_t*>(menu);
    using GetInstance = void*(__fastcall*)();
    if (*reinterpret_cast<const float*>(base + MENU_TRANSITION_TIMER) > 0.0f) return false;
    if (*reinterpret_cast<const uint64_t*>(base + MENU_CHILD)) return false;
    if (void* license = reinterpret_cast<GetInstance>(_addr(SLICENSE_GET_INSTANCE))()) {
        using GetScreen = uint64_t(__fastcall*)(void*);
        if (reinterpret_cast<GetScreen>(_addr(SLICENSE_GET_SCREEN))(license)) return false;
    }
    if (void* gameUi = reinterpret_cast<GetInstance>(_addr(SGAMEUI_GET_INSTANCE))()) {
        using GetFade = float(__fastcall*)(void*);
        if (*reinterpret_cast<const float*>(base + MENU_FADE_LIMIT) <=
            reinterpret_cast<GetFade>(_addr(SGAMEUI_GET_FADE))(gameUi))
            return false;
    }
    if (void* guide = reinterpret_cast<GetInstance>(_addr(SMVC3GUIDE_GET_INSTANCE))()) {
        using Blocks = bool(__fastcall*)(void*);
        if (reinterpret_cast<Blocks>(_addr(SMVC3GUIDE_BLOCKS_MENU_INPUT))(guide)) return false;
    }
    return true;
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

void FollowActivePad();

uint64_t __fastcall Hook_MenuNavigation(void* menu, int32_t* out)
{
    // No menu takes input while the title area is being rebuilt.
    if (g_titleReloadPending) return 1;
    if (!IsReleasedMainMenu(menu)) return g_originalMenuNavigation(menu, out);
    if (g_onlineChosen) return 1;

    // Only on the top menu: under Offline, the 2P controller select assigns
    // the second pad itself, and following it would make both pads player 1.
    if (!*reinterpret_cast<const uint64_t*>(static_cast<const uint8_t*>(menu) + MENU_CHILD))
        FollowActivePad();

    // Take the decide on Online before the menu sees it: its own handler
    // would start the native network entry.
    Input input = {};
    Native_ReadInput(input, false);
    if (input.confirm && MainMenuTakesInput(menu) &&
        *reinterpret_cast<int32_t*>(static_cast<uint8_t*>(menu) + MENU_CURSOR) ==
            MAINMENU_ITEM_ONLINE) {
        HookLog("LobbyFlow: Online chosen on the main menu\n");
        Native_PlaySfx(SFX_MENU_ENTER);
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

// Runs the savedata load Press Start would have run. Returns true while it is
// in progress: the lobby stays off screen so the native prompts can show and
// take input, and no match can boot on unloaded settings.
bool PumpBootSavedataLoad()
{
    if (g_saveLoad == SaveLoad::Done) return false;
    using GetInstance = void*(__fastcall*)();
    using ConfigQuery = bool(__fastcall*)(void*);
    void* config = reinterpret_cast<GetInstance>(_addr(SGAMECONFIG_GET_INSTANCE))();
    if (!config) return true;
    const auto query = [config](uintptr_t function) {
        return reinterpret_cast<ConfigQuery>(_addr(function))(config);
    };

    if (g_saveLoad == SaveLoad::NotStarted) {
        if (query(SGAMECONFIG_IS_SAVEDATA_LOADED)) {
            g_saveLoad = SaveLoad::Done;
            TrainingBoot_MarkNativeConfigLoaded();
            return false;
        }
        // uMenuTitle::setup enables storage; the lobby retires that menu
        // before it runs. sGameConfig starts enabled, so this is a backstop.
        if (!query(SGAMECONFIG_IS_STORAGE_ENABLED)) {
            using SetEnabled = void(__fastcall*)(void*, bool);
            reinterpret_cast<SetEnabled>(_addr(SGAMECONFIG_SET_STORAGE_ENABLED))(config, true);
            HookLog("LobbyFlow: enabled savedata storage before the boot load\n");
        }
        using Spawn = void(__fastcall*)(void*, int32_t);
        reinterpret_cast<Spawn>(_addr(SGAMECONFIG_SPAWN_BOOT_PREMENU))(config, 0);
        if (!query(SGAMECONFIG_PREMENU_ALIVE)) {
            HookLog("LobbyFlow: WARNING - the savedata load could not be started\n");
            g_saveLoad = SaveLoad::Done;
            return false;
        }
        g_saveLoad = SaveLoad::Running;
        HookLog("LobbyFlow: loading savedata through the title pre-menu\n");
        return true;
    }

    if (query(SGAMECONFIG_PREMENU_ALIVE)) {
        uint8_t* preMenu = *reinterpret_cast<uint8_t**>(
            static_cast<uint8_t*>(config) + SGAMECONFIG_PREMENU);
        int32_t* state = preMenu ? reinterpret_cast<int32_t*>(preMenu + PREMENU_STATE) : nullptr;
        if (state && (*state == kPreMenuNetworkNotice || *state == kPreMenuNetworkNoticeWait)) {
            if (*state == kPreMenuNetworkNoticeWait) {
                using Cancel = void(__fastcall*)(void*);
                reinterpret_cast<Cancel>(_addr(PREMENU_CANCEL_MESSAGE))(preMenu);
            }
            *state = kPreMenuTerminate;
            HookLog("LobbyFlow: skipped the native online-unavailable notice\n");
        }
        return true;
    }
    g_saveLoad = SaveLoad::Done;
    // Whatever the outcome, the game has made its decision about this save
    // (applied, recreated, or play without saving); a battle boot must not
    // read savedata.bin again behind it.
    TrainingBoot_MarkNativeConfigLoaded();
    HookLog("LobbyFlow: savedata load finished (%s)\n",
            query(SGAMECONFIG_IS_SAVEDATA_LOADED) ? "applied" : "not applied");
    return false;
}

// Saves the game's own save after a reserve unit was edited in the lobby,
// once the player is done with the team screen and no match is being set up.
// Returns true while the save is being written.
bool PumpReserveSave()
{
    if (!g_reserveSavePending && !g_reserveSaveRunning) return false;
    using GetInstance = void*(__fastcall*)();
    using ConfigQuery = bool(__fastcall*)(void*);
    void* config = reinterpret_cast<GetInstance>(_addr(SGAMECONFIG_GET_INSTANCE))();
    if (!config) return false;
    const auto query = [config](uintptr_t function) {
        return reinterpret_cast<ConfigQuery>(_addr(function))(config);
    };
    uint8_t& loaded = *(static_cast<uint8_t*>(config) + SGAMECONFIG_SAVEDATA_LOADED);

    if (g_reserveSaveRunning) {
        if (query(SGAMECONFIG_PREMENU_ALIVE)) return true;
        g_reserveSaveRunning = false;
        if (loaded) {
            HookLog("LobbyFlow: reserve units saved\n");
        } else {
            // The game marks the save as not loaded while it writes, and with
            // that mark the next title area resets every setting. A save that
            // did not finish must not leave it behind.
            loaded = 1;
            HookLog("LobbyFlow: WARNING - saving the reserve units did not finish\n");
        }
        return false;
    }

    if (g_phase != Phase::Lobby || g_mainMenuReleaseAt || !g_ui || g_ui->EditingTeam())
        return false;
    g_reserveSavePending = false;
    if (!loaded || !query(SGAMECONFIG_IS_STORAGE_ENABLED) || query(SGAMECONFIG_PREMENU_ALIVE)) {
        // Playing without a save, or the game is busy with it: the edit
        // stays in memory and goes out with the game's next save.
        HookLog("LobbyFlow: reserve units changed; no save written (loaded=%d)\n", loaded);
        return false;
    }
    using Spawn = void(__fastcall*)(void*, int32_t, char, int32_t);
    reinterpret_cast<Spawn>(_addr(SGAMECONFIG_SPAWN_SAVE_PREMENU))(config, kPreMenuSaveMode, 1, 0);
    if (!query(SGAMECONFIG_PREMENU_ALIVE)) {
        loaded = 1;
        HookLog("LobbyFlow: WARNING - the reserve unit save could not be started\n");
        return false;
    }
    g_reserveSaveRunning = true;
    HookLog("LobbyFlow: saving the edited reserve units\n");
    return true;
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
    // An accepted invite does not pull the room out from under a match.
    if (g_service)
        g_service->HoldRoomChange(phase == Phase::Launching || phase == Phase::InMatch ||
                                  phase == Phase::Returning);
}

void DropMatchConnection()
{
    // Before Netplay_Init the game-side ENet host is still ours to destroy.
    if (g_matchHost && !Netplay_IsActive()) enet_host_destroy(g_matchHost);
    g_matchHost = nullptr;
    g_matchPeer = nullptr;
    SteamRoom_DisconnectPeer();
}

// `notice` is the reason, shown to the player unless `quiet` (it is still
// logged, and a failure still gives the seat up).
void AbortMatchStart(const char* notice, bool quiet = false)
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
        if (notice && !quiet) g_ui->ShowNotice(notice);
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

// Delay and rollback cannot both be 0, and each setter refuses a 0 while the
// other value is still 0. Whatever the last match (or watching one) left
// behind, the non-zero one goes in first: a chosen delay of 0 used to be
// turned into 1 because the rollback window was applied after it.
void ApplyNetplaySettings(int32_t delay, int32_t rollback)
{
    if (delay > 0) {
        Controller_SetDelayFrames(delay);
        Controller_SetRollbackFrames(rollback);
    } else {
        Controller_SetRollbackFrames(rollback);
        Controller_SetDelayFrames(delay);
    }
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
    LaunchMenu_GetAutomaticNetplaySettings(rtt, &delay, &rollback);
    // The players' own delay picks. Both run the same delay, so the higher
    // pick is used (a player who asked for more must not get less); "Auto"
    // stands for the value measured from the ping. The rollback window is
    // the same in every match.
    const auto wanted = [&](int pick) { return pick > kDelayPickAuto ? pick - 1 : delay; };
    delay = std::max(wanted(g_pairing.localDelayPick), wanted(g_pairing.opponentDelayPick));
    rollback = kNetplayRollbackFrames;
    HookLog("LobbyFlow: delay picks local=%d opponent=%d measured ping=%u ms -> %dD/%dR\n",
            g_pairing.localDelayPick, g_pairing.opponentDelayPick, rtt, delay, rollback);
    ApplyNetplaySettings(delay, rollback);

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
    launch.stageId = PickMatchStage(g_pairing.localStage, g_pairing.opponentStage);
    HookLog("LobbyFlow: stage %d from preferences local=%d opponent=%d\n", launch.stageId,
            g_pairing.localStage, g_pairing.opponentStage);

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
    HookLog("LobbyFlow: sent launch as P1 ping=%u ms settings=%dD/%dR stage=%d\n", rtt, delay,
            rollback, launch.stageId);
    return true;
}

// ---------------------------------------------------------------------------
// Spectating
// ---------------------------------------------------------------------------
const Member* LocalRoomMember()
{
    if (!g_service) return nullptr;
    const RoomState& room = g_service->Room();
    for (int i = 0; i < room.memberCount; ++i)
        if (room.members[i].isLocal) return &room.members[i];
    return nullptr;
}

// Once per frame wherever the game is: the feed runs on the room's published
// state, for the two players as much as for anyone watching.
void PumpSpectate()
{
    SpectateRoom view = {};
    if (g_service && g_service->Room().inRoom) {
        const RoomState& room = g_service->Room();
        view.localId = SteamRoom_GetLocalId();
        for (int i = 0; i < room.memberCount && view.count < 8; ++i) {
            const Member& member = room.members[i];
            SpectateRoomMember& out = view.members[view.count++];
            out.id = member.id;
            out.slot = member.side < 2 ? member.slot : -1;
            out.fighting = member.status == MemberStatus::Fighting;
            out.watchSlot = member.watchSlot;
            out.watchSeq = member.watchSeq;
            out.upstream = member.upstream;
        }
    }
    const uint64_t upstream = Spectate_Pump(view);
    if (g_service && Spectate_GetState() != SpectateState::Off) g_service->SetUpstream(upstream);
}

void StopSpectating()
{
    Spectate_Stop();
    if (g_service) g_service->StopWatching();
    TrainingBoot_SetSpectatorBoot(false);
    g_spectating = false;
    g_watchIdleTick = 0;
    g_feedLostTick = 0;
    g_watchLine[0] = '\0';
    g_watchInfo.valid = false;
}

void UpdateWatchInfo(const RoomState& room, int slot)
{
    SpectatorInfo info = {};
    info.valid = true;
    info.feedPing = -1;
    const Member* local = nullptr;
    for (int i = 0; i < room.memberCount; ++i)
        if (room.members[i].isLocal) local = &room.members[i];
    const auto put = [](SpectatorInfo::Person& person, const Member& member) {
        snprintf(person.name, sizeof(person.name), "%s", member.name);
        person.local = member.isLocal;
    };
    for (int i = 0; i < room.memberCount; ++i) {
        const Member& member = room.members[i];
        if (local && local->upstream && member.id == local->upstream) {
            snprintf(info.feed, sizeof(info.feed), "%s", member.name);
            info.feedPing = member.pingMs;
        }
        if (member.watchSlot == slot && info.watcherCount < SpectatorInfo::kMax)
            put(info.watchers[info.watcherCount++], member);
    }
    // Front of the line first; the room derives each place.
    for (int place = 1; place <= room.memberCount; ++place) {
        for (int i = 0; i < room.memberCount; ++i) {
            const Member& member = room.members[i];
            if (member.queueSlot == slot && member.queuePos == place &&
                info.queueCount < SpectatorInfo::kMax)
                put(info.queue[info.queueCount++], member);
        }
    }
    memcpy(&g_watchInfo, &info, sizeof(info));
}

// Who is being watched, for the overlay and the in-battle name plates.
void UpdateWatchText()
{
    const int slot = Spectate_Slot();
    if (!g_service || slot < 0) return;
    const RoomState& room = g_service->Room();
    UpdateWatchInfo(room, slot);
    const Member* players[2] = {nullptr, nullptr};
    for (int i = 0; i < room.memberCount; ++i) {
        const Member& member = room.members[i];
        if (member.slot == slot && member.side < 2) players[member.side] = &member;
    }
    if (!players[0] || !players[1]) return; // keep the last names once they have left
    for (int side = 0; side < 2; ++side)
        snprintf(g_watchNames[side], sizeof(g_watchNames[side]), "%s", players[side]->name);
    char line[sizeof(g_watchLine)];
    const int firstTo = room.settings.firstTo;
    if (firstTo == 1)
        snprintf(line, sizeof(line), "%.40s  vs  %.40s", players[0]->name, players[1]->name);
    else
        snprintf(line, sizeof(line), "%.40s  %d - %d  %.40s", players[0]->name,
                 players[0]->setWins, players[1]->setWins, players[1]->name);
    memcpy(g_watchLine, line, sizeof(g_watchLine));
}

void BeginSpectateLaunch(const TrainingBootConfig& config, int cpuMode, bool fiveBars)
{
    TrainingBoot_PrepareNextBoot(true);
    TrainingBoot_SetSpectatorBoot(true);
    TrainingBoot_SetCpuMode(static_cast<TrainingBootCpuMode>(cpuMode));
    TrainingBoot_SetFiveBarStart(fiveBars);
    TrainingBoot_SetConfig(config);
    SetNetworkHandles(nullptr, nullptr);
    // A netplay session without a peer; every frame it simulates is final,
    // so nothing is ever predicted or rolled back.
    Netplay_InitSpectator();
    Controller_SetDelayFrames(1);
    Controller_SetRollbackFrames(0);
    FrameStepper_InitNetplay();
    g_spectating = true;
    g_feedLostTick = 0;
    g_leaveSeatOnReturn.store(false, std::memory_order_relaxed);
    ResetSet(1);
    UpdateWatchText();
    g_ui->SetBusy("Loading the match...");
    SetPhase(Phase::Launching);
    HookLog("LobbyFlow: launching setup %d's match as a spectator\n", Spectate_Slot() + 1);
}

// The room publishes who watches what (the screens set it); this follows.
void UpdateSpectateJoin(bool cancelPressed)
{
    if (g_phase != Phase::Lobby && g_phase != Phase::WatchJoining) return;
    const Member* local = LocalRoomMember();
    const RoomState& room = g_service->Room();
    int wanted = local && room.inRoom ? local->watchSlot : -1;
    if (g_phase == Phase::Lobby) {
        if (g_watchOptOutRequested.exchange(false, std::memory_order_acq_rel))
            g_service->SetWatchIntent(-1);
        // The player asked to watch a setup's matches (with or without a
        // place in its queue): each one is joined as it comes up. From the
        // moment both players are ready, not only once they fight, so the
        // links to them are up before the match is.
        const int intent = local && room.inRoom && local->slot < 0 ? g_service->WatchIntent() : -1;
        if (wanted < 0 && intent >= 0 && intent < room.settings.slotCount &&
            room.slots[intent] >= SlotState::Starting) {
            g_service->Watch(intent);
            wanted = intent;
        }
        if (wanted < 0) return;
        Spectate_Start(wanted);
        g_watchIdleTick = 0;
        g_watchLiveTick = 0;
        g_ui->SetBusy("Preparing to spectate...");
        SetPhase(Phase::WatchJoining);
        return;
    }

    const ULONGLONG now = GetTickCount64();
    const char* failure = nullptr;
    bool stop = wanted < 0 || cancelPressed; // seated by a queue, left, or changed their mind
    bool neverStarted = false;
    if (!stop) {
        const SlotState state = room.slots[wanted];
        if (state == SlotState::InProgress && !g_watchLiveTick) g_watchLiveTick = now;
        if (state >= SlotState::Starting) g_watchIdleTick = 0;
        else if (!g_watchIdleTick) g_watchIdleTick = now;
        else if (now - g_watchIdleTick > 4000) {
            // The two were ready and then were not: nothing to report.
            if (g_watchLiveTick) failure = "That match has ended.";
            else stop = neverStarted = true;
        }
        // The clock starts with the match: before that the players are
        // still loading and there is nothing to receive.
        if (!failure && g_watchLiveTick && now - g_watchLiveTick > 30000)
            failure = "Could not join the match as a spectator.";
    }
    if (stop || failure) {
        // Not this match again by itself: neither after a cancel nor in a
        // loop after a failure.
        if (!neverStarted) g_service->SetWatchIntent(-1);
        HookLog("LobbyFlow: stopped joining as a spectator (%s)\n", failure ? failure : "cancelled");
        StopSpectating();
        g_ui->SetBusy(nullptr);
        if (failure) g_ui->ShowNotice(failure);
        SetPhase(Phase::Lobby);
        return;
    }
    TrainingBootConfig config;
    int cpuMode = 0;
    bool fiveBars = false;
    if (Spectate_TakeLaunch(&config, &cpuMode, &fiveBars))
        BeginSpectateLaunch(config, cpuMode, fiveBars);
}

void BeginLaunch()
{
    // The lobby stays on screen (busy overlay) until the boot commits; see
    // LobbyFlow_OnBattleBootCommitted.
    TrainingBoot_SetSpectatorBoot(false);
    g_spectating = false;
    Spectate_RecBegin(); // from here on this match can be watched
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
                    AbortMatchStart("Connection to opponent has been lost.", true);
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
            AbortMatchStart("Connection to opponent has been lost.", true);
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
                ApplyNetplaySettings(settings.delay, settings.rollbackFrames);
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
    const bool paired = g_service->FindPairing(current) &&
                        current.opponentId == g_pairing.opponentId;
    if (!paired && !g_matchIsHost) {
        // Player 1 marks itself as fighting the moment it launches, and the
        // room can show that before the launch packet has been read here:
        // the pairing then looks gone when it has in fact just started.
        // Whatever the room says, a launch that is already on the wire wins.
        if (PumpHandshake(false)) {
            BeginLaunch();
            return;
        }
        if (g_phase != Phase::Handshake) return; // the pump found the link dead
        // Still opposite us and fighting: the launch is on its way. Give it
        // a few seconds before believing the room over the connection.
        const Member* local = LocalRoomMember();
        bool launching = false;
        const RoomState& room = g_service->Room();
        for (int i = 0; local && i < room.memberCount; ++i) {
            const Member& member = room.members[i];
            launching |= member.id == g_pairing.opponentId && member.slot == local->slot &&
                         member.status == MemberStatus::Fighting;
        }
        const ULONGLONG now = GetTickCount64();
        if (!launching) g_launchWaitTick = 0;
        else if (!g_launchWaitTick) g_launchWaitTick = now;
        if (launching && now - g_launchWaitTick < 5000) return;
    }
    if (paired) g_launchWaitTick = 0;
    if (!paired) {
        g_launchWaitTick = 0;
        const Member* local = LocalRoomMember();
        HookLog("LobbyFlow: pairing with %llu is gone (local slot=%d status=%d)\n",
                static_cast<unsigned long long>(g_pairing.opponentId), local ? local->slot : -9,
                local ? static_cast<int>(local->status) : -1);
        AbortMatchStart(nullptr);
        return;
    }
    // Teams, preferred stages and delay picks are chosen in the prompts, after
    // the connection: what the room says now is what the match starts with.
    g_pairing.localTeam = current.localTeam;
    g_pairing.opponentTeam = current.opponentTeam;
    g_pairing.localStage = current.localStage;
    g_pairing.opponentStage = current.opponentStage;
    g_pairing.localDelayPick = current.localDelayPick;
    g_pairing.opponentDelayPick = current.opponentDelayPick;
    // The prompts show this in place of the room's estimate, and "Auto" is
    // worked out from it on both sides.
    if (const uint32_t rtt = SteamTransport_GetRTT())
        g_service->SetMeasuredPing(g_pairing.opponentId, static_cast<int>(rtt));
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
                // Queued, not joined here: Steam may not be up yet, and the
                // join then waits for it instead of being lost.
                HookLog("LobbyFlow: joining invited lobby %llu\n",
                        static_cast<unsigned long long>(lobbyId));
                SteamLobby_QueueJoin(lobbyId);
            }
        }
    }
    if (!g_ui) g_ui.reset(new LobbyUi(*g_service, kRosterSource, LoadPrefs()));
    // Lobby test build: bring up the second player's game now that this one has
    // finished starting (no-op otherwise, and only ever once).
    LocalLobby_SpawnSecondInstance();
}

// "v0.5.1" for either "0.5.1" or a "v0.5.1" release tag.
void FormatVersion(char* out, size_t size, const char* version)
{
    snprintf(out, size, "%s%s", (version[0] == 'v' || version[0] == 'V') ? "" : "v", version);
}

void OfferUpdate()
{
    char installed[32], latest[72];
    FormatVersion(installed, sizeof(installed), UMVC3HOOK_VERSION);
    FormatVersion(latest, sizeof(latest), Updater_LatestVersion());
    g_ui->OfferUpdate(installed, latest);
    g_updateFlow = UpdateFlow::Offered;
}

// Each fresh visit to the online menu: check once per session, quietly in
// the background, then offer while one is available.
void PumpUpdate()
{
    switch (g_updateFlow) {
    case UpdateFlow::Idle:
    case UpdateFlow::Offered:
        if (g_updateOfferPending && Updater_PollOnlineCheck() == UpdateCheck::Idle) {
            Updater_BeginOnlineCheck();
            g_updateFlow = UpdateFlow::Checking;
        } else if (g_updateOfferPending && Updater_PollOnlineCheck() == UpdateCheck::Available) {
            g_updateOfferPending = false;
            OfferUpdate();
        } else {
            g_updateOfferPending = false;
        }
        if (g_updateFlow == UpdateFlow::Offered && g_ui->TakeUpdateAccepted()) {
            // Out of any room first: a match must not start under the update.
            if (g_service->Room().inRoom) g_service->Leave();
            Updater_BeginDownload();
            g_updateFlow = UpdateFlow::Downloading;
        }
        break;
    case UpdateFlow::Checking: {
        const UpdateCheck check = Updater_PollOnlineCheck();
        if (check == UpdateCheck::Checking) break;
        g_updateFlow = UpdateFlow::Idle;
        // Nothing waited on the check, so the player may have moved into a
        // room by now; the offer is then kept for the next visit instead.
        if (check == UpdateCheck::Available && !g_service->Room().inRoom) OfferUpdate();
        g_updateOfferPending = false;
        break;
    }
    case UpdateFlow::Downloading: {
        if (g_ui->TakeUpdateCancelled()) {
            Updater_CancelDownload();
            g_ui->SetUpdateProgress(nullptr);
            g_ui->ShowNotice("The update was cancelled.");
            g_updateFlow = UpdateFlow::Idle;
            break;
        }
        const UpdateProgress& progress = Updater_PollDownload();
        UpdateView view = {};
        snprintf(view.stage, sizeof(view.stage), "%s", progress.stage);
        view.fraction = progress.total ? static_cast<float>(progress.downloaded) / progress.total : -1.0f;
        view.cancellable = true;
        const double mib = 1024.0 * 1024.0;
        if (progress.total)
            snprintf(view.status, sizeof(view.status), "%u%%  |  %.2f / %.2f MiB  |  %.0f KiB/s",
                     static_cast<unsigned>(progress.downloaded * 100 / progress.total),
                     progress.downloaded / mib, progress.total / mib, progress.bytesPerSecond / 1024.0);
        else if (progress.downloaded)
            snprintf(view.status, sizeof(view.status), "%.2f MiB downloaded  |  %.0f KiB/s",
                     progress.downloaded / mib, progress.bytesPerSecond / 1024.0);

        if (progress.state == UpdateProgress::Failed) {
            g_ui->SetUpdateProgress(nullptr);
            char notice[300];
            snprintf(notice, sizeof(notice), "The update could not be downloaded.\n%s", progress.error);
            g_ui->ShowNotice(notice);
            g_updateFlow = UpdateFlow::Idle;
        } else if (progress.state == UpdateProgress::Ready) {
            snprintf(view.stage, sizeof(view.stage), "Restarting the game to install the update...");
            view.status[0] = '\0';
            view.fraction = 1.0f;
            view.cancellable = false;
            g_ui->SetUpdateProgress(&view);
            // Shown for one frame first, then the swap and restart.
            g_updateFlow = UpdateFlow::Installing;
        } else {
            g_ui->SetUpdateProgress(&view);
        }
        break;
    }
    case UpdateFlow::Installing: {
        // Does not return when it works: the game restarts on the new ASI.
        char error[256];
        if (!Updater_InstallAndRestart(error, sizeof(error))) {
            g_ui->SetUpdateProgress(nullptr);
            g_ui->ShowNotice(error);
            g_updateFlow = UpdateFlow::Idle;
        }
        break;
    }
    }
}

void RunLobbyFrame(uint64_t titleArea)
{
    g_service->Pump();

    Input input = {};
    Native_ReadInput(input, g_ui->TextEntryActive());
    // MAIN MENU was picked: the lobby stays up, inert, until black covers it.
    if (g_mainMenuReleaseAt) input = Input{};
    g_ui->Update(input);
    PumpUpdate();
    if (g_ui->TakePrefsDirty()) SavePrefs(g_ui->Prefs());
    const uint32_t sfx = g_ui->TakeSfx();
    if (sfx & SFX_MATCH_FOUND) Native_FocusGameWindow();
    Native_PlaySfx(sfx);

    if (g_ui->TakeMainMenuRequested() && !g_mainMenuReleaseAt) {
        StartBlack();
        g_mainMenuReleaseAt = GetTickCount64() + kOnlineFadeOutMs;
    }
    // The title area is not rebuilt, and no match set up, over a save that
    // is still being written.
    const bool saving = PumpReserveSave();
    if (g_mainMenuReleaseAt && GetTickCount64() >= g_mainMenuReleaseAt && !saving) {
        g_mainMenuReleaseAt = 0;
        ReleaseToMainMenu(titleArea);
        return;
    }

    PumpSpectate();
    UpdateSpectateJoin(input.cancel);
    UpdateMatchStart(input.cancel);

    if (Canvas* canvas = Native_BeginFrame()) {
        if (g_entrancePending) {
            // First frame the lobby can be seen on this visit: play the
            // screen's slide-in now, after most of any fade from black.
            g_entrancePending = false;
            // The reserve units may have been edited on the license screen
            // since the last visit: read them again, and bring a team that
            // is one of them up to date.
            g_presetsLoaded = false;
            g_ui->RefreshPreset();
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
           (g_phase == Phase::Lobby || g_phase == Phase::WatchJoining ||
            g_phase == Phase::Connecting || g_phase == Phase::Handshake);
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

    // Watching: who is playing (and the set score), with the state of the
    // feed under it when there is something to say.
    // F3 hides it, except the out-of-sync warning: the picture is wrong then.
    const bool overlayShown = g_spectatorOverlay.load(std::memory_order_relaxed);
    if (g_phase == Phase::InMatch && g_spectating &&
        (overlayShown || Spectate_GetStatus().desynced)) {
        char line[sizeof(g_watchLine)];
        memcpy(line, g_watchLine, sizeof(line));
        line[sizeof(line) - 1] = '\0';
        const SpectateStatus status = Spectate_GetStatus();
        const char* note = status.desynced ? "DESYNCED"
                         : status.feedLost ? "RECONNECTING..."
                         : status.catchingUp ? "CATCHING UP..."
                         : status.waiting ? "WAITING FOR THE MATCH..." : "SPECTATING";
        const ImVec2 display = ImGui::GetIO().DisplaySize;
        ImFont* font = ImGui::GetFont();
        ImDrawList* draw = ImGui::GetBackgroundDrawList();
        const auto centred = [&](const char* text, float size, float y, ImU32 color) {
            const ImVec2 extent = font->CalcTextSizeA(size, 100000.0f, 0.0f, text);
            const ImVec2 at((display.x - extent.x) * 0.5f, y);
            draw->AddText(font, size, ImVec2(at.x + 2.0f, at.y + 2.0f), IM_COL32(0, 0, 0, 220), text);
            draw->AddText(font, size, at, color, text);
        };
        if (overlayShown)
            centred(line, display.y * 0.03f, display.y * 0.012f, IM_COL32(255, 255, 255, 255));
        centred(note, display.y * 0.022f, display.y * 0.047f,
                status.desynced ? IM_COL32(255, 110, 90, 255) : IM_COL32(120, 220, 255, 255));
    }

    // Set score during a room match, top centre between the health bars.
    if (g_phase == Phase::InMatch && !g_spectating && g_setFirstTo != 1) {
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
        ImGui::TextUnformatted("Details are in Timestone.log.");
        ImGui::TextUnformatted("Launch with -launcher to use the launcher.");
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
            TrainingBoot_RestoreLocalBindings();
            FrameStepper_EndNetplaySession();
            SetNetworkHandles(nullptr, nullptr);
            TrainingBoot_PrepareNextBoot(false);
            if (g_spectating) StopSpectating();
            else g_service->FinishMatch(false, false);
            g_ui->SetBusy(nullptr);
            SetPhase(Phase::Lobby);
            return false;
        }
        PumpSpectate();
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
        // Back from a netplay match: P2 gets the player's own bindings again.
        TrainingBoot_RestoreLocalBindings();
        Native_Invalidate();
        RetireNativeTitleUnits(titleArea);
        ApplyPressStartState();
        EnsureObjects();
        if ((g_phase == Phase::Returning || g_phase == Phase::InMatch) && g_spectating) {
            // Back from watching: nothing of the match was ours.
            StopSpectating();
            ResetSet(1);
            g_ui->ReturnToRoom();
            g_entrancePending = false;
            SetPhase(Phase::Lobby);
        } else if (g_phase == Phase::Returning || g_phase == Phase::InMatch) {
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
        // A fresh visit, not back from a match: check for (or re-offer) an update.
        if (g_entrancePending) g_updateOfferPending = true;
        HookLog("LobbyFlow: title area ready for the lobby (area=%llx)\n",
                static_cast<unsigned long long>(titleArea));
    }
    // The area can respawn its menu units on a later frame of the same visit.
    RetireNativeTitleUnits(titleArea);
    HideNativeMenuChrome();

    if (PumpBootSavedataLoad()) return false;

    // The lobby is about to run: startup is over (dllmain.cpp).
    HookLog_MarkGameLoaded();
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

    // An invite accepted on the main menu opens the lobby as Online does.
    // Once per invite: one that cannot be joined (Steam is down) must not
    // send the player back to the lobby every time they leave it.
    static uint64_t openedFor = 0;
    const uint64_t invited = SteamLobby_QueuedJoin();
    if (invited && invited != openedFor && !g_onlineChosen) {
        HookLog("LobbyFlow: invite accepted on the main menu; opening the lobby\n");
        openedFor = invited;
        g_onlineChosen = true;
        StartBlack();
    }

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
    // In the title area the lobby frame pumps the feed; everywhere else, here.
    if (g_phase == Phase::InMatch || g_phase == Phase::Returning) PumpSpectate();
    if (g_phase != Phase::InMatch) {
        g_returnRequested.store(false, std::memory_order_relaxed);
        return;
    }

    if (g_spectating) {
        UpdateWatchText();
        const Member* local = LocalRoomMember();
        const ULONGLONG now = GetTickCount64();
        if (local && local->slot >= 0) {
            // A queue reached the front: the seat comes before the show.
            HookLog("LobbyFlow: seated while watching; back to the room\n");
            LobbyFlow_RequestReturnToLobby("");
        } else if (!Spectate_GetStatus().feedLost) {
            g_feedLostTick = 0;
        } else if (!g_feedLostTick) {
            g_feedLostTick = now;
        } else if (now - g_feedLostTick > 10000) {
            LobbyFlow_RequestReturnToLobby("The connection to the match was lost.");
        }
    } else {
        UpdateSet();
        SampleTeamHealth();
        if (Netplay_TakePeerDisconnected())
            LobbyFlow_RequestReturnToLobby(Netplay_PeerLeftByChoice()
                ? "Your opponent left the match." : "Connection to opponent has been lost.");
    }
    // An invite to another room was accepted: leave the match as if from its
    // menu; the room is changed once back at the title.
    if (g_service && g_service->RoomChangeWaiting() &&
        !g_returnRequested.load(std::memory_order_relaxed)) {
        HookLog("LobbyFlow: invite accepted during the match; leaving it\n");
        LobbyFlow_RequestLeaveMatch();
    }
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

bool LobbyFlow_IsSpectating()
{
    return g_enabled && g_spectating;
}

bool LobbyFlow_GetWatchedNames(const char** p1, const char** p2)
{
    if (!g_enabled || !g_spectating || !g_watchNames[0][0] || !g_watchNames[1][0]) return false;
    if (p1) *p1 = g_watchNames[0];
    if (p2) *p2 = g_watchNames[1];
    return true;
}

void LobbyFlow_RequestLeaveMatch()
{
    if (!LobbyFlow_InRoomMatch()) return;
    if (g_spectating) {
        // Only the watching ends; a place in a queue is kept, and the room
        // does not send the player straight back in.
        g_watchOptOutRequested.store(true, std::memory_order_release);
        LobbyFlow_RequestReturnToLobby("");
        return;
    }
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

void LobbyFlow_ToggleSpectatorOverlay()
{
    const bool shown = !g_spectatorOverlay.load(std::memory_order_relaxed);
    g_spectatorOverlay.store(shown, std::memory_order_relaxed);
    char path[MAX_PATH] = {};
    LaunchMenuUtils::GetSettingsPath(path, sizeof(path));
    if (!path[0] || !IniSafe_WriteString("Lobby", "SpectatorOverlay", shown ? "1" : "0", path))
        HookLog("LobbyFlow: failed to save the spectator overlay setting\n");
    HookLog("LobbyFlow: spectator overlay %s\n", shown ? "shown" : "hidden");
}

SpectatorInfo LobbyFlow_GetSpectatorInfo()
{
    SpectatorInfo info;
    memcpy(&info, &g_watchInfo, sizeof(info));
    for (auto* person : {info.watchers, info.queue})
        for (int i = 0; i < SpectatorInfo::kMax; ++i) person[i].name[sizeof(person[i].name) - 1] = '\0';
    info.feed[sizeof(info.feed) - 1] = '\0';
    if (info.watcherCount > SpectatorInfo::kMax) info.watcherCount = SpectatorInfo::kMax;
    if (info.queueCount > SpectatorInfo::kMax) info.queueCount = SpectatorInfo::kMax;
    return info;
}
