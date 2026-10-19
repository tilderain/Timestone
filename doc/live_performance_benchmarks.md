# Live Performance Benchmarks

Status: Passive implementation added 2026-10-19. The investigation and extended
design below are retained as context; the implementation scope is described here.
Checked against current source and Ghidra's `umvc3.exe.unpacked.exe` instance.

## Implemented Behavior

`UMVC3Hook/code/live_benchmark.cpp` owns collection, protocol/dispatch, assessment,
and display; `live_benchmark.h` is its small timing integration API.

- Collection is passive and samples one advancing battle frame about once per
  second. Other frames do not read QPC or record save/restore/resim timings.
  It never forces a save, restore, or simulation step and never enables the
  detailed profiler. Natural rollback events outside sampled frames are skipped.
- Press the top-row `0` key to send a reliable display request. Both peers show
  their local/opponent table in the top-right for five seconds from each side's
  request receipt. Pressing again refreshes the display. Text entry suppresses
  the hotkey. Both sides immediately send their latest available summary on a
  request, without forcing another sample. The table is always visible while
  the custom Escape menu is open.
- Each metric has a fixed 120-sample ring. Display low/median/high FPS as
  `1e6 / maximum`, `1e6 / median`, and `1e6 / minimum` microsecond cost. Save,
  restore, and whole-correction rows show median milliseconds in the compact HUD.
  FPS here means inverse observed cost, not display refresh rate. With one frame
  sampled per second, live history covers roughly two minutes rather than two
  seconds; sparse rollback samples can span longer.
- The compact top-right panel leads with per-player frame health: green SMOOTH,
  amber STUTTERING, red TOO SLOW, or gray CHECKING / NO RECENT DATA. Median and
  slow-frame (p95) costs use a 16.67 ms budget with 0.5 ms tolerance; a fast median
  cannot hide slow-frame spikes. Live bars use slow-frame capacity, with the same
  status color as the headline. Both bars use a 0-200% capacity scale with a
  midpoint marker at 100%; more fill means more capacity. Numerical values above
  200% remain visible even when the bar is full. Typical FPS, low/high figures,
  resim capacity, and operation costs are secondary.
  This is sampled PC performance, not a network-lag detector or a guarantee that
  unsampled frames never stutter. Steam connection type is included in the panel
  header instead of a separate overlapping window. Colors and spacing follow
  the Escape menu theme.
- Live FPS measures the main hook including correction and forward snapshot
  work, minus the existing native base timing/pacing scope (Sleep, spinning, and
  base bookkeeping). Rendering, worker/GPU synchronization, and Present waits
  remain included.
- Resim shows estimated capacity for the reporting peer's configured rollback
  depth, not single-headless-tick FPS. Cost is restore + N headless ticks + N
  corrected saves when multi-resim is enabled. Capacity is `16667 us / cost *
  100%`: a 9 ms stack is about 185%, while seven 9 ms ticks already exceed the
  budget. Below 100% is red; 100-125% amber; above 125% green. The cost and depth
  are shown next to the bar, and low/high are estimated capacity bounds from
  component maximum/minimum costs. Disabled rollback is Off; missing measurements
  are not treated as zero-cost or unlimited capacity. This resim-only capacity
  does not reserve the cost of the live rendered frame.
- Successful saves (both forward and corrected), successful restores, headless
  ticks, and complete correction calls are timed at coarse function boundaries.
  Early-return save/restore failures are not recorded as cheap successes.
- A private clean-live ring excludes frames with corrections. Assessment uses
  p95 costs and this baseline to estimate a correction at the current configured
  depth as clean live + restore + N headless ticks + N saves with multi-resim. This is a
  heuristic, not an exact measured transaction p95 or a hardware certification;
  patching/repair/input bookkeeping are not included in that estimate. Measured
  correction costs and live-frame costs do include those expenses.
- Assessment starts after two live samples (roughly two seconds). Rollback
  headroom also requires two clean-live, restore, and headless samples, plus two
  save samples when multi-resim is enabled.
  These early estimates use very little evidence and improve as samples arrive.
  A smooth frame-health status without rollback evidence says "Rollback:
  untested", not that rollback capability is established. A 0.5 ms tolerance avoids classifying
  tiny 60 Hz boundary variations as an inadequate PC.
- Summaries are built after each sampled frame, about once per second. Background
  reports are sent at privately jittered 5-10 second intervals; the game's RNG
  is never touched. Requests/replies bypass that interval, with bounded retries
  for queued requests and fresh replies. Each 148-byte packet uses
  `PACKET_BENCHMARK` through the existing bounded reliable feature queue. A
  separate top-level type is necessary because older roster-only consumers can
  otherwise block on a larger unfamiliar extension packet; older peers simply
  ignore the new packet type. There is no input-packet layout change.
- One full-size feature-queue drain routes benchmark messages and passes roster
  messages to the existing roster handler. Versions, generations, sizes,
  sequences, flags, rollback depth (bits 8-12, clamped to 0-16), counts, and
  ordered percentiles are validated. A depth change resets the local window.
  Duplicate
  requests do not extend display lifetime. Failed background summary sends are
  dropped; display requests and requested replies retry only until their
  five-second deadline. No forced sample is taken for a display request.
- The game thread owns sample rings and report assembly. Three fixed display
  slots transfer completed views to the renderer using a lock-free atomic index
  exchange. The renderer keeps its owned slot until its next read; publication
  cannot overwrite it. EndScene's existing reentrancy guard supplies one reader.
  There are no benchmark publication locks, waits, retry loops, heap allocations,
  or per-render-frame report copies. Cached visible reads need one acquire load;
  an exchange is needed only when a new snapshot is available. Display requests
  use lock-free atomics, with load-before-exchange on the ordinary game path.
  Collection does not allocate, log, sort, take locks, or send packets inside
  individual timed scopes. Summary sorting stays once per sampled frame.
- The hidden HUD returns before clock reads or snapshot acquisition. The visible
  Steam connection label caches its SDK query for one second rather than querying
  Steam every rendered frame. Existing transport queue locks and the renderer's
  pre-existing overlay/device synchronization are not removed by this feature.
- Non-advancing frames discard their staged samples, retaining prior results.
  Connection, generation, execution-mode, or frame-timeline changes reset it.
  Local results older than 2.5 seconds and peer reports older than 12 seconds
  remain readable but are labeled old and receive no current assessment. The
  longer peer timeout accommodates the background report interval. Only locally
  fresh results are sent. Missing peer data is N/A, not zero. Old peers need an updated
  plugin to participate in the display request and reporting.

Verification: `python3 tools/test_live_benchmark.py` compiles and exercises the
collector, packet routing/validation, pacing subtraction, percentiles, expiry,
queue backpressure, sampling/report cadence, requested exchanges, display
hotkey/menu behavior, bar values, Steam label placement, and assessments with fake game,
clock, and ImGui APIs. The native harness also validates held-reader-slot safety
and coherent payloads under 500,000 concurrent publications, and reports isolated
publication/cached-read nanoseconds per operation. Those microbenchmarks do not
measure Windows QPC, Steam, ImGui, or whole-game overhead.
Real two-peer gameplay, rendering-thread mode behavior,
and observer overhead still require in-game verification.

The duration selector, explicit capture sessions, fixed-depth controlled stress
runner, per-depth correction distributions, and GPU query timings in the design
below are not implemented. Keeping v1 passive is intentional.

## Recommendation

Add a small runtime-gated timing collector, separate from the detailed rollback
profiler. A request starts a bounded live capture on this machine and optionally
on its peer. Display both summaries side by side. Observe actual work by default;
do not run extra save/restore/simulation operations during an ordinary match.

Provide a separate, explicitly enabled training stress mode for fixed-depth
rollback measurements when natural corrections are too rare. Passive capture
answers "what is this match costing now?"; controlled stress answers "what does
an N-frame correction cost on these machines?" They must not share unlabeled data.

## Existing Integration Points

| Source | Finding |
|---|---|
| `rollback_profile.h:6-10` | Detailed profiling defaults to compile-time OFF. |
| `rollback_profile.cpp:75-88,221-238,240-491` | An active profile covers a rollback transaction, has detailed save/restore/headless phases, and writes JSON synchronously. It is not a live rolling collector. |
| `rollback_hooks.cpp:193-298` | Main game-thread boundary: consume rollback, save the current frame, then call the ordinary native rendered tick. |
| `rollback_resim.cpp:129-245` | Restore once, replay `[start,end)` using `Rollback_HeadlessTick`, repair live state, and disable patches. Multi-resim can also save corrected snapshots inside the loop. |
| `rollback_tick.cpp:655-866` | Headless simulation is implemented and used. Rendering, base pacing, and many presentation services are omitted. |
| `rollback_snapshot.cpp:1428-1643` | Whole save includes manifest construction, recycling/allocation, copies, collision clones, character render state, shot/effect recipes, inputs, and optional snapdump capture. |
| `rollback_restore.cpp:1226-2304` | Restore is ownership-aware reconstruction, not just a memory copy. Missing snapshots return early. |
| `rollback_hooks.cpp:439-453` | Existing base `sMain__move` hook can measure the native timing/pacing segment separately. |
| `rollback_hooks.cpp:309-314` | Existing render-end hook encloses native end-frame work and presentation, but not all rendering. |
| `dllmain.cpp:871-982` | ImGui renders in D3D9 EndScene. This is not the completed game-frame boundary. |
| `framestepper.cpp:1818-1920` | Existing local status HUD has ping/FPS/delay/rollback, but no local/peer execution-cost summaries. |
| `netplay.h:331-334`, `netplay.cpp:1300-1335` | Reliable extension queue supports packets up to 256 bytes. Sending can fail when the bounded queue is full. |
| `chrsel_imgui.cpp:946-951` | Roster code currently consumes the entire extension queue. A second independent consumer would steal packets or block on a packet larger than its buffer. |

`doc/current.md`'s resimulation section is stale: the present source uses the
implemented headless tick, not the full native tick described there. Prefer the
source findings above for this design.

## Measurement Boundaries

All initial measurements are QPC wall-clock elapsed times, not CPU cycle counts
or GPU execution times. Scheduling stalls and waits inside the scope count.
Initialize QPC frequency once; keep raw integer ticks locally.

| Metric | Boundary and meaning |
|---|---|
| Full save | Complete `Rollback_SaveSnapshot` call, including its tail and enabled diagnostics. Separate forward saves from corrected resim saves. |
| Full restore | Complete successful `Rollback_RestoreSnapshot` call, including reconstruction and repairs. Do not report an early missing-slot return as a fast successful restore. |
| Headless tick | Each complete `Rollback_HeadlessTick` call, without surrounding snapshot/input/checksum work. |
| Replay loop | Complete replay loop, including input injection, corrected saves, headless ticks, and checksum/bookkeeping. |
| Correction total | Complete successful `Rollback_ReSimulate`, including patches, input-history preservation, restore, replay loop, deferred model bounds, UI/visibility repair, and timing resync. |
| Forward rendered tick | Around the ordinary `original(main)` call in `hkMvc3MainMove`. Includes native pacing, gameplay, drawing, GPU synchronization, Present, and post-frame tail. Excludes preceding rollback/save work. |
| Full live frame | Outer `hkMvc3MainMove` entry through exit, including rollback, forward save, and the rendered tick. Exclude nested/resim invocations. |
| Base timing/pacing | Around `oMainMove(main)` in `hkMainMove`. This includes timing bookkeeping as well as Sleep/spin pacing; do not label it pure sleep. |
| Frame excluding base timing | Full live frame minus its measured base timing/pacing segment. Includes render/Present waits. A useful cost view, not pure CPU or uncapped GPU throughput. |
| Render end / Present | Existing `hkCallSRender` duration includes completion/playback, GPU fence or worker wait, primary/additional Presents, and cleanup. Label as a combined segment unless separately instrumented. |

The existing `Profile_RecordSnapshot` endpoint precedes input metadata and
optional snapdump capture. Reusing that number would not measure the entire save.
Also, its active rollback profile does not cover ordinary forward-frame saves.

There is no single naturally adjacent "save, restore, headless tick" operation:
the restored snapshot was saved on an earlier tick. Show those component
distributions, plus the measured correction total grouped by actual depth N.
If a synthetic composite is useful, explicitly label
`mean forward save + mean restore + N * mean headless tick` as an estimate. It
omits replay bookkeeping, corrected saves, patching, and repairs. Never present
the sum of independent p95 values as a measured p95 transaction.

### Rendering Interpretation

Ghidra confirms `sMvc3Main__move @ 0x1402594A0` starts with
`sMain__move @ 0x140521DF0`, which includes Sleep and busy-wait pacing. Its tail
builds rendering, calls `sRender__endFrameAndPresent @ 0x14053A250`, then performs
post-frame work. A whole-tick stopwatch alone will often report about 16.67 ms
even when substantial headroom exists.

`sRender__presentVsync @ 0x140541030`, despite its name, performs inline playback
and GPU synchronization or waits for the render worker. The actual device
Present occurs in `sRender__endFrameAndPresent`. EndScene timing alone misses
those later costs. See also `input_to_display_timing.md`.

With parallel rendering enabled, playback is of the preceding tick's draw list
and overlaps current simulation. Do not add worker duration to main-thread frame
duration or attribute the rendered content to the same simulation frame. Include
the actual latched rendering-thread mode and VSYNC/display configuration with
results. Keep modes in separate windows when settings change.

The first implementation should retain GPU/Present waits in the full-frame cost
and explicitly say so. Optional later instrumentation can time the primary
device Present and render worker separately. True GPU duration requires D3D9
timestamp/frequency queries, support checks, delayed nonblocking query reads,
and reset handling. Do not flush/wait for benchmark queries or subtract guessed
GPU time from CPU wall time. No GPU-query infrastructure is needed for v1.

## Low-Overhead Collector

- Disabled: a runtime gate at each coarse boundary, no QPC, allocations, logging,
  locks, sorting, packet construction, or detailed profiler activation.
- Enabled: two QPC reads per complete scope, integer accumulation, and fixed-size
  storage. Collect coarse scopes only, not every unit/hook/object.
- Start with a rolling 120 valid forward-frame window, updated for display and
  transmission at most twice per second. Use a separate bounded correction-event
  ring so rollback depth/count and sparse events remain visible.
- Keep count, total, max, and bounded raw samples for mean/p50/p95. Compute
  percentiles from a copied bounded window at publication, not in hot scopes.
  Define nearest-rank percentiles and report counts; small samples are provisional.
- Track actual resim depth, corrected-save count, and snapshot bytes/region and
  shot/effect recipe counts already available from capture. Do not scan objects
  just for the HUD. Separate warm-up/allocation samples from steady-state views.
- Keep telemetry outside game snapshots and gameplay checksums. Simulation
  replay must not rewind counters or duplicate forward-frame samples.
- Make the game thread the collector owner. Send only published copies to the
  renderer through a short locked handoff or a correctly synchronized bounded
  buffer. A plain unsynchronized struct copy is a C++ data race; do not assume
  EndScene always executes on the game thread when rendering-thread mode changes.
- Render previous completed results. EndScene can occur before the current outer
  frame finishes, so that frame's total cannot appear in its own overlay.
- Disable the detailed JSON profiler for representative measurements, or clearly
  flag it. Report snapdump/autotest/other diagnostic mode too; do not silently
  exclude costs paid by the enabled production path.

The overhead target is below 0.1% of the 16.67 ms frame budget on tested systems
(about 16.7 us/frame), including amortized publication. This is an acceptance
target, not an established measurement. Compare OFF, capture-only, and
capture+HUD+peer reporting before claiming it.

## Requests And Peer Reporting

Suggested UI: F1 performance panel with **Local**, **Local + Peer**, **Stop**,
**Reset**, a duration selector, and **Pin HUD**. Default request duration: 10
seconds, with rolling updates and a final frozen result. Pinning the display is
independent of extending capture. Peer capture never forces their HUD open.

Reuse reliable extension transport at two summaries/second; do not expand the
latency-sensitive input packet. First introduce one extension-queue dispatcher
with a full 256-byte receive buffer and route roster/benchmark magic values to
their handlers. Preserve existing roster packet behavior. Do not add a competing
`Netplay_GetExtensionPacket` loop.

Define versioned Start/Ack/Summary/Stop messages, each at most 256 bytes. Include
feature magic/version, request owner and ID, connection/battle generation,
sequence, capture kind, bounded duration, frame range, actual depth, valid sample
counts, and configuration flags. Use fixed-width fields, explicit layout/size
checks, and integer microseconds on the wire, never raw local QPC timestamps.
Use multiple identified metric-group packets if necessary rather than truncating
the result. A summary is complete only when its groups share a sequence.

Start is an idempotent capture request, not a gameplay action. Ack advertises
support and acceptance; lack of Ack becomes "peer unavailable/unsupported", not
a match stall. Clamp durations/rates, allow one bounded peer subscription, and
reject unknown versions, malformed sizes, stale generations/sequences, and
requests during invalid battle state. Simultaneous local/remote requests must
not repeatedly reset the collector or create unbounded sessions.

Attach results to the request and generation. Stop/expiry/disconnect ends remote
publication; do not allow a forgotten requester to leave profiling running
indefinitely. Use a latest-result pending slot if enqueue fails, not a backlog
of obsolete summaries. Show remote receipt age and mark stale after two seconds
without updates. A stale or unsupported peer is N/A, never 0 ms.

Passive peer windows need not start on exactly the same frame. Display both frame
ranges and sample counts. A comparison is of two observed workloads, not proof
of identical workload or hardware speed. Corrected depths can differ by peer.

## HUD

Use a small movable/pinnable ImGui table, preserving the existing HUD style:

```text
Live costs (ms)                  You mean/p95     Peer mean/p95
Save (forward)                     ...               ...
Restore                            ...               ...
Headless tick                      ...               ...
Correction, N=4                    ...               ...
Rendered tick (paced)               ...               ...
Full frame (includes rollback)      ...               ...
Frame excluding base timing         ...               ...
Samples: frames / restores / ticks     Peer age: ...
Window: ...   Mode: passive   Render thread: ...   VSYNC: ...
```

Default to the latest observed correction depth, selectable when expanded.
Show maxima, counts, frame ranges, corrected saves, and >16.67 ms full-frame
counts in the expanded panel. No corrections means restore/resim N/A, not zero.
Keep the 60 Hz budget line, but do not turn paced frame duration into a headroom
estimate or subtract independent percentiles to infer spare rollback frames.

## Controlled Training Benchmark

Only explicit training/dual-training stress mode should force corrections. Use
the production correction path on a known input sequence and available history,
with selected depths such as 1, 2, 4, and 8 (bounded by current history/config).
The existing F7 path in `hkMvc3MainMove` and forced-client resim in `autotest.cpp`
are integration precedents, not sufficient validation of a new stress runner.

Schedule peer trials by battle generation and agreed frame range, not wall clock.
Run no more than one trial per configured interval and label forced results.
Do not overwrite the pending natural correction request; coalesce or skip a trial
when a real correction is pending. Never re-enter simulation from EndScene or
the ENet thread. Do not repeatedly restore an arbitrary live state under the
assumption that rendering/audio/effect side effects are fully reversible.

## Implementation And Validation

1. Add a small `live_benchmark.h/.cpp` collector and coarse scopes at save,
   restore, resim/loop/headless, outer live tick, base timing, and render-end.
   Successful operation recording must be explicit; existing void APIs and early
   returns cannot establish success just from elapsed time.
2. Add local request controls and a published HUD snapshot. Validate timing
   boundaries in both rendering-thread modes before adding network exchange.
3. Route extensions centrally; implement bounded peer capture and two-Hz reports.
4. Add controlled training trials only after passive capture is proven harmless.

Unit-test integer aggregation, ring rollover, percentile rank, sparse/empty
samples, depth grouping, invalid operations, duration expiry, generation resets,
packet validation, stale reports, queue saturation, and simultaneous requests.
Regression-test roster extension routing explicitly.

Runtime-test capture OFF/ON with deterministic replay/checksum comparison; zero,
one, and many corrections; multi-resim ON/OFF; projectile/effect-heavy scenes;
pause/step/desync wait; restart/roster change/disconnect; and device reset.
Pause/loading/reset boundaries invalidate or suspend the window rather than
producing gameplay performance samples. Terminate a scope/window safely if its
generation changes mid-frame. Test VSYNC and rendering thread ON/OFF separately.
Run paired machines as well as local dual instances: two instances share CPU/GPU
contention and are not independent hardware benchmarks.

Build implementation with `make -j8`. Validate representative cost and observer
overhead in the running game; source inspection cannot supply those numbers.
