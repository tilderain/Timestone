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
  Assist1–3, SimpleMode, and StageId. Defaults come from the built-in defaults,
  never from the advanced P1/P2 sections. All 11 keys save in one safe transaction.
- `[Network] HostIp/HostPort` remains shared network configuration.

Steam and direct-IP connections place your personal roster on P1 when hosting
and P2 when joining. Local edits update only your personal team. Received team
and launch packets modify match state only, never saved preferences. The host
still selects the actual match stage; joining does not overwrite your preferred
stage with the host's choice. An explicit local host-side stage edit updates your
StageId preference. Leaving an online setup restores the advanced menu's own
rosters rather than exposing the previous peer's roster as an advanced preference.

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
