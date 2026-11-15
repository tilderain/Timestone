# Rollback Sound System: Plan

Ghidra 8192 `umvc3.exe.unpacked.exe`, 2026-11-15. Static analysis only. Facts
marked **verified** were read in native code this pass; everything else is design.

Goal:
- Sound is not processed during resim.
- On rollback, kill the voices that exist only in the abandoned future.
- Sounds the corrected timeline queues are played once, at the end of resim.
- Sounds both timelines agree on keep playing untouched.

## 1. Native Facts That Shape The Design (verified)

### Two layers

| Layer | Object | Runs in resim? |
|---|---|---|
| Game | `sGSound`: voice manager at `+0x29B0` (64 entries at `+0x90`, list `+0x68`), cookies `(entryPtr<<8)\|gen8` (`sGSound__allocVoiceId`, global byte `0x140D5336C`) | Producers yes; `sGSound::move` (`0x1402AA6C0`) **no** |
| Engine | `sSound` (`0x140E18520`): command rings → voices | `sSound::move` (`0x1405CC040`) **no** |

`rollback_tick.cpp` skips both `mpSound::move` and `sGSound::move` in the headless
tick. Every command issued during resim therefore sits in sSound's rings and runs
on the next live `sSound::move`, which is already "after resim".

### All audio funnels into two ring writers

- **SE commands:** `sSound__setSeEntry @ 0x1405D1100`. The ring is at `+0x1CEF0`:
  stride `0x80`, capacity `+0x1CF60` = **128**, count `+0x1CFA8`.
- **Stream commands** (voices, announcer, BGM): `sSound__setStreamEntry @ 0x1405D2410`.
  The ring is at `+0x1CEF8`: stride `0x78`, capacity `+0x1CF64` = **32**, count
  `+0x1CFE8`.
- Both writers take a critical section, so producers may run on several threads.
- The only consumer is `sSound__resolveEntry @ 0x1405CDBB0`, called from
  `sSound::move` on the main thread. A full ring drops the command silently.

Entry layout, shared by both rings:

| Offset | Field |
|---|---|
| `+0x00` | request |
| `+0x08` | cue id |
| `+0x10` | owner key |
| `+0x18` | type: 1 start, 2 bank play, 3 stop, 4 pause, 5 resume, 6 control, 7 fade |
| `+0x1C` | subcommand |
| `+0x20..+0x47` | args |
| `+0x58` | link |
| `+0x68` | retained request ref |

Producers of starts (type 1):

- `sGSound__SeVoiceData__dispatchCommand @ 0x1402A5E80` (renamed): SE
  start/stop/pause/keyOff for game voices.
- `sGSound__StreamVoiceData__dispatchCommand @ 0x1402A7920` (renamed): the stream
  equivalent.
- `uEffect__requestSoundSe @ 0x1405E6970`: particle SEs. The owner is the
  `cParticleManager`. This runs from `mpEffect::move`, which is **not** ticked in
  resim.
- `cSoundPhysicsSoftBody__requestSe`.

`sSound__playSoundWithDb @ 0x1405D28B0` is **not** a start. It is a stream gain
control (type 6, sub `0x2E`). The current "raw start" suppression hook targets a
volume command.

### Voice identity

**Voice identity is the triple `(request, cueId, owner)`.** Stops, status queries
and fades all match on it, with wildcards: request 0, id −1, owner −1.

- For game voices, the owner is the caller's object (character or shot), taken
  from VoiceRequest `+0x40`. Narration and system cues use owner 0.
- Each sSound voice also carries a unique **serial** at `+0x44`, taken from
  `++sSound+0x1D720`. Both SE and stream voices get one.
  - SE voices are at `+0x70` (stride `0xB50`, count `+0x1CF50`).
  - Stream voices are at `+0x16A70` (stride `0xB30`, count `+0x1CF54`).
  - Voice fields: `+0x0C` state (2 = stop), `+0x10` request, `+0x18` id,
    `+0x20` owner.

The serial lets us kill exactly one voice instead of everything sharing its triple.

### Native frame order

`mpSound::move` resolves the rings before `sMvc3Manager::move` / `sGSound::move`.
`sGSound::move` polls each game entry's status by triple through
`sSound__getSeStatus` / `getStreamStatus`, and unlinks entries that report 0.
Commands flushed before the live `sSound::move` are therefore seen correctly.

### Sound-side state that is gameplay-produced but not rewound

None of `sGSound` is snapshotted today (grep: no region).

- **Delayed character voices:** 6 records of `0x30` bytes at
  `sGSound+0xA38 + i*0x438`. Each holds a cRef, character `+0x10`, cue `+0x18`, key
  `+0x20`, flags `+0x28` and a delay float `+0x2C`. They are consumed by
  `processCharacterSoundQueues @ 0x1402B0E00`, which runs in resim (cat 7).
- **Same-tick group queue:** 20 records of `0x50` bytes at `+0x1FD0..+0x2610`.
- **Narration (announcer) queues:**
  - Two queues: `+0x2610` (ref 4) and `+0x27D8` (ref 5). Each has 8 entries of
    `0x38` bytes and a timer at `+0x1C0`. Together they span `+0x2610..+0x29A0`.
  - Globals at `+0x8A70..+0x8A7B`.
  - Producers `queueGroup4Voice @ 0x1402AE4B0` (KO cue `0x6F`) and
    `queueGroup5Voice` run in gameplay.
  - The consumer `sGSound__updateActiveCueOrReset @ 0x1402B1330` runs **only** in
    `sGSound::move`. So the announcer queue is neither rewound nor advanced in
    resim. This is the KO-announcer bug.
- All cRefs are plain data (`{vtable 0x140B12478, value}`), with no refcount. Raw
  memcpy restore is safe: a stale cookie fails its generation check in
  `findVoiceByKey`.

## 2. Design: One Ledger At The Ring Writers

Hook only `setSeEntry` and `setStreamEntry`, plus the existing `sSound::move` hook.
Everything above them (allocator, RNG, cookies, game voice list) runs natively in
both live and resim, so RNG consumption and cookies stay identical.

### Data (fixed size, no per-frame allocation)

```cpp
struct SoundKey { uint64_t request, owner; uint32_t id; uint8_t stream; };
struct LedgerEntry {            // one per live start (type 1/2) from a sim source
    SoundKey key; int32_t frame; uint32_t serial;   // serial 0 = not yet bound
    uint8_t alive, matched;     // alive: no stop seen; matched: reissued in resim
};
LedgerEntry g_ledger[512];      // ring; entries older than the confirmed frame retire
struct DeferredCmd { uint8_t stream; int32_t frame; SetEntryArgs args; }; // 11 args
std::vector<DeferredCmd> g_resimCmds;  // reserve(1024) once
```

### Live frame

1. In the `setXxEntry` hook (with a lock), for type 1/2 from a **sim source**:
   push `{key, frame = Controller_GetExecutingFrame()}`. For type 3 (stop) and 7
   (fade-out): mark matching alive entries `alive = 0` (same wildcard rules as
   native). Then call the original.
2. In `hkSoundMove`:
   - Before `oSoundMove`, read `serialBefore = sSound+0x1D720`.
   - After it, if any entries are unbound, scan the SE and stream voice arrays once
     for `serial > serialBefore`.
   - Bind each new voice to the oldest unbound entry with the same triple.

### Resim (between `Rollback_BeginSoundResim()` before restore and `End` after the last frame)

- The `setXxEntry` hook calls nothing native. It appends to `g_resimCmds` with the
  resim frame, and addrefs the request (`cResource__addRef 0x14050D100`) so it
  survives until flush.
- Restore-time kills, such as quarantined shot destructors that stop their
  cookies, land here too, in chronological order.

### End of resim: one ordered pass over `g_resimCmds`

| Resim command | Action |
|---|---|
| Start, and an alive unmatched ledger entry has the same key at `\|frame − f\| ≤ 2` | **Duplicate.** Set `matched = 1` and drop the command. The live voice keeps playing. |
| Start, no match | **New.** Flush it to the original writer. This plays it now. |
| Stop / fade aimed at a matched or older alive entry | Expand to concrete targets. Kill those entries by serial, unless the live timeline already stopped them. **Never forward a wildcard stop**: it would also hit voices the live timeline started after the stop's frame. |
| Stop aimed at a new start in the same buffer | Cancel both. |
| Control (6), pause/resume (4/5) | Forward only if the target is a new start. Live already applied the same controls to duplicates. |

Then, for every ledger entry with `frame > restoreFrame`, `alive` and `!matched`:
it exists only in the abandoned future, so **kill it by serial**. If the voice slot
still has that serial, set state `+0x0C = 2`, on the main thread before
`sSound::move`, exactly as `extractStopSe_entry` does. Use the ring stop by triple
only if the voice is not bound yet.

Finally:
- Drop the killed entries.
- Replace the window with matched entries and new starts tagged with their resim
  frames.
- Release the addrefs and clear the buffer.

Cost:
- **Live:** one hook per ring command, a 40-byte push per start, a ≤512 compare
  scan per stop, and one voice-array scan per frame that had starts.
- **Resim:** cheaper than today, since commands are only appended.
- **End:** O(commands × window entries); both are small.

### Sim-source classification (implemented by context, not owner)

The owner key is not an object: game voices pass a small setting id
(`createOrdinaryVoiceRequest` param 5 → VoiceRequest `+0x40`), and the real
owner is attached later at `+0x178`. Classification is therefore by **when** a
command is issued. A live command counts as **sim** while one of these is
running:
- the per-line `sUnit` passes for the resim lanes (3, 7–16, 18, 21):
  `moveLine 0x14051B4C0`, `moveAfterLine 0x14051B6F0`, `syncLine 0x14051BF10`.
  Their jobs finish inside the line call, so a global depth counter also covers
  worker threads;
- `sHitSolver::move 0x140011D90` or `sCharacter::move 0x1400597F0`;
- the narration consumer `0x1402B1330`.

`RollbackSoundPresentationScope` (opened by `hkPlaySystemCue`) forces
presentation for the music path. In resim every command is sim.

### Cut-off protection (implemented as held stops)

A sim stop on a frame that can still roll back (netplay and the frame is
unconfirmed) does not reach sSound: the matching ledger entries remember the
stop frame instead. Once the frame is confirmed, or is `ROLLBACK_WINDOW` old,
the hold is released and the entry's voices are killed by serial. If a rollback
replaces that frame and the corrected timeline does not stop the voice again,
the hold is dropped and the voice continues without a gap. The cost: on a
predicted frame, a real cut-off lands at the confirmation latency instead of
immediately.

## 3. Make The Sim-Side Sound Queues Real Simulation State

1. Add raw snapshot regions on `sGSound`, about 3.3 KB in one or two memcpy
   ranges:
   - the 6 × `0x30` delayed records at `+0xA38 + i*0x438`;
   - `+0x1FD0..+0x29A0` (group queue and both narration queues);
   - `+0x8A70..+0x8A7C`.
2. Tick the narration consumer in the headless tick, at the `sGSound::move`
   position. Mirror its tail exactly: if not paused and `!sGameUI+0x5ABA`, then for
   ref 4 and ref 5, if `findVoiceByRef(gs+0x29B0, ref)` then
   `updateActiveCueOrReset(gs, ref, queue)`. The announcer's
   `playOrdinaryVoice` then issues its start inside resim, and the ledger
   dedups or kills it like any other sound.

This fixes the KO announcer:
- A predicted KO that rolls back has its "K.O." voice killed at the end of resim.
- A KO that only the corrected timeline has gets announced once.

## 4. Hooks: Remove, Keep, Add

**Remove:**
- `hkGSoundVoiceDispatch` (`0x1402A3380`)
- `hkGSoundCreateVoice` (`0x1402A21F0`)
- `hkSoundPlayWithDb` (`0x1405D28B0`)
- the `SUPPRESS_RESIM_SOUND` switch

Resim allocation runs natively, so looping shots keep real cookies whose triples
match the live voice. A later destructor stop then reaches it correctly.

**Keep:**
- `hkSoundMove` (it gains the serial bind and the kill apply);
- the theme hooks, with resim system cues still disabled;
- `hkSceneUpdateSoundEnv`;
- the sound-variant RNG isolation.

**Add:**
- `setSeEntry` and `setStreamEntry`;
- `Rollback_BeginSoundResim(restoreFrame)` and `Rollback_EndSoundResim()` at the
  current theme Begin/End call sites in `rollback_resim.cpp`;
- `Rollback_SoundConfirm(frame)` to retire old ledger entries;
- a round or reset hook to clear the ledger.

**One-time init:** `sSound__setStreamEntryNum(sSound, 128)` (`0x1405D2590`;
`setSeEntryNum` is at `0x1405D1260`). The 32-entry stream ring is too small for a
burst of flushed starts plus a live frame. The call clears the ring, so do it
before battle.

## 5. Known Limits

- **Un-stop.** Handled by held stops (above). A presentation stop, or a stop on
  a confirmed frame, is applied immediately and cannot be undone.
- **Stage-owned effects** run on live time only, but their lanes count as sim.
  Their sounds in the rollback window are killed and not replayed.
- **Re-born owners.** A shot reborn in resim at a different address changes the
  owner key, so its sound restarts instead of continuing. Optional fallback:
  match `(request, id, frame)` ignoring the owner when both owners are new shots
  or effects, then patch the live voice's `+0x20` to the new owner so later stops
  reach it.
- **Game voice list cap (`+0x6090`).** Not polled during resim. A very long resim
  with many sounds could hit the cap. Unverified at runtime.
- **Particle SEs** of rolled-back hit sparks are presentation and are not killed.
  They are short one-shots.

## 6. Order Of Work

1. Ring hooks, ledger, and End pass with triple-stop kills. Remove the old
   suppression hooks. Enlarge the stream ring.
2. Serial binding in `hkSoundMove` and precise kills.
3. The `sGSound` snapshot regions and the headless narration tick (KO announcer).
4. Optional: un-stop restart and re-born owner fallback.
