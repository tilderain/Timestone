# Native Area and Scene Management

**Status: Native reference. Reviewed 2026-07-25.** Addresses refer to the
unpacked executable at preferred image base `0x140000000`. This document
describes native MT Framework area management, not a plugin-owned state
machine.

## Summary

Classes such as `aTitle`, `aChrSelectBlank`, `aStage000`, `aNextBattle`, and
`aAction` are `cArea` descendants managed by the global `sArea` singleton.
`sArea` does not merely hold one current scene. It builds and owns an active
chain of as many as eight area objects from a target DTI's inheritance path.

An area transition therefore has two phases:

1. A caller queues a target `MtDTI` through virtual slot `+0x50`, implemented
   by `sArea__requestAreaTransition` at `0x140545350`.
2. `sArea__updateChildren` waits until the current leaf has initialized, then
   applies the target with virtual slot `+0x58`, implemented by
   `sArea__applyPendingAreaTransition` at `0x140545640`.

The transition implementation retains the common prefix between the current
and requested DTI chains. It finalizes and deletes only the divergent old
suffix, then constructs only the missing new suffix. Calls that appear to
"set the area to `aTitle`" or "set the area to `aStage000`" consequently select
an inherited chain, not necessarily one isolated object.

## Type Hierarchy

The concrete MVC scene classes derive through this native base:

```text
MtObject
  cArea              size 0x50
    aMvcArea          size 0x58
      concrete a* areas
```

Verified concrete sizes include:

| Type | Size | Principal role |
|---|---:|---|
| `aAction` | `0x70` | Battle gameplay loading and per-frame action coordination |
| `aChrSelectBlank` | `0x60` | Character-select-to-stage transition gate |
| `aGame` | `0x58` | Shared game/common-resource area |
| `aNextBattle` | `0x88` | Next-battle presentation and battle-round setup |
| `aStageNNN` | `0x70` | Concrete stage archive loader |
| `aTitle` | `0xB8` | Title/menu presentation and session reset |
| `aTitleLogo` | `0x80` | Startup caution/logo sequence |

The DTI area-parent relation determines which objects coexist in a chain. The
verified battle path is `aGame -> aAction -> aStageNNN`; `aNextBattle` instead
uses `aGame` as its area parent and forms a separate next-battle presentation
branch. A stage request therefore runs shared game/action behavior plus the
concrete stage loader, without code explicitly queuing each DTI in sequence.

## `cArea` Layout and Lifecycle

The base constructor is `cArea__ctor` at `0x1406581B0`. The fields directly
confirmed by constructor, property metadata, and manager accesses are:

| Offset | Type | Verified role |
|---:|---|---|
| `+0x08` | `int32_t` | `mRno`, reflected by property metadata |
| `+0x10` | object state | Embedded MT Framework synchronization/state storage |
| `+0x38` | `bool` | `mInit`; set after one-time initialization |
| `+0x3C` | `int32_t` | `mAreaPt`; active-chain index |
| `+0x40` | string storage pointer | Cached area name used by common area logic |
| `+0x48` | `cResource *` | Resource reference released by `cArea__final` |

`aMvcArea__ctor` at `0x1402126E0` adds a dword at `+0x50` and establishes the
MVC-specific virtual interface. Concrete constructors then initialize fields
from `+0x58` onward.

The relevant virtual calls made by `sArea` are:

| Vtable offset | Manager-observed behavior |
|---:|---|
| `+0x00` | Deleting destructor |
| `+0x08` | DTI factory/new-instance operation |
| `+0x20` | Return the instance's DTI/area metadata used during rebuilding |
| `+0x28` | Load/readiness predicate |
| `+0x30` | One-time init callback |
| `+0x38` | Per-frame move callback |
| `+0x40` | Final callback before destruction |
| `+0x48` | DTI parent query on metadata objects |
| `+0x50` | Area name or metadata query, depending on object kind |

The per-frame manager sequence at `0x140545360` is precise:

```text
for each active area from root to leaf:
    if !area->mInit:
        if !area->load(): stop processing deeper areas this frame
        area->init()
        area->mInit = true
    area->move()

if a target DTI is pending and the current leaf is initialized:
    applyPendingAreaTransition(target)
    pendingTarget = null
```

Thus the names `load`, `init`, and `move` used in Ghidra align with how `sArea`
actually dispatches them. A queued transition is deliberately deferred rather
than performed synchronously by most callers.

## `sArea` Layout

`sArea` is allocated as `0x7088` bytes and published through the singleton
pointer at `0x140E17A80`.

| Offset | Type | Verified role |
|---:|---|---|
| `+0x38` | `AreaInfo[512]` | Fixed metadata cache, each entry `0x38` bytes |
| `+0x7038` | `uint32_t` | Number of populated `AreaInfo` entries |
| `+0x703C` | `int32_t` | Active area-chain length |
| `+0x7040` | `MtDTI *` | Queued target DTI |
| `+0x7048` | `cArea *[8]` | Active root-to-leaf area chain |

The constructor `sArea__sArea` at `0x140544C40` initializes all 512 cache
entries, creates the `AreaRoot` metadata node, and recursively indexes the DTI
tree with `sArea__buildAreaInfoTreeFromDTI` at `0x140544FA0`.

An `AreaInfo` is `0x38` bytes. Confirmed relationships include its DTI pointer,
display/name pointer, parent pointer, first-child pointer, next-sibling pointer,
and previous-sibling pointer. `sArea__applyPendingAreaTransition` follows parent
links to produce a root-to-leaf target chain, compares it with the active
objects' metadata, and preserves the matching prefix.

The manager also supports:

| Address | Function | Role |
|---|---|---|
| `0x1405454D0` | `sArea__rebuildChildren` | Hot-recreate the same active chain while preserving DTIs |
| `0x1405455B0` | `sArea__releaseChildren` | Finalize/delete all active objects and clear state |
| `0x140545120` | `sArea__createReloadProperty` | Expose editor reload operations |
| `0x1405451F0` | `sArea__createProperty` | Expose `mpArea` and `mAreaPt` properties |
| `0x1402034E0` | `sArea__getPendingAreaDTI` | Return the queued target, not the current leaf |

That last distinction matters: code comparing this result with
`aNextBattle__DTI` is inspecting the next requested transition.

## Native Transition Flow

### Startup and title

`aTitleLogo__processBootSequence` at `0x1402177B0` displays the caution
scheduler, waits for its minimum duration or accepted skip input, tears it
down, and reports completion. `aTitle` then owns title/menu presentation.

`aTitle__init` at `0x140216940` is much broader than a menu constructor. It
resets UI, license, network-room, battle-slot, guide, and play-counter state;
creates draw and menu-camera units; selects behavior from `sGameUI` boot state;
loads `sound\\lang`; reseeds the global RNG; and restores frame-wait behavior.

`aTitle__load` at `0x140216F50` is an asynchronous readiness gate. It releases
old character archives, coordinates title loading UI, waits for character/UI
resources, loads `sound\\next`, and performs session/ranking checks.

`aTitle__move` at `0x140217200` is only the area move callback. It clears dead
effect/camera pointers and freezes matching light-space-shadow menu units. It
is not, by itself, the complete title state machine. The plugin's training boot
hook currently intercepts this callback.

Other title-related transitions include:

| Address | Behavior |
|---|---|
| `0x14020E080` | Returns `aMenu__DTI` as title's next/menu area |
| `0x14020CAC0` | Attract completion sets boot state 1 and queues `aTitle` |
| `0x14020FB00` | Movie completion releases its object and queues `aTitle` |
| `0x140215FB0` | Idle timeout handling |
| `0x140212BC0` | License/title readiness check |

### Character selection to battle

`aChrSelect__onSelectionComplete` at `0x1402131C0` dispatches by selected mode.
The confirmed training branch (`uVar5 == 6`) does the following:

1. Resets the agent session.
2. Restores the battle-setting backup.
3. Sets CSS state to zero.
4. Sets battle type to 5.
5. Creates/ensures the loading unit.
6. Queues `aChrSelectBlank__DTI` through `sArea`.

`sBattleSetting__requestEnter` at `0x14024B540` follows the same intermediary
model: it stores the pending stage ID in `sGameUI` and queues
`aChrSelectBlank`. Native online forces pending stage ID 2.

`aChrSelectBlank` then performs the actual handoff:

1. `load` (`0x140209400`) marks the battle transition active and waits for the
   stage archive/UI and action readiness.
2. `move` (`0x140209B70`) waits for optional online state, records stage/team
   information, formats `aStage000` or `aStage%03d`, resolves that DTI by name,
   and queues it through `sArea`.
3. For native online battle it also disables the battle runtime parallel flag.
4. `final` (`0x140208980`) releases transition archives and restores that
   parallel flag if the transition is aborted or torn down while active.

The concrete `aStageNNN` area loads two resources. For stage 000 these are
`stg\\000\\0000` and `ui\\res\\arc\\stg\\0000`; sibling stage classes use the
same pattern with the corresponding numeric ID.

### Next battle and gameplay

`aNextBattle` is a next-battle presentation area under `aGame`. It is not the
global area manager, the active gameplay state singleton, or the parent of the
stage/action branch.

| Address | Function | Verified role |
|---|---|---|
| `0x1402150F0` | `aNextBattle__init` | Create next-battle UI/scheduler, stage preview, shadow/camera, and UI state |
| `0x1402154A0` | `aNextBattle__load` | Wait for `ui\\mnnext`, stage preview, and UI archives |
| `0x140203810` | `aAction__init` | Configure rules/determinism, create action units, spawn characters, reset gameplay systems |
| `0x140202DE0` | `aAction__final` | Restore parallel settings and reset replay/action/shot/effect/stage/UI state during departure |

`aAction__load` at `0x140204130` requests all configured character archive sets
and waits for readiness. `aAction__move` at `0x140204470` drives the `sAction`
state machine. At action step 1 it waits for the live stage body and, for native
online modes, synchronization/session readiness; it then requests action step
2 and tears down loading UI.

The plugin previously called `0x140001A90` `getAmaterasu()` and named its return
value `aNextBattle`. Native references establish that this function is
`sAction__getInstance` and returns the pointer stored at `0x140D47E68`.
Consequently `+0xD8`, `+0xDC`, and `+0xE0` are `sAction` state fields, not
fields of the `0x88`-byte `aNextBattle` area object. Source names and diagnostics
have been corrected accordingly.

### Leaving battle

Confirmed area-routing callbacks include:

| Address | Behavior |
|---|---|
| `0x14020C430` | When the post-match child reaches status 4, queue `aCredit` or `aTitle` |
| `0x14020DCA0` | Continue/victory routing to `aChrSelect`, `aEnding`, or `aTitle` |
| `0x14020C040` | Release battle-end UI/resources and restore camera color |

These callbacks queue DTIs through `sArea`; they do not directly construct the
new areas.

## Plugin Implications

The training boot implementation correctly invokes virtual slot `+0x50` on
`sArea`, but its argument is `aChrSelectBlank__DTI` (`0x140D500F0`). It does
not directly pass `aNextBattle__DTI` (`0x140D50618`). The native sequence is:

```text
queue aChrSelectBlank
  -> wait for transition resources
  -> resolve and queue aStageNNN
  -> sArea builds aGame -> aAction -> aStageNNN
```

This distinction explains why bypassing `aChrSelectBlank` or directly forcing
one concrete area can omit stage/resource preparation or inherited area
objects. Runtime code should continue relocating all preferred addresses with
`_addr()`.

## Key Addresses

| Address | Symbol |
|---|---|
| `0x140E17A80` | `sArea` singleton pointer |
| `0x140544C40` | `sArea__sArea` |
| `0x140544FA0` | `sArea__buildAreaInfoTreeFromDTI` |
| `0x140545350` | `sArea__requestAreaTransition` |
| `0x140545360` | `sArea__updateChildren` |
| `0x1405454D0` | `sArea__rebuildChildren` |
| `0x1405455B0` | `sArea__releaseChildren` |
| `0x140545640` | `sArea__applyPendingAreaTransition` |
| `0x1406581B0` | `cArea__ctor` |
| `0x1402126E0` | `aMvcArea__ctor` |
| `0x140D500F0` | `aChrSelectBlank__DTI` |
| `0x140D50618` | `aNextBattle__DTI` |
| `0x140D506E8` | `aTitle__DTI` |
