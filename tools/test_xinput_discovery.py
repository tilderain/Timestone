"""Exercise production discovery/cache/worker code against deterministic Win32 mocks."""

from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]

MOCKS = r'''
#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <vector>
#define WINAPI
#define __fastcall
using DWORD = uint32_t;
using ULONGLONG = uint64_t;
using HANDLE = void*;
using HMODULE = void*;
using LPCSTR = const char*;
using SRWLOCK = bool;
#define SRWLOCK_INIT false
constexpr DWORD XUSER_MAX_COUNT = 4, XINPUT_FLAG_GAMEPAD = 1;
constexpr DWORD ERROR_SUCCESS = 0, ERROR_DEVICE_NOT_CONNECTED = 1167;
constexpr DWORD WAIT_OBJECT_0 = 0, INFINITE = 0xffffffff;
constexpr int FALSE = 0, MH_OK = 0;
constexpr int GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS = 4, GET_MODULE_HANDLE_EX_FLAG_PIN = 1;
struct XINPUT_STATE { DWORD dwPacketNumber; uint8_t Gamepad[12]; };
struct XINPUT_CAPABILITIES { uint8_t Type, SubType; uint16_t Flags; uint8_t rest[16]; };
bool active = true, failLibrary = false;
uint64_t now = 100;
int wakes = 0, waits = 0, nativeReads = 0, nativeCaps = 0, nativeDetects = 0;
DWORD nativeResult = ERROR_SUCCESS;
std::vector<DWORD> probes, capsProbes;
void (*duringProbe)() = nullptr;
bool GameplayHooks_IsActive() { return active; }
void HookLog(const char*, ...) {}
void AcquireSRWLockExclusive(SRWLOCK* lock) { assert(!*lock); *lock = true; }
bool TryAcquireSRWLockExclusive(SRWLOCK* lock) {
    if (*lock) return false;
    *lock = true; return true;
}
void ReleaseSRWLockExclusive(SRWLOCK* lock) { assert(*lock); *lock = false; }
ULONGLONG GetTickCount64() { return now; }
bool SetEvent(HANDLE) { ++wakes; return true; }
DWORD WaitForSingleObject(HANDLE, DWORD) { return waits++ == 0 ? WAIT_OBJECT_0 : 0xffffffff; }
HMODULE LoadLibraryW(const wchar_t*) { return failLibrary ? nullptr : (void*)1; }
bool FreeLibrary(HMODULE) { return true; }
DWORD BackendState(DWORD user, XINPUT_STATE* state);
DWORD BackendCaps(DWORD user, DWORD flags, XINPUT_CAPABILITIES* caps);
void* GetProcAddress(HMODULE, const char* name) {
    return std::strcmp(name, "XInputGetState") == 0 ? (void*)&BackendState : (void*)&BackendCaps;
}
uintptr_t _addr(uintptr_t va) { return va; }
int GameplayHooks_Create(void*, void*, void**) { return MH_OK; }
int GameplayHooks_QueueEnable(void*) { return MH_OK; }
bool GetModuleHandleExA(int, LPCSTR, HMODULE*) { return true; }
HANDLE CreateEventW(void*, bool, bool, void*) { return (void*)1; }
HANDLE CreateThread(void*, size_t, DWORD(*)(void*), void*, DWORD, void*) { return nullptr; }
bool CloseHandle(HANDLE) { return true; }
'''

CHECKS = r'''
DWORD BackendState(DWORD user, XINPUT_STATE* state) {
    assert(!cacheLock); // No driver calls under the publication lock.
    probes.push_back(user);
    if (duringProbe) duringProbe();
    state->dwPacketNumber = 123;
    return user == 0 ? ERROR_SUCCESS : ERROR_DEVICE_NOT_CONNECTED;
}
DWORD BackendCaps(DWORD user, DWORD flags, XINPUT_CAPABILITIES* caps) {
    assert(!cacheLock && flags == XINPUT_FLAG_GAMEPAD);
    capsProbes.push_back(user);
    caps->SubType = 1;
    return ERROR_SUCCESS;
}
DWORD NativeState(DWORD, XINPUT_STATE* state) {
    ++nativeReads;
    state->dwPacketNumber = 999;
    return nativeResult;
}
DWORD NativeCaps(DWORD, DWORD, XINPUT_CAPABILITIES*) { ++nativeCaps; return 17; }
uint8_t NativeDetect(uintptr_t, uint32_t, uint32_t flags) {
    assert(flags == 0x18);
    ++nativeDetects;
    XINPUT_STATE state{};
    XINPUT_CAPABILITIES caps{};
    if (inDiscovery) {
        assert(ReadState(0, &state) == ERROR_SUCCESS && state.dwPacketNumber == 123);
        assert(ReadCaps(0, 1, &caps) == ERROR_SUCCESS && caps.SubType == 1);
        state.dwPacketNumber = 77;
        assert(ReadState(1, &state) == ERROR_DEVICE_NOT_CONNECTED && state.dwPacketNumber == 77);
        assert(ReadState(4, &state) == ERROR_DEVICE_NOT_CONNECTED);
        assert(ReadCaps(4, 1, &caps) == ERROR_DEVICE_NOT_CONNECTED);
    }
    return 42;
}
void reset() {
    for (unsigned i = 0; i < 4; ++i) { cache[i] = {}; generations[i] = 0; }
    active = true; failLibrary = false; cacheLock = false; inDiscovery = false;
    now = 100; wakes = waits = nativeReads = nativeCaps = nativeDetects = 0;
    probes.clear(); capsProbes.clear(); duringProbe = nullptr;
    nativeResult = ERROR_SUCCESS;
    workerStatus = WorkerStatus::Ready;
    wakeEvent = (void*)1;
}
void runWorker() {
    workerStatus = WorkerStatus::Starting;
    waits = 0;
    Worker(nullptr);
    assert(workerStatus == WorkerStatus::Failed); // Mock's second wait terminates the loop.
    workerStatus = WorkerStatus::Ready;
}
int main() {
    alignas(8) uint8_t pad[0xC40]{};
    const uintptr_t object = (uintptr_t)pad;
    originalState = NativeState; originalCaps = NativeCaps; originalDetect = NativeDetect;
    reset();
    assert(Redetect(object, 0, 0x18) == 0);
    assert(nativeDetects == 0 && wakes == 1 && probes.empty());
    assert(Redetect(object, 0, 0x18) == 0 && wakes == 1); // Coalesce pending work.
    runWorker();
    assert((probes == std::vector<DWORD>{0, 1, 2, 3}));
    assert((capsProbes == std::vector<DWORD>{0}));
    assert(Redetect(object, 0, 0x18) == 42);
    assert(!inDiscovery && nativeReads == 0 && nativeCaps == 0);
    // Cached state is discovery-only. Every active read forwards the current sample.
    XINPUT_STATE state{};
    XINPUT_CAPABILITIES caps{};
    assert(ReadState(0, &state) == ERROR_SUCCESS && state.dwPacketNumber == 999);
    assert(ReadState(0, &state) == ERROR_SUCCESS && nativeReads == 2);
    assert(ReadCaps(0, 1, &caps) == 17 && nativeCaps == 1);
    // A disconnect invalidates even a still-fresh positive discovery result.
    nativeResult = ERROR_DEVICE_NOT_CONNECTED;
    ReadState(0, &state);
    assert(!PrepareSnapshot(object) && cache[0].pending);
    assert(!cache[1].pending);
    runWorker();
    assert(PrepareSnapshot(object));
    // Assigned user 0 is not probed when other users' entries expire.
    pad[0x60] = 1; pad[0x63] = 4;
    now += CacheLifetimeMs;
    assert(!PrepareSnapshot(object));
    assert(!cache[0].pending && cache[1].pending && cache[2].pending && cache[3].pending);
    probes.clear(); capsProbes.clear(); runWorker();
    assert((probes == std::vector<DWORD>{1, 2, 3}) && capsProbes.empty());
    // Forced detection of an occupied slot keeps the original close/reopen behavior.
    assert(Redetect(object, 0, 0x18) == 42 && !inDiscovery);
    pad[0x60] = pad[0x63] = 0;
    // Lock contention defers instead of waiting or falling through to DirectInput.
    cacheLock = true;
    const int before = nativeDetects;
    assert(Redetect(object, 0, 0x18) == 0 && nativeDetects == before);
    cacheLock = false;
    workerStatus = WorkerStatus::Starting;
    assert(Redetect(object, 0, 0x18) == 0);
    workerStatus = WorkerStatus::Failed;
    assert(Redetect(object, 0, 0x18) == 42); // Startup failure restores native behavior.
    active = false; workerStatus = WorkerStatus::Ready;
    assert(Redetect(object, 0, 0x18) == 42); // Normal Launch/native startup passthrough.
    active = true;
    XInputDiscovery_Stop();
    assert(workerStatus == WorkerStatus::Stopped);
    assert(Redetect(object, 0, 0x18) == 42);
    probes.clear(); Worker(nullptr); assert(probes.empty());
    // A generation change while a query runs cannot publish a usable stale sample.
    reset();
    assert(!PrepareSnapshot(object));
    duringProbe = [] { ++generations[0]; };
    runWorker();
    assert(!PrepareSnapshot(object) && cache[0].pending);
    reset(); failLibrary = true; workerStatus = WorkerStatus::Starting;
    Worker(nullptr); assert(workerStatus == WorkerStatus::Failed && probes.empty());
    reset();
    XInputDiscovery_Init(); // Mock CreateThread failure must not leave Starting forever.
    assert(workerStatus == WorkerStatus::Failed && !wakeEvent);
}
'''


class XInputDiscoveryTests(unittest.TestCase):
    def test_production_discovery(self):
        source = (ROOT / "UMVC3Hook/code/xinput_discovery.cpp").read_text()
        source = '\n'.join(line for line in source.splitlines() if not line.startswith('#include'))
        with tempfile.TemporaryDirectory(dir="/tmp/opencode") as tmp:
            cpp = Path(tmp) / "test.cpp"
            exe = Path(tmp) / "test"
            cpp.write_text(MOCKS + source + CHECKS)
            subprocess.run(["g++", "-std=c++20", "-Wall", "-Wextra", str(cpp), "-o", str(exe)], check=True)
            subprocess.run([str(exe)], check=True)

    def test_single_hook_owner(self):
        hooks = (ROOT / "UMVC3Hook/code/rollback_hooks.cpp").read_text()
        self.assertIn("XInputDiscovery_Init();", hooks)
        self.assertNotIn("EnableHook(0x140514950", hooks)
        self.assertNotIn("EnableHook(0x1409981c1", hooks)
        self.assertIn("XInputDiscovery_Stop();", (ROOT / "UMVC3Hook/code/rollback.cpp").read_text())
        self.assertIn("XInputDiscovery_Stop();", (ROOT / "UMVC3Hook/dllmain.cpp").read_text())


if __name__ == "__main__":
    unittest.main()
