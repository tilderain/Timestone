# Input-to-Display Timing

**Status: Ghidra-verified native reference, 2026-08-29.** This note explains
why high-speed-camera button-to-screen measurements can vary even when the
native simulation and render-thread handoff are behaving consistently.

## Result

The native render worker does not race gameplay and does not create a
variable-depth CPU queue. Hardware input and gameplay run on the main thread.
Enabling `sRender+0x3D` adds one fixed game tick between simulation and the draw
list presented by the game.

The variable component is primarily phase quantization:

1. The pad is sampled once per 60 Hz game tick. A press immediately before the
   poll is accepted this tick; a press immediately after it waits almost one
   tick.
2. The completed image is presented at a display boundary when VSYNC is on.
   Windowed DWM composition, scanout position, panel response, and a rolling
   camera shutter add measurement phase that is not visible in game-frame
   counts.
3. A camera records both endpoints discretely. At 240 FPS, one 60 Hz interval
   is four camera frames. Results separated by roughly four camera frames are
   expected when otherwise identical presses land on opposite sides of the
   input poll.

Consequently, camera counts such as 7, 3, and 1 are not evidence that the
render worker sometimes takes seven game frames. They are camera-frame counts
across at least two unsynchronized sampling systems. A one-camera-frame result
with parallel rendering enabled is especially likely to mean that the visible
button light is not a reliable timestamp for when XInput reported the press,
or that the photographed screen transition began earlier in scanout than the
chosen visible marker.

## Verified Main-Tick Order

`sMvc3Main__move @ 0x1402594A0` executes the relevant work in this order:

```text
sMain__move                         pace the 60 Hz tick
sNetPad__processOnlineInput
  sPad__stepHelper_focusGated
    sPad__perFrameUpdate            poll hardware once
sRender__beginFrame                 hand off prior draw list if parallel
gameplay and sUnit movement
  cChrInput__processFrameInput      consume this tick's sampled input
sRender__move                       build this tick's draw list
sRender__endFrameAndPresent         finish playback and call Present
```

`sPad__perFrameUpdate @ 0x140514570` is the sole per-tick XInput/DirectInput
acquisition path. It runs before `cChrInput__processFrameInput @ 0x14002DD60`.
There is no second hardware poll later in the tick.

## Parallel Rendering

The relevant `sRender` fields are:

| Offset | Role |
|---:|---|
| `+0x38` | A rendered frame is in progress |
| `+0x39` | Presentation frame-skip control |
| `+0x3D` | Parallel-rendering enable |
| `+0x3E` | `+0x3D` latched for the current frame |
| `+0xF8` | Render-worker wake event |
| `+0x100` | Render-worker completion event |

The old Ghidra `sRender` datatype incorrectly named `+0x39` as
`mParallelRendering`. The instructions in `sRender__beginFrame` prove that the
actual enable is `+0x3D`; the datatype has been corrected.

With parallel rendering enabled, `sRender__beginFrame @ 0x140537080` does this
before current-tick gameplay:

```text
wait(renderCompleteEvent)
signal(renderWorkerEvent)
```

`sRender__workerFrame @ 0x14053D490` then plays the already completed draw list
and signals `renderCompleteEvent`. Meanwhile, the main thread simulates and
builds the next list. `sRender__presentVsync @ 0x140541030` waits for worker
completion before the primary device is presented.

The timelines are therefore:

```text
Parallel OFF: poll T -> simulate T -> build T -> draw T -> present T
Parallel ON:  poll T -> simulate T -> build T -> draw/present T on tick T+1
```

The renderer uses a three-entry dynamic vertex/index-buffer ring, but this is
resource reuse, not a three-frame presentation queue. Native device creation
requests one backbuffer. Draw playback also calls
`sRender__gpuEventSync @ 0x14053B650`, which issues a D3D event query and waits
for GPU completion before `Present`, preventing an unbounded native GPU queue.
The OS compositor or driver may still contribute latency outside this code.

## Runtime RenderingThread Override

Timestone exposes **Rendering thread** in launcher Settings and in the live
**F1 > Settings** tab. Choices are Game default (the renderer's startup value),
Force OFF (inline playback), and Force ON (worker playback). The selection is
persisted in `Timestone.ini`, `[Advanced] RenderingThreadOverride`, as -1/0/1.
The default for a missing or invalid setting is 0 (Force OFF).

The `sRender__beginFrame` hook changes only `+0x3D`, and only when `+0x38` is
clear. An active or frame-skipped frame defers the change. The native code still
owns the `+0x3E` latch, worker events, draw-buffer rotation, and presentation.
The worker is created unconditionally by `sRender__initCore` and stays asleep
when OFF; the toggle does not suspend or terminate a thread. No device reset is
requested. Switching modes can repeat or skip a displayed tick as list age
changes; gameplay continues normally.

## VSYNC

The presentation-interval table at `0x140BA84F8` begins with:

```text
0x80000000  D3DPRESENT_INTERVAL_IMMEDIATE
0x00000001  D3DPRESENT_INTERVAL_ONE
```

`sRender__createD3D9Device @ 0x14053BC50` requests one backbuffer and selects
the interval from the VSYNC setting. The main loop is also independently paced
by `sMain__move @ 0x140521DF0`. Depending on fullscreen mode, the D3D runtime,
and DWM, `Present` can introduce another refresh-boundary quantization even
though the engine-side draw-list age remains fixed.

## Measurement Procedure

To isolate the cause, record at least 30 presses for each configuration and
keep camera FPS, display mode, VSYNC, and monitor refresh fixed:

1. Parallel OFF, VSYNC OFF.
2. Parallel ON, VSYNC OFF.
3. Parallel OFF, VSYNC ON.
4. Parallel ON, VSYNC ON.

Use exclusive fullscreen if available to remove DWM as a variable. Report both
the minimum and the distribution, not one trial. Parallel ON should shift the
distribution by approximately one 60 Hz tick relative to OFF. VSYNC should
change presentation phase or spread, but it must not change the game tick on
which `cChrInput` accepts the button.

For a definitive runtime trace, timestamp these four points with QPC and a
monotonic game-frame number:

```text
sPad__pollActiveDevice returns a changed packet
cChrInput__processFrameInput receives the logical button
sRender__workerFrame starts playback (or inline playback starts)
IDirect3DDevice9::Present is called
```

That trace separates hardware-to-poll latency, simulation acceptance, the
fixed parallel-render offset, and display latency. A button-mounted LED should
be validated against the controller's XInput packet timing before treating its
first visible camera frame as the input timestamp.
