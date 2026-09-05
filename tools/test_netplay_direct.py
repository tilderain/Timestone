"""Test production direct dispatch with mocked APIs and real ENet loopback."""
from pathlib import Path
import subprocess
import tempfile
import os
import unittest

ROOT = Path(__file__).resolve().parents[1]
SOURCE = (ROOT / "UMVC3Hook/code/netplay.cpp").read_text()
CONTROLLER = (ROOT / "UMVC3Hook/code/controller.cpp").read_text()


def function(source, name):
    pos = source.index(name + "(")
    start = source.rfind("\n", 0, pos) + 1
    end = source.index("{", pos) + 1
    depth = 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


WINDOWS = r'''
#pragma once
#include <atomic>
#include <cstdint>
#include <mutex>
using HANDLE = void*;
using DWORD = uint32_t;
using LPVOID = void*;
#define WINAPI
constexpr DWORD WAIT_OBJECT_0 = 0;
inline thread_local DWORD currentThread = 1;
inline DWORD GetCurrentThreadId() { return currentThread; }
inline DWORD GetTickCount() { return 100; }
inline void Sleep(DWORD) {}
inline HANDLE CreateThread(void*, size_t, DWORD(*)(LPVOID), void*, DWORD, void*) { return nullptr; }
inline DWORD WaitForSingleObject(HANDLE, DWORD) { return WAIT_OBJECT_0; }
inline void CloseHandle(HANDLE) {}
struct SRWLOCK { std::mutex mutex; };
#define SRWLOCK_INIT {}
inline thread_local int lockDepth = 0;
inline unsigned lockEntries = 0;
inline void AcquireSRWLockExclusive(SRWLOCK* lock) { lock->mutex.lock(); ++lockEntries; ++lockDepth; }
inline void ReleaseSRWLockExclusive(SRWLOCK* lock) { --lockDepth; lock->mutex.unlock(); }
'''

PREAMBLE = r'''
#include <cassert>
#include <cstring>
#include <cstdio>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <deque>
#include <vector>
#include "Windows.h"
#include "netplay.h"
bool g_isResimulating = false;
constexpr bool ROLLBACK_FREEZE_WATCHDOG_ENABLED = false;
void Rollback_FreezeWatchdogPoll() {}
int launchOption = 1;
constexpr int LAUNCH_DUAL_TRAINING = 1;
int GetLaunchOption() { return launchOption; }
void HookLog(const char*, ...) {}
extern "C" uint32_t FrameStepper_GetBattleGeneration() { return 7; }
void SteamLobby_Shutdown(bool) {}
bool SteamTransport_IsActive() { return false; }
uint32_t SteamTransport_GetRTT() { return 0; }

// Run the actual controller packet receiver against a small mock timeline.
constexpr size_t RING_SIZE = 512;
int32_t g_netFrameCount = 10, g_lastRemoteFrame = -1;
uint32_t g_lastRemoteInput, g_receivedInput[RING_SIZE];
int32_t g_confirmedFrame[RING_SIZE], g_remoteFrameAdvantage;
unsigned inputCalls, checksumCalls, updateCalls, frameCalls, startCalls, settingsCalls, extensionCalls;
uint32_t lastChecksum, lastControl;
bool recursiveReceive = false;
extern "C" void Controller_ResetBuffers() {
    std::memset(g_confirmedFrame, -1, sizeof(g_confirmedFrame));
    std::memset(g_receivedInput, 0, sizeof(g_receivedInput));
    g_lastRemoteFrame = -1;
}
bool IsFrameConfirmed(int32_t frame) { return g_confirmedFrame[frame % RING_SIZE] == frame; }
void QueueInboundChecksum(const InputPacket& p) { ++checksumCalls; lastChecksum = p.checksum; }
void Rollback_UpdateRemoteInput(int32_t, uint32_t) { ++updateCalls; }
void LogReceivedPacket(const InputPacket&) {
    assert(lockDepth == 0);
    ++inputCalls;
    if (recursiveReceive) { Netplay_Pump(); Netplay_SendDelay(3, 42); }
}
void TraceInputPacket(const char*, const char*, int32_t, const InputPacket&, int, uint16_t, uint16_t, uint16_t) {}
void FrameStepper_ReceiveNetplayControl(uint32_t type, const void*) { assert(lockDepth == 0); lastControl = type; }
void TrainingBoot_ReceiveNetplaySettings(const LaunchTrainingPacket&) { assert(lockDepth == 0); ++settingsCalls; }
void AutoTest_ReceiveNetplayStart(const AutoTestStartPacket&) { assert(lockDepth == 0); ++startCalls; }
void AutoTest_ReceiveNetplayFrame(const AutoTestFramePacket&) { assert(lockDepth == 0); ++frameCalls; }
void ChrSelImGui_ReceiveNetplayPacket(const void*, size_t) { assert(lockDepth == 0); ++extensionCalls; }
'''

MOCK_ENET = r'''
std::deque<ENetEvent> incoming;
std::vector<uint32_t> sentTypes;
unsigned serviceCalls, flushCalls, destroyed, destroyedHosts;
bool sendFail = false, createFail = false;
ENetPacket* enet_packet_create(const void* data, size_t size, enet_uint32 flags) {
    if (createFail) return nullptr;
    auto* p = (ENetPacket*)std::calloc(1, sizeof(ENetPacket));
    p->data = (enet_uint8*)std::malloc(size);
    std::memcpy(p->data, data, size);
    p->dataLength = size;
    p->flags = flags;
    return p;
}
void enet_packet_destroy(ENetPacket* p) { ++destroyed; std::free(p->data); std::free(p); }
int enet_peer_send(ENetPeer*, enet_uint8, ENetPacket* p) {
    assert(lockDepth == 1);
    if (sendFail) return -1;
    uint32_t type;
    std::memcpy(&type, p->data, sizeof(type));
    assert(p->flags == (type == PACKET_INPUT ? ENET_PACKET_FLAG_UNSEQUENCED : ENET_PACKET_FLAG_RELIABLE));
    sentTypes.push_back(type);
    enet_packet_destroy(p);
    return 0;
}
void enet_host_flush(ENetHost*) { assert(lockDepth == 1); ++flushCalls; }
int enet_host_service(ENetHost*, ENetEvent* event, enet_uint32 timeout) {
    assert(timeout == 0 && lockDepth == 1);
    ++serviceCalls;
    if (incoming.empty()) return 0;
    *event = incoming.front(); incoming.pop_front(); return 1;
}
void enet_peer_disconnect_now(ENetPeer*, enet_uint32) { assert(lockDepth == 1); }
void enet_peer_timeout(ENetPeer*, enet_uint32, enet_uint32, enet_uint32) { assert(lockDepth == 1); }
void enet_peer_throttle_configure(ENetPeer* peer, enet_uint32 interval,
                                  enet_uint32 acceleration, enet_uint32 deceleration) {
    assert(lockDepth == 1);
    peer->packetThrottleInterval = interval;
    peer->packetThrottleAcceleration = acceleration;
    peer->packetThrottleDeceleration = deceleration;
}
void enet_host_destroy(ENetHost*) { assert(lockDepth == 1); ++destroyedHosts; }
'''

CASES = r'''
template<typename T> void Queue(const T& p, ENetPeer* peer) {
    incoming.push_back({ENET_EVENT_TYPE_RECEIVE, peer, 0, 0, enet_packet_create(&p, sizeof(p), 0)});
}
template<typename T> void Receive(const T& p) {
    ENetPacket wire = {};
    wire.data = (enet_uint8*)&p; wire.dataLength = sizeof(p);
    HandleReceivedPacket(wire, g_peer);
}
int main() {
    ENetHost host = {}; host.address.port = 1234;
    ENetPeer peer = {}; peer.state = ENET_PEER_STATE_CONNECTED;
    peer.packetThrottleLimit = ENET_PEER_PACKET_THROTTLE_SCALE;
    peer.packetThrottle = 2;
    Netplay_Init(&host, &peer);
    assert(peer.packetThrottle == ENET_PEER_PACKET_THROTTLE_SCALE);
    assert(peer.packetThrottleDeceleration == 0);
    assert(Netplay_IsActive() && Netplay_OwnsHost());
    Netplay_Pump(); assert(serviceCalls == 1);
    InputPacket input = {}; input.type = PACKET_INPUT; input.battle_generation = 7;
    input.start_frame = 10; input.inputs[0] = 123; input.checksum = 456;
    input.inputs[1] = 222;
    Queue(input, &peer);
    recursiveReceive = true;
    Netplay_Pump(); // Receiver may send a reply and attempt a recursive pump.
    recursiveReceive = false;
    assert(inputCalls == 1 && g_receivedInput[10] == 123 && lastChecksum == 456);
    assert(sentTypes.back() == PACKET_DELAY && flushCalls == 1);
    input.inputs[0] = 999; input.inputs[1] = 333;
    Receive(input);
    assert(g_receivedInput[10] == 123); // Confirmed past/current frames immutable.
    assert(g_receivedInput[11] == 333); // Future corrections remain accepted.
    input.battle_generation = 6;
    Receive(input); assert(inputCalls == 2); // Reject stale epoch.
    input.battle_generation = 7;
    input.start_frame = UINT32_MAX;
    Receive(input); assert(inputCalls == 2); // Invalid frame range cannot overflow.
    input.start_frame = 10;

    unsigned before = serviceCalls;
    currentThread = 2; Netplay_Pump(); assert(serviceCalls == before);
    Netplay_SendPacket(input); // UI-thread send remains serialized and immediate.
    currentThread = 1;
    g_isResimulating = true; Netplay_Pump(); assert(serviceCalls == before);
    g_isResimulating = false;
    for (int i = 0; i < 130; ++i) Queue(input, &peer);
    Netplay_Pump(); assert(serviceCalls == before + 64 && incoming.size() == 66);
    Netplay_Pump(); Netplay_Pump(); assert(incoming.empty());

    AutoTestFramePacket report = {}; report.type = PACKET_AUTOTEST_FRAME;
    for (int i = 0; i < 5000; ++i) Receive(report);
    assert(frameCalls == 5000); // No intermediate report queue to overflow.
    AutoTestStartPacket start = {}; start.type = PACKET_AUTOTEST_START;
    Receive(start); assert(startCalls == 1);
    launchOption = 0; Receive(start); Receive(report);
    assert(startCalls == 1 && frameCalls == 5000);
    launchOption = LAUNCH_DUAL_TRAINING;
    LaunchTrainingPacket launch = {}; launch.type = PACKET_LAUNCH_TRAINING;
    Receive(launch); assert(settingsCalls == 1);
    launch.voiceLanguage[0][0] = 2; Receive(launch); assert(settingsCalls == 1);
    MultiResimPacket multi = {PACKET_MULTI_RESIM, 1}; Receive(multi);
    assert(lastControl == PACKET_MULTI_RESIM);
    lastControl = UINT32_MAX; multi.enabled = 2; Receive(multi); assert(lastControl == UINT32_MAX);
    DelayPacket delay = {PACKET_DELAY, 3, 8, 42}; Receive(delay); assert(lastControl == PACKET_DELAY);
    BattleReadyPacket ready = {}; ready.type = PACKET_BATTLE_READY; Receive(ready);
    assert(lastControl == PACKET_BATTLE_READY);
    DesyncPacket desync = {}; desync.type = PACKET_DESYNC; desync.action = DESYNC_TRIGGER;
    Receive(desync); assert(lastControl == PACKET_DESYNC);
    RestartMatchPacket restart = {}; restart.type = PACKET_RESTART_MATCH; Receive(restart);
    assert(lastControl == PACKET_RESTART_MATCH);
    SnapDumpPacket snap = {}; snap.type = PACKET_SNAPDUMP; Receive(snap);
    assert(lastControl == PACKET_SNAPDUMP);
    uint32_t extension[2] = {PACKET_EXTENSION, 123}; Receive(extension); assert(extensionCalls == 1);
    Netplay_SendAutoTestFrame(report); Netplay_SendAutoTestComplete();
    assert(sentTypes.back() == PACKET_AUTOTEST_COMPLETE);
    assert(sentTypes[sentTypes.size()-2] == PACKET_AUTOTEST_FRAME);
    unsigned oldDestroyed = destroyed;
    sendFail = true; Netplay_SendPacket(input); assert(destroyed == oldDestroyed + 1);
    createFail = true; Netplay_SendPacket(input); assert(destroyed == oldDestroyed + 1);
    sendFail = createFail = false;
    ENetPacket truncated = {}; truncated.data = (enet_uint8*)&input; truncated.dataLength = 3;
    before = inputCalls; HandleReceivedPacket(truncated, &peer); assert(inputCalls == before);
    PlayerDisconnectPacket disconnect = {PACKET_PLAYER_DISCONNECT}; Queue(disconnect, &peer);
    Netplay_Pump();
    assert(!Netplay_IsActive() && Netplay_TakePeerDisconnected() && Netplay_TakePlayerDisconnectNotice());
    assert(Netplay_OwnsHost());
    Netplay_Shutdown(); assert(!Netplay_OwnsHost() && destroyedHosts == 1);
    Netplay_Init(&host, &peer); assert(Netplay_IsActive());
    Netplay_Shutdown(); assert(destroyedHosts == 2);
}
'''

LOOPBACK = r'''
#include <thread>
int main() {
    assert(enet_initialize() == 0);
    ENetAddress address = {ENET_HOST_ANY, 0};
    ENetHost* server = enet_host_create(&address, 1, 1, 0, 0);
    ENetHost* client = enet_host_create(nullptr, 1, 1, 0, 0);
    assert(server && client);
    address.port = server->address.port;
    assert(enet_address_set_host(&address, "127.0.0.1") == 0);
    ENetPeer* remote = enet_host_connect(client, &address, 1, 0);
    ENetPeer* local = nullptr;
    bool clientConnected = false;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    auto checkDeadline = [&] { assert(std::chrono::steady_clock::now() < deadline); };
    ENetEvent event;
    while (!local || !clientConnected) {
        checkDeadline();
        if (enet_host_service(server, &event, 0) > 0 && event.type == ENET_EVENT_TYPE_CONNECT)
            local = event.peer;
        if (enet_host_service(client, &event, 0) > 0 && event.type == ENET_EVENT_TYPE_CONNECT)
            clientConnected = true;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    // Reproduce the launch->frame-paced ACK transition using ENet's real
    // throttle implementation. Defaults can suppress virtually all inputs
    // until the 5-second baseline refresh. Our configured peer must not.
    ENetPeer defaults = *local;
    defaults.lastRoundTripTime = 1;
    defaults.lastRoundTripTimeVariance = 0;
    defaults.packetThrottle = defaults.packetThrottleLimit = ENET_PEER_PACKET_THROTTLE_SCALE;
    for (unsigned i = 0; i < 16; ++i) enet_peer_throttle(&defaults, 17);
    assert(defaults.packetThrottle == 0);
    local->lastRoundTripTime = 1;
    local->lastRoundTripTimeVariance = 0;
    local->packetThrottle = 2; // Also undo throttling inherited from startup.
    Netplay_Init(server, local);
    assert(local->packetThrottle == local->packetThrottleLimit);
    for (unsigned i = 0; i < 300; ++i) {
        enet_peer_throttle(local, 17 + i % 17);
        assert(local->packetThrottle == local->packetThrottleLimit);
    }
    ENetPeer limited = *local;
    limited.packetThrottle = limited.packetThrottleLimit = 8;
    for (unsigned i = 0; i < 32; ++i) {
        enet_peer_throttle(&limited, i % 2 ? 1 : 33);
        assert(limited.packetThrottle == 8); // Do not bypass bandwidth limits.
    }
    InputPacket input = {}; input.type = PACKET_INPUT;
    input.battle_generation = 7; input.start_frame = 10; input.inputs[0] = 0x55;
    ENetPacket* packet = enet_packet_create(&input, sizeof(input), ENET_PACKET_FLAG_UNSEQUENCED);
    assert(enet_peer_send(remote, 0, packet) == 0);
    for (int i = 0; i < 130; ++i) {
        AutoTestFramePacket report = {}; report.type = PACKET_AUTOTEST_FRAME; report.frame = i;
        packet = enet_packet_create(&report, sizeof(report), ENET_PACKET_FLAG_RELIABLE);
        assert(enet_peer_send(remote, 0, packet) == 0);
    }
    enet_host_flush(client);
    Netplay_SendDelay(3, 42);
    bool gotDelay = false;
    while (!gotDelay || inputCalls == 0 || frameCalls < 130) {
        checkDeadline();
        Netplay_Pump();
        while (enet_host_service(client, &event, 0) > 0) {
            if (event.type == ENET_EVENT_TYPE_RECEIVE) {
                DelayPacket delay;
                assert(event.packet->dataLength == sizeof(delay));
                std::memcpy(&delay, event.packet->data, sizeof(delay));
                assert(delay.type == PACKET_DELAY && delay.delay == 3 && delay.effective_frame == 42);
                gotDelay = true;
                enet_packet_destroy(event.packet);
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    assert(g_receivedInput[10] == 0x55);
    assert(remote->packetThrottleDeceleration == 0); // Configuration reaches the peer too.
    PlayerDisconnectPacket disconnect = {PACKET_PLAYER_DISCONNECT};
    packet = enet_packet_create(&disconnect, sizeof(disconnect), ENET_PACKET_FLAG_RELIABLE);
    assert(enet_peer_send(remote, 0, packet) == 0);
    enet_host_flush(client);
    while (Netplay_IsActive()) { checkDeadline(); Netplay_Pump(); }
    assert(Netplay_TakePlayerDisconnectNotice() && Netplay_OwnsHost());
    Netplay_Shutdown();
    enet_host_destroy(client);
    enet_deinitialize();
}
'''


def production():
    return function(CONTROLLER, "Controller_ReceiveNetplayInput") + SOURCE[SOURCE.index("extern void HookLog"):]


class DirectNetplayTests(unittest.TestCase):
    def test_timesync_ignores_stale_rtt(self):
        # Compile the actual frame-start pacing code and its real history window,
        # rather than a duplicate of the formula. Sleep is recorded, not executed.
        declarations = CONTROLLER[CONTROLLER.index("int32_t      g_localFrameAdvantage"):
                                  CONTROLLER.index("// Log the contents of a received input packet")]
        frame_start = function(CONTROLLER, "Controller_OnFrameStart")
        pacing = "void TickTimeSync() {\n" + frame_start[frame_start.index("    // --- Frame-offset timesync."):]
        mocks = r'''
#include <cassert>
#include <cstdint>
#include <cstring>
using DWORD = uint32_t;
int32_t g_netFrameCount, g_lastRemoteFrame;
uint32_t fakeRTT = 0, rttQueries = 0;
unsigned sleepCalls = 0, sleepMs = 0;
uint32_t Netplay_GetRTT() { ++rttQueries; return fakeRTT; }
DWORD GetTickCount() { return 100; }
void HookLog(const char*, ...) {}
void Sleep(unsigned ms) { ++sleepCalls; sleepMs += ms; }
'''
        cases = r'''
void Reset() {
    g_timesyncFrameCount = 0;
    g_localFrameAdvantage = g_remoteFrameAdvantage = 0;
    std::memset(g_localAdvHistory, 0, sizeof(g_localAdvHistory));
    std::memset(g_remoteAdvHistory, 0, sizeof(g_remoteAdvHistory));
    sleepCalls = sleepMs = rttQueries = 0;
}
int main() {
    // The reported localhost RTTs alone produced a ~10-frame fictitious lead.
    const int oldClient = 312 * 60 / 1000;
    const int oldHost = 645 * 60 / 1000;
    assert((oldHost - oldClient) / 2 == 10);
    // Replay the frame-60 log's window: local=17.25, remote=39 with RTT included.
    assert(int((39.0f - 17.25f) / 2 + 0.5f) == 11);
    Reset();
    fakeRTT = 312;
    for (int i = 0; i < TIMESYNC_WINDOW_SIZE; ++i) {
        g_netFrameCount = 60 + i;
        g_lastRemoteFrame = g_netFrameCount + (i < 30 ? -1 : 0);
        g_remoteFrameAdvantage = 1;
        TickTimeSync();
    }
    assert(sleepCalls == 40 && sleepMs == 40); // ~1-frame offset, not an 11 ms wait.
    for (uint32_t rtt : {0u, 17u, 312u, 645u, 10000u}) {
        for (int commonOffset : {-8, -1, 0, 2, 8}) {
            Reset();
            fakeRTT = rtt;
            for (int frame = 20; frame < 620; ++frame) {
                g_netFrameCount = frame;
                g_lastRemoteFrame = frame + commonOffset;
                g_remoteFrameAdvantage = commonOffset;
                TickTimeSync();
                assert(g_localFrameAdvantage == commonOffset);
            }
            assert(sleepCalls == 0 && rttQueries == 0);
        }
    }
    // A real four-frame lead still slows only the leading peer, regardless of RTT.
    for (int lead : {-4, 4}) {
        Reset();
        fakeRTT = lead > 0 ? 312 : 645;
        for (int frame = 20; frame < 620; ++frame) {
            g_netFrameCount = frame;
            g_lastRemoteFrame = frame - lead - 2;
            g_remoteFrameAdvantage = lead - 2;
            TickTimeSync();
        }
        if (lead > 0) assert(sleepCalls == 600 && sleepMs == 2400);
        else assert(sleepCalls == 0);
        assert(rttQueries == 0);
        // Once the lead is gone, pacing settles within the existing 40-frame window.
        for (int i = 0; i < TIMESYNC_WINDOW_SIZE; ++i) {
            g_lastRemoteFrame = g_netFrameCount;
            g_remoteFrameAdvantage = 0;
            TickTimeSync();
        }
        unsigned before = sleepCalls;
        TickTimeSync();
        assert(sleepCalls == before);
    }
    Reset();
    g_netFrameCount = 0; g_lastRemoteFrame = -1;
    TickTimeSync();
    assert(sleepCalls == 0 && g_timesyncFrameCount == 0);
}
'''
        with tempfile.TemporaryDirectory(prefix="netplay-timesync-", dir="/tmp/opencode") as tmp:
            tmp = Path(tmp)
            (tmp / "test.cpp").write_text("#include <initializer_list>\n" + mocks + declarations + pacing + cases)
            flags = os.environ.get("NETPLAY_TEST_FLAGS", "").split()
            subprocess.run(["g++", "-std=c++20", "-O2", *flags, str(tmp / "test.cpp"),
                            "-o", str(tmp / "test")], check=True)
            subprocess.run([str(tmp / "test")], check=True, timeout=30)

    def test_dispatch_and_pump(self):
        self.build_and_run(False)

    def test_real_enet_loopback(self):
        self.build_and_run(True)

    def build_and_run(self, real_enet):
        with tempfile.TemporaryDirectory(prefix="netplay-direct-", dir="/tmp/opencode") as tmp:
            tmp = Path(tmp)
            (tmp / "Windows.h").write_text(WINDOWS)
            (tmp / "test.cpp").write_text(PREAMBLE + ("" if real_enet else MOCK_ENET) +
                                          production() + (LOOPBACK if real_enet else CASES))
            flags = os.environ.get("NETPLAY_TEST_FLAGS", "").split()
            objects = []
            if real_enet:
                for name in ("callbacks", "compress", "host", "list", "packet", "peer", "protocol", "unix"):
                    obj = tmp / (name + ".o")
                    subprocess.run([
                        "gcc", "-O2", "-DHAS_SOCKLEN_T", "-DHAS_FCNTL", "-DHAS_POLL",
                        "-DHAS_INET_PTON", "-DHAS_INET_NTOP", "-DHAS_MSGHDR_FLAGS",
                        "-DHAS_GETADDRINFO", "-DHAS_GETNAMEINFO",
                        "-I" + str(ROOT / "enet-1.3.18/include"), *flags,
                        "-c", str(ROOT / "enet-1.3.18" / (name + ".c")), "-o", str(obj),
                    ], check=True)
                    objects.append(str(obj))
            subprocess.run([
                "g++", "-std=c++20", "-O2", "-pthread", *flags, "-I" + str(tmp),
                "-I" + str(ROOT / "UMVC3Hook/code"), "-I" + str(ROOT / "enet-1.3.18/include"),
                str(tmp / "test.cpp"), *objects, "-o", str(tmp / "test"),
            ], check=True)
            subprocess.run([str(tmp / "test")], check=True, timeout=30)


if __name__ == "__main__":
    unittest.main()
