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
constexpr uintptr_t FN_RESOLVE_SOUND_REF    = 0x1402AA2B0;
constexpr uintptr_t FN_FIND_VOICE_BY_REF    = 0x1402A1460;
constexpr uintptr_t FN_RESOURCE_ADD_REF     = 0x14050D100;
constexpr uintptr_t FN_RESOURCE_RELEASE     = 0x14050D5A0;
constexpr uintptr_t FN_MVC3MGR_GETINSTANCE  = 0x140001AF0;
constexpr uintptr_t FN_GET_GSOUND           = 0x140259270;
constexpr uintptr_t FN_GET_GAME_UI          = 0x14097D370;
constexpr uintptr_t PTR_SSOUND              = 0x140E18520;

// sSound
constexpr uint32_t SOUND_SERIAL = 0x1D720;
struct VoiceArray { uint32_t base, stride, count; };
constexpr VoiceArray VOICES[2] = {{0x70, 0xB50, 0x1CF50},      // SE
                                  {0x16A70, 0xB30, 0x1CF54}};  // stream
constexpr uint32_t VOICE_ACTIVE = 0x08, VOICE_STATE = 0x0C, VOICE_REQUEST = 0x10,
                   VOICE_ID = 0x18, VOICE_OWNER = 0x20, VOICE_SERIAL = 0x44;
constexpr uint32_t VOICE_STATE_STOP = 2;
constexpr uint32_t STREAM_RING_CAPACITY = 0x1CF64, STREAM_RING_ENTRIES = 128; // native default 32
// Ring command types (entry +0x18).
constexpr uint32_t CMD_START = 1, CMD_START_BANK = 2, CMD_STOP = 3;

// sGSound
constexpr uint32_t GSOUND_VOICE_MANAGER = 0x29B0;
constexpr uint32_t GAMEUI_SOUND_BLOCKED = 0x5ABA;
struct NarrationQueue { uint32_t ref, offset; };
constexpr NarrationQueue NARRATION[2] = {{4, 0x2610}, {5, 0x27D8}};
struct Range { uint32_t offset, size; };
constexpr Range GSOUND_STATE[] = {
    // Delayed character voice records: cRef, character, cue, key, flags, delay.
    {0xA38 + 0 * 0x438, 0x30}, {0xA38 + 1 * 0x438, 0x30}, {0xA38 + 2 * 0x438, 0x30},
    {0xA38 + 3 * 0x438, 0x30}, {0xA38 + 4 * 0x438, 0x30}, {0xA38 + 5 * 0x438, 0x30},
    // Same-tick group queue (20 x 0x50) and both narration queues with timers.
    {0x1FD0, 0x29A0 - 0x1FD0},
    // Narration exclusivity state.
    {0x8A70, 0x0C},
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

struct GSoundStateSlot { int32_t frame = NO_FRAME; uint8_t data[GSoundStateSize()]; };
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

void ResolveNarration(uint64_t gsound, uint32_t ref, uint8_t* storage)
{
    ((uint64_t(__fastcall*)(uint64_t, uint8_t*, uint32_t))_addr(FN_RESOLVE_SOUND_REF))(
        gsound, storage, ref);
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

    size_t replayed = 0, played = 0, killed = 0, kept = 0;
    for (size_t i = 0; i < g_resim.size(); ++i) {
        Command& command = g_resim[i];
        if (IsStart(command.type)) {
            if (Entry* entry = FindReplayedStart(command)) {
                entry->matched = true;
                ++replayed;
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
            ++killed;
        } else if (entry.heldStop >= g_resimStart && !entry.heldReissued) {
            entry.heldStop = NO_FRAME; // the stop belonged to the abandoned future
            ++kept;
        }
    }
    EraseEntries([](const Entry& entry) { return entry.frame == NO_FRAME; });

    for (Command& command : g_resim) {
        if (command.forward && sound) {
            oSetEntry[command.kind](sound, command.request, command.id, command.owner,
                                    command.args, command.link, command.type, command.sub,
                                    command.p9, command.p10, command.p11);
            if (command.isNew) {
                PushEntry(command.kind, command.request, command.id, command.owner, command.frame);
                ++played;
            }
        }
        Release(command.request);
    }
    if (replayed || played || killed || kept) {
        HookLog("Rollback sound: from %d replayed=%zu played=%zu killed=%zu uncut=%zu\n",
                g_resimStart, replayed, played, killed, kept);
    }
    g_resim.clear();
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
}

void Rollback_SoundHeadlessTick()
{
    // sGSound::move's narration tail. Its pause gate never holds in resim.
    const uint64_t gsound = ManagerSubsystem(FN_GET_GSOUND);
    const uint64_t ui = ManagerSubsystem(FN_GET_GAME_UI);
    if (!gsound || !ui || !oNarrationUpdate || *(const uint8_t*)(ui + GAMEUI_SOUND_BLOCKED))
        return;
    const auto findVoice = (uint64_t(__fastcall*)(uint64_t, uint8_t*))_addr(FN_FIND_VOICE_BY_REF);
    for (const NarrationQueue& queue : NARRATION) {
        alignas(16) uint8_t ref[16];
        ResolveNarration(gsound, queue.ref, ref);
        if (!findVoice(gsound + GSOUND_VOICE_MANAGER, ref)) continue;
        ResolveNarration(gsound, queue.ref, ref);
        oNarrationUpdate(gsound, (uint64_t)ref, gsound + queue.offset, 0);
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
        {FN_NARRATION_UPDATE, (void*)&hkSimScope<&oNarrationUpdate>, (void**)&oNarrationUpdate, "narration update"},
    };
    for (const Hook& hook : hooks) {
        MH_STATUS status = GameplayHooks_Create((LPVOID)_addr(hook.address), hook.detour, (LPVOID*)hook.original);
        if (status == MH_OK) status = GameplayHooks_QueueEnable((LPVOID)_addr(hook.address));
        if (status != MH_OK)
            HookLog("Rollback sound: failed to hook %s (%d)\n", hook.name, status);
    }
    HookLog("Rollback sound: ledger hooks installed\n");
}
