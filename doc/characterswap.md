# Mid-Match Character Loading and Replacement

**Status:** Ghidra-verified investigation for the current
`umvc3.exe.unpacked.exe` build. Descriptive names are inferred where noted.

## Summary

The game has a reasonably direct native path for asynchronously loading and
creating a roster character without returning to character select. The core
sequence is:

```text
CharacterId__getDTI
    -> sCharacter__loadArcSet_CS
    -> wait for sCharacter__areRequestedArchivesLoaded
    -> generic character factory at 0x140058AD0
```

Creating an additional character is easier than replacing a live team member.
A live replacement must also retire the old fighter, repair team and UI state,
clear dependent shots/effects, and reset rollback history. The safest first
implementation is a synchronized, training-reset-style replacement barrier
rather than a seamless speculative swap during active combat.

## Character DTI Lookup

```text
0x140058FB0: CharacterId__getDTI
```

This function accepts character IDs below `0x3C`, obtains the corresponding
character name, prefixes it with `u`, and resolves the class through
`MtDTI__findByName`.

Conceptually:

```cpp
MtDTI* GetCharacterDTI(uint32_t characterId)
{
    if (characterId >= 60)
        return nullptr;

    return MtDTI::findByName("u" + CharacterName(characterId));
}
```

This avoids maintaining a separate table of concrete constructor addresses.

## Character Archive Cache

The character archive table begins at:

```text
sCharacter + 0x600
```

The native loader stores up to nine records for each archive category. A load
request succeeds if the matching record is already present or a free record is
available.

### High-Level Loader

```text
0x140059600: sCharacter__loadArcSet_CS
```

This function enters the `sCharacter` critical section and requests the archive
categories needed by a character:

| Category | Resource path |
|---|---|
| `1` | `chr\\archive\\%04d_%02d` |
| `2` | `chr\\archive\\%04d_cmn` |
| `3` | `chr\\archive\\%04d_param` |
| `4` | `sound\\se\\chr\\archive\\%04d_%02d` |
| `5` | Character pair/team sound archive |
| `6` | Character pair/team sound archive |

Its approximate signature is:

```cpp
bool sCharacter::loadArcSet(
    uint32_t characterId,
    uint32_t drawMode,
    uint32_t variant,
    int teamSoundVariantA,
    int teamSoundVariantB);
```

Categories `1` through `4` are requested for every valid character. Categories
`5` and `6` are requested when their corresponding sound variants are positive.

The lower-level critical-section wrapper is:

```text
0x140059770: sCharacter__loadArc_locked_59770
```

It requests a single archive category.

### Core Archive Request

```text
0x14001EF00: PossibleArcLoadingFunction
```

This function calculates the archive resource ID, builds its path, checks for
an existing matching cache entry, finds a free slot, and calls
`cChrArchive__queueLoad`.

It supports categories `0` through `6`. Category zero is the common archive:

```text
chr\\archive\\0000_00
```

### Asynchronous Loading

Relevant functions are:

```text
0x14001ED80: cChrArchive__queueLoad
0x14001F2E0: cChrArchive__updateLoad
0x14001E6E0: cChrSlot__loadArchivesIntoSlot
```

`cChrArchive__queueLoad` either asks `sResource` to load immediately or marks
the cache record pending. `sCharacter__move` calls
`cChrArchive__updateLoad` on normal unpaused frames, allowing pending records to
begin loading through the existing resource manager.

Do not create a character from an asynchronous loader thread. Queue the load,
poll from the game thread, and create the fighter at a controlled frame
boundary.

### Readiness

```text
0x140059440: sCharacter__areRequestedArchivesLoaded
0x140058F40: sCharacter__queryArcLoadState
0x14001EA20: cChrSlot__getLoadState
```

`cChrSlot__getLoadState` returns the following observed states:

| State | Meaning |
|---:|---|
| `0` | No resource pointers |
| `1` | Deferred request is pending |
| `2` | Loading |
| `3` | Resource has a failure/cancel flag |
| `4` | Resource has failure/cancel plus release flag |
| `5` | Loaded and usable |
| `6` | Resource exists but is not in a recognized ready state |

`sCharacter__areRequestedArchivesLoaded` expects required categories to report
state `5`.

## Generic Character Factory

```text
0x140058AD0: uDoctorStrange__setup_8AD0
```

The current name is misleading. This is a generic factory used for the roster,
model viewer, normal match construction, secondary forms, and character-owned
helpers. A more accurate descriptive name would be:

```text
sCharacter__createCharacterFromDTI
```

Its approximate signature is:

```cpp
cChr* sCharacter::createCharacterFromDTI(
    MtDTI* characterDti,
    int drawMode,
    int teamId,
    int playerId,
    int settingSlot,
    int cpuMode,
    int language,
    uint32_t characterFlags,
    cChr* parentCharacter,
    int unitMoveLine);
```

Argument names remain partly inferred, but Ghidra establishes the following
behavior:

1. Return null if the DTI is null.
2. Invoke the DTI's instance factory.
3. Get and store the DTI's character ID.
4. Apply the requested character flags.
5. Copy parent-character information when a parent is supplied.
6. Set body/draw mode.
7. Register the fighter with the requested `cChrTeam`.
8. Allocate a unique runtime fighter ID.
9. Set player ID, team ID, and `sBattleSetting` slot ID.
10. Add the fighter to the requested `sUnit` move line.
11. For normal fighters, copy the selected character ID from `sBattleSetting`.
12. Create the CPU/controller object.
13. Add the character action object to `sUnit` line `0x15`.
14. Set the character language.

### Team Registration

```text
0x140057440: cChr__registerWithTeamAndAssignIdentity
0x140020550: cChrTeam__insertCharacterAndAssignMemberIds
0x1400590B0: sCharacter__allocateUniqueId
```

Registration links the fighter into its team's character lists, assigns member
IDs, and stores:

- Player ID.
- Team ID.
- Unique runtime fighter ID.
- `sBattleSetting` slot ID.

The runtime unique ID counter belongs to `sCharacter`; it is unrelated to the
mouse/UI `sId` singleton.

## Runtime-Spawning Precedent

```text
0x14020FC30: aModelViewer__spawnSelectedCharacterWhenLoaded
```

The model viewer proves that the game can wait for character archives and then
instantiate a roster character outside the character-select match-construction
path.

Its sequence is approximately:

```cpp
if (sCharacter->allArchivesReady()) {
    if (sCharacter->areRequestedArchivesLoaded(
            characterId, variant, -1, 0, 0)) {
        MtDTI* dti = CharacterId__getDTI(characterId);

        cChr* chr = sCharacter->createCharacterFromDTI(
            dti,
            variant,
            0,
            -1,
            0,
            0,
            1,
            0x10040,
            nullptr,
            8);

        if (chr)
            chr->mChrF |= 1;
    }
}
```

These parameters create a model-viewer object rather than a normal combat team
member. A combat replacement must supply valid team, player, and battle-setting
slot values.

## `sBattleSetting` Dependency

The generic factory records a `settingSlot` in the fighter. During normal setup,
`uCharacter__setup_base` reads that slot from `sBattleSetting` to initialize:

- Archive/variant values.
- Attack and damage settings.
- Character sound settings.
- Character DTI ID.
- One-play values.
- Owner thread/player information.
- Other slot-specific flags.

Therefore, constructing a character with an arbitrary or stale slot index is
unsafe. The easiest supported model is to reserve or reuse one of the six native
battle-setting character slots.

A replacement operation should update the slot's complete record, including:

- Character ID.
- Team ID.
- Player/member position.
- Draw mode/color.
- Variant.
- Assist type.
- Life or status overrides.
- DTI-related fields and flags.

The normal character-select path in
`aChrSelect__commitCharSelectionForSlot` writes these values, updates `sGameUI`,
marks network selection fields dirty, releases stale archives, and queues the
new archive set.

There are only six ordinary battle slots. Adding a permanent fourth member to a
team would require broader changes to battle rules, team iteration, UI, and
possibly fixed-size arrays. Replacing an existing slot is substantially more
realistic.

## Character Setup

The generic factory allocates, registers, and schedules the fighter, but the
concrete character setup still performs substantial initialization.

```text
0x1400570D0: uCharacter__setup_base
```

Each roster class has an override such as:

```text
uRyu__setup
uDante__setup
uDoctorStrange__setup
```

Setup work includes:

- Shared actor/model initialization.
- Collision-node setup.
- Reading the battle-setting slot.
- Assigning model and character resources.
- Allocating command buffers.
- Registering character-specific callbacks.
- Creating weapons and helper characters.
- Initializing character-specific state.

The native factory adds the new object to `sUnit`, so the preferred approach is
to let the normal unit lifecycle invoke setup. Calling concrete setup functions
manually is risky until that lifecycle is fully verified.

## Per-Character Resource Loading

```text
0x140057EF0: uCharacter__loadCharacterResources
```

Once a concrete fighter exists, it requests additional resources by character
name, including:

- `anmchr`
- common animation commands
- team-down animation commands
- attack information
- attack collision
- body collision
- base actions and easy-mode base actions
- special-command data and easy-mode data
- combo-command data and easy-mode data
- status and profile data
- character CPU data

These resource references are stored in the concrete fighter and normally live
for the fighter's match lifetime. This is another reason changing an existing
fighter's character ID or vtable in place is not viable.

## Retiring the Old Fighter

A live replacement cannot safely overwrite the existing fighter. The old
object participates in:

- Two team-linked structures.
- The team's active-character pointer.
- `sGameUI` team-character state.
- CPU and action units.
- Weapons and helper forms.
- Collision nodes and hit primitives.
- Shots and effects that retain owner pointers.
- Character sound references.
- Concrete character resource references.
- Rollback snapshot regions and pointer relations.

### Native Kill

```text
0x1400564D0: uCharacter__kill
```

The native kill path detaches the character's linked references, kills up to 21
owned effect pointers, and delegates to the normal unit/effect kill path.

### Cleanup

```text
0x140054FE0: cChr__cleanup
0x140055830: cChr__dtor
```

Cleanup releases character resources and tears down arrays, collision objects,
input state, commands, status, abilities, CPU state, sound references, and other
owned subobjects.

Do not call the scalar destructor directly while the object is still registered
with `sUnit`, `cChrTeam`, collision systems, UI state, or rollback snapshots.
Use the native kill path and allow the unit manager to retire it.

## Training Reset as a Replacement Boundary

```text
0x140009DF0: sAction__stepTrainingReset
```

The training reset state machine already performs much of the stabilization
needed around a character replacement:

- Purges stale `sUnit` categories.
- Clears shots and effects.
- Rebuilds the stage body.
- Waits for character archives to become ready.
- Resets team positions.
- Rebuilds helper characters and audio.
- Releases motion state after reset.

At reset frame three it waits for both the stage body and character archive
subsystems to report ready before advancing.

```text
0x14024B7E0: sBattleSetting__rebuildSubChrSummonsAndAudio
```

This helper iterates active `sBattleSetting` slots and invokes the generic
character factory, then rebuilds related sound objects. It is called by training
reset and normal battle-round initialization.

This makes a training-reset-style pause the safest first implementation target.

## Suggested Prototype

### Phase 1: Preload

Queue the desired character's archives while the current match continues:

```cpp
bool RequestCharacterArchives(
    int characterId,
    int drawMode,
    int variant,
    int teamSoundA,
    int teamSoundB)
{
    return sCharacter__loadArcSet_CS(
        sCharacter,
        characterId,
        drawMode,
        variant,
        teamSoundA,
        teamSoundB);
}
```

Poll readiness from the game thread. Do not block the resource thread or spawn
from a loader callback.

### Phase 2: Synchronized Replacement Barrier

At an agreed safe frame:

1. Pause shared simulation.
2. Stop accepting rollback requests.
3. Confirm the new archive set is loaded on every peer.
4. Purge or retire shots and effects owned by the outgoing fighter.
5. Invoke the outgoing fighter's native kill path.
6. Allow `sUnit` to retire stale objects.
7. Update the complete `sBattleSetting` slot.
8. Update corresponding `sGameUI` slot data.
9. Resolve the new character DTI.
10. Invoke the generic factory using the reused slot.
11. Let the normal `sUnit` lifecycle run concrete setup.
12. Repair active-character and member IDs if normal construction did not do so.
13. Rebuild helper forms, character audio, and team UI.
14. Rebuild rollback regions.
15. Clear all snapshots from the old object graph.
16. Resume from a new synchronized timeline epoch.

The initial prototype should replace a benched team member, not the currently
active fighter. This reduces immediate dependencies on camera, collision,
attacks, active shots, and transition state.

## Netplay and Rollback

Archive loading and object creation are asynchronous, externally visible side
effects. They must not execute speculatively during rollback.

A network character-change command should include at least:

```cpp
struct CharacterChangeRequest {
    uint32_t epoch;
    uint32_t barrierFrame;
    uint8_t team;
    uint8_t slot;
    uint8_t characterId;
    uint8_t drawMode;
    uint8_t variant;
    uint8_t assist;
};
```

Recommended protocol:

1. Send the request reliably.
2. Both peers validate the same target slot and character data.
3. Both peers queue the required archives.
4. Each peer reports local resource readiness.
5. Select a barrier frame after all peers are ready.
6. Stop simulation at the barrier.
7. Perform the same replacement operation on both peers.
8. Increment the simulation epoch.
9. Clear rollback and input-prediction history before the barrier.
10. Resume synchronized simulation.

Do not roll back across the replacement. Old snapshots contain:

- The old fighter address and concrete DTI size.
- Old vtables and resource pointers.
- Old team links.
- Old collision ownership.
- Old shot/effect owner pointers.
- Old snapshot-region metadata.

Restoring one of those snapshots after replacement would corrupt the new object
graph.

## Risks and Open Questions

The following need runtime validation before a production implementation:

- Exact meanings of all generic factory arguments.
- Which `sUnit` line should be used for a normal replacement fighter.
- Whether concrete setup runs automatically in every mid-match game phase.
- Which slot fields must be copied or reset for each mode.
- How the team active-character pointer is repaired when replacing an active
  fighter.
- Which shots, effects, and character-specific helper objects must be killed
  before retirement.
- Whether archive cache capacity must be freed before requesting a new roster
  member.
- Whether loading while combat is active causes unacceptable resource-thread or
  rendering stalls.
- Which UI and sound rebuild calls are mandatory outside training mode.
- Whether any character-specific singleton or static state survives fighter
  destruction.

## Conclusion

The easy native part is loading and creating a character:

```text
CharacterId__getDTI
    -> sCharacter__loadArcSet_CS
    -> wait for state 5
    -> generic factory at 0x140058AD0
```

The difficult part is replacing a live combat member while preserving ownership
and deterministic netplay. The recommended first implementation is to reuse an
existing `sBattleSetting` slot, preload asynchronously, replace at a synchronized
training-reset-style barrier, rebuild rollback regions, and begin a new timeline
epoch.
