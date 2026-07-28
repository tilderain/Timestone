# Command Matcher State Requirements

**Status:** Native behavior confirmed against `umvc3.exe.unpacked.exe` in
Ghidra on 2026-07-28. Field meanings that remain uncertain are described by
their behavior rather than given speculative names.

## Matcher instances

Each `cChr` embeds two independent `cChrCmd` interpreters:

| Character offset | Concrete type | Autotest index | Role |
|---|---|---|---|
| `+0x2738` | `cChrCmdSpatk` | 0 | Standalone attacks and special commands |
| `+0x2950` | `cChrCmdCombo` | 1 | Chain/combo commands |

Both use the command resource at matcher `+0x168`, the command count at
`+0x170`, and the mutable `0x20`-byte line array at `+0x178`.

## Ground and air requirement

For `cChrCmdSpatk`, the signed value at **command record `+0x18`** controls the
ground/air restriction. This is part of the static command record returned by
`rCommandList__getCommandByLineIndex`, not the mutable line entry.

| `command+0x18` | Requirement |
|---:|---|
| `0` | Grounded: reject if `cChr::mAnmF & 0x20` or `cChr::mPrgF & 0x4` is set |
| `1` | Airborne: reject unless at least one of those two bits is set |
| Other | No ground/air rejection in this check |

The check is at `cChrCmdSpatk__passesLineRestrictions` (`0x14002CDE0`, native
instructions `0x14002CFBE` through `0x14002CFFC`). This is the correct source
for autotest jump setup. **`command+0x20 bit 0` is not the air marker.** That
bit is also used by command precedence and command-list/hyper handling.

`cChrCmdCombo` does not apply this ground/air interpretation. Its restriction
function uses command `+0x18` as a numeric threshold, then applies combo-chain
and cancellation rules. Therefore ground/air setup must be limited to matcher
0.

## Matcher-wide limits

Before or around command-step evaluation, the matcher applies its embedded
`cChrCmdLimit` (`matcher+0x08`). `cChrCmdLimit__passesCharacterState` at
`0x14002AE30` confirms these gates:

| Matcher offset | Property | Requirement |
|---:|---|---|
| `+0x10`, `+0x14` | `mLifeRateB`, `mLifeRateS` | Character life-ratio range |
| `+0x18`, `+0x1C` | `mGaugeRateB`, `mGaugeRateS` | Team gauge-ratio range |
| `+0x20` | `mRLifeRateB` | Recoverable-life threshold |
| `+0x24`, `+0x28` | `mStatusNo`, `mStatusNoN` | Required/forbidden status number |
| `+0x2C`, `+0x30` | `mAbility`, `mAbilityN` | Required/forbidden team-slot ability/state |
| `+0x34`..`+0x40` | animation mark/number pairs | Forbidden current/old animation combinations |
| `+0x44`, `+0x48` | `mHeightB`, `mHeightS` | Minimum/maximum world Y position |
| `+0x4C` | `mJumpCount` | Reject after the permitted jump-use count |
| `+0x50` | `mAirSpatk` | Reject after the permitted total air-special count |
| `+0x54` | `mAirSpatkEach` | Per-line air-special use limit |
| `+0x58` | `mHVRCount` | Team/resource count limit |
| `+0x60` | `mFlag` | Additional movement/team restrictions |

The limit object also has required, forbidden, and equality masks for the
character's flag words:

| Matcher range | Character state word |
|---:|---|
| `+0x64`..`+0x6C` | `mChrF` |
| `+0x70`..`+0x78` | `mAnmF` |
| `+0x7C`..`+0x84` | `mPrgF` |
| `+0x88`..`+0x90` | `mNotF` |
| `+0x94`..`+0x9C` | `mNo2F` |
| `+0xA0`..`+0xA8` | `mDmgF` |
| `+0xAC`..`+0xB4` | `mEtcF` |
| `+0xB8`..`+0xC0` | `mEt2F` |
| `+0xC4`..`+0xCC` | Combined program flags returned by a virtual/helper call |
| `+0xD0`..`+0xD8` | Aggregated team/ability flags |
| `+0xDC`..`+0xE4` | Secondary ability-state flags |
| `+0xE8`..`+0xF0` | `mUsrF` |

For each triplet, `Tst` requires any requested bit, `NTst` forbids any
requested bit, and `Eq` rejects when all requested bits are simultaneously
present. The last behavior is counterintuitive but is what the native branches
implement.

## Per-command predicate steps

`cChrCmd__Line__evaluateCommandSteps` (`0x140028240`) interprets more than pad
inputs. Relevant state opcodes include:

| Opcode | Native check |
|---:|---|
| `0x0B`/`0x0C` | Require/forbid a mask in `mChrF` |
| `0x0D`/`0x0E` | Require/forbid a mask in `mPrgF` |
| `0x0F`/`0x10` | Require/forbid a mask in `mNotF` |
| `0x11`/`0x12` | Require/forbid a mask in `mNo2F` |
| `0x13`/`0x14` | Require/forbid a mask in `mAnmF` |
| `0x15`/`0x16` | Require/forbid a mask in `mDmgF` |
| `0x17`/`0x18` | Require/forbid a mask in `mEtcF` |
| `0x19` | Current-action predicate |
| `0x1A`/`0x1B` | Require/forbid a team-state mask |
| `0x1C` | Team-life threshold |
| `0x1D` | Team aerial-exchange/TAC permission |
| `0x1E` | Required team-slot state |
| `0x1F` | Charge timer threshold |
| `0x20` | Jump-use count threshold |
| `0x22` | Character-specific limit callback |
| `0x23`..`0x26` | Success-state writes/directional state setup |
| `0x27`..`0x5D` | Character- and mode-specific predicates |

These predicates explain why supplying the visible motion and buttons can
still fail. Autotest currently skips them while constructing input because
they are state checks, but the native matcher evaluates them normally.

## Practical setup order

For an isolated runtime-command test:

1. Start from a neutral, actionable character state.
2. For matcher 0 only, jump before commands whose `commandState` is `1`.
3. Wait until the character has actually entered the airborne state; inputting
   the motion on the jump's first frame can precede the native state-bit change.
4. Satisfy gauge, height, jump-count, air-special-count, status, and flag gates.
5. Treat failures that remain as per-command predicate or character-specific
   callback failures rather than input-notation failures.

The mutable line entry's `+0x18` is padding/unnamed state in the current local
layout and is unrelated to the static command record's `+0x18` state selector.
