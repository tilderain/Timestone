# Missing pain voices: native cause and code-patch design

**Research date:** 2026-09-04. Ghidra program:
`ichneumon_sub/umvc3.exe.unpacked.exe`, image base `0x140000000`.

**Status:** Failure mechanism verified in native code and locally installed
character voice resources. Patch design below is not installed or runtime-tested.
The installed archives have not been authenticated against a clean Steam depot;
the audit emits resource SHA-256 values to make comparisons reproducible.

## Conclusion

The ordinary damage-voice queue submits **group IDs `0x15..0x17` directly to
allocation**, without passing through the PC executable's voice-variant resolver.
Those SRQR entries have a signed word at `+0x1E` of at least 1000. The allocator
explicitly rejects such requests and returns a disabled sound cookie.

The individual pain clips `0x18..0x20` remain available. The executable even has
the light/medium/heavy group-to-clip selection logic at `0x1402A9110`; the queued
damage path simply does not call it.

Dark Phoenix overrides the normal group request with direct IDs `0x61..0x63`.
Those entries pass the checks in her installed resource, explaining why she works.

This is not evidence that the pain samples are missing or that their Ogg volume
is wrong. The inspected voice archives actually contain RIFF/MSADPCM samples.
We have not compared a PS3/360 executable, so the exact historical porting change
that introduced the mismatch remains unverified.

## Native chain

| Address | Function / observation |
|---|---|
| `0x14003D8E0` | `cChr__processDamageReactionState`: ordinary reaction stops previous character voices, then calls character vtable `+0x3A0` with the damage-voice rank. |
| `0x140041F00` | `cChr__getDamageVoiceRank`: reads dword `cChr+0x63B8`. |
| `0x140056730` | `uCharacter__queueDamageVoice`: default vtable `+0x3A0` implementation. Previously misleadingly named `playWeaponStateVoice`. |
| `0x1402ACDE0` | `sGSound__queueDamageVoice`: forwards character and rank to category 1 of the next function. |
| `0x1402ACFE0` | `sGSound__playVoiceForCharAction`: category 1 maps rank 0/1/2 to `0x15/0x16/0x17`, obtains character voice resource ref, queues it with delay 10. |
| `0x1402AD320` | Queue record writer: resource ref `+0x00`, character `+0x10`, ID `+0x18`, slot key `+0x20`, flags `+0x28`, delay `+0x2C`. |
| `0x1402B0E00` | `sGSound__processCharacterSoundQueues`: six voice records at `sGSound+0xA38`, stride `0x438`; waits for delay/time-scale gates. |
| **`0x1402B1019`** | Calls `0x1402A2A10` with **unresolved queued ID**. No call to the variant resolver. Return address `0x1402B101E`. |
| `0x1402A2A10` | `sGSound__createPersistentVoiceRequest`: builds request, sets flag `0x80`, calls shared allocator. This flag is also used for these queued voices; do not infer that every such voice audibly loops. |
| `0x1402A21F0` | `sGSound__createStructuredVoice`: looks up the SRQR entry and rejects it when signed `entry+0x1E >= 1000`. |

The damage rank is obtained before resolving the damage animation, not inferred
from a ground/air animation number. A fix at the queue's allocation boundary can
therefore retain per-hit strength selection for air combos. Its exact behavior
on specials/hypers should still be checked in game against original-version footage.

### Rejection instructions

```asm
1402A22ED  mov eax, 1000
1402A22F5  cmp word ptr [rbx+1Eh], ax
1402A22F9  jl  1402A2363       ; only values below 1000 continue
1402A22FB  or  rdx, -1
1402A22FF  mov rcx, rdi       ; output cookie
1402A2302  call 1400239C0     ; native disabled-cRef initializer
```

Do not globally NOP this check without investigating why it was introduced.
**Follow-up:** these are valid bank-group descriptors, not malformed sample
descriptors; the lower backend supports resolving them (see below). A narrowly
scoped bypass for validated pain groups is another candidate, not yet tested.

## Resource evidence

`rSoundRequest__convert @ 0x140688AE0` accepts `SRQR`, version 5.
`rSoundRequest__resolveLoadedTables @ 0x1406890B0` establishes:

```text
file + 0x08: entry count
file + 0x48: first entry
entry stride: 0x90
entry + 0x00: ushort cue ID
entry + 0x04: request type discriminator
entry + 0x08: enabled/nonzero check
entry + 0x1E: signed sound-bank group ID, tested against 1000 by sGSound
```

`rSoundRequest__findEntryById @ 0x140688A50` was incorrectly labeled a shader
sampler lookup. It searches these records using the cue ID, either linearly or
through a ushort index table. Ghidra's function type/name were corrected.

Representative local values:

| Resource | IDs | Type | Enabled | `+0x1E` |
|---|---|---:|---:|---|
| Ryu English | `15/16/17` | 0 | 1 | `1392/1393/1394` — rejected |
| Ryu Japanese | `15/16/17` | 0 | 1 | `1400/1401/1402` — rejected |
| Phoenix English | `15/16/17` | 0 | 1 | `1365/1366/1367` — rejected |
| Dante English | `15/16/17` | 0 | 1 | `1083/1084/1085` — rejected |
| Vergil English | `15/16/17` | 0 | 1 | `1544/1545/1546` — rejected |
| All above | `18..20` | 2 | 1 | `0` — pass entry checks |
| Phoenix English | `61/62/63` | 2 | 1 | `0` — pass entry checks |

Audited all top-level `????_0[01].arc` under the installed
`nativePCx64/sound/se/chr/archive`: **73 voice SRQR resources**.
All 73 reject all three normal pain-group IDs; all 73 have all nine concrete
pain IDs enabled and below the rejection threshold. Passing these checks is not
by itself proof of audible playback or a complete validation of the sound bank.

Example resource SHA-256 values:

```text
Ryu EN: 360d6d63a2e182467e98af831093b9104cfe9fd752e997a1c3be2c41f6eb5e13
Ryu JP: c3d6b3e36bf5550117b28528eded111023d5e03b6e06b5d5dd211ce756b0f927
Phoenix EN: 4abd2a028b3c572ea6b9698e49ab3791852bf1c820f892316f1addf8786d4f74
```

Reproduce without modifying/extracting game files:

```sh
python3 tools/audit_pain_voice_requests.py \
  '../ULTIMATE MARVEL VS. CAPCOM 3/nativePCx64/sound/se/chr/archive/'????_0[01].arc \
  > /tmp/opencode/pain_voice_audit.jsonl
```

### Follow-up: what 1392 means

`FUN_1405C4030` reads request `+0x1E` when the caller's bank-group override is
`-1`, passes that value to `rSoundBank__findGroupById @ 0x1407F9660`, then to
the weighted bank resolver at `0x1407F93B0`. The lookup compares it with the
first ushort of SBKR Table 1 entries. Thus **1392 is a bank-local group ID**,
not a volume, duration, byte offset, or index into the character's SRQR array.

In the installed Ryu EN SBKR, group 1392 points to Table 2 entries 132..135:

| Source suffix | Weight |
|---|---:|
| `source\\blank` | 40 |
| `source\\ryu_013e` | 20 |
| `source\\ryu_014e` | 20 |
| `source\\ryu_015e` | 20 |

Group 1393 is the medium group: blank/016e/017e/018e, weights 30/30/20/20.
Group 1394 is the heavy group: blank/019e/020e/021e, weights 20/30/30/20.
All three groups exist in the bank. The high IDs are not intrinsically invalid;
the higher-level allocator imposes an additional restriction that blocks them.
Why that restriction exists in the port remains unresolved.

This also qualifies the proposed hardcoded selector below: it reproduces the
**PC executable's** selector, not necessarily the bank's authored probabilities.
For Ryu heavy pain the bank is 20/30/30/20, while the executable selector is
20/30/10/40. A validated, pain-only admission to the existing bank resolver could
preserve bank-specific probabilities and mods better than hardcoded remapping.
That alternative requires checking the later backend path, global RNG use and
cookie/resimulation behavior in game before recommending it over remapping.

## Dark Phoenix and install exceptions

`uPhoenix__queueDamageVoice @ 0x1400BC150`:

```cpp
if (!character->darkPhoenixFlag) // byte +0x6930
    queueDamageVoice(sound, character, rank);
else
    playVoiceForCharAction(sound, character, -1, rank + 0x61, 0);
```

Dante `0x140070370` and Vergil `0x1400E8390` deliberately do not queue the
ordinary pain voice while their respective Devil Trigger flags are set. Preserve
those overrides; do not globally replace character vtable slots or force normal
pain voices into install states as part of this fix.

## Proposed mod-side patch

**Recommended boundary:** intercept the single allocation call at
`0x1402B1019`. Preserve its ABI and forward every argument to the original
`0x1402A2A10`, changing only `cueId` when it is `0x15..0x17`.

Options: a nearby relay for the original `E8 F2 19 FF FF` call, or a MinHook
detour on `0x1402A2A10` restricted to caller return address `0x1402B101E`.
Relocate addresses with `_addr()`, verify original bytes, and fail closed on an
unsupported executable. Do not assume an arbitrary DLL target is within rel32
reach. A global allocator remap would affect unrelated banks and is not proposed.

The call's arguments, verified in disassembly, are:

```cpp
uint64_t* createPersistentVoiceRequest(
    void* dispatcher, uint64_t* outCookie, void* consumedBankRef,
    uint32_t cueId, uint64_t slotKey, void* character,
    uint32_t mode, float gain, float extra);
// Here: dispatcher = sGSound+0x29B0, mode = 7.
```

The following selector reproduces the existing native normal-pain mappings from
`sGSound__selectRandomVoiceVariant @ 0x1402A9110`. `roll` must be supplied by a
**presentation-only** source and lie in `0..99`:

```cpp
uint32_t ResolveNormalPainCue(uint32_t cue, uint32_t roll)
{
    switch (cue) {
    case 0x15:
        if (roll < 40) return 0; // native intentional no-voice outcome
        if (roll < 60) return 0x18;
        return roll < 80 ? 0x19 : 0x1A;
    case 0x16:
        if (roll < 30) return 0;
        if (roll < 60) return 0x1B;
        return roll < 80 ? 0x1C : 0x1D;
    case 0x17:
        if (roll < 20) return 0;
        if (roll < 50) return 0x1E;
        return roll < 60 ? 0x1F : 0x20;
    default:
        return cue;
    }
}
```

Only consume presentation RNG for the three affected IDs on live playback.
Continue through the native allocator even for result 0, preserving its
disabled-cookie and consumed-bank-ref cleanup behavior. Do not return early
with an uninitialized cookie or leak a copied `cRef`. Keep Dark Phoenix IDs,
queue delay, hitstop gates, stop behavior, attachment, gain, mode and flags intact.

This targets native-like probabilistic pain voices, not "play on every hit."
An always-play option would be a separate behavioral choice.

### Rollback integration

`UMVC3Hook/code/rollback_hooks.cpp::hkGSoundCreateVoice` already suppresses
flag-`0x80` structured requests during resimulation by returning the native
disabled cookie. The proposed wrapper must preserve that call path and flag.
Do not replace it with an ordinary voice allocation, which is not suppressed by
that particular guard. On resimulation, forward the original cue without drawing
new presentation randomness and let the existing guard handle suppression.

`rollback_rng.cpp::hkSoundRandomVariant` already saves/restores global RNG around
the native selector **when netplay is active**. Thus calling the hooked selector
could reuse existing online isolation, but would add global-RNG consumption
offline and depend on successful hook initialization. A small isolated selector
for just these three groups avoids that dependency and makes an audio toggle
independent of gameplay RNG in offline/replay tests too.

Sound remains unsnapshotted presentation state. Do not change combat data,
animation commands, roster protocol or snapshot layout for this fix. A local-only
toggle is the intended design, **not yet a tested desync-safety guarantee**.

## Validation before enabling by default

1. Clean/unmodified universal animations; disable Yoshi/other archive pain fixes
   during comparison to avoid duplicate calls. Do not automatically overwrite mods.
2. Log queue ID, resolved ID, character/slot and returned cookie for L/M/H hits,
   on ground and in air; demonstrate `15..17` fail and `18..20` allocate live.
3. Test normal Phoenix vs Dark Phoenix; Dante/Vergil with and without installs;
   both voice languages, assists, repeated hits, hitstop, throws, knockdowns and KOs.
4. Confirm no duplicate pain voices where animation-script voices already work.
5. Compare identical replay/input runs with toggle on/off for gameplay checksums
   and RNG, offline and online; force rollback/resimulation and check for audio
   stacking, stuck cookies and missed/late queue behavior.
6. Validate against clean depot resources and any supported custom voice banks.

## Related Frank / Wright clue (not part of this patch)

`0x1402A9110` explicitly checks sound labels containing `CHR SE` and `Frank` or
`Naruhodo`. It remaps requested IDs `1/2/3` to `20..22 / 24..26 / 28..2A`.
This is executable-side, label-dependent behavior consistent with the report that
cloning can change the symptom. We did not trace those requests to plunger/paper
samples, so this is a lead, not a verified complete fix for their mismatched SFX.
