# Online modes: Discord rich presence + ranked link (Timestone hook)

Maintenance notes for the optional online features in `Timestone.asi`.
Nothing here is player-facing documentation.

## SDK

- Social SDK **1.10.19337** (`DiscordSocialSdk-1.10.19337.zip`), downloaded from
  Discord's developer portal (Application > "Social SDK" sidebar section).
- Vendored under `UMVC3Hook/third_party/discord_social_sdk/`:
  - `include/cdiscord.h` - generated C API header (pure `extern "C"` exports).
  - `bin/release/discord_partner_sdk.dll` - x64 release DLL (9.9 MB).
  - `License-Notices.txt` - third-party license notices shipped with the game.
- The C++ wrapper `discordpp.h`, Krisp/voice DLLs, and other binaries are NOT
  vendored; the module drives the C API directly.
- The DLL is **optional at runtime**: `discord_rich_presence.cpp` does
  `LoadLibraryA("discord_partner_sdk.dll")` (found next to the game exe) and
  resolves every export via `GetProcAddress`. Missing DLL or a missing required
  export disables the presence tiers (1-2) only; the ranked link (tier 4) never
  touches the DLL.

## Dev-portal setup (application id 1544202390536982610)

- Social SDK enablement form submitted (application side bar).
- Large image asset key: `timestone` (keys are lowercased by Discord).
- Advanced mode (tier 2 OAuth) requires the application to be a **Public
  Client** with the OAuth redirect `http://127.0.0.1/callback`.

## Player modes (`[Settings] OnlineMode`, values 0-3)

The settings modal's online section ("Online / Discord") shows four radio
rows:

| stored | UI label | SDK | Link | v1 extras |
|---|---|---|---|---|
| `0` | Off (no Discord) | off | - | - |
| `1` | Basic rich presence | local client, activity | - | - |
| `2` | Advanced (link Discord account) | activity + join | loopback OAuth (`127.0.0.1/callback`) | - |
| `3` | Ranked (link for matchmaking) | **off** | hosted: login service | one POST on link complete |

- `0` Off (default). No client is started; nothing is published.
- `1` Basic - local presence through the running Discord client, no account
  link. Steam launch-to-join registration still applies. Hosting shows the
  party with **no join secret**.
- `2` Advanced - Discord OAuth (PKCE) account link over the loopback redirect;
  one-time consent prompt before the browser flow. On any OAuth error the
  module drops back to Basic. The lobby join secret is published **only** in
  this tier and only after the account link completed.
- `3` Ranked - links the Steam + Discord accounts through the hosted service.
  The Discord SDK is off under this mode (no presence published). Consent, the
  browser open, and the link poll are owned by `ranked_account.cpp`; entering
  this tier without a completed link shows a consent box and opens the
  default browser to `login.hero-rehab.xyz/link?...` (the link service).

### Keys

- `[Settings] OnlineMode` - the single persisted mode key (0-3).
- `[Settings] RankedLinked` - `1` once the hosted service accepted the link;
  suppresses the whole tier-4 flow on later sessions (silent re-entry).
- `[Settings] RankedBaseUrl` - optional override of the link-service origin
  (status poll + completion request), e.g. `http://127.0.0.1:8787` for the
  local stub; empty means the production link service.
- `[Settings] RankedLinkBaseUrl` - optional override of just the browser page
  origin. Unset means "use `RankedBaseUrl` when set, else the link service", so
  a single ini line still points the whole flow at the stub.
- Legacy `[Settings] DiscordRichPresence` (0-2) is migrated to `OnlineMode`
  once when `OnlineMode` is absent; the legacy key is left in the file and
  never read again.
- On first clean boot the hook adds user-facing `;` comment lines above each of
  these keys in the live `Timestone.ini` (via `IniSafe_EnsureLineComment`, in
  `dllmain.cpp`), so players editing the file see what every value means.

## Presence state machine

Published through the SDK as an activity named "Timestone" (default app
display name; `Discord_Activity_SetName` is never called), large image
`timestone`.

| Game state | Details | State | Party | Secret | Timestamp |
|---|---|---|---|---|---|
| Main menu / char select / pause | `Main Menu` | - | - | - | - |
| Hosting lobby, waiting (tier 2 linked) | `Hosting a Lobby` | `Waiting for an opponent` | 1/2 public | `+connect_lobby <id>` | - |
| Hosting lobby, waiting (other tiers) | `Hosting a Lobby` | `Waiting for an opponent` | 1/2 public | - | - |
| Hosting lobby, opponent present | `Hosting a Lobby` | `In the lobby` | 2/2 | - | - |
| Guest in lobby | `In a Lobby` | `Waiting for the host` | members/2 | - | - |
| In a match (any mode) | `In a Match` | - | - | - | match start |

- Push rate is capped at one update per 15 s (Discord silently drops faster
  updates); transitions are compared against a fingerprint before pushing.
- Join secrets route into the existing Steam funnel through
  `SteamLobby_QueueJoinByConnectString` (same path as a Steam invite).
- The in-match reader (`InMatch()` in `discord_rich_presence.cpp`) inspects the
  RE-verified `sArea` active chain: instance pointer `0x140e17a80`
  (double-dereferenced), chain length `sArea+0x703C`, `cArea*[8]` at `+0x7048`,
  per-area cached name at `+0x40`. Presence of `aAction`/`aStageNNN` in the
  chain means a battle is active. See `doc/area.md`.

## Tier 4 v1 contract (client side)

The server side lands later, outside this repo; the client targets the
following contract through `ranked_account.cpp`. Timestone runs several
separate services on `hero-rehab.xyz`: **login/link** (`login.hero-rehab.xyz`,
which v1 calls for the whole flow), **ranked** (`mvc3-ranked.hero-rehab.xyz`,
no v1 endpoints), **mm** (Steam lobby join pages) and **replays**
(`replays.hero-rehab.xyz`, future). If the real endpoint contract differs when
the server lands, only the constants at the top of that file and this section
need updating.

- `GET <link>/link?app=umvc3&steam_id=<steam id>&state=<16 hex>` - browser page
  the client opens through `ShellExecuteA` after the consent box. `state` is
  generated from `GetTickCount()`/PID entropy.
- The client then polls `GET <link>/link/status?state=<hex>` from a background
  thread every 2 s (never on the game thread) for up to 8 minutes (240
  attempts), with a 3 s timeout per request. Each request opens and closes its
  own WinHTTP handles. A response body containing `"status":"linked"` triggers
  the single completion request; `"pending"` or any error keeps polling.
- `POST <link>/link/complete`, Content-Type `application/json`, body:
  `{"app":"umvc3","build":"0.3.6","steam_id":1234,"state":"<hex>"}` - exactly
  ONE client request, sent only after the status poll observed `linked`. On a
  2xx reply the client writes `[Settings] RankedLinked=1` and stops; a non-2xx
  or transport error is logged and the flow stops (re-selecting tier 4
  restarts it).
- Leaving tier 4 (or a second game instance completing the same link first)
  stops the poll thread; `RankedLinked` is never cleared by mode switches.
- `SteamLobby_GetLocalSteamId()` (`steam_lobby.cpp`) supplies the Steam ID by
  probing `ISteamUser` interface versions `SteamUser021/020/019` through the
  locally running Steam client; `0` means Steam is unavailable and tier 4
  refuses with a hook log.
- WinHTTP (`winhttp.dll`) is loaded at runtime on the first tier-4 selection;
  missing exports make tier 4 inert with a one-time hook log.

## Known accepted behavior

- Dual training spawns a second game process under the same Discord account;
  both update the same presence, so the last writer wins and the activity may
  flap. Matches Steam lobby behavior; not suppressed.
- Tier 4 v1 has no gameplay-visible effect beyond the "(linked)" row label and
  hook logs - link plumbing only. Per-match telemetry, result/roster decode,
  and login tickets are deferred to the later server work.
- The link's Steam ID is trusted because it is read from the locally running,
  authenticated Steam client, not supplied by the user.
- Rollback is cheap: a player sets mode 0, deletes `discord_partner_sdk.dll`,
  or reinstalls. Code rollback is a git revert.
