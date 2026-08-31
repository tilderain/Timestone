# sGameConfig

**Ghidra type:** `sGameConfig`
**Known size:** `0x514B0`
**Base class:** `cSystem`
**Status:** Ghidra-verified native reference, 2026-08-31

## Role And Lifetime

`sGameConfig` is the game's large global configuration, profile, savedata, and
combat-parameter service. It is not the standalone `sBattleSetting` match
descriptor.

`sMvc3Manager__createGameConfig @ 0x140253580` allocates `0x514B0` bytes with
`0x10` alignment and calls `sGameConfig__ctor @ 0x14024ECE0`. The constructor
publishes the singleton at `0x140D50F88`; `sGameConfig__getInstance @
0x140001AB0` returns that pointer. The object derives from `cSystem`, and most
public accessors serialize through its critical section.

The vtable at `0x140B07E50` has ten entries:

| Slot | Address | Method |
|---:|---:|---|
| `0` | `0x14024F050` | deleting destructor |
| `1` | `0x14065EBE0` | inherited virtual |
| `2` | `0x14014D5D0` | inherited virtual |
| `3` | `0x14024FB20` | `createProperty` |
| `4` | `0x140251C60` | `getDTI` |
| `5` | `0x1402537B0` | `reset` |
| `6` | `0x140253490` | `move` |
| `7` | `0x1407E1A10` | shared no-op |
| `8` | `0x1407E1A10` | shared no-op |
| `9` | `0x140255260` | one-time battle/audio initialization |

## Confirmed Fields

Offsets are complete-object offsets. Broad ranges are shown where the native
type is established but its internal fields belong in a separate class
reference.

| Offset | Size | Type | Name | Access | Evidence |
|---:|---:|---|---|---|---|
| `0x000` | `0x38` | `cSystem` | base | inherited | Constructor calls `cSystem__ctor`; locked accessors use its critical section. |
| `0x038` | `4` | `uint32_t` | `mLanguage` | locked get/set | Reflection callback and platform-language initializer. Values are clamped to `0..5`. |
| `0x050` | `4` | `int32_t` | `Difficulte` | reflected | Direct reflection and defaults initializer. |
| `0x054` | `4` | `int32_t` | `RoundMax` | locked get/set | Reflection callbacks; setter clamps to `1..9`. |
| `0x058` | `4` | `int32_t` | `TimeLimit` | locked get/set | Reflection callbacks; setter clamps to `0..99`. |
| `0x05C` | `4` | `float` | `SeVol` | locked get/set | Applies sound groups 1, 3, 5, and 6. |
| `0x060` | `4` | `float` | `BgmVol` | locked get/set | Applies sound group 2. |
| `0x064` | `4` | `float` | `VoiceVol` | locked get/set | Applies sound group 4. |
| `0x068` | `4` | `float` | `VoiceChatVol` | locked get/set | Applies guide/voice-chat volume. |
| `0x06C` | `4` | `float` | `MasterVol` | locked get/set | Applies sound group 0. |
| `0x070` | `4` | `int32_t` | `SoundType` | reflected | Direct reflection. |
| `0x074` | `4` | `int32_t` | `SeSoundType` | reflected | Direct reflection. |
| `0x078` | `4` | `int32_t` | `BgmType` | reflected | Direct reflection. |
| `0x07C` | `4` | `int32_t` | `BroadcastMen` | locked get/set | Reflection callbacks. |
| `0x080` | `4` | `int32_t` | `BroadcastWomen` | locked get/set | Reflection callbacks. |
| `0x084` | `4` | `int32_t` | `ExCpuDelivery` | reflected | Direct reflection. |
| `0x08C` | `4` | `int32_t` | `ChrVoiceAll` | reflected/read | Selects default, all-Japanese, all-English, or custom mask. |
| `0x090` | `4` | `uint32_t` | `ChrVoiceAttr` | reflected/read | Per-character English-voice bit mask. |
| `0x0A0` | `0x10` | vector storage | `HPGaugeOffset` | reflected | Registered as `Vector3`; native storage occupies four floats. |
| `0x0B0` | `0x10` | vector storage | `HCGaugeOffset` | reflected | Registered as `Vector3`; native storage occupies four floats. |
| `0x0C0` | `0x58` | `uint32_t[2][11]` | primary custom gameplay bindings | locked | Set/reset functions use player stride `0x2C`. |
| `0x118` | `0x58` | `uint32_t[2][11]` | alternate custom gameplay bindings | locked | Selected by the alternate-bank Boolean. |
| `0x170` | `8` | `int32_t[2]` | binding mode | read by resolver | Chooses built-in table family. |
| `0x178` | `8` | `int32_t[2]` | binding layout index | get/set | Values `0..2` choose built-ins; values above 2 select inline custom tables. |
| `0x188` | `4` | `uint32_t` | `CtrlType` | reflected | Direct reflection. |
| `0x18C` | `4` | `uint32_t` | `Num` | reflected/read | Also returned by `getField_18C`. |
| `0x190` | `1` | `bool` | `Announced` | reflected | Direct reflection. |
| `0x191` | `1` | `bool` | `Fix` | reflected | Direct reflection. |
| `0x192` | `1` | `bool` | `AutoJump` | reflected/get/set | Read by launch and character-select code. |
| `0x1B8` | `4` | `uint32_t` | `ConfigAttr` | reflected/bit access | Direct reflection and bit-0 accessor. |
| `0x1BC` | `0x50` | `uint32_t[20]` | primary input mapping table | locked | Bulk set/reset bank 0. |
| `0x20C` | `0x50` | `uint32_t[20]` | alternate input mapping table | locked | Bulk set/reset bank 1. |
| `0x320` | `0x10` | `uint32_t *[2]` | active gameplay binding tables | read/write | Cached pointers returned by the table resolver. May point into this object or `.rdata`. |
| `0x338` | `0x100` | `char[256]` | platform user/name text | periodic update | Zeroed by constructor; refreshed by `move` every 60 ticks. |
| `0x438` | `4` | `uint32_t` | platform-text refresh counter | `move` | Triggers refresh after 59. |
| `0x43C` | `4` | `int32_t` | `Region` | reflected | Constructor clears it. |
| `0x440` | `4` | `int32_t` | `mCountry` | reflected/network | Constructor clears it. |
| `0x444` | `4` | `int32_t` | `mCountryArea` | reflected/network | Constructor initializes it to `-1`. |
| `0x448` | `4` | `int32_t` | network locality/slot field | network refresh | Constructor initializes it to `-1`; exact reflected meaning remains unresolved. |
| `0x450` | `1` | `bool` | `mIsNetBattleReq` | reflected | Registered under both old and `m`-prefixed labels. |
| `0x458` | `0x298` | `cPlayDataAccumulate` | `mPlayData` | inline object | Constructed/destructed in place; offline play accumulator getter. |
| `0x6F0` | `0x298` | `cPlayDataAccumulate` | `mPlayDataRank` | inline object | Constructed/destructed in place; online/ranked accumulator getter. |
| `0x988` | `0x4A0` | `cExtraCpu` | `mExCpu` | inline object | Constructor and direct getter. |
| `0xE28` | `0x148` | `cEventData` | `mEventData` | inline object | Constructor and direct getter. |
| `0xF70` | `0xEA0` | secret/unlock object | `mSecret` | inline object | Constructor, destructor, and direct getter. |
| `0x1E10` | `0x20` | `cClearFlag` | `mClearFlag` | inline object | Constructor and direct getter. |
| `0x1E30` | `0x48A70` | `cSPCData` | `mSPCData` | inline object | Constructor call and `getSPCData`; ends at `+0x4A8A0`. |
| `0x4A89C` | `10` | `uint8_t[10]` | projectile slot-active flags | combat read/write | Last ten bytes of `cSPCData`; currently captured for rollback. |
| `0x4A8A0` | variable | inline unlock/profile state | random character/unlock data | profile methods | Reset, bit-test, and completion-ratio routines. Exact end is unresolved. |
| `0x4ABBC` | `1` | `bool` | `mIsUseSPC` | reflected | Constructor clears it. |
| `0x4ABBD` | `1` | `bool` | `mbSPCReceive` | reflected/network | Constructor clears it. |
| `0x4ACC8` | `0x120` | selection records | live character-select presets | reset/read/write | Three `0x60` presets, each with two sides of three `0x10` records. Not the persisted License presets. |
| `0x4ADE8` | `4` | `int32_t` | `mChrSelSelectStgId` | reflected | Constructor clears; reset sets `0x13`. |
| `0x4ADEC` | `1` | `bool` | `mbExistSaveData` | reflected/get/set | Save/profile state. |
| `0x4ADED` | `1` | `bool` | `mbParentalLockWarning` | reflected/get/set | Save/profile state. |
| `0x4ADEE` | `1` | `bool` | `mIsReset` | reflected/vfn9 | Guards one-time battle initialization. |
| `0x4ADF0` | `8` | `MtObject *` | active net-message container | UI lifecycle | Validated/cancelled/killed by net-message and destructor paths. |
| `0x4AE00` | `0x2550` | saved payload | registered savedata buffer | load/apply | Registered with `uStoragePC`; ends immediately before storage flags. |
| `0x4D350` | `1` | `bool` | `mIsBusyStorage` | locked/load | Prevents concurrent native savedata loads. |
| `0x4D351` | `1` | `bool` | `mResurveSave` | locked/setters | Set by option and binding setters to request persistence. |
| `0x4D352` | `1` | `uint8_t` | unknown storage byte | reflected getter | Constructor clears it. |
| `0x4D353` | `1` | `bool` | `mSaveOk` | reflected | Constructor sets it. |
| `0x4D354` | `4` | `uint32_t` | `mSavePenalty` | reflected/locked | Constructor clears it. |
| `0x4D358` | `0x0C` | network-message state | net-message fields | locked | Cleared together by `clearNetMesFields`. |
| `0x4D364` | `0x10` | deferred unlock state | flags and active marker | locked | Cleared by `clearDeferredUnlockState_locked`; consumed by deferred award application. |
| `0x514A0` | `8` | `MtObject *` | PC text/UI helper unit | lifecycle | Initialized by `initPCTextBuffer`; killed by destructor. |

## Saved Payload Layout

`sGameConfig__startSavedataLoad @ 0x140253620` creates a `uStoragePC`, registers
exactly `0x2550` bytes at `+0x4AE00`, assigns completion callback
`sGameConfig__applyLoadedSavedata @ 0x140253370`, requests operation 6, and sets
`mIsBusyStorage`. The callback calls
`sGameConfig__applySavedPayloadToLiveSystems @ 0x14024F080`.

| Payload offset | Object offset | Meaning |
|---:|---:|---|
| `0x0000` | `0x4AE00` | payload header/state and reference bookkeeping |
| `0x0020` | `0x4AE20` | persisted `sLicense` profile, `0x690` bytes |
| `0x06B0` | `0x4B4B0` | mission common data |
| `0x09B0` | `0x4B7B0` | secret/profile status block |
| `0x183C` | `0x4C63C` | clear/config data begins |
| `0x1850` | `0x4C650` | additional profile block |
| `0x1BD4` | `0x4C9D4` | additional profile block |
| `0x1CFC` | `0x4CAFC` | matrix/profile block |
| `0x1DA0` | `0x4CBA0` | additional profile block |
| `0x20BC` | `0x4CEBC` | Earth Attack result data |
| `0x2444` | `0x4D244` | trailing License scalar |
| `0x2448` | `0x4D248` | trailing License scalar |
| `0x244C` | `0x4D24C` | trailing profile scalar |

The live presets at `+0x4ACC8` are outside this payload. The three persisted
rosters are inside the copied `sLicense` profile at payload-relative offsets
`+0xD8`, `+0xFC`, `+0x120`, and `+0x144` for character, assist, color, and use
count tables respectively.

## cSPCData Boundary

`sGameConfig__getSPCData @ 0x1400049F0` returns `this + 0x1E30`. Older Ghidra
typing and names called this `getBattleSetting`; that is incorrect. The actual
standalone `sBattleSetting` is a separate `0x930`-byte singleton at
`0x140D50E58`.

`cSPCData` contains immutable card/combat parameters and mutable Heroes and
Heralds card runtime. Its important high-level ranges are:

| `cSPCData` range | `sGameConfig` range | Role |
|---:|---:|---|
| `+0x00008..+0x3BBFF` | `+0x1E38..+0x3DA2F` | mostly static card/combat parameter database |
| `+0x3BC00..+0x3C027` | `+0x3DA30..+0x3DE57` | mutable counters, flags, triggers, and per-team/card values |
| `+0x3C028..+0x404E7` | `+0x3DE58..+0x42317` | additional inline card runtime and condition records |
| `+0x404E8..+0x48A6F` | `+0x42318..+0x4A89F` | runtime/UI tail, `UseSPC`, and projectile flags |

The current rollback snapshot captures only the ten projectile flags at
`sGameConfig+0x4A89C`. Card-enabled rollback needs a separately audited mutable
`cSPCData` region; copying all of `sGameConfig` would incorrectly rewind locks,
save state, UI ownership, and process-local pointers.

## Relevant Methods

| Address | Name | Relationship |
|---:|---|---|
| `0x140001AB0` | `sGameConfig__getInstance` | Returns singleton pointer. |
| `0x140004840` | `sGameConfig__getLanguage_locked` | Reads global UI/audio language. |
| `0x1400049F0` | `sGameConfig__getSPCData` | Returns embedded `cSPCData`. |
| `0x14024ECE0` | `sGameConfig__ctor` | Constructs all inline objects and publishes singleton. |
| `0x14024EE80` | `sGameConfig__dtor_1` | Kills helper units and destructs inline objects in reverse order. |
| `0x14024FB20` | `sGameConfig__createProperty` | Registers reflected fields and callbacks. |
| `0x140251B80` | `sGameConfig__getCharacterVoiceLanguageMask_locked` | Resolves default/all/custom character voice mask. |
| `0x140251BD0` | `sGameConfig__getCharacterVoiceLanguage` | Returns effective voice language for a character ID. |
| `0x1402522A0` | `sGameConfig__getButtonMappingValue` | Reads an active 11-entry gameplay binding table. |
| `0x140252770` | `sGameConfig__resetCharacterSelectionPresets` | Resets the live `+0x4ACC8` preset block. |
| `0x1402529E0` | `sGameConfig__initDefaultsBlob_529E0` | Initializes settings, bindings, and default option values. |
| `0x140253370` | `sGameConfig__applyLoadedSavedata` | Native storage completion callback. |
| `0x140253490` | `sGameConfig__move` | Maintains object references and periodically refreshes platform text. |
| `0x140253620` | `sGameConfig__startSavedataLoad` | Starts native asynchronous save loading. |
| `0x1402537B0` | `sGameConfig__reset` | Resets options, profile state, and live presets. |
| `0x140254640` | `sGameConfig__setInputMappingTable` | Writes one 20-entry input mapping bank. |
| `0x1402547F0` | `sGameConfig__resetInputMappingTable` | Restores one 20-entry bank to defaults. |
| `0x140254A20` | `sGameConfig__setPlayerBindingTable` | Writes one player's 11-entry custom gameplay binding bank. |
| `0x140254B10` | `sGameConfig__resetPlayerBindingTable` | Restores one player's custom gameplay bindings. |
| `0x140254C40` | `sGameConfig__selectPlayerBindingTable` | Selects and caches a player's active binding table. |
| `0x140255040` | `sGameConfig__setSeVolume` | Applies and persists effect volume. |
| `0x140255180` | `sGameConfig__setVoiceChatVolume` | Applies and persists guide/voice-chat volume. |
| `0x1402551F0` | `sGameConfig__setVoiceVolume` | Applies and persists character voice volume. |

## Candidate Fields

| Offset | Size | Proposed type/name | Confidence | Evidence needed |
|---:|---:|---|---|---|
| `0x03C..0x04F` | `0x14` | network request/return state | Medium | Trace every setter and native network consumer. |
| `0x260..0x31F` | `0xC0` | profile/input selector state | Medium | Correlate reflection labels with exact LEA offsets. |
| `0x448` | `4` | platform/network area or slot | Medium | Identify writer in `refreshNetworkSlotState` and downstream protocol field. |
| `0x4A8A0..0x4ABC0` | `0x320` | unlock/challenge state | Medium | Separate random unlock, challenge flags, and static profile data. |
| `0x4D358..0x4D363` | `0x0C` | network-message state | Medium | Resolve each field through message creation and countdown paths. |
| `0x4D374..0x5149F` | `0x412C` | net-message/UI text runtime | Low | Audit ownership, allocation, and all writers before typing or snapshotting. |

## Ghidra Corrections Applied

- Corrected `getSPCData` documentation so it no longer claims to return
  `sBattleSetting`.
- Corrected option callback names for language, round/time limits, all five
  audio volumes, and the save-request flag at `+0x4D351`.
- Named the binding-table resolver and selector and documented both table
  banks, selector fields, and cached pointers.
- Named direct embedded-object accessors for `cEventData`, secret data,
  `cClearFlag`, and `cExtraCpu`.
- Removed false `sGameConfig` ownership from unrelated
  `uMenuSPCGallery`, `cParticleNode`, and `uGeometry2Group` constructors and
  destructors caused by prior parameter-type pollution.

## Open Questions

- The reflected repeated `Type`, `Body`, and `Assist` properties need exact
  record offsets reconstructed from property-registration instructions rather
  than the decompiler's collapsed field names.
- The complete `+0x260..+0x31F` selector/configuration layout is not yet named.
- The mutable versus immutable boundary in the final `cSPCData` tail still
  needs a complete write audit before Heroes and Heralds rollback can snapshot
  it safely.
- The large runtime tail after the saved payload contains UI/network ownership
  and must not be flat-copied until all pointers and helper units are typed.
