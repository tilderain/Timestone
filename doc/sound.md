# Sound System Notes (UMVC3)

**Status: Native reverse-engineering reference. Verified against Ghidra and
current source on 2026-07-23.** This is resource-format research, not a complete
sound-runtime layout. Unknown fields remain unnamed rather than assigned inferred
PCM or trigger semantics.

## Focused verification — 2026-10-26

Fresh read-only native bodies on **Ghidra 8193**,
`umvc3.exe.unpacked.exe`: structured allocator `0x1402A21F0`,
queue processor `0x1402B0E00`, persistent wrapper `0x1402A2A10`, and variant
selector `0x1402A9110`. Preferred VAs here assume image base `0x140000000`.
No Ghidra mutations or game/runtime tests were performed.

| Boundary | Fresh observation |
|---|---|
| Structured allocator | Voice-data virtual `+0x1A0 == 2` selects the SRQR lookup path. Missing/disabled entries fail; signed **SRQR entry `+0x1E >= 1000`** additionally returns disabled `cRef` through `0x1400239C0`. This is not a blanket check on every resource/source type. |
| Type distinction | The voice-data discriminator above is not the SRQR entry's own dword `+0x04`. Entry types 0..5 are subsequently mapped to request source types 1..6. Do not read "type 2" as rejecting every concrete clip with entry type 2. |
| Queue submission | Six slots at `sGSound+0xA38`, stride `0x438`; delay, object-alive, UI, and time-scale gates precede submission. At `0x1402B0FE3`, queued ID `+0x18` loads into R9D; `0x1402B1019` calls `0x1402A2A10`, returning to `0x1402B101E`. No direct variant-resolver call in that body. |
| Wrapper | `0x1402A2A10` writes that cue into a new request, marks it persistent, calls the shared allocator, and resets the consumed input ref, including failure paths. No direct call to `0x1402A9110` here either. |
| Variant resolver | `0x1402A9110` draws from global RNG after finding the voice ref, then uses resource identity/labels to select mappings. Normal pain groups map to silence or concrete cues; merely queueing a group does not invoke this resolver. |

Disassembly anchors: signed threshold `CMP word [RBX+0x1E],AX` at
`0x1402A22F5`, `JL 0x1402A2363` at `0x1402A22F9`; original queue call bytes
`E8 F2 19 FF FF` at `0x1402B1019`. Static signatures remain partially inferred
(the wrapper's decompilation omits stack arguments); use callsite instructions
rather than copying its four-argument pseudocode as a complete ABI.

Current source was read directly in `rollback_hooks.cpp` and `rollback_rng.cpp`.
The region-builder portion of `rollback_snapshot.cpp` still registers the global
RNG's 16 bytes, independently of audio presentation. The bank/source-format
sections below remain earlier research, **not freshly re-read archive evidence**.
Installed-bank values in [pain_voice_fix.md](pain_voice_fix.md) are unauthenticated
against a clean depot. Proposed pain fixes remain uninstalled and untested.

For the missing character pain voices, see [pain_voice_fix.md](pain_voice_fix.md):
the queued damage path bypasses variant resolution and submits group IDs rejected
by the structured allocator. That report includes resource evidence and a scoped
code-patch design; it is not an installed fix.

## Current Rollback Boundary

The native `sSound` singleton pointer is stored at `0x140E18520`, and
`sSound__move` is at `0x1405CC040`. Sound state is presentation state and is not
included in rollback snapshots.

Current `rollback_hooks.cpp::hkSoundMove` behavior is:

- During re-simulation, return without calling native `sSound__move`.
- During normal frames, call native `sSound__move` directly.

**Source correction (2026-10-26):** the earlier 40-scene inline sweep
(`sSound+0x70`, stride `0xB50`, count `+0x1CF50`) is historical, not the current
`hkSoundMove` implementation. Validation now lives in
`hkSceneUpdateSoundEnv @ native target 0x1405D6CD0`: it reads the scene's model
at `+0x80`, joint index `+0x88`, and position mode `+0xC0`. It clears the model
slot when `IsBaseModelObject` fails, the matrix virtual at vtable `+0xA0` is
inaccessible/non-executable, or mode 2 has a negative joint index, then calls the
original environment update. This is current plugin policy, not a freshly
decompiled native layout proof or a guarantee against all object-lifetime races.

Sound starts are also guarded at `sGSound__callVoicePlayOrStopVt` at
`0x1402A3380`. Re-simulation drops play dispatches (`flag == 0`) because the
audio backend is intentionally not rewound; replaying them would stack another
voice for every correction. Stop dispatches remain enabled so a corrected
timeline can retire a voice started by the live path. Hooking this lower helper
keeps the surrounding `cRef` initialization and reset paths intact.

That dispatch is not used by structured voice allocation. Character and shot
paths such as MODOK's `sGSound__playVoiceExt_AD60` create voices through the
shared allocator at `0x1402A21F0`. During re-simulation, allocations whose voice
request has the persistent-loop flag `0x80` at request `+0x30` are replaced with
the native disabled-`cRef` initializer at `0x1400239C0`; ordinary structured
voices still use the native allocator. The `sGSound__playVoiceExt_AD60` path sets
this flag before allocation, while the ordinary `sGSound__playVoice_AC40` path
does not. This is important for looping shots: Jamming Bomb, Fire Carpet, Light
Wall, Healing Field, TK Bind, Barrier, Ankihou, and Shocking Pink retain a cookie
that their destructor passes to `cGSound__stopSoundByCookie`. A disabled cookie
is safe to stop and cannot orphan a backend loop when the shot is reconstructed
repeatedly. The separate raw start wrapper `sSound__playSoundWithDb` at
`0x1405D28B0` is also suppressed during re-simulation; cookie-based stop paths
remain enabled.

The variable name `isLooping` in `hkGSoundCreateVoice` tests request flag `0x80`;
it does not prove every flagged request audibly loops. The character queue uses
the persistent wrapper too. Source inspection confirms this guard and the native
disabled-cookie return, but not successful hook installation in a live process.
`rollback_rng.cpp::hkSoundRandomVariant` saves/restores global state only while
`Netplay_IsActive()`. Its selected low-level presentation callsites are not a
universal bank-picker isolation guarantee. See the pain report for offline/replay
RNG requirements and the corrected normal heavy-pain probability table.

This prevents headless ticks and stale post-rollback model references from
advancing or dereferencing audio-scene presentation state. Type validation is
required because a freed model address can be reused by another valid object;
checking only that vtable `+0xA0` is executable can dispatch an unrelated
virtual with the sound scene's joint index as an incompatible argument. It does
not rewind voices, emitters, streams, or the XAudio2 backend.

## Resource Types

The resource extension/type strings returned by the verified classes are:

| Type string | Class | Verified meaning |
|---|---|---|
| `sbkr` | `rSoundBank` | Sound-bank container/index |
| `xsew` | `rSoundSourceMSADPCM` | MSADPCM sound source |
| `sngw` | `rSoundSourceOggVorbis` | Ogg Vorbis sound source |
| Unknown | `rSoundSourcePC` | Encoding not verified |

An `.sbkr` resource holds tables that can reference other `cResource` objects by
stored DTI hash and resource path. `xsew` and `sngw` are two verified source
types, but the bank reader itself resolves arbitrary stored DTIs and is not
restricted to those two classes.

## MVC3 Loading Pipeline

`sMvc3Sound__buildStagePath` at `0x1402B0A10` formats stage sound paths as:

```text
sound\se\stg\st%s\st%s
```

It then calls the common MVC3 sound loader with a formatted display label and
the path.

`sMvc3Sound__allocAndDispatchLoad` at `0x1402A0A40` has two verified source
choices:

- Type 1 allocates a `0xA8`-byte object and constructs it through
  `rSoundSourceMSADPCM__constructLoad_maybe` at `0x1402A6A20`.
- Type 2 allocates a `0xA8`-byte object and constructs it through
  `rSoundSourceOggVorbis__constructLoad_maybe` at `0x1402A4E40`.
- Any other value returns an empty/disabled reference.

Both successful paths invoke common virtual initialization, append the object to
`sGSound`, and enable it. Failed initialization runs native cleanup. The integer
choices are dispatcher values, not proven universal on-disk format identifiers.

## `.sbkr` Resource

### Native methods

| Address | Function | Role |
|---|---|---|
| `0x1407F9320` | `rSoundBank__dtor` | Reset bank, destroy base resource, optionally free object |
| `0x1407F96A0` | `rSoundBank__reset_maybe` | Release retained resources and free all three tables |
| `0x1407F9820` | `rSoundBank__getExt` | Return `"sbkr"` |
| `0x1407F9830` | Bank read virtual | Read and resolve `.sbkr` data |
| `0x1407F9AD0` | Bank readiness virtual | Poll resources and discard/reset failed entries |
| `0x1407F9BF0` | Bank write virtual | Serialize `.sbkr` data |

The read and write slots are established from the `rSoundBank` vtable and their
stream behavior. Some existing Ghidra names for these slots are misleading.

### Runtime layout

Offsets are relative to the `rSoundBank` object:

| Offset | Size | Meaning |
|---|---:|---|
| `+0x70` | 4 | Table 1 count |
| `+0x78` | 8 | Table 1 pointer; entries have stride `0x18` |
| `+0x80` | 4 | Table 2 count |
| `+0x88` | 8 | Table 2 pointer; entries have stride `0x58` |
| `+0x90` | 4 | Table 3 count |
| `+0x98` | 8 | Table 3 pointer; entries have stride `0x08` |

The preceding bytes belong to the inherited `cResource` body. The verified bank
allocator global is `0x140E1DEA0`; the reset and destructor paths obtain an
allocator through that global to free the tables or object.

### File header

```text
Offset  Size  Field
0x00    4     Magic 0x524B4253 (bytes "SBKR")
0x04    4     Version 4
0x08    4     Table 1 count
0x0C    4     Table 2 count
0x10    4     Table 3 count
0x14          Table 1 begins
```

The reader rejects a mismatched magic or version.

### Table 1

Table 1 has `count1` entries of `0x18` bytes:

```text
+0x00  qword with unknown identifier semantics
+0x08  Table 2 index in the file; Table 2 pointer at runtime
+0x10  qword with unknown semantics
```

The reader converts the file value at `+0x08` to
`table2 + value * 0x58`. The writer performs the inverse calculation. Calling it
a file-relative byte pointer would therefore be incorrect.

### Table 2

Table 2 has `count2` entries of `0x58` bytes:

```text
+0x00  retained cResource pointer at runtime; not stored directly in the file
+0x08  DTI hash
+0x0C  0x4C-byte payload with incompletely recovered semantics
```

For each entry, the file stores a null-terminated resource path/name of at most
`0x40` bytes, followed by `0x50` bytes loaded into entry `+0x08`. For a non-empty
string, the reader:

1. Resolves the DTI with `MtDTI__findByHash(entry.hash)`.
2. Calls the `sResource` virtual at `+0x60` with the DTI, string, and flag 1.
3. Stores the returned retained `cResource*` at entry `+0x00`.

An empty string leaves the runtime resource pointer null. Reset releases every
non-null pointer with `cResource__releaseRef` before freeing the table.

### Table 3

Table 3 is a direct array of `count3` qwords. Its purpose is not statically
verified.

### Read and write behavior

The bank reader:

1. Opens a stream with `0x1000` buffering and reads the `0x14`-byte header.
2. Validates magic and version, then resets the previous bank state.
3. Allocates all three tables with 16-byte alignment.
4. Reads Table 1 and converts Table 2 indices into pointers.
5. Reads each Table 2 string and payload and resolves non-empty resources.
6. Reads Table 3 as `count3 * 8` bytes.

The bank writer emits the inverse representation. For each populated Table 2
entry it writes the resource string, derives a DTI hash through the resource's
DTI virtual path, and writes the remaining `0x4C` bytes. For a null entry it
writes a zero hash and an empty string before writing that payload.

The readiness virtual at `0x1407F9AD0` is not the writer. It walks Table 2 and:

- Releases and clears resources whose loaded-state flag is not set.
- Calls resource virtual `+0x80` for flagged entries.
- Resets the complete bank and returns false if a resource reports failure.

## Source Parsers

### `xsew` (`rSoundSourceMSADPCM`)

`rSoundSourceMSADPCM_vfn36` at `0x1407FB0D0` opens the source through
`sResource`, reads structured headers and format fields, and supports either a
memory-backed source at `+0xC0` or a stream object at `+0xF8`.

Offsets `+0x90` and `+0x100` participate in bounds and availability checks, but
the current static evidence does not justify naming both as data-size fields.
The exact container variants and semantics of every parsed word remain open.

### `sngw` (`rSoundSourceOggVorbis`)

`rSoundSourceOggVorbis_vfn36` at `0x1407FB740` verifies or synthesizes the Ogg
capture pattern `OggS`, initializes the bundled Vorbis reader, and extracts
stream metadata. It recognizes comments including `LoopStart`, `LoopEnd`, `Ver`,
`PeriodNum`, `P`, `Time`, and `MarkerNum`.

## Allocator Notes

| Class | Global | Verified use |
|---|---|---|
| `rSoundBank` | `0x140E1DEA0` | Bank table allocation/free and optional object destruction |
| `rSoundSourceMSADPCM` | `0x140E1DF50` | Class allocator global; not the DTI object itself |
| `rSoundSource` | `rSoundSource__AllocDTI` | Allocator/DTI helper; exact global layout not documented here |
