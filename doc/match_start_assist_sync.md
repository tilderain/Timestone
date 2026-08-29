# Match-Start Assist Character Selection

**Status:** Ghidra-verified and implemented, 2026-08-29. Addresses refer to
`ichneumon_sub/umvc3.exe.unpacked.exe` at preferred image base
`0x140000000`.

## Finding

The assist-button choice of the opening point character is resolved before the
current custom-netplay input epoch begins. Each process currently sees its own
team's physical input and neutral input for the remote team, so phase-2 setup
can commit different team orders on the two peers.

Retail online avoids this problem by synchronizing a selected member value
through `cNetChrSel::MsgCurrentMember`. It does not sample local assist buttons
when constructing the opening team order.

The safest custom-netplay equivalent is a small pre-phase-2 member-selection
handshake. Both selected values must be available before
`sAction__stepFightingCore` runs.

## Native Offline Path

`sAction__executeMatchPhase @ 0x140004B80` calls
`sAction__stepFightingCore @ 0x140006370` while entering match phase 2.

For each team, the offline branch performs this sequence:

```text
sCharacter__getTeam
SharedProperty__getDwordAt58
sMvc3Manager__getPlayerCharIdx
sPad__getPlayerInput_withSOCDClean
cChrInput__buttonRemap
```

The relevant instructions are:

```text
0x1400064FA  call sPad__getPlayerInput_withSOCDClean
0x140006507  call cChrInput__buttonRemap
0x140006533  test logical input bit 8
0x14000653B  test logical input bit 9
0x14000656A  call cChrTeam__findChrByMemberId
0x140006598  call sCharacter__requestOrApplyCharacterSwap
```

The resulting member choice is:

```cpp
int openingMember = 0;
if (logicalInput & 0x100)
    openingMember = 1;
if (logicalInput & 0x200)
    openingMember = 2;
```

Therefore assist 2 wins if both assist bits are held. The team-1 branch also
retains the native mode predicate at `sAction__queryGameModeUnlockState_6980`.
The byte returned by `sBattleSetting__getByte_at_885` can suppress assist-based
selection for both teams.

The selected member is validated with
`cChrTeam__findChrByMemberId @ 0x140021D70`. A missing member falls back to
member 0. This lookup uses the mutable member ID at `cChr+0x2DB0`; it is not a
character ID or stable roster-slot lookup.

The core then requests an immediate mode-1 transition from member 0 to the
selected member:

```cpp
sCharacter__requestOrApplyCharacterSwap(
    sCharacter, team, 0, openingMember, -1, 0, 1);
```

`sCharacterChangeOrder__requestOrApplySwap @ 0x14001F690` reports this as an
immediate transition. The wrapper at `0x14005A010` consequently publishes the
pending IDs through `cChrTeam__setPendingCharacterSwap`. After both teams are
processed, the core advances both change-order records and calls
`cChrTeam__commitPendingCharacterSwap @ 0x140021510`.

Commit swaps the two characters' mutable member IDs, updates
`cChrTeam::mpCurrentChr`, updates `sGameUI`, and clears character messages. The
opening selection is thus authoritative team state, not presentation-only
input.

## Native Online Path

The same function has a separate branch when `g__isOnlineBattle` is true. It
does not read `sPad`. For each team it reads:

```text
sBattleSetting__getTeamHistoryEntry @ 0x14024D2D0
```

The two values are stored at:

```text
sBattleSetting + 0x924 + teamIndex * 4
```

The only setter is `sBattleSetting__setTeamHistoryEntry @ 0x14024E480`. Its
caller at `0x140262900` is dispatched specifically for the runtime DTI
`cNetChrSel::MsgCurrentMember`. That handler is now named
`cNetChrSel__applyCurrentMemberMessage` in Ghidra.

This establishes the native synchronization model:

```text
exchange current-member choice during character-select protocol
    -> store one member ID per team in sBattleSetting
    -> enter phase 2
    -> construct and commit both opening team orders from shared values
```

## Current Custom-Netplay Gap

The custom ENet timeline is established at the intro-ready barrier after phase
2 setup has completed. `FrameStepper` marks local readiness only after observing
`step == 2 && pendingStep < 0`. By then `sAction__stepFightingCore` has already
sampled the opening assist buttons and committed both teams.

During custom netplay, `hkGetsCurrentInput_withSOCD` deliberately returns zero
for the non-owned player. As a result:

```text
host process:   team 0 sees host input, team 1 sees neutral
client process: team 0 sees neutral,     team 1 sees client input
```

If either player holds an assist at match start, the owning process selects the
reserve while the other process initially keeps that team on member 0.

Normal rollback input synchronization cannot repair this. The decision occurs
before frame zero, before input history is retained, and before snapshots are
created.

## Recommended Design

### 1. Add a pre-phase-2 handshake

In the `FrameStepper` hook, detect an action with `pendingStep == 2` before
calling the original `sMvc3Main::move`. At this point the phase-2 entry routine
and `sAction__stepFightingCore` have not run.

Each peer should sample only its owned team's current logical assist state and
freeze it as member `0`, `1`, or `2`. Exchange at least:

```cpp
struct OpeningMemberPacket {
    uint32_t type;
    uint32_t battleGeneration;
    uint8_t team;
    uint8_t member;
    uint8_t flags;
    uint8_t reserved;
};
```

The existing `BattleReadyPacket` could instead be extended with an opening
member and two additional flags. Either form needs a ready/release
acknowledgement, like the current intro barrier, so one peer cannot enter phase
2 after merely transmitting a choice that the other peer has not observed.

The choice should be captured once when the pending phase-2 transition is first
observed. Continuously changing an already advertised choice would introduce a
race between input polling and packet receipt.

### 2. Let the native core apply both synchronized choices

Once both peers have acknowledged the two choices, allow the pending transition
to execute. The smallest hook is the existing `cChrInput__buttonRemap` hook:

- Recognize the call from `sAction__stepFightingCore` by return address
  `0x14000650C`.
- Use its `playerIndex` argument as the team index.
- Return `0`, `0x100`, or `0x200` from the synchronized member value.
- Let the rest of `sAction__stepFightingCore` perform native mode checks,
  member validation, transition publication, and commit.

This is preferable to manually writing `cChrTeam` fields. It preserves native
member-ID exchange, active-pointer updates, UI updates, message resets, and any
future side effects in the normal transition path.

The scoped override must only apply to this phase-2 callsite. Ordinary gameplay
calls to `cChrInput__buttonRemap` must continue using the rollback timeline.

### 3. Keep the existing intro barrier

After phase 2 has run identically, retain the current battle-ready barrier for
round-seed synchronization, input queue reset, frame-epoch reset, and snapshot
initialization. The new handshake establishes opening team order; it does not
replace those later responsibilities.

### 4. Reset on every battle generation

Clear local choice, remote choice, and both acknowledgement flags whenever
`g_battleGeneration` changes. Cover initial launch, full match restart, native
phase-12 rematch, and disconnect/abort paths. Ignore packets for stale
generations.

## Rejected Shortcuts

### Correcting the remote team at the intro barrier

Applying the missing swap after phase 2 is tempting, but the core continues
with active-character status, camera, ready-character references, sound, and
other setup after committing the opening member. Reconciliation at the later
barrier would make the final active pointer agree without proving that those
earlier side effects were based on the same character.

There is also an identity trap: member IDs are exchanged by the first commit.
Sending "member 1" and blindly requesting another `0 -> 1` transition toggles
the already-correct peer back to the original point character.

### Enabling the native online predicate

Writing `sBattleSetting+0x924/+0x928` alone has no effect in the offline branch.
Forcing `g__isOnlineBattle` true would activate native netpad, room, UI,
session, timing, and teardown behavior that the ENet implementation does not
initialize. The custom hook should reproduce only the synchronized member
input at this callsite.

### Writing `mpCurrentChr` directly

This omits mutable member-ID exchange, `sGameUI` updates, pending transition
cleanup, and character message resets. Use the native opening transition path.

## Validation

Log the following immediately before and after phase-2 setup on both peers:

```text
battle generation
local and remote advertised opening member
team index
logical mask returned at 0x14000650C
cChrTeam +0x48 active pointer
cChrTeam +0x9C/+0xA0/+0xA4 pending mode/current/next IDs
all three cChr +0x2DB0 member IDs
sGameUI selected team member
```

Test neutral, assist 1, assist 2, and both assists for each player, including
both players selecting reserves simultaneously. The active pointer and all
three member IDs must agree before the first intro update and before rollback
frame zero.

## Ghidra Updates

- Renamed `cChrTeam__findChrByCharId_1D70` to
  `cChrTeam__findChrByMemberId` and corrected its signature.
- Renamed `FUN_140262900` to
  `cNetChrSel__applyCurrentMemberMessage`.
- Added explanatory comments to those functions and
  `sAction__stepFightingCore`.

## Implementation

The implementation uses the recommended design:

- `BattleReadyPacket` carries the sender's owned-team opening member.
- `BATTLE_SYNC_OPENING_READY` and `BATTLE_SYNC_OPENING_RELEASE` provide a
  two-sided acknowledgement before phase 2.
- `FrameStepper` freezes a pending phase-2 transition until both choices are
  acknowledged.
- `Controller_CaptureOpeningMember` resolves the owning player's current input
  through native button remapping.
- `hkbuttonRemap` substitutes `0`, `0x100`, or `0x200` only for the native
  opening call returning to `0x14000650C`.
- The normal native swap, validation, UI update, and phase-2 setup remain
  authoritative.
