MVC3 Rollback :steamhappy:

# Compilation

Makefile on Linux is supported for now (cross compiling a dll)

# Updates

`make` builds both `Timestone.asi` and `TimestoneUpdater.exe`. Install both files
beside the game executable. When the launch menu opens, the ASI asks the updater
to check the latest public release at `github.com/tilderain/NormalRepo`.

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

The network address and port come from `rollbacksetting.ini`. NDJSON capture
continues to follow the existing NDJSON logging setting.

# Credits

Armonte- Many reverse engineering and documentation, Shot snapshot, inspiration from his repo (multi asi loader, exception handler)
Hkhann- Reverse engineering, the one who started it all with the OG UMVC3Rollback repo
Ryn (wistfulhopes)- Reverse engineering
anotak- Reverse engineering
Eternal Yoshi- Reverse engineering
Archeroflegend- Framestepper
ermaccer- UMVC3Hook
Vevota- Playtesting
John2143- "Sponsorship"
Those random guys who played with me in a steam lobby one day
