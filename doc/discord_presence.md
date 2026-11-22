# Discord rich presence

Timestone can show what you are doing on your Discord profile: the lobby you
are in, whether a set is being played, who you are playing, both teams, the
score and how long the match has run. Public rooms with a free seat also get a
**Join** button.

It talks to the Discord **desktop** app over its local pipe. There is no SDK
DLL, no sign-in and no server involved, and nothing is published unless you
turn it on.

## Turning it on and off

`Timestone.ini`, beside `umvc3.exe`:

```ini
[Discord]
RichPresence=1
```

`1` is **Basic**, anything else (and the default) is **Off**: no pipe is opened
and nothing is sent.

In game:

- Esc or F3 → Settings → the **Discord Rich Presence** checkbox. Available in a
  match and offline; the lobby has its own screens and no Esc menu.
- Launcher (`-launcher`) → Settings → **Discord Rich Presence (lobby, opponent,
  match time)**, below the Steam routing option.

The choice is saved immediately. Turning it off clears the activity within a
second and stops the worker; turning it on again starts it.

## What Basic publishes

| Where | Details | State |
|---|---|---|
| Online lobby, no room | In the online lobby | Browsing rooms |
| In a room | Room: `<name>` | Hanging out / Seated at setup N / Queued (#N) / Starting a match |
| Playing a room match | vs `<opponent>` - `<wins>-<losses>` (FT`<n>`) | `<your team>` vs `<their team>`, with a match timer |
| Watching a room match | Watching `<player 1>` vs `<player 2>` | Room: `<name>` |
| Launcher training | Training mode | — |
| Launcher direct IP match | Direct online match | — |

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
sign-in.

The cold start cannot carry the room: you land in the lobby, not in the room,
so click Join once more from the profile. With the game already running, Join
goes straight to the room.

## Linux and Proton

The pipe lives on the same machine as the Discord client. Under Proton the game
sees a Windows prefix with no Discord, so the plugin logs `Discord: desktop app
not found` and publishes nothing until a pipe bridge forwards
`discord-ipc-0` from the host into the prefix.
