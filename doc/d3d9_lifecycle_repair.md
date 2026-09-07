# D3D9 lifecycle repair: common boundary for ERR09 triggers

Research date: 2026-09-05. Native executable inspected through Ghidra:
`umvc3.exe.unpacked.exe`. The working-tree implementation is described below;
real driver/overlay validation is still required. See
`d3d9_57d463b_regression.md` for the commit comparison.

## Working-tree implementation status

`d3d9reset.cpp` now owns an eleven-hook native controller: property queue,
graphics-option synchronization, native apply/release boundary, cooperative
checks, UI dispatch, Reset submission and HRESULT handling. Device Reset itself
is hooked at device creation. The group of native hooks is enabled together;
installation failure removes only this controller's hooks and stops startup with
an explicit error. MinHook initialization or device-hook failure also stops
startup; these failures disable automatic relaunch instead of silently leaving
the overlay permanently disabled.

- Device ownership comes exclusively from native UI job 3: the creation output
  must be the renderer's `+0xB0` slot and the D3D core must match `+0xC0`.
  The controller prepares the native request, calls the current CreateDevice
  chain, records the returned device and installs Reset interception before
  publishing native job completion. Helper devices cannot replace the tracked
  device, caps, creation thread or window policy, including helpers created
  inside an overlay's CreateDevice wrapper. The shared COM creation hook only
  retains logging and dual-process serialization, not device adoption.
- A raw, unmanaged Reset of the tracked game device is rejected before entering
  the driver or invalidating resources. Only a managed Reset may alter that
  device, and it explicitly closes native readiness. Resets of unrelated devices
  pass through without changing the game's admission gate or presentation policy.

- Property changes own their arguments, coalesce the latest request of each
  kind, and retain ordering between kinds. Resolution choice/sentinel and
  dimension validation precedes native mutation. The five graphics selections
  are captured together; native property-only sync steps run at the safe apply
  boundary before one teardown. DEVICELOST during a native property check waits
  instead of silently discarding the user's queued setting.
  Native constructor property-only writes remain immediate until the first
  apply boundary: `sApp__mainThreadEntry` saves configuration before that
  boundary, so deferring these writes would persist stale settings. Their dirty
  flags still coalesce into the first controlled teardown/Reset, with overlay
  admission closed throughout startup. Their cooperative-level queries use the
  bounded controller UI jobs too, even though `g_inTransition` is not yet set;
  they no longer fall back to the unbounded native dispatcher for the renderer.
- A release/acquire-published UI job owns an immutable request. The native
  `sApp__execute` loop at `0x14051DA70` calls updateUIThread on idle iterations,
  even when no native job is pending, so this does not overwrite native job
  storage. Actual Reset follows the device's current hook chain. The UI hook
  avoids reentrant native D3D job dispatch during managed Reset.
- The native apply call stack stays suspended inside our submission hook while
  the controller waits. No fibers, native unwind tricks or copied apply routine
  are needed. UI/network threads continue independently; gameplay does not run
  on a renderer whose resources are released. This is asynchronous device-thread
  dispatch, not a nonblocking gameplay tick during recovery.
- Automatic lost-device retries keep resources released and reinitialize each
  attempt from immutable input. An exactly matching healthy request can avoid
  Reset entirely. The native submission's stack parameters are preserved even
  though the underlying COM call mutates its private output; native's optional
  second submission cannot inherit zeroed dimensions from the first.
- Healthy teardown remains native. Already-lost teardown uses the verified
  release-only sequence below. Neither repeated failed Reset nor a cancelled
  transition re-enters teardown or proceeds into rebuilding.
- INVALIDCALL and prolonged device loss offer an ownerless Win32 Retry/Cancel
  dialog, not fake success. Cancel and unrecoverable native errors send a peer
  disconnect notice and close the process. A 30-second UI-job timeout retains
  the live job storage; after acknowledgement, a stuck driver requires process
  termination rather than unwinding the request or running DLL detach against
  a thread stuck in external code. Native shutdown is observed during job waits.
- Effective device parameters are queried after actual success; dimensions must
  match the native request before rebuilding resumes. Native HRESULT failures
  during the managed transition stop execution rather than returning into unsafe
  code. Committed presentation/generation and overlay readiness are published
  only after the native apply routine finishes and required wrappers exist.

Configuration persistence is not rewritten or polled by the controller. No
settings are sent to the peer. No abstract controller interface, additional
rendering thread, automatic VSync fallback, or forced release of third-party
resources is introduced.

Tests in `tools/test_d3d9reset.py` execute production functions against mocks:
reset/admission, immutable UI jobs and bounded retries, native release-only
ordering, property queue ownership/coalescing, native creation ownership and
startup failure, and hook-installation failure/rollback. These do not prove actual
driver, overlay, allocation-failure, or native worker behavior. Native healthy
GPU fences/worker waits are retained and can still stall if their implementation
or driver stops responding; the UI-job timeout cannot interrupt those waits.

### Lost-device release adapter evidence

`ReleaseLostResources` follows `0x140540DB0`'s lifetime operations in order:
three virtual `+0x28` deferred-release flushes; wrappers at `+0x8676E8`,
`+0x8676F8`, `+0x8676F0`; native array cleanup `0x14053D230` preserving
`+0x867718`; display wrappers/swapchains at `+0x8897C8/+0x8897C0` with stride
`0x28`; and the texture-type `+0x30` callback/`+0x29` recreate flag over the
resource list at `+0x8817B0` (count `+0x8897B0`).

It omits gamma, Clear, SetRenderTarget, Present and GPU query calls. Both base
and derived renderer virtual `+0x28` resolve to `0x14053D5A0`, which does not
Present when frame-in-flight byte `+0x38` is zero. The adapter requires that
condition. `cSystem__cSystem` at `0x140531AA0` always initializes the critical
section at `+8`, even when its optional locking flag is disabled.

## Verified native convergence

All offsets in this section are relative to `sRender` unless noted otherwise.

| Entry | Address | Observed state change |
| --- | --- | --- |
| Device creation | `0x14053BC50` | Reads DISPLAY FullScreen/VSYNC, uses window client dimensions, enumerates modes, submits UI job 3; ORs dirty mask with `0x14` afterward. |
| Fullscreen property | `0x14053EB00` | Updates fullscreen byte `+0x3A`, viewport dimensions and dirty bits `0x01/0x04`; clears `0x02` when leaving fullscreen. |
| Resolution property | `0x14053F620` | Resolves choice-table entry, updates requested dimensions `+0x8676E0/+0x8676E4`, recalculates viewport and sets `0x04`, potentially fullscreen bits. |
| VSync property | `0x140540D80` | Normalizes bool to 0/1, writes interval selector `+0x867700`, ORs dirty mask with `0x20` if changed. |
| Direct resize/toggle | `0x14053EDE0` | Updates dimensions/fullscreen and dirty flags; called by native main loop's keyboard-toggle path. |
| Present | `0x14053A250` | DEVICELOST from primary Present ORs dirty mask with `0x10`. |
| Main loop | `0x14051DEE0` | Calls applyWindowAndResize before gameplay virtual `+0x30`. |
| Apply/recover | `0x14053D690` | Consumes nonzero dirty mask `+0x867734`, drains rendering, checks cooperative level, tears down, submits Reset, rebuilds, clears mask. |

The VSync acceptor and Resolution acceptor are registered explicitly by
`sRender__initCore` at `0x140535D20`. Hooking only resizeBackbuffer would miss
the resolution-property implementation, which contains its own dimension writes.
Filtering only dirty bits 0-2 would miss VSync (`0x20`) and could confuse
device-loss/startup work (`0x10`) with ordinary presentation changes.

## Startup correction

Native creation genuinely reads initial display settings and forces later
renderer rebuilding (`dirty |= 0x14`). However, `sMvc3Main__sMvc3Main` ends by
calling `sMvc3Main__setGraphicsSettingsPending(this, false)` at `0x140257C95`.
Do not assume every startup executes the pending graphics-option synchronizer.
That flag must be observed at runtime for the actual launch path.

The in-game options UI calls `sMvc3Main__setPendingGraphicsOption` at
`0x1400037B0` and the pending setter at `0x1400037C0`.
`sMvc3Main__syncGraphicsSettings` at `0x140259BC0` performs property changes
across frames. Suppressing it wholesale can discard non-presentation options.

## Threading and failure boundary

`0x14051F8C0`, formerly misnamed MtNetMediator__submitJobAndWait, is
`sApp__testD3DCooperativeLevelOnUIThread`: UI job 5 calls device vtable `+0x18`.
Native CreateDevice (job 3), Reset (job 4), and this cooperative check are
dispatched by `sApp__updateUIThread` at `0x14051F930`.
Both cooperative-check and Reset submissions contain unbounded job polling.
Reset's later retry time checks cannot interrupt a driver/overlay call that
never returns. Do not forcibly terminate a thread executing such a call.

applyWindowAndResize returns without teardown while cooperative level is
DEVICELOST. Otherwise it reaches native teardown at `0x140540DB0`. That teardown
is not just Release calls: it changes device state, presents, issues GPU fences,
and releases/retires renderer resources. It must not be blindly repeated after
a failed Reset, when ordinary device methods are no longer valid.

After Reset submission the apply routine rebuilds backbuffers at `0x14053DB80`
without a recoverable-failure branch around that rebuild. Merely returning a
different value from the submission hook is therefore not sufficient to defer
rebuilding. A coordinated outer transition boundary is required.

## Recommended replacement architecture

One presentation controller owns desired, pending and active configurations,
plus a monotonically increasing successful device/resource generation.
Keep logical borderless fullscreen separate from D3D exclusive fullscreen.
Configuration persistence is best-effort and separate from applying a valid
local runtime setting; read-only files must not cause repeated Reset attempts.

1. At startup resolve/validate the initial display request before CreateDevice.
   Record a pre-call copy, then query actual swapchain/backbuffer state while
   healthy. Reset parameters are in/out; post-call arguments are not a reliable
   permanent record of the active request. Reconcile renderer dimensions,
   interval selector and option selections with the effective presentation.
2. Preserve mandatory native startup resource initialization. When a startup
   Reset is genuinely redundant, bypass only the unnecessary device operation,
   with native post-teardown rebuilding still required. Do not clear all startup
   dirty bits or skip the entire apply routine.
3. Route runtime property changes and direct toggles into a pending transaction
   before they mutate active renderer fields. Coalesce related edits. Validate
   choice bounds, mode support and presentation fields before teardown.
4. At a safe transition boundary drain the render worker and exclude plugin
   capture/ImGui/device access. Preserve UI message processing and native
   device-thread affinity. Do not assume an SRW lock around EndScene alone
   serializes native rendering or third-party overlays.
5. Release native and plugin-owned reset-sensitive resources once. Enter a
   reset-only recovery phase; do not re-enter native pre-reset drawing/fences
   after a failure. Only a successful Reset allows resource rebuilding.
6. Rebuild resources, verify required results, and commit active state/generation
   only when usable. Resume drawing afterward, not merely after Reset entry or
   overlay initialization. On failure do not copy old parameters and return OK.
7. Where supported, try a validated last-known-good request through real Reset;
   there is no guarantee it succeeds if external references remain. If recovery
   cannot complete, retain a responsive non-D3D status UI and terminate netplay
   cleanly rather than leave an unexplained black screen. Device removal or a
   stuck external call may ultimately require restarting the process.

Runtime resolution changes require Reset in the existing native design. A
restart-required graphics mode is a reasonable explicit compatibility option,
but is not equivalent to implementing live changes and does not eliminate
genuine device loss. Borderless reduces exclusive-mode transitions; it does not
eliminate backbuffer-size/interval resets. D3D9Ex or a translation layer would
be a separate compatibility project, not a proven drop-in cure.

## Instrumentation needed to finish the fix

Record the exact D3D operation associated with ERR09, request source, dirty mask,
desired/active presentation, thread IDs, device generation, pre/post HRESULTs,
and teardown/rebuild phase durations. Preserve this history across relaunches.
Do not infer Reset solely from ERR09: the common HRESULT handler serves other
D3D methods too.

For INVALIDCALL, audit live reset-sensitive resources: default-pool buffers and
textures, explicit surface references, state blocks and additional swapchains.
Track plugin ownership first; use diagnostic resource lifetime/callsite tracing
or the D3D debug runtime where available to identify remaining native/overlay
owners. Existing bound-resource probes cannot identify unbound resources or
reliably assign ownership from Release's returned reference count. Never force
Release on an unknown owner's reference.

Test startup, UI resolution/VSync changes, keyboard fullscreen toggle,
minimize/restore, alt-tab, monitor/DPI changes, and repeated lobby loads.
Include writable/read-only config, mixed peer graphics settings, native Windows
drivers and Wine/Proton, with overlays individually off/on. Required assertions:
no rebuilding after failed Reset, no render access during recovery, no duplicate
startup Reset for an already-effective request, no peer overwriting local display
settings, and explicit failure rather than fake recovery when compatibility fails.

Ghidra changes: applied existing sRender/sApp types and named the VSync acceptor,
Resolution acceptor, and UI-thread cooperative-level helper. Existing historical
comments claiming installed experimental guards are not evidence those guards
are present in the current plugin. Use the implementation-status section above
for the current controller; the remaining design notes include optional follow-up
work such as resource-owner tracing and last-known-good fallback.
