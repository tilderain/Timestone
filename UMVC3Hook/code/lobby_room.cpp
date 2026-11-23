#include "../pch.h"
#include "lobby_room.h"
#include "lobby_music.h"
#include "lobby_native.h"
#include "LaunchMenu.h"
#include "steam_lobby.h"

#include <Windows.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>

extern void HookLog(const char* fmt, ...);

namespace lobby {

namespace {

constexpr char kMemberKey[] = "ts";      // per-member room state
// Per-member lobby icon, on its own key so the member string keeps its
// format: a decimal number, kIconSteam or a character id.
constexpr char kIconKey[] = "ti";
// Per-member preferred stage, likewise: a decimal number, 0 for no
// preference, else the game's stage id + 1. Anything else reads as 0.
constexpr char kStageKey[] = "tg";

// A stage id as published by another player: one of the game's stages, or
// kStageAny for anything else.
int KnownStage(int id)
{
    for (int i = 0; i < kStageCount; ++i)
        if (kStages[i].id == id) return id;
    return kStageAny;
}
constexpr char kMusicKey[] = "bgm";      // lobby data: the host's music choice
// Lobby data read by the lobby list: the host's Steam id (decimal) and icon.
constexpr char kHostIdKey[] = "hid";
constexpr char kHostIconKey[] = "hicon";
// Lobby data: SteamIDs the host removed, as comma-separated hex. Steam has no
// kick, so the list is the kick: a listed client leaves by itself, every
// other client drops listed members from its room, and Join refuses a lobby
// that lists the local player. It lives and dies with the lobby.
constexpr char kBanKey[] = "ban";
constexpr char kFirstToKey[] = "ft";     // lobby data: games needed to win a set
constexpr char kVisibilityKey[] = "vis"; // lobby data: "1" for a friends-only room
constexpr char kRotationKey[] = "rot";   // lobby data: Rotation, as a number
constexpr int kMaxBans = 64;
constexpr int kMemberVersion = 7;
constexpr uint8_t kChatText = 'C';       // lobby chat payload: 'C' + UTF-8
constexpr uint32_t kRefreshInterval = 6; // frames between room rebuilds

SteamLobbyType ToSteamType(Visibility visibility)
{
    switch (visibility) {
    case Visibility::Private: return SteamLobbyType::Private;
    case Visibility::FriendsOnly: return SteamLobbyType::FriendsOnly;
    default: return SteamLobbyType::Public;
    }
}

bool IsBanned(const char* list, uint64_t id)
{
    if (!list || !id) return false;
    for (const char* cursor = list; *cursor;) {
        char* end = nullptr;
        const uint64_t entry = strtoull(cursor, &end, 16);
        if (end == cursor) break;
        if (entry == id) return true;
        cursor = *end == ',' ? end + 1 : end;
    }
    return false;
}

// A missing or bad value means first to 1; "0" is a real choice (endless).
int ParseFirstTo(const char* text)
{
    if (!text || text[0] < '0' || text[0] > '9') return 1;
    const int value = atoi(text);
    return value > kMaxFirstTo ? 1 : value;
}

bool IsPlausibleTeam(const Team& team)
{
    for (int i = 0; i < 3; ++i) {
        const bool character = team.chars[i] == kCharRandom ||
                               (team.chars[i] >= 1 && team.chars[i] <= 50);
        if (!character || team.assists[i] < 0 || team.assists[i] > 2 || team.colors[i] < 0 ||
            team.colors[i] > 7)
            return false;
    }
    return true;
}

// Member state travels as one short string so a member's seat, readiness and
// team always change atomically for observers.
void EncodeMember(char* out, size_t size, int slot, int side, bool ready, MemberStatus status,
                  bool accepted, int delayPick, const Team& team, int wins, int losses,
                  int setWins, int queueSlot, uint32_t queueSeq, int delayHover,
                  const uint8_t health[3], int watchSlot, uint32_t watchSeq, uint64_t upstream)
{
    snprintf(out, size,
             "%d|%d|%d|%d|%d|%d|%d|%d,%d,%d|%d,%d,%d|%d,%d,%d|%d|%d|%d|%d|%u|%d|%d,%d,%d|%d|%u|%u,%u",
             kMemberVersion, slot, side, ready ? 1 : 0, static_cast<int>(status),
             accepted ? 1 : 0, delayPick,
             team.chars[0], team.chars[1], team.chars[2], team.assists[0], team.assists[1],
             team.assists[2], team.colors[0], team.colors[1], team.colors[2], wins, losses,
             setWins, queueSlot, queueSeq, delayHover, health[0], health[1], health[2],
             watchSlot, watchSeq, static_cast<unsigned>(upstream >> 32),
             static_cast<unsigned>(upstream & 0xFFFFFFFFu));
}

bool DecodeMember(const char* text, Member& member)
{
    int version = 0, slot = -1, side = 0, ready = 0, status = 0, accepted = 0, wins = 0, losses = 0;
    int setWins = 0, delayPick = kDelayPickNone, queueSlot = -1, delayHover = kDelayPickNone;
    unsigned queueSeq = 0;
    int health[3] = {kHealthUnknown, kHealthUnknown, kHealthUnknown};
    int watchSlot = -1;
    unsigned watchSeq = 0;
    unsigned upstreamHigh = 0, upstreamLow = 0;
    Team team = {};
    if (!text || sscanf(text, "%d|%d|%d|%d|%d|%d|%d|%d,%d,%d|%d,%d,%d|%d,%d,%d|%d|%d|%d|%d|%u|%d|%d,%d,%d|%d|%u|%u,%u", &version,
                        &slot, &side, &ready, &status, &accepted, &delayPick, &team.chars[0],
                        &team.chars[1],
                        &team.chars[2], &team.assists[0], &team.assists[1], &team.assists[2],
                        &team.colors[0], &team.colors[1], &team.colors[2], &wins, &losses,
                        &setWins, &queueSlot, &queueSeq, &delayHover, &health[0], &health[1],
                        &health[2], &watchSlot, &watchSeq, &upstreamHigh, &upstreamLow) != 29)
        return false;
    if (version != kMemberVersion) return false;
    if (slot < -1 || slot >= kMaxSlots || side < 0 || side > 2 || status < 0 ||
        status > static_cast<int>(MemberStatus::Watching) || delayPick < kDelayPickNone ||
        delayPick > kMaxDelayPick)
        return false;
    member.slot = static_cast<int8_t>(slot);
    member.side = static_cast<int8_t>(side);
    member.ready = ready != 0;
    member.status = static_cast<MemberStatus>(status);
    member.accepted = accepted != 0;
    member.delayPick = static_cast<int8_t>(delayPick);
    member.delayHover = static_cast<int8_t>(
        delayHover < kDelayPickNone || delayHover > kMaxDelayPick ? kDelayPickNone : delayHover);
    // Seated players are not waiting for anything.
    member.queueSlot = static_cast<int8_t>(slot >= 0 || queueSlot < 0 || queueSlot >= kMaxSlots ? -1 : queueSlot);
    member.queueSeq = queueSeq;
    // Nobody watches from a seat.
    member.watchSlot = static_cast<int8_t>(slot >= 0 || watchSlot < 0 || watchSlot >= kMaxSlots ? -1 : watchSlot);
    member.watchSeq = watchSeq;
    member.upstream = member.watchSlot >= 0
        ? (static_cast<uint64_t>(upstreamHigh) << 32) | upstreamLow : 0;
    for (int i = 0; i < 3; ++i)
        member.health[i] = static_cast<uint8_t>(health[i] < 0 || health[i] > 100 ? kHealthUnknown : health[i]);
    member.team = team;
    member.wins = static_cast<int16_t>(wins < 0 ? 0 : wins > 999 ? 999 : wins);
    member.losses = static_cast<int16_t>(losses < 0 ? 0 : losses > 999 ? 999 : losses);
    member.setWins = static_cast<int8_t>(setWins < 0 ? 0 : setWins > kMaxFirstTo ? kMaxFirstTo : setWins);
    return true;
}

} // namespace

SteamRoomService::SteamRoomService()
{
    available_ = SteamLobby_Initialize();
    if (available_) SteamRoom_SetMode(true);
}

const char* SteamRoomService::StatusText() const
{
    return SteamLobby_GetStatus();
}

const char* SteamRoomService::LocalName() const
{
    const char* name = SteamLobby_GetLocalUsername();
    return name && name[0] ? name : "Player";
}

RoomBusy SteamRoomService::Busy() const
{
    switch (SteamLobby_GetPendingKind()) {
    case 1: return RoomBusy::Searching;
    case 2: return RoomBusy::Creating;
    case 3: return RoomBusy::Joining;
    default: return RoomBusy::None;
    }
}

const SearchResult& SteamRoomService::Result(int index) const
{
    static const SearchResult empty = {};
    return index >= 0 && index < resultCount_ ? results_[index] : empty;
}

const ChatLine& SteamRoomService::Chat(int index) const
{
    static const ChatLine empty = {};
    return index >= 0 && index < chatCount_ ? chat_[(chatHead_ + index) % kMaxChatLines] : empty;
}

void SteamRoomService::SetError(const char* text)
{
    snprintf(error_, sizeof(error_), "%s", text ? text : "");
}

bool SteamRoomService::TakeError(char* message, size_t capacity)
{
    if (!error_[0] || !message || !capacity) return false;
    snprintf(message, capacity, "%s", error_);
    error_[0] = '\0';
    return true;
}

void SteamRoomService::AddChat(const char* sender, const char* text, bool system)
{
    // A ring: the oldest line makes way once the history is full.
    if (chatCount_ == kMaxChatLines) {
        chatHead_ = (chatHead_ + 1) % kMaxChatLines;
        --chatCount_;
    }
    ChatLine& line = chat_[(chatHead_ + chatCount_++) % kMaxChatLines];
    line = {};
    snprintf(line.sender, sizeof(line.sender), "%s", sender ? sender : "");
    snprintf(line.text, sizeof(line.text), "%s", text ? text : "");
    line.system = system;
}

void SteamRoomService::PostSystemLine(const char* text)
{
    AddChat("", text, true);
}

// ---------------------------------------------------------------------------
// Browsing and membership
// ---------------------------------------------------------------------------
void SteamRoomService::Search()
{
    if (!available_) return;
    SteamLobby_Search();
}

void SteamRoomService::Create(const RoomSettings& settings)
{
    if (!available_ || room_.inRoom) return;
    creating_ = true;
    pendingMusic_ = settings.music;
    pendingFirstTo_ = settings.firstTo;
    pendingVisibility_ = static_cast<int>(settings.visibility);
    pendingRotation_ = static_cast<int>(settings.rotation);
    SteamRoom_Create(ToSteamType(settings.visibility), settings.maxMembers, settings.slotCount,
                     settings.name);
}

void SteamRoomService::Join(uint64_t roomId)
{
    if (!available_ || !roomId) return;
    if (IsBanned(SteamRoom_GetLobbyData(roomId, kBanKey), SteamRoom_GetLocalId())) {
        SetError("You were removed from this lobby by its host\nand cannot join it again.");
        return;
    }
    joining_ = true;
    pendingMusic_ = -1;
    pendingFirstTo_ = -1;
    pendingVisibility_ = -1;
    pendingRotation_ = -1;
    SteamLobby_Join(roomId);
}

void SteamRoomService::Leave()
{
    SteamRoom_Leave();
    local_.slot = -1;
    local_.side = 0;
    ClearHandshake();
    handshakeOpponent_ = 0;
    local_.queueSlot = -1;
    local_.watchSlot = -1;
    local_.upstream = 0;
    watchIntent_ = -1;
    resumeWatch_ = -1;
    local_.status = MemberStatus::Idle;
    local_.wins = 0;
    local_.losses = 0;
    local_.setWins = 0;
    room_ = {};
    publishedOnce_ = false;
    liveAdvertised_ = false;
    knownMemberCount_ = 0;
    chatCount_ = 0;
    chatHead_ = 0;
}

void SteamRoomService::InviteFriends()
{
    if (!SteamRoom_InviteFriends())
        SetError("The Steam overlay is not available.\nInvite from your Steam friends list instead.");
}

void SteamRoomService::CopyLobbyLink()
{
    char link[128] = {};
    bool copied = false;
    if (SteamRoom_GetJoinLink(link, sizeof(link))) {
        const size_t size = strlen(link) + 1;
        if (HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, size)) {
            if (void* data = GlobalLock(memory)) {
                memcpy(data, link, size);
                GlobalUnlock(memory);
                if (OpenClipboard(nullptr)) {
                    copied = EmptyClipboard() && SetClipboardData(CF_TEXT, memory) != nullptr;
                    CloseClipboard();
                }
            }
            if (!copied) GlobalFree(memory); // the clipboard owns it otherwise
        }
    }
    if (copied) PostSystemLine("Lobby link copied.");
    else SetError("Could not copy the lobby link.\nPlease try again.");
}

// The choice lives in the lobby's own data, so Steam delivers it to everyone
// in the room and to anyone who joins later, and it survives the host leaving.
void SteamRoomService::SetMusic(int32_t trackId)
{
    if (!room_.inRoom || SteamRoom_GetOwner() != SteamRoom_GetLocalId()) return;
    if (FindMusicTrack(trackId) < 0) return;
    char value[16];
    snprintf(value, sizeof(value), "%d", trackId);
    SteamRoom_SetData(kMusicKey, value);
    room_.music = trackId; // Steam echoes it back a moment later
}

void SteamRoomService::ViewProfile(uint64_t memberId)
{
    if (!SteamRoom_ViewProfile(memberId))
        SetError("The Steam overlay is not available.");
}

void SteamRoomService::Kick(uint64_t memberId)
{
    const uint64_t localId = SteamRoom_GetLocalId();
    if (!room_.inRoom || !memberId || memberId == localId || SteamRoom_GetOwner() != localId)
        return;
    char list[kMaxBans * 17 + 32];
    snprintf(list, sizeof(list), "%s", SteamRoom_GetData(kBanKey));
    if (IsBanned(list, memberId)) return;
    int count = list[0] ? 1 : 0;
    for (const char* c = list; *c; ++c) count += *c == ',';
    if (count >= kMaxBans) {
        // Oldest entry makes room; it is far more likely to be long gone.
        const char* comma = strchr(list, ',');
        memmove(list, comma ? comma + 1 : list + strlen(list), comma ? strlen(comma + 1) + 1 : 1);
    }
    const size_t used = strlen(list);
    snprintf(list + used, sizeof(list) - used, "%s%llx", used ? "," : "",
             static_cast<unsigned long long>(memberId));
    char line[kChatCapacity];
    snprintf(line, sizeof(line), "%.48s was removed from the lobby.", SteamRoom_GetMemberName(memberId));
    SteamRoom_SetData(kBanKey, list);
    // Already announced here; the membership difference must not repeat it.
    for (int i = 0; i < knownMemberCount_; ++i) {
        if (knownMembers_[i] != memberId) continue;
        knownMembers_[i] = knownMembers_[--knownMemberCount_];
        snprintf(knownNames_[i], sizeof(knownNames_[i]), "%s", knownNames_[knownMemberCount_]);
        break;
    }
    AddChat("", line, true);
    RefreshRoom();
}

// ---------------------------------------------------------------------------
// Local seat, readiness and team
// ---------------------------------------------------------------------------
void SteamRoomService::PublishLocal()
{
    if (!SteamRoom_IsActive()) return;
    char encoded[192];
    EncodeMember(encoded, sizeof(encoded), local_.slot, local_.side, local_.ready, local_.status,
                 local_.accepted, local_.delayPick, local_.team, local_.wins, local_.losses,
                 local_.setWins, local_.queueSlot, local_.queueSeq, local_.delayHover,
                 local_.health, local_.watchSlot, local_.watchSeq, local_.upstream);
    SteamRoom_SetLocalData(kMemberKey, encoded);
    publishedOnce_ = true;
}

void SteamRoomService::Sit(int slot, int side)
{
    if (!room_.inRoom || !IsValidSeat(room_.settings, slot, side)) return;
    if (local_.status == MemberStatus::Fighting) return;
    local_.slot = slot;
    local_.side = side;
    local_.queueSlot = -1;
    local_.watchSlot = -1; // a seat ends any watching
    local_.upstream = 0;
    // Only for as long as the seat is held: watching is the player's standing
    // choice, and it picks up again when they stand or are rotated out.
    if (watchIntent_ >= 0) resumeWatch_ = watchIntent_;
    watchIntent_ = -1;
    ClearHandshake();
    local_.status = MemberStatus::Seated;
    PublishLocal();
    RefreshRoom();
}

void SteamRoomService::Stand()
{
    if (local_.status == MemberStatus::Fighting) return;
    if (local_.slot >= 0 && resumeWatch_ >= 0) watchIntent_ = resumeWatch_;
    resumeWatch_ = -1;
    local_.slot = -1;
    local_.side = 0;
    ClearHandshake();
    local_.status = MemberStatus::Idle;
    PublishLocal();
    RefreshRoom();
}

int SteamRoomService::QueueLength(int slot) const
{
    int length = 0;
    for (int i = 0; i < room_.memberCount; ++i) length += room_.members[i].queueSlot == slot;
    return length;
}

void SteamRoomService::JoinQueue(int slot)
{
    if (!room_.inRoom || slot < 0 || slot >= room_.settings.slotCount || local_.slot >= 0 ||
        local_.status == MemberStatus::Fighting)
        return;
    // One past the highest number anyone in the room holds: later than all of
    // them, without relying on anyone's clock.
    uint32_t last = 0;
    for (int i = 0; i < room_.memberCount; ++i)
        if (room_.members[i].queueSlot >= 0 && room_.members[i].queueSeq > last)
            last = room_.members[i].queueSeq;
    local_.queueSlot = slot;
    local_.queueSeq = last + 1;
    PublishLocal();
    RefreshRoom();
}

void SteamRoomService::LeaveQueue()
{
    if (local_.queueSlot < 0) return;
    // Out of the line is out altogether: no watching either, whichever setup
    // it was for and whether or not a match is being joined right now.
    watchIntent_ = -1;
    resumeWatch_ = -1;
    local_.queueSlot = -1;
    if (local_.watchSlot >= 0) {
        local_.watchSlot = -1;
        local_.upstream = 0;
        if (local_.status == MemberStatus::Watching) local_.status = MemberStatus::Idle;
    }
    PublishLocal();
    RefreshRoom();
}

void SteamRoomService::Watch(int slot)
{
    if (!room_.inRoom || slot < 0 || slot >= room_.settings.slotCount || local_.slot >= 0 ||
        local_.status == MemberStatus::Fighting)
        return;
    // Later than everyone already watching, by the same clockless rule as
    // the queues: a spectator is only ever fed by one who came before.
    uint32_t last = 0;
    for (int i = 0; i < room_.memberCount; ++i)
        if (room_.members[i].watchSlot >= 0 && room_.members[i].watchSeq > last)
            last = room_.members[i].watchSeq;
    local_.watchSlot = slot;
    local_.watchSeq = last + 1;
    local_.upstream = 0;
    local_.status = MemberStatus::Watching;
    PublishLocal();
    RefreshRoom();
}

void SteamRoomService::StopWatching()
{
    if (local_.watchSlot < 0) return;
    local_.watchSlot = -1;
    local_.upstream = 0;
    if (local_.status == MemberStatus::Watching) local_.status = MemberStatus::Idle;
    PublishLocal();
    RefreshRoom();
}

void SteamRoomService::SetUpstream(uint64_t memberId)
{
    if (local_.watchSlot < 0 || local_.upstream == memberId) return;
    local_.upstream = memberId;
    PublishLocal();
}

// The head of a slot's queue takes a seat there as soon as one is open. Each
// client only ever moves its own player.
void SteamRoomService::RunQueue()
{
    if (!room_.inRoom || local_.queueSlot < 0 || local_.slot >= 0) return;
    const int slot = local_.queueSlot;
    if (slot >= room_.settings.slotCount) {
        LeaveQueue();
        return;
    }
    if (room_.slots[slot] == SlotState::InProgress) return;
    bool taken[2] = {false, false};
    int position = 0;
    for (int i = 0; i < room_.memberCount; ++i) {
        const Member& member = room_.members[i];
        if (member.slot == slot && member.side < 2) taken[member.side] = true;
        if (member.isLocal) position = member.queuePos;
    }
    const int open = (taken[0] ? 0 : 1) + (taken[1] ? 0 : 1);
    // Two open seats seat the first two in line; one seats only the first.
    if (open == 0 || position < 1 || position > open) return;
    const int side = position == 1 ? (taken[0] ? 1 : 0) : 1;
    if (taken[side]) return;
    HookLog("LobbyRoom: queue reached the front for setup %d; taking seat %d\n", slot + 1, side + 1);
    Sit(slot, side);
}

void SteamRoomService::SetReady(bool ready)
{
    if (local_.slot < 0 || local_.status == MemberStatus::Fighting) return;
    local_.ready = ready;
    local_.status = ready ? MemberStatus::Ready : MemberStatus::Seated;
    PublishLocal();
    RefreshRoom();
}

void SteamRoomService::SetTeam(const Team& team)
{
    local_.team = team;
    PublishLocal();
}

void SteamRoomService::SetStage(int stage)
{
    local_.stage = KnownStage(stage);
    if (!SteamRoom_IsActive()) return;
    char value[8];
    snprintf(value, sizeof(value), "%d", local_.stage + 1);
    SteamRoom_SetLocalData(kStageKey, value);
    RefreshRoom();
}

void SteamRoomService::SetIcon(int icon)
{
    local_.icon = icon < kIconSteam || icon > kIconMax ? kIconSteam : icon;
    if (!SteamRoom_IsActive()) return;
    char value[8];
    snprintf(value, sizeof(value), "%d", local_.icon);
    SteamRoom_SetLocalData(kIconKey, value);
    PublishHostIcon(); // the lobby list shows the host's icon
    RefreshRoom();
}

Tex SteamRoomService::AvatarTex(uint64_t memberId) const
{
    return Native_AvatarTex(memberId ? memberId : SteamRoom_GetLocalId());
}

int SteamRoomService::AutoDelay(int pingMs) const
{
    if (pingMs < 0) return -1;
    int32_t delay = 0, rollback = 0;
    LaunchMenu_GetAutomaticNetplaySettings(static_cast<uint32_t>(pingMs), &delay, &rollback);
    return delay;
}

void SteamRoomService::ClearHandshake()
{
    local_.accepted = false;
    local_.delayPick = kDelayPickNone;
    local_.delayHover = kDelayPickNone;
    local_.ready = false;
    if (local_.status == MemberStatus::Ready) local_.status = MemberStatus::Seated;
}

void SteamRoomService::AcceptMatch()
{
    if (local_.slot < 0 || local_.status == MemberStatus::Fighting || local_.accepted) return;
    local_.accepted = true;
    PublishLocal();
    RefreshRoom();
}

void SteamRoomService::HoverDelay(int pick)
{
    // Published before the pick too: choosing a delay is how a player readies up.
    if (local_.slot < 0 || local_.delayHover == pick) return;
    local_.delayHover = pick < kDelayPickAuto ? kDelayPickAuto : pick > kMaxDelayPick ? kMaxDelayPick : pick;
    PublishLocal();
}

void SteamRoomService::PickDelay(int pick)
{
    if (local_.slot < 0 || local_.status == MemberStatus::Fighting || !local_.accepted) return;
    local_.delayPick = pick < kDelayPickAuto ? kDelayPickAuto : pick > kMaxDelayPick ? kMaxDelayPick : pick;
    PublishLocal();
    RefreshRoom();
}

void SteamRoomService::SetLocalStatus(MemberStatus status)
{
    if (local_.status == status) return;
    local_.status = status;
    PublishLocal();
    RefreshRoom();
}

void SteamRoomService::FinishMatch(bool counted, bool localWon)
{
    if (counted) {
        if (localWon) ++local_.wins;
        else ++local_.losses;
    }
    local_.setWins = 0;
    memset(local_.health, kHealthUnknown, sizeof(local_.health));
    ClearHandshake();
    handshakeOpponent_ = 0; // a rematch with the same player starts over too
    measuredPeer_ = 0;
    measuredPing_ = -1;
    local_.status = local_.slot >= 0 ? MemberStatus::Seated : MemberStatus::Idle;
    // A finished set rotates one of the two players out when others are
    // waiting for this slot. The one who moves does it for themselves.
    const int slot = local_.slot;
    const Rotation rotation = room_.settings.rotation;
    const bool rotateOut = counted && slot >= 0 && QueueLength(slot) > 0 &&
                           ((rotation == Rotation::WinnerStays && !localWon) ||
                            (rotation == Rotation::WinnerLeaves && localWon));
    PublishLocal();
    RefreshRoom();
    if (rotateOut) {
        Stand();
        JoinQueue(slot);
        AddChat("", "You moved to the back of the queue.", true);
    }
}

void SteamRoomService::ReportHealth(const uint8_t percent[3])
{
    if (memcmp(local_.health, percent, sizeof(local_.health)) == 0) return;
    memcpy(local_.health, percent, sizeof(local_.health));
    PublishLocal();
}

void SteamRoomService::SetMeasuredPing(uint64_t memberId, int rttMs)
{
    if (measuredPeer_ == memberId && measuredPing_ == rttMs) return;
    measuredPeer_ = memberId;
    measuredPing_ = rttMs;
    for (int i = 0; i < room_.memberCount; ++i)
        if (room_.members[i].id == memberId && rttMs >= 0)
            room_.members[i].pingMs = static_cast<int16_t>(rttMs > 9999 ? 9999 : rttMs);
}

void SteamRoomService::ReportSetScore(int localGamesWon)
{
    if (local_.setWins == localGamesWon) return;
    local_.setWins = localGamesWon;
    PublishLocal();
}

void SteamRoomService::SendChat(const char* utf8)
{
    if (!room_.inRoom || !utf8 || !utf8[0]) return;
    uint8_t payload[1 + kChatCapacity] = {};
    payload[0] = kChatText;
    const size_t length = strlen(utf8) < kChatCapacity - 1 ? strlen(utf8) : kChatCapacity - 1;
    memcpy(payload + 1, utf8, length);
    // Our own message comes back through the chat callback like everyone's.
    if (!SteamRoom_SendChat(payload, static_cast<int>(length + 1)))
        AddChat("", "Message could not be sent.", true);
}

void SteamRoomService::DrainChat()
{
    uint64_t sender = 0;
    char buffer[1 + kChatCapacity + 4];
    int size = 0;
    while (SteamRoom_PollChat(&sender, buffer, sizeof(buffer) - 1, &size)) {
        if (size < 2 || static_cast<uint8_t>(buffer[0]) != kChatText) continue;
        buffer[size < static_cast<int>(sizeof(buffer)) ? size : sizeof(buffer) - 1] = '\0';
        // Drop control characters; names and text are untrusted input.
        for (char* c = buffer + 1; *c; ++c)
            if (static_cast<unsigned char>(*c) < 0x20) *c = ' ';
        AddChat(SteamRoom_GetMemberName(sender), buffer + 1, false);
    }
}

// ---------------------------------------------------------------------------
// Derived state
// ---------------------------------------------------------------------------
void SteamRoomService::RefreshResults()
{
    resultCount_ = 0;
    const size_t count = SteamLobby_GetResultCount();
    for (size_t i = 0; i < count && resultCount_ < kMaxSearchResults; ++i) {
        SearchResult& result = results_[resultCount_++];
        result = {};
        result.id = SteamLobby_GetResult(i);
        snprintf(result.name, sizeof(result.name), "%s", SteamLobby_GetResultName(i));
        snprintf(result.host, sizeof(result.host), "%s", SteamLobby_GetResultUsername(i));
        if (!result.name[0]) snprintf(result.name, sizeof(result.name), "%.48s's Lobby", result.host);
        result.members = SteamLobby_GetResultMemberCount(i);
        result.maxMembers = SteamLobby_GetResultMemberLimit(i);
        if (result.maxMembers <= 0) result.maxMembers = kMaxMembers;
        // Someone entering a full room is counted until they leave again.
        if (result.members > result.maxMembers) result.members = result.maxMembers;
        result.slotCount = SteamLobby_GetResultSlotCount(i);
        const int estimate = SteamLobby_GetResultEstimatedPing(i);
        result.pingMs = estimate >= 0 ? estimate : -1;
        result.compatible = SteamLobby_IsLobbyVersionCompatible(result.id);
        if (!result.compatible)
            result.versionOrder = static_cast<int8_t>(SteamLobby_FormatVersionMismatch(
                SteamLobby_GetResultProtocol(i), result.versionNotice, sizeof(result.versionNotice)));
        result.friendLobby = SteamLobby_IsFriendLobby(result.id);
        result.matchInProgress = SteamLobby_IsResultInMatch(i);
        const char* visibility = SteamRoom_GetLobbyData(result.id, kVisibilityKey);
        result.friendsOnly = visibility && strcmp(visibility, "1") == 0;
        result.firstTo = ParseFirstTo(SteamRoom_GetLobbyData(result.id, kFirstToKey));
        // Written by the lobby's host, so untrusted: the id is only ever
        // handed to Steam to look up a picture, and the icon is a number
        // that must be one of the known icons.
        const char* hostId = SteamRoom_GetLobbyData(result.id, kHostIdKey);
        bool digits = hostId && hostId[0];
        for (const char* c = hostId; digits && *c; ++c) digits = *c >= '0' && *c <= '9';
        result.hostId = digits && strlen(hostId) <= 20 ? strtoull(hostId, nullptr, 10) : 0;
        const char* hostIcon = SteamRoom_GetLobbyData(result.id, kHostIconKey);
        const int icon = hostIcon ? atoi(hostIcon) : kIconSteam;
        result.hostIcon = static_cast<int16_t>(icon < kIconSteam || icon > kIconMax ? kIconSteam : icon);
    }
    // What the lobby list is showing, whenever that set changes.
    static uint64_t listed = ~0ull;
    uint64_t signature = static_cast<uint64_t>(resultCount_);
    for (int i = 0; i < resultCount_; ++i)
        signature = signature * 1099511628211ull ^ results_[i].id ^
                    (static_cast<uint64_t>(results_[i].members) << 56);
    if (signature != listed) {
        listed = signature;
        HookLog("LobbyRoom: lobby list shows %d\n", resultCount_);
        for (int i = 0; i < resultCount_; ++i)
            HookLog("LobbyRoom:   %llu \"%s\" %d/%d compatible=%d friend=%d\n",
                    static_cast<unsigned long long>(results_[i].id), results_[i].name,
                    results_[i].members, results_[i].maxMembers, results_[i].compatible ? 1 : 0,
                    results_[i].friendLobby ? 1 : 0);
    }
}

// The lobby's owner keeps their Steam id and icon in the lobby's data, where
// the lobby list can read them without joining. A new owner (the host left)
// takes this over on their next pump.
void SteamRoomService::PublishHostIcon()
{
    if (!SteamRoom_IsActive() || SteamRoom_GetOwner() != SteamRoom_GetLocalId()) return;
    const uint64_t room = SteamRoom_GetLobbyId();
    if (hostIconRoom_ == room && hostIconPublished_ == local_.icon) return;
    char value[24];
    snprintf(value, sizeof(value), "%llu", static_cast<unsigned long long>(SteamRoom_GetLocalId()));
    SteamRoom_SetData(kHostIdKey, value);
    snprintf(value, sizeof(value), "%d", local_.icon);
    SteamRoom_SetData(kHostIconKey, value);
    hostIconRoom_ = room;
    hostIconPublished_ = local_.icon;
}

void SteamRoomService::RefreshRoom()
{
    RoomState next = {};
    if (!SteamRoom_IsActive()) {
        room_ = next;
        return;
    }
    next.inRoom = true;
    next.id = SteamRoom_GetLobbyId();
    snprintf(next.settings.name, sizeof(next.settings.name), "%s", SteamRoom_GetData("name"));
    next.settings.maxMembers = SteamRoom_GetMemberLimit();
    if (next.settings.maxMembers < 2 || next.settings.maxMembers > kMaxMembers)
        next.settings.maxMembers = kMaxMembers;
    next.settings.slotCount = atoi(SteamRoom_GetData("slots"));
    if (next.settings.slotCount < 1) next.settings.slotCount = 1;
    if (next.settings.slotCount > kMaxSlots) next.settings.slotCount = kMaxSlots;
    next.settings.firstTo = ParseFirstTo(SteamRoom_GetData(kFirstToKey));
    next.settings.rotation = atoi(SteamRoom_GetData(kRotationKey)) == 2 ? Rotation::WinnerLeaves
                                                                         : Rotation::WinnerStays;
    next.music = atoi(SteamRoom_GetData(kMusicKey));
    if (FindMusicTrack(next.music) < 0) next.music = kMusicDefault;

    const uint64_t owner = SteamRoom_GetOwner();
    const uint64_t localId = SteamRoom_GetLocalId();
    char bans[kMaxBans * 17 + 32];
    snprintf(bans, sizeof(bans), "%s", SteamRoom_GetData(kBanKey));
    if (owner != localId && IsBanned(bans, localId) && local_.status != MemberStatus::Fighting) {
        // The host removed us (or we got in past the join check, e.g. through
        // an invite). A match in progress is allowed to finish first.
        HookLog("LobbyRoom: removed from room %llu by its host\n",
                static_cast<unsigned long long>(next.id));
        Leave();
        SetError("You were removed from the lobby by the host.");
        return;
    }
    const int count = SteamRoom_GetMemberCount();
    for (int i = 0; i < count && next.memberCount < kMaxMembers; ++i) {
        const uint64_t id = SteamRoom_GetMember(i);
        if (!id) continue;
        // A removed player's client leaves by itself; until it has (or if it
        // never does), nobody else treats them as part of the room.
        if (id != owner && id != localId && IsBanned(bans, id)) continue;
        Member& member = next.members[next.memberCount++];
        member = {};
        member.id = id;
        member.slot = -1;
        member.isHost = id == owner;
        member.isLocal = id == localId;
        snprintf(member.name, sizeof(member.name), "%s", SteamRoom_GetMemberName(id));
        if (member.isLocal) {
            // Our own state is authoritative locally; Steam echoes it later.
            member.slot = static_cast<int8_t>(local_.slot);
            member.side = static_cast<int8_t>(local_.side);
            member.ready = local_.ready;
            member.status = local_.status;
            member.accepted = local_.accepted;
            member.delayPick = static_cast<int8_t>(local_.delayPick);
            member.delayHover = static_cast<int8_t>(local_.delayHover);
            member.team = local_.team;
            member.icon = static_cast<int16_t>(local_.icon);
            member.stage = static_cast<int16_t>(local_.stage);
            member.wins = static_cast<int16_t>(local_.wins);
            member.losses = static_cast<int16_t>(local_.losses);
            member.setWins = static_cast<int8_t>(local_.setWins);
            member.queueSlot = static_cast<int8_t>(local_.slot >= 0 ? -1 : local_.queueSlot);
            member.queueSeq = local_.queueSeq;
            member.watchSlot = static_cast<int8_t>(local_.slot >= 0 ? -1 : local_.watchSlot);
            member.watchSeq = local_.watchSeq;
            member.upstream = local_.upstream;
            memcpy(member.health, local_.health, sizeof(member.health));
            member.pingMs = 0;
        } else {
            if (!DecodeMember(SteamRoom_GetMemberData(id, kMemberKey), member) ||
                !IsPlausibleTeam(member.team)) {
                // Not published yet (or malformed): present but not seated.
                member.slot = -1;
                member.ready = false;
                member.accepted = false;
                member.delayPick = kDelayPickNone;
                member.delayHover = kDelayPickNone;
                member.queueSlot = -1;
                member.watchSlot = -1;
                member.upstream = 0;
                memset(member.health, kHealthUnknown, sizeof(member.health));
                member.status = MemberStatus::Idle;
            }
            // Another player's choice is only ever a small number: anything
            // outside the known icons is shown as their Steam picture.
            const char* iconText = SteamRoom_GetMemberData(id, kIconKey);
            const int icon = iconText ? atoi(iconText) : kIconSteam;
            member.icon = static_cast<int16_t>(icon < kIconSteam || icon > kIconMax ? kIconSteam : icon);
            const char* stageText = SteamRoom_GetMemberData(id, kStageKey);
            member.stage = static_cast<int16_t>(KnownStage(stageText ? atoi(stageText) - 1 : kStageAny));
            if (member.slot >= next.settings.slotCount) member.slot = -1;
            if (member.watchSlot >= next.settings.slotCount) member.watchSlot = -1;
            if (member.queueSlot >= next.settings.slotCount) member.queueSlot = -1;
            member.pingMs = static_cast<int16_t>(SteamRoom_EstimateMemberPing(id));
            // A live connection to this member knows better than the estimate.
            if (id == measuredPeer_ && measuredPing_ >= 0)
                member.pingMs = static_cast<int16_t>(measuredPing_ > 9999 ? 9999 : measuredPing_);
        }
    }

    // Two members can claim one seat in the same instant. Everyone applies the
    // same rule, so the room converges without an arbiter: lowest SteamID sits.
    for (int a = 0; a < next.memberCount; ++a) {
        Member& first = next.members[a];
        if (first.slot < 0 || first.side > 1) continue;
        for (int b = 0; b < next.memberCount; ++b) {
            Member& second = next.members[b];
            if (a == b || second.slot != first.slot || second.side != first.side) continue;
            Member& loser = first.id < second.id ? second : first;
            loser.slot = -1;
            loser.ready = false;
            loser.status = MemberStatus::Idle;
            if (loser.isLocal && local_.slot >= 0 && local_.status != MemberStatus::Fighting) {
                local_.slot = -1;
                ClearHandshake();
                local_.status = MemberStatus::Idle;
                PublishLocal();
                AddChat("", "That seat was taken by another player.", true);
            }
        }
    }

    // The pre-match steps belong to one pairing. If the opposite seat empties
    // or someone else takes it, the local player starts over with them.
    if (local_.slot >= 0 && local_.status != MemberStatus::Fighting) {
        uint64_t opponent = 0;
        for (int i = 0; i < next.memberCount; ++i) {
            const Member& member = next.members[i];
            if (!member.isLocal && member.slot == local_.slot && member.side < 2 &&
                member.side != local_.side)
                opponent = member.id;
        }
        if (opponent != handshakeOpponent_) {
            handshakeOpponent_ = opponent;
            if (local_.accepted || local_.ready || local_.delayPick != kDelayPickNone) {
                ClearHandshake();
                PublishLocal();
                for (int i = 0; i < next.memberCount; ++i) {
                    Member& member = next.members[i];
                    if (!member.isLocal) continue;
                    member.accepted = false;
                    member.delayPick = kDelayPickNone;
                    member.ready = false;
                    member.status = local_.status;
                }
            }
        }
    } else if (local_.slot < 0) {
        handshakeOpponent_ = 0;
    }

    // Queue positions: per slot, by joining order, SteamID breaking ties. Every
    // client sorts the same published values, so all agree who is next.
    for (int i = 0; i < next.memberCount; ++i) {
        Member& member = next.members[i];
        member.queuePos = 0;
        if (member.queueSlot < 0) continue;
        int ahead = 0;
        for (int j = 0; j < next.memberCount; ++j) {
            const Member& other = next.members[j];
            if (j == i || other.queueSlot != member.queueSlot) continue;
            ahead += other.queueSeq < member.queueSeq ||
                     (other.queueSeq == member.queueSeq && other.id < member.id);
        }
        member.queuePos = static_cast<int8_t>(ahead + 1);
    }

    bool anyLive = false;
    for (int slot = 0; slot < next.settings.slotCount; ++slot) {
        int seated = 0, ready = 0, fighting = 0;
        for (int i = 0; i < next.memberCount; ++i) {
            const Member& member = next.members[i];
            if (member.slot != slot || member.side > 1) continue;
            ++seated;
            ready += member.ready ? 1 : 0;
            fighting += member.status == MemberStatus::Fighting ? 1 : 0;
        }
        next.slots[slot] = fighting == 2 ? SlotState::InProgress
                         : seated == 2 && ready == 2 ? SlotState::Starting
                         : seated == 2 ? SlotState::Standby
                         : seated == 1 ? SlotState::Waiting : SlotState::Open;
        anyLive |= next.slots[slot] == SlotState::InProgress;
    }

    // Join/leave notices from the membership difference.
    if (room_.inRoom && room_.id == next.id) {
        for (int i = 0; i < next.memberCount; ++i) {
            bool known = false;
            for (int j = 0; j < knownMemberCount_; ++j)
                known |= knownMembers_[j] == next.members[i].id;
            if (!known && !next.members[i].isLocal) {
                char line[kChatCapacity];
                snprintf(line, sizeof(line), "%.48s joined the lobby.", next.members[i].name);
                AddChat("", line, true);
            }
        }
        for (int j = 0; j < knownMemberCount_; ++j) {
            bool present = false;
            for (int i = 0; i < next.memberCount; ++i)
                present |= knownMembers_[j] == next.members[i].id;
            if (!present) {
                char line[kChatCapacity];
                snprintf(line, sizeof(line), "%.48s left the lobby.", knownNames_[j]);
                AddChat("", line, true);
            }
        }
    }
    knownMemberCount_ = next.memberCount;
    for (int i = 0; i < next.memberCount; ++i) {
        knownMembers_[i] = next.members[i].id;
        snprintf(knownNames_[i], sizeof(knownNames_[i]), "%s", next.members[i].name);
    }

    // The owner advertises whether a match is running, for the lobby browser.
    if (owner == localId && anyLive != liveAdvertised_) {
        SteamRoom_SetData("live", anyLive ? "1" : "0");
        liveAdvertised_ = anyLive;
    }
    room_ = next;
}

bool SteamRoomService::FindPairing(MatchPairing& pairing) const
{
    if (!room_.inRoom || local_.slot < 0 || local_.status == MemberStatus::Fighting)
        return false;
    for (int i = 0; i < room_.memberCount; ++i) {
        const Member& member = room_.members[i];
        if (member.isLocal || member.slot != local_.slot || member.side > 1 ||
            member.side == local_.side || member.status == MemberStatus::Fighting)
            continue;
        pairing = {};
        pairing.slot = local_.slot;
        pairing.localSide = local_.side;
        pairing.opponentId = member.id;
        snprintf(pairing.opponentName, sizeof(pairing.opponentName), "%s", member.name);
        pairing.localTeam = local_.team;
        pairing.opponentTeam = member.team;
        pairing.localStage = local_.stage;
        pairing.opponentStage = member.stage;
        pairing.localDelayPick = local_.delayPick;
        pairing.opponentDelayPick = member.delayPick;
        pairing.bothReady = local_.ready && member.ready;
        return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// Per-frame pump
// ---------------------------------------------------------------------------
void SteamRoomService::Pump()
{
    ++frame_;
    if (!available_) {
        // Steam may come up late (overlay, slow client start); retry quietly.
        if (frame_ >= nextInitAttempt_) {
            nextInitAttempt_ = frame_ + 300;
            available_ = SteamLobby_Initialize();
            if (available_) SteamRoom_SetMode(true);
        }
        return;
    }

    SteamLobby_Pump();

    char mismatch[256];
    if (SteamLobby_TakeVersionMismatch(mismatch, sizeof(mismatch))) {
        SetError(mismatch);
        joining_ = false;
    }
    // A Steam invite accepted while we were in another room. Never under a
    // match: the room would take the match connection down with it, so the
    // match is left first (lobby_flow.cpp) and the move made after.
    uint64_t replacement = 0;
    if (SteamLobby_TakeReplacementJoin(&replacement)) invitedRoom_ = replacement;
    if (invitedRoom_ && !holdRoomChange_) {
        HookLog("LobbyRoom: moving to invited room %llu\n",
                static_cast<unsigned long long>(invitedRoom_));
        Leave();
        SteamLobby_Join(invitedRoom_);
        // Steam was busy with another request: joined once it is free.
        if (SteamLobby_GetPendingKind() != 3) SteamLobby_QueueJoin(invitedRoom_);
        invitedRoom_ = 0;
    }
    // A join Steam started for us (an invite) reports its outcome like one
    // picked from the list.
    if (SteamLobby_GetPendingKind() == 3) joining_ = true;

    const bool inRoom = SteamRoom_IsActive();
    const bool pending = SteamLobby_GetPendingKind() != 0;
    if ((joining_ || creating_) && !pending) {
        if (!inRoom && !error_[0])
            SetError(creating_ ? "Failed to create game session."
                     : SteamLobby_GetLastJoinRefusal() == 4 ? "LOBBY IS FULL"
                     : "Could not enter the lobby.");
        joining_ = false;
        creating_ = false;
    }
    if (inRoom && !room_.inRoom) {
        // Fresh room: start unseated with a clean record and publish at once.
        local_.slot = -1;
        local_.side = 0;
        ClearHandshake();
        handshakeOpponent_ = 0;
        local_.queueSlot = -1;
        local_.watchSlot = -1;
        local_.upstream = 0;
        watchIntent_ = -1;
        resumeWatch_ = -1;
        local_.status = MemberStatus::Idle;
        local_.wins = 0;
        local_.losses = 0;
        local_.setWins = 0;
        knownMemberCount_ = 0;
        chatCount_ = 0;
        chatHead_ = 0;
        liveAdvertised_ = false;
        PublishLocal();
        RefreshRoom();
        knownMemberCount_ = room_.memberCount; // do not announce existing members
        // A room we just created starts with the music chosen on the create
        // screen (SetMusic ignores it if we are not the owner).
        if (pendingMusic_ >= 0) SetMusic(pendingMusic_);
        pendingMusic_ = -1;
        if (pendingFirstTo_ >= 0) {
            char value[8];
            snprintf(value, sizeof(value), "%d", pendingFirstTo_);
            SteamRoom_SetData(kFirstToKey, value); // owner only, as for the music
            room_.settings.firstTo = pendingFirstTo_;
        }
        pendingFirstTo_ = -1;
        // So the lobby list can mark it; Steam does not say what type a
        // lobby is to those who find it.
        if (pendingVisibility_ >= 0)
            SteamRoom_SetData(kVisibilityKey,
                              pendingVisibility_ == static_cast<int>(Visibility::FriendsOnly)
                                  ? "1" : "0");
        pendingVisibility_ = -1;
        if (pendingRotation_ >= 0) {
            char value[8];
            snprintf(value, sizeof(value), "%d", pendingRotation_);
            SteamRoom_SetData(kRotationKey, value);
            room_.settings.rotation = static_cast<Rotation>(pendingRotation_);
        }
        pendingRotation_ = -1;
        HookLog("LobbyRoom: entered room %llu (%d members)\n",
                static_cast<unsigned long long>(room_.id), room_.memberCount);
    }

    if (inRoom || room_.inRoom) {
        DrainChat();
        if (frame_ % kRefreshInterval == 0) {
            RefreshRoom();
            RunQueue();
            PublishHostIcon();
        }
    }
    if (SteamLobby_TakeResultsChanged() || (!inRoom && frame_ % 45 == 0)) RefreshResults();
}

} // namespace lobby
