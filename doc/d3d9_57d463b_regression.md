# Black-screen investigation: 57d463b

Reviewed 2026-09-05 against commit
`57d463b6ce4480f63183313b8e1adb1df542a866`, its parent, and current source
at `882fd6e`. Native control flow was checked in Ghidra's
`umvc3.exe.unpacked.exe` project. No affected-player log or live reproduction
was available. Existing Ghidra comments describe several historical experiments;
the conclusions below use decompiled instructions and the actual commit diff,
not those comments as evidence of installed hooks.

## Conclusion

There is a concrete graphics regression candidate, not yet proof that it caused
the reported lobby disconnects. The commit both exposes an additional real
startup Reset and converts a failed Reset into success without recovering the
device. This is consistent with overlay-sensitive black screens. Restarting the
PC is not diagnostic by itself, and the Discord report does not prove which
overlay resource or hook caused a Reset failure.

The relevant behavior remains present at `882fd6e`.

## Changes relative to the parent

All locations below are in `UMVC3Hook/dllmain.cpp`:

1. `HookMvc3MainSyncGraphicsSettings` now requires `g_firstD3D9ResetSeen`
   before suppressing pending graphics properties. The parent suppressed them
   whenever the tracked device was healthy. Until the Reset detour is entered,
   native fullscreen/resolution callbacks can now run.
2. `hkResetDevice` adds `(!firstReset || parametersMatch)` to the healthy-device
   suppression condition. A healthy first Reset with different parameters now
   reaches `oResetDevice`; the parent skipped it. Equality compares every
   presentation field, not just resolution and fullscreen.
3. When the actual Reset (including the existing fallback retry, if applicable)
   returns `D3DERR_INVALIDCALL`, the new branch logs
   `D3D9: masking failed Reset INVALIDCALL as success`, copies cached parameters
   back, sets `g_initialD3D9ResetComplete`, and returns `D3D_OK`.
   No successful recovery is required. Copying the parameters does not restore
   the device. The branch also bypasses the plugin's successful-reset resource
   recreation below it.

The chat-window changes do not directly modify the native renderer or the
network protocol. The added diagnostic counters do not themselves repair Reset.

## Native evidence from Ghidra

| Address | Function | Relevant behavior |
| --- | --- | --- |
| `0x140259BC0` | `sMvc3Main__syncGraphicsSettings` | Applies fullscreen/resolution property callbacks; an early change returns true to continue on a later frame. |
| `0x14053D690` | `sRender__applyWindowAndResize` | Releases device resources, submits Reset, then recreates backbuffer wrappers and clears renderer dirty flags. |
| `0x14051F6A0` | `sApp__resetD3DDeviceOnUIThread` | Submits UI job 4, polls job completion, and stops retrying when `sApp+0x2AF0` is zero. |
| `0x14051F930` | `sApp__updateUIThread` | Job 4 invokes device vtable `+0x80` (Reset), handles nonzero errors other than DEVICELOST, stores the HRESULT at `+0x2AF0`, and clears job type at `+0x2AC4`. |
| `0x14053DB80` | `sRender__recreateBackBufferWrappersAfterReset` | Calls GetBackBuffer and constructs texture wrappers/dependent render targets. |
| `0x1405371F0` | `sApp__proc_71F0` | INVALIDCALL takes the ERR09 abort path. |

Thus a synthetic zero HRESULT tells the UI dispatcher and submitting thread
that recovery succeeded. Native code continues into rendering-resource rebuild
without knowing that the actual device Reset failed. The original submitter's
return includes retry-ack bookkeeping; zero in `+0x2AF0`, rather than an assumed
unconditional return value of 1, is the decisive completion test.

The graphics-property caller performs one sync call per update, not a tight
spin loop. The UI submission function does have Sleep(1) polling loops; its
80-second/conditional 10-second failure checks occur after a submitted job
completes. Those checks do not bound a Reset call that never returns.

Microsoft's Reset contract says a failed Reset leaves the device lost/not-reset;
only Reset, TestCooperativeLevel, and Release methods are valid afterward.
Returning success to the caller does not change that state:
https://learn.microsoft.com/en-us/windows/win32/api/d3d9/nf-d3d9-idirect3ddevice9-reset

The existing failure diagnostics also call GetCreationParameters, GetDisplayMode,
and GetNumberOfSwapChains after Reset failure. Those calls are outside that
documented recovery contract. They predate this commit, but the newly forwarded
first Reset can expose them more often.

## Why the disconnect is not proven

`UMVC3Hook/code/netplay.cpp` services ENet on its own thread. A render/game-thread
stall does not by itself prove that ENet stops acknowledging packets. The freeze
watchdog reports stalls; it does not directly disconnect the peer.

The loading connection uses a 15-second peer timeout; normal play later requests
4 seconds (2 seconds in autotest). A timeout, a process exit after a downstream
D3D error, or an explicit peer disconnect must be distinguished using both peers'
logs. In particular, `Remote player sent disconnect notice` and
`Remote peer disconnected` represent different receive paths; the latter alone
does not distinguish all ENet disconnect causes.

## Confirmation and repair direction

Capture the complete log from startup through failure on both machines. Look for:

```text
D3D9: forwarding native Reset submission
D3D9: initial Reset requires real call
D3D9: Reset returned 0x8876086C
D3D9: Reset failure state cooperative=...
D3D9: masking failed Reset INVALIDCALL as success
```

`0x88760868` is DEVICELOST; `0x88760869` is DEVICENOTRESET. Also collect
VSync-disabled retry results, native submission completion/elapsed times,
FREEZEWATCH output when enabled, and the disconnect messages. A forwarding line
without a completion line suggests a different branch: blocked dispatch/Reset
or termination, rather than a demonstrated masked return.

Compare the parent and this commit on the same affected machine/configuration,
with a fresh game process for each overlay-on/overlay-off run. Determine whether
failure occurs before any match has worked, or only on subsequent lobby loads.
The first-Reset change is primarily a startup trigger; the INVALIDCALL masking
branch applies to later lost-device recovery as well.

Do not treat removal of the masking branch alone as a complete fix: it may just
restore ERR09 termination. A repair must avoid unnecessary healthy-device Reset
before destructive teardown, or implement actual lost-device recovery without
reporting completion until Reset succeeds. Keep native startup resource setup
and post-reset rebuilding intact; skipping all of applyWindowAndResize is not
safe (see `render_crash_bandaid.md`). Also separate Reset-observed from
Reset-completed state: the native redundant-submission bypass currently sets
`g_initialD3D9ResetComplete` but never enters the detour that sets
`g_firstD3D9ResetSeen`.

Research only: no runtime source changes or build were made. Ghidra signatures
for the Reset submitter and backbuffer rebuilder were updated to use the existing
`sApp` and `sRender` pointer types.
