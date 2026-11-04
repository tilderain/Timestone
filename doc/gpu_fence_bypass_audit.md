# GPU Fence Bypass Audit

Read-only native-code research, 2026-11-04, Ghidra port 8192,
`umvc3.exe.unpacked.exe`, base 0x140000000. Two inaccurate function comments
were corrected in the Ghidra session; no executable bytes or runtime settings
were changed for this audit. Database persistence was not attempted.

## Findings

Current experiment implementation: `FORCE_NATIVE_SLI_ON = true` replaces the
old `RENDER_GPU_FENCE_ENABLED = false` direct bypass. The device-initialization
hook forces +0x867730 after the native config read; the native SLI setter hook
forces incoming values to on. On movement, gpuEventSync is not hooked; lag
diagnostic instrumentation is omitted.
Setting the new constexpr false respects native configuration, rather than
forcing SLI off. No config file is rewritten. Gameplay-hook disabling removes
these detours; an already initialized SLI value is not retroactively restored.

The executable already supports skipping this exact barrier: native
`GRAPHICS/SLI` sets sRender+0x867730 and gpuEventSync returns immediately when
it is nonzero. That is important evidence against inevitable-crash claims,
not proof of safety on every backend or engine/plugin resource path.

Removing the fence preserves CPU command-list ownership if playback waits,
arena rotation, and resource tagging are retained. It removes explicit GPU
completion ordering before presentation and retirement. Actual D3D resource
lifetimes and implicit API synchronization must not be confused with engine
wrapper lifetimes. No inevitable GPU use-after-free was demonstrated.

## Barrier Implementation

`sRender__gpuEventSync`, 0x14053B650:

| Address | Operation |
| --- | --- |
| 0x14053B654 | Test native SLI byte +0x867730; nonzero bypasses barrier |
| 0x14053B68D | Device CreateQuery(EVENT=8) |
| 0x14053B6BB | Query Issue(END=1) |
| 0x14053B6D1 | GetData(NULL, 0, FLUSH=1) |
| 0x14053B6E0..0x14053B701 | Sleep(0), repeat GetData while result is S_FALSE |
| 0x14053B70B | Release query |

No timeout. Issue HRESULT is ignored; GetData errors also terminate the loop.
Consequently even the enabled helper does not prove GPU completion on every
possible return. Successful event completion covers preceding device work;
it is not a wait for image scanout or CPU command-list construction.

SLI setting evidence:

- 0x14053BC50 reads GRAPHICS/SLI, default false; store at 0x14053BCF9.
- 0x14053C2B0 reads the flag (isSLI).
- 0x14053F9D0 writes it and sets renderer dirty bit 0x100.
- Scalar search within sRender found the barrier test, initialization store,
  getter, and setter accesses. This is not a whole-program alias proof.

## Every Direct Caller

| Call site | Caller | Position |
| --- | --- | --- |
| 0x14053D533 | workerFrame, 0x14053D490 | After playback, before completion signal |
| 0x14054109B | sync, 0x140541030 | Inline playback, before return |
| 0x140536971 | destructor, 0x140536950 | Before shutdown flag and worker join |
| 0x140540E6C | reset release, 0x140540DB0 | After initial clear/present, before cleanup |
| 0x140540F32 | reset release | After first resource-release batch |
| 0x140540FD6 | reset release | After resource-suspend sweep |

Two additional data xrefs at 0x140C25178/0x140C25188 are not direct call sites.

## Frame Handoff

Threaded healthy path:

```text
beginFrame: rotate CPU arena; reset completion event; wake worker
worker:    play previous list -> gpuEventSync -> signal completion
main:      construct current list -> sync/wait -> Present -> retire resources
```

beginFrame 0x140537080 calls ResetEvent(+0x100) at 0x14053712C, not a worker
wait. It calls SetEvent(+0xF8) at 0x140537139. The worker calls playback at
0x14053D527, fence at 0x14053D533, and SetEvent(+0x100) at 0x14053D57F.
When renderer dirty flags are nonzero, the worker signals without playback.

sync waits on the completion event at 0x1405410EE in threaded mode. Inline
mode plays the current list at 0x14054108F and fences at 0x14054109B. It
clears frame-in-flight byte +0x38 afterward.

endFrameAndPresent 0x14053A250 calls sync at 0x14053A377 and primary Present
at 0x14053A393, then endProtect at 0x14053A513. A conditional early path
(threaded, in-flight, +0x39 enabled) polls completion until its frame deadline;
it can return without presenting if work remains unfinished. Therefore this
diagram is the ordinary path, not a claim that every invocation presents.

Bypassing the barrier does not change which command list is selected:
initRenderState 0x14053C150 selects the current arena inline and the opposite
arena threaded. The existing one-tick threaded handoff remains. Additional
GPU queueing is possible but actual queue depth/latency is not inferred here.

## CPU Arenas And Resource Retirement

allocDrawBuffer 0x140536EF0 uses two CPU arenas at +0x67650/+0x67658.
beginFrame toggles +0x67648; move 0x14053CAF0 resets/builds the current arena.
Playback 0x14053A660 consumes snapshots and wrapper pointers into ordinary
D3D state/draw calls. setShaderState 0x14053FD00 even passes stack temporary
constant data to D3D setters (example call ending at 0x140540706).

No evidence was found that the GPU directly reads these CPU command arenas.
Their reuse requires CPU playback completion, not intrinsically an idle GPU.
Do not remove the CPU wait or resource tags along with the GPU barrier.

Resource prefix: frame tag +0x08, refcount +0x0C. Resource release
0x14076F760 decrements the refcount; at zero it deletes immediately if the
tag is older than the current CPU frame, otherwise queues through
0x140536CB0. endProtect 0x14053A570 deletes queued objects with signed
tag < currentFrame and compacts the queue. No GPU completion serial is read.

updateUsageStamp 0x14031BB70 writes currentFrame + lead. beginFrame publishes
lead=1 inline or 2 threaded. A last stamp in frame F thus becomes eligible
at F+2 inline / F+3 threaded absent further updates. This protects deferred
CPU playback references; it does not bound GPU backlog.

Bindings and materials propagate tags to child resources, including
0x14031BDD0 (resource slot), 0x1404529E0/0x140452BD0 (buffer bindings),
0x14063E9C0 (targets), 0x1405E00C0 (texture), and 0x1405DD810 (state release).

Deferred destruction can release D3D VB/IB interfaces (0x14079F890 /
0x14079FD30), texture/surface interfaces (0x140781AC0, 0x140781CF0), and
CPU material/constant-state arrays (0x1405DD810, 0x1405DA3E0).
COM Release alone is NOT proof of GPU use-after-free; runtime/driver pending
work retention and bound-resource ownership remain relevant. CPU payload
consumed during playback is also not necessarily needed until GPU completion.
Unexamined external backing allocations or plugin assumptions remain a risk.

## Vertex/Index Ring Reuse

Constructor 0x140535D20 creates three VB/IB pairs, usage argument 0, at
0x140536121/0x140536152. Arrays are +0x867688/+0x8676A0. beginFrame locks
slot frameSerial%3 at 0x140537163/0x140537190, mode 3.

VB create 0x14079F900 and IB create 0x14079FDA0 choose WRITEONLY / MANAGED
for usage 0. Lock wrappers 0x14079FA40/0x14079FEE0 choose NOSYSLOCK (0x800).
DISCARD (0x2800 with NOSYSLOCK) and NOOVERWRITE (0x1800) are only selected
for usage 1 with mode 3/4. These particular ring buffers do NOT use them.

Thus buffer reuse still goes through ordinary D3D Lock/Unlock. It is not
an unsynchronized raw write into a known in-flight GPU allocation; nor does
triple buffering prove no stall. Lock can become another synchronization
point after explicit barrier removal. Actual pool/lock behavior may differ
under a device wrapper; this audit describes native requests.

## Reset And Destruction

applyWindowAndResize 0x14053D690 calls sync at 0x14053D6BE before its
cooperative-level check and reset-resource release at 0x14053D743.

releaseDeviceResourcesForReset 0x140540DB0:

1. Gamma/clear/present preparation; fence 1.
2. Virtual +0x28 cleanup; release scene/texture/record array/display wrappers
   and swapchains; fence 2.
3. Virtual +0x28 cleanup; lock resource list and suspend nDraw::Buffer-derived
   objects through virtual +0x30, set recreate byte +0x29; fence 3.
4. Virtual +0x28 cleanup; two additional Presents, with no final fence.

Virtual +0x28 resolves in base/derived renderer tables to reset 0x14053D5A0.
It clears list state and forcibly drains protected resources, not just
age-eligible entries. Suspend releases native resources for later recreation.
The actual device Reset is a subsequent UI-thread job, not one of these calls.

Destructor 0x140536950 fences first, then sets shutdown +0x10E, wakes worker,
joins its thread (0x1405369A1), and only then releases resources, device/core,
and CPU arenas. Bypassing the fence does not remove this join. It removes
the initial GPU barrier, which is distinct from CPU thread shutdown.

Native SLI skips all these barriers too. Static analysis establishes loss of
explicit lifecycle ordering, not inevitable failure without it. Keeping
lifecycle barriers while testing only per-frame bypass is a narrower change.

## Evidence-Based Pros And Cons

Proven removed costs: query creation/issue/release, FLUSH polling, Sleep(0)
iterations, and the explicit GPU drain before worker completion/Present.
Potential benefit: greater CPU/GPU overlap and less blocking at that boundary.
No universal throughput, latency, or frame-consistency improvement is proven.

Proven lost guarantee: successful GPU completion before the normal completion
signal and subsequent retirement. Potential costs: more GPU backlog/latency,
implicit waits in Present or Lock, and unverified resource/lifecycle behavior.
No extra fixed game tick of CPU playback delay is introduced by this toggle.

User fence-off captures show waits can occur in Present (35-43 ms), while
worker waits are tiny. That is compatible with deferred synchronization, not
proof that each Present stall was caused by the bypass. Separate mouse/input
stalls also occur and cannot be fixed by this barrier change.

Recommendation: experimental per-frame bypass is technically plausible;
global bypass is not certified safe or consistently faster. Preserve CPU
sync, resource tags, and D3D Lock/Unlock; retain lifecycle barriers for a
narrower comparison. Measure presentation latency and repeated frame-time
distributions, not just the render_end average, and exercise resize/reset,
capture, stage/resource churn, and shutdown before shipping.
