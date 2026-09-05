# INI persistence

Timestone's INI writes use `UMVC3Hook/IniSafe.cpp`, including legacy
`CIniReader` writes and the launch menu's roster/training/network writes.
The game's own display `config.ini` still uses its existing Win32 writer.

## Behavior

- Reads retain their existing Win32 behavior. A malformed file is not quarantined,
  reset, or automatically replaced with a backup.
- Saves read the actual file bytes, not the profile API's cached settings tree.
  Only the requested key is edited (or inserted). Comments, unknown settings,
  malformed lines, and unrelated bytes are preserved. ANSI, UTF-8 BOM, and
  UTF-16 LE/BE BOM files are supported, along with CRLF, LF, and CR line endings.
- All cooperating writers share a named mutex across game processes. The file
  is reread after acquiring it, so concurrent saves merge the latest contents.
- The new contents are written to a unique temporary file in the same directory,
  flushed, read back, and compared before a same-filesystem replacement. The live
  file is never opened with a truncating disposition or deleted as a fallback.
- Before replacement, the previous nonempty contents are similarly staged and
  saved as `<filename>.bak` (for example, `Timestone.ini.bak`). This is the previous
  **save transaction** (one key or a batch), not a validated or complete-session snapshot. Unchanged
  saves and empty inputs do not overwrite the backup.
- An actual read error, short write, failed flush, verification mismatch, backup
  failure, or failed replacement aborts the save. Later attempts retry normally;
  there is no persistent failure flag. Errors are logged as `IniSafe:` entries.
- A missing file may be created, and an existing empty or malformed file can still
  receive settings. A 4 MiB size limit bounds memory usage; exceeding it fails the
  save without changing the original.

## Separate advanced and personal roster settings

All settings use `Timestone.ini`. There is no legacy migration or fallback;
`rollbacksetting.ini` is not read, written, or deleted.

- `[Roster.P1]` and `[Roster.P2]` (Slot1–3, Color1–3, Assist1–3, SimpleMode),
  together with `[Training] StageId`, are for advanced/offline/dual-training setup.
- `[ConnectRoster]` holds your single personal roster: Slot1–3, Color1–3,
  Assist1–3, SimpleMode, and StageId. Team defaults come from the built-in defaults,
  never from the advanced P1/P2 sections. All 11 keys save in one safe transaction.
  A missing personal StageId defaults to Bonne Wonderland (105), including when
  the entire ConnectRoster section is absent. An explicitly saved Training Room
  (0) is preserved. Invalid stage IDs fall back to Bonne in the launch menu.
- `[Network] HostIp/HostPort` remains shared network configuration.

Steam and direct-IP connections place your personal roster on P1 when hosting
and P2 when joining. Local edits update only your personal team. Received team
and launch packets modify match state only, never saved preferences. The host
still selects the actual match stage; joining does not overwrite your preferred
stage with the host's choice. An explicit local host-side stage edit updates your
StageId preference, as does an explicit local stage edit in the advanced menu.
Hosting from the advanced menu always adopts its selected stage, including a
selection loaded from `[Training] StageId`. Joining does not perform this adoption
and received host stages never overwrite the client's `[ConnectRoster] StageId`.
Leaving an online setup restores the advanced menu's own
rosters rather than exposing the previous peer's roster as an advanced preference.

Online setup is initialized as soon as Steam lobby ownership is known, and before
the compact menu's battle setup controls are created. Receiving transport handles
for the same role does not reload preferences. Explicit Simple Mode edits made
in the advanced menu before choosing a connection role are retained for the
local side. Untouched advanced/offline team preferences are still not migrated.

In-match online roster commits similarly save only the local side, including its
simple-mode setting, without changing the preferred stage. Advanced/offline and
dual-training roster commits keep their separate two-team configuration.

## Limits

The mutex coordinates the new writer, not older plugin versions or arbitrary
editors. In-place external writes are excluded during staging, but an external
rename or a write racing the final replacement can still lose an edit. A process
crash during staging may leave a `tsi*.tmp` file, but does not truncate the INI.
Disk hardware/filesystem corruption and power-loss durability are not absolute
guarantees of this scheme. It does not reconstruct settings already lost before
these protections were installed.

## Integration tests

Build the standalone test on Linux:

```sh
x86_64-w64-mingw32-g++ -std=c++20 -Wall -Wextra -static \
  tools/test_ini_safe.cpp UMVC3Hook/IniSafe.cpp UMVC3Hook/code/connect_roster.cpp \
  -o /tmp/opencode/test_ini_safe.exe
```

Run on Windows, or under a dedicated Wine prefix. Tests only modify generated
files in the Windows temporary directory, not the game's INI. They cover byte
preservation, malformed inputs, encodings, line endings, failed reads/replacements/
backups, retries, empty/missing files, profile API visibility, and multiprocess
saves, batch atomicity, and personal-roster isolation/side selection/stage preservation.

### Launch-menu regression checks (two game instances)

- In the advanced menu, select Bonne before creating a Steam lobby, then join
  from the other instance. Repeat with a stage edit after lobby creation but
  before the peer joins. The host dropdown, client dropdown, and loaded stage
  must all stay on Bonne. Repeat using direct-IP Host Game.
- Restart with `[Training] StageId=105` and `[ConnectRoster] StageId=0` on the
  host. Host from the advanced menu without touching the stage dropdown: the
  match and host's saved ConnectRoster stage must become Bonne (105). Give the
  joining client a different saved ConnectRoster stage and verify it is unchanged
  after connection, launch, and disconnect.
- With an existing personal stage preference, use the compact lobby browser.
  Battle Setup must display that preference immediately for the host and keep
  it when transport connects. The client must adopt the host's match stage
  without saving it as its own preferred stage.
- Set the two players to opposite Simple Mode values, ready, and launch. Repeat
  with the values reversed, including changing them before connecting in the
  advanced menu (P1 for host, P2 for client). Check both players' actual controls;
  the host log records the received P2 Simple Mode value.
- Leave/recreate the lobby, swap host/client roles, and repeat. Each player must
  retain only their personal control-scheme preference; leaving online setup
  must restore the advanced menu's separate settings.
