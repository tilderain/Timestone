#include "discord_presence_format.h"

#include <cstdio>
#include <cstring>

namespace {

// Discord rejects `details` and `state` over 128 characters. Two long names
// can get there ("Watching <63 bytes> vs <63 bytes>"), so text is cut at 128
// bytes without splitting a UTF-8 sequence.
constexpr std::size_t kTextLimit = 128;

std::string Quoted(const std::string& raw, std::size_t limit = kTextLimit)
{
    const std::size_t full = raw.size();
    std::size_t n = full < limit ? full : limit;
    if (n < full)
        while (n > 0 && (static_cast<unsigned char>(raw[n]) & 0xC0) == 0x80) --n;

    std::string out = "\"";
    out.reserve(n + 2);
    for (std::size_t i = 0; i < n; ++i) {
        const unsigned char c = static_cast<unsigned char>(raw[i]);
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (c < 0x20) {
                char buf[8] = {};
                std::snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned>(c));
                out += buf;
            } else {
                out += static_cast<char>(c);
            }
        }
    }
    out += '"';
    return out;
}

std::string Decimal(long long value)
{
    char buf[24] = {};
    std::snprintf(buf, sizeof(buf), "%lld", value);
    return buf;
}

// Discord rejects an empty or one-character details/state; both are omitted.
void AppendField(std::string& out, const char* key, const std::string& value)
{
    if (value.size() < 2) return;
    if (!out.empty()) out += ',';
    out += '"';
    out += key;
    out += "\":";
    out += Quoted(value);
}

std::string JoinNames(const char names[3][kPresenceCharBytes])
{
    std::string out;
    for (int i = 0; i < 3; ++i) {
        if (!names[i][0]) continue;
        if (!out.empty()) out += '/';
        out += names[i];
    }
    return out;
}

uint64_t Fnv1a64(const char* text)
{
    uint64_t hash = 1469598103934665603ull;
    for (const unsigned char* p = reinterpret_cast<const unsigned char*>(text); *p; ++p) {
        hash ^= *p;
        hash *= 1099511628211ull;
    }
    return hash;
}

bool IsRoomish(PresenceActivity activity)
{
    return activity == PresenceActivity::InRoom || activity == PresenceActivity::InMatch ||
           activity == PresenceActivity::Spectating;
}

} // namespace

bool DiscordPresence_IsJoinable(const PresenceSnapshot& s)
{
    return IsRoomish(s.activity) && s.lobbyId != 0 && s.roomPublic && s.memberLimit > 0 &&
           s.memberCount < s.memberLimit;
}

std::string DiscordPresence_ActivityJson(const PresenceSnapshot& s)
{
    if (s.activity == PresenceActivity::None) return {};

    std::string details;
    std::string state;
    bool timer = false;

    switch (s.activity) {
    case PresenceActivity::LobbyMenu:
        details = "In the online lobby";
        state = "Browsing rooms";
        break;
    case PresenceActivity::InRoom:
        details = s.roomName[0] ? std::string("Room: ") + s.roomName : std::string("In a room");
        if (s.startingMatch)
            state = "Starting a match";
        else if (s.seat >= 0)
            state = "Seated at setup " + Decimal(s.seat + 1);
        else if (s.queuePos >= 1)
            state = "Queued (#" + Decimal(s.queuePos) + ")";
        else
            state = "Hanging out";
        break;
    case PresenceActivity::InMatch: {
        details = s.opponent[0] ? std::string("vs ") + s.opponent : std::string("In a match");
        if (s.firstTo > 0) {
            details += " - " + Decimal(s.winsLocal) + "-" + Decimal(s.winsOpponent) + " (FT" +
                       Decimal(s.firstTo) + ")";
        } else if (s.winsLocal + s.winsOpponent > 0) {
            details += " - " + Decimal(s.winsLocal) + "-" + Decimal(s.winsOpponent);
        }
        const std::string local = JoinNames(s.localTeam);
        const std::string opponent = JoinNames(s.opponentTeam);
        if (local.empty() && opponent.empty())
            state = "Fighting";
        else if (local.empty())
            state = opponent;
        else if (opponent.empty())
            state = local;
        else
            state = local + " vs " + opponent;
        timer = s.startUnix > 0;
        break;
    }
    case PresenceActivity::Spectating:
        if (s.watchP1[0] && s.watchP2[0])
            details = std::string("Watching ") + s.watchP1 + " vs " + s.watchP2;
        else
            details = "Watching a match";
        state = s.roomName[0] ? std::string("Room: ") + s.roomName : std::string("In a room");
        break;
    case PresenceActivity::OfflineMenu:
        details = "Playing offline";
        break;
    case PresenceActivity::Training:
        details = "Training mode";
        break;
    case PresenceActivity::DualTraining:
        details = "Training mode";
        state = "Two-window training";
        break;
    case PresenceActivity::DirectMatch:
        details = "Direct online match";
        timer = s.startUnix > 0;
        break;
    case PresenceActivity::None:
        return {};
    }

    std::string out = "{";
    AppendField(out, "details", details);
    AppendField(out, "state", state);

    if (timer && s.startUnix > 0) {
        if (out.size() > 1) out += ',';
        out += "\"timestamps\":{\"start\":" + Decimal(s.startUnix) + "}";
    }

    if (out.size() > 1) out += ',';
    out += "\"assets\":{\"large_image\":\"timestone\",\"large_text\":";
    out += Quoted(s.version[0] ? std::string("Timestone ") + s.version : std::string("Timestone"));
    out += "}";

    if (s.lobbyId != 0 && IsRoomish(s.activity)) {
        const std::string id = Decimal(static_cast<long long>(s.lobbyId));
        char hex[17] = {};
        std::snprintf(hex, sizeof(hex), "%016llx", static_cast<unsigned long long>(Fnv1a64(id.c_str())));
        if (out.size() > 1) out += ',';
        out += "\"party\":{\"id\":\"timestone-";
        out += hex;
        out += '"';
        if (s.memberCount >= 1 && s.memberLimit >= s.memberCount) {
            out += ",\"size\":[" + Decimal(s.memberCount) + "," + Decimal(s.memberLimit) + "]";
        }
        out += '}';

        if (DiscordPresence_IsJoinable(s)) {
            out += ",\"secrets\":{\"join\":";
            out += Quoted("+connect_lobby " + id);
            out += '}';
        }
    }

    if (out.size() > 1) out += ',';
    out += "\"instance\":false}";
    return out;
}

std::string DiscordPresence_HandshakeJson(const char* clientId)
{
    return std::string("{\"v\":1,\"client_id\":") + Quoted(clientId ? clientId : "") + "}";
}

std::string DiscordPresence_SetActivityJson(const std::string& activityJson, uint32_t pid,
                                            uint32_t nonce)
{
    std::string out = "{\"cmd\":\"SET_ACTIVITY\",\"args\":{\"pid\":";
    out += Decimal(pid);
    if (!activityJson.empty()) {
        out += ",\"activity\":";
        out += activityJson;
    }
    out += "},\"nonce\":" + Quoted(Decimal(nonce)) + "}";
    return out;
}

std::string DiscordPresence_SubscribeJson(const char* evt, uint32_t nonce)
{
    return std::string("{\"cmd\":\"SUBSCRIBE\",\"evt\":") + Quoted(evt ? evt : "") +
           ",\"nonce\":" + Quoted(Decimal(nonce)) + "}";
}

std::string DiscordPresence_JoinReplyJson(const std::string& userId, bool accept, uint32_t nonce)
{
    return std::string("{\"cmd\":\"") +
           (accept ? "SEND_ACTIVITY_JOIN_INVITE" : "CLOSE_ACTIVITY_JOIN_REQUEST") +
           "\",\"args\":{\"user_id\":" + Quoted(userId) + "},\"nonce\":" + Quoted(Decimal(nonce)) +
           "}";
}

std::string DiscordPresence_Frame(uint32_t opcode, const std::string& body)
{
    std::string out(8, '\0');
    const uint32_t length = static_cast<uint32_t>(body.size());
    for (int i = 0; i < 4; ++i) {
        out[i] = static_cast<char>((opcode >> (8 * i)) & 0xFF);
        out[4 + i] = static_cast<char>((length >> (8 * i)) & 0xFF);
    }
    out += body;
    return out;
}

namespace {

// Index of the first non-space byte after "<key">:, or npos.
std::size_t FindValue(const std::string& json, const char* key, std::size_t from)
{
    const std::string token = std::string("\"") + key + "\"";
    const std::size_t at = json.find(token, from);
    if (at == std::string::npos) return std::string::npos;
    std::size_t i = at + token.size();
    while (i < json.size() && (json[i] == ' ' || json[i] == '\t' || json[i] == '\n' || json[i] == '\r'))
        ++i;
    if (i >= json.size() || json[i] != ':') return std::string::npos;
    ++i;
    while (i < json.size() && (json[i] == ' ' || json[i] == '\t' || json[i] == '\n' || json[i] == '\r'))
        ++i;
    return i;
}

int HexDigit(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

} // namespace

bool DiscordPresence_JsonString(const std::string& json, const char* key, std::string& out,
                               std::size_t from)
{
    if (!key) return false;
    std::size_t i = FindValue(json, key, from);
    if (i == std::string::npos || json[i] != '"') return false;
    ++i;
    std::string value;
    while (i < json.size()) {
        const char c = json[i++];
        if (c == '"') {
            out = value;
            return true;
        }
        if (c != '\\') {
            value += c;
            continue;
        }
        if (i >= json.size()) return false;
        const char esc = json[i++];
        switch (esc) {
        case '"': value += '"'; break;
        case '\\': value += '\\'; break;
        case '/': value += '/'; break;
        case 'n': value += '\n'; break;
        case 't': value += '\t'; break;
        case 'r': value += '\r'; break;
        case 'b': value += '\b'; break;
        case 'f': value += '\f'; break;
        case 'u': {
            if (i + 4 > json.size()) return false;
            int code = 0;
            for (int d = 0; d < 4; ++d) {
                const int digit = HexDigit(json[i + d]);
                if (digit < 0) return false;
                code = code * 16 + digit;
            }
            i += 4;
            value += code < 0x80 ? static_cast<char>(code) : '?';
            break;
        }
        default:
            value += esc;
            break;
        }
    }
    return false;
}

bool DiscordPresence_JsonInt(const std::string& json, const char* key, long long& out)
{
    if (!key) return false;
    std::size_t i = FindValue(json, key, 0);
    if (i == std::string::npos) return false;

    bool negative = false;
    if (i < json.size() && json[i] == '-') {
        negative = true;
        ++i;
    }
    long long value = 0;
    bool any = false;
    int digits = 0;
    while (i < json.size() && json[i] >= '0' && json[i] <= '9') {
        if (++digits > 18) return false; // beyond any code Discord sends; no overflow
        value = value * 10 + (json[i] - '0');
        ++i;
        any = true;
    }
    if (!any) return false;
    out = negative ? -value : value;
    return true;
}

bool DiscordPresence_ShouldAsk(int richPresenceSetup, int showDiscordStatus)
{
    return richPresenceSetup != 1 && showDiscordStatus != 1;
}
