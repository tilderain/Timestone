# Mouse Input, `sId`, and Netplay

**Status:** Ghidra-verified reverse-engineering reference for the current
`umvc3.exe.unpacked.exe` build. Function names describing behavior are inferred
unless they are supported by DTI property names or resource names.

## Summary

`sId` is not a unique object-ID allocator. It is a mouse/UI hit-map service that
maps cursor coordinates and mouse-button state to IDs of rectangular UI items.

The service loads `boot\\idmaplist`, maintains four input contexts, tests the
cursor against each active map's rectangles, and exposes hover, press, release,
hold, and repeat-like results to menu code. It does not appear to allocate IDs
for shots, effects, or other combat objects.

For netplay, mouse-driven UI can be synchronized, but the network protocol
should normally transmit resolved UI actions rather than physical cursor
coordinates. Shared menus should use input delay or confirmed execution. UI
rollback is possible only when the complete menu state is reversible and its
external side effects are deferred or deduplicated.

## Singleton and Main-Loop Dispatch

The global singleton pointer is stored at:

```text
0x140D51218: sId*
```

`sId__getInstance` at `0x140001AD0` returns this pointer. `sId__ctor` at
`0x14025BAB0` installs it, and the destructor clears it.

`sMvc3Main` also stores the singleton pointer in its member slot at
`sMvc3Main + 0x40228`. The per-frame call through that member's vtable slot
`+0x30` resolves to:

```text
0x14025CD00: sId__move_gate
```

The substantial update is:

```text
0x14025CD40: sId__move_body
```

The object referenced by `sMvc3Main + 0x40228` is therefore a mouse/UI service,
not a combat object-ID allocator.

## Resource Model

`sId__rIdMapList__classFunc` at `0x140207F80` loads:

```text
boot\\idmaplist
```

as an `sId::rIdMapList`, then installs it in the singleton. DTI property
creation establishes the following hierarchy:

```text
sId
└── mprIdm: sId::rIdMapList
    └── mMaps: array<sId::Map>
        ├── mName
        └── mIds: array<sId::Id>
            ├── mRect
            └── mMType
```

Relevant functions are:

| Address | Function | Reflected property |
|---|---|---|
| `0x14025C2E0` | `sId__createProperty` | `mprIdm` |
| `0x14025C290` | `sId_rIdMapList__createProperty` | `mMaps` |
| `0x14025C200` | `sId_Map__createProperty` | `mName`, `mIds` |
| `0x14025C170` | `sId_Id__createProperty` | `mRect`, `mMType` |

An `sId::Id` therefore represents a rectangular interactive region and a type
describing its mouse behavior. In this subsystem, "ID" means a UI hit-region
ID, usually the region's index in a map.

## Approximate Object Layout

The constructor and update functions establish an object extending beyond
offset `0xA68`. The following layout is approximate; field names are descriptive
unless they match the reflected property names above.

```cpp
struct sId {
    cSystem base;                       // +0x000
    MouseContext contexts[4];           // +0x038, stride 0x70
    uint8_t queried[4][0x200];          // +0x1F8
    Point currentMouse;                 // +0x9F8
    Point mouseDelta;                   // +0xA00
    Point previousMouse;                // +0xA08
    uint32_t mouseButtonMasks[3];       // +0xA10
    MtArray maps;                       // +0xA20
    int activeMouseDevice;              // +0xA40
    // Pending/configuration fields follow.
    sId_rIdMapList* mprIdm;             // +0xA60
    int moveState;                      // +0xA68
};
```

The four contexts start at `+0x38`, have stride `0x70`, and hold values such as:

- Enabled or disabled state.
- Active ID-map index.
- Current and previous hovered IDs.
- Pressed, released, and held IDs.
- Repeat-related IDs.
- Hold and repeat timers.
- Cached cursor position.

`sId__setMapEntry` at `0x14025D840` selects a map for one context, enables that
context, and resets its cached position:

```cpp
void sId::setMapEntry(uint32_t mapIndex, int context)
{
    contexts[context].mapIndex = mapIndex;
    contexts[context].disabled = false;
    contexts[context].position = defaultPosition;
}
```

This function has more than 100 callers, predominantly in menus, character
select, galleries, options, and UI rendering code.

## Per-Frame Behavior

`sId__move_gate` runs the body only when the ID-map resource at `+0xA60` is
present. Its small state at `+0xA68` gates resource readiness; it is not an ID
counter.

`sId__move_body` performs approximately the following work:

1. Reads the frame/time scale used by mouse timers.
2. Clears transient results for all four contexts.
3. Checks that mouse input and the required application state are enabled.
4. Reads the cursor from `sMouse`.
5. Converts physical viewport coordinates to the UI reference coordinate space.
6. Resolves each context's active `sId::Map`.
7. Iterates the map's `sId::Id` entries.
8. Tests the cursor against each entry's `mRect`.
9. Records the hovered region index.
10. Derives press, release, held, repeat, and related transient IDs.
11. Advances or resets hold and repeat timers.
12. Processes queried IDs and clears the `4 * 0x200` query table.

The central hit test is equivalent to:

```cpp
if (id.mMType == 0 &&
    mouseX >= id.mRect.left &&
    mouseX <  id.mRect.right &&
    mouseY >= id.mRect.top &&
    mouseY <  id.mRect.bottom) {
    hoveredId = index;
}
```

`FUN_14025BFF0` converts viewport-relative cursor coordinates to an internal UI
coordinate space approximately based on `1920 x 900`, with aspect-ratio
adjustment. This resolution conversion is one reason raw cursor coordinates are
a poor network representation.

## Query Functions

Several current Ghidra names around this subsystem are misleading.

### Current Mapped Item

`sId__getMappedInputValue` at `0x14025C3C0` returns a current mapped UI ID for a
context, or `-1` when that context cannot supply mouse input. Menu callers use
the result as a menu item or cursor index.

Examples include option screens and `uMenu__modeFlow_s7_inputNavigation`, which
uses the returned ID to change the current menu selection.

### Mouse Event Test

`sId__processNetworkSync` at `0x14025C860` is misnamed. It does not perform
network synchronization. Its behavior is closer to:

```cpp
bool sId::testMouseEvent(int expectedId, EventType event, int context)
{
    if (expectedId == -1)
        expectedId = deriveDefaultId(context);

    if (contextUnavailable(context))
        return false;

    markIdQueried(context, expectedId);

    int actualId;
    switch (event) {
    case 0: actualId = getNormalMouseId(context);    break;
    case 1: actualId = getActionMouseId(context);    break;
    case 2: actualId = getDefaultMouseId(context);   break;
    case 3: actualId = getAlternateMouseId(context); break;
    case 4: actualId = getExtraMouseId(context);     break;
    case 7: actualId = getBulkMouseId(context);      break;
    default: return false;
    }

    return actualId == expectedId;
}
```

The precise semantic name of every event selector remains to be established,
but callers clearly use this function to ask whether a particular mouse event
occurred over a particular UI item.

For example, character-select and menu functions call it with an expected item
ID, an event selector, and context zero. If it succeeds, they synthesize the
same logical input mask that keyboard or controller navigation would have
produced.

## Correction to Earlier Rollback Notes

Any documentation describing `sId` as a unique object-ID allocator should be
treated as historical and incorrect. In particular, it is not supported to
claim that an unrewound `sId` causes newly reconstructed shots or effects to
receive different identities.

Verified conclusions are:

- `sId` maps mouse activity to rectangular UI item IDs.
- It loads `boot\\idmaplist`.
- Its primary callers are menus and other UI systems.
- No reviewed function allocates shot or effect identities.
- Its `+0xA68` field is a move/resource gate, not an allocation counter.
- The object contains dynamic arrays and a resource pointer, so a flat snapshot
  would not be an appropriate restore policy.

The call currently present in the unused headless sequence at
`UMVC3Hook/code/rollback.cpp`:

```cpp
VTableCall(sMain, 0x40228, 0x30);  // sId
```

should not be considered a required deterministic combat step. It samples the
current physical mouse and advances UI timers. A headless combat re-simulation
should normally omit it unless a specific in-match gameplay system is proven to
consume these mapped mouse IDs.

## Netplay Design

Mouse-driven UI can be synchronized by either input delay or rollback. The
recommended representation is a semantic UI action, not an operating-system
mouse event.

### Transmit Resolved Actions

A useful packet payload is:

```cpp
struct UiInputFrame {
    uint32_t frame;
    uint16_t screenId;
    uint16_t mapId;
    int16_t itemId;       // -1 means no item
    uint8_t context;      // sId context 0..3
    uint8_t events;       // Hover/press/release/held/repeat bits
    uint8_t buttons;      // Left/right/middle when required
    uint8_t sequence;
};
```

If only authoritative clicks are needed, use a smaller action:

```cpp
struct UiAction {
    uint32_t frame;
    uint16_t screenId;
    int16_t itemId;
    uint8_t event;
};
```

For example:

```text
frame=300, screen=character-select, item=17, event=left-click
```

This is preferable to sending `x=1287, y=412`. Raw coordinates depend on:

- Resolution and aspect ratio.
- Viewport scaling.
- UI animation and transition state.
- Localized or otherwise different layouts.
- The active `sId::Map`.
- Local menu timing.

Resolving the item locally before transmission avoids most of these sources of
divergence.

### Delay-Based UI Synchronization

Delay or lockstep is the preferred policy for shared menus:

1. Sample local mouse and controller input.
2. Resolve the mouse through `sId` into a semantic action.
3. Assign the action to absolute frame `F + delay`.
4. Send it with a short history window.
5. Do not process UI frame `F` until required peer input has arrived.
6. Apply peer actions in a fixed deterministic order.

This is suitable for:

- Character and stage selection.
- Lobby options.
- Ready and confirmation screens.
- Rematch prompts.
- Pause-menu voting.
- Shared training settings.

The UI incurs a small delay, but the implementation does not need to reverse
resource loads, sounds, network messages, or menu object lifetimes.

### Rollback UI Synchronization

Rollback is possible only if all affected state can be restored and replayed.
Restoring `sId` alone is insufficient. Relevant state may include:

- Active menu and screen state.
- Current selected item.
- UI animation frames and timers.
- Relevant `sId` context records.
- Active ID-map selections.
- Created and destroyed UI units.
- Character and stage choices.
- Ready and confirmation flags.

External side effects are the principal problem:

- Sending network messages.
- Starting resource loads.
- Saving settings or profile data.
- Playing sounds.
- Creating or deleting UI objects.
- Starting matchmaking or session operations.
- Entering another mode or loading a stage.
- Updating achievements or license data.

Speculative simulation must not execute these effects irreversibly. Queue them
with stable identities and execute them only through the confirmed frame:

```cpp
QueueUiSideEffect({
    .frame = simulationFrame,
    .type = UiEffect::SendReady,
    .id = stableEventId,
});

ExecuteUiSideEffectsThrough(confirmedFrame);
```

A simple `if (!IsResimulating())` guard prevents duplicate effects but can also
lose an effect that should occur on the corrected timeline. Confirmed-frame
queues or explicit idempotency are safer.

### Prediction Policy

Mouse movement is difficult to predict usefully. Reasonable defaults are:

```text
Cursor/hover: retain the last confirmed item, or keep it local-only
Button held:  retain the previous state
Press edge:   predict no press
Release edge: predict no release
```

A late click can trigger rollback, but undoing the resulting screen transition
is usually more costly and risky than delaying the click. A hybrid policy is
therefore preferable:

| Input or action | Recommended policy |
|---|---|
| Cursor movement and hover | Local-only or delayed |
| Cosmetic highlighting | Local-only |
| Confirm/cancel click | Delay until received |
| Character/stage selection | Delayed deterministic action |
| Ready/rematch decision | Reliable event or delayed action |
| In-match gameplay click | Rollback input if gameplay-authoritative |
| Resource load or screen transition | Execute only after confirmation |

## Integration Approach

Do not transmit or restore `sId`'s internal pointers. Resolve local input into a
semantic action and apply that action to the shared menu state:

```text
OS mouse
   |
   v
local sId resolution
   |
   v
UiAction { screen, map, item, event, frame }
   |
   v
network history
   |
   v
shared menu state machine
```

The ideal receiver invokes the menu's semantic operation directly. If existing
menu code can only consume `sId` queries, hook the two query paths rather than
rewriting the singleton's dynamic state:

```cpp
int hkGetMappedInputValue(sId* self, int context)
{
    if (UsingSynchronizedUiInput())
        return GetUiInputForCurrentFrame(context).itemId;

    return originalGetMappedInputValue(self, context);
}

bool hkTestMouseEvent(sId* self, int expectedId, int eventType, int context)
{
    if (UsingSynchronizedUiInput()) {
        const UiInputFrame& input = GetUiInputForCurrentFrame(context);
        return input.itemId == expectedId &&
               (input.events & EventMask(eventType)) != 0;
    }

    return originalTestMouseEvent(self, expectedId, eventType, context);
}
```

This avoids coupling the protocol to the partially understood context layout,
timers, `MtArray` internals, and resource ownership.

## Protocol Requirements

UI packets should carry enough identity to reject stale input:

```text
session epoch
screen/state generation
absolute UI frame
active map ID
context
item ID
event bits
sequence number
```

The screen generation is essential. Without it, a delayed click for item `3`
on one screen could arrive after a transition and activate unrelated item `3`
on the next screen.

Validate received actions:

- Context is in `0..3`.
- Item ID exists in the expected map.
- Event kind is recognized.
- Screen generation and map match synchronized state.
- The peer is authorized to operate the item.
- Duplicate actions are ignored.

When both players can act in the same UI frame, use deterministic conflict
rules:

1. Sort by absolute frame.
2. Process player zero before player one within a frame.
3. Reject an action invalidated by an earlier action in the same frame.
4. Advance the same screen generation on both peers.

Independent player slots should use explicit ownership rather than conflict
resolution where possible.

## Recommended Policy for This Project

1. Keep the existing combat input protocol unchanged.
2. Add a separate absolute-frame UI action history and packet type.
3. Resolve local mouse input through `sId`.
4. Synchronize authoritative actions, not cosmetic hover state.
5. Use short input delay or lockstep for menus.
6. Defer transitions and external side effects until confirmed.
7. Use rollback only for in-match UI already covered by deterministic snapshots.
8. Skip native `sId::move` during headless combat re-simulation unless a proven
   gameplay consumer requires it.

The practical conclusion is that mouse-controlled UI is compatible with the
netcode, but the protocol should send resolved item actions. Delay-based
execution is the robust default; rollback should be reserved for UI whose full
state and side effects have deliberately been made rollback-safe.
