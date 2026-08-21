#include "../pch.h"

#include "rollback_diagnostics.h"
#include "rollback_memory.h"

#include <cstdio>
#include <cstring>
#include <atomic>

EXTERN_C IMAGE_DOS_HEADER __ImageBase;
extern void HookLog(const char* fmt, ...);

static std::atomic<uint64_t> g_freezeHeartbeat{0};
static std::atomic<DWORD> g_freezeThreadId{0};
static std::atomic<const char*> g_freezePhase{"not-started"};
static std::atomic<int32_t> g_freezeFrame{-1};
static std::atomic<uint32_t> g_freezeCategory{UINT32_MAX};
static std::atomic<uint64_t> g_freezeUnit{0};
static std::atomic<uint64_t> g_freezeDti{0};
static std::atomic<uint64_t> g_freezeMaterialModel{0};
static std::atomic<uint32_t> g_freezeMaterialCount{0};
static std::atomic<uint32_t> g_freezeMaterialInvalid{0};

int Rollback_ResolveDllPath(char* destination, size_t destinationLength,
                            const char* relativePath)
{
    if (!destination || destinationLength == 0) return -1;

    char modulePath[260];
    DWORD result = GetModuleFileNameA((HINSTANCE)&__ImageBase, modulePath,
                                      sizeof(modulePath));
    if (result == 0 || result >= sizeof(modulePath)) {
        destination[0] = '\0';
        return -1;
    }

    char* separator = strrchr(modulePath, '\\');
    if (separator)
        *(separator + 1) = '\0';
    else
        modulePath[0] = '\0';
    return snprintf(destination, destinationLength, "%s%s", modulePath, relativePath);
}

uint64_t Rollback_Fnv1a64(const void* data, size_t length)
{
    uint64_t hash = 0xCBF29CE484222325ULL;
    const uint8_t* bytes = (const uint8_t*)data;
    for (size_t i = 0; i < length; ++i) {
        hash ^= bytes[i];
        hash *= 0x100000001B3ULL;
    }
    return hash;
}

void Rollback_FreezeWatchdogHeartbeatImpl(const char* phase, int32_t frame)
{
    g_freezeThreadId.store(GetCurrentThreadId(), std::memory_order_relaxed);
    g_freezePhase.store(phase, std::memory_order_relaxed);
    g_freezeFrame.store(frame, std::memory_order_relaxed);
    g_freezeCategory.store(UINT32_MAX, std::memory_order_relaxed);
    g_freezeUnit.store(0, std::memory_order_relaxed);
    g_freezeDti.store(0, std::memory_order_relaxed);
    g_freezeHeartbeat.store(GetTickCount64(), std::memory_order_release);
}

void Rollback_FreezeWatchdogUnitImpl(const char* phase, uint32_t category,
                                     uint64_t unit, uint64_t dti, int32_t frame)
{
    g_freezeThreadId.store(GetCurrentThreadId(), std::memory_order_relaxed);
    g_freezePhase.store(phase, std::memory_order_relaxed);
    g_freezeFrame.store(frame, std::memory_order_relaxed);
    g_freezeCategory.store(category, std::memory_order_relaxed);
    g_freezeUnit.store(unit, std::memory_order_relaxed);
    g_freezeDti.store(dti, std::memory_order_relaxed);
    g_freezeHeartbeat.store(GetTickCount64(), std::memory_order_release);
}

void Rollback_FreezeWatchdogMaterialImpl(uint64_t model, uint32_t count,
                                         uint32_t invalid, int32_t frame)
{
    g_freezeMaterialModel.store(model, std::memory_order_relaxed);
    g_freezeMaterialCount.store(count, std::memory_order_relaxed);
    g_freezeMaterialInvalid.store(invalid, std::memory_order_relaxed);
    Rollback_FreezeWatchdogHeartbeatImpl("material-reset", frame);
}

void Rollback_FreezeWatchdogPollImpl()
{
    static uint64_t reportedHeartbeat = 0;
    const uint64_t heartbeat = g_freezeHeartbeat.load(std::memory_order_acquire);
    const uint64_t now = GetTickCount64();
    if (!heartbeat || now - heartbeat < 5000 || heartbeat == reportedHeartbeat)
        return;
    reportedHeartbeat = heartbeat;

    const DWORD threadId = g_freezeThreadId.load(std::memory_order_relaxed);
    CONTEXT context = {};
    context.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
    uint64_t stack[12] = {};
    uint64_t unwindFrames[24] = {};
    size_t unwindCount = 0;
    bool captured = false;
    HANDLE thread = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT |
                               THREAD_QUERY_INFORMATION, FALSE, threadId);
    if (thread) {
        if (SuspendThread(thread) != (DWORD)-1) {
            captured = GetThreadContext(thread, &context) != FALSE;
            if (captured) {
                SIZE_T bytesRead = 0;
                ReadProcessMemory(GetCurrentProcess(), (LPCVOID)context.Rsp,
                                  stack, sizeof(stack), &bytesRead);

                CONTEXT unwind = context;
                while (unwindCount < sizeof(unwindFrames) / sizeof(unwindFrames[0]) &&
                       unwind.Rip) {
                    unwindFrames[unwindCount++] = unwind.Rip;
                    const DWORD64 previousRip = unwind.Rip;
                    const DWORD64 previousRsp = unwind.Rsp;
                    DWORD64 imageBase = 0;
                    PRUNTIME_FUNCTION function = RtlLookupFunctionEntry(
                        unwind.Rip, &imageBase, nullptr);
                    if (function) {
                        PVOID handlerData = nullptr;
                        DWORD64 establisherFrame = 0;
                        RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, unwind.Rip,
                                         function, &unwind, &handlerData,
                                         &establisherFrame, nullptr);
                    } else {
                        uint64_t returnAddress = 0;
                        SIZE_T returnBytes = 0;
                        if (!ReadProcessMemory(GetCurrentProcess(),
                                               (LPCVOID)unwind.Rsp,
                                               &returnAddress,
                                               sizeof(returnAddress),
                                               &returnBytes) ||
                            returnBytes != sizeof(returnAddress)) {
                            break;
                        }
                        unwind.Rip = returnAddress;
                        unwind.Rsp += sizeof(returnAddress);
                    }
                    if ((unwind.Rip == previousRip && unwind.Rsp == previousRsp) ||
                        unwind.Rsp <= previousRsp ||
                        (unwind.Rip && !IsExecutableAddress(unwind.Rip))) {
                        break;
                    }
                }
            }
            ResumeThread(thread);
        }
        CloseHandle(thread);
    }

    HookLog("FREEZEWATCH age=%llu thread=%lu frame=%d phase=%s "
            "category=%u unit=0x%llX dti=0x%llX material=0x%llX "
            "materialCount=%u materialInvalid=%u captured=%d "
            "rip=0x%llX rsp=0x%llX\n",
            now - heartbeat, threadId,
            g_freezeFrame.load(std::memory_order_relaxed),
            g_freezePhase.load(std::memory_order_relaxed),
            g_freezeCategory.load(std::memory_order_relaxed),
            g_freezeUnit.load(std::memory_order_relaxed),
            g_freezeDti.load(std::memory_order_relaxed),
            g_freezeMaterialModel.load(std::memory_order_relaxed),
            g_freezeMaterialCount.load(std::memory_order_relaxed),
            g_freezeMaterialInvalid.load(std::memory_order_relaxed),
            captured ? 1 : 0,
            captured ? context.Rip : 0, captured ? context.Rsp : 0);
    if (captured) {
        HookLog("FREEZEWATCH stack=%016llX,%016llX,%016llX,%016llX,"
                "%016llX,%016llX,%016llX,%016llX,%016llX,%016llX,"
                "%016llX,%016llX\n",
                stack[0], stack[1], stack[2], stack[3], stack[4], stack[5],
                stack[6], stack[7], stack[8], stack[9], stack[10], stack[11]);
        HookLog("FREEZEWATCH unwindCount=%zu\n", unwindCount);
        for (size_t frame = 0; frame < unwindCount; ++frame)
            HookLog("FREEZEWATCH unwind[%02zu]=0x%016llX\n",
                    frame, unwindFrames[frame]);
    }
}
