# Capturing the dominant effect cost

The old profiles identify outer effect DTIs, not generator/resource costs.
The effect capture is enabled by default, independent of the default-off
general rollback profiler. Set `TIMESTONE_EFFECT_PROFILE=0` to disable it and
avoid installing diagnostic detours. Restart the game to change this setting.

No launch option is required to capture. To disable it for a normal launch:

```sh
TIMESTONE_EFFECT_PROFILE=0 <normal-game-launch-command>
```

Reproduce the expensive rollback workload. The existing Timestone log receives
`EFFECTPROFILE_WINDOW`, `EFFECTPROFILE_SAMPLE`, `EFFECTPROFILE_PHASE` and
`EFFECTPROFILE_GENERATOR` records after each rollback window. No output or
capture-buffer allocation occurs inside the timed native callbacks.

Analyze a captured log with:

```sh
python3 tools/analyze_effect_profile.py /path/to/Timestone.log --top 20
```

## What is measured

- All recorded roots contribute phase totals; the slowest eight calls at least
  0.25 ms per window retain resource paths, effect pointers and detailed phases.
- Both ordinary game effects and base-effect moveAfter are covered. Graph creation
  outside moveAfter is also captured during restoration/resimulation.
- Owner watching, attachment, setup, joint history/before/after work, linked-child
  preparation, normal and zero-substep particle passes, ordinary/aggregate
  generators, and Model/Light/Adhesion/Force post-updates have separate timings.
  Attachment includes the existing weak-parent validation guard as well as native
  transform work; it is not a pure native-transform measurement.
- Synchronized child acquisition is measured separately from its surrounding
  post-update. Graph creation through the existing tracked factory hook is timed.
- Generator rows group by concrete vtable and descriptor within each root, with
  move/post times, requested spawns, successful-spawn counter advances and particle
  visit counts. Serial decreases are flagged instead of reported as huge deltas.
- Initial flags, authored startup frame, resulting loop count, and actual
  joint-before call counts distinguish ordinary updates from catch-up.
- Restore-time samples are marked explicitly rather than using the previous
  resimulation frame number.
- Root thread CPU cycles are recorded alongside wall-clock duration. A large
  wall-clock spike without correspondingly high CPU cycles suggests waiting or
  preemption, not necessarily excessive particle calculations. Cycle-query
  failures are flagged; cycles must not be converted to milliseconds using an
  assumed fixed clock frequency.

## Interpretation

Use exclusive phase times to locate the dominant work. Inclusive phases overlap:
do not add root, base, particle-pass and generator durations together.

| Dominant evidence | Next fix |
|---|---|
| High loop count and repeated particle/joint passes | Trace startup/INIT/restart source; eliminate duplicated reconstruction work if demonstrated, not legitimate substeps. |
| Child acquisition or graph creation | Investigate backing-storage/graph reuse and historical state restoration. |
| Model/Light post-update, outside acquisition | Implement audited presentation-only split. |
| Adhesion post-update, outside acquisition | Add query-versus-projection attribution before choosing deferred projection or query optimization. |
| Ordinary generator move, outside emission | Identify concrete particle type/descriptor; target reduced presentation, sampling or expensive movement. |
| Outer exclusive time without corresponding child timings | Trace still-uninstrumented boundary/virtual work or mod hooks; do not attribute the residual to particles. |

Missing hooks are reported in every window. Generator detail storage is bounded
to sixteen distinct vtable/descriptor pairs per root; saturation is reported.
Only the simulation thread owning the rollback is recorded. Normal netplay
serializes unit categories; worker execution would not be attributed by this
capture and requires separate investigation.

Timings include detour, clock and bookkeeping overhead, especially during large
catch-up counts. Use this capture for attribution, not as an uninstrumented
performance baseline. Metadata and reporting occur outside each root's timer,
but the broader game frame still pays for them. No per-particle clock hook or
particle-list census has been introduced.

This does not yet establish the cause of the reported spikes or a performance
gain. The code builds, aggregation and log-analysis tests run locally, but an
instrumented game capture is required to identify the large fix.
