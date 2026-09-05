# Global RNG versus gameplay RNG

Ghidra recheck, 2026-09-05, PC `umvc3.exe.unpacked.exe`.

## Conclusion and limits

The global stream is **not exclusively cosmetic**: native offline CPU action
selection and script wait timing consume it. It must not be discarded as
presentation-only state. However, this audit does **not** establish that changing
the global stream changes ordinary human-versus-human hitbox overlap results.
Several prominent combat random choices instead use character-local RNG.

## Verified routing

- Global state: `0x140D765D8`, four 32-bit words; getter `0x1400046D0`.
- `MtXorShift__dispatcherNext @ 0x14014EB80` and
  `MtXorShift__lockedAdvanceForCharOrGlobal @ 0x14014EAB0` select the supplied
  object's `+0x2D80` state if it is kind-of the character DTI at `0x140D44A30`.
  Null or non-character objects fall back to the global stream.
- Inclusive integer helper `0x14014EB30` calls that owner-aware float dispatcher.
- `MtXorShift__floatInRange @ 0x140455550` and
  `MtXorShift__intInRange @ 0x140455590` use the global stream directly.

## Concrete consumers

| Consumer | Verified effect / RNG domain |
|---|---|
| `rChrCpu__randomMod @ 0x14010FE50` | Offline calls global getter and next, then returns modulo. Native live-online branch uses the associated object and owner-aware dispatcher instead. |
| `rChrCpu__updateRandomActionSelection` (calls at `0x140115B82`, `0x140115C02`, `0x140115C19`) | Uses randomMod results to choose stored action-selection state and output bits. Gameplay-relevant CPU decisions, not sound. |
| `rChrCpu__processRandomWaitOpcode` (call `0x140118864`) | Uses randomMod to initialize a countdown; advances the script when the countdown expires. Gameplay timing. Corrected its receiver type from sMain to rChrCpu in Ghidra. |
| `uHsienKo__selectReservedAnkiVariant @ 0x1400A1260` | When no reserved variant exists, draws 0..15 with `this` as RNG owner: character-local. |
| `uPhoenixWright__randomizeEvidenceSlot` (calls `0x1400B1E35`, `0x1400B1ECE`) | Chooses and writes evidence slots using `this` as owner: character-local. |
| `cChr__populateHitReactionResult` (call `0x140048CCC`) | Offline random-counter-hit branch rolls modulo 100 through dispatcher with `this`: character-local for a normal fighter. Result bit 4 selects attack data +0x58 instead of +0x54. Native online skips this roll. Not evidence of global RNG affecting counter-hits. |
| `uShotVulcanMysticStare__selectUnusedVariant` (call `0x1401AD68C`) | Dispatches through the owning object obtained at +0x1308; ordinarily character-local, not directly global. |
| `sEarthAtk__selectRandomRoundEvent @ 0x140275EC0` | Global draws select campaign/round event paths. Not proof of ordinary versus collision effects. |

## Collision check

Inspected `nCollision__testNodeGeometryPairs @ 0x1405AB9A0`. Its body checks
collision ID inequality, mask overlap, bounding boxes, and primitive intersection
dispatch, then invokes callbacks. There is no direct RNG draw in this function.
This is not an exhaustive proof about every callback, primitive implementation,
or upstream producer of geometry.

RNG can influence gameplay by selecting actions or timing even if the overlap
math itself is deterministic. Conversely, finding RNG in a projectile method
does not establish a trajectory or collision effect: follow its output.

### Misleading names encountered

`uShot__lookupAttackInputSlot @ 0x1401CD5A0` forwards to `0x1401CD630`, which
reads rShot slots, calls `sMvc3Manager__getGSound` / `FUN_1402AE7F0`, and stores
sound references. Thus the random 0/1 arguments in Formation A Tiger setup
`0x140184BF0`, Formation B Hawk setup `0x140185030`, and FallRock vfn100 are
**not evidence of random attack hitbox selection**. The existing name is
misleading; this audit did not rename it globally.

Deadpool SomersaultGrenade has global draws at setup `0x140168A30` and contact
callback `0x140168550`. The examined output feeds a rotation-rate/rotation path
used by `0x1401688D0`, rather than proving random translational bounce velocity.
The follow-up [grenade RNG audit](deadpool_grenade_rng.md) traces the destination
to an embedded uCnsTSR model-joint constraint and checks the installed SHT:
mode-0 capsules with zero local endpoints, independent of the constrained joint.
The inspected movement and terminal callbacks also do not use this random spin
to choose trajectory or lifetime. These draws are not a demonstrated collision
change for this resource; see the follow-up for evidence and scope.

## Relevance to pain sounds and current hooks

Both `sGSound__selectRandomVoiceVariant @ 0x1402A9110` and bank-side
`sSound__pickVoiceVariant @ 0x1407F93B0` consume global RNG. The latter explicitly
calls next on `g_MtXorShift__globalState` for weighted selection.

`UMVC3Hook/code/rollback_rng.cpp::hkSoundRandomVariant` saves/restores global
state only while Netplay_IsActive. Its existence does not isolate every bank
selection: restoring the previously rejected group path reaches the lower bank
selector without necessarily passing through this wrapper. The listed
presentation callsites do not establish coverage of the bank picker itself.

The current CPU hook also differs from native: during netplay it returns
`rand() % modulus`; the save/restore code below that return is unreachable.
Therefore native Ghidra findings and current hooked runtime behavior must be
distinguished. No hooks were changed here.

An extra audio draw can shift subsequent global consumers, including offline CPU
decisions. It does not directly advance Phoenix Wright or Hsien-Ko's separate
character streams. For a sound-only fix, preserve/isolate audio RNG and test
offline/replay as well as online. A universal human-versus-human desync claim is
not established by this static audit.
