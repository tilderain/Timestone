#include "../pch.h"

#include "rollback.h"
#include "mk10utils.h"
#include "gameplay_hooks.h"
#include "controller.h"
#include "netplay.h"

extern void HookLog(const char* fmt, ...);

// Missing pain voices (doc/pain_voice_fix.md).
//
// sGSound__processCharacterSoundQueues (0x1402B0E00) submits the damage voice
// groups 0x15/0x16/0x17 (light/medium/heavy) straight to
// sGSound__createPersistentVoiceRequest (0x1402A2A10) at 0x1402B1019. Their
// SRQR entries carry a bank group id >= 1000, which the structured allocator
// rejects, so ordinary pain voices never play. The PC executable's own
// selector (sGSound__selectRandomVoiceVariant, 0x1402A9110) maps those groups
// to concrete clips 0x18..0x20 or to silence; the queue just never calls it.
//
// This detour reproduces that mapping for the queue's call only. The global
// gameplay RNG is untouched. The queue runs in uGSoundProcess::moveAfter
// (sUnit lane 7), which resimulation replays, so online the roll is a hash of
// the frame, slot and group: a replayed pain voice picks the same clip as the
// live one and rollback_sound.cpp keeps it playing instead of restarting it
// with another clip. Offline it comes from a private generator.
// The silence outcome forwards the original group, which
// takes the native rejection path (disabled cookie, consumed bank ref reset).
// Dark Phoenix's direct cues 0x61..0x63 and the Dante/Vergil Devil Trigger
// overrides never reach this with 0x15..0x17 and are unaffected.
namespace {

constexpr uintptr_t FN_CREATE_PERSISTENT_VOICE = 0x1402A2A10;
constexpr uintptr_t RET_CHARACTER_SOUND_QUEUE = 0x1402B101E; // after the call at 0x1402B1019

using CreatePersistentVoiceFn = uint64_t* (__fastcall*)(void* dispatcher, uint64_t* outCookie, void* bankRef,
                                                        uint32_t cueId, uint64_t slotKey, void* character,
                                                        uint32_t mode, float gain, float extra);
CreatePersistentVoiceFn g_original = nullptr;

uint32_t g_rngState = 0;

uint32_t Roll100(uint32_t cue, uint64_t slotKey)
{
    if (Netplay_IsActive()) {
        const int32_t frame = g_isResimulating ? g_resimulateCurrentFrame : Controller_GetExecutingFrame();
        uint32_t h = 2166136261u;
        const uint32_t values[] = {(uint32_t)frame, (uint32_t)slotKey, cue};
        for (uint32_t v : values)
            h = (h ^ v) * 16777619u;
        h ^= h >> 15;
        h *= 0x2C1B3C6Du;
        h ^= h >> 12;
        return h % 100;
    }
    uint32_t x = g_rngState;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    g_rngState = x;
    return x % 100;
}

// Native normal-pain mapping from 0x1402A9110. 0 means no voice.
uint32_t ResolveNormalPainCue(uint32_t cue, uint32_t roll)
{
    switch (cue) {
    case 0x15:
        if (roll < 40) return 0;
        if (roll < 60) return 0x18;
        return roll < 80 ? 0x19 : 0x1A;
    case 0x16:
        if (roll < 30) return 0;
        if (roll < 60) return 0x1B;
        return roll < 80 ? 0x1C : 0x1D;
    case 0x17:
        if (roll < 20) return 0;
        if (roll < 50) return 0x1E;
        return roll < 80 ? 0x1F : 0x20;
    default:
        return cue;
    }
}

uint64_t* __fastcall hkCreatePersistentVoice(void* dispatcher, uint64_t* outCookie, void* bankRef, uint32_t cueId,
                                             uint64_t slotKey, void* character, uint32_t mode, float gain,
                                             float extra)
{
    if (cueId >= 0x15 && cueId <= 0x17 &&
        (uintptr_t)__builtin_return_address(0) == _addr(RET_CHARACTER_SOUND_QUEUE)) {
        const uint32_t resolved = ResolveNormalPainCue(cueId, Roll100(cueId, slotKey));
        if (resolved)
            cueId = resolved;
    }
    return g_original(dispatcher, outCookie, bankRef, cueId, slotKey, character, mode, gain, extra);
}

} // namespace

void PainVoice_Init()
{
    // Verify the queue's call still targets the allocator before relying on it.
    const uint8_t* call = (const uint8_t*)_addr(0x1402B1019);
    const bool callMatches = call[0] == 0xE8 &&
        (uintptr_t)(call + 5 + *(const int32_t*)(call + 1)) == _addr(FN_CREATE_PERSISTENT_VOICE);
    if (!callMatches) {
        HookLog("PainVoice: unexpected code at the damage voice queue call, not installed\n");
        return;
    }
    g_rngState = (uint32_t)GetTickCount64() | 1;
    const bool hooked = GameplayHooks_Create((LPVOID)_addr(FN_CREATE_PERSISTENT_VOICE),
                                             (LPVOID)&hkCreatePersistentVoice, (LPVOID*)&g_original) == MH_OK &&
                        GameplayHooks_QueueEnable((LPVOID)_addr(FN_CREATE_PERSISTENT_VOICE)) == MH_OK;
    if (!hooked)
        HookLog("PainVoice: failed to hook createPersistentVoiceRequest\n");
}
