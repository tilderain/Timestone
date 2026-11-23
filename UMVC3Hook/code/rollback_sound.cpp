#include "../pch.h"

#include "rollback_sound.h"

#include "controller.h"
#include "gameplay_hooks.h"
#include "lag_diagnostics.h"
#include "mk10utils.h"
#include "netplay.h"
#include "rollback.h"
#include "rollback_profile.h"

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <vector>

extern void HookLog(const char* fmt, ...);

// ---------------------------------------------------------------------------
// Rollback sound (doc/sound_rollback_plan.md)
//
// Every sound reaches sSound through two ring writers, setSeEntry and
// setStreamEntry, and is named by the triple (request, cue id, owner key).
// Live gameplay starts are recorded per frame in a ledger and bound, after
// sSound::move resolves them, to the voice they created (voice serial +0x44).
// During resim nothing reaches sSound: commands are buffered and, at the end,
// compared with the ledger. A replayed start that matches a live one is
// dropped (the voice keeps playing), a start only the corrected timeline has
// is played, and a live voice only the abandoned future had is killed.
//
// Cut-off protection: a gameplay stop on an unconfirmed frame is held instead
// of applied, and released once its frame is confirmed. If a rollback replaces
// that frame and the corrected timeline does not stop the voice again, the
// held stop is dropped and the voice plays on uninterrupted.
// ---------------------------------------------------------------------------

namespace {

constexpr uintptr_t FN_SET_SE_ENTRY         = 0x1405D1100;
constexpr uintptr_t FN_SET_STREAM_ENTRY     = 0x1405D2410;
constexpr uintptr_t FN_SET_STREAM_ENTRY_NUM = 0x1405D2590;
constexpr uintptr_t FN_SSOUND_MOVE          = 0x1405CC040;
constexpr uintptr_t FN_UNIT_MOVE_LINE       = 0x14051B4C0; // sUnit vtable +0x48
constexpr uintptr_t FN_UNIT_MOVE_AFTER_LINE = 0x14051B6F0; // +0x50
constexpr uintptr_t FN_UNIT_SYNC_LINE       = 0x14051BF10; // +0x60
constexpr uintptr_t FN_HIT_SOLVER_MOVE      = 0x140011D90;
constexpr uintptr_t FN_CHARACTER_MOVE       = 0x1400597F0;
constexpr uintptr_t FN_NARRATION_UPDATE     = 0x1402B1330; // sGSound__updateActiveCueOrReset
constexpr uintptr_t FN_QUEUE_GROUP4_VOICE   = 0x1402AE4B0; // writes narration queue 0
constexpr uintptr_t FN_QUEUE_GROUP5_VOICE   = 0x1402AE190; // writes narration queue 1
constexpr uintptr_t FN_RESOURCE_ADD_REF     = 0x14050D100;
constexpr uintptr_t FN_RESOURCE_RELEASE     = 0x14050D5A0;
constexpr uintptr_t FN_MVC3MGR_GETINSTANCE  = 0x140001AF0;
constexpr uintptr_t FN_GET_GSOUND           = 0x140259270;
constexpr uintptr_t PTR_SSOUND              = 0x140E18520;

// sSound
constexpr uint32_t SOUND_SERIAL = 0x1D720;
struct VoiceArray { uint32_t base, stride, count; };
constexpr VoiceArray VOICES[2] = {{0x70, 0xB50, 0x1CF50},      // SE
                                  {0x16A70, 0xB30, 0x1CF54}};  // stream
constexpr uint32_t VOICE_ACTIVE = 0x08, VOICE_STATE = 0x0C, VOICE_REQUEST = 0x10,
                   VOICE_ID = 0x18, VOICE_OWNER = 0x20, VOICE_SERIAL = 0x44;
constexpr uint32_t VOICE_STATE_STOP = 2;
// The unit a positioned voice follows (borrowed, not retained).
constexpr uint32_t VOICE_TARGET = 0x80;
// Command rings (sSound::setSeEntry / setStreamEntry): type +0x18, sub +0x1C,
// the five argument qwords from +0x20.
struct Ring { uint32_t lock, head, count, base, capacity, stride; };
constexpr Ring RINGS[2] = {{0x1CF00, 0x1CF98, 0x1CFA8, 0x1CEF0, 0x1CF60, 0x80},   // SE
                           {0x1CF28, 0x1CFD8, 0x1CFE8, 0x1CEF8, 0x1CF64, 0x78}};  // stream
constexpr uint32_t RING_TYPE = 0x18, RING_SUB = 0x1C, RING_ARGS = 0x20;
constexpr uint32_t UNIT_STATE = 0x10;
constexpr uint32_t STREAM_RING_CAPACITY = 0x1CF64, STREAM_RING_ENTRIES = 128; // native default 32
// Ring command types (entry +0x18).
constexpr uint32_t CMD_START = 1, CMD_START_BANK = 2, CMD_STOP = 3, CMD_CONTROL = 6;
// A start with sub 2 follows the unit in argument 3; control 0x19 points a
// playing voice at the unit in argument 0 (sSound__applyVoiceCommand).
constexpr uint32_t START_FOLLOW_UNIT = 2, CONTROL_SET_TARGET = 0x19;

// sGSound
// Narration (announcer) queues: 8 entries of 0x38, flags at +0x30.
constexpr uint32_t NARRATION_QUEUE[2] = {0x2610, 0x27D8};
constexpr uint32_t NARRATION_SLOTS = 8, NARRATION_STRIDE = 0x38, NARRATION_FLAGS = 0x30;
constexpr uint32_t NARRATION_PENDING = 1, NARRATION_STARTED = 2, NARRATION_DONE = 8;
struct Range { uint32_t offset, size; };
constexpr Range GSOUND_STATE[] = {
    // Per character slot (stride 0x438):
    // +0x00 delayed voice record: cRef, character, cue, key, flags, delay.
    {0xA38 + 0 * 0x438, 0x30}, {0xA38 + 1 * 0x438, 0x30}, {0xA38 + 2 * 0x438, 0x30},
    {0xA38 + 3 * 0x438, 0x30}, {0xA38 + 4 * 0x438, 0x30}, {0xA38 + 5 * 0x438, 0x30},
    // +0x70 voice pause state (FUN_1402B1280, every character move) and +0x74
    // the death voice latch: sGSound__playVoiceForCharAction category 0 (cues
    // 0x11..0x13) plays once until the match phase resets it. Unrewound, a
    // resim across a KO hits the latch, never queues the death voice, and the
    // live one is killed as future-only or wiped from the restored record.
    {0xAA8 + 0 * 0x438, 8}, {0xAA8 + 1 * 0x438, 8}, {0xAA8 + 2 * 0x438, 8},
    {0xAA8 + 3 * 0x438, 8}, {0xAA8 + 4 * 0x438, 8}, {0xAA8 + 5 * 0x438, 8},
    // Same-tick group queue (20 x 0x50). The narration queues follow it and
    // are merged per entry instead (Rollback_RestoreSoundState).
    {0x1FD0, 0x2610 - 0x1FD0},
};
constexpr size_t GSoundStateSize()
{
    size_t size = 0;
    for (const Range& range : GSOUND_STATE) size += range.size;
    return size;
}

constexpr int32_t NO_FRAME = INT32_MIN;
constexpr int32_t MATCH_TOLERANCE = 2;
constexpr size_t LEDGER_CAPACITY = 1024;
constexpr int MAX_ENTRY_VOICES = 4;

using SetEntryFn = void(__fastcall*)(uint64_t, uint64_t, uint32_t, uint64_t, const uint64_t*,
                                     uint8_t, uint32_t, uint32_t, uint64_t, uint64_t, uint64_t);
using SoundMoveFn = void(__fastcall*)(uint64_t);
using LineFn = void(__fastcall*)(uint64_t, uint32_t);
using Call4Fn = void(__fastcall*)(uint64_t, uint64_t, uint64_t, uint64_t);

SetEntryFn oSetEntry[2] = {};
SoundMoveFn oSoundMove = nullptr;
LineFn oMoveLine = nullptr, oMoveAfterLine = nullptr, oSyncLine = nullptr;
Call4Fn oHitSolverMove = nullptr, oCharacterMove = nullptr, oNarrationUpdate = nullptr;
using QueueVoiceFn = void(__fastcall*)(uint64_t, uint32_t, uint8_t, uint32_t, uint64_t);
QueueVoiceFn oQueueVoice[2] = {};

struct Voice { uint64_t ptr; uint32_t serial; };

struct Entry {
    uint64_t request, owner;
    uint32_t id;
    uint8_t kind;
    int32_t frame;
    bool alive = true;        // no applied stop yet
    bool matched = false;     // replayed by the current resim
    bool bindOpen = true;     // waiting for sSound to create its voice
    bool heldReissued = false;
    uint8_t bindTries = 0;
    uint8_t voiceCount = 0;
    uint32_t bindBatch = 0;
    int32_t heldStop = NO_FRAME;
    Voice voices[MAX_ENTRY_VOICES];
};

struct Command {
    uint64_t request, owner, args[5], p9, p10, p11;
    uint32_t id, type, sub;
    int32_t frame;
    uint8_t link, kind;
    bool forward, isNew, cancelled;
};

SRWLOCK g_lock = SRWLOCK_INIT;
std::vector<Entry> g_ledger;
std::vector<Command> g_resim;
size_t g_pendingBind = 0;
uint32_t g_bindBatch = 0;
int32_t g_lastExecutingFrame = NO_FRAME;

bool g_soundResim = false;
int32_t g_resimStart = 0;
int32_t g_resimStaleFrame = 0;
bool g_resimLoopSeen = false;

std::atomic<int> g_simDepth{0};
thread_local bool t_presentation = false;

// Which frame queued each narration entry, and whether gameplay did.
struct NarrationTag { int32_t frame = NO_FRAME; bool sim = false; };
NarrationTag g_narrationTag[2][NARRATION_SLOTS];

struct GSoundStateSlot {
    int32_t frame = NO_FRAME;
    uint8_t data[GSoundStateSize()];
    uint8_t narration[2][NARRATION_SLOTS][NARRATION_STRIDE];
    NarrationTag tags[2][NARRATION_SLOTS];
};
GSoundStateSlot g_gsoundState[ROLLBACK_WINDOW];

struct Lock {
    Lock() { AcquireSRWLockExclusive(&g_lock); }
    ~Lock() { ReleaseSRWLockExclusive(&g_lock); }
};

uint64_t SoundInstance() { return *(uint64_t*)_addr(PTR_SSOUND); }

uint64_t ManagerSubsystem(uintptr_t getter)
{
    const uint64_t manager = ((uint64_t(__fastcall*)())_addr(FN_MVC3MGR_GETINSTANCE))();
    return manager ? ((uint64_t(__fastcall*)(uint64_t))_addr(getter))(manager) : 0;
}

void AddRef(uint64_t resource)
{
    if (resource) ((void(__fastcall*)(uint64_t))_addr(FN_RESOURCE_ADD_REF))(resource);
}

void Release(uint64_t resource)
{
    if (resource) ((void(__fastcall*)(uint64_t))_addr(FN_RESOURCE_RELEASE))(resource);
}

bool IsStart(uint32_t type) { return type == CMD_START || type == CMD_START_BANK; }

// Native stop/status matching: request 0, id -1 and owner -1 are wildcards.
bool PatternMatches(uint64_t request, uint32_t id, uint64_t owner,
                    uint64_t keyRequest, uint32_t keyId, uint64_t keyOwner)
{
    return (request == 0 || request == keyRequest) &&
           (id == UINT32_MAX || id == keyId) &&
           (owner == UINT64_MAX || owner == keyOwner);
}

// Lanes the headless tick replays (rollback_tick.cpp IsHeadlessGameplayUnitCategory).
bool IsSimLine(uint32_t line)
{
    return line == 3 || (line >= 7 && line <= 16) || line == 18 || line == 21;
}

int32_t CommandFrame()
{
    if (!g_soundResim) return Controller_GetExecutingFrame();
    // Commands issued by the restore, before the first replayed frame, belong
    // to the start frame; g_resimulateCurrentFrame still holds a stale value.
    if (!g_resimLoopSeen && g_resimulateCurrentFrame != g_resimStaleFrame)
        g_resimLoopSeen = true;
    return g_resimLoopSeen ? g_resimulateCurrentFrame : g_resimStart;
}

bool VoiceAlive(const Voice& voice)
{
    if (*(const uint32_t*)(voice.ptr + VOICE_SERIAL) != voice.serial) return false;
    const uint32_t active = *(const uint32_t*)(voice.ptr + VOICE_ACTIVE);
    const uint32_t state = *(const uint32_t*)(voice.ptr + VOICE_STATE);
    return (active != 0 && state != VOICE_STATE_STOP) || ((state - 1) & ~4u) == 0;
}

bool AnyVoiceAlive(const Entry& entry)
{
    for (int i = 0; i < entry.voiceCount; ++i)
        if (VoiceAlive(entry.voices[i])) return true;
    return false;
}

// Stop exactly this entry's voices. A start still waiting in the native ring
// gets a stop by triple queued behind it.
void KillEntry(uint64_t sound, Entry& entry)
{
    for (int i = 0; i < entry.voiceCount; ++i)
        if (VoiceAlive(entry.voices[i]))
            *(uint32_t*)(entry.voices[i].ptr + VOICE_STATE) = VOICE_STATE_STOP;
    if (entry.voiceCount == 0 && entry.bindOpen && sound) {
        const uint64_t noArgs[5] = {};
        oSetEntry[entry.kind](sound, entry.request, entry.id, entry.owner, noArgs, 0,
                              CMD_STOP, 0, 0, 0, 0);
    }
    entry.alive = false;
    entry.heldStop = NO_FRAME;
}

void PushEntry(uint8_t kind, uint64_t request, uint32_t id, uint64_t owner, int32_t frame)
{
    if (g_ledger.size() >= LEDGER_CAPACITY) {
        if (g_ledger.front().bindOpen) --g_pendingBind;
        Release(g_ledger.front().request);
        g_ledger.erase(g_ledger.begin());
    }
    AddRef(request);
    Entry entry{};
    entry.request = request;
    entry.owner = owner;
    entry.id = id;
    entry.kind = kind;
    entry.frame = frame;
    entry.alive = true;
    entry.bindOpen = true;
    entry.heldStop = NO_FRAME;
    g_ledger.push_back(entry);
    ++g_pendingBind;
}

template <typename Pred>
void EraseEntries(Pred pred)
{
    size_t out = 0;
    for (size_t i = 0; i < g_ledger.size(); ++i) {
        Entry& entry = g_ledger[i];
        if (pred(entry)) {
            if (entry.bindOpen) --g_pendingBind;
            Release(entry.request);
            continue;
        }
        if (out != i) g_ledger[out] = entry;
        ++out;
    }
    g_ledger.resize(out);
}

bool CanRollBackTo(int32_t frame)
{
    return Netplay_IsActive() && !Controller_IsFrameConfirmed(frame) &&
           Controller_GetExecutingFrame() - frame < ROLLBACK_WINDOW;
}

// Returns true when every tracked voice the stop hits is held, so the native
// stop must not run. Untracked stops pass through.
bool LiveStop(uint8_t kind, uint64_t request, uint32_t id, uint64_t owner, bool sim, int32_t frame)
{
    const bool hold = sim && CanRollBackTo(frame);
    bool matched = false;
    for (Entry& entry : g_ledger) {
        if (entry.kind != kind || !entry.alive ||
            !PatternMatches(request, id, owner, entry.request, entry.id, entry.owner))
            continue;
        matched = true;
        if (!hold) entry.alive = false;
        else if (entry.heldStop == NO_FRAME) entry.heldStop = frame;
    }
    return hold && matched;
}

template <int Kind>
void __fastcall hkSetEntry(uint64_t sound, uint64_t request, uint32_t id, uint64_t owner,
                           const uint64_t* args, uint8_t link, uint32_t type, uint32_t sub,
                           uint64_t p9, uint64_t p10, uint64_t p11)
{
    if (g_soundResim) {
        if (t_presentation) return;
        Command command{};
        command.request = request;
        command.owner = owner;
        if (args) std::memcpy(command.args, args, sizeof(command.args));
        command.p9 = p9;
        command.p10 = p10;
        command.p11 = p11;
        command.id = id;
        command.type = type;
        command.sub = sub;
        command.link = link;
        command.kind = (uint8_t)Kind;
        AddRef(request);
        Lock lock;
        command.frame = CommandFrame();
        g_resim.push_back(command);
        return;
    }

    const int32_t frame = Controller_GetExecutingFrame();
    if (frame >= 0 && !t_presentation) {
        const bool sim = g_simDepth.load(std::memory_order_relaxed) > 0;
        if (IsStart(type)) {
            if (sim) {
                Lock lock;
                PushEntry((uint8_t)Kind, request, id, owner, frame);
            }
        } else if (type == CMD_STOP) {
            Lock lock;
            if (LiveStop((uint8_t)Kind, request, id, owner, sim, frame)) return;
        }
    }
    oSetEntry[Kind](sound, request, id, owner, args, link, type, sub, p9, p10, p11);
}

// Bind voices created by this sSound::move (serials in (before, after]) to the
// starts that asked for them. A chained request creates several voices with
// different cue ids from one start; those join the entry bound in this batch.
void BindVoices(uint64_t sound, uint32_t before, uint32_t after)
{
    const uint32_t batch = ++g_bindBatch;
    for (int kind = 0; kind < 2; ++kind) {
        const VoiceArray& array = VOICES[kind];
        const uint32_t count = *(const uint32_t*)(sound + array.count);
        for (uint32_t i = 0; i < count; ++i) {
            const uint64_t voice = sound + array.base + (uint64_t)i * array.stride;
            const uint32_t serial = *(const uint32_t*)(voice + VOICE_SERIAL);
            if (serial - before - 1 >= after - before) continue;
            const uint64_t request = *(const uint64_t*)(voice + VOICE_REQUEST);
            const uint32_t id = *(const uint32_t*)(voice + VOICE_ID);
            const uint64_t owner = *(const uint64_t*)(voice + VOICE_OWNER);
            Entry* exact = nullptr;
            Entry* chain = nullptr;
            for (Entry& entry : g_ledger) {
                if (!entry.bindOpen || entry.kind != kind || entry.request != request ||
                    entry.owner != owner)
                    continue;
                if (entry.voiceCount == 0 && entry.id == id) { exact = &entry; break; }
                if (!chain && entry.bindBatch == batch && entry.voiceCount < MAX_ENTRY_VOICES)
                    chain = &entry;
            }
            Entry* target = exact ? exact : chain;
            if (!target) continue;
            target->voices[target->voiceCount++] = {voice, serial};
            target->bindBatch = batch;
        }
    }
}

void CloseBinding()
{
    for (Entry& entry : g_ledger) {
        if (!entry.bindOpen) continue;
        if (entry.voiceCount || ++entry.bindTries > 2) {
            entry.bindOpen = false;
            --g_pendingBind;
        }
    }
}

void ReleaseHeldStops(uint64_t sound)
{
    for (Entry& entry : g_ledger)
        if (entry.heldStop != NO_FRAME && !CanRollBackTo(entry.heldStop))
            KillEntry(sound, entry);
}

void RetireEntries(int32_t frame)
{
    if (g_lastExecutingFrame != NO_FRAME && frame < g_lastExecutingFrame - ROLLBACK_WINDOW) {
        // The frame counter restarted (new match): nothing can roll back into the old one.
        EraseEntries([](const Entry&) { return true; });
    }
    g_lastExecutingFrame = frame;
    EraseEntries([frame](const Entry& entry) {
        return !entry.bindOpen && entry.heldStop == NO_FRAME &&
               frame - entry.frame > ROLLBACK_WINDOW + MATCH_TOLERANCE &&
               (!entry.alive || !AnyVoiceAlive(entry));
    });
}

void __fastcall hkSoundMove(uint64_t sound)
{
    LagDiagnosticScope diagnostic(LagMetric::SoundMove);
    RollbackHookProfileScope profile(RollbackHookProfileId::SoundMove);
    {
        Lock lock;
        ReleaseHeldStops(sound);
    }
    const uint32_t before = *(const uint32_t*)(sound + SOUND_SERIAL);
    oSoundMove(sound);
    // The rings are drained here. The native 32-entry stream ring overflows
    // (dropping commands) when a resim flush meets a live frame's own.
    if (*(const uint32_t*)(sound + STREAM_RING_CAPACITY) < STREAM_RING_ENTRIES)
        ((void(__fastcall*)(uint64_t, uint32_t))_addr(FN_SET_STREAM_ENTRY_NUM))(sound, STREAM_RING_ENTRIES);
    const uint32_t after = *(const uint32_t*)(sound + SOUND_SERIAL);
    Lock lock;
    if (g_pendingBind) {
        if (after != before) BindVoices(sound, before, after);
        CloseBinding();
    }
    RetireEntries(Controller_GetExecutingFrame());
}

// The argument of a command that names the unit a voice follows, if any.
uint64_t* CommandTarget(uint32_t type, uint32_t sub, uint64_t* args)
{
    if (type == CMD_START && sub == START_FOLLOW_UNIT) return &args[3];
    if (type == CMD_CONTROL && sub == CONTROL_SET_TARGET) return &args[0];
    return nullptr;
}

// With `freed`, that object only. Without, sSound's own rule (0x1405D6D38,
// 0x1405D76E0): a followed unit is alive in lifecycle states 1 and 2.
bool TargetGone(uint64_t target, uint64_t freed)
{
    if (!target) return false;
    if (freed) return target == freed;
    return ((*(const uint32_t*)(target + UNIT_STATE) & 7) - 1) > 1;
}

// sSound drops a dead target once per sSound::move, and a dying unit stays
// allocated long enough for that. A resim runs several ticks with no
// sSound::move in between, so a unit can die and be freed, and its memory
// reused, before the check runs again: the voice then calls into whatever
// object lives there (crash of 2026-11-22, a uUiSprite3D). Apply the same
// rule to every place a target waits: voices, both rings, and the commands
// this resim has buffered.
void DropSoundTargets(uint64_t freed)
{
    const uint64_t sound = SoundInstance();
    if (sound) {
        for (const VoiceArray& array : VOICES) {
            const uint32_t count = *(const uint32_t*)(sound + array.count);
            for (uint32_t i = 0; i < count; ++i) {
                uint64_t& target = *(uint64_t*)(sound + array.base + (uint64_t)i * array.stride + VOICE_TARGET);
                if (TargetGone(target, freed)) target = 0;
            }
        }
        for (const Ring& ring : RINGS) {
            CRITICAL_SECTION* lock = (CRITICAL_SECTION*)(sound + ring.lock);
            EnterCriticalSection(lock);
            const uint64_t base = *(const uint64_t*)(sound + ring.base);
            const uint64_t end = base + (uint64_t)*(const uint32_t*)(sound + ring.capacity) * ring.stride;
            uint64_t entry = *(const uint64_t*)(sound + ring.head);
            for (int32_t left = *(const int32_t*)(sound + ring.count); entry && left > 0; --left) {
                uint64_t* target = CommandTarget(*(const uint32_t*)(entry + RING_TYPE),
                                                 *(const uint32_t*)(entry + RING_SUB),
                                                 (uint64_t*)(entry + RING_ARGS));
                if (target && TargetGone(*target, freed)) *target = 0;
                entry += ring.stride;
                if (entry == end) entry = base;
            }
            LeaveCriticalSection(lock);
        }
    }
    Lock lock;
    for (Command& command : g_resim) {
        uint64_t* target = CommandTarget(command.type, command.sub, command.args);
        if (target && TargetGone(*target, freed)) *target = 0;
    }
}

template <LineFn* Original>
void __fastcall hkLine(uint64_t unit, uint32_t line)
{
    const bool sim = IsSimLine(line);
    if (sim) g_simDepth.fetch_add(1, std::memory_order_relaxed);
    (*Original)(unit, line);
    if (sim) g_simDepth.fetch_sub(1, std::memory_order_relaxed);
}

template <Call4Fn* Original>
void __fastcall hkSimScope(uint64_t a, uint64_t b, uint64_t c, uint64_t d)
{
    g_simDepth.fetch_add(1, std::memory_order_relaxed);
    (*Original)(a, b, c, d);
    g_simDepth.fetch_sub(1, std::memory_order_relaxed);
}

Entry* FindReplayedStart(const Command& command)
{
    Entry* best = nullptr;
    int32_t bestDistance = MATCH_TOLERANCE + 1;
    for (Entry& entry : g_ledger) {
        if (entry.matched || entry.frame < g_resimStart || entry.kind != command.kind ||
            entry.request != command.request || entry.id != command.id ||
            entry.owner != command.owner)
            continue;
        const int32_t distance = std::abs(entry.frame - command.frame);
        if (distance < bestDistance) {
            best = &entry;
            bestDistance = distance;
        }
    }
    return best;
}

// A replayed stop lands on the voices the corrected timeline has at that
// point: earlier live voices it keeps, and its own new starts. It is never
// forwarded as a wildcard, which would also hit voices started after it.
void ReplayStop(size_t index)
{
    Command& stop = g_resim[index];
    for (Entry& entry : g_ledger) {
        if (entry.kind != stop.kind || !entry.alive ||
            (entry.frame >= g_resimStart && !entry.matched) ||
            !PatternMatches(stop.request, stop.id, stop.owner, entry.request, entry.id, entry.owner))
            continue;
        if (entry.heldStop == NO_FRAME) entry.heldStop = stop.frame;
        entry.heldReissued = true;
    }
    for (size_t i = 0; i < index; ++i) {
        Command& start = g_resim[i];
        if (start.isNew && !start.cancelled && start.kind == stop.kind &&
            PatternMatches(stop.request, stop.id, stop.owner, start.request, start.id, start.owner)) {
            start.cancelled = true;
            start.forward = false;
        }
    }
}

bool TargetsNewStart(size_t index)
{
    const Command& command = g_resim[index];
    for (size_t i = index; i-- > 0;) {
        const Command& start = g_resim[i];
        if (start.isNew && start.kind == command.kind &&
            PatternMatches(command.request, command.id, command.owner,
                           start.request, start.id, start.owner))
            return !start.cancelled;
    }
    return false;
}

uint32_t& NarrationFlags(uint64_t gsound, int queue, uint32_t slot)
{
    return *(uint32_t*)(gsound + NARRATION_QUEUE[queue] + slot * NARRATION_STRIDE + NARRATION_FLAGS);
}

bool IsQueuedOnly(uint32_t flags)
{
    return (flags & NARRATION_PENDING) && !(flags & (NARRATION_STARTED | NARRATION_DONE));
}

// Producers run in gameplay (and in resim) or in the UI. Tag each entry they
// add with its frame and source.
template <int Queue>
void __fastcall hkQueueVoice(uint64_t gsound, uint32_t cue, uint8_t gated, uint32_t flags, uint64_t owner)
{
    uint32_t before[NARRATION_SLOTS];
    for (uint32_t i = 0; i < NARRATION_SLOTS; ++i) before[i] = NarrationFlags(gsound, Queue, i);
    oQueueVoice[Queue](gsound, cue, gated, flags, owner);
    const bool sim = g_soundResim || (!t_presentation && g_simDepth.load(std::memory_order_relaxed) > 0);
    Lock lock;
    const int32_t frame = CommandFrame();
    for (uint32_t i = 0; i < NARRATION_SLOTS; ++i) {
        const uint32_t after = NarrationFlags(gsound, Queue, i);
        if (IsQueuedOnly(after) && (!(before[i] & NARRATION_PENDING) || (before[i] & NARRATION_DONE)))
            g_narrationTag[Queue][i] = {frame, sim};
    }
}

// The announcer is presentation: it runs on live frames only and never sees
// a gameplay entry until the frame that queued it is confirmed, so it never
// announces something a rollback takes back, and its voices are never
// killed or held by the ledger.
void __fastcall hkNarrationUpdate(uint64_t gsound, uint64_t ref, uint64_t queueAddress, uint64_t extra)
{
    int queue = -1;
    for (int q = 0; q < 2; ++q)
        if (queueAddress == gsound + NARRATION_QUEUE[q]) queue = q;
    uint32_t masked = 0;
    if (queue >= 0 && Netplay_IsActive()) {
        for (uint32_t i = 0; i < NARRATION_SLOTS; ++i) {
            uint32_t& flags = NarrationFlags(gsound, queue, i);
            const NarrationTag& tag = g_narrationTag[queue][i];
            if (IsQueuedOnly(flags) && tag.sim && tag.frame != NO_FRAME && CanRollBackTo(tag.frame)) {
                flags &= ~NARRATION_PENDING;
                masked |= 1u << i;
            }
        }
    }
    {
        RollbackSoundPresentationScope presentation;
        oNarrationUpdate(gsound, ref, queueAddress, extra);
    }
    for (uint32_t i = 0; i < NARRATION_SLOTS; ++i)
        if (masked & (1u << i)) NarrationFlags(gsound, queue, i) |= NARRATION_PENDING;
}

} // namespace

RollbackSoundPresentationScope::RollbackSoundPresentationScope() : previous(t_presentation)
{
    t_presentation = true;
}

RollbackSoundPresentationScope::~RollbackSoundPresentationScope()
{
    t_presentation = previous;
}

void Rollback_BeginSoundResim(int32_t startFrame)
{
    Lock lock;
    g_soundResim = true;
    g_resimStart = startFrame;
    g_resimStaleFrame = g_resimulateCurrentFrame;
    g_resimLoopSeen = false;
    g_resim.clear();
    for (Entry& entry : g_ledger) {
        if (entry.frame >= startFrame) entry.matched = false;
        if (entry.heldStop >= startFrame) entry.heldReissued = false;
    }
}

void Rollback_EndSoundResim()
{
    const uint64_t sound = SoundInstance();
    Lock lock;
    if (!g_soundResim) return;
    g_soundResim = false;

    for (size_t i = 0; i < g_resim.size(); ++i) {
        Command& command = g_resim[i];
        if (IsStart(command.type)) {
            if (Entry* entry = FindReplayedStart(command)) {
                entry->matched = true;
            } else {
                command.forward = command.isNew = true;
            }
        } else if (command.type == CMD_STOP) {
            ReplayStop(i);
        } else {
            command.forward = TargetsNewStart(i);
        }
    }

    // Voices only the abandoned future started.
    for (Entry& entry : g_ledger) {
        if (entry.frame >= g_resimStart && !entry.matched) {
            KillEntry(sound, entry);
            entry.matched = true; // erase below
            entry.frame = NO_FRAME;
        } else if (entry.heldStop >= g_resimStart && !entry.heldReissued) {
            entry.heldStop = NO_FRAME; // the stop belonged to the abandoned future
        }
    }
    EraseEntries([](const Entry& entry) { return entry.frame == NO_FRAME; });

    for (Command& command : g_resim) {
        if (command.forward && sound) {
            oSetEntry[command.kind](sound, command.request, command.id, command.owner,
                                    command.args, command.link, command.type, command.sub,
                                    command.p9, command.p10, command.p11);
            if (command.isNew)
                PushEntry(command.kind, command.request, command.id, command.owner, command.frame);
        }
        Release(command.request);
    }
    g_resim.clear();
}

void Rollback_DropDeadSoundTargets()
{
    if (g_soundResim) DropSoundTargets(0);
}

void Rollback_ForgetSoundTarget(const void* object)
{
    if (g_soundResim && object) DropSoundTargets((uint64_t)object);
}

void Rollback_SaveSoundState(int32_t frame)
{
    const uint64_t gsound = ManagerSubsystem(FN_GET_GSOUND);
    if (!gsound) return;
    GSoundStateSlot& slot = g_gsoundState[(uint32_t)frame % ROLLBACK_WINDOW];
    uint8_t* out = slot.data;
    for (const Range& range : GSOUND_STATE) {
        std::memcpy(out, (const void*)(gsound + range.offset), range.size);
        out += range.size;
    }
    for (int q = 0; q < 2; ++q)
        std::memcpy(slot.narration[q], (const void*)(gsound + NARRATION_QUEUE[q]), sizeof(slot.narration[q]));
    std::memcpy(slot.tags, g_narrationTag, sizeof(slot.tags));
    slot.frame = frame;
}

void Rollback_RestoreSoundState(int32_t frame)
{
    const uint64_t gsound = ManagerSubsystem(FN_GET_GSOUND);
    const GSoundStateSlot& slot = g_gsoundState[(uint32_t)frame % ROLLBACK_WINDOW];
    if (!gsound || slot.frame != frame) return;
    const uint8_t* in = slot.data;
    for (const Range& range : GSOUND_STATE) {
        std::memcpy((void*)(gsound + range.offset), in, range.size);
        in += range.size;
    }
    // Rewind only what gameplay queued and the announcer has not taken yet.
    // Started or finished entries belong to the announcer, which acts on
    // confirmed entries only; UI-queued entries are not replayed by resim.
    for (int q = 0; q < 2; ++q) {
        for (uint32_t i = 0; i < NARRATION_SLOTS; ++i) {
            const uint32_t current = NarrationFlags(gsound, q, i);
            NarrationTag& tag = g_narrationTag[q][i];
            if ((current & NARRATION_PENDING) && (!IsQueuedOnly(current) || !tag.sim)) continue;
            uint8_t* entry = (uint8_t*)(gsound + NARRATION_QUEUE[q] + i * NARRATION_STRIDE);
            const uint8_t* saved = slot.narration[q][i];
            const uint32_t savedFlags = *(const uint32_t*)(saved + NARRATION_FLAGS);
            if (IsQueuedOnly(savedFlags) && slot.tags[q][i].sim) {
                std::memcpy(entry, saved, NARRATION_STRIDE);
                tag = slot.tags[q][i];
            } else {
                std::memset(entry, 0, NARRATION_STRIDE);
                tag = {};
            }
        }
    }
}

void Rollback_InitSoundHooks()
{
    g_ledger.reserve(LEDGER_CAPACITY);
    g_resim.reserve(1024);

    struct Hook { uintptr_t address; void* detour; void** original; const char* name; };
    const Hook hooks[] = {
        {FN_SET_SE_ENTRY, (void*)&hkSetEntry<0>, (void**)&oSetEntry[0], "sSound::setSeEntry"},
        {FN_SET_STREAM_ENTRY, (void*)&hkSetEntry<1>, (void**)&oSetEntry[1], "sSound::setStreamEntry"},
        {FN_SSOUND_MOVE, (void*)&hkSoundMove, (void**)&oSoundMove, "sSound::move"},
        {FN_UNIT_MOVE_LINE, (void*)&hkLine<&oMoveLine>, (void**)&oMoveLine, "sUnit::moveLine"},
        {FN_UNIT_MOVE_AFTER_LINE, (void*)&hkLine<&oMoveAfterLine>, (void**)&oMoveAfterLine, "sUnit::moveAfterLine"},
        {FN_UNIT_SYNC_LINE, (void*)&hkLine<&oSyncLine>, (void**)&oSyncLine, "sUnit::syncLine"},
        {FN_HIT_SOLVER_MOVE, (void*)&hkSimScope<&oHitSolverMove>, (void**)&oHitSolverMove, "sHitSolver::move"},
        {FN_CHARACTER_MOVE, (void*)&hkSimScope<&oCharacterMove>, (void**)&oCharacterMove, "sCharacter::move"},
        {FN_NARRATION_UPDATE, (void*)&hkNarrationUpdate, (void**)&oNarrationUpdate, "narration update"},
        {FN_QUEUE_GROUP4_VOICE, (void*)&hkQueueVoice<0>, (void**)&oQueueVoice[0], "sGSound::queueGroup4Voice"},
        {FN_QUEUE_GROUP5_VOICE, (void*)&hkQueueVoice<1>, (void**)&oQueueVoice[1], "sGSound::queueGroup5Voice"},
    };
    for (const Hook& hook : hooks) {
        MH_STATUS status = GameplayHooks_Create((LPVOID)_addr(hook.address), hook.detour, (LPVOID*)hook.original);
        if (status == MH_OK) status = GameplayHooks_QueueEnable((LPVOID)_addr(hook.address));
        if (status != MH_OK)
            HookLog("Rollback sound: failed to hook %s (%d)\n", hook.name, status);
    }
}
