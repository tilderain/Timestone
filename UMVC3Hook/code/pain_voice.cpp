#include "../pch.h"

#include "rollback.h"
#include "mk10utils.h"
#include "gameplay_hooks.h"

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
// This detour runs that native selector for the queue's call only, so pain
// clips are picked like every other character voice variant. Online the
// selector draws from a per-frame stream (rollback_rng.cpp
// hkSoundRandomVariant), so both peers and a resim pick the same clip, and
// rollback_sound.cpp keeps a replayed pain voice playing.
// The silence outcome (0) forwards the original group, which
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

constexpr uintptr_t FN_SELECT_RANDOM_VOICE_VARIANT = 0x1402A9110;
constexpr uintptr_t FN_CREF_INIT = 0x140003990;
constexpr uint64_t GSOUND_VOICE_DISPATCHER = 0x29B0;

uint64_t* __fastcall hkCreatePersistentVoice(void* dispatcher, uint64_t* outCookie, void* bankRef, uint32_t cueId,
                                             uint64_t slotKey, void* character, uint32_t mode, float gain,
                                             float extra)
{
    if (cueId >= 0x15 && cueId <= 0x17 &&
        (uintptr_t)__builtin_return_address(0) == _addr(RET_CHARACTER_SOUND_QUEUE)) {
        // The selector consumes (resets) the ref it is given, so hand it a copy.
        uint64_t ref[2];
        ((void*(__fastcall*)(void*, void*))_addr(FN_CREF_INIT))(ref, bankRef);
        const uint64_t gsound = (uint64_t)dispatcher - GSOUND_VOICE_DISPATCHER;
        const int resolved =
            ((int(__fastcall*)(uint64_t, void*, int))_addr(FN_SELECT_RANDOM_VOICE_VARIANT))(gsound, ref, (int)cueId);
        if (resolved > 0)
            cueId = (uint32_t)resolved;
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
    const bool hooked = GameplayHooks_Create((LPVOID)_addr(FN_CREATE_PERSISTENT_VOICE),
                                             (LPVOID)&hkCreatePersistentVoice, (LPVOID*)&g_original) == MH_OK &&
                        GameplayHooks_QueueEnable((LPVOID)_addr(FN_CREATE_PERSISTENT_VOICE)) == MH_OK;
    if (!hooked)
        HookLog("PainVoice: failed to hook createPersistentVoiceRequest\n");
}
