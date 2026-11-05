# Timestone — Public Roadmap

Status: `[ ]` not started · `[~]` in progress · `[x]` done.
Priority: **P0** = blocks launch / breaks play · **P1** = fix soon · **P2** = lower priority.

> Timestone adds rollback netcode, replays, and training tools to UMVC3 on Steam.
> This is what we're working on — statuses update as things ship. Public summary
> of the working roadmap in `doc/private_roadmap_v1.md`. Last updated: 2026-09-01.

## Desyncs (P2)

- [ ] **Entity visual-state bugs** — characters and effects invisible, lingering, or T-posing; characters vanishing after DHC/THC/super
- [x] **Round-start assist-hold team swap desync**
- [ ] **Team-roster change between matches → start-of-match desync**
- [ ] **Randomize team → black screen + host desync**
- [~] **Rocket Raccoon trap desyncs/crashes**
- [~] **Strider ouroboros crash + Legion/tigers desync**
- [~] **Dormammu spell desyncs** (eruption/inferno/volcano)
- [ ] **Hsien-Ko desyncs/crashes** — gong/bombs/items; bombs won't detonate
- [~] **Dr. Strange desyncs** — FoF+gems, Grace of Hoggoth, teleport/Bolts
- [ ] **Nova desyncs/crashes** — St. L 99, human rocket punch + Doom super
- [ ] **Hawkeye desyncs/crashes** — speedshot, Trick>Rusty Nail>Gimlet
- [~] **Jam Session cluster** — Dante+Deadpool+Sentinel crashes, Wesker pickup
- [~] **Chris** — grenade crashes, lvl3 rapid-fire, 2nd-rocket desync
- [~] **Vergil** — sword desync/crashes, lvl3 corner freeze
- [~] **Spencer** — zip-move crashes
- [~] **Frank West** — cart crash, zombies attaching after Blue Light Special
- [~] **Trish** — crashes, sword-super/hard-tag desync
- [~] **Amaterasu** — effect crashes, Heavy Paper desync
- [ ] **Tron** — lvl1→PW DHC, servbot lunchtime DHC desyncs
- [ ] **Jill lvl1 machinegun spray desync**
- [~] **Arthur** — cross sword, axe throw, j.M-after-j.214M desync
- [ ] **MODOK** — shield-hit crash, projectile counter
- [ ] **Thunder Knuckle desync**
- [ ] **Pendulum / Bolts of Balthakk round-start desync**
- [ ] **Raw tag into Round Harvest on wakeup desync**
- [ ] **Phoenix Wright desyncs/crashes** — Missile assist + Maya super, PW-vs-Hsien-Ko drift, Phoenix-Trap crash
- [ ] **Spider-Man UWT state bugs** — stuck in web cocoon after UWT
- [ ] **Auto delay/rollback suggestion stutters at 0 frames**
- [~] **Desync detection misses** — silent desyncs
- [x] **Autosuperjump setting mismatch desync**

## Bugs

### P0 — Crashes / stability

- [~] **ERR09 on launch (fullscreen/vsync)**
- [~] **Resolution change crash / black screen / stuck 720p**
- [ ] **Black screen / hard freeze on match start**
- [ ] **AV/Defender false positives** — block download/update/launch
- [ ] **C.Viper crashes** — dp cancels, M Seismo vs fireball
- [ ] **Doom missiles/photons crash with many projectiles**
- [x] **Deadpool grenade/pineapple crash**
- [ ] **Shuma assist crash**
- [ ] **Iron Man drones crash**
- [ ] **Unidentified crashes** — no logs or clear trigger yet

### P1 — Netcode / UI / gameplay

- [ ] **One-sided rollback / frame skip when one end drops below 60fps**
- [~] **Health bars / team-order HUD not switching**
- [ ] **Controller not recognized online**
- [~] **Character-select hotkeys (F8/F9)**
- [ ] **Inputs ignored when window unfocused**
- [~] **Rematch screen / stuck after round**
- [~] **Invisible-assist / helper state bugs**
- [ ] **Overlay/notification interference** — Steam DM lag, NVIDIA clipping
- [ ] **Friends' lobbies missing from Steam lobby list**
- [ ] **Borderless Gaming interference**
- [x] **Muting lobby music kills in-game sound**
- [x] **Advanced menu cut off / DPI scaling**
- [ ] **Non-ASCII text broken in launcher menus**
- [x] **FPS counter misleading (120/60 mismatch)**

### P2 — Performance & hygiene

- [ ] **Long-session performance** — game gets choppier over time (effects/projectiles not cleaned up)
- [~] **F11-restart slowdown**

## Features

Planned and in-progress features:

- [ ] **Dark mode** — launcher window
- [ ] **Opponent WiFi / connection-type indicator + jitter display**
- [ ] **Steam link / invite to join lobby**
- [ ] **Lobby join indicator in-game**
- [ ] **Lobby modes: KOTH / rotations / FT3-5 selector / big lobbies**
- [x] **Minimize upper-left panel**
- [ ] **Automatic crash/desync report upload**
- [~] **Low-spec / potato config in launcher**
- [ ] **Skill-level lobby field** (Beginner/etc)
- [~] **Disable stage objects option**
- [ ] **Swap P1↔P2 team positions in launcher**
- [~] **Quick chat / one-button emotes**
- [x] **Preset / saved teams**
- [ ] **Sound when someone joins lobby**
- [~] **Opponent ping before entering / while hosting**
- [x] **Usernames in rollback HUD**
- [x] **Direct-IP connect option**
- [~] **Launcher redesign (FGC look)** — char icons, color dots, SFX, hotkey toggle, footer links
- [~] **Delay/rollback preset before match**
- [ ] **Kick players from lobby**
- [ ] **Lobby entry codes**
- [ ] **Character search/filter in character select**
- [~] **In-game character select / ready button / rematch screen**
- [~] **CTRL+F11 full restart keeping connection**
- [~] **Manual log save hotkey** (for bug reports)
- [ ] **Client-side stage selection**
- [~] **Replay system + game score**
- [ ] **Voice chat** — not planned (bandwidth)
- [x] **Music in launcher**
- [~] **Per-character/team voice & language settings**
- [x] **Bonne Wonderland default stage / remember last stage**
- [~] **Per-lobby toggle for empty 3rd slot (None/Galactus)**
- [~] **Fullscreen / resolution handling**
- [~] **Set controls from launcher**
- [ ] **Training-mode delay setting**
- [ ] **Controller support in the launcher**
- [ ] **Same-color lockout in launcher**
- [~] **Mod pause menu keyboard UX**
- [ ] **In-game chatbox during match**
- [~] **Suppress hotkeys while typing in text fields**
- [~] **Toggle to disable mash-instant-rematch**
- [x] **Hold assist to swap point character**
- [~] **Hotkey to randomize team + restart**
- [ ] **Return to client / rehost or rejoin without closing the game**
- [ ] **Practice vs simulated rollback (CPU/dummy)**

## Stretch goals

Ideas we're exploring — nothing promised:

- [ ] **Ranked matchmaking**
- [ ] **Spectator mode**
- [ ] **Training while matchmaking / in-game lobby integration**
- [ ] **Mod / Community Edition support**
- [ ] **Region settings for the mod**
