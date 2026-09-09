# Discord Rich Presence (Timestone hook)

Maintenance notes for the optional Discord presence feature in `Timestone.asi`.
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
  export disables the whole feature (no crash, settings row disabled).

## Dev-portal setup (application id 1544202390536982610)

- Social SDK enablement form submitted (application side bar).
- Large image asset key: `timestone` (keys are lowercased by Discord).
- Full mode (OAuth) requires the application to be a **Public Client** with the
  OAuth redirect `http://127.0.0.1/callback`.

## Player modes (`[Settings] DiscordRichPresence`, values 0/1/2)

- `0` Off (default). No client is started; nothing is published.
- `1` Basic - local presence through the running Discord client, no OAuth
  account link. Steam launch-to-join registration still applies.
- `2` Full - Discord OAuth (PKCE) account link; one-time consent prompt before
  the browser flow. On any OAuth error the module drops back to Basic.

The launch-menu advanced settings modal ("Discord Rich Presence" section)
shows three radio checkboxes; the row is disabled with a note when the DLL is
missing.

## Presence state machine

Published through the SDK as an activity named "Timestone" (default app
display name; `Discord_Activity_SetName` is never called), large image
`timestone`.

| Game state | Details | State | Party | Secret | Timestamp |
|---|---|---|---|---|---|
| Main menu / char select / pause | `Main Menu` | - | - | - | - |
| Hosting lobby, waiting | `Hosting a Lobby` | `Waiting for an opponent` | 1/2 public | `+connect_lobby <id>` | - |
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

## Known accepted behavior

- Dual training spawns a second game process under the same Discord account;
  both update the same presence, so the last writer wins and the activity may
  flap. Matches Steam lobby behavior; not suppressed.
- Rollback is cheap: a player sets mode 0, deletes `discord_partner_sdk.dll`,
  or reinstalls. Code rollback is a git revert.
