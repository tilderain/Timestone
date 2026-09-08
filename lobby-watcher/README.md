# lobby-watcher

Private dev tool (not shipped to players). Headless service that polls **all
Timestone lobbies** (every version - no protocol filter) every 15 s through
the public Steamworks lobby-search API. Emits structured **tracing events** as
log lines, and optionally runs a Discord bot with slash commands (set
`DISCORD_TOKEN`).

## How it runs

Prerequisites - on the machine running it:

- Steam **Linux client running and logged in** as an account that **owns UMVC3**
  (appid 357190). The Steamworks API operates as the logged-in Steam user; there
  is no server-side equivalent. Without this, the service logs `event="init_failed"`
  every 5 s and retries.
- No config, no flags, no `steam_appid.txt`. One instance per machine. Logs go
  to stderr; Ctrl+C stops it.

```bash
nix run ./lobby-watcher          # built binary, logs to the terminal
nix build ./lobby-watcher#default # → ./result/bin/lobby-watcher

# dev loop:
cd lobby-watcher && nix develop  # sets STEAM_SDK_LOCATION + LD_LIBRARY_PATH
cargo run
```

## Lobby lifecycle log

The watcher appends one plain-text line to `./lobby-watcher.log` every time a
lobby is **created** or **deleted** (the events in the table below, diffed
against the previous poll) - a persistent history of every lobby seen, ready
to grow into a database later. Override the path with `LOBBY_LOG_FILE`. The
file is opened append-only per event, so it survives restarts and can be
rotated or deleted while the watcher runs; a write failure only logs a warning
and never stops the watcher. `tail -f lobby-watcher.log` to watch live.

Each line is one event, one row (name/host are `{:?}`-escaped so quotes,
tabs, or other odd characters in player text never break the line):

```
2026-09-08T19:40:01Z created id=109775999888744469 name="Party \"Up\"" host="John\tDoe" protocol="1" members=2 owner=76561198000000000
2026-09-08T19:43:02Z deleted id=109775999888744469 name="Party \"Up\"" host="John\tDoe" protocol="1" members=2 owner=76561198000000000
```

Columns: UTC timestamp, event (`created` / `deleted`), then the lobby fields
from the events table. A `start` line bookends each process run.

Running 24/7 (e.g. a NixOS box where Steam stays up):

```bash
systemd-run --user --unit=lobby-watcher --property=Restart=on-failure \
  $(nix build --no-link --print-out-paths ./lobby-watcher#default)/bin/lobby-watcher
journalctl --user -u lobby-watcher -f   # logs land here
```

## Discord bot (optional)

Set `DISCORD_TOKEN` to enable the built-in bot. Set `DISCORD_GUILD_ID` to
register commands instantly in one server (otherwise global registration is
used, which can take up to an hour to propagate).

- `/mm` - posts a complex embed listing every open Timestone lobby (busiest
  first): one field per lobby with name, host, protocol, player count, and a
  `https://mm.b.hero.rehab/joinlobby/…` join link for each lobby.

- **Live lobby card** - set `DISCORD_LOBBY_CHANNEL_ID` (a text-channel
  snowflake, right-click → Copy Channel ID) and the bot keeps a single message
  in that channel updated with the same embed after every poll (~15 s). On
  restart it finds and reuses the existing card (no duplicates); if the message
  was deleted it reposts; if the channel/permissions are bad it logs
  `event="card_error"` and runs without the card.

```bash
DISCORD_TOKEN=... DISCORD_GUILD_ID=... DISCORD_LOBBY_CHANNEL_ID=... nix run ./lobby-watcher
```

If the server runs a third-party message-logging bot (YAGPDB, Carl-bot, …),
add the lobby-card channel (or this bot) to the logger's ignore list - every
15 s card edit would otherwise be recorded as a log entry (~5,760/day).

The bot fails gracefully: a bad token logs `event="discord_error"` and the
watcher keeps polling.

Card events use the same target (only emitted when a bot is enabled): the
events below.

## Events (wire contract for future integrations)

Target `lobby_watcher`, filter on `event`:

| event | fields |
|---|---|
| `start` | - |
| `init_failed` | - |
| `search_failed` | - |
| `created` / `updated` / `deleted` | `lobby.id`, `lobby.name`, `lobby.host`, `lobby.protocol`, `lobby.members`, `lobby.owner` (host SteamID64 - join links need it) |
| `poll` | `poll`, `total`, `created`, `deleted`, `updated` |
| `card_start` / `card_disabled` | `channel` |
| `card_found` / `card_recreated` | `message` |
| `card_error` | `reason` + `channel` (when known); `status`/`phase` when applicable |

Diff semantics: `created` = new id; `updated` = name/host/protocol/members/
owner changed; `deleted` = absent for 2 consecutive polls (blip grace). No ping
field (steamworks 0.13.1 exposes none).

## Build notes

- `flake.nix` builds a native Linux binary; the Steamworks SDK's
  `libsteam_api.so` is committed in `sdk/` (copied from the Steam client's own
  `steamrt64/` - Valve-distributed, sha256-pinned). Do not replace it with a
  random download; if the Steam client bumps interface versions, re-copy from
  `~/.local/share/Steam/steamrt64/libsteam_api.so`.
- `steamworks = "=0.13.1"` - the build pins the SDK interface versions; bump the
  crate and the committed `.so` together.
