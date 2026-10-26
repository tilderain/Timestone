# Current Implementation

**Status: Current-source reference, reviewed 2026-10-26.**

This file summarizes what the plugin currently implements. It intentionally
uses source symbols instead of line numbers because line numbers change often.

The source review covered `controller.cpp`, `netplay.h/.cpp`,
`steam_transport.cpp`, `rollback_hooks.cpp`, `rollback_resim.cpp`,
`rollback_tick.cpp`, `rollback_snapshot.cpp`, `rollback_restore.cpp`,
`rollback_runtime.cpp`, and `LaunchMenu_settings.cpp`. Selected native scheduler,
frame-timing and ownership anchors were rechecked read-only in Marvel on Ghidra
8193. This is not a fresh two-peer runtime validation. See the
[index](README.md) for verification scope, historical evidence and proposals.

## Netplay

- The plugin uses custom ENet transport. It does not use the retail Steam/CMU
  input transport for gameplay.
- `Netplay_ServiceThread` owns the gameplay ENet host/peer I/O. Steam sessions
  additionally use a separate bridge thread and loopback ENet endpoints in
  `steam_transport.cpp`, forwarding through Steam Networking Sockets. The service
  thread is not the only ENet caller across the entire process.
- The game thread exchanges packets with that thread through bounded queues and
  Win32 critical sections.
- `InputPacket` carries 16 absolute-frame input samples, battle generation,
  confirmed-checksum frame/value, frame advantage, and flags. Generations prevent
  old-match input packets being treated as current-match samples.
- Input packets are unsequenced. Control packets that change shared state are
  reliable.
- Current controls include delay/rollback changes, readiness/round seeds/opening
  members, launch/roster setup, desync/restart, multi-resimulation, autotest,
  snapdump, disconnect, chat and feature/benchmark traffic. `netplay.h` is the
  packet-layout authority; old diagnostics/ground-truth descriptions are not an
  exhaustive current wire-protocol table.

## Input Timeline

- `cChrInput__processFrameInput` at `0x14002DD60` scopes the current character,
  `cChrInput__buttonRemap` at `0x14002DA30` captures and substitutes the normal
  pad path, and `cChrInput__swapLRIfFaceLeft` at `0x14002E560` covers native
  action/CPU paths that bypass button remapping.
- Timeline values are logical actions after physical button configuration but
  before character-specific facing, ability, team, and pause transformations.
- CPU-controlled teams capture the native `uChrCpu` logical mask at the first
  facing-conversion call; only the peer owning that team publishes it.
- Local button-remapped logical input is accumulated and scheduled at
  `currentFrame + delayFrames`.
- Remote input is stored by absolute sender frame.
- Missing remote input is predicted from the last known value.
- A late remote value that differs from the value actually used requests a
  rollback if the frame is still available.
- Controller input/checksum rings have 256 entries. The packet history and live
  snapshot ring each have 16 entries. Neither number is the diagnostic recorder's
  255-slot capacity, and none automatically equals the configured rollback depth.
- Controller initializers currently select 3 delay frames and 7 rollback frames.
  Saved settings and synchronized runtime controls may override them; rollback
  is clamped to 16 and delay to 15. Zero-delay/zero-rollback combinations also
  have explicit policy guards; inspect the setters rather than inferring all
  combinations from the capacities.
- The plugin derives its relative net frame from `sReplay+0x54` minus a match
  baseline. Mutual fight release establishes that baseline through
  `Controller_ResetFightTimer`; it is not normally a first-fighting-update race.
  The plugin does not maintain an independent free-running network clock.

## Snapshot And Restore

- `ROLLBACK_WINDOW` is 16.
- `Rollback_SaveSnapshot` builds a per-frame region manifest and packs region
  bytes into a snapshot-slot buffer.
- Regions include static managers, character bodies, external command and
  collision allocations, RNG domains, timing state, and other verified mutable
  gameplay state.
- Restore has ownership-aware handlers for character bodies, external
  animation/collision records, and specialized reconstruction contracts.
  Existing handlers/constants for a singleton do not prove it is captured:
  current `BuildRegions` disables broad `sMain`, `sHitSolver`, `sCollision` and
  separate `sAction.full` capture. `sAction.compact` remains an active `0x300`
  region with selective restore; the native `sAction` type is `0x250`, so the
  region's tail must not be given fabricated `sAction` member names.
- Accessible pointer-looking words are generally preserved from the live
  allocation graph unless a dedicated reconstruction path owns them.
- Snapshotting is not a general heap checkpoint. Every new pointer-owning
  subsystem needs an explicit capture/restore or reconstruction policy.
- Common `uCharacter` state ends at `0x68F0`; concrete allocation extents vary.
  Generated properties and pointer heuristics are not exhaustive ownership proof.
  [Character](chrfields.md), [shot](shotfields.md), and
  [collision](scollision_rollback_snapshot.md) ledgers retain their qualifications.

## Projectiles And Effects

- Native `sShot` intrusive lists remain authoritative.
- `uShot` objects are polymorphic, variable-sized DTI allocations. They are not
  stored in a fixed flat object array and their heap bodies are not blindly
  restored.
- Spawn hooks record recipes and verified gameplay state.
- Restore retires current shots through native unit/destructor paths, respawns
  the saved set through the native factory, restores safe scalar and concrete
  state, and rebuilds native registration.
- Shot resources are pinned while old shots are destroyed and reconstructed.
- Effects and collision ownership are treated separately because shots can own
  inline objects and external arrays.
- Native finish, retirement and destruction are distinct. In contrast,
  `Effect_RetireCurrent` can synchronously unlink/delete abandoned effects with
  explicit retention exceptions. Its name is not proof of a universal deferred
  free policy or the exact graph-rewind design proposed in
  [effect_rollback_design.md](effect_rollback_design.md).

## Re-Simulation

- Rollback requests are consumed at the hooked `sMvc3Main__move` frame boundary
  on the game thread.
- `Rollback_ReSimulate(start, end)` restores `start` and replays `[start, end)`.
- Historical local and remote inputs are injected before each replayed tick.
- The current loop calls the implemented `Rollback_HeadlessTick`, not the full
  native rendered main tick. Sleep, Present, swapchain and selected draw call
  patches also remain around correction. Native `sMvc3Main__move`
  (`0x1402594A0`) is the ordering reference, not a claim that every service runs
  during replay.
- Headless simulation retains selected area/action work, unit movement,
  collision/hit solving, both late unit phases, camera and gameplay manager
  tails. Hardware input, pacing/rendering, generic `sAI` passes and many other
  services are omitted. Unit and exact-object filters add further distinctions;
  see [sunit_categories.md](sunit_categories.md).
- Audio and other presentation-side callbacks have dedicated re-simulation
  guards where required; they are not rollback state authorities.
- The implementation can replace replayed snapshot slots with corrected states
  in multi-resimulation mode.
- After replay it restores controller history, resynchronizes live pacing,
  flushes deferred model bounds and reconciles active-character UI/visibility.
  Resource/reference topology remains subject to the specialized policies.

## Determinism And Diagnostics

- Netplay applies native battle serialization switches to reduce job-order
  nondeterminism.
- Global RNG, `sEffect` RNG, and per-character RNG are snapshot concerns.
- Input packets carry checksums; a mismatch synchronizes a desync warning and
  pause state between peers.
- Snapdumps retain packed rollback history for byte-level desync comparison.
- Recorder code is compiled in, but capture/writes also require the cached
  `[Rollback] EnableSnapDumpWrites` runtime setting. Compiled-in does not mean
  active history collection by default. See [desync_snapdump.md](desync_snapdump.md).
- F10 writes matching host/client snapdumps during dual-training netplay. The
  snapdump control packet is rejected in every other launch mode.

## Threads

- The injected DLL creates a bootstrap `MainThread` from `DllMain`.
- The gameplay ENet service owns a dedicated thread while netplay is active;
  Steam bridging adds another. Native UI/device, rendering and resource workers
  have separate ownership and are not replaced by these plugin threads.
- Frame-stepper and rollback processing now run from the game/render hook path;
  old references to a permanent `HookUpdate` thread are stale comments.
- Re-simulation must run on the game thread. The ENet thread must never touch
  game objects or invoke game simulation.
- Gameplay serialization disables selected native parallel paths, not every
  worker. `EntireGameSingleThreaded` separately changes regular-job counts and
  a geometry-group mode; it does not currently suppress delay-worker creation.

## Known Limits

- Full rollback correctness still depends on discovering every mutable external
  allocation and ownership relationship used by gameplay.
- Concrete character and projectile subclasses can introduce state not covered
  by common layouts.
- Collision and effect graphs require runtime two-peer stress testing; source
  review alone cannot prove deterministic behavior.
- Comparator matches cover only compared common frames/nonexcluded data. Missing
  dumps, new mismatches, or visual-sounding field names do not prove a fix or
  authorize broader restoration/exclusion.
- Retail native-online notes are useful reverse-engineering references but do
  not describe the plugin's active transport or prediction policy.
