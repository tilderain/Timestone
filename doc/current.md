# Current Implementation

**Status: Current reference. Verified 2026-07-23.**

This file summarizes what the plugin currently implements. It intentionally
uses source symbols instead of line numbers because line numbers change often.

## Netplay

- The plugin uses custom ENet transport. It does not use the retail Steam/CMU
  input transport for gameplay.
- `Netplay_ServiceThread` is the only owner of ENet calls.
- The game thread exchanges packets with that thread through bounded queues and
  Win32 critical sections.
- `InputPacket` carries 16 absolute-frame input samples, a delayed checksum,
  frame advantage, and flags.
- Input packets are unsequenced. Control packets that change shared state are
  reliable.
- Current control messages cover delay/rollback settings, resimulation
  diagnostics, ground truth, battle readiness, launch configuration, desync
  pause/unpause, synchronized restart, and multi-resimulation mode.

## Input Timeline

- `cChrInput__processFrameInput` at `0x14002DD60` remains the final native
  translation point before the plugin applies timeline input.
- Local physical input is accumulated and scheduled at
  `currentFrame + delayFrames`.
- Remote input is stored by absolute sender frame.
- Missing remote input is predicted from the last known value.
- A late remote value that differs from the value actually used requests a
  rollback if the frame is still available.
- The history and snapshot capacities are both 16 frames.
- Source defaults are currently 2 delay frames and 8 rollback frames. Runtime
  controls may change them; rollback is clamped to 16 and delay to 15.
- The plugin derives its relative net frame from `sReplay+0x54` minus a match
  baseline. It does not maintain an independent free-running network clock.

## Snapshot And Restore

- `ROLLBACK_WINDOW` is 16.
- `Rollback_SaveSnapshot` builds a per-frame region manifest and packs region
  bytes into a snapshot-slot buffer.
- Regions include static managers, character bodies, external command and
  collision allocations, RNG domains, timing state, and other verified mutable
  gameplay state.
- Restore has ownership-aware handlers for objects that cannot be safely
  restored with a flat `memcpy`, notably `sCollision`, `sMain`, `sHitSolver`,
  character bodies, and external animation/collision records.
- Accessible pointer-looking words are generally preserved from the live
  allocation graph unless a dedicated reconstruction path owns them.
- Snapshotting is not a general heap checkpoint. Every new pointer-owning
  subsystem needs an explicit capture/restore or reconstruction policy.

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

## Re-Simulation

- Rollback requests are consumed at the hooked `sMvc3Main__move` frame boundary
  on the game thread.
- `Rollback_ReSimulate(start, end)` restores `start` and replays `[start, end)`.
- Historical local and remote inputs are injected before each replayed tick.
- The current loop calls the full native `oMvc3Main_move` for every replayed
  frame while temporarily patching Sleep, Present, swapchain, and selected draw
  calls. `Rollback_HeadlessTick` is currently an unused early-return stub.
- Audio and other presentation-side callbacks have dedicated re-simulation
  guards where required; they are not rollback state authorities.
- The implementation can replace replayed snapshot slots with corrected states
  in multi-resimulation mode.

## Determinism And Diagnostics

- Netplay applies native battle serialization switches to reduce job-order
  nondeterminism.
- Global RNG, `sEffect` RNG, and per-character RNG are snapshot concerns.
- Input packets carry checksums; a mismatch synchronizes a desync warning and
  pause state between peers.
- Resimulation and ground-truth packets are diagnostic channels, not authority
  for gameplay correction.
- NDJSON logging can capture live, resimulated, projectile, collision, input,
  and RNG state. It is expensive and controlled by diagnostic toggles.

## Threads

- The injected DLL creates a bootstrap `MainThread` from `DllMain`.
- The ENet service owns a dedicated thread while netplay is active.
- Frame-stepper and rollback processing now run from the game/render hook path;
  old references to a permanent `HookUpdate` thread are stale comments.
- Re-simulation must run on the game thread. The ENet thread must never touch
  game objects or invoke game simulation.

## Known Limits

- Full rollback correctness still depends on discovering every mutable external
  allocation and ownership relationship used by gameplay.
- Concrete character and projectile subclasses can introduce state not covered
  by common layouts.
- Collision and effect graphs require runtime two-peer stress testing; source
  review alone cannot prove deterministic behavior.
- Retail native-online notes are useful reverse-engineering references but do
  not describe the plugin's active transport or prediction policy.
