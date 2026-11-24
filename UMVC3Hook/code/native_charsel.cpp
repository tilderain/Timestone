// The game's character select screen as a shared screen for a room match.
// Design and reverse engineering: doc/native_character_select.md.
#include "../pch.h"
#include "native_charsel.h"

#include "chr_colors.h"
#include "mk10utils.h"
#include "../kiero/minhook/include/MinHook.h"

#include <Windows.h>
#include <algorithm>
#include <cstdint>
#include <cstring>

extern void HookLog(const char* fmt, ...);

using lobby::Team;

namespace charsel {
namespace {

// ---------------------------------------------------------------------------
// Native addresses
// ---------------------------------------------------------------------------
constexpr uintptr_t SAREA_INSTANCE_PTR = 0x140E17A80;
constexpr size_t SAREA_VT_REQUEST_TRANSITION = 0x50;
constexpr size_t SAREA_PENDING_DTI = 0x7040;
constexpr uintptr_t ACHRSELECT_DTI = 0x140D50128;
constexpr uintptr_t ATITLE_DTI = 0x140D506E8;

constexpr uintptr_t SGAMEUI_GET_INSTANCE = 0x140001AC0;
constexpr uintptr_t SBATTLESETTING_GET_INSTANCE = 0x140004700;
constexpr uintptr_t SGAMECONFIG_GET_INSTANCE = 0x140001AB0;
constexpr uintptr_t SMVC3MANAGER_INSTANCE_PTR = 0x140D510A0;
constexpr size_t MANAGER_PAD_ASSIGN = 0x140; // int32_t[4], pad slot per player index

constexpr uintptr_t SBATTLESETTING_SET_BATTLE_TYPE = 0x14024E050; // (this, type)
constexpr uintptr_t SGAMEUI_SET_CSS_STATE = 0x140214AC0;          // (this, state)
// While set, aChrSelect::final drops every character archive on its way out.
// The game clears it just before it enters a battle from this screen.
constexpr uintptr_t SGAMEUI_SET_BATTLE_TRANSITION = 0x140209F30;  // (this, active)
constexpr size_t SGAMEUI_SLOT_STATE = 0x58D8;                     // int32_t[2]
constexpr uintptr_t SGAMECONFIG_GET_CHARACTER_VOICE = 0x140251BD0; // (this, id) -> 0 / 1
// With both set the screen applies the saved control type without asking;
// otherwise it opens a Normal / Simple chooser that reads both players' pads.
constexpr size_t SGAMECONFIG_CTRL_TYPE_SET = 0x190;
constexpr size_t SGAMECONFIG_CTRL_TYPE_FIXED = 0x191;

// sBattleSetting's six live character records (cMainChr, 0x58 bytes each);
// the screen writes team * 3 + member.
constexpr size_t BS_SLOTS = 0x38;
constexpr size_t BS_SLOT_SIZE = 0x58;
constexpr size_t SLOT_TEAM = 0x08;
constexpr size_t SLOT_CHARACTER = 0x0C;
constexpr size_t SLOT_COLOR = 0x14;
constexpr size_t SLOT_ASSIST = 0x1C;
constexpr size_t SLOT_VOICE = 0x44;
constexpr uintptr_t SLOT_SET_CHARACTER = 0x140146560; // (record, id): id and DTI
constexpr uintptr_t SLOT_SET_COLOR = 0x1401461E0;     // (record, color)
constexpr uintptr_t SLOT_SET_ASSIST = 0x140024D80;    // (record, assist)

// uMenuChrSel
constexpr size_t MENU_BG_MAIN = 0xF0;       // uMenuChrSelBgMain*
constexpr size_t MENU_CURSOR = 0x120;       // uMenuChrSelCursor*[2]
constexpr size_t MENU_ASSIST = 0x150;       // uMenuChrSelAssist*[2]
constexpr size_t MENU_RESERVE_UNIT = 0x160; // uMenuChrSelReserveUnit*[2]
// {grid index, color, assist} per team and member, 12 bytes each. The tick
// copies the first two to sGameUI, where the card reads the member being
// hovered.
constexpr size_t MENU_RECORDS = 0x178;
constexpr size_t MENU_MODE = 0x2B4;         // int32_t[2], mirrors the sGameUI slot states
constexpr size_t BGMAIN_TIME_INFINITE = 0xB1;
constexpr size_t BGMAIN_CURSOR_POS = 0xBC;  // uint32_t[2], the grid highlight
constexpr size_t BGMAIN_TIME_LIMIT = 0xCC;  // float, frames
constexpr size_t CURSOR_SELECTION = 0xC4;   // mCursor.mSelection
constexpr size_t ASSIST_PHASE = 0x09;       // 0 closed, 1 opening, 2 taking input, 3/4 moving
constexpr size_t ASSIST_HIGHLIGHT = 0x0A;
constexpr size_t ASSIST_SCHEDULER = 0x58;
constexpr size_t ASSIST_FIRST_UNIT = 0xB0;
constexpr size_t CUNIT_FLAGS = 0x10;
constexpr size_t CUNIT_VT_KILL = 0x78;

constexpr uintptr_t MENU_TICK = 0x140368300;        // uMenuChrSel__tickSlotStateMachine
constexpr uintptr_t MENU_STATE_0 = 0x140363D80;     // (menu, team)
constexpr uintptr_t MENU_STATE_1 = 0x140363350;     // (menu, team)
constexpr uintptr_t MENU_STATE_246 = 0x140364C50;   // (menu, team, member, state, record, field)
constexpr uintptr_t MENU_STATE_357 = 0x140362EF0;   // (menu, team, member, r9)
constexpr uintptr_t MENU_STATE_11 = 0x140366390;    // (menu, team)
constexpr uintptr_t MENU_COMMIT_RESERVE = 0x140368BF0; // (menu, team, unit)
constexpr uintptr_t MENU_FIND_GRID_INDEX = 0x140361F50; // (characterId) -> cell
constexpr uintptr_t ASSIST_CAPTURE_INPUT = 0x14036A7F0; // (widget, team, out)
constexpr uintptr_t ASSIST_OPEN = 0x14036BC50;          // (widget, record index, assist)
constexpr uintptr_t ASSIST_CLOSE = 0x14036AB20;         // (widget)
constexpr uintptr_t SBATTLESETTING_REQUEST_ENTER = 0x14024B530;
// A copy of the license profile; the reserve units are int32_t[3][3] tables.
constexpr uintptr_t SLICENSE_COPY_PROFILE = 0x140203550; // (license, out)
constexpr uintptr_t COMMIT_RESERVE_COPY_RETURN = 0x140368C3A;
constexpr size_t PROFILE_RESERVE_CHARS = 0xB8;
constexpr size_t PROFILE_RESERVE_ASSISTS = 0xDC;
constexpr size_t PROFILE_RESERVE_COLORS = 0x100;
// byte[60]: 1 for an id the select screen offers.
constexpr uintptr_t CHARACTER_SELECTABLE_TABLE = 0x140A6FD30;
constexpr int kSelectableTableSize = 60;

// The stage select page (uMenuChrSelOption). It always reads player index 0,
// and in versus it has two rows of match options under the stage.
constexpr size_t MENU_STAGE_PAGE = 0xF8;    // uMenuChrSelOption*
constexpr size_t MENU_STAGE_SELECT = 0x2C0; // the team whose "complete" opens the page
// Set by picking the roster's RANDOM ALL cell (slotState246): from the member
// being picked on, the screen draws each one itself. Cleared by a step back.
constexpr size_t MENU_RAND_ALL = 0x2C4;     // uint8_t[2]
// Frames each team has been "complete". Team 0's handler waits for both to
// pass 5, and each only counts in its own handler.
constexpr size_t MENU_WAIT_COUNT = 0x2CC;   // float[2]
// The reserve unit a team was filled from, -1 for none; the versus sequence
// records it in the license profile as the unit last used.
constexpr size_t MENU_RESERVE_ID = 0x2F4;   // int32_t[2]
constexpr size_t OPTION_STATE = 0x09;       // 4 stage row, 6 option rows, 0xC done
constexpr size_t OPTION_ROW = 0x0A;
constexpr size_t OPTION_BG_MAIN = 0x1C8;
constexpr size_t OPTION_PAGE_LEFT = 0x1DA;
constexpr size_t OPTION_DECIDED = 0x1DB;    // set at confirm; input is ignored from then
constexpr size_t OPTION_DRAW_STAGE = 0x1EC; // position whose picture is up
constexpr size_t OPTION_PAGE_COUNT = 0x1F0; // frames until the picture follows the cursor
constexpr size_t OPTION_STAGE_CURSOR = 0x1F4; // 0..18 into the table below
constexpr size_t OPTION_DIRECTION = 0x234;
constexpr size_t OPTION_INPUT_VALID = 0x238;
constexpr uintptr_t OPTION_UPDATE_STATE = 0x140376EE0;   // (page)
constexpr uintptr_t OPTION_SAMPLE_INPUT = 0x140379BF0;   // (page)
constexpr uintptr_t OPTION_CONFIRM_INPUT = 0x1403795D0;  // (page)
constexpr uintptr_t OPTION_INPUT_MASK = 0x1403767E0;     // (page, mask): 1 left, 2 right, 4 down, 8 up
constexpr uintptr_t OPTION_FORCE_DECIDE = 0x140376560;   // (page): what the game's clock does at 0
// Starts an idle page (state 2): it then loads its options and plays its
// opening. The game calls this from the waiting team's state 0x10
// handler (0x1403649B0), once the background has finished changing over.
constexpr uintptr_t OPTION_START = 0x140376070;          // (page)
constexpr uintptr_t BGMAIN_STATUS_KIND = 0x14036D650;    // (background) -> 0 while still on the roster
// What the game plays when a reserve unit is taken: the decide cue (bank 1),
// then from the select screen's bank the team-complete cue and the announcer.
constexpr uintptr_t GSOUND_GET_INSTANCE = 0x140003AA0;
constexpr uintptr_t GSOUND_RESOLVE_REF = 0x1402AA2B0;         // (this, cRef* out, bank)
constexpr uintptr_t GSOUND_PLAY_ORDINARY_VOICE = 0x1402AAC40; // (this, cRef* out, ref, id, team, 1.0f, 0)
// The announcer calling a character's name, as a pick does (slotState246).
constexpr uintptr_t GSOUND_PLAY_CHARACTER_NAME = 0x1402AC980; // (this, cRef* out, character id)
constexpr uintptr_t BGMAIN_DELAY_ELAPSED = 0x14036DCB0;  // (background) -> bool
constexpr uintptr_t BGMAIN_SET_STAGE = 0x140378CA0;      // (background, table value)
constexpr uintptr_t STAGE_PAGE_TABLE = 0x140B400C0;      // int32_t[19], [0] = 0x13 (random)
constexpr int kStagePositions = 19;
constexpr size_t SGAMEUI_SELECTED_STAGE = 0x5934;        // a stage id, or 0x13 for random
constexpr size_t BS_STAGE_AREA = 0x358;
constexpr size_t BS_TIME_LIMIT = 0x35C;
constexpr uintptr_t SBATTLESETTING_SET_AREA_INDEX = 0x1401CF670; // (this, stage id)
// Opening the stage page applies the player's saved versus options to the
// match: the time limit and sAction's damage rate. Both are put back.
constexpr uintptr_t SACTION_GET_INSTANCE = 0x140001A90;
constexpr uintptr_t SCHARACTER_GET_INSTANCE = 0x140001AA0;
constexpr uintptr_t SCHARACTER_ARCHIVES_READY = 0x1400592F0; // (this, -1, 0, 0, 0, 0): all of them
constexpr size_t SACTION_DAMAGE_RATE = 0x128;
// What the stage page can produce (the table in StageSelectArray, 0x1402BC5D0).
constexpr int32_t kStageIds[] = {105, 5, 104, 4, 110, 10, 103, 3, 102, 2,
                                 109, 9, 107, 7, 106, 6, 100, 0, 11};

constexpr uint8_t kStateTeamComplete = 0x0B;
constexpr uint8_t kStateDone = 0x11;
constexpr uint32_t kGridCells = 56;
constexpr ULONGLONG kOpenTimeoutMs = 15000;
constexpr ULONGLONG kRepeatMs = 250;
// After a session ends its screen is held still for this long, by which time
// its area is gone.
constexpr uint32_t kFreezeFrames = 120;
// The game decides for the player when its own clock reaches 0, with random
// characters. It is never allowed to: the clock shown stops a second short
// and the session's own deadline puts the player's team in.
constexpr float kMinClockFrames = 61.0f;

template <typename T> T& Field(void* object, size_t offset)
{
    return *reinterpret_cast<T*>(static_cast<uint8_t*>(object) + offset);
}

template <typename Fn> Fn Native(uintptr_t address)
{
    return reinterpret_cast<Fn>(_addr(address));
}

void* Instance(uintptr_t getter)
{
    using GetInstance = void*(__fastcall*)();
    return Native<GetInstance>(getter)();
}

bool UnitAlive(void* unit)
{
    if (!unit) return false;
    const uint32_t state = Field<uint32_t>(unit, CUNIT_FLAGS) & 7u;
    return state == 1 || state == 2;
}

// ---------------------------------------------------------------------------
// Session
// ---------------------------------------------------------------------------
struct Session {
    bool active = false;
    // The session is over but the screen is still there: nothing on it may
    // move until its area is gone.
    uint32_t freezeUntilFrame = 0;
    Config config = {};
    Status status = Status::Idle;
    ULONGLONG beginTick = 0;
    ULONGLONG deadlineTick = 0;
    uint32_t frame = 0;
    void* menu = nullptr;
    uint32_t menuFrame = 0;

    bool localLocked = false;
    bool presetListOpen = false;
    bool teamPending = false; // ApplyLocalTeam: put in at the next tick
    Team pendingTeam = {};
    uint8_t pendingRandomMask = 0;
    // RANDOM ALL on the local side: the first member it covers while the
    // screen's own flag is up (-1 otherwise), and what the locked-in team
    // ends with. A team put in whole brings its own mask.
    int randomFrom = -1;
    uint8_t localRandomMask = 0;
    bool randomMaskGiven = false;
    uint8_t remoteRandomMask[2] = {};
    Team localTeam = {};
    CharSelPacket local = {};
    bool localValid = false;
    CharSelPacket lastSent = {};
    ULONGLONG lastSentTick = 0;
    uint32_t sequence = 0;

    // A team drawn from its owner's packets, by team index: the other
    // player's, and for a spectator both.
    CharSelPacket remote[2] = {};
    bool remoteValid[2] = {};
    bool remoteLocked[2] = {};
    Team remoteTeam[2] = {};
    uint8_t remoteAssistOpen[2] = {}; // the state the assist list was opened for
    bool remoteFinalApplied[2] = {};  // the locked-in team is on the screen
    uint8_t coin[2] = {};             // one bit per team, XORed: which team picks the stage
    uint32_t remoteSoundFrame[2] = {}; // tick a side's step last made a sound on
    // The stage select, as the picking team's packets describe it.
    uint8_t remoteStageCursor = 0;
    bool remoteStageDecided = false;
    int32_t remoteStageId = 0;

    // Both teams final: the game's stage select, then its versus sequence.
    ULONGLONG stageDeadlineTick = 0;
    ULONGLONG stageLogTick = 0;
    void* stagePageStarted = nullptr; // the page the start call was made on
    uint8_t localStageCursor = 0;
    bool stageDecided = false;
    int32_t stageId = 0;
    bool enterRequested = false; // the game asked to enter the battle
    bool badPacketLogged = false;

    bool commitOverride = false; // the reserve-unit commit takes overrideTeam
    Team overrideTeam = {};

    bool saved = false; // Begin's changes are still in place
    uint8_t savedCtrlSet = 0, savedCtrlFixed = 0;
    int32_t savedPad1 = 0;
    int32_t savedTimeLimit = 0;
    float savedDamageRate = 1.0f;
};

Session g;
bool g_installed = false;

// A spectator's screen: no team on it is the local player's.
bool Watching() { return g.config.watch; }
// A team that is drawn from packets and never run from the local pad.
bool IsRemoteTeam(int team) { return Watching() || team != g.config.localTeam; }
bool TeamLocked(int team) { return IsRemoteTeam(team) ? g.remoteLocked[team] : g.localLocked; }
bool Frozen() { return !g.active && g.frame < g.freezeUntilFrame; }
// Both teams are final and on the screen: from here the stage picker's slot
// runs the game's own code on both clients (it opens the stage page), and the
// other team's waits.
bool StagePhase()
{
    for (int team = 0; team < 2; ++team) {
        if (!TeamLocked(team)) return false;
        if (IsRemoteTeam(team) && !g.remoteFinalApplied[team]) return false;
    }
    return true;
}
// The team that picks the stage: a coin toss both clients work out alike from
// the bit each player sent. Only meaningful in the stage phase.
int StageTeam() { return (g.coin[0] ^ g.coin[1]) & 1; }
bool LocalPicksStage() { return !Watching() && StageTeam() == g.config.localTeam; }

bool IsValidStage(int32_t stageId)
{
    for (const int32_t known : kStageIds)
        if (known == stageId) return true;
    return false;
}

// Members whose character is decided in a slot state, and members whose
// assist is: 2/4/6 pick member 0/1/2, 3/5/7 pick that member's assist.
int PickedCount(uint8_t state)
{
    if (state >= kStateTeamComplete) return 3;
    return state >= 7 ? 3 : state >= 5 ? 2 : state >= 3 ? 1 : 0;
}

int AssistCount(uint8_t state)
{
    if (state >= kStateTeamComplete) return 3;
    return state >= 6 ? 2 : state >= 4 ? 1 : 0;
}

bool IsAssistState(uint8_t state) { return state == 3 || state == 5 || state == 7; }

void* SlotRecord(void* battleSetting, int team, int member)
{
    return static_cast<uint8_t*>(battleSetting) + BS_SLOTS + (team * 3 + member) * BS_SLOT_SIZE;
}

bool IsValidTeam(const Team& team)
{
    for (int i = 0; i < 3; ++i) {
        if (!IsSelectableCharacter(team.chars[i]) ||
            !ChrColors_Available(team.chars[i], team.colors[i]) || team.assists[i] < 0 ||
            team.assists[i] > 2)
            return false;
        for (int j = 0; j < i; ++j)
            if (team.chars[j] == team.chars[i]) return false;
    }
    return true;
}

// Every field of a packet is checked here, before any of it reaches the game.
bool IsValidPeerPacket(const CharSelPacket& packet)
{
    const uint8_t state = packet.slotState;
    if (!(state <= 7 || state == kStateTeamComplete)) return false;
    if (packet.cursorCell >= kGridCells || packet.assistHover > 2) return false;
    if (packet.flags &
        ~(CHARSEL_LOCKED_IN | CHARSEL_PRESET_LIST | CHARSEL_LEFT | CHARSEL_STAGE_DECIDED))
        return false;
    if ((packet.flags & CHARSEL_LOCKED_IN) && state != kStateTeamComplete) return false;
    if (packet.stageCursor >= kStagePositions || packet.coin > 1) return false;
    if (packet.randomMask > 7) return false;
    if ((packet.flags & CHARSEL_STAGE_DECIDED) &&
        (!(packet.flags & CHARSEL_LOCKED_IN) || !IsValidStage(packet.stageId)))
        return false;
    const int picked = PickedCount(state);
    for (int i = 0; i < 3; ++i) {
        if (packet.assists[i] < 0 || packet.assists[i] > 2) return false;
        if (i < picked) {
            if (!IsSelectableCharacter(packet.chars[i]) ||
                !ChrColors_Available(packet.chars[i], packet.colors[i]))
                return false;
            for (int j = 0; j < i; ++j)
                if (packet.chars[j] == packet.chars[i]) return false;
        } else if (packet.chars[i] != 0 || packet.colors[i] < 0 ||
                   packet.colors[i] >= kChrColorSlots) {
            return false;
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// The remote team: drawn from the last packet, never simulated
// ---------------------------------------------------------------------------
void WriteRemoteSlots(void* battleSetting, int team, const CharSelPacket& state)
{
    using SetSlotValue = void(__fastcall*)(void*, int32_t);
    using GetVoice = bool(__fastcall*)(void*, uint32_t); // the result is one byte
    void* config = Instance(SGAMECONFIG_GET_INSTANCE);
    const int picked = PickedCount(state.slotState);
    const int assists = AssistCount(state.slotState);
    for (int member = 0; member < 3; ++member) {
        void* record = SlotRecord(battleSetting, team, member);
        if (member >= picked) {
            if (Field<int32_t>(record, SLOT_CHARACTER) != 0)
                Native<SetSlotValue>(SLOT_SET_CHARACTER)(record, 0);
            continue;
        }
        Field<int32_t>(record, SLOT_TEAM) = team;
        if (Field<int32_t>(record, SLOT_CHARACTER) != state.chars[member]) {
            Native<SetSlotValue>(SLOT_SET_CHARACTER)(record, state.chars[member]);
            // The card asks for the character's archives with this, as it
            // does for a local pick. The battle boot sets the real one.
            // 0 Japanese, 1 English; the Marvel cast (ids above 25) only has
            // English. A value outside that names an archive that does not
            // exist, and "team complete" then waits on it for ever.
            const int32_t id = state.chars[member];
            const bool english = id > 25 || !config ||
                Native<GetVoice>(SGAMECONFIG_GET_CHARACTER_VOICE)(config, static_cast<uint32_t>(id));
            Field<uint32_t>(record, SLOT_VOICE) = english ? 1u : 0u;
        }
        if (Field<int32_t>(record, SLOT_COLOR) != state.colors[member])
            Native<SetSlotValue>(SLOT_SET_COLOR)(record, state.colors[member]);
        if (member < assists && Field<int32_t>(record, SLOT_ASSIST) != state.assists[member])
            Native<SetSlotValue>(SLOT_SET_ASSIST)(record, state.assists[member]);
    }
}

void DriveRemoteAssistList(void* menu, int team, const CharSelPacket& state)
{
    void* widget = Field<void*>(menu, MENU_ASSIST + 8 * team);
    uint8_t& open = g.remoteAssistOpen[team];
    if (!UnitAlive(widget)) {
        open = 0;
        return;
    }
    const uint8_t wanted = IsAssistState(state.slotState) ? state.slotState : 0;
    if (open && open != wanted) {
        using Close = void(__fastcall*)(void*);
        Native<Close>(ASSIST_CLOSE)(widget);
        open = 0;
    }
    if (!wanted) return;
    if (!open) {
        // Its layout is bound a few frames after the screen appears.
        if (!Field<void*>(widget, ASSIST_SCHEDULER) || !Field<void*>(widget, ASSIST_FIRST_UNIT))
            return;
        using Open = void(__fastcall*)(void*, int32_t, int32_t);
        Native<Open>(ASSIST_OPEN)(widget, team * 3 + (wanted - 3) / 2, state.assistHover);
        open = wanted;
        return;
    }
    // Shown without the step animation between two assists.
    if (Field<uint8_t>(widget, ASSIST_PHASE) == 2)
        Field<uint8_t>(widget, ASSIST_HIGHLIGHT) = state.assistHover;
}

void ApplyRemote(void* menu, int team)
{
    if (!g.remoteValid[team]) return;
    // Once its final team is on the screen the remote side is left alone:
    // the stage select kills the cursors and assist lists written to here.
    if (g.remoteFinalApplied[team]) return;
    void* gameUi = Instance(SGAMEUI_GET_INSTANCE);
    void* battleSetting = Instance(SBATTLESETTING_GET_INSTANCE);
    if (!gameUi || !battleSetting) return;
    const CharSelPacket& state = g.remote[team];

    // The card reads the descriptor when the slot state changes, so the
    // picks go in first.
    WriteRemoteSlots(battleSetting, team, state);

    using FindGridIndex = uint64_t(__fastcall*)(int32_t);
    const int picked = PickedCount(state.slotState);
    const bool onRoster = state.slotState == 2 || state.slotState == 4 || state.slotState == 6;
    for (int member = 0; member < 3; ++member) {
        uint8_t* record = static_cast<uint8_t*>(menu) + MENU_RECORDS + (team * 3 + member) * 12;
        uint32_t cell = 0xFFFFFFFFu;
        if (member < picked)
            cell = static_cast<uint32_t>(
                Native<FindGridIndex>(MENU_FIND_GRID_INDEX)(state.chars[member]));
        else if (member == picked && onRoster)
            cell = state.cursorCell;
        *reinterpret_cast<uint32_t*>(record) = cell;
        *reinterpret_cast<int32_t*>(record + 4) = state.colors[member];
        *reinterpret_cast<int32_t*>(record + 8) = state.assists[member];
    }
    if (void* background = Field<void*>(menu, MENU_BG_MAIN))
        Field<uint32_t>(background, BGMAIN_CURSOR_POS + 4 * team) = state.cursorCell;
    if (void* cursor = Field<void*>(menu, MENU_CURSOR + 8 * team))
        Field<int32_t>(cursor, CURSOR_SELECTION) = state.cursorCell;

    DriveRemoteAssistList(menu, team, state);
    Field<int32_t>(gameUi, SGAMEUI_SLOT_STATE + 4 * team) = state.slotState;
    if (g.remoteLocked[team]) g.remoteFinalApplied[team] = true;
}

// The stage select and what follows it. The picking team's slot goes 0xB ->
// 0xF (page open) -> 0x11 by the game's own code; the other's is held at 0xB
// and moved to 0x11 with it, which is what lets the tick report "selection
// over" and the versus sequence start. Its own 0xF / 0x10 states (the
// handicap unit) never run.
void DriveStagePhase(void* menu)
{
    void* gameUi = Instance(SGAMEUI_GET_INSTANCE);
    void* battleSetting = Instance(SBATTLESETTING_GET_INSTANCE);
    if (!gameUi || !battleSetting) return;
    const ULONGLONG now = GetTickCount64();
    if (!g.stageDeadlineTick) {
        g.stageDeadlineTick = now + static_cast<ULONGLONG>(g.config.stageSeconds) * 1000;
        HookLog("NativeCharSel: both teams are final; stage select (player %d picks)\n",
                StageTeam() + 1);
    }
    const int picker = StageTeam();
    Field<int32_t>(menu, MENU_STAGE_SELECT) = picker;
    // The other team's handler never runs to count its own frames.
    Field<float>(menu, MENU_WAIT_COUNT + 4 * (1 - picker)) = 10.0f;
    int32_t& selector = Field<int32_t>(gameUi, SGAMEUI_SLOT_STATE + 4 * picker);
    int32_t& waiting = Field<int32_t>(gameUi, SGAMEUI_SLOT_STATE + 4 * (1 - picker));
    if (selector < kStateDone) {
        if (waiting != kStateTeamComplete) waiting = kStateTeamComplete;
        // The page is created idle and waits to be started. That is the
        // waiting team's state 0x10 handler's job, which never runs here.
        void* stagePage = Field<void*>(menu, MENU_STAGE_PAGE);
        void* background = Field<void*>(menu, MENU_BG_MAIN);
        // (Idle is its state 2, which only keeps its widgets hidden; the
        // start call has its own "already started" flag.)
        if (selector == 0x0F && UnitAlive(stagePage) && g.stagePageStarted != stagePage &&
            Field<uint8_t>(stagePage, OPTION_STATE) == 2 && background) {
            using StatusKind = uint64_t(__fastcall*)(void*);
            using DelayElapsed = bool(__fastcall*)(void*);
            using Start = void(__fastcall*)(void*);
            if (static_cast<int32_t>(Native<StatusKind>(BGMAIN_STATUS_KIND)(background)) != 0 &&
                Native<DelayElapsed>(BGMAIN_DELAY_ELAPSED)(background)) {
                Native<Start>(OPTION_START)(stagePage);
                g.stagePageStarted = stagePage;
                HookLog("NativeCharSel: stage page started\n");
            }
        }
        // Should the page not come up, say every five seconds where the
        // picking team's slot is waiting.
        if (now >= g.stageLogTick) {
            if (g.stageLogTick) {
                using Ready = uint64_t(__fastcall*)(void*, int32_t, uint32_t, uint32_t, int32_t, int32_t);
                void* characters = Instance(SCHARACTER_GET_INSTANCE);
                const bool ready = characters &&
                    (Native<Ready>(SCHARACTER_ARCHIVES_READY)(characters, -1, 0, 0, 0, 0) & 0xFF);
                HookLog("NativeCharSel: stage select pending: picker state %d other %d page %p "
                        "(state %d) wait %.0f/%.0f archives ready %d\n", selector, waiting,
                        stagePage,
                        UnitAlive(stagePage) ? Field<uint8_t>(stagePage, OPTION_STATE) : -1,
                        Field<float>(menu, MENU_WAIT_COUNT),
                        Field<float>(menu, MENU_WAIT_COUNT + 4), ready ? 1 : 0);
            }
            g.stageLogTick = now + 5000;
        }
        // Out of time: what the game's own clock does, the stage under the
        // cursor. Only where the page is the player's to drive.
        void* page = Field<void*>(menu, MENU_STAGE_PAGE);
        if (LocalPicksStage() && now >= g.stageDeadlineTick && UnitAlive(page) &&
            Field<uint8_t>(page, OPTION_STATE) >= 4) {
            using ForceDecide = void(__fastcall*)(void*);
            Native<ForceDecide>(OPTION_FORCE_DECIDE)(page);
        }
        return;
    }
    if (!g.stageDecided) {
        if (LocalPicksStage()) {
            g.stageId = Field<int32_t>(battleSetting, BS_STAGE_AREA);
        } else {
            // The page here was shown what the other player picked, random
            // included.
            g.stageId = g.remoteStageId;
            using SetInt = void(__fastcall*)(void*, int32_t);
            Native<SetInt>(SBATTLESETTING_SET_AREA_INDEX)(battleSetting, g.stageId);
            Field<int32_t>(gameUi, SGAMEUI_SELECTED_STAGE) = g.stageId;
        }
        g.stageDecided = true;
        HookLog("NativeCharSel: stage %d decided; versus sequence\n", g.stageId);
    }
    if (waiting < kStateDone) waiting = kStateDone;
}

// ---------------------------------------------------------------------------
// The local team
// ---------------------------------------------------------------------------
uint8_t LocalSlotState()
{
    void* gameUi = Instance(SGAMEUI_GET_INSTANCE);
    if (!gameUi) return 0;
    const int32_t state = Field<int32_t>(gameUi, SGAMEUI_SLOT_STATE + 4 * g.config.localTeam);
    return state < 0 ? 0 : state > 0x12 ? 0x12 : static_cast<uint8_t>(state);
}

void PlayVoice(uint32_t bank, uint32_t id, int team)
{
    void* sound = Instance(GSOUND_GET_INSTANCE);
    if (!sound) return;
    using ResolveRef = void*(__fastcall*)(void*, void*, uint32_t);
    using Play = void*(__fastcall*)(void*, void*, void*, uint32_t, uint64_t, float, uint32_t);
    alignas(16) uint8_t ref[32] = {};
    alignas(16) uint8_t handle[32] = {};
    void* bankRef = Native<ResolveRef>(GSOUND_RESOLVE_REF)(sound, ref, bank);
    Native<Play>(GSOUND_PLAY_ORDINARY_VOICE)(sound, handle, bankRef, id,
                                             static_cast<uint64_t>(team), 1.0f, 0);
}

// What a pick sounds like in the game (slotState246, 0x140364C50): the pick
// cue of that side, then the announcer with the character's name. The other
// player's picks arrive as state, so they are given the same here.
void PlayPickSounds(int team, int32_t character)
{
    PlayVoice(6, team == 0 ? 0x8D : 0x97, team); // kCuePick
    void* sound = Instance(GSOUND_GET_INSTANCE);
    if (!sound) return;
    using PlayName = void*(__fastcall*)(void*, void*, uint32_t);
    alignas(16) uint8_t handle[32] = {};
    Native<PlayName>(GSOUND_PLAY_CHARACTER_NAME)(sound, handle, static_cast<uint32_t>(character));
}

// The select screen's cues, bank 6, player 1's (player 2's are 10 higher):
// roster cursor moved (slotState246), character picked, assist list moved,
// assist decided, a step taken back (uMenuChrSelAssist__processAssistSelection,
// 0x14036B4B0), team complete.
enum SelectCue : uint32_t {
    kCueCursor = 0x8C,
    kCuePick = 0x8D,
    kCueAssistMove = 0x8E,
    kCueAssistDecide = 0x8F,
    kCueBack = 0x90,
    kCueComplete = 0x91,
};

void PlayTeamCompleteSounds(int team);

void PlayCue(int team, SelectCue cue)
{
    PlayVoice(6, cue + (team == 0 ? 0u : 10u), team);
}

// The sounds of one step on a side drawn from packets, from what it showed
// before to what it shows now: what the game plays when the same step is
// taken on its own pad.
void PlayRemoteStepSounds(int team, const CharSelPacket& before, const CharSelPacket& now)
{
    const uint8_t was = before.slotState, is = now.slotState;
    const bool locked = (now.flags & CHARSEL_LOCKED_IN) != 0;
    if (locked) {
        // A third assist completes the team; a preset is all at once.
        if (IsAssistState(was)) PlayCue(team, kCueAssistDecide);
        PlayTeamCompleteSounds(team);
        return;
    }
    if (is == was) {
        if (IsAssistState(is)) {
            if (now.assistHover != before.assistHover) PlayCue(team, kCueAssistMove);
        } else if (is == 2 || is == 4 || is == 6) {
            const int hovered = PickedCount(is);
            if (now.cursorCell != before.cursorCell) {
                PlayCue(team, kCueCursor);
            } else if (hovered < 3 && now.colors[hovered] != before.colors[hovered]) {
                // A color cycled on the character under the cursor: the same
                // cue for either player (updateCharacterVariantSelection,
                // 0x140362BA0 area: bank 1, voice 9).
                PlayVoice(1, 9, 0);
            }
        }
        return;
    }
    if (was > 7 || is > 7) return;
    if (is < was) {
        if (was >= 2) PlayCue(team, kCueBack);
        return;
    }
    const int picked = PickedCount(is);
    if (picked > PickedCount(was) && picked >= 1) PlayPickSounds(team, now.chars[picked - 1]);
    else if (IsAssistState(was)) PlayCue(team, kCueAssistDecide);
}

// And a completed team: its cue and the announcer's call for that side.
void PlayTeamCompleteSounds(int team)
{
    PlayVoice(6, team == 0 ? 0x91 : 0x9B, team);
    PlayVoice(6, team == 0 ? 0x54 : 0x86, team);
}

// Puts a whole team on the local side through the game's own reserve-unit
// path (uMenuChrSel__commitCharSelectionForSlot): all three records, the
// card, the archives, and the slot at "team complete".
void CommitLocalTeam(void* menu, const Team& team, uint8_t randomMask)
{
    void* gameUi = Instance(SGAMEUI_GET_INSTANCE);
    if (!gameUi || !IsValidTeam(team)) {
        HookLog("NativeCharSel: refused to put in a team that is not valid here\n");
        return;
    }
    const int local = g.config.localTeam;
    if (IsAssistState(LocalSlotState())) {
        void* widget = Field<void*>(menu, MENU_ASSIST + 8 * local);
        if (UnitAlive(widget)) {
            using Close = void(__fastcall*)(void*);
            Native<Close>(ASSIST_CLOSE)(widget);
        }
    }
    g.localRandomMask = randomMask & 7;
    g.randomMaskGiven = true;
    g.overrideTeam = team;
    g.commitOverride = true;
    using Commit = void(__fastcall*)(void*, uint32_t, int32_t);
    Native<Commit>(MENU_COMMIT_RESERVE)(menu, static_cast<uint32_t>(local), 0);
    g.commitOverride = false;
    Field<int32_t>(menu, MENU_RESERVE_ID + 4 * local) = -1; // it was not a reserve unit
    // The commit sets the menu's copy of the state; the tick takes it from
    // sGameUI.
    Field<int32_t>(gameUi, SGAMEUI_SLOT_STATE + 4 * local) =
        Field<int32_t>(menu, MENU_MODE + 4 * local);
    // What taking a reserve unit sounds like in the game: the decide cue
    // (slotState246, 0x140364C50), then the team-complete cue and the
    // announcer (uMenuChrSelReserveUnit__processSelectionInput, 0x14037AC80).
    PlayVoice(1, local == 0 ? 9 : 0x0F, local);
    PlayVoice(6, local == 0 ? 0x91 : 0x9B, local);
    PlayVoice(6, local == 0 ? 0x54 : 0x86, local);
    HookLog("NativeCharSel: local team put in (%d/%d/%d), slot state %d\n", team.chars[0],
            team.chars[1], team.chars[2], Field<int32_t>(menu, MENU_MODE + 4 * local));
}

void ReadLocalSide(void* menu)
{
    void* battleSetting = Instance(SBATTLESETTING_GET_INSTANCE);
    if (!battleSetting) return;
    const int local = g.config.localTeam;
    uint8_t state = LocalSlotState();
    // The back-out prompts and anything past "complete" are not states the
    // other side draws.
    if (state > kStateTeamComplete) state = kStateTeamComplete;
    else if (state > 7 && state < kStateTeamComplete) state = 2;

    CharSelPacket packet = {};
    packet.type = PACKET_CHARSEL;
    packet.slotState = state;
    const int picked = PickedCount(state);
    const int assists = AssistCount(state);
    for (int member = 0; member < 3; ++member) {
        void* record = SlotRecord(battleSetting, local, member);
        const uint8_t* shown = static_cast<uint8_t*>(menu) + MENU_RECORDS + (local * 3 + member) * 12;
        const int32_t shownColor = *reinterpret_cast<const int32_t*>(shown + 4);
        const int32_t shownAssist = *reinterpret_cast<const int32_t*>(shown + 8);
        packet.chars[member] = member < picked ? Field<int32_t>(record, SLOT_CHARACTER) : 0;
        packet.colors[member] = std::clamp(
            member < picked ? Field<int32_t>(record, SLOT_COLOR) : shownColor, 0,
            kChrColorSlots - 1);
        packet.assists[member] = std::clamp(
            member < assists ? Field<int32_t>(record, SLOT_ASSIST) : shownAssist, 0, 2);
    }
    if (void* background = Field<void*>(menu, MENU_BG_MAIN)) {
        const uint32_t cell = Field<uint32_t>(background, BGMAIN_CURSOR_POS + 4 * local);
        packet.cursorCell = static_cast<uint8_t>(cell < kGridCells ? cell : 0);
    }
    if (IsAssistState(state)) {
        void* widget = Field<void*>(menu, MENU_ASSIST + 8 * local);
        if (UnitAlive(widget))
            packet.assistHover = std::min<uint8_t>(Field<uint8_t>(widget, ASSIST_HIGHLIGHT), 2);
    }

    // RANDOM ALL picked on the roster: it covers the member being picked when
    // the flag went up and every one after it.
    if (!g.localLocked && state < kStateTeamComplete) {
        if (!Field<uint8_t>(menu, MENU_RAND_ALL + local)) g.randomFrom = -1;
        else if (g.randomFrom < 0) g.randomFrom = PickedCount(state);
    }
    if (state == kStateTeamComplete && !g.localLocked) {
        if (!g.randomMaskGiven)
            g.localRandomMask = g.randomFrom >= 0 && g.randomFrom < 3
                ? static_cast<uint8_t>((7u << g.randomFrom) & 7u) : 0;
        Team team = {};
        for (int member = 0; member < 3; ++member) {
            team.chars[member] = packet.chars[member];
            team.colors[member] = packet.colors[member];
            team.assists[member] = packet.assists[member];
        }
        // The screen only offers what is installed, so this is a formality:
        // a color this build does not know becomes the first one, and a team
        // that still fails is replaced by the team in use.
        for (int member = 0; member < 3; ++member) {
            if (!ChrColors_Available(team.chars[member], team.colors[member]))
                team.colors[member] = packet.colors[member] = 0;
        }
        if (!IsValidTeam(team)) {
            HookLog("NativeCharSel: the screen completed a team that is not valid (%d/%d/%d); "
                    "the team in use is played\n", team.chars[0], team.chars[1], team.chars[2]);
            team = g.config.team;
            g.localRandomMask = g.config.teamRandomMask & 7;
            for (int member = 0; member < 3; ++member) {
                packet.chars[member] = team.chars[member];
                packet.colors[member] = team.colors[member];
                packet.assists[member] = team.assists[member];
            }
        }
        g.localTeam = team;
        g.localLocked = true;
        HookLog("NativeCharSel: local team locked in: %d/%d/%d colors %d/%d/%d assists %d/%d/%d\n",
                team.chars[0], team.chars[1], team.chars[2], team.colors[0], team.colors[1],
                team.colors[2], team.assists[0], team.assists[1], team.assists[2]);
    }
    if (g.localLocked) {
        // From here on the other player is told the team that was locked in,
        // whatever the screen holds.
        packet.slotState = kStateTeamComplete;
        packet.flags |= CHARSEL_LOCKED_IN;
        packet.randomMask = g.localRandomMask;
        for (int member = 0; member < 3; ++member) {
            packet.chars[member] = g.localTeam.chars[member];
            packet.colors[member] = g.localTeam.colors[member];
            packet.assists[member] = g.localTeam.assists[member];
        }
    }
    if (g.presetListOpen) packet.flags |= CHARSEL_PRESET_LIST;
    packet.coin = g.coin[local];
    if (StagePhase() && LocalPicksStage()) {
        void* page = Field<void*>(menu, MENU_STAGE_PAGE);
        if (UnitAlive(page))
            g.localStageCursor = static_cast<uint8_t>(
                std::clamp(Field<int32_t>(page, OPTION_STAGE_CURSOR), 0, kStagePositions - 1));
        packet.stageCursor = g.localStageCursor;
        if (g.stageDecided) {
            packet.flags |= CHARSEL_STAGE_DECIDED;
            packet.stageId = g.stageId;
        }
    }
    g.local = packet;
    g.localValid = true;
}

void BeforeTick(void* menu)
{
    // Both player indices read the local pad, so player 2's side of the
    // screen takes the same device. The remote team's handlers are hooked off
    // below, which is what keeps that pad from driving both sides.
    if (uint64_t manager = *reinterpret_cast<uint64_t*>(_addr(SMVC3MANAGER_INSTANCE_PTR));
        manager && g.saved && !Watching()) {
        int32_t* pads = reinterpret_cast<int32_t*>(manager + MANAGER_PAD_ASSIGN);
        pads[1] = pads[0];
    }

    // The game's step-through reserve picker gives way to the preset list.
    for (int team = 0; team < 2; ++team) {
        void*& unit = Field<void*>(menu, MENU_RESERVE_UNIT + 8 * team);
        if (!unit) continue;
        if (UnitAlive(unit)) {
            using Kill = void(__fastcall*)(void*);
            (*reinterpret_cast<Kill*>(*reinterpret_cast<uint8_t**>(unit) + CUNIT_VT_KILL))(unit);
        }
        unit = nullptr;
    }

    for (int team = 0; team < 2; ++team)
        if (IsRemoteTeam(team)) ApplyRemote(menu, team);
    if (StagePhase()) DriveStagePhase(menu);

    const ULONGLONG now = GetTickCount64();
    if (void* background = Field<void*>(menu, MENU_BG_MAIN)) {
        const ULONGLONG deadline = g.stageDeadlineTick ? g.stageDeadlineTick : g.deadlineTick;
        const float framesLeft = now < deadline
            ? static_cast<float>(deadline - now) * 0.06f : 0.0f;
        Field<uint8_t>(background, BGMAIN_TIME_INFINITE) = 0;
        Field<float>(background, BGMAIN_TIME_LIMIT) = std::max(framesLeft, kMinClockFrames);
    }

    if (Watching()) return;
    // The local team is put in whole when the preset list hands one over,
    // and when the clock runs out on a player who is still picking.
    const uint8_t state = LocalSlotState();
    if (!g.localLocked && state >= 2 && state < kStateTeamComplete) {
        if (g.teamPending) {
            g.teamPending = false;
            CommitLocalTeam(menu, g.pendingTeam, g.pendingRandomMask);
        } else if (now >= g.deadlineTick) {
            HookLog("NativeCharSel: out of time; the team in use goes in\n");
            g.presetListOpen = false;
            CommitLocalTeam(menu, g.config.team, g.config.teamRandomMask);
        }
    }
}

// ---------------------------------------------------------------------------
// Hooks
// ---------------------------------------------------------------------------
using MenuTick = uint64_t(__fastcall*)(void*, uint64_t, uint64_t, uint64_t);
using MenuState2 = uint64_t(__fastcall*)(void*, uint32_t);
using MenuState246 = uint64_t(__fastcall*)(void*, uint32_t, uint32_t, uint64_t, void*, void*);
using MenuState357 = uint64_t(__fastcall*)(void*, uint32_t, int32_t, uint64_t);
using AssistCapture = uint64_t(__fastcall*)(void*, uint32_t, uint32_t*);
using RequestEnter = uint64_t(__fastcall*)(void*);
using CopyProfile = uint64_t(__fastcall*)(void*, void*);

MenuTick g_originalTick = nullptr;
MenuState2 g_originalState0 = nullptr, g_originalState1 = nullptr, g_originalState11 = nullptr;
MenuState246 g_originalState246 = nullptr;
MenuState357 g_originalState357 = nullptr;
AssistCapture g_originalAssistCapture = nullptr;
RequestEnter g_originalRequestEnter = nullptr;
CopyProfile g_originalCopyProfile = nullptr;

// A team whose state handler must not run this tick: the remote team always
// (it is drawn from packets), the local one while the preset list covers it.
bool TeamHeld(uint32_t team)
{
    if (!g.active) return false;
    if (team > 1 || IsRemoteTeam(static_cast<int>(team))) return true;
    return g.presetListOpen || g.teamPending;
}

uint64_t __fastcall Hook_Tick(void* menu, uint64_t a, uint64_t b, uint64_t c)
{
    if (Frozen()) return 1; // "still selecting": nothing moves
    if (!g.active) return g_originalTick(menu, a, b, c);
    g.menu = menu;
    g.menuFrame = g.frame;
    if (g.status == Status::Opening) {
        g.status = Status::Selecting;
        g.deadlineTick = GetTickCount64() + static_cast<ULONGLONG>(g.config.seconds) * 1000;
        HookLog("NativeCharSel: the screen is up (%s %d)\n",
                Watching() ? "watching, team" : "local team", g.config.localTeam);
    }
    BeforeTick(menu);
    const uint64_t result = g_originalTick(menu, a, b, c);
    if (!Watching()) ReadLocalSide(menu);
    // Zero in AL moves the screen on to its versus sequence: only with the
    // stage decided.
    if (g.stageDecided) return result;
    return (result & ~static_cast<uint64_t>(0xFF)) | 1;
}

uint64_t __fastcall Hook_State0(void* menu, uint32_t team)
{
    return TeamHeld(team) ? 1 : g_originalState0(menu, team);
}

uint64_t __fastcall Hook_State1(void* menu, uint32_t team)
{
    return TeamHeld(team) ? 1 : g_originalState1(menu, team);
}

uint64_t __fastcall Hook_State246(void* menu, uint32_t team, uint32_t member, uint64_t state,
                                  void* record, void* field)
{
    return TeamHeld(team) ? 1 : g_originalState246(menu, team, member, state, record, field);
}

uint64_t __fastcall Hook_State357(void* menu, uint32_t team, int32_t member, uint64_t r9)
{
    return TeamHeld(team) ? 1 : g_originalState357(menu, team, member, r9);
}

// "Team complete" natively loads the battle's archives and, for the team
// that picks the stage, opens the stage page. It runs for that team alone, on
// both clients, and only once both teams are final (DriveStagePhase).
uint64_t __fastcall Hook_State11(void* menu, uint32_t team)
{
    if (!g.active || (StagePhase() && static_cast<int>(team) == StageTeam()))
        return g_originalState11(menu, team);
    return 1;
}

// The stage page. On the picking player's client it is the game's own, less
// the two rows of match options. On the other's it shows what the picker
// does: its input is cut, the cursor comes from the packets, and it closes
// when the picker has decided. (The page reads player index 0, which is the
// local pad on either client.)
using OptionCall = void(__fastcall*)(void*);
using OptionMask = uint64_t(__fastcall*)(void*, uint64_t);
OptionCall g_originalOptionUpdate = nullptr, g_originalOptionSample = nullptr,
           g_originalOptionConfirm = nullptr;
OptionMask g_originalOptionMask = nullptr;

bool StagePageWatched() { return g.active && !LocalPicksStage(); }

void __fastcall Hook_OptionUpdate(void* page)
{
    if (g.active) {
        // The option rows are reached with up / down (cut in Hook_OptionMask)
        // or the mouse; either way the page goes back to its stage row.
        if (Field<uint8_t>(page, OPTION_STATE) == 6) {
            Field<uint8_t>(page, OPTION_STATE) = 4;
            Field<uint8_t>(page, OPTION_ROW) = 0;
        }
        if (StagePageWatched() && Field<uint8_t>(page, OPTION_STATE) == 4) {
            int32_t& cursor = Field<int32_t>(page, OPTION_STAGE_CURSOR);
            const int32_t wanted = g.remoteStageCursor;
            if (cursor != wanted) {
                // What the picker's own page plays for a step
                // (uMenuChrSelOption__updateStageSelection).
                PlayVoice(6, 0x8C, 0);
                // The shorter way round decides which way the picture slides.
                const int32_t back = (cursor - wanted + kStagePositions) % kStagePositions;
                Field<uint8_t>(page, OPTION_PAGE_LEFT) = back <= kStagePositions / 2;
                cursor = wanted;
                Field<int32_t>(page, OPTION_PAGE_COUNT) = 8;
            }
            if (g.remoteStageDecided && !Field<uint8_t>(page, OPTION_DECIDED)) {
                Field<uint8_t>(page, OPTION_DECIDED) = 1;
                PlayVoice(6, 0x8D, 0); // the confirm (aChrSelect__confirmStageSelectInput)
            }
        }
    }
    g_originalOptionUpdate(page);
}

void __fastcall Hook_OptionSample(void* page)
{
    if (!StagePageWatched() && !Frozen()) {
        g_originalOptionSample(page);
        return;
    }
    Field<uint32_t>(page, OPTION_DIRECTION) = 0;
    Field<uint8_t>(page, OPTION_INPUT_VALID) = 1;
}

void __fastcall Hook_OptionConfirm(void* page)
{
    if (!StagePageWatched() && !Frozen()) {
        g_originalOptionConfirm(page);
        return;
    }
    // What the game does once its own confirm has been pressed, with the
    // picker's stage in place of the one under the cursor.
    if (!g.active || !g.remoteStageDecided) return;
    const int32_t cursor = Field<int32_t>(page, OPTION_STAGE_CURSOR);
    if (cursor != g.remoteStageCursor || Field<int32_t>(page, OPTION_PAGE_COUNT) >= 0 ||
        cursor != Field<int32_t>(page, OPTION_DRAW_STAGE))
        return; // the picture is still catching up
    void* background = Field<void*>(page, OPTION_BG_MAIN);
    if (background) {
        using DelayElapsed = bool(__fastcall*)(void*);
        if (!Native<DelayElapsed>(BGMAIN_DELAY_ELAPSED)(background)) return;
    }
    void* gameUi = Instance(SGAMEUI_GET_INSTANCE);
    if (!gameUi) return;
    Field<int32_t>(gameUi, SGAMEUI_SELECTED_STAGE) = g.remoteStageId;
    if (background && cursor >= 0 && cursor < kStagePositions) {
        using SetStage = void(__fastcall*)(void*, int32_t);
        Native<SetStage>(BGMAIN_SET_STAGE)(
            background, reinterpret_cast<const int32_t*>(_addr(STAGE_PAGE_TABLE))[cursor]);
    }
    Field<uint8_t>(page, OPTION_STATE) = 0x0C;
}

uint64_t __fastcall Hook_OptionMask(void* page, uint64_t mask)
{
    if (g.active && (mask & 0xC)) return 0; // up / down: no option rows
    return g_originalOptionMask(page, mask);
}

uint64_t __fastcall Hook_AssistCapture(void* widget, uint32_t team, uint32_t* out)
{
    if (Frozen() || TeamHeld(team)) {
        // What the game's own "this side is not yours" exit leaves.
        out[0] = 0xFFFFFFFFu;
        out[1] = 0;
        out[2] = 0;
        *reinterpret_cast<uint16_t*>(out + 3) = 0;
        return 0;
    }
    return g_originalAssistCapture(widget, team, out);
}

uint64_t __fastcall Hook_RequestEnter(void* battleSetting)
{
    // The versus sequence is over and the game wants the battle: the caller
    // boots it. The screen asks again every frame until its area goes.
    if (g.active || Frozen()) {
        if (g.active && !g.enterRequested) {
            g.enterRequested = true;
            HookLog("NativeCharSel: versus sequence finished; the battle can be booted\n");
        }
        return 0;
    }
    return g_originalRequestEnter(battleSetting);
}

uint64_t __fastcall Hook_CopyProfile(void* license, void* out)
{
    const uint64_t result = g_originalCopyProfile(license, out);
    if (g.commitOverride &&
        reinterpret_cast<uintptr_t>(__builtin_return_address(0)) ==
            static_cast<uintptr_t>(_addr(COMMIT_RESERVE_COPY_RETURN))) {
        // Reserve unit 0 of the caller's copy only; the save is not touched.
        for (int member = 0; member < 3; ++member) {
            Field<int32_t>(out, PROFILE_RESERVE_CHARS + 4 * member) = g.overrideTeam.chars[member];
            Field<int32_t>(out, PROFILE_RESERVE_ASSISTS + 4 * member) = g.overrideTeam.assists[member];
            Field<int32_t>(out, PROFILE_RESERVE_COLORS + 4 * member) = g.overrideTeam.colors[member];
        }
    }
    return result;
}

template <typename Fn> bool Hook(uintptr_t address, Fn detour, Fn* original, const char* name)
{
    void* target = reinterpret_cast<void*>(_addr(address));
    MH_STATUS status = MH_CreateHook(target, reinterpret_cast<void*>(detour),
                                     reinterpret_cast<void**>(original));
    if (status == MH_OK) status = MH_EnableHook(target);
    if (status == MH_OK) return true;
    HookLog("NativeCharSel: hook on %s failed (%d)\n", name, static_cast<int>(status));
    return false;
}

void RestoreSettings()
{
    if (!g.saved) return;
    g.saved = false;
    if (void* config = Instance(SGAMECONFIG_GET_INSTANCE)) {
        Field<uint8_t>(config, SGAMECONFIG_CTRL_TYPE_SET) = g.savedCtrlSet;
        Field<uint8_t>(config, SGAMECONFIG_CTRL_TYPE_FIXED) = g.savedCtrlFixed;
    }
    if (uint64_t manager = *reinterpret_cast<uint64_t*>(_addr(SMVC3MANAGER_INSTANCE_PTR)))
        reinterpret_cast<int32_t*>(manager + MANAGER_PAD_ASSIGN)[1] = g.savedPad1;
}

// The stage page writes the player's saved versus options into the match
// when it opens, on each client from its own save. They go back to what they
// were, so the two players cannot end up with different rules.
void RestoreMatchOptions(bool timeLimit)
{
    // Not the time limit once the battle boot has set its own.
    if (void* battleSetting = Instance(SBATTLESETTING_GET_INSTANCE); battleSetting && timeLimit)
        Field<int32_t>(battleSetting, BS_TIME_LIMIT) = g.savedTimeLimit;
    if (void* action = Instance(SACTION_GET_INSTANCE))
        Field<float>(action, SACTION_DAMAGE_RATE) = g.savedDamageRate;
}

} // namespace

bool IsSelectableCharacter(int32_t characterId)
{
    if (characterId < 1 || characterId >= kSelectableTableSize) return false;
    // 52..55 are the grid's "?", RANDOM and RANDOM ALL cells, not characters.
    if (characterId >= 0x34 && characterId <= 0x37) return false;
    if (!reinterpret_cast<const uint8_t*>(_addr(CHARACTER_SELECTABLE_TABLE))[characterId])
        return false;
    return ChrColors_Count(characterId) > 0;
}

bool Install()
{
    if (g_installed) return true;
    const MH_STATUS init = MH_Initialize();
    if (init != MH_OK && init != MH_ERROR_ALREADY_INITIALIZED) {
        HookLog("NativeCharSel: MinHook is not available (%d)\n", static_cast<int>(init));
        return false;
    }
    bool ok = true;
    ok &= Hook(MENU_TICK, &Hook_Tick, &g_originalTick, "the slot state machine");
    ok &= Hook(MENU_STATE_0, &Hook_State0, &g_originalState0, "slot state 0");
    ok &= Hook(MENU_STATE_1, &Hook_State1, &g_originalState1, "slot state 1");
    ok &= Hook(MENU_STATE_246, &Hook_State246, &g_originalState246, "character pick");
    ok &= Hook(MENU_STATE_357, &Hook_State357, &g_originalState357, "assist pick");
    ok &= Hook(MENU_STATE_11, &Hook_State11, &g_originalState11, "team complete");
    ok &= Hook(ASSIST_CAPTURE_INPUT, &Hook_AssistCapture, &g_originalAssistCapture,
               "assist list input");
    ok &= Hook(SBATTLESETTING_REQUEST_ENTER, &Hook_RequestEnter, &g_originalRequestEnter,
               "battle entry");
    ok &= Hook(SLICENSE_COPY_PROFILE, &Hook_CopyProfile, &g_originalCopyProfile,
               "license profile copy");
    ok &= Hook(OPTION_UPDATE_STATE, &Hook_OptionUpdate, &g_originalOptionUpdate, "stage page");
    ok &= Hook(OPTION_SAMPLE_INPUT, &Hook_OptionSample, &g_originalOptionSample,
               "stage page directions");
    ok &= Hook(OPTION_CONFIRM_INPUT, &Hook_OptionConfirm, &g_originalOptionConfirm,
               "stage page confirm");
    ok &= Hook(OPTION_INPUT_MASK, &Hook_OptionMask, &g_originalOptionMask, "stage page rows");
    g_installed = ok;
    HookLog("NativeCharSel: hooks %s\n", ok ? "installed" : "incomplete; the screen is not offered");
    return ok;
}

bool Begin(const Config& config)
{
    if (!g_installed || g.active) return false;
    if (config.localTeam < 0 || config.localTeam > 1 ||
        (!config.watch && !IsValidTeam(config.team))) {
        HookLog("NativeCharSel: cannot open: the team in use is not valid here\n");
        return false;
    }
    const uint64_t area = *reinterpret_cast<uint64_t*>(_addr(SAREA_INSTANCE_PTR));
    void* gameUi = Instance(SGAMEUI_GET_INSTANCE);
    void* battleSetting = Instance(SBATTLESETTING_GET_INSTANCE);
    void* gameConfig = Instance(SGAMECONFIG_GET_INSTANCE);
    if (!area || !gameUi || !battleSetting || !gameConfig) return false;

    g = Session{};
    g.active = true;
    g.config = config;
    g.status = Status::Opening;
    g.beginTick = GetTickCount64();
    g.deadlineTick = g.beginTick + kOpenTimeoutMs + static_cast<ULONGLONG>(config.seconds) * 1000;

    g.savedCtrlSet = Field<uint8_t>(gameConfig, SGAMECONFIG_CTRL_TYPE_SET);
    g.savedCtrlFixed = Field<uint8_t>(gameConfig, SGAMECONFIG_CTRL_TYPE_FIXED);
    if (uint64_t manager = *reinterpret_cast<uint64_t*>(_addr(SMVC3MANAGER_INSTANCE_PTR)))
        g.savedPad1 = reinterpret_cast<int32_t*>(manager + MANAGER_PAD_ASSIGN)[1];
    g.savedTimeLimit = Field<int32_t>(battleSetting, BS_TIME_LIMIT);
    if (void* action = Instance(SACTION_GET_INSTANCE))
        g.savedDamageRate = Field<float>(action, SACTION_DAMAGE_RATE);
    g.saved = true;
    LARGE_INTEGER counter = {};
    QueryPerformanceCounter(&counter);
    if (!config.watch)
        g.coin[config.localTeam] =
            static_cast<uint8_t>(((counter.QuadPart >> 4) ^ (counter.QuadPart >> 11)) & 1);
    Field<uint8_t>(gameConfig, SGAMECONFIG_CTRL_TYPE_SET) = 1;
    Field<uint8_t>(gameConfig, SGAMECONFIG_CTRL_TYPE_FIXED) = 1;

    // What the game's own Versus entry does from the title area: battle type
    // 0, then the area. The select state is the one its offline modes use.
    using SetInt = void(__fastcall*)(void*, int32_t);
    Native<SetInt>(SBATTLESETTING_SET_BATTLE_TYPE)(battleSetting, 0);
    Native<SetInt>(SGAMEUI_SET_CSS_STATE)(gameUi, 0);
    using RequestTransition = void(__fastcall*)(uint64_t, void*);
    const RequestTransition request = *reinterpret_cast<RequestTransition*>(
        *reinterpret_cast<uint64_t*>(area) + SAREA_VT_REQUEST_TRANSITION);
    request(area, reinterpret_cast<void*>(_addr(ACHRSELECT_DTI)));
    HookLog("NativeCharSel: opening %s %d, %u s to pick\n",
            config.watch ? "to watch, team" : "as team", config.localTeam, config.seconds);
    return true;
}

void End(bool startingBattle)
{
    if (!g.active) return;
    RestoreSettings();
    RestoreMatchOptions(!startingBattle);
    if (startingBattle) {
        if (void* gameUi = Instance(SGAMEUI_GET_INSTANCE)) {
            using SetInt = void(__fastcall*)(void*, int32_t);
            Native<SetInt>(SGAMEUI_SET_BATTLE_TRANSITION)(gameUi, 0);
        }
    }
    g.active = false;
    g.status = Status::Idle;
    // The screen outlives the session by the frames its area takes to go.
    g.freezeUntilFrame = g.frame + kFreezeFrames;
    HookLog("NativeCharSel: closed (%s)\n", startingBattle ? "battle follows" : "back to the room");
}

void Tick()
{
    ++g.frame;
    if (!g.active) return;
    const ULONGLONG now = GetTickCount64();
    if (g.status == Status::Opening && now - g.beginTick > kOpenTimeoutMs) {
        HookLog("NativeCharSel: the screen did not come up\n");
        g.status = Status::Failed;
    }
    if (g.status != Status::Selecting) return;
    // "Return to the main menu?" answered yes: the game has asked for the
    // title area itself.
    const uint64_t area = *reinterpret_cast<uint64_t*>(_addr(SAREA_INSTANCE_PTR));
    if (area && *reinterpret_cast<uint64_t*>(area + SAREA_PENDING_DTI) ==
                    static_cast<uint64_t>(_addr(ATITLE_DTI))) {
        HookLog("NativeCharSel: the local player backed out\n");
        g.status = Status::LocalLeft;
    } else if (!g.stageDecided && g.frame - g.menuFrame > 600) {
        // (With the stage decided the slot machine is done and no longer runs.)
        HookLog("NativeCharSel: the screen went away on its own\n");
        g.status = Status::Failed;
    }
}

void ReleaseSettings()
{
    if (!g.active) return;
    RestoreSettings();
    RestoreMatchOptions(true);
}

bool Active() { return g.active; }
Status GetStatus() { return g.status; }
bool LocalLockedIn() { return g.active && !Watching() && g.localLocked; }
bool RemoteLockedIn() { return g.active && !Watching() && g.remoteLocked[1 - g.config.localTeam]; }
Team LocalTeam() { return g.localTeam; }
Team RemoteTeam() { return g.remoteTeam[1 - g.config.localTeam]; }
uint8_t LocalRandomMask() { return g.localLocked ? g.localRandomMask : 0; }
uint8_t RemoteRandomMask()
{
    const int team = 1 - g.config.localTeam;
    return g.remoteLocked[team] ? g.remoteRandomMask[team] : 0;
}
bool AllLockedIn() { return g.active && TeamLocked(0) && TeamLocked(1); }

uint32_t SecondsLeft()
{
    const ULONGLONG now = GetTickCount64();
    return g.active && now < g.deadlineTick
        ? static_cast<uint32_t>((g.deadlineTick - now + 999) / 1000) : 0;
}

bool StageDecided() { return g.active && g.stageDecided; }
int StagePicker()
{
    return g.active && StagePhase() && g.stageDeadlineTick && !g.stageDecided ? StageTeam() : -1;
}
int32_t StageId() { return g.stageId; }
bool EnterRequested() { return g.active && g.enterRequested; }

bool TakeOutbound(CharSelPacket* packet)
{
    if (!g.active || Watching() || g.status != Status::Selecting || !g.localValid) return false;
    const ULONGLONG now = GetTickCount64();
    CharSelPacket current = g.local;
    current.sequence = g.lastSent.sequence;
    const bool changed = memcmp(&current, &g.lastSent, sizeof(current)) != 0;
    if (!changed && g.lastSentTick && now - g.lastSentTick < kRepeatMs) return false;
    current.sequence = ++g.sequence;
    g.lastSent = current;
    g.lastSentTick = now;
    *packet = current;
    return true;
}

void OnPeerPacket(const CharSelPacket& packet)
{
    if (g.active && !Watching()) OnTeamPacket(1 - g.config.localTeam, packet);
}

// The character a roster cell stands for, 0 for none (the "?" and random
// cells included).
int32_t CharacterAtCell(uint32_t cell)
{
    using FindGridIndex = uint64_t(__fastcall*)(int32_t);
    for (int32_t id = 1; id < kSelectableTableSize; ++id)
        if (IsSelectableCharacter(id) &&
            static_cast<uint32_t>(Native<FindGridIndex>(MENU_FIND_GRID_INDEX)(id)) == cell)
            return id;
    return 0;
}

void OnTeamPacket(int team, const CharSelPacket& received)
{
    if (!g.active || team < 0 || team > 1 || !IsRemoteTeam(team)) return;
    CharSelPacket packet = received;
    // The color shown for a member that is still being picked is drawn on
    // the card of whatever the cursor is on: it must be one that character
    // has here, which the range check alone does not say.
    if (const int hovered = PickedCount(packet.slotState); hovered < 3) {
        const int32_t character = CharacterAtCell(packet.cursorCell);
        if (!character || !ChrColors_Available(character, packet.colors[hovered]))
            packet.colors[hovered] = 0;
    }
    if (packet.type != PACKET_CHARSEL || !IsValidPeerPacket(packet)) {
        if (!g.badPacketLogged) {
            g.badPacketLogged = true;
            HookLog("NativeCharSel: dropped a select-screen packet that failed its checks "
                    "(state %u cell %u flags %u chars %d/%d/%d)\n", packet.slotState,
                    packet.cursorCell, packet.flags, packet.chars[0], packet.chars[1],
                    packet.chars[2]);
        }
        return;
    }
    if (packet.flags & CHARSEL_LEFT) {
        if (g.status == Status::Opening || g.status == Status::Selecting) {
            HookLog("NativeCharSel: the other player backed out\n");
            g.status = Status::RemoteLeft;
        }
        return;
    }
    // A team that was locked in stays as it was. From the player who picks
    // the stage, the stage select then follows.
    if (g.remoteLocked[team]) {
        if (TeamLocked(0) && TeamLocked(1) && team == StageTeam() &&
            (packet.flags & CHARSEL_LOCKED_IN) && !g.remoteStageDecided) {
            g.remoteStageCursor = packet.stageCursor;
            if (packet.flags & CHARSEL_STAGE_DECIDED) {
                g.remoteStageId = packet.stageId;
                g.remoteStageDecided = true;
                HookLog("NativeCharSel: the other player picked stage %d\n", packet.stageId);
            }
        }
        return;
    }
    // The sounds of what just happened on that side. Not for the first
    // packet: that is a screen joined part-way, not a step.
    // (One step's worth a tick for each side, however fast packets come.)
    if (g.remoteValid[team] && g.status == Status::Selecting &&
        g.remoteSoundFrame[team] != g.frame) {
        g.remoteSoundFrame[team] = g.frame;
        PlayRemoteStepSounds(team, g.remote[team], packet);
    }
    g.remote[team] = packet;
    g.remoteValid[team] = true;
    if (packet.flags & CHARSEL_LOCKED_IN) {
        for (int member = 0; member < 3; ++member) {
            g.remoteTeam[team].chars[member] = packet.chars[member];
            g.remoteTeam[team].colors[member] = packet.colors[member];
            g.remoteTeam[team].assists[member] = packet.assists[member];
        }
        g.remoteLocked[team] = true;
        g.remoteRandomMask[team] = packet.randomMask & 7;
        g.coin[team] = packet.coin; // fixed here; later ones are not looked at
        HookLog("NativeCharSel: team %d locked in from its owner: %d/%d/%d\n", team,
                packet.chars[0], packet.chars[1], packet.chars[2]);
    }
}

bool CanPickPreset()
{
    if (!g.active || Watching() || g.status != Status::Selecting || g.localLocked ||
        g.teamPending || g.frame - g.menuFrame > 2)
        return false;
    const uint8_t state = LocalSlotState();
    return state == 2 || state == 4 || state == 6;
}

void SetPresetListOpen(bool open)
{
    if (g.active) g.presetListOpen = open;
}

bool ApplyLocalTeam(const Team& team, uint8_t randomMask)
{
    if (!g.active || Watching() || g.status != Status::Selecting || g.localLocked ||
        !IsValidTeam(team))
        return false;
    g.pendingTeam = team;
    g.pendingRandomMask = randomMask & 7;
    g.teamPending = true;
    return true;
}

} // namespace charsel
