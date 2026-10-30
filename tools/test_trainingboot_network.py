"""Run the production pre-battle network gate and dual handoff with mocked APIs."""

from pathlib import Path
import subprocess
import tempfile
import unittest

from test_d3d9reset import function

ROOT = Path(__file__).resolve().parents[1]

MOCKS = r'''
#include <atomic>
#include <cassert>
#include <cstdint>
#include <vector>
#define __fastcall
using ULONGLONG = uint64_t;
constexpr uintptr_t SGAMECONFIG_GET_INSTANCE = 1;
constexpr uint32_t PACKET_LAUNCH_TRAINING = 6;
struct TrainingBootConfig {
    int p1Chars[3] = {1,2,3}, p2Chars[3] = {4,5,6};
    bool autoJump[2] = {};
    uint8_t voiceLanguage[2][3] = {};
} g_bootConfig;
struct LaunchTrainingPacket {
    uint32_t type = 0, autoJumpValidMask = 0;
    uint8_t voiceValidMask = 0;
    uint32_t autoJump[2] = {};
    uint8_t voiceLanguage[2][3] = {};
};
bool g_networkRequired = false, g_networkConnected = false, g_networkFailed = false;
bool g_initialSettingsSynchronized = false, g_initialSettingsSent = false;
bool g_initialSettingsReceived = false;
ULONGLONG g_initialSettingsStartTick = 0, now = 1000;
bool active = false, hostRole = true, inboundReady = false, initSucceeds = true;
int popupCount = 0, disconnectCount = 0, settingsSent = 0;
int netplayInits = 0, launchPackets = 0, serviceCalls = 0;
int configReads = 0, voiceReads = 0;
uint8_t config[0x200] = {};
LaunchTrainingPacket inbound;
uint64_t GetConfig() { ++configReads; return reinterpret_cast<uint64_t>(config); }
uintptr_t _addr(uintptr_t address) {
    assert(address == SGAMECONFIG_GET_INSTANCE);
    return reinterpret_cast<uintptr_t>(&GetConfig);
}
ULONGLONG GetTickCount64() { return now; }
bool Netplay_IsActive() { return active; }
bool Netplay_IsHost() { return hostRole; }
void HookLog(const char*, ...) {}
void FrameStepper_ShowOpponentDisconnected() { ++popupCount; }
bool Netplay_SendPlayerDisconnect(uint32_t) { ++disconnectCount; return true; }
uint8_t GetCharacterVoiceLanguage(uint64_t, int character) { ++voiceReads; return character % 2; }
void Netplay_SendLaunchTrainingSettings(const LaunchTrainingPacket&) { ++settingsSent; }
bool Netplay_GetInboundLaunchTrainingSettings(LaunchTrainingPacket& packet) {
    if (!inboundReady) return false;
    packet = inbound;
    inboundReady = false;
    return true;
}
struct ENetHost {} server;
struct ENetPeer { void* data = nullptr; } peer;
struct ENetPacket { size_t dataLength; void* data; };
struct ENetEvent { int type; ENetPeer* peer; ENetPacket* packet; };
constexpr int ENET_EVENT_TYPE_CONNECT = 1, ENET_EVENT_TYPE_RECEIVE = 2;
constexpr int ENET_EVENT_TYPE_DISCONNECT = 3;
struct LaunchDialogConfig { ENetHost* enetHost = &server; ENetPeer* enetPeer = nullptr; } dialog;
std::atomic<bool> g_dualClientSpawned{true}, g_dualConnectionFailed{false};
std::atomic<ENetPeer*> g_dualConnectedPeer{nullptr};
int serviceResult = ENET_EVENT_TYPE_CONNECT;
ULONGLONG serviceElapsed = 100;
std::vector<ENetEvent> events;
size_t nextEvent = 0;
const char* g_autoConnectIp = "127.0.0.1";
int g_autoConnectPort = 7777;
bool ApplyTrainingLaunchPacket(const LaunchTrainingPacket& packet) {
    return packet.type == PACKET_LAUNCH_TRAINING;
}
const LaunchDialogConfig& GetLaunchDialogConfig() { return dialog; }
int enet_host_service(ENetHost*, ENetEvent* event, int) {
    ++serviceCalls;
    now += serviceElapsed;
    if (nextEvent < events.size()) {
        *event = events[nextEvent++];
        return 1;
    }
    if (serviceResult <= 0) return serviceResult;
    *event = {serviceResult, &peer, nullptr};
    return 1;
}
void enet_packet_destroy(void*) {}
void SetNetworkHandles(ENetHost* host, ENetPeer* connected) { dialog = {host, connected}; }
void SendTrainingLaunchPacket() { ++launchPackets; }
void TrainingBoot_NotifyNetworkConnected();
void FrameStepper_InitNetplay() {
    ++netplayInits;
    active = initSucceeds;
    if (active) TrainingBoot_NotifyNetworkConnected();
}
'''

CHECKS = r'''
void reset() {
    g_bootConfig = {};
    g_networkRequired = g_networkConnected = g_networkFailed = false;
    g_initialSettingsSynchronized = g_initialSettingsSent = g_initialSettingsReceived = false;
    g_initialSettingsStartTick = 0;
    active = inboundReady = false; hostRole = initSucceeds = true;
    popupCount = disconnectCount = settingsSent = netplayInits = launchPackets = serviceCalls = 0;
    now = 1000; config[0x192] = 1;
    configReads = voiceReads = 0;
    inbound = {}; dialog = {&server, nullptr};
    g_dualClientSpawned = true; g_dualConnectionFailed = false; g_dualConnectedPeer = nullptr;
    serviceResult = ENET_EVENT_TYPE_CONNECT;
    serviceElapsed = 100; events.clear(); nextEvent = 0;
}
void remoteSettings() {
    const int remote = hostRole ? 1 : 0;
    inbound.autoJumpValidMask = inbound.voiceValidMask = 1u << remote;
    inbound.voiceLanguage[remote][0] = 1;
    inboundReady = true;
}
int main() {
    reset();
    assert(SynchronizeInitialPlayerSettings()); // Intentional offline boot.
    assert(g_initialSettingsSynchronized && settingsSent == 0);
    assert(configReads == 1 && voiceReads == 6);
    assert(SynchronizeInitialPlayerSettings() && configReads == 1 && voiceReads == 6);

    reset();
    TrainingBoot_SetNetworkRequired(true);
    assert(!SynchronizeInitialPlayerSettings()); // Dual host is still connecting.
    assert(!g_initialSettingsSynchronized && !g_initialSettingsSent);
    WaitForDualTrainingClient();
    assert(g_dualConnectedPeer.load() == &peer && netplayInits == 0 && launchPackets == 0);
    const int workerServices = serviceCalls;
    UpdateLaunchNetworkConnection();
    assert(active && dialog.enetPeer == &peer && netplayInits == 1 && launchPackets == 1);
    assert(serviceCalls == workerServices);
    UpdateLaunchNetworkConnection();
    assert(netplayInits == 1); // Ownership handed off exactly once.
    assert(!SynchronizeInitialPlayerSettings() && settingsSent == 1);
    assert(configReads == 1 && voiceReads == 3);
    config[0x192] = 0;
    for (int i = 0; i < 120; ++i) {
        now += 16;
        assert(!SynchronizeInitialPlayerSettings());
    }
    assert(configReads == 1 && voiceReads == 3 && settingsSent == 1);
    assert(g_bootConfig.autoJump[0]); // Keep the settings that were actually sent.
    remoteSettings();
    assert(SynchronizeInitialPlayerSettings());
    active = false;
    assert(!SynchronizeInitialPlayerSettings()); // Even after settings were synchronized.
    assert(g_networkFailed && popupCount == 1);
    assert(!SynchronizeInitialPlayerSettings() && popupCount == 1);

    reset();
    TrainingBoot_SetNetworkRequired(true); // Steam drops before the first native config tick.
    active = true;
    TrainingBoot_NotifyNetworkConnected();
    active = false;
    assert(!SynchronizeInitialPlayerSettings() && g_networkFailed);
    assert(!g_initialSettingsSynchronized && settingsSent == 0);

    reset();
    TrainingBoot_SetNetworkRequired(true);
    hostRole = false; active = true;
    TrainingBoot_NotifyNetworkConnected();
    assert(!SynchronizeInitialPlayerSettings());
    remoteSettings();
    assert(SynchronizeInitialPlayerSettings()); // Steam/dual client settings exchange.

    reset();
    TrainingBoot_SetNetworkRequired(true);
    active = true;
    TrainingBoot_NotifyNetworkConnected();
    assert(!SynchronizeInitialPlayerSettings());
    now += 30000;
    assert(!SynchronizeInitialPlayerSettings() && g_networkFailed);
    assert(popupCount == 1 && disconnectCount == 1 && !g_initialSettingsSynchronized);

    for (int failure : {0, -1, ENET_EVENT_TYPE_DISCONNECT}) {
        reset();
        TrainingBoot_SetNetworkRequired(true);
        serviceResult = failure;
        WaitForDualTrainingClient();
        assert(g_dualConnectionFailed.load());
        UpdateLaunchNetworkConnection();
        assert(g_networkFailed && popupCount == 1 && netplayInits == 0);
        assert(!SynchronizeInitialPlayerSettings());
    }

    reset();
    TrainingBoot_SetNetworkRequired(true);
    initSucceeds = false;
    WaitForDualTrainingClient();
    UpdateLaunchNetworkConnection();
    assert(g_networkFailed && !SynchronizeInitialPlayerSettings());

    reset();
    TrainingBoot_SetNetworkRequired(true);
    active = true; now = 0;
    TrainingBoot_NotifyNetworkConnected();
    assert(!SynchronizeInitialPlayerSettings());
    now = 30000;
    assert(!SynchronizeInitialPlayerSettings() && g_networkFailed);

    reset();
    serviceElapsed = 0; serviceResult = 0;
    events.push_back({ENET_EVENT_TYPE_CONNECT, &peer, nullptr});
    uint32_t ignoredType = 0;
    ENetPacket ignored = {sizeof(ignoredType), &ignoredType};
    for (int i = 0; i < 200; ++i)
        events.push_back({ENET_EVENT_TYPE_RECEIVE, &peer, &ignored});
    LaunchTrainingPacket launch;
    launch.type = PACKET_LAUNCH_TRAINING;
    ENetPacket valid = {sizeof(launch), &launch};
    events.push_back({ENET_EVENT_TYPE_RECEIVE, &peer, &valid});
    assert(ReceiveClientLaunch()); // Immediate events must not consume 100 ms each.
    assert(serviceCalls == 202 && now == 1000);

    reset();
    serviceResult = 0;
    assert(!ReceiveClientLaunch());
    assert(now == 16000 && serviceCalls == 150);

    reset();
    serviceResult = -1;
    assert(!ReceiveClientLaunch() && serviceCalls == 1);

    reset();
    serviceResult = ENET_EVENT_TYPE_DISCONNECT;
    assert(!ReceiveClientLaunch() && serviceCalls == 1);
}
'''


class TrainingBootNetworkTests(unittest.TestCase):
    def test_production_network_gate_and_handoff(self):
        boot = (ROOT / "UMVC3Hook/code/trainingboot.cpp").read_text()
        dll = (ROOT / "UMVC3Hook/dllmain.cpp").read_text()
        code = "".join(function(boot, name) for name in [
            "TrainingBoot_SetNetworkRequired", "TrainingBoot_NotifyNetworkConnected",
            "TrainingBoot_FailNetwork", "IsBootNetworkReady", "SynchronizeInitialPlayerSettings"])
        code += function(dll, "WaitForDualTrainingClient")
        code += function(dll, "UpdateLaunchNetworkConnection")
        completion = function(dll, "CompleteLaunchInitialization")
        start = completion.index("const ULONGLONG started = GetTickCount64();")
        end = completion.index("if (!GetLaunchDialogConfig().enetPeer || !receivedLaunchConfig)", start)
        code += "bool ReceiveClientLaunch() { ENetHost* client = &server; ENetPeer* peer = &::peer; ENetEvent event;\n"
        code += completion[start:end] + "return receivedLaunchConfig; }\n"
        with tempfile.TemporaryDirectory(dir="/tmp/opencode", prefix="boot-network-") as directory:
            cpp = Path(directory) / "test.cpp"
            exe = Path(directory) / "test"
            cpp.write_text(MOCKS + code + CHECKS)
            subprocess.run(["g++", "-std=c++20", "-Wall", "-Wextra", str(cpp), "-o", str(exe)], check=True)
            subprocess.run([str(exe)], check=True, timeout=10)

    def test_boot_guards_and_intent_wiring(self):
        boot = (ROOT / "UMVC3Hook/code/trainingboot.cpp").read_text()
        body = function(boot, "BootIntoTraining")
        self.assertLess(body.index("UpdateLaunchNetworkConnection();"), body.index("LoadNativeGameConfig()"))
        self.assertLess(body.index("SynchronizeInitialPlayerSettings()"), body.index("g_hasBooted = true;"))
        self.assertLess(body.index("IsBootNetworkReady()"), body.index("g_hasBooted = true;"))
        dll = (ROOT / "UMVC3Hook/dllmain.cpp").read_text()
        completion = function(dll, "CompleteLaunchInitialization")
        self.assertIn("TrainingBoot_SetNetworkRequired(opt == LAUNCH_DUAL_TRAINING ||", completion)
        self.assertIn("GetLaunchDialogConfig().hostEnabled", completion)
        self.assertLess(completion.index("TrainingBoot_SetNetworkRequired("), completion.index("enet_host_create("))
        worker = function(dll, "WaitForDualTrainingClient")
        self.assertNotIn("FrameStepper_InitNetplay();", worker)
        self.assertNotIn("SetNetworkHandles(", worker)
        frame = (ROOT / "UMVC3Hook/code/framestepper.cpp").read_text()
        self.assertIn("TrainingBoot_NotifyNetworkConnected();", function(frame, "FrameStepper_InitNetplay"))


if __name__ == "__main__":
    unittest.main()
