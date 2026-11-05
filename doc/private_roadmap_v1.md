# Timestone — Working Roadmap (private)

> Internal document — do not publish as-is. Public-facing summary:
> `doc/public_roadmap_v1.md`. Keep the two roadmaps in sync.

Only approved items are tracked here. Status: `[ ]` not started · `[~]` in
progress · `[x]` done. `[User Req]` = community bug/feature reports (Discord).
`(→ x-ref)` = related entry tracked elsewhere; `[verify]` = status uncertain.
Priority: **P0** = hard-stops launch / breaks play · **P1** = fix soon · **P2** = slow burn (desyncs: the match restarts, the game doesn't)

## Desyncs (P2 — slow burn; desyncs restart the match, not the game)

- [ ] **[User Req] Entity visual-state bugs (invisible/lingering/T-pose)** — the most-reported class (~15 reports): Morrigan soul fist, Mag/Akuma/Spidey/Firebrand fireballs, Nova pulse+shield, MODOK dorito, Vergil swords, Doom missiles, Thor Mighty Spark, Taskmaster/Strider-B counter-reflect, Joe voom; **plus characters vanishing after DHC/THC/super** (Dante after DT, Akuma after THC, 3rd char vanished, Spencer). Archer: "known bug"; rain: "missing shot state or setup". Partial fixes: `b2e8b79` shot-model desync (8/23), `b26eca5`/`906b584`/`ce25933` hidden missiles, `76756c3` Spidey effect crash, `a3a95f8` Morrigan effect crash, `fb570a1`/`275f40b` Joe, `73a41b2` chars invisible on resim (7/28), `c887425` cUnit flag restore (8/20). (→ invisible-assist; → UWT)
- [x] **[User Req] Round-start assist-hold team swap desync** — 7 reports; rain: "assist to swap at start is broken"; fixed by `6cbfb7a4` "Sync starting character assists" (8/29, controller opening-member capture + peer sync + `doc/match_start_assist_sync.md`); no regression follow-up found
- [ ] **[User Req] Team-roster change between matches → start-of-match desync / F11 char-lock** — tvisionary 8/28–8/29: locked into Hulk after F11; hulk/none→nemesis/none desyncs every match (→ round-start assist-swap)
- [ ] **[User Req] Randomize team (P2) → black screen + host desync** — nity 8/29; no follow-up fix after `08455a` (8/19) found (→ random-all hotkey feature)
- [~] **[User Req] Rocket Raccoon trap desyncs/crashes** — log trap worst (6 reports); rain: "log starts t-posing for 1 frame"; partial fixes `1c5bc52` prickle board, `0252327` jetpack desync, `fb83b7d` general (all 8/22); log-trap sub-item `[verify]`
- [~] **[User Req] Strider ouroboros crash + Legion/tigers desync** — ouroboros fixed (`a746acc8`, `3dd340af` 8/26); Legion "tigers t-posing" report 8/27 open; `c0d9dc7` tigers fix (8/8) predates it — no post-8/26 strider/legion work
- [~] **[User Req] Dormammu spell desyncs (2R1B eruption/inferno/volcano)** — `551666e7` inferno fix (8/19); reports continue through 8/30
- [ ] **[User Req] Hsien-Ko desyncs/crashes** — gong/bombs/items; "all games against hsien-ko desynced"; bombs won't detonate (→ PW entry: PW-vs-Hsien-Ko drift)
- [~] **[User Req] Dr. Strange desyncs** — FoF+gems, Grace of Hoggoth vanish, teleport/Bolts (4 reports); prior attempt `ae90bb0` "dr strange bandaid fix" (8/1)
- [ ] **[User Req] Nova desyncs/crashes** — St. L 99 (80% insta-desync vs Vergil), human rocket punch + Doom super
- [ ] **[User Req] Hawkeye desyncs/crashes** — speedshot, Trick>Rusty Nail>Gimlet, up-arrows blocked
- [~] **[User Req] Jam Session cluster** — Dante+Deadpool+Sentinel crashes, Wesker pickup+Jam Session desync; `a371a872` attempt (8/14); reports continue 8/27–28
- [~] **[User Req] Chris** — grenade crashes fixed (`124e572` pineapple, `f481bc47`, `eb704dcd`, `cc7ea4b4` firegrenade blacklist, `37163a5`, `723fc6b`); lvl3 rapid-fire + 2nd-rocket desync open
- [~] **[User Req] Vergil** — sword desync/crash fixes `d5d8926c`/`c6bb545`/`7a683b6`/`5186e4f` (8/22); lvl3 corner freeze open (invis → entity visual-state)
- [~] **[User Req] Spencer** — zip crash fixes 8/13 (`bf6989f2`, `12c163df`, `e9bae1b`); crash still reported 8/25 `[verify]`
- [~] **[User Req] Frank West** — cart crash/weapon desync fixed (`f90d1d1e` 8/22); zombies-attach after Blue Light Special + Funny Face Crusher desync open
- [~] **[User Req] Trish** — `306c1034` crash fix (8/13) + `66f2866` spada boomerang (8/21); sword-super/hard-tag desync open
- [~] **[User Req] Amaterasu** — effect-crash fix reverted `0961c51` (8/14, leak); attempts `fd0dbe0`, `dc99ec5` (Okami shuffle); Heavy Paper desync open (corrected hash — `370fb6f1` was unrelated)
- [ ] **[User Req] Tron** — lvl1→PW DHC, servbot lunchtime DHC desyncs
- [ ] **[User Req] Jill lvl1 machinegun spray desync**
- [~] **[User Req] Arthur** — `ab6ded7` cross sword (8/21), `3fb05ad` axe throw (8/7); j.M-after-j.214M desync open
- [ ] **[User Req] MODOK** — shield-hit crash, projectile counter; dorito-invisible tracked under entity visual-state
- [ ] **[User Req] Thunder Knuckle desync**
- [ ] **[User Req] Pendulum / Bolts of Balthakk round-start desync** — 3x reproducible
- [ ] **[User Req] Raw tag into Round Harvest on wakeup desync**
- [ ] **[User Req] Phoenix Wright desyncs/crashes** — Missile assist + Maya super (0.2.7 "summons" desyncs), PW-vs-Hsien-Ko state drift, Phoenix-Trap crash (3–4 reports 8/27–8/30); partial `5e99786` item throw + intro-freeze attempt (8/22), `910d3ae` cchrstatus::mode (8/8)
- [ ] **[User Req] Spider-Man UWT state bugs** — stuck in web cocoon after UWT, web ball stuck on opponent (3 reports); zero commits found — fully open (→ entity visual-state for invis-only reports)
- [ ] **[User Req] Auto delay/rollback suggestion stutters at 0 frames** — Shiburizu! 8/31: low-ping suggests 0 rollback yet pauses persist; plus rain-acknowledged `8/27`: "2 delay/8 rollback rolls back instead of staying in delay mode at 16 ms"
- [~] **[User Req] Desync detector false negatives** — silent desyncs (50s desync resolving next match); attempt family `f728fe6f` (8/26), `5840ea8` "Maybe fix fake desyncs" (8/24), `95fcfaf`/`f05dafe` (8/14–15), Aug 2–4 batch; rain: "sometimes completely fails"
- [x] **[User Req] Autosuperjump setting mismatch desync** — `89a73ab` + `a1dfcfa` sync (8/27)

## Bugs

### P0 — Crashes / stability
- [~] **[User Req] ERR09 on launch (fullscreen/vsync)** — ~14 reports; root cause: mod forces VSYNC on → ERR09 when config read-only; fixes `90107c95` (8/28 race fix), `b5d7ec81`, `5e166919`, `741a843` d3d9-reset-vsync-disabled, `7c39937` redundant-reset check, `38b31e5` (8/5); **regression note**: forced vsync-off → choppy frames for some (Tmoe 8/28) **· launch-blocker**
- [~] **[User Req] Resolution change crash / black screen / stuck 720p** — ~10 reports (incl. in-game PC-settings changes blocked while mod active, eyeamtom 8/29); rain iterated multiple ASI builds 8/28; Tachyon stuck-720p + 45s black boot 8/31; (→ borderless-Gaming; → ERR09) **· launch-blocker**
- [ ] **[User Req] Black screen / hard freeze on match start** — join AND host: 0.5s audio then frozen (Xurlglurlblu ×3 8/27), "send fail result = 3" (CadyRocks 8/27), host black screen (Eternal Yoshi 8/28); 5 reports — distinct from resolution-change black screen **· launch-blocker (match start)**
- [ ] **[User Req] AV/Defender false positives** — block download/update/launch (~4 reporters); known-issues note, not a code bug (→ signing helps once re-enabled) **· launch-blocker**
- [ ] **[User Req] C.Viper crashes** — dp cancels (8/28), M Seismo vs Firebrand fireball (8/29)
- [ ] **[User Req] Doom missiles/photons crash with many projectiles** — 6 crash reports (foot-dive fireballs, mass missiles + Akuma tatsu, lvl3 X happy birthday, H hyper grav, Sentinel j.S, photons); partial `b26eca5` hidden-missiles desync fix
- [x] **[User Req] Deadpool grenade/pineapple crash** — `124e572` pineapple, `f481bc47`/`eb704dcd` grenade fixes
- [ ] **[User Req] Shuma assist crash**
- [ ] **[User Req] Iron Man drones crash** — single report, no logs (8/26)
- [ ] **[User Req] Unattributed / no-log crash triage bucket** — ~11 reports with no identified trigger; several generate no log (crash-before-log) — needs the logging-flow work (→ auto-upload feature)

### P1 — Netcode / UI / gameplay
- [ ] **[User Req] One-sided rollback / frame skip when one end drops below 60fps** — needs repro; frame advantage already sent; **mitigation idea**: slowdown/speedup toward other instance or sync points at cinematics/super-flashes (Wednesday 8/30, SFV-style)
- [~] **[User Req] Health bars / team-order HUD not switching** — bar position, portraits, assist-slot HUD (7+ reports); `5a27bcc` re-enabled `Rollback_SyncActiveCharacterUi()` post-resim + teamId/playerId mixup fix (8/31); post-fix community retest pending
- [ ] **[User Req] Controller not recognized online** — PS4 pad (base + Madcatz TE2 PS4 mode), "controls fried on match enter"; sub-note: keyboard dash-macro suppresses up-input (Canyon 8/26, NKRO-verified — looks mod-side)
- [~] **[User Req] F8/F9 chrsel hotkeys** — F9 reliable (`a98ca22` 8/20); F8 still flaky (no F8 fix found); restart-stability refs `3dfc921` (8/27 routes F11/match-end through chrsel restart), `643ab39` (8/22), `7ca6a08` (8/27)
- [ ] **[User Req] Inputs ignored when window unfocused** — (→ hotkeys-while-typing feature; same requester, both channels)
- [~] **[User Req] Rematch screen / stuck after round** — auto-restart after outro exists (`1604daad`, `cc23cd0f`); hotkeys dead on rematch screen reported (→ in-game CSS feature; → rematch-toggle feature)
- [~] **[User Req] Invisible-assist / helper state bugs** — kitty helper stuck (inez h 8/26), invisible assist char (loog 8/27); partial `1c1db66` assist/helper input fix (8/14), `73a41b2` (→ entity visual-state)
- [ ] **[User Req] Overlay/notification interference** — Steam DM during play → significant lag (Glass 8/26); NVIDIA clipping pauses game (Rowdy Howdy 8/26)
- [ ] **[User Req] Friends' lobbies missing from Steam lobby list** — Rowdy Howdy 8/27 (AU); rain: "friends lobbies don't show in the steam return lobby list by default, i'll look into it" (→ Steam link feature)
- [ ] **[User Req] Borderless Gaming interference** — crash on launch with it on (8/26); connect fails to start if either side runs it (Genz 8/30) (→ resolution; → fullscreen feature) **· launch-blocker (env)**
- [x] **[User Req] Muting lobby music kills in-game sound** — `b0548d2` (8/31)
- [x] **[User Req] Advanced menu cut off / DPI scaling** — `f4156f8` (8/31)
- [ ] **[User Req] Non-ASCII text broken in launcher menus** — accents → `?`; greek/arabic/hebrew missing (Teo G 8/29)
- [x] **[User Req] FPS counter misleading (120/60 mismatch)** — "get actual fps instead of confusing people" `c05d65e` (8/29); Linux stuck-60 report (Schwi 8/27) predates it

### P2 — Performance & hygiene
- [ ] **[User Req] Performance debt (effect/projectile leak)** — "game not cleaning up effects/props/projectiles" (Eternal Yoshi); Dark Phoenix tanks FPS; "choppier the longer the game goes"; chug after ~15 matches (Cobbler/RINA 8/31); refs `6e1f940` log-perf fix (8/31), `e77515e` Dark Phoenix effect crash (8/24)
- [~] **[User Req] F11-restart slowdown** — fixed for Amidoo by `8838af7e` "Fix for 3 perf regressions" (8/29); reports 8/31 postdate `[verify]`

## Dev requests
Internal asks from the dev team — held above community feature requests.

- [ ] **[Dev Req] App icons** — icon for the game executable, launcher, and Windows (taskbar/titlebar)
- [ ] **[Dev Req] Steam lobby watcher** — poll all lobbies every few seconds and list them, groundwork for a future Discord integration
- [ ] **[Dev Req] Server infra: 3rd-party login server, ranked matchmaking, community moderation tooling** — the design direction for the Ranked stretch goal (→ Ranked matchmaking)
- [ ] **[Dev Req] Steam browser protocol (steam://) for direct lobby joins** — `steam://joinlobby/357190/<lobbyID>` (undocumented protocol, stable; SG uses it ~10 yrs). Mechanism: Steam passes `+connect_lobby <id>` / `GameLobbyJoinRequested_t` — receiving side already exists (`df48dbb`); work = link generation + routing. (→ Steam link/invite feature)

## Features
- [ ] **Dark mode** — launcher window (LaunchMenu.cpp Win32 dialog); Javy 8/27 + z1thern22 8/31 (→ FGC redesign — named component)
- [ ] **[User Req] Opponent WiFi / connection-type indicator + jitter display** — 4 requests (Javy 8/27, Frank Weast 8/31, T.Hawk 8/31, MAD MEW MEW 8/31); idea: ms/ping lobby filter (SIF 8/27)
- [ ] **[User Req] Steam link / invite to join lobby** — invite→game attempted (`df48dbb`); paste-link joining open (→ friends-lobbies bug; → Dev req: steam:// protocol)
- [ ] **[User Req] Lobby join indicator in-game** — see who joins persistently, not just at char-pick (Miss Monica 8/27) (→ join sound)
- [ ] **[User Req] Lobby modes: KOTH / rotations / FT3-5 selector / big lobbies**
- [x] **[User Req] Minimize upper-left panel**
- [ ] **[User Req] Crash/desync report auto-upload** — dev previously judged too much work; revisit (→ no-log crash bucket)
- [~] **[User Req] Low-spec / potato config in launcher** — `doc/lowspec.md` exists; launcher option open
- [ ] **[User Req] Skill-level lobby field** (Beginner/etc)
- [~] **[User Req] Disable stage objects option** — stage-object RNG disabled globally (`7bedcdc`); per-toggle open
- [ ] **[User Req] Swap P1↔P2 team positions in launcher**
- [~] **[User Req] Quick chat / one-button emotes** — pre-launch chat done (`3bf1346`); in-game emotes open
- [x] **[User Req] Preset / saved teams** — announced in-thread; `doc/saved_roster_presets.md` exists
- [ ] **[User Req] Sound when someone joins lobby** (→ join indicator)
- [~] **[User Req] Opponent ping before entering / while hosting** — lobby-list ping done (`d43bcf2` 8/27, `8b69235` 8/27, `a8887e5` 8/21 RTT/2); connected-opponent ping open; note: UI ping = one-way time (Steam returns RTT) — intended, not a bug
- [x] **[User Req] Usernames in rollback HUD** — `34359c7` steam name in ingame name slot
- [x] **[User Req] Direct-IP connect option** — `1a6f5bc8` direct-connect toggle + IP/port fields (8/25); relief for bad Steam-relay routes (fredystar200 8/30)
- [~] **[User Req] Launcher redesign (FGC look)** — char icons, color dots, SFX, hotkey toggle, footer links (incl. ko-fi — Vagabond 8/31); dark mode is a component; Archer: prefer hooking in-game lobby system
- [~] **[User Req] Delay/rollback preset before match** — automatic delay set (`1d9d0880`), default delay (`d99f8496`)
- [ ] **[User Req] Kick players from lobby**
- [ ] **[User Req] Lobby entry codes** — dev said "soon"
- [ ] **[User Req] Character search/filter in chrsel textbox**
- [~] **[User Req] In-game CSS / ready button / rematch screen** — launcher ready button (`1a6f5bc8`), ingame char select (`ed646dd`); full flow open (→ rematch bug; → rematch toggle)
- [~] **[User Req] CTRL+F11 full restart keeping connection (memory flush)** — F11 restart exists; variant open
- [~] **[User Req] Manual log hotkey** — hooklog flush exists (`6f613bf`); standalone hotkey open
- [ ] **[User Req] Client-side stages**
- [~] **[User Req] Replay system + game score** — `doc/replay.md` exists; feature state `[verify]`
- [ ] **[User Req] Voice chat** — documented-declined by devs (Archer "Lolno"; Eternal Yoshi bandwidth argument)
- [x] **[User Req] Music in launcher** — `d0793e1` "Lobby music lol" + fade/fix commits
- [~] **[User Req] Voice/lang settings per char/team** — `4c1490e` lang per team (8/31); stage/dynamic music open
- [x] **[User Req] Bonne Wonderland default stage + remember stage** — `55b7aab`
- [~] **[User Req] None/Galactus per-lobby toggle** — none char in chrsel (`469f50`); toggle open (→ 3rd-char-vanished bug, "coded none too well")
- [~] **[User Req] Fullscreen / resolution handling** — borderless conversion (`a7f9773`), err09 fixes (→ ERR09 bug, → resolution bug); exclusive FS still blocked; **flip-model idea**: dgVoodoo d3d11_fl11_0/flip_discard cuts display lag (Tyron verified via PresentMon 8/29; rain considering default)
- [~] **[User Req] Set controls from launcher** — incl. controller button mapping; taunt unmapped on pad (Pabloz 8/31)
- [ ] **[User Req] Training-mode delay setting** (match online feel)
- [ ] **[User Req] Controller control of launcher**
- [ ] **[User Req] Same-color lockout in launcher**
- [~] **[User Req] Mod pause menu keyboard UX** — esc menu exists (`a3085b9`); flow open
- [ ] **[User Req] In-game chatbox during match** — pre-launch chat only
- [ ] **[User Req] Hotkeys suppressed while typing** (text input adds rollback) — `5c9527f` `[verify]` (→ unfocused-inputs bug)
- [~] **[User Req] Disable mash-instant-rematch toggle** — auto-restart exists (`1604daad`); toggle open
- [x] **[User Req] Hold assist to swap point character** — works in normal play (rain 8/30); round-start case fixed, tracked in Bugs
- [~] **[User Req] Random-all restart hotkey** — randomize chr (`08455a`) + F9 restart (`a98ca22`); combined key open (→ P2-randomize bug)
- [ ] **[User Req] Return to client / rehost or rejoin without closing the game** — Miss Monica 8/28; Archer: "eventually" (→ training-while-matching stretch)
- [ ] **[User Req] Emulate rollback locally (vs dummy/CPU)** — Noob 8/26: practice against simulated rollback opponent (→ training-while-matching stretch)

## Stretch goals
- [ ] **Ranked matchmaking** — first requested by //FIXMESTEVE 8/26 ("+R lobbies/quickmatch"), then perfectlittle42 8/31; devs: "too early"; needs design (identity, ladder, service) (→ Dev req: 3rd-party login + moderation tooling = the chosen direction)
- [ ] **[User Req] Spectator mode** — devs: "down the line"
- [ ] **[User Req] Training while matchmaking / in-game lobby integration** — "defo in the future"; no-external-launcher goal (→ return-to-client; → emulate-rollback)
- [ ] **[User Req] Mod / Community Edition support** — devs: after base game
- [ ] **[User Req] Region settings for the mod** — Jufo 8/29: "not necessary for a loooong time"
