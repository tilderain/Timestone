// Host test for the Discord presence payloads. Builds and runs on Linux:
//   make DISCORD_PRESENCE_TEST=1 discord-presence-test
#include "discord_presence_format.h"

#include <cctype>
#include <cstdio>
#include <cstring>
#include <string>

namespace {

int g_failures = 0;
int g_checks = 0;

void Check(bool ok, const char* file, int line, const char* what)
{
    ++g_checks;
    if (ok) return;
    ++g_failures;
    std::printf("%s:%d: FAILED: %s\n", file, line, what);
}

bool Contains(const std::string& haystack, const std::string& needle)
{
    return haystack.find(needle) != std::string::npos;
}

void Copy(char* dst, int size, const char* src)
{
    std::snprintf(dst, static_cast<size_t>(size), "%s", src);
}

PresenceSnapshot Room(bool pub, int members, int limit)
{
    PresenceSnapshot s;
    s.activity = PresenceActivity::InRoom;
    s.lobbyId = 109775244080946091ull;
    Copy(s.roomName, kPresenceNameBytes, "Probe");
    s.memberCount = members;
    s.memberLimit = limit;
    s.roomPublic = pub;
    Copy(s.version, 16, "0.6.5");
    return s;
}

std::string Value(const std::string& json, const char* key)
{
    std::string out;
    Check(DiscordPresence_JsonString(json, key, out), __FILE__, __LINE__, "json key present");
    return out;
}

// A strict JSON reader. Discord's client parses every payload and closes the
// pipe on the first syntax error, so finding a key in the text is not enough.
struct JsonReader {
    const std::string& t;
    std::size_t i = 0;

    void Space()
    {
        while (i < t.size() && (t[i] == ' ' || t[i] == '\n' || t[i] == '\r' || t[i] == '\t')) ++i;
    }
    bool Eat(char c)
    {
        Space();
        if (i >= t.size() || t[i] != c) return false;
        ++i;
        return true;
    }
    bool Digits()
    {
        const std::size_t start = i;
        while (i < t.size() && std::isdigit(static_cast<unsigned char>(t[i]))) ++i;
        return i > start;
    }
    bool String()
    {
        if (!Eat('"')) return false;
        while (i < t.size()) {
            const unsigned char c = static_cast<unsigned char>(t[i++]);
            if (c == '"') return true;
            if (c < 0x20) return false;
            if (c != '\\') continue;
            if (i >= t.size()) return false;
            const char e = t[i++];
            if (e == 'u') {
                for (int k = 0; k < 4; ++k)
                    if (i >= t.size() || !std::isxdigit(static_cast<unsigned char>(t[i++]))) return false;
            } else if (e == '\0' || !std::strchr("\"\\/bfnrt", e)) {
                return false;
            }
        }
        return false;
    }
    bool Number()
    {
        if (i < t.size() && t[i] == '-') ++i;
        if (i < t.size() && t[i] == '0')
            ++i;
        else if (!Digits())
            return false;
        if (i < t.size() && t[i] == '.' && (++i, !Digits())) return false;
        if (i < t.size() && (t[i] == 'e' || t[i] == 'E')) {
            ++i;
            if (i < t.size() && (t[i] == '+' || t[i] == '-')) ++i;
            if (!Digits()) return false;
        }
        return true;
    }
    bool Word(const char* word)
    {
        const std::size_t n = std::strlen(word);
        if (t.compare(i, n, word) != 0) return false;
        i += n;
        return true;
    }
    bool Value(int depth)
    {
        Space();
        if (depth > 32 || i >= t.size()) return false;
        switch (t[i]) {
        case '{':
            ++i;
            if (Eat('}')) return true;
            do {
                if (!String() || !Eat(':') || !Value(depth + 1)) return false;
            } while (Eat(','));
            return Eat('}');
        case '[':
            ++i;
            if (Eat(']')) return true;
            do {
                if (!Value(depth + 1)) return false;
            } while (Eat(','));
            return Eat(']');
        case '"': return String();
        case 't': return Word("true");
        case 'f': return Word("false");
        case 'n': return Word("null");
        default: return Number();
        }
    }
};

bool IsJson(const std::string& text)
{
    JsonReader reader{text};
    if (!reader.Value(0)) return false;
    reader.Space();
    return reader.i == text.size();
}

void CheckJson(const std::string& json, const char* file, int line)
{
    Check(IsJson(json), file, line, "valid JSON");
    if (!IsJson(json)) std::printf("    %s\n", json.c_str());
}

} // namespace

#define CHECK(cond) Check((cond), __FILE__, __LINE__, #cond)
#define CHECK_JSON(json) CheckJson((json), __FILE__, __LINE__)

int main()
{
    // 1. A public room with three of eight seats: joinable, id never published.
    {
        const std::string json = DiscordPresence_ActivityJson(Room(true, 3, 8));
        CHECK(Contains(json, "\"secrets\":{\"join\":\"+connect_lobby 109775244080946091\"}"));
        CHECK(Contains(json, "\"party\":{\"id\":\"timestone-"));
        CHECK(Contains(json, "\"size\":[3,8]"));
        CHECK(Value(json, "details") == "Lobby: Probe");
        CHECK(Value(json, "state") == "In Lobby");

        const std::size_t idAt = json.find("109775244080946091");
        const std::size_t partyAt = json.find("\"party\":{\"id\":\"timestone-");
        CHECK(idAt != std::string::npos);
        CHECK(partyAt != std::string::npos);
        CHECK(idAt > partyAt); // only inside the join secret
        CHECK(partyAt < idAt);

        const std::string partyId = Value(json, "id");
        CHECK(partyId.size() == 26);
        CHECK(partyId.compare(0, 10, "timestone-") == 0);
    }

    // 2. A private room: party but no join secret, and no privacy field.
    {
        const std::string json = DiscordPresence_ActivityJson(Room(false, 3, 8));
        CHECK(!Contains(json, "\"secrets\""));
        CHECK(!Contains(json, "\"privacy\""));
        CHECK(Contains(json, "\"size\":[3,8]"));
        CHECK(DiscordPresence_IsJoinable(Room(true, 3, 8)));
        CHECK(!DiscordPresence_IsJoinable(Room(false, 3, 8)));
    }

    // 3. A full room is not joinable.
    {
        const std::string json = DiscordPresence_ActivityJson(Room(true, 8, 8));
        CHECK(!Contains(json, "\"secrets\""));
        CHECK(!DiscordPresence_IsJoinable(Room(true, 8, 8)));
    }

    // 4. A match with teams, score and timer.
    {
        PresenceSnapshot s;
        s.activity = PresenceActivity::InMatch;
        s.lobbyId = 109775244080946091ull;
        s.roomPublic = true;
        s.memberCount = 2;
        s.memberLimit = 8;
        Copy(s.roomName, kPresenceNameBytes, "Probe");
        Copy(s.opponent, kPresenceNameBytes, "Bob");
        Copy(s.localTeam[0], kPresenceCharBytes, "Ryu");
        Copy(s.localTeam[1], kPresenceCharBytes, "Dante");
        Copy(s.localTeam[2], kPresenceCharBytes, "Zero");
        Copy(s.opponentTeam[0], kPresenceCharBytes, "Hulk");
        Copy(s.opponentTeam[1], kPresenceCharBytes, "Thor");
        Copy(s.opponentTeam[2], kPresenceCharBytes, "Storm");
        s.firstTo = 2;
        s.winsLocal = 1;
        s.winsOpponent = 0;
        s.startUnix = 1700000000;
        Copy(s.version, 16, "0.6.5");

        const std::string json = DiscordPresence_ActivityJson(s);
        CHECK(Value(json, "details") == "Playing: versus - 1-0 (FT2)");
        CHECK(Value(json, "state") == "vs Bob - Ryu/Dante/Zero vs Hulk/Thor/Storm");
        CHECK(Contains(json, "\"timestamps\":{\"start\":1700000000}"));
        CHECK(Contains(json, "\"assets\":{\"large_image\":\"timestone\",\"large_text\":\"Timestone 0.6.5\"}"));
        CHECK(Contains(json, "\"instance\":false"));
    }

    // 5. Escaping of quotes, backslashes and control bytes.
    {
        PresenceSnapshot s = Room(true, 1, 8);
        std::snprintf(s.roomName, kPresenceNameBytes, "a\"b\\c%c", '\x01');
        const std::string json = DiscordPresence_ActivityJson(s);
        CHECK(Contains(json, "\"details\":\"Lobby: a\\\"b\\\\c\\u0001\""));
    }

    // 6. Multi-byte names survive whole, and text over 128 bytes is cut
    //    without splitting a sequence.
    {
        PresenceSnapshot s = Room(true, 1, 8);
        std::string name;
        for (int i = 0; i < 31; ++i) name += "\xC3\xA9";
        Copy(s.roomName, kPresenceNameBytes, name.c_str());
        const std::string json = DiscordPresence_ActivityJson(s);
        const std::string details = Value(json, "details");
        CHECK(details == "Lobby: " + name);
        CHECK(details.size() <= 128);

        // A match line is the one long enough to be cut: opponent plus both
        // teams can pass 128 bytes.
        PresenceSnapshot match = Room(true, 2, 8);
        match.activity = PresenceActivity::InMatch;
        Copy(match.opponent, kPresenceNameBytes, name.c_str()); // 62 bytes
        std::string slot;
        for (int i = 0; i < 11; ++i) slot += "\xC3\xA9";
        for (int i = 0; i < 3; ++i) {
            Copy(match.localTeam[i], kPresenceCharBytes, slot.c_str());
            Copy(match.opponentTeam[i], kPresenceCharBytes, slot.c_str());
        }
        const std::string cut = Value(DiscordPresence_ActivityJson(match), "state");
        CHECK(cut.size() <= 128);
        CHECK(cut.size() > 100);
        CHECK(cut.compare(0, 3, "vs ") == 0);
        for (std::size_t i = 0; i < cut.size(); ++i) {
            const unsigned char c = static_cast<unsigned char>(cut[i]);
            if (c == 0xC3) CHECK(i + 1 < cut.size() && static_cast<unsigned char>(cut[i + 1]) == 0xA9);
            if (c == 0xA9) CHECK(i > 0 && static_cast<unsigned char>(cut[i - 1]) == 0xC3);
        }
    }

    // 7. Off publishes nothing and SET_ACTIVITY without an activity clears it.
    {
        PresenceSnapshot s;
        CHECK(DiscordPresence_ActivityJson(s).empty());
        CHECK(DiscordPresence_SetActivityJson("", 42, 7) ==
              "{\"cmd\":\"SET_ACTIVITY\",\"args\":{\"pid\":42},\"nonce\":\"7\"}");
        CHECK(DiscordPresence_HandshakeJson("1544202390536982610") ==
              "{\"v\":1,\"client_id\":\"1544202390536982610\"}");
        CHECK(DiscordPresence_SubscribeJson("ACTIVITY_JOIN", 3) ==
              "{\"cmd\":\"SUBSCRIBE\",\"evt\":\"ACTIVITY_JOIN\",\"nonce\":\"3\"}");
        CHECK(DiscordPresence_JoinReplyJson("123", true, 4) ==
              "{\"cmd\":\"SEND_ACTIVITY_JOIN_INVITE\",\"args\":{\"user_id\":\"123\"},\"nonce\":\"4\"}");
        CHECK(DiscordPresence_JoinReplyJson("123", false, 5) ==
              "{\"cmd\":\"CLOSE_ACTIVITY_JOIN_REQUEST\",\"args\":{\"user_id\":\"123\"},\"nonce\":\"5\"}");
    }

    // 8. Reading the client's replies.
    {
        const std::string dispatch =
            "{\"cmd\":\"DISPATCH\",\"data\":{\"secret\":\"+connect_lobby 5\"},"
            "\"evt\":\"ACTIVITY_JOIN\",\"nonce\":null}";
        CHECK(Value(dispatch, "evt") == "ACTIVITY_JOIN");
        CHECK(Value(dispatch, "secret") == "+connect_lobby 5");

        const std::string quoted = "{\"data\":{\"message\":\"a \\\"b\\\" c\"}}";
        CHECK(Value(quoted, "message") == "a \"b\" c");

        const std::string request =
            "{\"cmd\":\"DISPATCH\",\"evt\":\"ACTIVITY_JOIN_REQUEST\","
            "\"data\":{\"user\":{\"avatar\":null,\"id\":\"123\",\"username\":\"x\"}}}";
        const std::size_t userAt = request.find("\"user\"");
        CHECK(userAt != std::string::npos);
        std::string id;
        CHECK(DiscordPresence_JsonString(request, "id", id, userAt));
        CHECK(id == "123");

        long long code = 0;
        CHECK(DiscordPresence_JsonInt("{\"evt\":\"ERROR\",\"data\":{\"code\":4006,\"message\":\"no\"}}",
                                      "code", code));
        CHECK(code == 4006);
        CHECK(!DiscordPresence_JsonInt("{\"code\":\"4006\"}", "code", code));
        CHECK(!DiscordPresence_JsonInt("{\"code\":99999999999999999999999}", "code", code));

        std::string missing;
        CHECK(!DiscordPresence_JsonString("{\"a\":1}", "evt", missing));
    }

    // 9. Framing.
    {
        const std::string frame = DiscordPresence_Frame(1, "ab");
        CHECK(frame.size() == 10);
        CHECK(static_cast<unsigned char>(frame[0]) == 1);
        CHECK(static_cast<unsigned char>(frame[4]) == 2);
        CHECK(frame.substr(8) == "ab");
    }

    // 10. First-run question: due only until answered or turned on by hand.
    CHECK(DiscordPresence_ShouldAsk(0, 0));
    CHECK(DiscordPresence_ShouldAsk(2, 0));
    CHECK(!DiscordPresence_ShouldAsk(1, 0));
    CHECK(!DiscordPresence_ShouldAsk(0, 1));
    CHECK(!DiscordPresence_ShouldAsk(1, 1));

    // 11. Every payload is whole, valid JSON: the client closes the pipe on
    //     the first one that is not (seen 2026-11-22: "{," at position 53).
    {
        CHECK(IsJson("{\"a\":[1,-2.5e3,true,null],\"b\":{\"c\":\"\\u0001\"}}"));
        CHECK(!IsJson("{\"cmd\":\"SET_ACTIVITY\",\"args\":{\"pid\":6988,\"activity\":{,\"details\":\"x\"}}}"));
        CHECK(!IsJson("{\"a\":1,}"));
        CHECK(!IsJson("{\"a\":1}x"));

        PresenceSnapshot all[16];
        int count = 0;
        all[count++] = Room(true, 3, 8);
        all[count++] = Room(false, 3, 8);
        all[count++] = Room(true, 8, 8);
        {
            PresenceSnapshot s = Room(true, 1, 8);
            std::snprintf(s.roomName, kPresenceNameBytes, "a\"b\\c%c", '\x01');
            all[count++] = s;
        }
        {
            PresenceSnapshot s = Room(true, 2, 8);
            s.roomName[0] = '\0';
            s.seat = 0;
            all[count++] = s;
        }
        {
            PresenceSnapshot s = Room(true, 2, 8);
            s.queuePos = 2;
            all[count++] = s;
        }
        {
            PresenceSnapshot s = Room(true, 2, 8);
            s.activity = PresenceActivity::InMatch;
            Copy(s.opponent, kPresenceNameBytes, "Bob");
            Copy(s.localTeam[0], kPresenceCharBytes, "Ryu");
            s.firstTo = 2;
            s.winsLocal = 1;
            s.startUnix = 1700000000;
            all[count++] = s;
        }
        {
            PresenceSnapshot s = Room(true, 2, 8);
            s.activity = PresenceActivity::InMatch; // no names, no timer yet
            all[count++] = s;
        }
        {
            PresenceSnapshot s = Room(false, 2, 8);
            s.activity = PresenceActivity::Spectating;
            Copy(s.watchP1, kPresenceNameBytes, "A");
            Copy(s.watchP2, kPresenceNameBytes, "B");
            all[count++] = s;
        }
        const PresenceActivity plain[] = {PresenceActivity::LobbyMenu, PresenceActivity::OfflineMenu,
                                          PresenceActivity::Training, PresenceActivity::DualTraining,
                                          PresenceActivity::DirectMatch};
        for (PresenceActivity activity : plain) {
            PresenceSnapshot s;
            s.activity = activity;
            Copy(s.version, 16, "0.6.5");
            all[count++] = s;
        }
        all[count - 1].startUnix = 1700000000; // the direct match's timer
        {
            PresenceSnapshot s;
            s.activity = PresenceActivity::Training; // no version
            all[count++] = s;
        }

        for (int n = 0; n < count; ++n) {
            const std::string activity = DiscordPresence_ActivityJson(all[n]);
            CHECK(!activity.empty());
            CHECK_JSON(activity);
            CHECK_JSON(DiscordPresence_SetActivityJson(activity, 6988, static_cast<uint32_t>(n)));
        }
        PresenceSnapshot lobby;
        lobby.activity = PresenceActivity::LobbyMenu;
        CHECK(DiscordPresence_ActivityJson(lobby).compare(0, 11, "{\"details\":") == 0);

        CHECK_JSON(DiscordPresence_SetActivityJson("", 6988, 1));
        CHECK_JSON(DiscordPresence_HandshakeJson("1544202390536982610"));
        CHECK_JSON(DiscordPresence_SubscribeJson("ACTIVITY_JOIN", 2));
        CHECK_JSON(DiscordPresence_JoinReplyJson("123", true, 3));
        CHECK_JSON(DiscordPresence_JoinReplyJson("123", false, 4));
    }

    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
