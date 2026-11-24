// The game's result screen menu for a room match: see native_result.h and the
// result screen sections of doc/native_character_select.md.
#include "../pch.h"
#include "native_result.h"

#include "mk10utils.h"
#include "../kiero/minhook/include/MinHook.h"

#include <Windows.h>
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>

extern void HookLog(const char* fmt, ...);
// The intro / outro skip's press, from confirmed frames of the input timeline
// (controller.cpp): the same on both players' machines and a spectator's.
extern "C" bool Controller_TimelineSkipPressed();
extern "C" void Controller_CaptureSkipInput();

namespace resultmenu {
namespace {

// ---------------------------------------------------------------------------
// Native addresses
// ---------------------------------------------------------------------------
// uBtResult: the result screen. A phase machine; each way out of the screen
// is a small function of its own that ends by killing the unit.
constexpr uintptr_t RESULT_MOVE = 0x1402FC790;          // (this, ...)
constexpr uintptr_t RESULT_PHASE_REMATCH = 0x1402FAA20; // phase 8
constexpr uintptr_t RESULT_PHASE_TITLE_9 = 0x1402FAD00; // phase 9, title with boot state 1
constexpr uintptr_t RESULT_PHASE_CHARSEL = 0x1402FA9D0; // phase 10
constexpr uintptr_t RESULT_PHASE_TITLE_D = 0x1402FB070; // phase 0xD, versus "main menu"
constexpr uintptr_t RESULT_PHASE_TITLE_10 = 0x1402FAC30; // phase 0x10, title with boot state 2

// uBtResultScoreVersus: the score board and the menu under it.
constexpr uintptr_t SCORE_TICK = 0x14030A770; // vtable slot 29: the running state's update
constexpr size_t SCORE_SCHEDULER = 0x58;      // uMenuSubBase::mpUiScheduler
constexpr size_t SCORE_POPUP_OWNER = 0x0A;    // mRno[2]: player + 1 while "really leave?" is up
constexpr size_t SCORE_CHOICE = 0x940;        // int32_t[2]: 0, or 1 / 2 / 4
constexpr size_t SCORE_SELECT_POS = 0x948;    // int32_t[2]: 0..2
// The "return to the main menu?" pop-up the board opens for MAIN MENU: a
// uUiChoiceWindow, which reads the pad of the player index it was opened for.
constexpr uintptr_t CHOICE_WINDOW_INPUT = 0x140322630; // uUiChoiceWindow__processChoiceInput
constexpr size_t CHOICE_WINDOW_PAD_NO = 0x11C;
// The menu takes input once the board's scheduler has passed this frame.
constexpr uint32_t kMenuInputFrame = 0x78;
constexpr uintptr_t SCHEDULER_GET_FRAME = 0x1401F8A70; // (scheduler) -> frame

// The three reads the result screen makes of a player's pad. Each looks the
// pad up from the player index (sMvc3Manager's table).
constexpr uintptr_t RESULT_PAD_RAW = 0x1402FE180;    // (this, player): decide / skip
constexpr uintptr_t RESULT_PAD_ANALOG = 0x1402FE130; // (this, player): up / down
constexpr uintptr_t RESULT_PAD_INPUT = 0x1402FE0E0;  // (this, player): repeat

constexpr uintptr_t SPAD_GET_INSTANCE = 0x140001B00;
constexpr uintptr_t SPAD_GET_DECIDE_MASK = 0x140001A70; // (sPad) -> mask
constexpr uintptr_t SMVC3MANAGER_INSTANCE_PTR = 0x140D510A0;
constexpr size_t MANAGER_PAD_ASSIGN = 0x140; // int32_t[4], pad slot per player index
constexpr int kPadSlots = 4;

constexpr size_t CUNIT_FLAGS = 0x10;
constexpr size_t CUNIT_VT_KILL = 0x78;

constexpr ULONGLONG kRepeatMs = 250;

template <typename T> T& Field(void* object, size_t offset)
{
    return *reinterpret_cast<T*>(static_cast<uint8_t*>(object) + offset);
}

template <typename Fn> Fn Native(uintptr_t address)
{
    return reinterpret_cast<Fn>(_addr(address));
}

bool UnitAlive(void* unit)
{
    if (!unit) return false;
    const uint32_t state = Field<uint32_t>(unit, CUNIT_FLAGS) & 7u;
    return state == 1 || state == 2;
}

void KillUnit(void* unit)
{
    using Kill = void(__fastcall*)(void*);
    (*reinterpret_cast<Kill*>(*reinterpret_cast<uint8_t**>(unit) + CUNIT_VT_KILL))(unit);
}

bool IsChoice(uint8_t value)
{
    return value == kRematch || value == kCharSelect || value == kMainMenu;
}

// ---------------------------------------------------------------------------
// Session
// ---------------------------------------------------------------------------
struct Session {
    bool active = false;
    bool watching = false;
    int localTeam = 0;
    uint32_t generation = 0;
    uint32_t frame = 0;

    void* result = nullptr; // the uBtResult that is the screen
    uint32_t resultFrame = 0;
    bool duplicateLogged = false;
    void* score = nullptr;  // its board, while that takes input
    uint32_t scoreFrame = 0;
    bool menuSeen = false;
    bool inScoreTick = false;
    bool menuOpen = false;  // the board has reached its menu: input is the players' own

    uint8_t localCursor = 0;
    uint8_t localChoice = kNone;

    uint8_t remoteCursor = 0;
    uint8_t remoteChoice = kNone;
    bool badPacketLogged = false;

    ResultMenuPacket lastSent = {};
    ULONGLONG lastSentTick = 0;
};

Session g;
bool g_installed = false;

using PadRead = uint64_t(__fastcall*)(void*, uint32_t);
using ScoreTick = void(__fastcall*)(void*, uint64_t, uint64_t, uint64_t);
using ResultMove = void(__fastcall*)(void*, uint64_t, uint64_t, uint64_t);
using ResultPhase = void(__fastcall*)(void*);

PadRead g_originalRaw = nullptr, g_originalAnalog = nullptr, g_originalInput = nullptr;
ScoreTick g_originalScoreTick = nullptr;
ResultMove g_originalResultMove = nullptr;
ResultPhase g_originalRematch = nullptr, g_originalTitle9 = nullptr, g_originalCharSel = nullptr,
            g_originalTitleD = nullptr, g_originalTitle10 = nullptr;

uint32_t DecideMask()
{
    using GetInstance = void*(__fastcall*)();
    using GetMask = uint32_t(__fastcall*)(void*);
    void* pad = Native<GetInstance>(SPAD_GET_INSTANCE)();
    return pad ? Native<GetMask>(SPAD_GET_DECIDE_MASK)(pad) : 0;
}

// A read of the local player's pad, whichever player index they are. In a
// match the pad table maps index 1 to the second hardware slot, where a
// netplay client has nothing; as in the battle itself, the local player is
// whoever is at this machine, on any slot.
uint64_t LocalRead(PadRead original, void* self, uint32_t player)
{
    const uint64_t manager = *reinterpret_cast<uint64_t*>(_addr(SMVC3MANAGER_INSTANCE_PTR));
    if (!manager || player >= kPadSlots) return original(self, player);
    int32_t* pads = reinterpret_cast<int32_t*>(manager + MANAGER_PAD_ASSIGN);
    const int32_t saved = pads[player];
    uint64_t value = 0;
    for (int slot = 0; slot < kPadSlots; ++slot) {
        pads[player] = slot;
        value |= original(self, player);
    }
    pads[player] = saved;
    return value;
}

uint64_t ReadFor(PadRead original, void* self, uint32_t player, bool decide)
{
    if (!g.active) return original(self, player);
    // Everything before the menu (the winner's quote, the score board's
    // count-up) is skipped by a confirm from either player. Those skips come
    // from the input timeline, so both screens take them on the same frame;
    // the menu itself is each player's own, with no delay.
    if (!g.menuOpen) {
        if (!decide || player != 0) return 0;
        return Controller_TimelineSkipPressed() ? DecideMask() : 0;
    }
    if (g.watching || static_cast<int>(player) != g.localTeam) return 0;
    uint64_t value = LocalRead(original, self, player);
    return value;
}

uint64_t __fastcall Hook_PadRaw(void* self, uint32_t player)
{
    return ReadFor(g_originalRaw, self, player, true);
}

uint64_t __fastcall Hook_PadAnalog(void* self, uint32_t player)
{
    return ReadFor(g_originalAnalog, self, player, false);
}

uint64_t __fastcall Hook_PadInput(void* self, uint32_t player)
{
    return ReadFor(g_originalInput, self, player, false);
}

bool MenuTakesInput(void* score)
{
    void* scheduler = Field<void*>(score, SCORE_SCHEDULER);
    if (!scheduler) return false;
    using GetFrame = uint32_t(__fastcall*)(void*);
    return Native<GetFrame>(SCHEDULER_GET_FRAME)(scheduler) > kMenuInputFrame;
}

void BeforeScoreTick(void* score)
{
    const int local = g.localTeam, remote = 1 - local;
    // The other player's side, as they last described it.
    Field<int32_t>(score, SCORE_SELECT_POS + 4 * remote) = g.remoteCursor;
    Field<int32_t>(score, SCORE_CHOICE + 4 * remote) = g.remoteChoice;
    // A choice made stays made.
    if (g.localChoice) Field<int32_t>(score, SCORE_CHOICE + 4 * local) = g.localChoice;

}

void AfterScoreTick(void* score)
{
    const int local = g.localTeam;
    g.localCursor = static_cast<uint8_t>(
        std::clamp(Field<int32_t>(score, SCORE_SELECT_POS + 4 * local), 0, 2));
    const int32_t choice = Field<int32_t>(score, SCORE_CHOICE + 4 * local);
    if (!g.localChoice && choice > 0 && choice <= 0xFF && IsChoice(static_cast<uint8_t>(choice))) {
        g.localChoice = static_cast<uint8_t>(choice);
        HookLog("NativeResult: local choice %d (game %u)\n", choice, g.generation);
    }
}

void __fastcall Hook_ScoreTick(void* score, uint64_t a, uint64_t b, uint64_t c)
{
    if (!g.active) {
        g_originalScoreTick(score, a, b, c);
        return;
    }
    g.score = score;
    g.scoreFrame = g.frame;
    g.menuOpen = MenuTakesInput(score);
    if (!g.menuSeen) {
        g.menuSeen = true;
        HookLog("NativeResult: result menu up (game %u, local team %d%s)\n", g.generation,
                g.localTeam, g.watching ? ", watching" : "");
    }
    if (g.watching) {
        g_originalScoreTick(score, a, b, c);
        return;
    }
    BeforeScoreTick(score);
    g.inScoreTick = true;
    g_originalScoreTick(score, a, b, c);
    g.inScoreTick = false;
    AfterScoreTick(score);
}

// MAIN MENU's "really?" pop-up. It looks its pad up from the player index it
// was opened for, which in a match is the second hardware slot for player 2:
// on a netplay client nothing is there. While the local player's pop-up reads
// its input, that index is pointed at the slot the local player is using.
using ChoiceInput = void(__fastcall*)(void*);
ChoiceInput g_originalChoiceInput = nullptr;

void __fastcall Hook_ChoiceInput(void* window)
{
    const uint64_t manager = *reinterpret_cast<uint64_t*>(_addr(SMVC3MANAGER_INSTANCE_PTR));
    const uint32_t player = Field<uint32_t>(window, CHOICE_WINDOW_PAD_NO);
    if (!g.active || g.watching || !manager || !MenuUp() || player >= kPadSlots ||
        static_cast<int>(player) != g.localTeam) {
        g_originalChoiceInput(window);
        return;
    }
    int32_t* pads = reinterpret_cast<int32_t*>(manager + MANAGER_PAD_ASSIGN);
    const int32_t saved = pads[player];
    // The slot with something held on it; the first one when all are idle.
    int32_t slot = 0;
    for (int candidate = 0; candidate < kPadSlots; ++candidate) {
        pads[player] = candidate;
        if (g_originalInput(g.score, player) || g_originalAnalog(g.score, player)) {
            slot = candidate;
            break;
        }
    }
    pads[player] = slot;
    g_originalChoiceInput(window);
    pads[player] = saved;
}

// A rollback that reaches back over the frame the result screen is created on
// creates it again when that frame is replayed. The screen is not part of the
// rolled-back state, so the first one is still there: a second is retired
// before it does anything.
void __fastcall Hook_ResultMove(void* result, uint64_t a, uint64_t b, uint64_t c)
{
    if (g.active) {
        if (g.result && g.result != result && g.frame - g.resultFrame <= 2 &&
            UnitAlive(g.result)) {
            if (!g.duplicateLogged) {
                g.duplicateLogged = true;
                HookLog("NativeResult: a second result screen was created (a replayed "
                        "frame); retired\n");
            }
            KillUnit(result);
            return;
        }
        g.result = result;
        g.resultFrame = g.frame;
        // A press on the result screen reaches the timeline from here: the
        // characters no longer read their pads.
        if (!g.watching) Controller_CaptureSkipInput();
    }
    g_originalResultMove(result, a, b, c);
}

// The ways out of the screen. In a session none of them is taken: the caller
// reads the two choices and does what the room match calls for.
void Outcome(ResultPhase original, void* result)
{
    if (!g.active) original(result);
}

void __fastcall Hook_Rematch(void* result) { Outcome(g_originalRematch, result); }
void __fastcall Hook_Title9(void* result) { Outcome(g_originalTitle9, result); }
void __fastcall Hook_CharSel(void* result) { Outcome(g_originalCharSel, result); }
void __fastcall Hook_TitleD(void* result) { Outcome(g_originalTitleD, result); }
void __fastcall Hook_Title10(void* result) { Outcome(g_originalTitle10, result); }

template <typename Fn> bool Hook(uintptr_t address, Fn detour, Fn* original, const char* name)
{
    void* target = reinterpret_cast<void*>(_addr(address));
    MH_STATUS status = MH_CreateHook(target, reinterpret_cast<void*>(detour),
                                     reinterpret_cast<void**>(original));
    if (status == MH_OK) status = MH_EnableHook(target);
    if (status == MH_OK) return true;
    HookLog("NativeResult: hook on %s failed (%d)\n", name, static_cast<int>(status));
    return false;
}

void Reset()
{
    const uint32_t frame = g.frame;
    g = Session{};
    g.frame = frame;
}

} // namespace

bool Install()
{
    if (g_installed) return true;
    const MH_STATUS init = MH_Initialize();
    if (init != MH_OK && init != MH_ERROR_ALREADY_INITIALIZED) {
        HookLog("NativeResult: MinHook is not available (%d)\n", static_cast<int>(init));
        return false;
    }
    bool ok = true;
    ok &= Hook(SCORE_TICK, &Hook_ScoreTick, &g_originalScoreTick, "the result menu");
    ok &= Hook(RESULT_MOVE, &Hook_ResultMove, &g_originalResultMove, "the result screen");
    ok &= Hook(RESULT_PAD_RAW, &Hook_PadRaw, &g_originalRaw, "result pad (confirm)");
    ok &= Hook(RESULT_PAD_ANALOG, &Hook_PadAnalog, &g_originalAnalog, "result pad (directions)");
    ok &= Hook(RESULT_PAD_INPUT, &Hook_PadInput, &g_originalInput, "result pad (repeat)");
    ok &= Hook(RESULT_PHASE_REMATCH, &Hook_Rematch, &g_originalRematch, "result: rematch");
    ok &= Hook(RESULT_PHASE_TITLE_9, &Hook_Title9, &g_originalTitle9, "result: title");
    ok &= Hook(RESULT_PHASE_CHARSEL, &Hook_CharSel, &g_originalCharSel, "result: character select");
    ok &= Hook(RESULT_PHASE_TITLE_D, &Hook_TitleD, &g_originalTitleD, "result: main menu");
    ok &= Hook(RESULT_PHASE_TITLE_10, &Hook_Title10, &g_originalTitle10, "result: title (2)");
    ok &= Hook(CHOICE_WINDOW_INPUT, &Hook_ChoiceInput, &g_originalChoiceInput,
               "the leave pop-up's input");
    g_installed = ok;
    HookLog("NativeResult: hooks %s\n", ok ? "installed" : "incomplete; the result menu is the game's own");
    return ok;
}

void Tick(bool active, int localTeam, bool watching, uint32_t generation)
{
    ++g.frame;
    if (!g_installed) return;
    if (!active) {
        if (g.active) Reset();
        return;
    }
    if (!g.active || g.generation != generation || g.localTeam != localTeam ||
        g.watching != watching) {
        Reset();
        g.active = true;
        g.localTeam = localTeam & 1;
        g.watching = watching;
        g.generation = generation;
    }
}

bool MenuUp()
{
    return g.active && g.score && g.frame - g.scoreFrame <= 2;
}

Choice LocalChoice() { return g.active ? static_cast<Choice>(g.localChoice) : kNone; }
Choice RemoteChoice() { return g.active ? static_cast<Choice>(g.remoteChoice) : kNone; }

bool TakeOutbound(ResultMenuPacket* packet)
{
    if (!g.active || g.watching || !g.menuSeen || !packet) return false;
    ResultMenuPacket current = {};
    current.type = PACKET_EXTENSION;
    current.magic = kResultMenuMagic;
    current.generation = g.generation;
    current.cursor = g.localCursor;
    current.choice = g.localChoice;
    const ULONGLONG now = GetTickCount64();
    const bool changed = memcmp(&current, &g.lastSent, sizeof(current)) != 0;
    if (!changed && g.lastSentTick && now - g.lastSentTick < kRepeatMs) return false;
    g.lastSent = current;
    g.lastSentTick = now;
    *packet = current;
    return true;
}

void OnPeerPacket(const ResultMenuPacket& packet)
{
    if (!g.active || g.watching) return;
    if (packet.type != PACKET_EXTENSION || packet.magic != kResultMenuMagic) return;
    // A packet of the game before (or after) this one is simply late.
    if (packet.generation != g.generation) return;
    if (packet.cursor > 2 || (packet.choice != kNone && !IsChoice(packet.choice))) {
        if (!g.badPacketLogged) {
            g.badPacketLogged = true;
            HookLog("NativeResult: dropped a result-menu packet that failed its checks "
                    "(cursor %u choice %u)\n", packet.cursor, packet.choice);
        }
        return;
    }
    g.remoteCursor = packet.cursor;
    if (!g.remoteChoice && packet.choice) {
        g.remoteChoice = packet.choice;
        HookLog("NativeResult: the other player chose %u (game %u)\n", packet.choice,
                g.generation);
    }
}

void KillScreen()
{
    if (!g.active || !g.result || g.frame - g.resultFrame > 2) return;
    if (UnitAlive(g.result)) KillUnit(g.result);
    g.result = nullptr;
    g.score = nullptr;
}

} // namespace resultmenu
