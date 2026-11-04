# Native Self-Time Stall Investigation

## Scope and Evidence

2026-11-03 static analysis of `umvc3.exe.unpacked.exe`, Ghidra port 8192,
image base 0x140000000. Compared native dispatch against current
`rollback_hooks.cpp`, `controller.cpp`, and the lag metrics. No runtime behavior
changes or new capture runs were made for this investigation.

Target frames from the supplied captures:

| Frame / Thread | Outer frame | Native | Native self |
| --- | ---: | ---: | ---: |
| 203 / 428 | 111.707 ms | 110.241 ms | 94.913 ms |
| 176 / 780 | 103.852 ms | 102.390 ms | 88.396 ms |

Those particular frames have tiny gamepad polling, rendering, GPU-completion
wait, and Present times, and no recorded correction. They are distinct from
the separately correlated GPU-fence stalls. No scope overflow or frame
reentry was reported. The final partial capture windows are not used here.

`Native` brackets the original sMvc3Main::move call (0x1402594A0). Its self time
includes uninstrumented native calls, code in plugin callbacks reached from
those calls, blocking, and time when the thread is not scheduled. It is not
CPU-time accounting. A static candidate is not proof that its expensive branch
ran during either frame.

## Highest-Value Missing Boundaries

### Keyboard and Mouse Are Separate From Gamepad Polling

| Function | Address | Native vtable |
| --- | --- | --- |
| sKeyboard::move | 0x1406A9D40 | 0x140BB6390, slot +0x30 |
| sKeyboard::updateState | 0x1406AA1B0 | Called from keyboard move |
| sMouse::move | 0x1406AA990 | 0x140BB64E0, slot +0x30 |
| sMouse::updateState | 0x1406AAD00 | Called from mouse move |

Both constructors set mActive=true and attempt DirectInput device creation.
Keyboard constructor 0x1406A96C0 configures cooperative flags 0x15; mouse
constructor 0x1406AA4A0 uses 0x0A and initially acquires the device.

Both updateState functions contain this real native path:

1. Poll the DirectInput device (vtable +0xC8).
2. On failure, call Acquire (+0x38).
3. Repeat Acquire while the result is 0x8007001E (DIERR_INPUTLOST).
4. Read state through GetDeviceState (+0x48).

There is no explicit retry count or elapsed-time limit on that Acquire loop.
This makes loss/reacquisition and backend blocking concrete candidates. It does
not mean the loop was active in the capture. Polling is gated by the app-active
byte and subsystem active flag. If the keyboard device is null, its fallback
queries GetAsyncKeyState for 256 keys instead; the DirectInput path also checks
five special keys through GetAsyncKeyState after decoding.

Mouse move additionally uses window/cursor APIs, including clipping and
coordinate conversion, outside the gamepad timer. Its show-cursor helper only
publishes two fields; it does not synchronously wait for a UI job.

**Important exclusion correction:** `pad_read`, `dinput_read`, and `native_input`
do not cover these independent keyboard/mouse systems. A tiny gamepad reading
does not exclude native keyboard/mouse stalls.

### Native Network Maintenance, Not Opponent Input Waiting

The game calls two distinct network objects. Manager-owned sAppNetwork::move
is a no-op. Main-owned mpNetwork calls:

```text
main callsite 0x140259813
  nNet::sComponent::move                 0x140458320
    sNetwork::move                      0x1406864D0
      MtNetCore::move                    0x1404F1C10
        MtNetMediator::move              0x1404DACE0
          getInterfaceInformation        0x1404DA610
```

The last function synchronously calls GetAdaptersAddresses(AF_INET, 0, ...),
allocates a buffer, queries again, traverses results, and frees the buffer.
Mediator move enters this path if the previous enumeration result at +0x1E8
was nonzero, or the address-change event at +0x248 is signaled. Failed results
can cause retries on following moves. Its event check has a zero timeout;
the enumeration itself, not that event check, is the potentially expensive call.

A separate state-dependent path is:

```text
sNetwork::move
  nNetwork::Context::move                0x140913F40
    releaseSessionSlot                  0x1409143D0
      deleting destructor               0x140508850
        MtNetContext::beginDestruct       0x1404FAF70
```

Context shutdown/destruction cancels and pumps outstanding requests, with
Sleep(0x11) retry loops and no explicit timeout in the inspected loops.
This is concrete blocking code, but needs context shutdown/outstanding-work
state. A roughly 100 ms frame alone is not evidence for six 17 ms sleeps.

Custom netplay having low ping or no prediction wait does not exclude either
native network-maintenance path.

### Shader Finalization Outside Render Construction

Main callsite 0x1402595FE invokes sMvc3Shader2's inherited move at 0x14065CB20
(derived vtable 0x140B09120, slot +0x30).

- Dirty byte +0x64 invokes initObjectTable at 0x14065BE60: frees/reallocates
  tables, uses a 0x80000-byte temporary allocation, computes CRCs, performs
  pairwise object-ID conflict checks, and sorts the object index.
- Pending indices +0x86078/+0x8607C drive a 1024-entry ring at +0x80078 with
  0x18-byte records. Ready entries are processed without a per-frame time budget.
- Ready-resource handling calls createShader 0x14065B2A0, then vertex shader,
  pixel shader and input-layout helpers. Cache misses call D3D9
  CreateVertexShader, CreatePixelShader and CreateVertexDeclaration.
- Resource finalization calls cResource::releaseRef 0x14050D5A0 and
  sResource::release 0x140519BB0. The latter conditionally locks the resource
  table; a zero reference count leads to virtual destruction after unlocking.

The move returns immediately for a resource that is neither ready nor failed;
it does not spin waiting for that resource. A separate requestShader function
contains a wait loop, but it is not called by this move path. Active producers
of the dirty flag and pending queue were not established in this pass, so
neither branch should be assumed active during the spikes.

## Other Real Coverage Gaps

- **Collision:** beginFrameAndDrainDeferredState 0x1405BB710 flushes staged
  collision arrays, drains merge/retire lists, and can acquire the collision
  critical section. Native move 0x1405BAF60 processes node banks, meshes/BVHs
  and collider work, also under conditional locking. Retire queues can remove
  leaves, shift arrays, and conditionally invoke deleting destructors. Small
  `save_clones`/`restore_clones` timings do not measure native collision work;
  the detailed collision profiler scopes in rollback_tick.cpp are in headless
  replay, not live native move. Workload/counter state is needed to judge this
  candidate. The +0x48 end-phase callback 0x1405BB7A0 mainly clears flags.
- **Game sound:** sGSound::move 0x1402AA6C0 is different from the measured
  sSound::move. It conditionally locks and processes voice lists, dispatches
  cue work, stops/releases voices, and may invoke deleting destructors. The
  structured-request path uses a 64-entry pool, not a proven heap allocation
  on every request. Backend timing is unmeasured here.
- **Steam user text:** sGameConfig::move 0x140253490 refreshes the platform
  user text after 60 increments when nonempty. The accessor 0x140213160 obtains
  the SteamFriends interface (bootstrap explicitly requests SteamFriends015),
  then the move calls its first virtual method, GetPersonaName. If the network
  slot fields +0x444/+0x448 are negative, it can also enter the session/name
  refresh path. The external implementation's cost is not established.
- **UI texture allocation:** sGameUI::move 0x1402BE610 is not a lag scope in the
  current build. Its updateScreenCapture helper 0x1402B9670 creates the capture
  texture when missing and releases/recreates it on a viewport-size mismatch.
  Optional UI tasks and popup/DLC phases also tick here. This differs from
  sRender::completeTextureCapture, already measured by `render_capture`.
- **Shadow:** sShadow::move 0x1406D3100 can traverse unit lists, lazily allocate
  per-view state and create four occlusion-query objects, then drain available
  query results. Its GetData path returns on the first unavailable result;
  it is not a busy wait for GPU completion.
- **Capture system:** sCapture::move 0x14069CB50 is after render-end. When
  byte +0x14D is zero (constructor default), it returns immediately. When active,
  it can perform GetRenderTargetData, texture lock/readback, allocation/copy,
  file/codec output, and capture teardown. An inactive capture excludes it.
- **Stage retirement:** sStage::move 0x1401F0AA0 calls reapDeadStageSlots
  0x1401F03D0 under a stage-array critical section. Default array auto-delete
  is false, so array removal alone is not proof of resource destruction.

## Paths Excluded or Downgraded

Constructor/vtable inspection resolved these as actual return stubs:

| Native call | Concrete vtable / target |
| --- | --- |
| sMvc3Resource +0x48 | 0x140B135F0; 0x1401FE880 jumps to 0x1407E1A10 |
| sMvc3Tool +0x48 and +0x30 | 0x140B139B0; 0x1407E1A10 |
| sPlatformUtil +0x30 | 0x140BB6470; 0x1407E1A10 |
| sPrimitive +0x30 and +0x50 | 0x140BB1AC0; 0x1407E1A10 |
| sMvc3Manager +0x30 | 0x140B09480; 0x1407E1A10 |
| manager-owned sAppNetwork +0x30 | 0x140B0C150; 0x1407E1A10 |

sPrimitive::begin 0x14064CB80 resets existing tag/buffer state; no allocation,
driver call or wait was found in the inspected chain. sGameConfig's per-frame
battle-audio volume setters store/clamp values, rather than calling the audio
driver. sVibration::move processes eight control entries; its inspected body
does not perform a blocking device poll.

The direct SteamAPI_RunCallbacks call is in sApp::execute, outside the measured
sMvc3Main::move tail. That does not exclude indirect contention with Steam, but
it is not evidence that this native-self time is the callback pump.

No unconditional Sleep(100) explanation was established. Do not infer a fixed
100 ms timer merely from the two observed frame durations.

## Bounded Next Measurement

First add only these missing parent boundaries, with thread CPU time at the
outer native boundary to separate execution from elapsed blocking/preemption:

1. Keyboard move 0x1406A9D40 and mouse move 0x1406AA990.
2. Native network component move 0x140458320.
3. Shader move 0x14065CB20, recording dirty byte and queue indices at entry/exit.
4. Game-config move 0x140253490 if the first three groups remain small.

If a parent is large, narrow within it: DirectInput Poll/Acquire/GetDeviceState,
adapter enumeration/context destruction, shader creation/resource release, or
Steam name refresh. Only then expand to live collision/UI/sGSound boundaries.
This is an instrumentation priority, not a probability ranking or a proven fix.

Do not remove acquisition loops, resource destruction, synchronization, or
network processing based on this static analysis. Low thread CPU time can mean
blocking or descheduling; high CPU time can mean work or spinning. A scheduler
trace or a stack captured during the stall may be needed for the final cause.
