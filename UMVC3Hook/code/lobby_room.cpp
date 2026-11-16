#include "../pch.h"
#include "lobby_room.h"
#include "lobby_music.h"
#include "steam_lobby.h"

#include <Windows.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>

extern void HookLog(const char* fmt, ...);

namespace lobby {

namespace {

constexpr char kMemberKey[] = "ts";      // per-member room state
constexpr char kMusicKey[] = "bgm";      // lobby data: the host's music choice
// Lobby data: SteamIDs the host removed, as comma-separated hex. Steam has no
// kick, so the list is the kick: a listed client leaves by itself, every
// other client drops listed members from its room, and Join refuses a lobby
// that lists the local player. It lives and dies with the lobby.
constexpr char kBanKey[] = "ban";
constexpr char kFirstToKey[] = "ft";     // lobby data: games needed to win a set
constexpr int kMaxBans = 64;
constexpr int kMemberVersion = 2;
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

bool IsPlausibleTeam(const Team& team)
{
    for (int i = 0; i < 3; ++i) {
        if (team.chars[i] < 1 || team.chars[i] > 50 || team.assists[i] < 0 ||
            team.assists[i] > 2 || team.colors[i] < 0 || team.colors[i] > 8)
            return false;
    }
    return true;
}

// Member state travels as one short string so a member's seat, readiness and
// team always change atomically for observers.
void EncodeMember(char* out, size_t size, int slot, int side, bool ready, MemberStatus status,
                  EntryMode mode, const Team& team, int wins, int losses, int setWins)
{
    snprintf(out, size, "%d|%d|%d|%d|%d|%d|%d,%d,%d|%d,%d,%d|%d,%d,%d|%d|%d|%d", kMemberVersion,
             slot, side, ready ? 1 : 0, static_cast<int>(status), static_cast<int>(mode),
             team.chars[0], team.chars[1], team.chars[2], team.assists[0], team.assists[1],
             team.assists[2], team.colors[0], team.colors[1], team.colors[2], wins, losses,
             setWins);
}

bool DecodeMember(const char* text, Member& member)
{
    int version = 0, slot = -1, side = 0, ready = 0, status = 0, mode = 0, wins = 0, losses = 0;
    int setWins = 0;
    Team team = {};
    if (!text || sscanf(text, "%d|%d|%d|%d|%d|%d|%d,%d,%d|%d,%d,%d|%d,%d,%d|%d|%d|%d", &version,
                        &slot, &side, &ready, &status, &mode, &team.chars[0], &team.chars[1],
                        &team.chars[2], &team.assists[0], &team.assists[1], &team.assists[2],
                        &team.colors[0], &team.colors[1], &team.colors[2], &wins, &losses,
                        &setWins) != 18)
        return false;
    if (version != kMemberVersion) return false;
    if (slot < -1 || slot >= kMaxSlots || side < 0 || side > 2 || status < 0 ||
        status > static_cast<int>(MemberStatus::Watching) || mode < 0 || mode > 1)
        return false;
    member.slot = static_cast<int8_t>(slot);
    member.side = static_cast<int8_t>(side);
    member.ready = ready != 0;
    member.status = static_cast<MemberStatus>(status);
    member.entryMode = static_cast<EntryMode>(mode);
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
    return index >= 0 && index < chatCount_ ? chat_[index] : empty;
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
    if (chatCount_ == kMaxChatLines) {
        memmove(&chat_[0], &chat_[1], sizeof(ChatLine) * (kMaxChatLines - 1));
        --chatCount_;
    }
    ChatLine& line = chat_[chatCount_++];
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
    pendingFirstTo_ = 0;
    SteamLobby_Join(roomId);
}

void SteamRoomService::Leave()
{
    SteamRoom_Leave();
    local_.slot = -1;
    local_.side = 0;
    local_.ready = false;
    local_.status = MemberStatus::Idle;
    local_.wins = 0;
    local_.losses = 0;
    local_.setWins = 0;
    room_ = {};
    publishedOnce_ = false;
    liveAdvertised_ = false;
    knownMemberCount_ = 0;
    chatCount_ = 0;
}

void SteamRoomService::InviteFriends()
{
    SteamRoom_InviteFriends();
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
    char encoded[160];
    EncodeMember(encoded, sizeof(encoded), local_.slot, local_.side, local_.ready, local_.status,
                 local_.mode, local_.team, local_.wins, local_.losses, local_.setWins);
    SteamRoom_SetLocalData(kMemberKey, encoded);
    publishedOnce_ = true;
}

void SteamRoomService::Sit(int slot, int side)
{
    if (!room_.inRoom || !IsValidSeat(room_.settings, slot, side)) return;
    if (local_.status == MemberStatus::Fighting) return;
    local_.slot = slot;
    local_.side = side;
    local_.ready = false;
    local_.status = MemberStatus::Seated;
    PublishLocal();
    RefreshRoom();
}

void SteamRoomService::Stand()
{
    if (local_.status == MemberStatus::Fighting) return;
    local_.slot = -1;
    local_.side = 0;
    local_.ready = false;
    local_.status = MemberStatus::Idle;
    PublishLocal();
    RefreshRoom();
}

void SteamRoomService::SetReady(bool ready)
{
    if (local_.slot < 0 || local_.status == MemberStatus::Fighting) return;
    local_.ready = ready;
    local_.status = ready ? MemberStatus::Ready : MemberStatus::Seated;
    PublishLocal();
    RefreshRoom();
}

void SteamRoomService::SetTeam(const Team& team, EntryMode mode)
{
    local_.team = team;
    local_.mode = mode;
    PublishLocal();
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
    local_.ready = false;
    local_.status = local_.slot >= 0 ? MemberStatus::Seated : MemberStatus::Idle;
    PublishLocal();
    RefreshRoom();
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
        if (result.maxMembers < result.members) result.maxMembers = result.members;
        if (result.maxMembers <= 0) result.maxMembers = kMaxMembers;
        result.slotCount = SteamLobby_GetResultSlotCount(i);
        const int estimate = SteamLobby_GetResultEstimatedPing(i);
        result.pingMs = estimate >= 0 ? estimate / 2 : -1;
        result.compatible = SteamLobby_IsLobbyVersionCompatible(result.id);
        result.friendLobby = SteamLobby_IsFriendLobby(result.id);
        result.matchInProgress = SteamLobby_IsResultInMatch(i);
        result.firstTo = atoi(SteamRoom_GetLobbyData(result.id, kFirstToKey));
        if (result.firstTo < 1 || result.firstTo > kMaxFirstTo) result.firstTo = 1;
    }
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
    next.settings.firstTo = atoi(SteamRoom_GetData(kFirstToKey));
    if (next.settings.firstTo < 1 || next.settings.firstTo > kMaxFirstTo) next.settings.firstTo = 1;
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
            member.entryMode = local_.mode;
            member.team = local_.team;
            member.wins = static_cast<int16_t>(local_.wins);
            member.losses = static_cast<int16_t>(local_.losses);
            member.setWins = static_cast<int8_t>(local_.setWins);
            member.pingMs = 0;
        } else {
            if (!DecodeMember(SteamRoom_GetMemberData(id, kMemberKey), member) ||
                !IsPlausibleTeam(member.team)) {
                // Not published yet (or malformed): present but not seated.
                member.slot = -1;
                member.ready = false;
                member.status = MemberStatus::Idle;
            }
            if (member.slot >= next.settings.slotCount) member.slot = -1;
            member.pingMs = static_cast<int16_t>(SteamRoom_EstimateMemberPing(id));
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
                local_.ready = false;
                local_.status = MemberStatus::Idle;
                PublishLocal();
                AddChat("", "That seat was taken by another player.", true);
            }
        }
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
    if (!room_.inRoom || local_.slot < 0 || !local_.ready ||
        local_.status == MemberStatus::Fighting)
        return false;
    for (int i = 0; i < room_.memberCount; ++i) {
        const Member& member = room_.members[i];
        if (member.isLocal || member.slot != local_.slot || member.side > 1 ||
            member.side == local_.side || !member.ready ||
            member.status == MemberStatus::Fighting)
            continue;
        pairing = {};
        pairing.slot = local_.slot;
        pairing.localSide = local_.side;
        pairing.opponentId = member.id;
        snprintf(pairing.opponentName, sizeof(pairing.opponentName), "%s", member.name);
        pairing.localTeam = local_.team;
        pairing.opponentTeam = member.team;
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
    uint64_t replacement = 0;
    if (SteamLobby_TakeReplacementJoin(&replacement)) {
        // A Steam invite arrived while we were in another room.
        Leave();
        joining_ = true;
        SteamLobby_Join(replacement);
    }

    const bool inRoom = SteamRoom_IsActive();
    const bool pending = SteamLobby_GetPendingKind() != 0;
    if ((joining_ || creating_) && !pending) {
        if (!inRoom && !error_[0])
            SetError(creating_ ? "Failed to create game session." : "Could not enter the lobby.");
        joining_ = false;
        creating_ = false;
    }
    if (inRoom && !room_.inRoom) {
        // Fresh room: start unseated with a clean record and publish at once.
        local_.slot = -1;
        local_.side = 0;
        local_.ready = false;
        local_.status = MemberStatus::Idle;
        local_.wins = 0;
        local_.losses = 0;
        local_.setWins = 0;
        knownMemberCount_ = 0;
        chatCount_ = 0;
        liveAdvertised_ = false;
        PublishLocal();
        RefreshRoom();
        knownMemberCount_ = room_.memberCount; // do not announce existing members
        // A room we just created starts with the music chosen on the create
        // screen (SetMusic ignores it if we are not the owner).
        if (pendingMusic_ >= 0) SetMusic(pendingMusic_);
        pendingMusic_ = -1;
        if (pendingFirstTo_ > 0) {
            char value[8];
            snprintf(value, sizeof(value), "%d", pendingFirstTo_);
            SteamRoom_SetData(kFirstToKey, value); // owner only, as for the music
            room_.settings.firstTo = pendingFirstTo_;
        }
        pendingFirstTo_ = 0;
        HookLog("LobbyRoom: entered room %llu (%d members)\n",
                static_cast<unsigned long long>(room_.id), room_.memberCount);
    }

    if (inRoom || room_.inRoom) {
        DrainChat();
        if (frame_ % kRefreshInterval == 0) RefreshRoom();
    }
    if (SteamLobby_TakeResultsChanged() || (!inRoom && frame_ % 45 == 0)) RefreshResults();
}

} // namespace lobby
