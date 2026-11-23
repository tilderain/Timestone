# Discord rich presence

Timestone can show what you are doing on your Discord profile: the lobby you
are in, whether a set is being played, who you are playing, both teams, the
score and how long the match has run. Public rooms with a free seat also get a
**Join** button.

It talks to the Discord **desktop** app over its local pipe. There is no SDK
DLL, no sign-in and no server involved, and nothing is published unless you
turn it on. Until you do, Timestone does not look for Discord at all: no pipe,
no registry, no process list.

## Turning it on and off

The first time the online lobby's top screen is idle, Timestone asks once
whether to show your activity on your Discord status. **NO** is highlighted,
and backing out of the question counts as no. Either answer is saved and the
question is not asked again; you can change it later in Settings. Turning the
status on in a settings menu, or `ShowDiscordStatus=1` by hand, also counts as
an answer.

`Timestone.ini`, beside `umvc3.exe`:

```ini
[Discord]
RichPresenceSetup=1
ShowDiscordStatus=1
```

- `ShowDiscordStatus`: `1` is **On**; anything else, or a missing key, is
  **Off**: no pipe is opened and nothing is sent.
- `RichPresenceSetup`: `1` once you have answered the question or changed the
  setting.

In game:

- Esc or F3 → Settings → the **Show on Discord status** checkbox. Available in
  a match and offline; the lobby has its own screens and no Esc menu. Below it,
  a line shows the connection: connecting, desktop app not running, connected,
  or refused. There is no Discord sign-in; after a refusal, untick and tick the
  box to retry.
- Launcher (`-launcher`) → Settings → **Show on Discord status (lobby,
  opponent, match time)**, below the Steam routing option. Pressing Save
  without changing the box saves nothing.

The choice is saved immediately. Turning it off clears the activity within a
second, stops the worker and removes the Join protocol (below); turning it on
again starts it.

## What it publishes

| Where | Details | State |
|---|---|---|
| Online lobby, no room | In Menus | — |
| In a room | Lobby: `<name>` | Connected / Seated P`<n>` / Queued (#`<n>`) / Starting a match |
| Playing a room match | Playing: versus - `<wins>`-`<losses>` (FT`<n>`) | vs `<opponent>` - `<your team>` vs `<their team>`, with a match timer |
| Watching a room match | Watching a game | `<player 1>` vs `<player 2>` |
| In the game's main menu | In Menus | — |
| Launcher training | Training | — |
| Launcher two-window training | Training | — |
| Launcher direct IP match | Playing: versus | with a match timer |

Teams are the ones actually being played, so a member left to chance shows
the character the launch drew for it.

A **Join** button appears only for a room that is public (neither
invitation-only nor friends-only) and has a free seat; both players need the
presence on for it to work in either direction. Asking to join is accepted only
while that is still true. Private and friends-only rooms publish neither the
Join button nor the room id.

Not published, by design: URL buttons (Discord refuses them together with a
join secret), character portraits or other Capcom art, and anything about your
Steam account beyond the persona name other players already see in the room.

## Join, and what it can and cannot do

Discord's Join starts the game through `steam://rungameid/357190`, and the
plugin registers the protocol under
`HKCU\Software\Classes\discord-1544202390536982610` so that works without a
sign-in. Turning the status off in a settings menu deletes that key; editing
`ShowDiscordStatus=0` by hand does not, until the next time it is turned off
in a menu.

The cold start cannot carry the room: you land in the lobby, not in the room,
so click Join once more from the profile. With the game already running, Join
goes straight to the room.

## Linux and Proton

The pipe lives on the same machine as the Discord client. Under Proton the game
sees a Windows prefix with no Discord, so the plugin logs `Discord: desktop app
not found` and publishes nothing until a pipe bridge forwards
`discord-ipc-0` from the host into the prefix.
