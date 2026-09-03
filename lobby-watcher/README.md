# lobby-watcher

Private dev tool (not shipped to players). Headless service that polls **all
Timestone lobbies** (every version — no protocol filter) every 15 s through
the public Steamworks lobby-search API. Emits structured **tracing events** as
log lines, and optionally runs a Discord bot with slash commands (set
`DISCORD_TOKEN`).

## How it runs

Prerequisites — on the machine running it:

- Steam **Linux client running and logged in** as an account that **owns UMVC3**
  (appid 357190). The Steamworks API operates as the logged-in Steam user; there
  is no server-side equivalent. Without this, the service logs `event="init_failed"`
  every 5 s and retries.
- No config, no flags, no `steam_appid.txt`, no files written. One instance per
  machine. Logs go to stderr; Ctrl+C stops it.

```bash
nix run ./lobby-watcher          # built binary, logs to the terminal
nix build ./lobby-watcher#default # → ./result/bin/lobby-watcher

# dev loop:
cd lobby-watcher && nix develop  # sets STEAM_SDK_LOCATION + LD_LIBRARY_PATH
cargo run
```

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

- `/mm` — posts a complex embed listing every open Timestone lobby (busiest
  first): one field per lobby with name, host, protocol, player count, and a
  `https://mm.b.hero.rehab/joinlobby/…` join link for each lobby.

```bash
DISCORD_TOKEN=... DISCORD_GUILD_ID=... nix run ./lobby-watcher
```

The bot fails gracefully: a bad token logs `event="discord_error"` and the
watcher keeps polling.

## Events (wire contract for future integrations)

Target `lobby_watcher`, filter on `event`:

| event | fields |
|---|---|
| `start` | — |
| `init_failed` | — |
| `search_failed` | — |
| `created` / `updated` / `deleted` | `lobby.id`, `lobby.name`, `lobby.host`, `lobby.protocol`, `lobby.members`, `lobby.owner` (host SteamID64 — join links need it) |
| `poll` | `poll`, `total`, `created`, `deleted`, `updated` |

Diff semantics: `created` = new id; `updated` = name/host/protocol/members/
owner changed; `deleted` = absent for 2 consecutive polls (blip grace). No ping
field (steamworks 0.13.1 exposes none).

## Build notes

- `flake.nix` builds a native Linux binary; the Steamworks SDK's
  `libsteam_api.so` is committed in `sdk/` (copied from the Steam client's own
  `steamrt64/` — Valve-distributed, sha256-pinned). Do not replace it with a
  random download; if the Steam client bumps interface versions, re-copy from
  `~/.local/share/Steam/steamrt64/libsteam_api.so`.
- `steamworks = "=0.13.1"` — the build pins the SDK interface versions; bump the
  crate and the committed `.so` together.
