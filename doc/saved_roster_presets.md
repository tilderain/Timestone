# Saved Roster Presets

**Status: Native reference. Investigated 2026-08-30.**

## Goal

Expose the retail game's three saved preset teams in Timestone's launch-menu
character selector without constructing `sGameConfig` early or calling the
game's savedata functions.

## Conclusion

This is feasible by reading `savedata.bin` through Steam Remote Storage,
decrypting and validating the save locally, and extracting four fixed tables
from the saved `sLicense` profile. This is preferable to reading the live game
singleton because Timestone opens its launch menu before `sGameConfig` and the
normal game boot sequence are initialized.

The game has two similarly named but distinct roster stores:

- The persisted `sLicense` profile contains three user preset rosters. These
  are the records wanted by the launch menu.
- `sGameConfig+0x4ACC8` is a separate live character-select working block,
  reset by `sGameConfig__resetCharacterSelectionPresets @ 0x140252770`. It is
  not inside the persisted `sGameConfig+0x4AE00` payload and should not be used
  as the on-disk format.

## Native Load Path

`sGameConfig__startSavedataLoad @ 0x140253620` registers exactly `0x2550` bytes
at `sGameConfig+0x4AE00` and starts an asynchronous load. Its completion
callback is `sGameConfig__applyLoadedSavedata @ 0x140253370`, which calls the
payload application routine at `0x14024F080`.

The application routine copies `0x690` bytes from
`sGameConfig+0x4AE20` into the live `sLicense` profile. Therefore:

```text
saved payload +0x0000  sGameConfig saved-payload header/state
saved payload +0x0020  persisted sLicense profile (0x690 bytes)
```

The PC storage backend configures:

```text
filename: savedata.bin
cipher key: umvc3
Steam interface: STEAMREMOTESTORAGE_INTERFACE_VERSION013
```

Relevant functions:

| Address | Name | Purpose |
|---:|---|---|
| `0x1402B25B0` | `uStoragePC__setup` | Selects `savedata.bin` and key `umvc3`. |
| `0x14075BE20` | `sSavedata__loadFromSteamRemoteStorage` | Reads, decrypts, verifies, and parses the save. |
| `0x14075C1E0` | `sSavedata__saveToSteamRemoteStorage` | Builds, hashes, encrypts, and writes the save. |
| `0x14075C4A0` | `sSavedata__parsePlaintext` | Validates the plaintext header and dispatches records. |
| `0x14075C560` | `sSavedata__readRegisteredValue` | Reads one typed registered record. |
| `0x1404E0810` | `MtCipherStream__ctorWithKey` | Initializes the Blowfish stream and key schedule. |
| `0x1404FD240` | `MtCrypto__blowfishDecryptBuffer` | Decrypts independent 8-byte blocks. |
| `0x1404FD770` | `MtCrypto__blowfishEncryptBuffer` | Encrypts independent 8-byte blocks. |
| `0x1404FD840` | `MtCrypto__sha1` | Computes standard SHA-1 integrity data. |

## Preset Layout

`sLicense__cmdList_initSelectionCursor @ 0x14027D430` initializes all three
teams. `sLicense__copyProfileStruct524 @ 0x14021B510` copies the same fields as
part of the serializable profile.

Offsets below are relative to the start of the `0x690`-byte profile:

| Offset | Type | Meaning |
|---:|---|---|
| `0x0B8` | `int32_t chars[3][3]` | Three teams of three game character IDs. |
| `0x0DC` | `int32_t assists[3][3]` | Corresponding assist indices. |
| `0x100` | `int32_t colors[3][3]` | Corresponding zero-based color indices. |
| `0x124` | `int32_t useCount[3][3]` | Usage counters associated with those roster entries. |

The element address for each table is:

```cpp
tableBase + (presetIndex * 3 + slotIndex) * sizeof(int32_t)
```

The default character teams initialized for a new profile are:

```text
preset 0: 27, 33, 30
preset 1:  2,  4,  7
preset 2: 45, 21, 47
```

The assist, color, and usage tables initially contain zeroes. Character IDs
are already the numeric IDs used by `LaunchDialogConfig`; assists are expected
to be `0..2`, and colors are zero-based like Timestone's current selectors.

Relative to the registered `0x2550` saved payload, add `0x20` to the profile
offsets. For example, the character table begins at payload `+0xD8`.

## Save Container

The Steam file is not the raw `0x2550` payload.

1. The complete file is Blowfish-encrypted in independent 8-byte blocks using
   ASCII key `umvc3`. Ghidra's key setup matches standard Blowfish, including
   big-endian key-word assembly.
2. The decrypted stream starts with a `0x40`-byte header.
3. Header `+0x00` is format version `1`.
4. Header `+0x0C` contains a 20-byte SHA-1 digest, stored as five little-endian
   `uint32_t` words rather than canonical SHA-1 byte order.
5. Header `+0x20` contains the number of plaintext bytes hashed beginning at
   plaintext `+0x40`.
6. Header `+0x08` contains the byte size of an intervening table. The parser
   skips this table and processes `value / 8` typed records.
7. Each record begins with a `0x10`-byte descriptor. Type `0x0C` is a byte
   buffer; its 64-bit value contains the buffer length, followed immediately
   by that many payload bytes.

The extractor should locate a type-`0x0C`, length-`0x2550` record rather than
hard-coding a whole-file payload offset. This tolerates table-size and record
ordering differences while retaining strict bounds checks.

## Recommended Implementation

1. After `SteamLobby_Initialize`, acquire
   `STEAMREMOTESTORAGE_INTERFACE_VERSION013` and synchronously read
   `savedata.bin`. The launch menu already runs outside the frame loop, so the
   small synchronous read is acceptable.
2. Decrypt into a temporary buffer with a small local Blowfish implementation.
   Do not call `MtCipherStream` or any game function.
3. Validate version, all lengths, the SHA-1 digest, record type, and exact
   `0x2550` payload size before reading roster fields.
4. Extract the three roster tables into a plugin-owned value type. Never expose
   pointers into the temporary save buffer.
5. Reject an individual preset if any character is outside Timestone's valid
   character range. Clamp or reject invalid assists and colors using the same
   character-specific limits already applied by `LoadRollbackSettings`.
6. Add a `Preset Team` combo near each launch-menu team. Selecting preset 1-3
   should update the three character, color, and assist combos through the
   existing population/update helpers.
7. Treat missing Steam Remote Storage, a missing save, failed integrity, or an
   unsupported format as "no retail presets available." Keep the current INI
   roster untouched and do not prevent launch.

## Why Not Other Approaches

- Calling `sGameConfig__startSavedataLoad` is unsafe before normal manager
  construction and would require pumping game units from the launch thread.
- Reading `sGameConfig+0x4ACC8` cannot work at launch-menu time and targets a
  runtime working block rather than the persisted profile.
- Searching Steam `userdata` or Wine prefixes directly is brittle and violates
  Steam Cloud's authority. `ISteamRemoteStorage::FileRead` works for native
  Windows, Proton/Wine, and cloud-synchronized saves without path discovery.
- Importing only character IDs would discard the saved assist and color fields;
  all three parallel tables should be applied together.

## Remaining Verification

Before shipping, test the decoder against at least two real `savedata.bin`
files and compare all three extracted teams with the retail License/Preset Team
screen. This confirms byte order and determines whether zero usage entries
should be hidden or merely shown as valid default presets.
