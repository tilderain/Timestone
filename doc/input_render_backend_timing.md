# Input and Render Backend Timing

## Development Capture

Set `LAG_BACKEND_TIMING_ENABLED` in `lag_diagnostics.h` to true and build with
`make -j8`. Use the existing manual lag-capture button on the setup exhibiting
the issue. For unattended CPU tests, also enable the local `lagTest` constexpr
in `FrameStepper_Process`, then run `wine umvc3.exe -training-cpus`.

Both flags default to false. The backend Present detour is installed only in
the backend-timing build, not in the normal build. No native polling, input
results, GPU fence, rendering, or wait behavior is skipped or replaced.

Metrics:

- `pad_detect`: native rate-limited slot rediscovery, 0x140514950.
- `xinput_state`: game's XInputGetState import thunk, 0x1409981C1. Includes
  both active polling and rediscovery probes.
- `xinput_caps`: native XInput capability query wrapper, 0x140514450.
- `dinput_find`: native DirectInput enumeration/setup, 0x140513F10.
- `input_wmi`: WMI-based XInput exclusion during DirectInput enumeration,
  0x140510220. The native routine scans Win32_PNPEntity; it is not cached.
- `dinput_read`: active kind-2 slot polling, including Poll/Acquire/GetDeviceState.
  This is a branch-level timer, not yet a separate timer for each COM call.
- `render_worker_wait`: sRender::sync in parallel-render mode, 0x140541030.
  If work is outstanding it waits on the completion event at renderer +0x100.
- `render_inline`: the same sync function in inline-render mode.
- `render_playback`: native draw-command playback, 0x14053A660.
- `render_gpu_fence`: native GPU event-query synchronization, 0x14053B650.
- `backend_present`: the primary IDirect3DDevice9::Present call.
- `render_retire`, `render_capture`: endProtect and texture-capture completion.

Additional native-self boundaries:

| Metrics | Coverage |
| --- | --- |
| `keyboard_move`, `keyboard_state` | Existing keyboard capture hook, and native keyboard update/polling |
| `mouse_move`, `mouse_state` | Native mouse move and device-state update |
| `net_component`, `native_network` | Native network component and its underlying network manager |
| `adapter_query` | Synchronous native adapter enumeration |
| `net_context_destruct` | Context destruction, including native request-drain retry loops |
| `shader_move`, `shader_table` | Shader finalization and object-table rebuild |
| `game_config_move` | Game configuration update, including platform user-name refresh |
| `game_ui_move`, `ui_screen_capture` | Existing UI move hook and capture-texture maintenance |
| `collision_begin`, `collision_move` | Native deferred collision cleanup and live collision work |
| `game_sound_move` | Game-specific sound manager, separate from `sound_move` |
| `shadow_move`, `capture_move`, `stage_move` | Shadow manager, capture/readback system, and stage maintenance |

These appear in inclusive/exclusive summaries, the correlated worst frame, and
the timestamped slow-call records. Keyboard and game-UI parent scopes reuse
existing hooks. New detours are installed only with the backend flag enabled.
No parent timer bypasses work or changes a native result. In particular,
`adapter_query` preserves its 32-bit native status return.

Timings nest. In particular, a worker wait and the render-thread GPU fence can
overlap on separate threads; do not add their durations. `LAGSLOW` records give
the start/end QPC values of the slowest selected backend call in each reporting
window when it takes at least 5 ms. They reuse the scope's existing timestamps,
with formatting and output on the diagnostic writer. They are not a complete
trace of every slow call. Compare QPC across threads on the same machine only.

## Findings: 2026-11-03

Ghidra was rechecked against `umvc3.exe.unpacked.exe`. Native slot reads branch
on slot +0x0B: zero rediscover, two DirectInput, other nonzero XInput. Native
render sync either performs inline playback/fencing or waits for the rendering
thread. The rendering worker performs playback followed by gpuEventSync before
signaling completion. gpuEventSync creates a D3DQUERYTYPE_EVENT, issues END,
then polls GetData with D3DGETDATA_FLUSH and Sleep(0) until complete.

Two local dual-CPU captures:

1. `Timestone_lag_332_capture_271373960_2.log` and
   `Timestone_lag_352_capture_271373943_2.log`: client frame 824 took 58.092 ms;
   render-end took 50.357 ms, render-worker wait 49.886 ms, Present 0.422 ms.
   The render-thread window contained a 52.570 ms GPU fence. These first logs
   lack per-call backend timestamps, so that maximum alone is not a precise
   cross-thread correlation.
2. `Timestone_lag_332_capture_271686179_2.log` and
   `Timestone_lag_352_capture_271686198_2.log`: client frame 157 took 25.013 ms.
   Its worker wait spanned QPC 2716867698181..2716867775097 (7.692 ms).
   The render-thread GPU fence spanned 2716867634749..2716867773751 (13.900 ms).
   At 10 MHz, the wait returned 0.1346 ms after the fence ended. Present was
   0.107 ms. This directly demonstrates GPU-fence completion delaying the game
   thread, not expensive draw construction or a blocked Present on this frame.

This identifies the native blocking mechanism, not why the GPU/driver took that
long. Two-second NVIDIA telemetry samples did not show sustained saturation;
they cannot exclude short GPU/compositor/scheduling stalls. Some other waits
and native-self spikes remain unexplained by these window maxima.

The pasted user captures had 7-22 ms device-read spikes, but these automated
runs did not reproduce them. They contained approximately 20 XInput probes and
five capability checks per five-second window, no DirectInput activity, and
very small backend durations. Do not attribute the original device spikes to
XInput, WMI, or DirectInput without a backend-enabled capture reproducing them.
