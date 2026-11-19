# Sound And BGM Under Rollback (Research)

Ghidra **8192** `umvc3.exe.unpacked.exe` (static decompilation, 2026-11-14). This was a
Ghidra-only pass: no plugin source was read, so every statement about current
plugin behavior is phrased as something to verify. No runtime trace was taken.
Comments and renames were added in Ghidra 8192 at the addresses cited.

## 1. Native BGM Pipeline (Verified Statically)

### Round-Start Theme

`sMain__playInitialModeSound @ 0x14000B570` is called with `sAction` as `this`.
Callers:

- `sAction__executeMatchPhase` (phase 2)
- `enterFightingPhaseSetup`
- `stepCountdownShort`
- `advanceBattleIntroAndHandleSkip`

It selects the cue as follows:

- Game mode 6 → `0x46`; mode 2 → `0x70`.
- With game flag `0x10000`, the cue is `0x52` or `0x51` depending on
  `sGameConfig+0x78` (`BgmType`).
- `BgmType` 1 (stage theme) → stage vfunc `+0x238`.
- `BgmType` 0 (character theme) → theme vfunc `+0x2E0` of team 1's (P2's) active
  character.

The selected cue is written to `sAction+0x200`, and the voice handle (cRef) to
`sAction+0x1F0`.

**`BgmType` is a LOCAL option.** Two peers can legitimately hear different music.
That is not a simulation desync.

### Mid-Round Theme Changes

`cChrTeam__commitPendingCharacterSwap @ 0x140021550` runs for KO replacements
(swap type 5, or type 6 with defeat) while the `sAction` step is below 6. It calls
the new point character's `+0x2E0` theme cue through one of two functions:

- `sAction__playAnnouncerEvent @ 0x140003D60`. The name is misleading: this is the
  theme request.
- `sAction__updateUiBitfieldAndPlaySoundOnChange @ 0x140003EA0`, used when Phoenix
  Wright (charId `0x17`) is involved.

### Theme State Block In `sAction`

| Offset | Meaning |
|---|---|
| `+0x1F0` | BGM cRef |
| `+0x200` | Current theme cue, used for deduplication |
| `+0x208` | Stamp of the last change, taken from the `sMain+0x38` frame counter |
| `+0x210` | Initial-mode-sound-pending byte |
| `+0x214` | Per-team override bits: Wright Turnabout, which switches to cue `0x79` |

Both request functions apply two guards before doing anything:

- A 100-tick throttle: `(sMain+0x38 − sAction+0x208) < 0x65`.
- Deduplication against `+0x200`.

They **return before writing `+0x200`**. So the theme ID depends on the history
stored in `+0x208`.

### Playback

`sGSound__playSystemCue @ 0x1402AC5C0` plays the cue:

1. Stops cues `0x47..0x5A` and `0xD`/`0xE`.
2. Creates an ordinary voice.
3. Pushes the stream channels through `sSound__playSoundWithDb @ 0x1405D28B0`.
4. Remembers the current BGM cRef at `sGSound+0x668`.

**Stream chokepoint.** Every stream command goes through
`sSound__setStreamEntry @ 0x1405D2410`: play, stop, pause, resume, fades,
volume/pan/pitch, and the `requestStream_*` functions. It appends a `0x78`-byte
entry to the ring at `sSound+0x1CEF8` (count `+0x1CFE8`, capacity `+0x1CF64`). The
sound thread drains the ring through `sSound__extract*Stream`.

## 2. Why BGM Goes Out Of Sync

The verified part is which offsets are missing from the restore. The consequences
below are inferred from that.

1. **The theme block is not rewound.** The `sAction` restore ranges (as listed in
   `current.md`/restore) are `{28,E8}`, `{140,8}`, `{150,8}`, `{170,1}` and
   `{218,4}`. None of them covers `+0x1F0..+0x217`. Consequences:
   - When a predicted KO swap gets rolled back, `+0x200` and the playing theme
     keep the undone character's theme. Nothing ever undoes them.
   - A swap that happens only in the corrected timeline is requested during
     resim. Its outcome depends on the stale `+0x208` and `+0x200`, so it can be
     throttled or deduplicated differently than on the peer that saw it live.
   - The result is a different `+0x200` on each peer, and therefore a different
     theme.
2. **Audio suppression can leave BGM silent, depending on where the hook sits.**
   `playAnnouncerEvent` writes `+0x200` and then calls `playSystemCue`, which
   *stops* the other themes and then creates the new voice. Suppose a resim guard
   drops only the creation or the `playSoundWithDb` push. Then the old theme is
   stopped, the new one never starts, and the deduplication on `+0x200` blocks
   any later retry. BGM stays silent or wrong for the rest of the round. Verify
   where the plugin's guards on `0x1402A21F0`, `0x1402A3380` and `0x1405D28B0`
   sit relative to the stops.
3. **Wright's override bits (`+0x214`) are not rewound either.** The Turnabout
   theme (`0x79`) can be left stuck on or off after a rollback.

## 3. Sound Effects And Voices That Affect Gameplay (RNG)

Two sound functions consume the **global** `MtXorShift`:

| Function | When it consumes RNG | Called from |
|---|---|---|
| `sGSound__selectRandomVoiceVariant @ 0x1402A9110` | One or two `next()` calls whenever the voice bank resolves (`findVoiceByRef` returns non-null). | `playOrdinaryVoice 0x1402AAC40`, `playPersistentVoice 0x1402AAD60`, `playSoundAtPosition 0x1402AAB10`. These are used by intros, character voices and swap voices. |
| `sGSound__queueGroup5Voice @ 0x1402AE190` (announcer) | When the stream-request entry has a random range. | Gameplay code, about 20 call sites: team-switch transition, throw resolver, uCharacter states. |

`queueGroup5Voice` returns early, *before* consuming RNG, in these cases:

- The **local** option `sGameConfig+0x80` (`BroadcastWomen`) is set.
- Game mode is 5 or 6.
- `sAction` flag `0x10000` is set.

Hazards:

- **Peers with different announcer settings**, or different voice-bank load
  timing, consume different global RNG and desync gameplay.
- **Resim suppression placed at or above these functions** makes the replayed
  timeline consume less RNG than the live one did.

Any suppression must sit *downstream*: in the `createXxxVoiceRequest` functions
(`0x1402A28D0`, `0x1402A2A10`, `0x1402A2770`, `0x1402A21F0`) or in `sSound`.
Alternatively, isolate these RNG calls: run them against a private RNG, or save
and restore the global RNG around them.

The existing Ghidra comment on `0x1402B0E00` mentions "audio RNG isolation". Verify
that the isolation covers both functions, and also the second `next()` call in the
variant-selection path (`% 0x78`).

**Character voice queue during resim.** `uGSoundProcess` is spawned in sUnit
**category 7** (`0x1402AB800`), which headless resim includes. Its moveAfter
(`0x1402B17C0`) runs `sGSound__processCharacterSoundQueues @ 0x1402B0E00` on every
replayed frame. That turns the queue at `sGSound+0x1FE8` (20 entries) and the slots
at `+0xA38` (6 slots, stride `0x438`) into persistent voice requests. Those queues
overwrite their slots rather than append. Without downstream suppression they will
re-issue voices during resim.

## 4. Proposed Design And Hook Points

### A. BGM Follows The Corrected Simulation State

1. Rewind `sAction` `{0x200,4}`, `{0x208,8}`, `{0x210,1}` and `{0x214,4}`. These are
   deterministic simulation outputs. Keep the cRef at `+0x1F0` live.
2. During resim, replace `0x140003D60` and `0x140003EA0` with state-only
   reimplementations: the same mode, flag, throttle and deduplication logic, and
   the same writes to `+0x200`, `+0x208` and `+0x214`, but no `sGSound` calls. Also
   avoid calling `playInitialModeSound`'s cue path during resim.
3. After the last resimulated frame, compute the desired cue: `0x79` if `+0x214`
   is non-zero, otherwise `+0x200`. If it differs from the cue the plugin last
   played live, call `sGSound__playSystemCue(desired, 0, 1, 0)` once and store the
   result in `sAction+0x1F0`. This is the native path, and it stops the other
   themes itself. Live frames keep native behavior.

### B. Sound-Effect Deduplication And Mispredicted Sounds (GGPO-Style)

**Ledger.** Record every live voice creation at the request layer
(`0x1402A28D0`, `0x1402A2A10`, `0x1402A2770`, `0x1402A21F0`, which all return a
cRef/cookie). Key each entry by `{frame, owner (param or VoiceRequest owner),
bank/cue key, variant}`, and keep the native cookie.

**During resim**, keep RNG consumption and all simulation-side effects:

- A request that matches a ledger entry for the same frame (and the frame is
  inside the rollback window) reuses that cookie instead of creating a new voice.
  Stored cRefs stay valid that way.
- A request with no match is new. Create it, but skip it if its frame is more than
  a few frames old and the sound is short (one-shot SE or voice older than about
  6 frames).
- Defer stop and modify calls in the window: `stopVoiceByRef 0x1402A9F10`,
  `stopVoiceByCookie 0x1402A1930`, `findVoiceAndCallStop 0x1402A0FC0`,
  `stopCharacterVoices 0x1402AC270`, `setVoicePitchByCookie 0x1402A3050`. Apply
  them to the mapped cookie after resim.

**After resim**, stop (with a short fade) every ledger entry in the replayed window
that was not re-matched.

**Stream voices:** apply the same deduplication at `0x1405D2410`, keyed by
`{frame, stream id}`.

### C. Determinism Guards

- Force identical `sGameConfig+0x80` on both peers, or isolate the RNG in
  `queueGroup5Voice` and `selectRandomVoiceVariant`.
- Make RNG consumption independent of whether the voice bank is loaded.

## Open Questions

- Swap type 6 semantics.
- Whether `cChr__getResourceRef0`/`Ref1` are character stream themes or voice
  banks.
- The exact cookie layout (`cRef+8`) used for ledger keys.
- No in-game trace yet confirms the silent-BGM path.
