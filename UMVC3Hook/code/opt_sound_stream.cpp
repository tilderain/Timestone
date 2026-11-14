#include "../pch.h"

#include "rollback.h"
#include "mk10utils.h"
#include "gameplay_hooks.h"
#include <atomic>
#include <x86intrin.h>

extern void HookLog(const char* fmt, ...);

// rSoundStreamRequest::freeStreamingSource (0x1406897B0) cost 3% of the game
// thread, about 0.25 ms per frame, almost all of it wasted. Eight times a frame
// it takes every stream source, scans every element for one still playing from
// it, and releases the source's resource if there is none. It does that scan
// even for a source with no resource left to release, which is nearly all of
// them: about 62 sources and 79 elements per call, and one held resource in
// every five calls.
//
// This replacement skips sources whose resource is already null. The release
// is the function's only effect and sSound::getStreamStatus (0x1405C97C0) only
// reads a table, so the result is the same. Measured 2026-11-13: about 123,000
// ticks per call native, 2,400 here. It never runs during resimulation.
//
// Another thread can hand a source its resource during a call. Such a source
// is picked up on the next call; nothing is released that native would keep.
//
// Shadow mode predicts the releases, runs native and compares. Over 300,000
// calls the only differences were sources given a resource by another thread.
namespace {

enum class Mode { Off, Shadow, Replace };
constexpr Mode MODE = Mode::Replace;

// rSoundStreamRequest
constexpr uint64_t REQUEST_ELEMENT_NUM = 0x78, REQUEST_ELEMENTS = 0x80;
constexpr uint64_t REQUEST_SOURCES = 0xB0, REQUEST_SOURCE_NUM = 0xB8;
// element (0x98 bytes) and source (0x40 bytes) records
constexpr uint64_t ELEMENT_SIZE = 0x98, ELEMENT_REQUEST_NO = 0x0, ELEMENT_KEEP_WHEN_ZERO = 0xC, ELEMENT_SOURCE = 0x90;
constexpr uint64_t SOURCE_SIZE = 0x40, SOURCE_RESOURCE = 0x38;

template <typename T> T& At(uint64_t base, uint64_t offset) { return *(T*)(base + offset); }

void (__fastcall* g_original)(uint64_t request) = nullptr;

bool SourceInUse(uint64_t request, uint64_t source)
{
    const auto streamStatus = (int (__fastcall*)(uint64_t, uint64_t, uint32_t, int64_t))_addr(0x1405C97C0);
    for (uint32_t j = 0; j < At<uint32_t>(request, REQUEST_ELEMENT_NUM); ++j) {
        const uint64_t element = At<uint64_t>(request, REQUEST_ELEMENTS) + j * ELEMENT_SIZE;
        if (At<uint64_t>(element, ELEMENT_SOURCE) != source)
            continue;
        if (streamStatus(*(uint64_t*)_addr(0x140E18520), request, At<uint16_t>(element, ELEMENT_REQUEST_NO), -1) != 0 ||
            At<int32_t>(element, ELEMENT_KEEP_WHEN_ZERO) == 0)
            return true;
    }
    return false;
}

void FreeUnusedSources(uint64_t request)
{
    if (!At<uint64_t>(request, REQUEST_SOURCES) || !At<uint64_t>(request, REQUEST_ELEMENTS))
        return;
    for (uint32_t i = 0; i < At<uint32_t>(request, REQUEST_SOURCE_NUM); ++i) {
        const uint64_t source = At<uint64_t>(request, REQUEST_SOURCES) + i * SOURCE_SIZE;
        uint64_t& resource = At<uint64_t>(source, SOURCE_RESOURCE);
        if (!resource || SourceInUse(request, source))
            continue;
        ((void (__fastcall*)(uint64_t))_addr(0x14050D5A0))(resource); // cResource::releaseRef
        resource = 0;
    }
}

// Shadow mode only.
constexpr uint32_t SHADOW_MAX_SOURCES = 4096;
std::atomic<uint64_t> g_calls, g_resimCalls, g_sources, g_elements, g_liveSources, g_released, g_mismatches;
std::atomic<uint64_t> g_nativeTicks, g_scanTicks, g_lastReport;

void Shadow(uint64_t request)
{
    const uint32_t sourceNum = At<uint32_t>(request, REQUEST_SOURCE_NUM);
    const uint64_t sources = At<uint64_t>(request, REQUEST_SOURCES);
    if (!sources || !At<uint64_t>(request, REQUEST_ELEMENTS) || sourceNum > SHADOW_MAX_SOURCES) {
        g_original(request);
        return;
    }
    // Bit i: source i should still hold its resource after the call.
    uint64_t keeps[SHADOW_MAX_SOURCES / 64] = {};
    uint64_t lives[SHADOW_MAX_SOURCES / 64] = {}; // held a resource before the call
    uint32_t live = 0, released = 0;
    const uint64_t start = __rdtsc();
    for (uint32_t i = 0; i < sourceNum; ++i) {
        const uint64_t source = sources + i * SOURCE_SIZE;
        if (!At<uint64_t>(source, SOURCE_RESOURCE))
            continue;
        ++live;
        lives[i / 64] |= 1ull << (i % 64);
        if (SourceInUse(request, source))
            keeps[i / 64] |= 1ull << (i % 64);
        else
            ++released;
    }
    const uint64_t scanned = __rdtsc();
    g_original(request);
    const uint64_t done = __rdtsc();

    for (uint32_t i = 0; i < sourceNum; ++i) {
        if (!((lives[i / 64] >> (i % 64)) & 1))
            continue;
        const bool predicted = (keeps[i / 64] >> (i % 64)) & 1;
        const bool kept = At<uint64_t>(sources + i * SOURCE_SIZE, SOURCE_RESOURCE) != 0;
        if (kept != predicted && g_mismatches.fetch_add(1) < 16)
            HookLog("SoundStream: MISMATCH request=%p source=%u/%u predicted_keep=%d native_kept=%d in_use_now=%d\n",
                    (void*)request, i, sourceNum, predicted, kept,
                    SourceInUse(request, sources + i * SOURCE_SIZE));
    }

    g_sources += sourceNum;
    g_elements += At<uint32_t>(request, REQUEST_ELEMENT_NUM);
    g_liveSources += live;
    g_released += released;
    g_scanTicks += scanned - start;
    g_nativeTicks += done - scanned;
    if (g_isResimulating)
        ++g_resimCalls;
    const uint64_t calls = ++g_calls;

    const uint64_t now = GetTickCount64();
    uint64_t last = g_lastReport.load();
    if (now - last >= 10000 && g_lastReport.compare_exchange_strong(last, now)) {
        HookLog("SoundStream: calls=%llu (resim %llu) sources/call=%.1f live/call=%.2f elements/call=%.1f "
                "released=%llu native=%llu ticks/call scan=%llu ticks/call mismatch=%llu\n",
                (unsigned long long)calls, (unsigned long long)g_resimCalls.load(),
                (double)g_sources.load() / calls, (double)g_liveSources.load() / calls,
                (double)g_elements.load() / calls, (unsigned long long)g_released.load(),
                (unsigned long long)(g_nativeTicks.load() / calls), (unsigned long long)(g_scanTicks.load() / calls),
                (unsigned long long)g_mismatches.load());
    }
}

void __fastcall hkFreeStreamingSource(uint64_t request)
{
    if (MODE == Mode::Replace)
        FreeUnusedSources(request);
    else
        Shadow(request);
}

} // namespace

void OptSoundStream_Init()
{
    if (MODE == Mode::Off)
        return;
    const bool hooked = GameplayHooks_Create((LPVOID)_addr(0x1406897B0), (LPVOID)&hkFreeStreamingSource,
                                             (LPVOID*)&g_original) == MH_OK &&
                        GameplayHooks_QueueEnable((LPVOID)_addr(0x1406897B0)) == MH_OK;
    HookLog("SoundStream: %s mode, freeStreamingSource %s\n", MODE == Mode::Shadow ? "shadow" : "replace",
            hooked ? "hooked" : "FAILED");
}
