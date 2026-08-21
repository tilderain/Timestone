# Missing `cChr` Fields

## Summary

| Range | Identity | Gameplay relevance | Rollback policy |
|---|---|---|---|
| `+0x1408..+0x1438` | Seven owned target-processing tables | High | Keep live pointers; restore payload only when allocation still matches |
| `+0x1440..+0x1443` | Seven packed table counts | High, structural | Restore each nibble only with its matching table |
| `+0x1480..+0x1487` | `mHipRate`, `mHipRateOfs` | High; affects action Y placement | Explicitly restore `{0x1480, 0x08}` |
| `+0x2D80..+0x2D8F` | Per-character `MtXorShift` | High | Continue explicit restoration |
| `+0x2D90` | Write-only setup marker | None after setup | Keep live; no explicit restoration |
| `+0x2D91..+0x2D93` | Likely padding | None known | Ignore |
| `+0x2D94..+0x2D97` | Per-character frame counter | High | Explicitly restore `{0x2D94, 0x04}` |
| `+0x2D98..+0x2D9B` | Character archetype ID | High but immutable | Keep live; validate against DTI |
| `+0x6340..+0x635F` | Two inline combat vectors, not `MtArray`s | Likely high | Continue scalar restoration |
| `+0x645C..+0x645F` | Alignment padding | None known | Do not restore |
| `+0x6460` | Embedded `CRITICAL_SECTION` | Synchronization only | Always keep live |
| `+0x6718` | Last-attacker/linked-character pointer | High | Restore a validated `cChr` relationship |
| `+0x6720` | Capture/throw partner A | High | Restore a validated relationship |
| `+0x6728` | Capture/throw partner B | High | Restore a validated relationship |
| `+0x6730` | Selected throw-pair partner | High, partly derived | Restore or rebuild after validating partner |
| `+0x6738` | Character callback count | Setup-static | Keep live |
| `+0x6740` | Character-specific callback table | High infrastructure, setup-static | Keep live; never copy the historical pointer |
| `+0x6748..+0x67EF` | 21 effect-owner slots | Mixed, indirect gameplay risk | Validate or remap per slot; never raw-copy |
| `+0x67F0` | `uChrCpu*` | High in CPU modes | Keep the validated live controller; restore VM state separately |

## Corrected Classifications

- `+0x6340..+0x635F` are not demonstrated `MtArray` headers. They behave as two inline 16-byte vectors.
- `+0x2D90..+0x2D9B` is not one RNG-control block. It contains a setup marker, padding, a mutable frame counter, and immutable character identity.
- `+0x645C..+0x645F` appears to be alignment padding, not shot gameplay state.
- `+0x6740` is a character-specific function-dispatch table, not an effect allocation.
- The relationship pointers at `+0x6718..+0x6730` are gameplay-authoritative.

## Target Tables

### `cChr+0x1408..+0x1438`

Seven owned heap-table pointers:

| Offset | Table | Entry stride |
|---|---:|---:|
| `+0x1408` | 0 | `0xBB0` |
| `+0x1410` | 1 | `0xBB0` |
| `+0x1418` | 2 | `0xB90` |
| `+0x1420` | 3 | `0xB90` |
| `+0x1428` | 4 | `0xBB0` |
| `+0x1430` | 5 | `0xBB0` |
| `+0x1438` | 6 | `0xB80` |

Evidence:

- `uCharacter__processTarget1 @ 0x140200F50` invokes virtual `+0x40` on each active entry.
- `uCharacter__processTarget2 @ 0x140200430` invokes virtual `+0x88`.
- `uCharacter__processTarget3 @ 0x140200600` invokes virtual `+0x88`.
- `uActorGameModel__releaseOwnedResources @ 0x1401FF6B0` destroys or frees all seven allocations.
- The processing functions run during ordinary model/character processing.

The entries are active per-frame objects rather than passive render caches. Their callbacks may affect model or action state consumed by collision and character processing.

Rollback policy:

- Keep the current live allocation pointer.
- Restore table contents only when the owner slot still identifies the captured allocation.
- If an allocation changed, keep both its live pointer and live count.
- Never restore a saved pointer without restoring or reconstructing its allocation.

### `cChr+0x1440..+0x1443`

These four bytes contain seven packed four-bit counts used as bounds for the target tables.

- `uCharacter__processTarget1/2/3` read each nibble before indexing and invoking a virtual.
- A stale count can invoke an entry beyond the initialized portion of the live allocation.
- The counts are initialized with the `uActorGameModel` tail and are structurally tied to the seven pointers.

The counts appear more like setup/resource-derived capacities than frame counters, but native code consumes them during per-frame simulation. Match-time mutation has not been established.

Rollback policy:

- Do not restore `+0x1440` independently.
- Restore each nibble only when restoring its corresponding table coherently.
- Otherwise retain or rebuild the nibble from the live allocation.
- A blanket `{0x1440, 4}` scalar copy is unsafe.

## Hip Adjustment

### `cChr+0x1480` `mHipRate`

### `cChr+0x1484` `mHipRateOfs`

- Exposed by `uActorShareModel__createProperty @ 0x140201AC0`.
- Written by `cChrStatus__setMode @ 0x140031250` from mode-resource fields `+0x24/+0x28`.
- Read by `uActorShareModel__adjustAndStoreVerticalVector @ 0x14004C910`.
- `uCharacter__getTeamState @ 0x14004D0B0` installs that function as an action-state callback.
- Operation modes 2 and 3 multiply an action vector's Y value by `mHipRate`; mode 2 also adds `mHipRateOfs`.

This is gameplay-relevant action placement, not merely draw-time hip scaling. It can affect character placement and downstream joint-based hitbox or attachment positions.

The packed default `{1.0f, 0.0f}` is `0x000000003F800000`, which satisfies the current `IsLikelyPointer()` heuristic and can therefore be incorrectly skipped.

Rollback policy:

```cpp
{0x1480, 0x08}
```

Reapply it after any status/model repair that can call `cChrStatus__setMode`.

## RNG-Adjacent State

### `cChr+0x2D80..+0x2D8F`

This is the actual per-character `MtXorShift` state.

- Initialized by `MtXorShift__init` in `cChr__cChr @ 0x1400542D0`.
- `MtXorShift__dispatcherNext @ 0x14014EB80` selects `character+0x2D80`.
- It is gameplay-authoritative and already explicitly restored.

### `cChr+0x2D90`

| Property | Finding |
|---|---|
| Type | `bool` |
| Constructor value | `false` at `0x140054407` |
| Later write | Set to `true` at `0x1400573C9` |
| Writer | `uCharacter__setup_base @ 0x1400570D0` |
| Timing | Near the end of common character setup |
| Native readers | None found |
| Further writers | None found |
| RNG use | None |
| Match-time mutation | None found |
| Gameplay relevance | None after setup |
| Rollback policy | Keep live; no explicit restoration needed |

### Confirmed Evidence

An exhaustive native executable search found only two character-relative accesses:

```asm
; cChr constructor
140054407  mov byte ptr [r14+0x2D90], 0

; uCharacter__setup_base
1400573C9  mov byte ptr [rdi+0x2D90], 1
```

The second write occurs immediately before common setup creates the character and projectile hitbox models through virtual slots `+0x298` and `+0x2A0`.

No byte, word, dword, qword, overlapping, or address-taking access was found around `+0x2D90`. The per-character RNG dispatchers operate exclusively on `+0x2D80..+0x2D8F` and never inspect this flag.

### Interpretation

The current `mbRandomize` name is misleading or unverified. The observed native behavior supports only a write-only setup-completion marker:

```text
false: object constructed but common setup incomplete
true:  common character setup reached its final initialization stage
```

The access behavior is high-confidence. The exact semantic name is medium-confidence because no native reader exposes what the original source intended the flag to represent.

### Rollback Recommendation

Rollback snapshots occur after character setup, when this value is permanently `true`. It is not match timeline state and does not need explicit scalar restoration. Keep the live value and do not add `+0x2D90` to the scalar restore ranges.

### `cChr+0x2D91..+0x2D93`

No native readers or writers beyond initialization were demonstrated. These bytes are likely padding and have no known gameplay relevance.

### `cChr+0x2D94..+0x2D97`

This is a mutable per-character frame counter.

- Initialized to zero in `cChr__cChr`.
- Incremented at the beginning of `uCharacter__moveAfter @ 0x14004C360`.
- Advances during the phase-3 character callback.

It is authoritative per-character simulation time even though its downstream consumers have not been fully named.

Rollback policy:

```cpp
{0x2D94, 0x04}
```

### `cChr+0x2D98..+0x2D9B`

This is the immutable character archetype ID.

- Written once by `cChr__setSubobject2D78Field20 @ 0x14005A380`.
- Sourced from `MtDTI__getCharacterId` in `sCharacter__createCharacterFromDTI`.
- Returned by `cChr__getCharId @ 0x140004AD0`.
- Used by combat logic for character-specific behavior, including damage and projectile exceptions.

It is gameplay-relevant but setup-static. Keep the live value and validate it against the expected DTI rather than restoring it from the timeline.

## Inline Combat Vectors

### `cChr+0x6340..+0x635F`

The previous `MtArray` classification is unsupported.

- `cChr__cChr` invokes `0x14000BC10` at `+0x6340` and `+0x6350`.
- The same helper is invoked for `mPos`, `mMove`, and `mMoveAux`.
- The helper only clears the fourth dword of a 16-byte object.
- No destructor call releases either field.
- `cChr__cleanup` does not treat them as allocations.

They are consistent with two inline vector values, probably contact/damage working vectors. Their exact semantics remain unresolved, but their placement immediately before damage/combo state makes gameplay relevance likely.

Rollback policy:

```cpp
{0x6340, 0x10},
{0x6350, 0x10},
```

Continue scalar restoration. Do not preserve them as pointers, and describe them as unnamed combat vectors rather than `MtArray` headers.

## Synchronization Boundary

### `cChr+0x645C..+0x645F`

Constructor disassembly shows no field store at `+0x645C`. Instead, it loads `character+0x6460` and calls the `InitializeCriticalSection` wrapper. Cleanup passes the same address to `DeleteCriticalSection`.

These four bytes are alignment padding before the critical section. Leave them untouched and do not extend a scalar range through `+0x6460`.

### `cChr+0x6460`

This is an embedded Windows `CRITICAL_SECTION`.

The lock bytes are synchronization infrastructure and never rollback state. The exact neighboring data protected by the lock has not been fully enumerated, but the lock itself must always remain live.

## Relationship Fields

### `cChr+0x6710`

Context discovered during the audit: this is a current defender/target link. `cChr__commitAttackContactResult @ 0x140044350` stores the defender here when contact is committed.

### `cChr+0x6718`

Reciprocal attacker/linked-character pointer.

- `cChr__commitAttackContactResult` stores the attacker into `defender+0x6718`.
- `cChr__processDamageReactionState @ 0x14003D8E0` reads it for damage, voice, and team decisions.
- `uCharacterPost__sync @ 0x14005F2B0` validates liveness and uses it during character alignment.

### `cChr+0x6720`

First throw/capture alignment partner.

- Set and cleared by AnmChr commands and `uCharacterPost__sync`.
- `cChr__validateLinkedCharacter6720 @ 0x140054060` controls reciprocal flags and assist/partner transitions using this pointer.
- Used as the selected partner for one throw-alignment mode.

### `cChr+0x6728`

Second throw/capture alignment partner.

- Written alongside `+0x6718` by `cChr__linkAssistOrPartner @ 0x140053F40`.
- Set by AnmChr commands.
- Used by `uCharacterPost__sync` for the alternate capture/alignment mode.

### `cChr+0x6730`

Selected/current throw-pair pointer.

- Exposed as `mpThrowPairChr` by `cChr__createProperty`.
- `uCharacterPost__sync` copies either `+0x6720` or `+0x6728` here after selecting the active alignment mode.
- Cleared if the target is no longer alive.

All four requested relationship fields are strongly gameplay-authoritative. `+0x6730` is partly derived, but stale prediction-time relationships can still affect throws, captures, assist links, damage reactions, and alignment.

Rollback policy:

- Restore snapshot relationships only when the target still exists as the same live `uCharacter`/`cChr`.
- Validate address accessibility, callable vtable, DTI compatibility, and lifecycle.
- Preserve reciprocal invariants where applicable.
- Do not restrict valid targets to team-roster characters; native helper/AnmChr paths can involve non-roster character objects.
- If validation fails, clear the relationship and reciprocal flags rather than keeping prediction-time links.

## Character Dispatch Table

### `cChr+0x6738`

Number of entries in a character-specific callback table.

### `cChr+0x6740`

Heap allocation containing character-specific function pointers, not an effect allocation.

For example, `uTrish__setup` sets `+0x6738` to 6, allocates `6 * 8` bytes into `+0x6740`, and fills the allocation with character-specific callbacks. Other concrete character setup functions use the same mechanism with their own counts and callbacks.

The callbacks are gameplay infrastructure for character-specific shot/action behavior, but the allocation is process-local setup state rather than timeline state.

Rollback policy:

- Keep `+0x6738` and `+0x6740` live.
- Never restore a historical table pointer.
- Never copy historical function-pointer payload.
- Optionally validate the live count against the concrete character DTI.

## Indexed Effect Slots

### `cChr+0x6748..+0x67EF`

Twenty-one inline `MtObject*` effect-owner slots.

- `MtObject__getIndexedField_at_26440 @ 0x140056010` reads `+0x6748 + index*8`.
- `MtObject__replaceSlot_6F30 @ 0x140056F30` kills the previous object through virtual `+0x78` and installs its replacement.
- `cChr__cleanup` iterates all 21 slots and kills live objects.
- At least 55 native callsites use the slot replacement function.
- `sStage__spawnCharacterStageEffect @ 0x1401F1030` stores a character-specific stage aura in slot 10.
- `uTrish__tick @ 0x1400E2A10` uses slots 14 and 15 for persistent weapon/install effects.
- Concrete character and AnmChr code uses additional indices.

Most targets are presentation-oriented `uGameEffect` objects, but slot occupancy participates in character-specific installs, lifetime and replacement, owner attachment, category-13 processing, and allocator pressure. Some character code dispatches effect virtuals directly without validating DTI.

Stale occupancy can therefore suppress required effects, invoke recycled objects, or exhaust shared allocators even when the effect itself is not authoritative scalar gameplay state.

Rollback policy:

- Never raw-copy historical pointers.
- Validate live slots for accessibility, callable vtable, expected effect DTI/family, liveness, and owner relationship.
- Handle saved-null versus saved-occupied state with per-slot reconstruction semantics where needed.
- Do not blanket-preserve every live slot.
- Treat slots 14 and 15 carefully because unconditional killing causes replacement accumulation and allocator exhaustion.
- Slot 1 may remain an explicit teardown slot only if runtime evidence continues to support that policy.
- Prefer remapping to reconstructed effect recipes for snapshot-authoritative occupancy.

## CPU Controller

### `cChr+0x67F0`

This is the character's `uChrCpu*`.

- Written by `cChr__setCpuController @ 0x14005A2A0`.
- Created with the character and registered in category 21 by `sCharacter__createCharacterFromDTI`.
- `uChrCpu__sync @ 0x140120E90` writes itself back into `cChr+0x67F0`.
- `uChrCpu__moveAfter @ 0x140120AE0` performs CPU reaction/script processing.
- The controller maintains owner and target references.

The pointer is process-local identity, while the controller's VM and decision state are gameplay-authoritative in CPU and training-script modes.

Rollback policy:

- Keep the live controller pointer.
- Validate that its owner points back to this character.
- Restore mutable controller VM/script state separately.
- Never restore a raw historical `uChrCpu*`.
- Give this field an explicit policy rather than relying on `IsLikelyPointer()`.
