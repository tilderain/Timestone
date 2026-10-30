//Disable rng for objects we don't restore, stage objects
//Disable character sound rng (for now)

#include "../pch.h"

#include "rollback.h"
#include "rollback_memory.h"
#include "rollback_rng.h"
#include "mk10utils.h"
#include "netplay.h"
#include "gameplay_hooks.h"

#include <cstring>

extern void HookLog(const char* fmt, ...);

static constexpr uintptr_t FN_RNG_RANDOM_INT = 0x14014EB30;
static constexpr uintptr_t FN_RNG_ADVANCE = 0x14014EAB0;
static constexpr uintptr_t FN_RNG_LERP_RANDOM = 0x14014EA70;
static constexpr uintptr_t FN_RNG_NEXT_FLOAT = 0x14014EE10;
static constexpr uintptr_t FN_RNG_FLOAT_IN_RANGE = 0x140455550;
static constexpr uintptr_t FN_RNG_INT_IN_RANGE = 0x140455590;
static constexpr uintptr_t FN_RNG_NEXT = 0x14000AF80;
static constexpr uintptr_t FN_RNG_NEXT_IMPL = 0x1404600F0;
static constexpr uintptr_t FN_EFFECT_NEXT_RANDOM = 0x140543B70;
// Stage object classes, their generic FSM helpers, and stage implementations
// live in this image range. Their random results are local presentation state
// and must never advance the shared gameplay stream.
static constexpr uintptr_t STAGE_OBJECT_CODE_BEGIN = 0x1401CFC20;
static constexpr uintptr_t STAGE_OBJECT_CODE_END = 0x140200000;
static constexpr uintptr_t EFFECT_RNG_CALLSITE_A = 0x14080D423;
static constexpr uintptr_t EFFECT_RNG_CALLSITE_B = 0x14080D4A0;
static constexpr uintptr_t SOUND_RNG_CALLSITE_A = 0x1405D4C47;
static constexpr uintptr_t SOUND_RNG_CALLSITE_B = 0x1405D4CBB;
// sGSound::queueVoice5 selects a voice sampler variant directly with the
// global stream; this path does not go through sGSound__selectRandomVoiceVariant.
static constexpr uintptr_t SOUND_VOICE_QUEUE_RNG_CALLSITE = 0x1402AE35A;
// sCamera::jitterStep uses the shared stream for screen shake when its
// logistic-map mode is disabled. Jitter is presentation-only.
static constexpr uintptr_t CAMERA_JITTER_CALLSITE_A = 0x14092E29D;
static constexpr uintptr_t CAMERA_JITTER_CALLSITE_B = 0x14092E2E7;
static constexpr uintptr_t PARTICLE_RNG_CALLSITES[] = {
    0x140550D7B, 0x140550DA2, 0x140550DCE, 0x140550F4B,
    0x140550F5B, 0x140550F92, 0x1405513E2, 0x1407C1789,
};
static constexpr uintptr_t EFFECT_NOISE_RNG_CALLSITES[] = {
    0x14062FE19, 0x14062FE25, 0x14062FE65, 0x14062FE90,
    0x14062FEBD, 0x14062FF4E, 0x14062FF5A, 0x14062FF6F,
    0x14062FFAF,
};
static constexpr uintptr_t CLOUD_RNG_CALLSITES[] = {
    0x14070BFCB, 0x14070BFF2, 0x14070C01E, 0x14070C1A1,
    0x14070C1B1, 0x14070C1E8, 0x14070C642, 0x14076C6BE,
    0x14076CC76, 0x14076CC97, 0x14076CCC3, 0x14076CD75,
    0x14076CD9C, 0x14076CE3B,
};
static constexpr uintptr_t SHADOW_RNG_CALLSITE = 0x14067584B;
static constexpr uintptr_t JIGENZAN_EFFECT_RNG_CALLSITES[] = {
    0x1401C03B0, 0x1401C03D3,
    0x1401C054E, 0x1401C0571,
    0x1401C06FA, 0x1401C071D,
};
static constexpr uintptr_t FN_SOUND_RANDOM_VARIANT = 0x1402A9110;
static constexpr uintptr_t FN_HSIENKO_SELECT_ANKI = 0x1400A1260;
static constexpr uintptr_t FN_SET_RESERVED_ANKI = 0x1400D4D90;
static constexpr uintptr_t FN_RCHRCPU_RNG_MOD_N = 0x14010FE50;
static constexpr uintptr_t GLOBAL_RNG_STATE = 0x140D765D8;

using RandomIntRangeFn = int(__fastcall*)(void*, int, int, void*);
using RngAdvanceFn = float(__fastcall*)(uint64_t, void*);
using RngLerpRandomFn = float(__fastcall*)(uint64_t, float, float, void*);
using RngNextFloatFn = float(__fastcall*)(uint32_t*);
using RngFloatInRangeFn = float(__fastcall*)(float, float);
using RngIntInRangeFn = int(__fastcall*)(int, int);
using RngNextFn = void(__fastcall*)(uint32_t*);
using EffectNextRandomFn = uint64_t(__fastcall*)(void*, int);
using SoundRandomVariantFn = int(__fastcall*)(uint64_t, void*, int);
using HsienKoSelectAnkiFn = int(__fastcall*)(void*);
using SetReservedAnkiFn = void(__fastcall*)(void*, int);
using RChrCpuRngModNFn = uint32_t(__fastcall*)(void*, uint32_t);

static RandomIntRangeFn oRandomIntRange = nullptr;
static RngAdvanceFn oRngAdvance = nullptr;
static RngLerpRandomFn oRngLerpRandom = nullptr;
static RngNextFloatFn oRngNextFloat = nullptr;
static RngFloatInRangeFn oRngFloatInRange = nullptr;
static RngIntInRangeFn oRngIntInRange = nullptr;
static RngNextFn oRngNext = nullptr;
static RngNextFn oRngNextImpl = nullptr;
static EffectNextRandomFn oEffectNextRandom = nullptr;
static SoundRandomVariantFn oSoundRandomVariant = nullptr;
static HsienKoSelectAnkiFn oHsienKoSelectAnki = nullptr;
static SetReservedAnkiFn oSetReservedAnki = nullptr;
static RChrCpuRngModNFn oRChrCpuRngModN = nullptr;
static int g_rngTraceFrame = -1;
static int g_rngTraceCount = 0;
static int g_ankiTraceFrame = -1;
static int g_ankiTraceCount = 0;
static int g_ankiReserveTraceFrame = -1;
static int g_ankiReserveTraceCount = 0;

static int RngTraceFrame();

static bool IsStageObjectCallsite(uint64_t caller)
{
    const uint64_t address = caller;
    return address >= _addr(STAGE_OBJECT_CODE_BEGIN) &&
           address < _addr(STAGE_OBJECT_CODE_END);
}

static bool IsPresentationRngCallsite(uint64_t caller)
{
    if (caller == _addr(EFFECT_RNG_CALLSITE_A) ||
           caller == _addr(EFFECT_RNG_CALLSITE_B) ||
           caller == _addr(SOUND_RNG_CALLSITE_A) ||
           caller == _addr(SOUND_RNG_CALLSITE_B) ||
           caller == _addr(SOUND_VOICE_QUEUE_RNG_CALLSITE) ||
           caller == _addr(CAMERA_JITTER_CALLSITE_A) ||
           caller == _addr(CAMERA_JITTER_CALLSITE_B) ||
           caller == _addr(SHADOW_RNG_CALLSITE)) {
        return true;
    }
    for (uintptr_t address : PARTICLE_RNG_CALLSITES)
        if (caller == _addr(address)) return true;
    for (uintptr_t address : EFFECT_NOISE_RNG_CALLSITES)
        if (caller == _addr(address)) return true;
    for (uintptr_t address : CLOUD_RNG_CALLSITES)
        if (caller == _addr(address)) return true;
    for (uintptr_t address : JIGENZAN_EFFECT_RNG_CALLSITES)
        if (caller == _addr(address)) return true;
    return false;
}

static uint64_t __fastcall hkEffectNextRandom(void* effect, int useEffectRng)
{
    // The native zero path advances the shared gameplay stream. Effects have
    // their own state at sEffect+0x3C, so always draw from that local stream.
    return oEffectNextRandom(effect, GameplayHooks_IsActive() ? 1 : useEffectRng);
}

static int __fastcall hkSoundRandomVariant(uint64_t soundContext, void* soundRef,
                                           int variant)
{
    if (!Netplay_IsActive())
        return oSoundRandomVariant(soundContext, soundRef, variant);

    // FUN_1402A9110 chooses voice/sound variants with the global XorShift
    // stream. Audio presentation must not consume gameplay RNG on one peer.
    uint32_t savedState[4] = {};
    std::memcpy(savedState, (void*)_addr(GLOBAL_RNG_STATE), sizeof(savedState));
    int result = oSoundRandomVariant(soundContext, soundRef, variant);
    std::memcpy((void*)_addr(GLOBAL_RNG_STATE), savedState, sizeof(savedState));
    return result;
}

static void __fastcall hkRngNextCommon(uint32_t* state, RngNextFn original,
                                       uint64_t caller)
{
    if (!Netplay_IsActive()) {
        original(state);
        return;
    }

    const bool isolateGlobalRng = state == (uint32_t*)_addr(GLOBAL_RNG_STATE) &&
        (IsStageObjectCallsite(caller) || IsPresentationRngCallsite(caller));
    uint32_t savedGlobal[4] = {};
    if (isolateGlobalRng)
        std::memcpy(savedGlobal, state, sizeof(savedGlobal));

    original(state);

    if (isolateGlobalRng) {
        std::memcpy(state, savedGlobal, sizeof(savedGlobal));
        //HookLog("PRESENTRNG frame=%d resim=%d caller=0x%llX state=preserved\n",
        //        frame, g_isResimulating ? 1 : 0, caller);
    }

}

static void __fastcall hkRngNext(uint32_t* state)
{
    hkRngNextCommon(state, oRngNext,
                    (uint64_t)__builtin_return_address(0));
}

static void __fastcall hkRngNextImpl(uint32_t* state)
{
    hkRngNextCommon(state, oRngNextImpl,
                    (uint64_t)__builtin_return_address(0));
}

static float __fastcall hkRngNextFloat(uint32_t* state)
{
    const uint64_t caller = (uint64_t)__builtin_return_address(0);
    const bool isolate = Netplay_IsActive() &&
        state == (uint32_t*)_addr(GLOBAL_RNG_STATE) &&
        IsStageObjectCallsite(caller);
    uint32_t savedGlobal[4] = {};
    if (isolate)
        std::memcpy(savedGlobal, state, sizeof(savedGlobal));

    const float result = oRngNextFloat(state);
    if (isolate)
        std::memcpy(state, savedGlobal, sizeof(savedGlobal));
    return result;
}

static float __fastcall hkRngFloatInRange(float low, float high)
{
    const uint64_t caller = (uint64_t)__builtin_return_address(0);
    const bool isolate = Netplay_IsActive() &&
        IsStageObjectCallsite(caller);
    uint32_t savedGlobal[4] = {};
    if (isolate)
        std::memcpy(savedGlobal, (void*)_addr(GLOBAL_RNG_STATE),
                    sizeof(savedGlobal));

    const float result = oRngFloatInRange(low, high);
    if (isolate)
        std::memcpy((void*)_addr(GLOBAL_RNG_STATE), savedGlobal,
                    sizeof(savedGlobal));
    return result;
}

static int __fastcall hkRngIntInRange(int low, int high)
{
    const uint64_t caller = (uint64_t)__builtin_return_address(0);
    const bool isolate = Netplay_IsActive() &&
        IsStageObjectCallsite(caller);
    uint32_t savedGlobal[4] = {};
    if (isolate)
        std::memcpy(savedGlobal, (void*)_addr(GLOBAL_RNG_STATE),
                    sizeof(savedGlobal));

    const int result = oRngIntInRange(low, high);
    if (isolate)
        std::memcpy((void*)_addr(GLOBAL_RNG_STATE), savedGlobal,
                    sizeof(savedGlobal));
    return result;
}

static float __fastcall hkRngAdvance(uint64_t stateArg, void* rngOwner)
{
    if (!Netplay_IsActive())
        return oRngAdvance(stateArg, rngOwner);

    uint32_t globalBefore[4] = {};
    uint32_t globalAfter[4] = {};
    std::memcpy(globalBefore, (void*)_addr(GLOBAL_RNG_STATE), sizeof(globalBefore));
    const uint64_t caller = (uint64_t)__builtin_return_address(0);
    const bool presentationCaller =
        IsStageObjectCallsite(caller) || IsPresentationRngCallsite(caller);
    float result = oRngAdvance(stateArg, rngOwner);
    std::memcpy(globalAfter, (void*)_addr(GLOBAL_RNG_STATE), sizeof(globalAfter));
    const bool usedGlobalRng =
        std::memcmp(globalBefore, globalAfter, sizeof(globalBefore)) != 0;

    if (presentationCaller && usedGlobalRng) {
        std::memcpy((void*)_addr(GLOBAL_RNG_STATE), globalBefore,
                    sizeof(globalBefore));
        //HookLog("PRESENTRNGADV frame=%d resim=%d caller=0x%llX state=preserved\n",
        //        frame, g_isResimulating ? 1 : 0, caller);
    }
    return result;
}

static float __fastcall hkRngLerpRandom(uint64_t context, float low, float high,
                                        void* rngOwner)
{
    if (!Netplay_IsActive())
        return oRngLerpRandom(context, low, high, rngOwner);

    const uint64_t caller = (uint64_t)__builtin_return_address(0);
    uint32_t before[4] = {};
    std::memcpy(before, (void*)_addr(GLOBAL_RNG_STATE), sizeof(before));
    const float result = oRngLerpRandom(context, low, high, rngOwner);
    uint32_t after[4] = {};
    std::memcpy(after, (void*)_addr(GLOBAL_RNG_STATE), sizeof(after));
    const bool usedGlobalRng =
        std::memcmp(before, after, sizeof(before)) != 0;

    if (usedGlobalRng && IsPresentationRngCallsite(caller))
        std::memcpy((void*)_addr(GLOBAL_RNG_STATE), before, sizeof(before));

    return result;
}

static uint32_t __fastcall hkRChrCpuRngModN(void* cpu, uint32_t modulus)
{
    if (!Netplay_IsActive())
        return oRChrCpuRngModN(cpu, modulus);

    return rand() % modulus;
    uint32_t savedState[4];
    void* globalState = (void*)_addr(GLOBAL_RNG_STATE);
    std::memcpy(savedState, globalState, sizeof(savedState));
    uint32_t result = oRChrCpuRngModN(cpu, modulus);
    std::memcpy(globalState, savedState, sizeof(savedState));
    return result;
}

static int RngTraceFrame()
{
    uint64_t replay = *(uint64_t*)_addr(0x140d4d9d8);
    return replay ? *(int32_t*)(replay + 0x54) : -1;
}

static bool IsAnkihouRngCaller(uint64_t caller, void* rngOwner)
{
    if (rngOwner && GetObjectDti((uint64_t)rngOwner) == (uint64_t)_addr(0x140D455F8))
        return true;
    return caller == (uint64_t)_addr(0x1400A1289) ||
           caller == (uint64_t)_addr(0x14018BAF1) ||
           caller == (uint64_t)_addr(0x14018B5C2) ||
           caller == (uint64_t)_addr(0x14018D245 + 5) ||
           caller == (uint64_t)_addr(0x14018D299 + 5);
}

static int __fastcall hkRandomIntRange(void* stateArg, int low, int high,
                                       void* rngOwner)
{
    uint64_t caller = (uint64_t)__builtin_return_address(0);
    if (!IsAnkihouRngCaller(caller, rngOwner))
        return oRandomIntRange(stateArg, low, high, rngOwner);

    int frame = RngTraceFrame();
    if (frame != g_rngTraceFrame) {
        g_rngTraceFrame = frame;
        g_rngTraceCount = 0;
    }

    uint32_t globalBefore[4] = {};
    uint32_t ownerBefore[4] = {};
    std::memcpy(globalBefore, (void*)_addr(0x140D765D8), sizeof(globalBefore));
    if (rngOwner)
        std::memcpy(ownerBefore, (uint8_t*)rngOwner + 0x2D80, sizeof(ownerBefore));

    int result = oRandomIntRange(stateArg, low, high, rngOwner);

    uint32_t globalAfter[4] = {};
    uint32_t ownerAfter[4] = {};
    std::memcpy(globalAfter, (void*)_addr(0x140D765D8), sizeof(globalAfter));
    if (rngOwner)
        std::memcpy(ownerAfter, (uint8_t*)rngOwner + 0x2D80, sizeof(ownerAfter));

    if (g_rngTraceCount++ < 32) {
        /*HookLog("RNGTRACE frame=%d resim=%d caller=0x%llX stateArg=0x%llX owner=0x%llX "
                "range=[%d,%d] result=%d global=%08X,%08X,%08X,%08X->%08X,%08X,%08X,%08X "
                "ownerRng(+0x2D80)=%08X,%08X,%08X,%08X->%08X,%08X,%08X,%08X\n",
                frame, g_isResimulating ? 1 : 0, caller, (uint64_t)stateArg,
                (uint64_t)rngOwner, low, high, result,
                globalBefore[0], globalBefore[1], globalBefore[2], globalBefore[3],
                globalAfter[0], globalAfter[1], globalAfter[2], globalAfter[3],
                ownerBefore[0], ownerBefore[1], ownerBefore[2], ownerBefore[3],
                ownerAfter[0], ownerAfter[1], ownerAfter[2], ownerAfter[3]);*/
    }
    return result;
}

static int __fastcall hkHsienKoSelectAnki(void* character)
{
    if (!character)
        return oHsienKoSelectAnki(character);

    int frame = RngTraceFrame();
    if (frame != g_ankiTraceFrame) {
        g_ankiTraceFrame = frame;
        g_ankiTraceCount = 0;
    }

    uint32_t reservedBefore = *(uint32_t*)((uint8_t*)character + 0x68FC);
    uint32_t commandBefore = *(uint32_t*)((uint8_t*)character + 0x13C4);
    uint32_t actionId = *(uint32_t*)((uint8_t*)character + 0x1310);
    uint32_t rngBefore[4] = {};
    std::memcpy(rngBefore, (uint8_t*)character + 0x2D80, sizeof(rngBefore));

    int result = oHsienKoSelectAnki(character);

    uint32_t rngAfter[4] = {};
    std::memcpy(rngAfter, (uint8_t*)character + 0x2D80, sizeof(rngAfter));
    uint64_t caller = (uint64_t)__builtin_return_address(0);

    if (g_ankiTraceCount++ < 64) {
        /*HookLog("ANKITRACE frame=%d resim=%d caller=0x%llX char=0x%llX action=0x%08X "
                "reserved=%d commandBefore(+0x13C4)=%08X callbackResult=%d "
                "rng(+0x2D80)=%08X,%08X,%08X,%08X->%08X,%08X,%08X,%08X\n",
                frame, g_isResimulating ? 1 : 0, caller, (uint64_t)character,
                actionId, (int32_t)reservedBefore, commandBefore, result,
                rngBefore[0], rngBefore[1], rngBefore[2], rngBefore[3],
                rngAfter[0], rngAfter[1], rngAfter[2], rngAfter[3]);*/
    }
    return result;
}

static void __fastcall hkSetReservedAnki(void* character, int value)
{
    if (!character)
        return oSetReservedAnki(character, value);

    int frame = RngTraceFrame();
    if (frame != g_ankiReserveTraceFrame) {
        g_ankiReserveTraceFrame = frame;
        g_ankiReserveTraceCount = 0;
    }

    uint32_t before = *(uint32_t*)((uint8_t*)character + 0x68FC);
    uint32_t actionId = *(uint32_t*)((uint8_t*)character + 0x1310);
    uint64_t caller = (uint64_t)__builtin_return_address(0);
    oSetReservedAnki(character, value);

    if (g_ankiReserveTraceCount++ < 32) {
        /*HookLog("ANKIRESERVE frame=%d resim=%d caller=0x%llX object=0x%llX "
                "action=0x%08X before=%d after=%d\n",
                frame, g_isResimulating ? 1 : 0, caller, (uint64_t)character,
                actionId, (int32_t)before, value);*/
    }
}

void Rollback_InitRngHooks()
{
    MH_STATUS status = GameplayHooks_Create((LPVOID)_addr(FN_EFFECT_NEXT_RANDOM),
                                     (LPVOID)&hkEffectNextRandom,
                                     (LPVOID*)&oEffectNextRandom);
    if (status == MH_OK) {
        status = MH_EnableHook((LPVOID)_addr(FN_EFFECT_NEXT_RANDOM));
        if (status == MH_OK)
            HookLog("Rollback: forced effects onto local RNG at 0x%llX\n",
                    (uint64_t)_addr(FN_EFFECT_NEXT_RANDOM));
        else
            HookLog("Rollback: failed to enable effect RNG isolation (%d)\n", status);
    } else {
        HookLog("Rollback: failed to hook effect RNG isolation (%d)\n", status);
    }

    status = GameplayHooks_Create((LPVOID)_addr(FN_SOUND_RANDOM_VARIANT),
                           (LPVOID)&hkSoundRandomVariant,
                           (LPVOID*)&oSoundRandomVariant);
    if (status == MH_OK) {
        status = MH_EnableHook((LPVOID)_addr(FN_SOUND_RANDOM_VARIANT));
        if (status == MH_OK)
            HookLog("Rollback: isolated sound variant RNG at 0x%llX\n",
                    (uint64_t)_addr(FN_SOUND_RANDOM_VARIANT));
        else
            HookLog("Rollback: failed to enable sound variant RNG isolation (%d)\n", status);
    } else {
        HookLog("Rollback: failed to hook sound variant RNG (%d)\n", status);
    }

    status = GameplayHooks_Create((LPVOID)_addr(FN_RNG_NEXT),
                                      (LPVOID)&hkRngNext,
                                      (LPVOID*)&oRngNext);
    if (status == MH_OK) {
        status = MH_EnableHook((LPVOID)_addr(FN_RNG_NEXT));
        if (status == MH_OK)
            HookLog("Rollback: hooked MtXorShift next-state trace at 0x%llX\n",
                    (uint64_t)_addr(FN_RNG_NEXT));
        else
            HookLog("Rollback: failed to enable next-state RNG trace (%d)\n", status);
    } else {
        HookLog("Rollback: failed to hook next-state RNG trace (%d)\n", status);
    }

    status = GameplayHooks_Create((LPVOID)_addr(FN_RNG_NEXT_IMPL),
                           (LPVOID)&hkRngNextImpl,
                           (LPVOID*)&oRngNextImpl);
    if (status == MH_OK) {
        status = MH_EnableHook((LPVOID)_addr(FN_RNG_NEXT_IMPL));
        if (status == MH_OK)
            HookLog("Rollback: hooked MtXorShift implementation trace at 0x%llX\n",
                    (uint64_t)_addr(FN_RNG_NEXT_IMPL));
        else
            HookLog("Rollback: failed to enable implementation RNG trace (%d)\n", status);
    } else {
        HookLog("Rollback: failed to hook implementation RNG trace (%d)\n", status);
    }

    status = GameplayHooks_Create((LPVOID)_addr(FN_RNG_NEXT_FLOAT),
                           (LPVOID)&hkRngNextFloat,
                           (LPVOID*)&oRngNextFloat);
    if (status == MH_OK) {
        status = MH_EnableHook((LPVOID)_addr(FN_RNG_NEXT_FLOAT));
        if (status == MH_OK)
            HookLog("Rollback: isolated stage RNG float consumers\n");
        else
            HookLog("Rollback: failed to enable stage RNG float isolation (%d)\n", status);
    } else {
        HookLog("Rollback: failed to hook stage RNG float consumers (%d)\n", status);
    }

    status = GameplayHooks_Create((LPVOID)_addr(FN_RNG_FLOAT_IN_RANGE),
                           (LPVOID)&hkRngFloatInRange,
                           (LPVOID*)&oRngFloatInRange);
    if (status == MH_OK) {
        status = MH_EnableHook((LPVOID)_addr(FN_RNG_FLOAT_IN_RANGE));
        if (status == MH_OK)
            HookLog("Rollback: isolated stage float ranges\n");
        else
            HookLog("Rollback: failed to enable stage float-range isolation (%d)\n", status);
    } else {
        HookLog("Rollback: failed to hook stage float ranges (%d)\n", status);
    }

    status = GameplayHooks_Create((LPVOID)_addr(FN_RNG_INT_IN_RANGE),
                           (LPVOID)&hkRngIntInRange,
                           (LPVOID*)&oRngIntInRange);
    if (status == MH_OK) {
        status = MH_EnableHook((LPVOID)_addr(FN_RNG_INT_IN_RANGE));
        if (status == MH_OK)
            HookLog("Rollback: isolated stage integer ranges\n");
        else
            HookLog("Rollback: failed to enable stage integer-range isolation (%d)\n", status);
    } else {
        HookLog("Rollback: failed to hook stage integer ranges (%d)\n", status);
    }

    status = GameplayHooks_Create((LPVOID)_addr(FN_RNG_ADVANCE),
                                     (LPVOID)&hkRngAdvance,
                                     (LPVOID*)&oRngAdvance);
    if (status == MH_OK) {
        status = MH_EnableHook((LPVOID)_addr(FN_RNG_ADVANCE));
        if (status == MH_OK)
            HookLog("Rollback: hooked shared MtXorShift advancement trace at 0x%llX\n",
                    (uint64_t)_addr(FN_RNG_ADVANCE));
        else
            HookLog("Rollback: failed to enable shared RNG trace (%d)\n", status);
    } else {
        HookLog("Rollback: failed to hook shared RNG trace (%d)\n", status);
    }

    status = GameplayHooks_Create((LPVOID)_addr(FN_RNG_LERP_RANDOM),
                           (LPVOID)&hkRngLerpRandom,
                           (LPVOID*)&oRngLerpRandom);
    if (status == MH_OK) {
        status = MH_EnableHook((LPVOID)_addr(FN_RNG_LERP_RANDOM));
        if (status == MH_OK)
            HookLog("Rollback: isolated presentation lerp RNG at 0x%llX\n",
                    (uint64_t)_addr(FN_RNG_LERP_RANDOM));
        else
            HookLog("Rollback: failed to enable lerp RNG isolation (%d)\n", status);
    } else {
        HookLog("Rollback: failed to hook lerp RNG isolation (%d)\n", status);
    }

    status = GameplayHooks_Create((LPVOID)_addr(FN_RCHRCPU_RNG_MOD_N),
                                      (LPVOID)&hkRChrCpuRngModN,
                                      (LPVOID*)&oRChrCpuRngModN);
    if (status == MH_OK) {
        status = MH_EnableHook((LPVOID)_addr(FN_RCHRCPU_RNG_MOD_N));
        if (status == MH_OK)
            HookLog("Rollback: disabled CPU global RNG advancement during netplay\n");
        else
            HookLog("Rollback: failed to enable CPU RNG hook (%d)\n", status);
    } else {
        HookLog("Rollback: failed to hook CPU RNG dispatch (%d)\n", status);
    }

    status = GameplayHooks_Create((LPVOID)_addr(FN_RNG_RANDOM_INT),
                                     (LPVOID)&hkRandomIntRange,
                                     (LPVOID*)&oRandomIntRange);
    if (status == MH_OK) {
        // Intentionally left disabled; this hook is diagnostic-only.
        HookLog("Rollback: hooked Ankihou RNG trace at 0x%llX\n",
                (uint64_t)_addr(FN_RNG_RANDOM_INT));
    } else {
        HookLog("Rollback: failed to hook Ankihou RNG trace (%d)\n", status);
    }

    status = GameplayHooks_Create((LPVOID)_addr(FN_HSIENKO_SELECT_ANKI),
                           (LPVOID)&hkHsienKoSelectAnki,
                           (LPVOID*)&oHsienKoSelectAnki);
    if (status == MH_OK) {
        // Intentionally left disabled; this hook is diagnostic-only.
        HookLog("Rollback: hooked Hsien-Ko Anki selector trace at 0x%llX\n",
                (uint64_t)_addr(FN_HSIENKO_SELECT_ANKI));
    } else {
        HookLog("Rollback: failed to hook Hsien-Ko Anki selector trace (%d)\n", status);
    }

    status = GameplayHooks_Create((LPVOID)_addr(FN_SET_RESERVED_ANKI),
                           (LPVOID)&hkSetReservedAnki,
                           (LPVOID*)&oSetReservedAnki);
    if (status == MH_OK) {
        // Intentionally left disabled; this hook is diagnostic-only.
        HookLog("Rollback: hooked Hsien-Ko Anki reserve trace at 0x%llX\n",
                (uint64_t)_addr(FN_SET_RESERVED_ANKI));
    } else {
        HookLog("Rollback: failed to hook Hsien-Ko Anki reserve trace (%d)\n", status);
    }
}
