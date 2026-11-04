#include "../pch.h"
#include "xinput_discovery.h"
#include "gameplay_hooks.h"
#include "mk10utils.h"
#include <XInput.h>
#include <atomic>

extern void HookLog(const char* fmt, ...);

namespace {
using StateFn = DWORD(WINAPI*)(DWORD, XINPUT_STATE*);
using CapsFn = DWORD(WINAPI*)(DWORD, DWORD, XINPUT_CAPABILITIES*);
using DetectFn = uint8_t(__fastcall*)(uintptr_t, uint32_t, uint32_t);
StateFn originalState = nullptr;
CapsFn originalCaps = nullptr;
DetectFn originalDetect = nullptr;

enum class WorkerStatus { Starting, Ready, Failed, Stopped };
std::atomic<WorkerStatus> workerStatus{WorkerStatus::Starting};
HANDLE wakeEvent = nullptr;
SRWLOCK cacheLock = SRWLOCK_INIT;
constexpr ULONGLONG CacheLifetimeMs = 1000;
struct Sample {
    XINPUT_STATE state{};
    XINPUT_CAPABILITIES caps{};
    DWORD stateResult = ERROR_DEVICE_NOT_CONNECTED;
    DWORD capsResult = ERROR_DEVICE_NOT_CONNECTED;
    ULONGLONG completed = 0;
    bool valid = false;
    uint32_t generation = 0;
};
struct Entry {
    Sample sample;
    bool pending = false;
};
Entry cache[XUSER_MAX_COUNT];
std::atomic<uint32_t> generations[XUSER_MAX_COUNT]{};
thread_local Sample discovery[XUSER_MAX_COUNT];
thread_local bool inDiscovery = false;

// No lock is held across a driver call. Only this worker performs discovery I/O.
DWORD WINAPI Worker(void*)
{
    HMODULE xinput = LoadLibraryW(L"XINPUT1_3.dll");
    auto getState = xinput ? reinterpret_cast<StateFn>(GetProcAddress(xinput, "XInputGetState")) : nullptr;
    auto getCaps = xinput ? reinterpret_cast<CapsFn>(GetProcAddress(xinput, "XInputGetCapabilities")) : nullptr;
    WorkerStatus expected = WorkerStatus::Starting;
    workerStatus.compare_exchange_strong(expected,
        getState && getCaps ? WorkerStatus::Ready : WorkerStatus::Failed);
    if (!getState || !getCaps)
        HookLog("XInput discovery: backend unavailable; using native discovery\n");
    while (workerStatus.load() == WorkerStatus::Ready) {
        if (WaitForSingleObject(wakeEvent, INFINITE) != WAIT_OBJECT_0)
            break;
        for (DWORD user = 0; user < XUSER_MAX_COUNT; ++user) {
            if (workerStatus.load() != WorkerStatus::Ready)
                break;
            AcquireSRWLockExclusive(&cacheLock);
            const bool requested = cache[user].pending;
            ReleaseSRWLockExclusive(&cacheLock);
            if (!requested)
                continue;
            Sample result;
            result.generation = generations[user].load();
            result.stateResult = getState(user, &result.state);
            if (result.stateResult == ERROR_SUCCESS && workerStatus.load() == WorkerStatus::Ready) {
                result.capsResult = getCaps(user, XINPUT_FLAG_GAMEPAD, &result.caps);
            }
            result.completed = GetTickCount64();
            result.valid = true;
            AcquireSRWLockExclusive(&cacheLock);
            cache[user].sample = result;
            cache[user].pending = false;
            ReleaseSRWLockExclusive(&cacheLock);
        }
    }
    expected = WorkerStatus::Ready;
    workerStatus.compare_exchange_strong(expected, WorkerStatus::Failed);
    if (xinput)
        FreeLibrary(xinput);
    return 0;
}

bool PrepareSnapshot(uintptr_t pad)
{
    // Match redetectSlot's exclusion of XInput users owned by active native slots.
    bool assigned[XUSER_MAX_COUNT]{};
    for (unsigned slot = 0; slot < XUSER_MAX_COUNT; ++slot) {
        const auto* entry = reinterpret_cast<const uint8_t*>(pad + 0x58 + slot * 0x2E0);
        const uint32_t user = *reinterpret_cast<const uint32_t*>(entry + 0x18);
        if (entry[8] == 1 && entry[0xB] >= 4 && user < XUSER_MAX_COUNT)
            assigned[user] = true;
    }
    // A cache publication must never stall the frame either.
    if (!TryAcquireSRWLockExclusive(&cacheLock))
        return false;
    const ULONGLONG now = GetTickCount64();
    bool ready = true;
    bool wake = false;
    for (unsigned user = 0; user < XUSER_MAX_COUNT; ++user) {
        discovery[user] = {};
        if (assigned[user])
            continue;
        auto& entry = cache[user];
        if (entry.sample.valid && entry.sample.generation == generations[user].load() &&
            now - entry.sample.completed < CacheLifetimeMs) {
            discovery[user] = entry.sample;
        } else {
            ready = false;
            if (!entry.pending) {
                entry.pending = true;
                wake = true;
            }
        }
    }
    ReleaseSRWLockExclusive(&cacheLock);
    if (wake)
        SetEvent(wakeEvent);
    return ready;
}

DWORD WINAPI ReadState(DWORD user, XINPUT_STATE* state)
{
    if (inDiscovery) {
        if (user >= XUSER_MAX_COUNT)
            return ERROR_DEVICE_NOT_CONNECTED;
        const auto& sample = discovery[user];
        if (sample.stateResult == ERROR_SUCCESS)
            *state = sample.state;
        return sample.stateResult;
    }
    const DWORD result = originalState(user, state);
    if (result != ERROR_SUCCESS && user < XUSER_MAX_COUNT)
        ++generations[user];
    return result;
}

DWORD WINAPI ReadCaps(DWORD user, DWORD flags, XINPUT_CAPABILITIES* caps)
{
    if (inDiscovery && flags == XINPUT_FLAG_GAMEPAD) {
        if (user >= XUSER_MAX_COUNT)
            return ERROR_DEVICE_NOT_CONNECTED;
        const auto& sample = discovery[user];
        if (sample.capsResult == ERROR_SUCCESS)
            *caps = sample.caps;
        return sample.capsResult;
    }
    return originalCaps(user, flags, caps);
}

uint8_t __fastcall Redetect(uintptr_t pad, uint32_t slot, uint32_t flags)
{
    const auto status = workerStatus.load();
    if (GameplayHooks_IsActive() && slot < XUSER_MAX_COUNT &&
        *reinterpret_cast<const uint8_t*>(pad + 0x63 + slot * 0x2E0) == 0 &&
        (status == WorkerStatus::Starting || status == WorkerStatus::Ready)) {
        if (status == WorkerStatus::Starting || !PrepareSnapshot(pad))
            return 0;
        // Keep native slot assignment, subtype mapping and DirectInput fallback.
        // The snapshot stays coherent for both API calls throughout this dispatch.
        struct Scope {
            bool previous = inDiscovery;
            Scope() { inDiscovery = true; }
            ~Scope() { inDiscovery = previous; }
        } scope;
        return originalDetect(pad, slot, flags);
    }
    return originalDetect(pad, slot, flags);
}
}

void XInputDiscovery_Init()
{
    // All three hooks are created before any are queued, so a create failure
    // cannot enable cached discovery without intercepting its API calls.
    const struct { uintptr_t address; void* hook; void** original; } hooks[] = {
        {0x140514950, (void*)&Redetect, (void**)&originalDetect},
        {0x1409981C1, (void*)&ReadState, (void**)&originalState},
        {0x1409981E5, (void*)&ReadCaps, (void**)&originalCaps},
    };
    for (const auto& hook : hooks) {
        if (GameplayHooks_Create((void*)_addr(hook.address), hook.hook, hook.original) != MH_OK) {
            workerStatus.store(WorkerStatus::Failed);
            HookLog("XInput discovery: could not create hook at 0x%llX\n",
                    (unsigned long long)hook.address);
            return;
        }
    }
    for (const auto& hook : hooks)
        if (GameplayHooks_QueueEnable((void*)_addr(hook.address)) != MH_OK) {
            workerStatus.store(WorkerStatus::Failed);
            return;
        }

    // Pin before starting: detach must not join a potentially blocked driver call
    // under the loader lock. The worker owns no game objects or hook trampolines.
    HMODULE self = nullptr;
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
            reinterpret_cast<LPCSTR>(&XInputDiscovery_Init), &self) ||
        !(wakeEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr))) {
        workerStatus.store(WorkerStatus::Failed);
        return;
    }
    HANDLE thread = CreateThread(nullptr, 0, Worker, nullptr, 0, nullptr);
    if (!thread) {
        workerStatus.store(WorkerStatus::Failed);
        CloseHandle(wakeEvent);
        wakeEvent = nullptr;
        return;
    }
    CloseHandle(thread);
    // Event storage is process-lifetime, including after a nonblocking Stop.
    HookLog("XInput discovery: asynchronous probes enabled; active input stays live\n");
}

void XInputDiscovery_Stop()
{
    workerStatus.store(WorkerStatus::Stopped);
    if (wakeEvent)
        SetEvent(wakeEvent);
}
