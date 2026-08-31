# Native Per-Character Voice Language Synchronization

**Status:** Ghidra-verified and implemented, 2026-08-31. Addresses refer to
`ichneumon_sub/umvc3.exe.unpacked.exe` at preferred image base
`0x140000000`.

## Conclusion

Retail online synchronizes character voice language independently for every
team member. The finalized character-select message contains one voice byte,
and the receiver applies it to the matching team/member slot. This directly
matches the desired ownership model:

```text
host controls team 0's three character voice settings
client controls team 1's three character voice settings
```

The custom protocol should reproduce this with `uint8_t voice[2][3]` carried
alongside character, color, and assist selection. It should not synchronize the
whole local `sGameConfig`, because that would let one peer overwrite the other
peer's preferences.

## Voice Preference Source

The retail settings are stored in `sGameConfig` as:

```text
ChrVoiceAll
ChrVoiceAttr
```

`sGameConfig__getCharacterVoiceLanguageMask_locked @ 0x140251B80` resolves
them as follows:

| `ChrVoiceAll` | Effective mask |
|---:|---:|
| `0` | Retail default `0x025807F0` |
| `1` | `0x00000000` |
| `2` | `0xFFFFFFFF` |
| other | `ChrVoiceAttr` |

`sGameConfig__getCharacterVoiceLanguage @ 0x140251BD0` returns the bit at the
character ID for Capcom IDs `1..25`. Character IDs above 25 return `1`

The observed values are:

```text
0 = Japanese character voice
1 = English character voice
```

This setting is distinct from `sGameConfig::mLanguage`, read by
`sGameConfig__getLanguage_locked @ 0x140004840`. `mLanguage` controls general
language behavior and a small teammate-call archive remap; it is not the
per-character voice preference.

## Runtime Storage

Each of the six `sBattleSetting::cMainChr` records has reflected field
`mVoice` at slot offset `+0x44`:

```cpp
slot = battleSetting + 0x38 + slotIndex * 0x58;
voiceLanguage = *(uint32_t *)(slot + 0x44);
```

Relevant accessors are:

| Address | Ghidra name | Purpose |
|---:|---|---|
| `0x1401463D0` | `sBattleSetting__setCharSlotVoiceLanguage_locked` | Writes `cMainChr[slot].mVoice`. |
| `0x14001E9C0` | `sBattleSetting__getCharSlotVoiceLanguage_locked` | Reads `cMainChr[slot].mVoice`. |

The getter feeds the fourth category of
`sCharacter__requestCharacterArchiveSet @ 0x140059600`. Internally that request
uses the value as the category-4 archive key. The same slot value is also passed
to character construction by
`sBattleSetting__spawnConfiguredCharactersAndAudio @ 0x14024B7E0`.

Therefore this is not presentation metadata that can be corrected after load.
It must be final before character voice archives are requested and characters
are constructed.

## Retail Network Flow

The finalized selection packet is `cNetChrSel::MsgChrType`, constructed at
`0x140261970`. Its established fields are:

| Message offset | Meaning |
|---:|---|
| `+0x10` | Team |
| `+0x14` | Member index |
| `+0x18` | Character ID |
| `+0x1C` | Color/draw mode |
| `+0x20` | Character voice language |
| `+0x24` | Simple mode |
| `+0x25` | AutoJump |
| `+0x26` | Unresolved UI-only slot flag |
| `+0x28` | Replay/profile field |

The outbound publisher at `0x1402637B0` reads the voice value through:

```text
sBattleSetting__getCharVoiceLanguageByTeamMember_locked @ 0x14024CEC0
```

It passes that value to `cNetChrSel__MsgChrType__ctor`, which stores it at
message `+0x20`.

The inbound dispatcher sends `MsgChrType` to
`cNetChrSel__applyCharacterSelectionMessage @ 0x140262E00`. That handler writes
message `+0x20` through:

```text
sBattleSetting__setCharVoiceLanguageByTeamMember_locked @ 0x140264CB0
```

The `+0x26` field is separately copied to
`sGameUI + 0x593C + team*3 + member`. It is not `mVoice` and has no observed
effect on the battle slot's voice archive.

Retail also sends a final `cNetChrSel::MsgBattleData` snapshot. Its six slot
records begin at message `+0x10`, with four bytes per slot:

```text
byte 0: character ID
byte 1: color/draw mode
byte 2: assist
byte 3: character voice language
```

`cNetChrSel__MsgBattleData_setSlot @ 0x140264BD0` packs each tuple. The outbound
path reads byte 3 from `sBattleSetting::mVoice`, and
`cNetChrSel__applyBattleDataMessage @ 0x140262B50` writes byte 3 back through
`sBattleSetting__setCharVoiceLanguageByTeamMember_locked` for all six slots.
This final snapshot provides a second authoritative synchronization before
battle startup.

The normal local character-select path resolves the selected character's local
voice preference and stores it in `sBattleSetting::mVoice` before publishing.
Thus each player originates settings only for the team they select, while both
processes finish with the same six slot values.

## Custom-Netplay Design

Add one byte per slot to every roster representation that can construct or
restart a match:

```cpp
uint8_t voiceLanguage[2][3];
```

Apply these rules:

1. Resolve the local owned team's three values with
   `sGameConfig__getCharacterVoiceLanguage(config, characterId)` after the
   team's character IDs are known.
2. Host publishes only team 0 values; client publishes only team 1 values.
3. On receipt, copy only the remote-owned team's values, like the existing
   character/color/assist and AutoJump ownership logic.
4. Validate values as `0..1`. Normalize every character ID above 25 to `1`.
5. Write `voiceLanguage` to backup slot `+0x44` in
   `TrainingBoot::WriteSlotEntry` before `restoreBackupAndInit` copies the six
   records to the live array.
6. Pass the same value as `soundVariant` to
   `sCharacter__requestCharacterArchiveSet` and
   `sCharacter__areArchiveSetsReady`.
7. Preserve the six bytes through synchronized in-match roster restarts.

For the current implementation this means extending at least:

```text
TrainingBootConfig
LaunchP1SelectionPacket / LaunchP2SelectionPacket or LaunchTrainingPacket
ChrSelImGui::RosterConfig / ChrSelPacket
```

The existing initial AutoJump exchange can carry the launch-time owned-team
voice values, but all voice values must arrive before `WriteSlotEntry` and the
first archive request. The in-match `ChrSelPacket` already has ample room under
its 256-byte limit.

## Custom Implementation

The custom protocol now carries `voiceLanguage[2][3]` through launch and
in-match roster configuration. After native savedata has loaded, the launch
handshake resolves the local preference for the owned team and exchanges it
alongside AutoJump. Host supplies team 0 and client supplies team 1.

`TrainingBoot::WriteSlotEntry` writes the synchronized value to backup-slot
`mVoice` before `restoreBackupAndInit`. In-match roster restarts preserve those
six values, and archive requests and readiness diagnostics use the matching
per-slot category-4 key instead of a hard-coded English archive.

## Teammate Callout Caveat

`sCharacter__remapTeamSoundForLanguage @ 0x140059210` remaps teammate character
IDs `3`, `16`, `23`, and `24` to alternate IDs `52..55` when the separate global
`mLanguage` value is nonzero. Those values select the optional `mCCVoice1` and
`mCCVoice2` archives used for teammate callouts.

Retail's reviewed load paths derive this remap from each process's local global
language, not from synchronized slot `mVoice`. This can make callout audio
locale differ between peers, but it is presentation-only and does not alter the
authoritative per-character voice language protocol described above. Do not
replace this remap with a team voice byte without separate runtime testing.
