# Native Network Frame Disable

## Implementation

Ghidra audit: `umvc3.exe.unpacked.exe`, image base `0x140000000`, port 8192,
2026-11-04. `DisableNativeNetworkFrameUpdates` in `rollback_hooks.cpp` replaces
four three-byte indirect calls in `sMvc3Main::move` with NOPs at startup.
All four instructions are checked before any write. One protection window covers
the sites; original bytes are registered with gameplay-hook ownership for Normal
Launch restoration. This is unconditional for mod gameplay, not a live toggle.

| Callsite | Original bytes | Concrete update target | Scope |
| --- | --- | --- | --- |
| `0x140259813` | `FF 50 30` | `0x140458320` | `nNet::sComponent::move` |
| `0x140259820` | `FF 50 30` | `0x1402739E0` | `sAgentSession::move` |
| `0x140259843` | `FF 52 30` | `0x140285E90` | `sMvc3Ranking::move` |
| `0x140259859` | `FF 52 30` | `0x140290FB0` | `sNetworkRoom::move` |

The return values are unused. Object getters, Guide dispatch at `0x14025982D`,
and EarthAtk dispatch at `0x14025986F` remain intact. A smaller contiguous jump
would also remove mixed UI/dialog work and calendar maintenance; it was rejected.
No executable file on disk or Ghidra memory bytes are patched.

## Component Coverage

### GameConfig Maintenance

`Rollback_PatchGameConfigRefresh` also suppresses the two platform-refresh
blocks in `sGameConfig::move` (`0x140253490`), independent of timing flags:

| Patch | Original bytes | Replacement | Destination |
| --- | --- | --- | --- |
| `0x1402534BD` | `83 BB 44 04 00 00 00` | `E9 26 00 00 00 90 90` | `0x1402534E8` |
| `0x140253508` | `48 8D B3 38 03 00 00` | `E9 97 00 00 00 90 90` | `0x1402535A4` |

The first skips unresolved-slot tests, platform readiness, and
`refreshNetworkSlotState`. The second skips the 60-update Steam persona-name
refresh, including its counter. Dead-object checks for pointers at `+0x4ADF0`
and `+0x514A0` remain unchanged. The second jump lands at the original epilogue,
without entering the skipped RDI save/restore region.

The refresh helper (`0x140255160`) is NOT globally disabled: it resolves native
session fields and calls the player-name helper (`0x1402557D0`), which fetches
the Steam name under a critical section. Session resolution also reaches
`0x1402542B0`, updating native license/session-derived state. Explicit callers
at title-menu `0x1403F81EA` and Guide `0x140283A9B` are preserved, as are
construction and other configuration methods. Names will no longer refresh
automatically each second through move; fields can remain at their last values
between explicit refreshes. This is intended for mod gameplay, not native online.

Original bytes are verified and registered for Normal Launch restoration using
the existing patch helper. `game_config_move` timing now covers the retained
cleanup rather than periodic platform work. Runtime menu/name validation remains
necessary; removing this work does not prove the previously correlated
cross-instance stall was caused by Steam.

### Network Children

Component ctor `0x140457FB0` and concrete vtable `0x140B80FB0` establish these
children. Component movement calls every child's `+0x30` phase:

| Field | Child | Vtable | Move |
| --- | --- | --- | --- |
| `+0x38` | `sNetwork` | `0x140BB3FF0` | `0x1406864D0` |
| `+0x40` | `nNet::sSession` | `0x140B81850` | `0x14045B750` |
| `+0x48` | `nNet::sRanking` | `0x140B81700` | `0x140459A50` |
| `+0x50` | `nNet::sCommunity` | `0x140B80F20` | `0x140457EF0` |
| `+0x58` | `sRemoteProcedure` | `0x140BBF560` | `0x140791040` |

The component and these children have no-op `+0x38`/`+0x40` slots pointing to
`0x1407E1A10`. There is no additional active after/sync implementation to patch
at that level. Do not patch the shared stub or assume every class uses the same
slot semantics.

`sNetwork::move` updates the core/mediator (including adapter enumeration),
context, four session slots, ranking, voice chat, storage, and another optional
service. Its four sessions are processed in three passes: `+0x30` move,
`+0x40` receive, `+0x38` send. Concrete session vtable `0x140BC03E0` resolves to
`0x140798890`, `0x140799A40`, and `0x140799BE0`. The latter reaches transport
tick `0x14093CBE0`. All these normal frame paths are cut at the component call.
Session sibling movement can independently boot up a context, which is why
disabling only engine `sNetwork::move` is insufficient.

Agent advances native matchmaking/session operations. Game ranking advances
request objects at `+0x2A18`/`+0x2A20`. Room advances bootup/search and calls
room-message processing `0x140293560`. They are independent of Component and
require their own cuts. Their after/sync slots are already no-ops too.

Manager-owned `sAppNetwork` is distinct from engine `sNetwork`: vtable
`0x140B0C150` already has no-op move/after/sync. EarthAtk movement `0x140278360`
only converts three timestamps using the library timezone; it is not a network
pump. Constructor-resolved manager types were corrected in Ghidra during audit.

## Deliberate Exclusions

This disables the four principal native frame-update roots, **not every possible
native networking activity**. The following remain for safety:

- Constructors, singleton storage, cancellation, destructors, and callback
  registration/unregistration. Other game objects still query these singletons.
- `sMvc3Guide::move` (`0x140282990`): dialogs, fades, notifications and native
  invitations/readiness share this function. Local input gating reads its state;
  freezing it can leave input blocked. Its mixed native state machine remains.
- `sNetPad::processOnlineInput` (`0x140289C30`): also calls required local polling
  at `0x1402B41B0` and handles pause requests. SyncPad movement `0x140289820`
  includes offline input snapshots. Its `+0x50` target `0x1402B4290` processes
  local devices and is shared with ordinary pad. These are not blanket-no-opped.
- `SteamAPI_RunCallbacks`, called from `sApp::execute` at `0x14051DAF1`.
  The mod's Steam integration is not disabled. Native mediator callbacks
  `0x1404DA180`, `0x1404DA1A0`, `0x1404DA140`, `0x1404DA0A0` can still run and
  update readiness/invitations or invoke Steam. This is not a zero-network-I/O
  guarantee.
- Lower-level core/request processing needed to finish shutdown. No native
  network worker creation was found in the audited constructors; this does not
  exclude platform-internal threads or unresolved indirect calls.

Component destructor `0x140458100` deletes Library, RPC, Community, Ranking,
Session, then Network. Context teardown `0x1404FAF70`, ranking destructor
`0x1404DC3B0`, session/P2P teardown and achievement teardown `0x1404FDE30` pump
`MtNetCore::move` (`0x1404F1C10`) while requests remain, sleeping 17 ms between
attempts. Several loops have no fixed bound. Globally returning from the core
or request-controller move could prevent completion and hang shutdown.

## Validation And Limits

Mock tests verify exact patch sites, preservation of every intervening byte,
preflight rejection, protection failures, and restoration via gameplay-hook
ownership. Static references to the audited movement functions are their
vtables; this does not rule out every indirect caller.

In-game validation is still required: startup, local input, training, mod
host/client connection, return-to-menu, and exit. Native matchmaking/ranking/room
operations are intentionally no longer progressed; entering those game menus
may leave operations waiting. This patch is not a supported native-online mode
and is not safe to activate midway through an existing native session. Broader
suppression of Guide/input/Steam callbacks requires separate state-aware work,
not a global RET patch.
