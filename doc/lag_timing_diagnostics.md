# Lag Timing Diagnostics

## Enable

Diagnostics are enabled by default when the setting is absent. The equivalent
explicit setting in the active `Timestone.ini` beside the game executable is:

```ini
[Diagnostics]
LagTiming=1
```

Restart the game. The setting is read once, defaults to on, and is not written
automatically. Set it to 0 and restart to disable. After the one-time settings
lookup, scope starts and events use an inline relaxed atomic flag load and branch,
without entering the diagnostic implementation or accessing its thread-local
accounting. Disabled scopes perform no diagnostic QPC reads, aggregation, queue
operations or logging. The writer returns without opening a file or polling.
This is near-zero runtime overhead, not compile-time removal; it does not disable
the separate live benchmark or ordinary HookLog output.

Look for `LAGDIAG` in `Timestone_lag_<process-id>.log` beside the game executable.
Each game instance gets its own file. Diagnostics no longer use HookLog or print
to the console: synchronous diagnostic logging was measured taking 16.7 ms on a
render thread, so it has been removed from the real-time path.

Each participating thread submits at most one aggregate report per five seconds,
after its outermost instrumented scope returns. Submission makes a bounded copy
into one of eight preallocated slots. It does not format text, perform file I/O,
acquire a mutex, or wait for the writer. If no slot is available, it drops the
report. A stalled input wait is reported after it completes; this is not a freeze
watchdog. No per-frame or per-packet prints are added.

The existing initialization thread becomes the writer after startup completes.
It polls at 100 ms intervals and writes directly to the separate file, without
acquiring HookLog's lock. `dropped` is a process-wide cumulative count of failed
submissions, including periods before the writer is ready. `report_qpc` identifies
the original collection endpoint; file order need not be collection order.
File-open/write failure disables the writer; it never falls back to synchronous
logging. Shutdown is nonblocking and may discard pending reports. The ASI is
pinned for the worker's process lifetime to prevent execution after DLL unloading.

## Reproduction

1. Reproduce with the same opponents, delay, rollback and stage settings.
2. Keep Esc and the performance chart closed for at least 15 seconds.
3. Press 0 to show just the chart, then test Esc separately.
4. Retain the reports around visible skipping, noting which display was open.
5. Compare a run with diagnostics disabled to check observer effects.

The code does not change routing, input timing, overlay call order, or rollback
behavior. These measurements do not establish that function/code-layout changes
caused a regression. They expose expensive calls and unexpected logical nesting.

## Reading A Report

Each metric is `count/average_ms/maximum_ms/count_over_25_ms`. Absent metrics did
not execute on that thread in the window. The header includes thread ID, last
outer frame-entry net frame (or -1 on a renderer-only thread), window duration,
source commit and dirty-build flag.

| Metric | Meaning |
| --- | --- |
| `frame` | Outer non-resim HookedMove, including input wait and pre/post native work |
| `native` | Native main-move trampoline, including native rendering/synchronization |
| `pacing` | Native sMain move, including its timing/pacing work |
| `input_wait` | WaitForConfirmedFrame only when input was initially missing; includes packet draining/yielding |
| `bench_update` | Telemetry update, extension dispatch and summary enqueue |
| `bench_begin` | Regular-frame telemetry start/gating |
| `bench_end` | Regular-frame telemetry endpoint, summaries and publication |
| `bench_draw` | Performance chart construction, including its Steam label lookup when due |
| `steam_route` | SteamLobby_GetConnectionType call, including the synchronous Steam query |
| `endscene` | Accepted overlay EndScene callback, including preparation, overlay work and original EndScene |
| `status_hud`, `chrsel_hud`, `esc_hud` | Individual overlay construction calls in their existing order |
| `imgui_submit` | DX9 ImGui draw submission; not a GPU-completion timer |
| `diag_enqueue` | Previous report's bounded submission/accounting duration on this producer thread |
| `frame_gap`, `scene_gap` | Entry-to-entry intervals for outer frames / accepted EndScene callbacks |

### Expanded Frame Stages

| Metric | Meaning |
| --- | --- |
| `controller_start`, `controller_end` | Input resolution/rollback decision work and end-of-frame packet/checksum bookkeeping |
| `packet_drain` | Initial packet drain at frame start; drains inside waits remain charged to the enclosing wait |
| `prediction_wait` | Waiting for the rollback prediction budget |
| `checksum` | Per-frame checksum recording, including replayed frames |
| `main_hook` | Entire main-move detour, including pre-native snapshot/correction work |
| `frame_tail` | Work after the normal native tick: controller end, input capture and periodic netplay logging |
| `save_live`, `save_resim` | Complete snapshot save, distinguished by whether it runs during resimulation |
| `save_regions`, `save_alloc`, `save_copy` | Region-plan validation/build, storage preparation, raw region/camera/post-state capture |
| `save_clones`, `save_parents`, `save_render` | Native collision clones, parent references and render-state collection |
| `save_shots`, `save_shot_refs`, `save_effects` | Shot recipes, character shot references and effect recipes/references |
| `snapdump` | Extra snapshot-history capture when enabled |
| `restore` | Complete restore, including validation and late fixups |
| `restore_setup`, `restore_regions`, `restore_clones` | Restore preparation, region restoration and native collision-clone restoration |
| `restore_fixups` | Allocation-owner repair, hit-solver reset, stale-pointer and effect-slot cleanup |
| `restore_reconstruct`, `restore_late` | Shot/effect reconstruction and subsequent character/render/reference reapplication |
| `rebuild_pin`, `rebuild_retire` | Resource pinning, category sanitation and effect retirement |
| `rebuild_destroy` | Shot destruction, post-destruction sanitation and respawn preparation |
| `rebuild_spawn`, `rebuild_finalize` | All shot factories/registration/concrete restoration, then references/poses/effects/finalization |
| `correction` | Complete accepted rollback correction, including restore, replay and final repairs |
| `resim_frame`, `resim_tick` | One replayed frame including its bookkeeping/save, and its headless tick respectively |
| `overlay_setup`, `overlay_logic`, `imgui_finalize` | Overlay preparation, pre-HUD menu/hotkey logic, and ImGui draw-list finalization |
| `hooklog` | Ordinary HookLog calls, including lock acquisition and output; diagnostic output never uses HookLog |

`self_avg_ms{...}` gives average elapsed time excluding directly nested diagnostic
scopes on the same thread. For example, `controller_start` self time excludes its
instrumented packet drain and waits. `frame` self time exposes outer setup/barrier
work not covered by children. Self time still includes scheduling delays and
instrumentation overhead, not only CPU execution. Engine internals and separate
threads are not automatically instrumented or subtracted.

### Correlated Worst Frame

Each window that contains an outer frame also emits one `LAGWORST` line. Its
`net_frame` is the frame counter at entry to the slowest outer Frame scope in that
window. `report_qpc` links it to the corresponding `LAGDIAG` record.

Fields on this line are **inclusive_ms/self_ms for that specific outer frame**.
Repeated work, such as multiple replay ticks and resimulation saves, is summed
within that frame. This lets us distinguish a large correction from a large live
save without comparing unrelated per-function maxima. Inclusive values overlap;
do not add parent and child values. Self values partition that frame's measured
time when no scope overflow or reentry occurred. A separate render thread's work
is not part of this same-thread partition, though native waits for it still are.

No per-packet/character/particle timers were added. Stage scopes and one bounded
64-entry nesting stack provide coarse coverage without turning every native
operation into a profiling call. Reports and worst-frame snapshots still use the
bounded background queue, with formatting and file I/O off the producer threads.

`scene_gap` is not a Present interval or true display FPS. Initialization, loading,
pausing, debug stepping and disconnects can legitimately produce long intervals.
Do not interpret those as active-match regression evidence.

## Attribution

- Large `input_wait` with small benchmark costs indicates delay-only confirmation
  stalls rather than benchmark processing dominating that window.
- Large `bench_update` warrants examining shared extension queue contention and
  packet dispatch. It also includes the roster dispatcher, not just telemetry.
- Large `bench_end` identifies report computation/publication work.
- Large `bench_draw` plus matching `steam_route` spikes implicates the synchronous
  status lookup; an external Steam lock remains possible, not proven internally.
- Large HUD or submission cost only with Esc open points toward the UI path.
- Large native cost with small measured overlays/input waits points elsewhere in
  native work/synchronization; it does not by itself distinguish CPU and GPU waits.

Durations are inclusive and may overlap. For example, `bench_draw` contains its
Steam call and `native` contains `pacing`; never add all metrics together. Separate
threads may overlap too. For approximate per-frame benchmark bookkeeping overhead,
sum `count * average_ms` for bench_update/begin/end on the game thread and divide
by that window's outer frame count. Evaluate draw cost separately. This excludes
the tiny Now/Record wrappers and isolated correction-timer endpoints; `bench_qpc`
counts existing benchmark counter reads to expose sampling activity.

## Sampling And Order Counters

- `resets`: benchmark Clear calls. Repeated resets during steady play need investigation.
- `samples`: regular-frame sampling scopes started, not necessarily accepted reports.
- `corrections`: correction sampling scopes started, not total rollback requests.
- `bench_qpc`: QPC calls from the existing benchmark Counter, excluding diagnostic timers.
- `native_outside`: native scope entered without an outer Frame scope on that thread.
- `reentry`: nested non-resim Frame scope on the same thread.
- `overlay_order`: deviation from Status HUD -> Character Select HUD -> Esc HUD -> Benchmark Draw within an accepted EndScene.
- `scope_overflow`: instrumentation nesting exceeded 64; deeper scopes are skipped, and exclusive attribution should be treated as incomplete.

Normal steady delay-only play should have no correction samples, low reset counts,
and approximately five regular sample starts per five seconds. Debug/manual call
paths may legitimately trigger order counters; they are evidence to inspect, not
proof of memory corruption. The regular report aggregates a window; its maxima
are not necessarily from the same frame. Use `LAGWORST` for correlated attribution.

## Observer Effects And Verification

The enabled diagnostics add QPC reads and thread-local accounting. No allocation
or blocking cross-thread lock is used for collecting/submitting samples.
Formatting and writing run on the background writer only. `writer_prev_ms` measures
the previous report's formatting/write time there; it is not a game/render cost.
Slow output can consume queue capacity but cannot make producers wait for space.
There is no claim that diagnostic overhead or system-wide I/O effects are zero.
Ordinary HookLog output is unchanged and now timed, but diagnostics never hold its lock.

`tools/test_lag_diagnostics.py` compiles the actual aggregation code with fake
clock/config/file APIs under ASan/UBSan. It checks the disabled path, inclusive
scope timing, producer-thread I/O prohibition, concurrent publication, blocked
writer/queue saturation, file failures, separate-thread counters, interval tracking
order detection, nesting bounds, exclusive-time partitioning and correlated
worst-frame retention. The telemetry integration boundary test
also checks that the native scope stays inside the existing live timer.
