MVC3 Rollback :steamhappy:

# Compilation

Builds with MinGW-w64: cross-compile from Linux/macOS, or build natively on
Windows (MSYS2). Prereqs: a MinGW-w64 toolchain (gcc, g++, windres, binutils),
GNU make, and zip.

    make release     # -> Timestone.asi + TimestoneUpdater.exe (+ Timestone.zip)

NixOS:    nix develop -c make release       (or: nix build .#timestone)
Windows:  in an MSYS2 MINGW64 shell, first `pacman -S mingw-w64-x86_64-toolchain make zip`, then `make release`.

# Updates

`make` builds both `Timestone.asi` and `TimestoneUpdater.exe`. Install both files
beside the game executable. When the launch menu opens, the ASI asks the updater
to check the latest public release at `github.com/tilderain/Timestone`.

Each GitHub release must use a numeric tag such as `0.1.1` or `v0.1.1` and
contain assets named exactly `Timestone.asi` and `TimestoneUpdater.exe`. Both assets
are verified against their GitHub SHA-256 digests. The updater stores its own
replacement as `TimestoneUpdater.exe.new`; the updated ASI installs it before the
next launch-menu update check. The updater retains the previous ASI as
`Timestone.asi.old` and never modifies configuration files. Update the `VERSION`
value in the `Makefile` before publishing a release.

Launching `TimestoneUpdater.exe` directly checks the version resource of the
adjacent `Timestone.asi` and offers to install a newer release. The ASI launches
the same check quietly at startup; it displays a prompt only when an update is
available. Accepting the launch-menu prompt downloads and verifies the update,
closes UMVC3, replaces the ASI, and relaunches the game.

# Automated dual training

Use `-five-bars` with a training launch to start both teams with five hyper bars
(and every Frank at level 3).

Launch the game with `-autotest=<character>` to start a dual-training pair,
place the selected character in all P1 and P2 slots, and run the unique-command
autotest suite after both instances reach the synchronized battle boundary.
Character names are case-insensitive and may omit spaces and punctuation.
Both game processes close automatically after the suite completes.

For AI-assisted desync investigation, restrict the run to selected moves and
repeat each selected move independently:

```text
umvc3.exe -autotest=Ryu -autotest-moves="12,Hadoken" -autotest-repeat=5
```

`-autotest-moves` accepts comma-separated case-insensitive name fragments or
numeric runtime move ordinals. Numeric ordinals refer to the character's
runtime command list before duplicate removal. `-autotest-repeat` defaults to
three for focused move runs and is limited to 100. Each repetition performs a synchronized training
reset and receives its own move ID, so the TSV result and host/client
snapdumps can be correlated. The existing date, character, move, checksum,
and mismatch-frame metadata remains in the result TSV.

Examples:

```text
umvc3.exe -autotest=Ryu
umvc3.exe "-autotest=Phoenix Wright"
umvc3.exe -autotest=27
```

The host-side AI helper can derive a focused run from a prior result file and
launch the game pair:

```text
python tools/autotest_ai.py --character Hawkeye \
  --failed-tsv "../ULTIMATE MARVEL VS. CAPCOM 3/autotest/failed.tsv" \
  --repeat 5 --launch
```

Without `--launch`, it only prints the command so an external test runner can
start it. With `--launch`, it changes into the game directory, starts the game
through Wine, waits for completion, finds the newest matching host/client
`.usdp` pair, and writes `autotest/latest_ai_report.json` using
`tools/compare_desync_dumps.py`. The Wine executable defaults to `wine` and can
be overridden with `--wine`; a specific prefix can be set with `--wine-prefix`.

External automation should request JSON instead of parsing the human-readable
command:

```text
python tools/autotest_ai.py --character Ryu --moves "12,Hadoken" \
  --repeat 5 --json --manifest autotest/plan.json
```

The JSON contains an absolute executable path, working directory, argument
array, selected moves, repetition count, and run ID. For a managed launch, add
`--launch --timeout 1800`; the final manifest also includes the outcome,
artifact paths, comparator diagnosis, and first-divergence data. Only artifacts
created after launch are considered, preventing stale dumps from an earlier
run from being analyzed.

Runner exit codes are `0` for sync/no desync artifacts, `1` for a confirmed
desync report, `3` for launch/game failure, `4` for timeout, and `6` when the
snapshot analyzer fails. Argument errors use argparse's standard exit code `2`.

The network address and port come from `[Network]` in `Timestone.ini`.
Advanced/offline setup uses `[Roster.P1]`, `[Roster.P2]`, and `[Training] StageId`.
Online hosting/joining uses a separate `[ConnectRoster]` for your own characters,
colors, assists, simple-mode controls, and preferred stage, regardless of your side.
Other players' settings are match state only and never replace your preferences.
`rollbacksetting.ini` is no longer read or written; there is no automatic migration.


# Credits

Armonte- Many reverse engineering and documentation, Shot snapshot base, inspiration from his repo (exception handler.. etc)

Hkhann- Prior reverse engineering, the one who started it all with the OG UMVC3Rollback repo

catboy_friender (Sheep)- Help with Hkhann's repo, prior reverse engineering

Eternal Yoshi- Prior reverse engineering, ThreeWorkTool, playtesting

Ryn (wistfulhopes), anotak, Gneiss, tabsmvc, EddieLopes, dantarion, tge-was-taken - Prior reverse engineering

Archeroflegend- Framestepper, playtesting and harassment

Vevota- Playtesting and harassment

Jasongamedev- Playtesting

//FIXMESTEVE: : Standby support

John2143- "Sponsorship", build help and signing, discord bot

ermaccer- UMVC3Hook

ThirteenAG- Ultimate ASI Loader



Those random guys who played with me in an ingame lobby one day


# Third-Party Licenses

Licenses for code used in this project are collected in [THIRDPARTYNOTICES](THIRDPARTYNOTICES).
These licenses cover their respective projects, not all of Timestone. Timestone licensing is undecided.

| Component | License / notices |
| --- | --- |
| [ermaccer/UMVC3Hook](https://github.com/ermaccer/UMVC3Hook) | Permission granted by ermaccer: "just keep any edits of my work open source." |
| [4423Q/UMVC3Rollback](https://github.com/4423Q/UMVC3Rollback) | HkHaan's work with catboy_fucker, used with permission |
| [ENet 1.3.18](https://github.com/lsalzman/enet) | MIT, Lee Salzman |
| [Dear ImGui, including backends](https://github.com/ocornut/imgui) | MIT, Omar Cornut, v1.85 WIP |
| [kiero 1.2.6](https://github.com/Rebzzel/kiero) | MIT, Rebzzel |
| [MinHook](https://github.com/TsudaKageyu/minhook) | BSD-2-Clause, Tsuda Kageyu |
| Hacker Disassembler Engine inside MinHook | BSD-2-Clause, Vyacheslav Patkov; full HDE32/HDE64 notices included |
| [stb_vorbis and ImGui's three imstb headers](https://github.com/nothings/stb) | MIT OR Unlicense, Sean Barrett, MIT used |
| [ProggyClean embedded font](https://github.com/bluescan/proggyfonts) | MIT, Tristan Grimmer, separate from ImGui's notice |
| [ModUtils helpers](https://github.com/CookiePLMonster/ModUtils) | MIT, Adrian Zdanowicz (Silent), (CitizenFX scanner needs a license check) |
| Archer's framestepper | MIT, Archer; see [LICENSEframestepper](LICENSEframestepper) |
| [Ultimate ASI Loader 9.7.2 x64](https://github.com/ThirteenAG/Ultimate-ASI-Loader) | MIT, ThirteenAG; bundled as dinput8.dll |
| miniz inside Ultimate ASI Loader | MIT, RAD Game Tools/Valve and Rich Geldreich/Tenacious Software |
| [FunctionHookMinHook wrapper inside the loader](https://github.com/ThirteenAG/injector) | MIT, praydog, injector zlib and exception-tracer public-domain notices also included |
| ImGui Japanese glyph-range dataset | CC BY-SA 2.1 Japan per the vendored notice; IPA Japan attribution and license URI preserved |
| [Statically linked MinGW-w64 runtime](https://github.com/mingw-w64/mingw-w64) | Mixed runtime licenses; complete 8.0.0 notice bundle included for the local toolchain; other release toolchains need their matching notices |
| [MCF Gthread](https://github.com/lhmouse/mcfgthread), if linked by the release toolchain | LGPL-3.0-or-later with additional permissions including GCC Runtime Library Exception 3.1; conditional notice included |

No license was found for [rdbo's ImGui/DirectX 9/kiero hook example](https://github.com/rdbo/ImGui-DirectX-9-Kiero-Hook).

`THIRDPARTYNOTICES` should be included in the release zip, but is not yet


