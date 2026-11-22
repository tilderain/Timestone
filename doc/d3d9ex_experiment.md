# Experimental self-contained D3D9Ex / FLIPEX backend

Implemented 2026-10-16. This is an experiment, not a validated latency
improvement. Windows driver/gameplay testing is still required.

## Enable and revert

Enabled by default since 2026-11-19. The executable-adjacent `Timestone.ini`
setting is read once at startup and turns it off:

```ini
[Graphics]
ExperimentalD3D9Ex=0
```

Absent or non-zero means the D3D9Ex backend; zero means the previous D3D9
backend. Where Direct3D 9 runs through Microsoft's D3D9On12 layer (`d3d9on12.dll`
loaded: Intel Arc, Iris Xe and UHD graphics, which have no Direct3D 9 driver of
their own) the previous backend is used unless the setting is `2`. Added
2026-11-19 after an Arc A750 was the one machine where 0.6.3 loaded slowly; that
the layer is the cause, and that the fallback cures it, is not yet confirmed
there. Changing the setting requires a complete game restart. If the Ex device
cannot be created (old Windows, a driver without FLIPEX, a non-system
`d3d9.dll`, a failed hook or latency call) the game falls back to the previous
backend for that run and logs why. A failure after the device is running (a
failed `Present` or texture upload) still closes the game. The Esc menu's
Settings panel shows the backend in use on its first line: `Direct3D 9Ex
(flip)`, `Direct3D 9`, or `Direct3D 9 (9Ex unavailable)` after a fallback. The frame queue is always capped at one frame
(`SetMaximumFrameLatency(1)`); there is no setting for it. This does not change the user's CAPCOM display configuration or
render-thread setting.

No external graphics wrapper is needed. The experimental backend requires the
loaded `d3d9.dll` to be the system DLL; with a local dgVoodoo/DXVK/ReShade D3D9
proxy the game falls back to the previous backend rather than combining the
proxy with native resource adaptations.
Start testing with third-party overlays disabled. Wine/Proton support is not
established; the system-DLL path check alone does not establish native Windows
driver compatibility.

For the initial presentation comparison, use desktop-resolution borderless
fullscreen through Timestone's existing fullscreen option. Keep refresh rate,
VSync and RenderingThread identical between runs. Check PresentMon for
`Composed: Flip` or `Hardware: Independent Flip`; Independent Flip is dependent
on Windows/driver/window configuration, not guaranteed by FLIPEX. Measure actual
button-to-photon latency separately. A one-frame queue limit is not a promise of
one-frame total latency or a one-frame reduction.

## Device and reset integration

- Only the native renderer creation job can select Ex. The common CreateDevice
  hook retains its existing dual-process serialization; helper devices remain
  unchanged. EndScene hooks are obtained from the actual game device.
- Creation uses `Direct3DCreate9Ex` / `CreateDeviceEx`, windowed FLIPEX, two
  requested backbuffers, no swapchain MSAA or lockable backbuffer, and
  `SetMaximumFrameLatency(1)`. VSync selection is preserved.
- Native fullscreen requests still become borderless. Runtime resets normalize
  the same presentation parameters and pass through the existing managed Reset
  boundary and overlay Reset chain, with the backend operation changed to
  `ResetEx`. Static default-pool resources persist across ResetEx.
- Cooperative probes use `CheckDeviceState`. Occlusion is not fatal; a mode
  change requests native recovery. Device failures remain failures, not fake
  successful presents/resets. The prototype requires restarting after a fatal
  presentation/device error rather than attempting full device recreation.
- Effective primary swapchain parameters must report windowed FLIPEX. Logs show
  actual swap effect and buffer count, and an `active FLIPEX` startup message.
- Additional swapchains are not converted. Their native Present path supplies
  a destination HWND, which is invalid for FLIPEX.

## Present with VSync on must not wait inside Direct3D

Found 2026-11-19 from a Windows 0.6.3 log. The device is created MULTITHREADED
(behavior 0x46), so every call takes the device's lock, and the game's loader
thread creates and fills textures through it while the game thread presents.
With VSync on and a one-frame queue, `Present` waits inside Direct3D for the
queue to empty: 16 ms of every 16.7 ms frame (`present_max=16.3`, work about
1 ms). If that wait holds the lock, the loader gets it for about a millisecond a
frame. The log fits: 363 textures took 845 ms inside the native create (2.3 ms
each, up to 7 ms each in other windows) while GPU creation and uploads together
took 64 ms; under Proton with VSync off 2338 textures took 100 ms. On the
character select a character's textures took about 5 s to appear. The ordinary
backend queues up to three frames, the game's own 60 fps pacing keeps that
queue from filling, and `Present` there rarely waits.

`PresentFrame` first tries the Ex device with `D3DPRESENT_DONOTWAIT` when VSync
is on, which presents the frame if the queue has room. On
`D3DERR_WASSTILLDRAWING` it waits for the next refresh in `DwmFlush`, which is
not a Direct3D call and leaves the lock free, and then makes the ordinary
waiting `Present`. The frame is therefore still paced by the driver's VSync;
only where the wait is spent changes. Without a compositor to wait for, the
ordinary `Present` does all the waiting as before. A runtime that refuses the
flag is presented the ordinary way from then on (logged once).

Polling the queue with a timer was tried first and is not used: with 1 ms
sleeps a Windows log showed frames starting up to 1.9 ms late (5 to 38 of every
120 more than a millisecond off, against none before), and the texture create
time fell from 845 ms to 56 ms for a similar batch. The `DwmFlush` version has
not run on Windows: the checks are `late=0` in the `PACING` lines again and the
`fill` figure in the `D3D9Ex: textures created=` lines staying low.

## Managed resource adaptation

The native game cannot run on a bare Ex device because it uses MANAGED resources.
The adapter is game-specific, not a general COM implementation of D3D9 managed
resources. It never returns fake COM textures to the renderer.

| Native function | Address | Relevant behavior |
| --- | --- | --- |
| `rTexture__convert` | `0x140515240` | Ordinary TEX loading passes usage 0, selecting MANAGED |
| `MtTexture__createImpl` | `0x140782060` | Creates/uploads 2D, cube and volume textures; owns COM pointer at +0x48, shared native owner at +0x50 |
| `nDraw__Texture__map` | `0x1407832C0` | Direct texture LockRect/LockBox; output is pointer, row pitch, slice pitch |
| `nDraw__Texture__unmap` | `0x140783A40` | Direct texture unlock, cube face/mip decomposition |
| Native buffer base constructor | `0x1407A31F0` | Usage controls registration for native suspend/resume |
| Vertex creation/resume | `0x14079F900` / `0x14079FBA0` | Static buffers use MANAGED + WRITEONLY |
| Index creation/resume | `0x14079FDA0` / `0x1407A0040` | Static buffers use MANAGED + WRITEONLY |

For managed texture creation, the adapter temporarily selects native staging
usage (3), allowing the existing engine code to perform format-specific initial
uploads into SYSTEMMEM. It then creates a matching DEFAULT GPU texture. The
staging texture is retained in an IUnknown private-data object owned by the GPU
texture. This keeps shared native texture views consistent and releases the CPU
copy automatically on final GPU texture release.

Nothing is uploaded at creation (2026-11-19). The managed pool the game was
written for sends a texture to the GPU when it is first drawn; uploading every
texture whole inside the create call instead put that cost into each character
and stage load. 0.6.3, the first release with this backend on by default, was
reported as loading noticeably longer in character select and into matches,
and in netplay the opponent reached the start barrier more than 1.5 s after a
Proton machine on every rematch (0 of 6 before, 5 of 5 after). That the upload
is the cause was not measured on a Windows driver. A new texture is now listed
in `pendingTextures` and goes up whole by `UpdateTexture`:

- when it is first bound (the `SetTexture` hook, device vtable 65), as the
  managed pool does; or
- before that, in `WarmTextures`, which runs once a frame ahead of Present and
  uploads waiting textures for up to 6 ms while the game is loading (a texture
  was created in the last 250 ms) and 1 ms otherwise or in a synchronized
  netplay battle, so that few are left for their first draw. One found still
  mapped is tried once a frame and does not hold the others up.

A texture the game still has mapped is skipped until it is unmapped, and one
the game released before any use is dropped without an upload. The list holds
a reference on the GPU texture and its staging object, so neither can be freed
while listed. `SetTexture` costs one atomic load when nothing is waiting.

Later native map/unmap calls use staging storage. While a texture's first
upload is still owed an unmap sends nothing. After it, a 2D or cube texture
uploads only the mip (and face) that was mapped, with `UpdateSurface`,
even while other levels are still mapped. A single-level 2D texture (the
per-frame ones, such as `uModel__updateShaderAttributes`' 256x1) goes up with
one `UpdateTexture` call instead, which sends what the lock marked dirty. Volume textures have no per-level
surface copy: they are marked dirty and uploaded whole with `UpdateTexture`
once nothing is mapped. The adapter preserves the native static-map behavior,
including ignoring the rectangle argument for volume textures. Unadapted
textures retain native map and unmap paths.

A temporary log line (`D3D9Ex: textures created=...`, at most every two
seconds) gives the time spent filling CPU textures, creating GPU textures, and
uploading at first draw and ahead of use, for comparing a load on a real
driver. Not yet run in the game on any driver; `tools/test_d3d9ex.py` covers
the paths against mocks.

The CPU copy is kept for the life of the texture, as the managed pool itself
does: a default-pool texture cannot be read back, and native callers map
existing textures (`sScene__initSysTexture`, `rDeformWeightMap__load/save`).

Managed vertex/index creation is translated on the tracked Ex device only.
Native managed usage metadata is not rewritten, and ResetEx preserves these
static resources. Dynamic, render-target, depth/stencil and explicit staging
resource paths stay native.

A static buffer the game fills as it creates it (`nDraw::VertexBuffer::create`
/ `IndexBuffer::create` given data, which is how archives load their models) is
made in SYSTEMMEM, not write-only, and listed in `pendingBuffers` (2026-11-20).
Creating these in the default pool put a GPU allocation and a driver lock per
buffer on the loader's decoder threads, inside the archive decode: on a Windows
NVIDIA machine `stg\102\0000` decoded in 1.6 s on every load against 0.13-0.5 s
under DXVK, and a match load took 3.3 s against about 1 s, with textures
costing about 5% of it. That buffers are the cause was inferred, not measured.
`WarmBuffers` runs once a frame before Present with the same budget rule as
`WarmTextures` (its own 6 ms while loading, 1 ms otherwise): it creates the
default-pool write-only buffer, copies the data in, and stores it in the game's
object (+0x30) in place of the copy. Until then the game draws from the copy,
which Direct3D allows on a hardware-vertex-processing device and is only slow.
The GPU buffer keeps the copy alive through private data, as the managed pool
kept one. The buffer destructors (`0x14079F890`, `0x14079FD30`) take an object
off the list, and the list stays locked for the whole of a send, so an object
cannot be destroyed under one.

Not deferred: a buffer created empty (it is filled later through a lock this
does not see) becomes a default-pool buffer at creation, as before. Not
handled: the game locking a listed buffer again while it is being sent (the
copy could be read half-written, and a later edit to the copy would be lost);
no such lock of a static buffer is known. `[Graphics] D3D9ExStageBuffers=0`
turns the deferral off. The `D3D9Ex: buffers ...` log line gives counts and
times for both routes. Not yet run in the game on any driver.

## Stepping back towards 0.6.3 (2026-11-21)

One Windows machine (RTX 3060, VSync on) ran 0.6.3 without trouble and from
0.6.4 showed hundreds of frames a second in menus, a black screen that never
ends at menu changes, and once `E_FAIL : mpVertexBuffer->Lock(0,0,&pbuf,flags)`
(the game's result checker exits on any failed call) with an access violation
on an `nvd3dumx.dll` worker thread. A log of a hang has no failed call, no
staged buffer (`filled in memory=0` throughout) and the `DONOTWAIT` present in
use. Only that present path can change how fast frames are presented with
VSync on, so it is off unless asked for; that it is also the hang is not
shown. Each change since 0.6.3 has a `[Graphics]` switch:

- `D3D9ExPlainPresent` (default 1): one ordinary `Present` a frame. `0` brings
  back the `DONOTWAIT` try and `DwmFlush` wait (VSync on) and `FORCEIMMEDIATE`
  (VSync off). With it on, the loader waits behind Present again as in 0.6.3.
- `D3D9ExDeferTextures=0`: textures uploaded whole at creation.
- `D3D9ExStageBuffers=0`: no system-memory static buffers.

Nobody is asked to set these by hand. A failed Direct3D call on the Ex device
outside a graphics transition, or a loader that makes no progress for 20 s
while frames are drawn (`LoadDiag`), raises `[Graphics] D3D9ExSafeLevel` by one
(once a run, stored with `D3D9ExSafeVersion`), and each start applies that many
steps: 1 textures at creation, 2 and no staged buffers, 3 and the ordinary
backend (`ExperimentalD3D9Ex=2` still forces Ex). The log names the level and
the three choices at start. A count from another version is ignored.

For a hang the log gets where every thread is (`D3D9: thread listing`): when the
loader is stuck, when the game thread begins no frame for 10 s, and (temporary)
when the window is closed. `D3D9Ex: N presents in M ms` is written when frames
are presented faster than 100 a second.

A failed call also logs `D3D9: failure callers:` (the game's call chain) and
`D3D9Ex: state at failure:` (present counts, whether the failure fell between
the two present calls of a frame, device state, waiting and staged counts).
None of this has run in the game.

Staging covers less than was assumed: `rModel__convert` creates a model's main
vertex and index buffers empty and fills them through the lock wrappers
(`0x14079FA40` / `0x14079FEE0`), so they are still made on the GPU on the
decoder threads. Only the buffers created with data (model `+0x1D0` / `+0x1D8`)
are staged. Static buffers are locked after creation by `rModel__convert`,
`rDeformWeightMap__load/save`, `uSky__createBuffer`, `rRibbon__rebuildGeometry`
and the `sScene` primitive builders.

This retains CPU texture copies and adds explicit uploads; it can increase
upload cost and does not guarantee improved throughput. It does not emulate
every managed-resource operation for third-party code. Unknown resource paths,
unusual modded formats, and external overlays require runtime verification.

## Verification

- `python3 tools/test_d3d9ex.py`: runs production adapter functions with COM
  ownership mocks under AddressSanitizer/UndefinedBehaviorSanitizer. Covers
  2D/cube/volume staging, shared aliases, mip edits, deferred upload, final release,
  failure propagation, parameter policy, resource-device scoping and Ex status/reset handling.
- `python3 tools/test_d3d9reset.py`: existing managed reset/admission tests,
  including Ex-only FLIPEX validation. The stale expectation of twelve controller
  hooks was corrected to the current eleven (WM_PAINT hook is no longer there).
- `make -j8`: cross-compiles and links the ASI and normal package.

Mocks cannot prove native ABI/callsite coverage or actual D3D9Ex behavior.
Runtime checklist: startup and menus; character/stage loads; superflashes;
repeated rematches; texture-heavy modded skins; ImGui; frame stepping; replay;
resolution/VSync changes; minimize/restore; alt-tab; dual-process training;
online matches with mixed legacy/Ex peers. Compare memory and frame-time spikes
as well as latency. Do not promote this backend to default based on build success.

References:

- https://learn.microsoft.com/en-us/windows/win32/direct3d9/d3dpool
- https://learn.microsoft.com/en-us/windows/win32/direct3darticles/direct3d-9ex-improvements
- https://learn.microsoft.com/en-us/windows/win32/api/d3d9/nf-d3d9-idirect3ddevice9ex-resetex
