#include "../pch.h"
#include "lobby_local.h"

#include <Windows.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <vector>

extern void HookLog(const char* fmt, ...);

// Local test lobby: a stand-in for the Steam lobby that only spans game
// processes on this PC. Only in developer builds (kLobbyTestBuild). One room lives in named shared memory; every process
// started in this mode sees it, can create or join it, and plays its
// matches over ENet on localhost. steam_lobby.cpp forwards the calls the
// in-game lobby makes to the functions here, so the room logic above it
// (lobby_room.cpp) runs unchanged.

namespace {

constexpr uint32_t kMagic = 0x544C4C34; // layout version; bump on any change
constexpr int kMaxMembers = 16;
constexpr int kMaxRoomKeys = 16;
constexpr int kMaxMemberKeys = 4;
constexpr int kKeySize = 16;
constexpr int kRoomValueSize = 1280; // the ban list is the longest value
constexpr int kMemberValueSize = 200;
constexpr int kChatEntries = 64;
constexpr int kChatSize = 160;
constexpr ULONGLONG kMemberTimeoutMs = 4000;
constexpr uint64_t kIdBase = 0x0110000100000000ull; // shaped like a SteamID
// The match connection between two test instances: one fixed port, so it can
// be watched or shaped with ordinary network tools.
constexpr uint16_t kMatchPort = 7777;

struct KeyValue {
    char key[kKeySize];
    char value[kRoomValueSize];
};
struct MemberKeyValue {
    char key[kKeySize];
    char value[kMemberValueSize];
};
struct SharedMember {
    uint64_t id; // 0 = free row
    ULONGLONG heartbeat;
    char name[64];
    MemberKeyValue data[kMaxMemberKeys];
};
struct SharedChat {
    uint64_t sender;
    int32_t size;
    char data[kChatSize];
};
struct SharedRoom {
    uint32_t magic;
    uint64_t lobbyId; // 0 = no room
    uint64_t owner;
    int32_t memberLimit;
    KeyValue data[kMaxRoomKeys];
    SharedMember members[kMaxMembers];
    uint64_t chatCount; // total messages ever sent to this room
    SharedChat chat[kChatEntries];
};

bool g_child = false;
HANDLE g_mapping = nullptr;
HANDLE g_mutex = nullptr;
SharedRoom* g_room = nullptr;
uint64_t g_localId = 0;
char g_localName[64] = "Tester";
uint64_t g_joinedLobby = 0; // room this process is a member of
uint64_t g_chatCursor = 0;
bool g_resultsChanged = false;
bool g_childSpawned = false;

ENetHost* g_matchHost = nullptr;
ENetPeer* g_matchPeer = nullptr;
bool g_matchIsHost = false;
bool g_matchFailed = false;
char g_peerName[64] = {};

struct Lock {
    Lock() { if (g_mutex) WaitForSingleObject(g_mutex, 2000); }
    ~Lock() { if (g_mutex) ReleaseMutex(g_mutex); }
};

// Callers keep the returned pointers across a few more calls, as they may
// with Steam's own strings.
char* ScratchBuffer()
{
    static char buffers[8][kRoomValueSize];
    static int next = 0;
    char* buffer = buffers[next];
    next = (next + 1) % 8;
    buffer[0] = '\0';
    return buffer;
}

SharedMember* FindMember(uint64_t id)
{
    if (!g_room || !id) return nullptr;
    for (SharedMember& member : g_room->members)
        if (member.id == id) return &member;
    return nullptr;
}

int MemberCount()
{
    int count = 0;
    for (const SharedMember& member : g_room->members) count += member.id != 0;
    return count;
}

const char* RoomValue(const char* key)
{
    for (const KeyValue& entry : g_room->data)
        if (entry.key[0] && strcmp(entry.key, key) == 0) return entry.value;
    return "";
}

void SetRoomValue(const char* key, const char* value)
{
    KeyValue* target = nullptr;
    for (KeyValue& entry : g_room->data) {
        if (entry.key[0] && strcmp(entry.key, key) == 0) {
            target = &entry;
            break;
        }
        if (!entry.key[0] && !target) target = &entry;
    }
    if (!target) return;
    snprintf(target->key, sizeof(target->key), "%s", key);
    snprintf(target->value, sizeof(target->value), "%s", value ? value : "");
}

// Drops members whose process stopped pumping and hands the room on, the way
// Steam does when a member disconnects. Lock held.
void Housekeep()
{
    if (!g_room->lobbyId) return;
    const ULONGLONG now = GetTickCount64();
    for (SharedMember& member : g_room->members) {
        if (member.id && member.id != g_localId && now - member.heartbeat > kMemberTimeoutMs)
            member = {};
    }
    if (MemberCount() == 0) {
        g_room->lobbyId = 0;
        return;
    }
    if (!FindMember(g_room->owner)) {
        uint64_t lowest = 0;
        for (const SharedMember& member : g_room->members)
            if (member.id && (!lowest || member.id < lowest)) lowest = member.id;
        g_room->owner = lowest;
    }
}

bool InRoomLocked()
{
    return g_room && g_joinedLobby && g_room->lobbyId == g_joinedLobby && FindMember(g_localId);
}

bool AddSelfLocked()
{
    for (SharedMember& member : g_room->members) {
        if (member.id) continue;
        member = {};
        member.id = g_localId;
        member.heartbeat = GetTickCount64();
        snprintf(member.name, sizeof(member.name), "%s", g_localName);
        g_joinedLobby = g_room->lobbyId;
        g_chatCursor = g_room->chatCount;
        return true;
    }
    return false;
}

void DestroyMatchHost()
{
    if (g_matchHost) enet_host_destroy(g_matchHost);
    g_matchHost = nullptr;
    g_matchPeer = nullptr;
}

} // namespace

void LocalLobby_Enable(int instance)
{
    g_child = instance > 0;
    g_localId = kIdBase + GetCurrentProcessId();
    static const char* const names[] = {"Tester One", "Tester Two", "Tester Three",
                                        "Tester Four", "Tester Five", "Tester Six"};
    snprintf(g_localName, sizeof(g_localName), "%s",
             names[instance < 0 ? 0 : instance > 5 ? 5 : instance]);
    HookLog("LocalLobby: test lobby enabled (%s, id=%llu)\n", g_localName,
            static_cast<unsigned long long>(g_localId));
}

bool LocalLobby_IsChild() { return g_child; }

void LocalLobby_SpawnSecondInstance()
{
    if (!LocalLobby_Enabled() || g_child || g_childSpawned) return;
    g_childSpawned = true;
    wchar_t exe[MAX_PATH] = {};
    if (!GetModuleFileNameW(nullptr, exe, MAX_PATH)) return;
    for (int instance = 1; instance <= kLobbyTestExtraInstances; ++instance) {
        wchar_t command[MAX_PATH + 64];
        swprintf(command, MAX_PATH + 64, L"\"%ls\" -lobbytest-child=%d", exe, instance);
        STARTUPINFOW startup = {sizeof(startup)};
        PROCESS_INFORMATION process = {};
        if (CreateProcessW(exe, command, nullptr, nullptr, FALSE, CREATE_NEW_CONSOLE, nullptr,
                           nullptr, &startup, &process)) {
            CloseHandle(process.hProcess);
            CloseHandle(process.hThread);
            HookLog("LocalLobby: started game instance %d\n", instance + 1);
        } else {
            HookLog("LocalLobby: could not start instance %d (error %lu)\n", instance + 1,
                    GetLastError());
        }
    }
}

bool LocalLobby_Initialize()
{
    if (g_room) return true;
    g_mutex = CreateMutexA(nullptr, FALSE, "Local\\TimestoneTestLobbyMutex");
    g_mapping = CreateFileMappingA(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
                                   sizeof(SharedRoom), "Local\\TimestoneTestLobby");
    if (!g_mutex || !g_mapping) {
        HookLog("LocalLobby: could not create shared state (error %lu)\n", GetLastError());
        return false;
    }
    g_room = static_cast<SharedRoom*>(
        MapViewOfFile(g_mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(SharedRoom)));
    if (!g_room) return false;
    Lock lock;
    if (g_room->magic != kMagic) {
        memset(g_room, 0, sizeof(*g_room));
        g_room->magic = kMagic;
    }
    Housekeep();
    return true;
}

void LocalLobby_Pump()
{
    if (!g_room) return;
    Lock lock;
    if (SharedMember* self = g_joinedLobby ? FindMember(g_localId) : nullptr)
        self->heartbeat = GetTickCount64();
    Housekeep();
}

const char* LocalLobby_GetStatus() { return "Local test lobby (no Steam)"; }
const char* LocalLobby_GetLocalUsername() { return g_localName; }
uint64_t LocalLobby_GetLocalId() { return g_localId; }

// --- Browsing ---------------------------------------------------------------
void LocalLobby_Search() { g_resultsChanged = true; }

bool LocalLobby_TakeResultsChanged()
{
    const bool changed = g_resultsChanged;
    g_resultsChanged = false;
    return changed;
}

size_t LocalLobby_GetResultCount()
{
    if (!g_room) return 0;
    Lock lock;
    Housekeep();
    return g_room->lobbyId && g_room->lobbyId != g_joinedLobby ? 1 : 0;
}

uint64_t LocalLobby_GetResult()
{
    if (!g_room) return 0;
    Lock lock;
    return g_room->lobbyId;
}

const char* LocalLobby_GetResultName()
{
    return LocalLobby_GetLobbyData(LocalLobby_GetResult(), "name");
}

const char* LocalLobby_GetResultUsername()
{
    if (!g_room) return "";
    Lock lock;
    const SharedMember* owner = FindMember(g_room->owner);
    char* out = ScratchBuffer();
    snprintf(out, kRoomValueSize, "%s", owner ? owner->name : "");
    return out;
}

int LocalLobby_GetResultMemberCount()
{
    if (!g_room) return 0;
    Lock lock;
    return MemberCount();
}

int LocalLobby_GetResultMemberLimit()
{
    if (!g_room) return 0;
    Lock lock;
    return g_room->memberLimit;
}

int LocalLobby_GetResultSlotCount()
{
    const int slots = atoi(LocalLobby_GetLobbyData(LocalLobby_GetResult(), "slots"));
    return slots < 1 ? 1 : slots;
}

bool LocalLobby_IsResultInMatch()
{
    return strcmp(LocalLobby_GetLobbyData(LocalLobby_GetResult(), "live"), "1") == 0;
}

// --- Membership ---------------------------------------------------------------
bool LocalLobby_IsActive()
{
    if (!g_room) return false;
    Lock lock;
    return InRoomLocked();
}

uint64_t LocalLobby_GetLobbyId()
{
    return LocalLobby_IsActive() ? g_joinedLobby : 0;
}

void LocalLobby_Create(int maxMembers, int slotCount, const char* name)
{
    if (!g_room) return;
    Lock lock;
    Housekeep();
    if (g_room->lobbyId) {
        HookLog("LocalLobby: a test room already exists; join it instead\n");
        return;
    }
    memset(g_room->data, 0, sizeof(g_room->data));
    memset(g_room->members, 0, sizeof(g_room->members));
    g_room->lobbyId = kIdBase + (GetTickCount64() & 0xFFFFFFF) + 1;
    g_room->owner = g_localId;
    g_room->memberLimit = maxMembers < 2 ? 2 : maxMembers > kMaxMembers ? kMaxMembers : maxMembers;
    char slots[8];
    snprintf(slots, sizeof(slots), "%d", slotCount);
    char fallback[96];
    snprintf(fallback, sizeof(fallback), "%s's Lobby", g_localName);
    SetRoomValue("name", name && name[0] ? name : fallback);
    SetRoomValue("slots", slots);
    AddSelfLocked();
    HookLog("LocalLobby: created test room %llu\n", static_cast<unsigned long long>(g_room->lobbyId));
}

void LocalLobby_Join(uint64_t lobbyId)
{
    if (!g_room || !lobbyId) return;
    Lock lock;
    Housekeep();
    if (g_room->lobbyId != lobbyId || FindMember(g_localId)) return;
    if (MemberCount() >= g_room->memberLimit || !AddSelfLocked()) {
        HookLog("LocalLobby: test room is full\n");
        return;
    }
    HookLog("LocalLobby: joined test room %llu\n", static_cast<unsigned long long>(lobbyId));
}

void LocalLobby_Leave()
{
    LocalLobby_SpecCloseAll();
    if (!g_room) return;
    Lock lock;
    if (SharedMember* self = FindMember(g_localId)) *self = {};
    g_joinedLobby = 0;
    Housekeep();
}

int LocalLobby_GetMemberCount()
{
    if (!g_room) return 0;
    Lock lock;
    return InRoomLocked() ? MemberCount() : 0;
}

uint64_t LocalLobby_GetMember(int index)
{
    if (!g_room) return 0;
    Lock lock;
    for (const SharedMember& member : g_room->members) {
        if (!member.id) continue;
        if (index-- == 0) return member.id;
    }
    return 0;
}

uint64_t LocalLobby_GetOwner()
{
    if (!g_room) return 0;
    Lock lock;
    return InRoomLocked() ? g_room->owner : 0;
}

int LocalLobby_GetMemberLimit()
{
    if (!g_room) return 0;
    Lock lock;
    return g_room->memberLimit;
}

const char* LocalLobby_GetMemberName(uint64_t memberId)
{
    if (!g_room) return "";
    Lock lock;
    const SharedMember* member = FindMember(memberId);
    char* out = ScratchBuffer();
    snprintf(out, kRoomValueSize, "%s", member ? member->name : "");
    return out;
}

const char* LocalLobby_GetMemberData(uint64_t memberId, const char* key)
{
    if (!g_room || !key) return "";
    Lock lock;
    char* out = ScratchBuffer();
    if (const SharedMember* member = FindMember(memberId)) {
        for (const MemberKeyValue& entry : member->data)
            if (entry.key[0] && strcmp(entry.key, key) == 0)
                snprintf(out, kRoomValueSize, "%s", entry.value);
    }
    return out;
}

void LocalLobby_SetLocalData(const char* key, const char* value)
{
    if (!g_room || !key) return;
    Lock lock;
    SharedMember* self = InRoomLocked() ? FindMember(g_localId) : nullptr;
    if (!self) return;
    MemberKeyValue* target = nullptr;
    for (MemberKeyValue& entry : self->data) {
        if (entry.key[0] && strcmp(entry.key, key) == 0) {
            target = &entry;
            break;
        }
        if (!entry.key[0] && !target) target = &entry;
    }
    if (!target) return;
    snprintf(target->key, sizeof(target->key), "%s", key);
    snprintf(target->value, sizeof(target->value), "%s", value ? value : "");
}

const char* LocalLobby_GetData(const char* key)
{
    return LocalLobby_IsActive() ? LocalLobby_GetLobbyData(g_joinedLobby, key) : "";
}

const char* LocalLobby_GetLobbyData(uint64_t lobbyId, const char* key)
{
    if (!g_room || !key || !lobbyId) return "";
    Lock lock;
    char* out = ScratchBuffer();
    if (g_room->lobbyId == lobbyId) snprintf(out, kRoomValueSize, "%s", RoomValue(key));
    return out;
}

void LocalLobby_SetData(const char* key, const char* value)
{
    if (!g_room || !key) return;
    Lock lock;
    // As with Steam, only the owner may write the room's data.
    if (InRoomLocked() && g_room->owner == g_localId) SetRoomValue(key, value);
}

// --- Chat ---------------------------------------------------------------------
bool LocalLobby_SendChat(const void* data, int size)
{
    if (!g_room || !data || size <= 0) return false;
    Lock lock;
    if (!InRoomLocked()) return false;
    SharedChat& entry = g_room->chat[g_room->chatCount % kChatEntries];
    entry.sender = g_localId;
    entry.size = size > kChatSize ? kChatSize : size;
    memcpy(entry.data, data, static_cast<size_t>(entry.size));
    ++g_room->chatCount;
    return true;
}

bool LocalLobby_PollChat(uint64_t* sender, char* buffer, int capacity, int* size)
{
    if (!g_room || !sender || !buffer || !size || capacity <= 0) return false;
    Lock lock;
    if (!InRoomLocked() || g_chatCursor >= g_room->chatCount) return false;
    // Fell more than a ring behind: skip what was overwritten.
    if (g_room->chatCount - g_chatCursor > kChatEntries)
        g_chatCursor = g_room->chatCount - kChatEntries;
    const SharedChat& entry = g_room->chat[g_chatCursor++ % kChatEntries];
    *sender = entry.sender;
    *size = entry.size < capacity ? entry.size : capacity;
    memcpy(buffer, entry.data, static_cast<size_t>(*size));
    return true;
}

// --- Match connection -----------------------------------------------------------
// The player-1 side listens on localhost port 7777 and the other side dials
// it. One match at a time: a second pairing on this PC (three or more test
// windows and two match slots) cannot open the port while the first holds it.
bool LocalLobby_ConnectPeer(uint64_t memberId, bool asHost)
{
    DestroyMatchHost();
    g_matchIsHost = asHost;
    g_matchFailed = false;
    snprintf(g_peerName, sizeof(g_peerName), "%s", LocalLobby_GetMemberName(memberId));
    const uint16_t port = kMatchPort;
    if (asHost) {
        ENetAddress address = {};
        address.host = ENET_HOST_ANY;
        address.port = port;
        g_matchHost = enet_host_create(&address, 1, 2, 0, 0);
    } else {
        g_matchHost = enet_host_create(nullptr, 1, 2, 0, 0);
        if (g_matchHost) {
            ENetAddress target = {};
            enet_address_set_host(&target, "127.0.0.1");
            target.port = port;
            if (!enet_host_connect(g_matchHost, &target, 2, 0)) DestroyMatchHost();
        }
    }
    if (!g_matchHost) {
        HookLog("LocalLobby: could not open the match connection on port %u\n", port);
        g_matchFailed = true;
        return false;
    }
    HookLog("LocalLobby: match connection %s on 127.0.0.1:%u\n", asHost ? "listening" : "dialling",
            port);
    return true;
}

bool LocalLobby_TakeNetworkHandles(ENetHost** outHost, ENetPeer** outPeer, bool* outIsHost)
{
    if (!g_matchHost || !outHost || !outPeer || !outIsHost) return false;
    ENetEvent event;
    while (!g_matchPeer && enet_host_service(g_matchHost, &event, 0) > 0) {
        if (event.type == ENET_EVENT_TYPE_CONNECT) {
            event.peer->data = (void*)"connected";
            g_matchPeer = event.peer;
        } else if (event.type == ENET_EVENT_TYPE_RECEIVE) {
            enet_packet_destroy(event.packet);
        } else if (event.type == ENET_EVENT_TYPE_DISCONNECT && !g_matchIsHost) {
            // The other side was not listening yet; the caller's timeout or
            // the next pairing attempt retries.
            g_matchFailed = true;
        }
    }
    if (!g_matchPeer) return false;
    *outHost = g_matchHost;
    *outPeer = g_matchPeer;
    *outIsHost = g_matchIsHost;
    // The game owns (and later destroys) the host from here on.
    g_matchHost = nullptr;
    g_matchPeer = nullptr;
    return true;
}

void LocalLobby_DisconnectPeer()
{
    DestroyMatchHost();
    g_matchFailed = false;
}

bool LocalLobby_TakePeerFailed()
{
    const bool failed = g_matchFailed;
    g_matchFailed = false;
    if (failed) DestroyMatchHost();
    return failed;
}

const char* LocalLobby_GetPeerUsername() { return g_peerName; }

// --- Spectator links ------------------------------------------------------------
// Every process listens; of two members the one with the lower id dials, and
// says who it is in the connect data. Both ends "open" the link, as with
// Steam, so the listening end knows which callers it is expecting.
namespace {

constexpr uint16_t kSpecPortBase = 29100;
constexpr int kMaxSpecLinks = 8;
constexpr ULONGLONG kSpecConnectTimeoutMs = 10000;

struct SpecLink {
    uint64_t id = 0;
    ENetPeer* peer = nullptr;
    int state = 0;
    ULONGLONG deadline = 0;
    ULONGLONG nextDial = 0;
};
struct SpecMessage {
    uint64_t from;
    std::vector<uint8_t> data;
};
ENetHost* g_specHost = nullptr;
SpecLink g_specLinks[kMaxSpecLinks];
// Callers we have not been asked to open a link to yet.
struct SpecCaller {
    uint64_t id;
    ENetPeer* peer;
};
SpecCaller g_specCallers[kMaxSpecLinks] = {};
std::deque<SpecMessage> g_specInbox;

uint16_t SpecPort(uint64_t id)
{
    return static_cast<uint16_t>(kSpecPortBase + (id - kIdBase) % 2000);
}

bool EnsureSpecHost()
{
    if (g_specHost) return true;
    ENetAddress address = {};
    address.host = ENET_HOST_ANY;
    address.port = SpecPort(g_localId);
    g_specHost = enet_host_create(&address, 2 * kMaxSpecLinks, 1, 0, 0);
    if (!g_specHost)
        HookLog("LocalLobby: could not open the spectator port %u\n", address.port);
    return g_specHost != nullptr;
}

SpecLink* FindSpecLink(uint64_t id)
{
    if (!id) return nullptr;
    for (SpecLink& link : g_specLinks)
        if (link.id == id) return &link;
    return nullptr;
}

void DialSpecLink(SpecLink& link)
{
    ENetAddress target = {};
    enet_address_set_host(&target, "127.0.0.1");
    target.port = SpecPort(link.id);
    link.peer = enet_host_connect(g_specHost, &target, 1,
                                  static_cast<enet_uint32>(g_localId - kIdBase));
    if (link.peer) link.peer->data = nullptr;
    link.nextDial = GetTickCount64() + 2000;
}

} // namespace

void LocalLobby_SpecOpen(uint64_t memberId)
{
    if (!memberId || memberId == g_localId || FindSpecLink(memberId) || !EnsureSpecHost()) return;
    for (SpecLink& link : g_specLinks) {
        if (link.id) continue;
        link = SpecLink{};
        link.id = memberId;
        link.state = 1;
        link.deadline = GetTickCount64() + kSpecConnectTimeoutMs;
        if (g_localId < memberId) {
            DialSpecLink(link);
        } else {
            for (SpecCaller& caller : g_specCallers) {
                if (caller.id != memberId) continue;
                link.peer = caller.peer;
                link.state = 2;
                caller = {};
            }
        }
        return;
    }
}

void LocalLobby_SpecClose(uint64_t memberId)
{
    if (SpecLink* link = FindSpecLink(memberId)) {
        if (link->peer) enet_peer_disconnect(link->peer, 0);
        *link = SpecLink{};
    }
}

void LocalLobby_SpecCloseAll()
{
    for (SpecLink& link : g_specLinks) {
        if (link.id && link.peer) enet_peer_disconnect(link.peer, 0);
        link = SpecLink{};
    }
    if (g_specHost) enet_host_flush(g_specHost);
    g_specInbox.clear();
}

int LocalLobby_SpecState(uint64_t memberId)
{
    const SpecLink* link = FindSpecLink(memberId);
    return link ? link->state : 0;
}

bool LocalLobby_SpecSend(uint64_t memberId, const void* data, int size)
{
    const SpecLink* link = FindSpecLink(memberId);
    if (!link || link->state != 2 || !link->peer || !data || size <= 0) return false;
    ENetPacket* packet = enet_packet_create(data, static_cast<size_t>(size), ENET_PACKET_FLAG_RELIABLE);
    if (!packet) return false;
    if (enet_peer_send(link->peer, 0, packet) != 0) {
        enet_packet_destroy(packet);
        return false;
    }
    return true;
}

bool LocalLobby_SpecReceive(uint64_t* memberId, void* buffer, int capacity, int* size)
{
    while (!g_specInbox.empty()) {
        SpecMessage message = std::move(g_specInbox.front());
        g_specInbox.pop_front();
        if (static_cast<int>(message.data.size()) > capacity) continue;
        memcpy(buffer, message.data.data(), message.data.size());
        *memberId = message.from;
        *size = static_cast<int>(message.data.size());
        return true;
    }
    return false;
}

void LocalLobby_SpecPump()
{
    if (!LocalLobby_Enabled() || !g_localId || !EnsureSpecHost()) return;
    const ULONGLONG now = GetTickCount64();
    ENetEvent event;
    while (enet_host_service(g_specHost, &event, 0) > 0) {
        if (event.type == ENET_EVENT_TYPE_CONNECT) {
            // Our own dial completing, or someone calling us.
            SpecLink* link = nullptr;
            for (SpecLink& candidate : g_specLinks)
                if (candidate.id && candidate.peer == event.peer) link = &candidate;
            if (!link) {
                const uint64_t caller = kIdBase + event.data;
                link = FindSpecLink(caller);
                if (link && link->state == 1 && !link->peer) {
                    link->peer = event.peer;
                } else {
                    link = nullptr;
                    for (SpecCaller& slot : g_specCallers) {
                        if (slot.id && slot.id != caller) continue;
                        slot = {caller, event.peer};
                        break;
                    }
                }
            }
            // Loading screens can leave the socket unserviced for seconds.
            enet_peer_timeout(event.peer, 0, 30000, 60000);
            if (link) {
                link->state = 2;
                HookLog("LocalLobby: spectator link to %llu connected\n",
                        static_cast<unsigned long long>(link->id));
            }
        } else if (event.type == ENET_EVENT_TYPE_RECEIVE) {
            for (const SpecLink& link : g_specLinks) {
                if (!link.id || link.peer != event.peer) continue;
                SpecMessage message;
                message.from = link.id;
                message.data.assign(event.packet->data, event.packet->data + event.packet->dataLength);
                g_specInbox.push_back(std::move(message));
            }
            enet_packet_destroy(event.packet);
        } else if (event.type == ENET_EVENT_TYPE_DISCONNECT) {
            for (SpecCaller& slot : g_specCallers)
                if (slot.peer == event.peer) slot = {};
            for (SpecLink& link : g_specLinks) {
                if (!link.id || link.peer != event.peer) continue;
                link.peer = nullptr;
                // An unanswered dial is retried below; a dropped link is over.
                if (link.state == 2) link.state = 3;
            }
        }
    }
    for (SpecLink& link : g_specLinks) {
        if (!link.id || link.state != 1) continue;
        if (now >= link.deadline) {
            if (link.peer) enet_peer_reset(link.peer);
            link.peer = nullptr;
            link.state = 3;
        } else if (!link.peer && g_localId < link.id && now >= link.nextDial) {
            DialSpecLink(link);
        }
    }
}
