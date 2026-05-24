# Threading Architecture

## Overview

UMVC3 uses Capcom's MT Framework engine, which includes a **ConcRT-backed job
system** for parallel work. The game simulation itself is **single-threaded**
(`sMvc3Main::move` runs on the main thread at 60 fps), but the engine can
offload parallel work to a pool of worker threads between frames.

The hook adds **three extra threads** for rendering (ImGui/D3D11 overlay),
general hook processing, and netplay I/O.

---

## ConcRT (Concurrency Runtime)

ConcRT is Microsoft's parallel programming framework (VS 2010). UMVC3 uses it
only for the **scheduler and thread pool** — not task groups or
`parallel_for`. The job system (below) is custom game code that uses ConcRT
as the thread creation and synchronization backend.

### ConcRT Functions in the Binary

| Address | Symbol |
|---------|--------|
| `0x14002dc60` | `Concurrency::details::SchedulerProxy::GetNumExternalThreads` |
| `0x1400383f0` | `Concurrency::details::SchedulerProxy::GetNumNestedThreadSubscriptions` |
| `0x1403279d0` | `Concurrency::details::UMSThreadVirtualProcessor::GetDefaultDestination` |
| `0x14032d880` | `Concurrency::details::UMSThreadProxy::GetId` |
| `0x1403e72b0` | `Concurrency::details::UMSFreeThreadProxy::GetContext` |

ConcRT provides UMS (User-Mode Scheduling) threads — the kernel is not
involved in context switches between worker threads, making wake/sleep cycles
cheaper.

### ConcRT Calls in the Job System

The job system calls ConcRT through indirect call thunks (`DAT_00c50xxx`):

| Thunk | Likely API | Used In |
|-------|------------|---------|
| `DAT_00c5028e` | `Context::Create` | Creating worker contexts |
| `DAT_00c502b4` | `Context::Schedule` | Binding a context to a thread |
| `DAT_00c502e8` | `Context::Unsubscribe` | Releasing a context |
| `DAT_00c502d8` | `Context::Block` | Blocking a context |
| `DAT_00c5026c` | `Context::Yield` | Yielding execution |
| `DAT_00c50260` | `Event::Set` | Signaling thread readiness |
| `DAT_00c501d0` | `Event::Wait` | Waiting for work |
| `DAT_00c503b4` | (thread resume) | Resuming a delay thread |

---

## Job System (`sMain`)

### Thread Pool Configuration

The job system is managed by the `sMain` singleton. See [`main.md`](main.md)
for the full object layout at `+0x98` through `+0xac`.

```
sMain +0xa8:  desired_thread_count   (set by setJobThreadNum)
sMain +0xa4:  active_thread_count    (copied from +0xa8 each frame)
sMain +0xa8:  max_job_threads        (capped at 16)
sMain +0xac:  delay_thread_count     (up to 8)
```

### Thread Count Limits

`sMain::setJobThreadNum` (`0x140522450`):

```c
void sMain__setJobThreadNum(sMain* self, uint count) {
    if (count == 0) count = 1;
    if (count > self->maxSupported)       // +0x40088 from checkCPUCaps
        count = self->maxSupported;
    if (count > 16) count = 16;
    self->desired_threads = count;        // +0xa8
}
```

`sMain::getJobThreadNum` (`0x1408f5140`):

```c
uint sMain__getJobThreadNum(sMain* self) {
    return self->desired_threads;         // +0xa8
}
```

### Job System Initialization (`sMain::initJobSystem_maybe` @ `0x140520fc0`)

Called lazily on the first `sMain::executeJob` call. Creates **15 worker
slots**, each with:

1. Two ConcRT contexts (primary + secondary)
2. A ConcRT thread running `sMain::jobloop` (`0x140521760`)
3. A thread ID (1..15) and a ready event
4. All 15 threads are released and their ready events are waited on

```c
void sMain__initJobSystem_maybe(sMain* self) {
    int numCores = getNumProcessors();              // GetSystemInfo
    self->coreCount = numCores;                      // +0x3fad8

    for (int i = 1; i < 16; i++) {                   // 15 workers (id 1..15)
        void* ctx1 = Context::Create(0, 0, 0, 0);    // primary context
        void* ctx2 = Context::Create(0, 0, 0, 0);    // secondary context
        slot.exitFlag = 0;
        slot.readyEvent = Event::Create(0, 0);
        slot.thread = Context::Schedule(0, 0,
            sMain__jobloop, slot_ctx, 4, slot_param);
        slot.threadId = i;
        Context::Unsubscribe();                       // release
    }

    for each slot {                                   // wait for ready
        Context::Block(slot.thread);
        Event::Wait(slot.readyEvent, INFINITE);
    }
}
```

### Job Thread Main Loop (`sMain::jobloop` @ `0x140521780`)

Each worker thread loops, waiting for work from the global job queue:

```c
void sMain__jobloop(void* param_1, JobSlot* slot) {
    Event::Set(slot->readyEvent);        // signal "I'm alive"

    while (slot->exitFlag == 0) {
        Event::Wait(slot->workEvent, INFINITE);
        if (slot->exitFlag) break;

        // Read jobs from global queue at 0x140e177e8
        int mode = *(int32*)(0x140e177e8 + 0xa0);
        int count = *(int32*)(0x140e177e8 + 0x98);
        int* index = (int*)(0x140e177e8 + 0x9c);

        while (*index < count) {
            LOCK();                     // interlocked increment
            int idx = *index;
            (*index)++;
            UNLOCK();

            if (idx >= count) break;

            switch (mode) {
            case 0:                     // no extra args, 0x10-byte entries
            case 1:                     // single context arg, 0x10-byte
                job = jobs[idx];        // 0x10 bytes each
                job.func(job.arg);
                break;
            case 2:                     // param + size, 0x20-byte entries
            case 3:
            case 4:
                job = jobs[idx];        // 0x20 bytes each
                job.func(job.arg, job.size);
                break;
            case 5:                     // 3 args, 0x20-byte entries
                job = jobs[idx];
                job.func(job.arg, userCtx, job.size);
                break;
            }
        }

        Event::Set(slot->readyEvent);    // signal idle
    }
    return 0;
}
```

### Six Scheduling Modes

Controlled by `*(int32*)(0x140e177e8 + 0xa0)`:

| Mode | Entry Size | Args to func | Description |
|------|-----------|--------------|-------------|
| 0 | 0x10 | `(arg)` | Sequential, no context |
| 1 | 0x10 | `(arg, ctx)` | Sequential, single context |
| 2 | 0x20 | `(arg, size)` | 2 args from entry |
| 3 | 0x20 | `(arg, size)` | Same layout as 2 |
| 4 | 0x20 | `(arg, size)` | Same layout as 2 |
| 5 | 0x20 | `(arg, ctx, size)` | 3 args (extra ctx param) |

### Job Execution (`sMain::executeJob` @ `0x140521230`)

Called by the game when it has parallel work to dispatch:

```c
void sMain__executeJob(sMain* self, int mode) {
    if (self->jobCount == 0) return;                // +0x98

    if (!self->jobSystemInited) {                   // +0x7e
        sMain__initJobSystem_maybe();
        self->jobSystemInited = 1;
    }

    self->jobIndex = 0;                             // +0x9c
    self->jobMode = mode;                           // +0xa0

    if (self->jobCount == 1 || self->activeThreads == 1) {
        // Single-threaded: run inline
        sMain__scheduleAndWaitJob_maybe(&self->jobQueue);

    } else {
        // Multi-threaded:
        setJobSystemBusy(1);

        // Wake all workers
        for each worker slot {
            if (slot->active) {
                Event::Set(slot->workEvent);
                Context::Yield(slot->context);
            }
        }

        // Main thread participates too
        sMain__scheduleAndWaitJob_maybe(&self->jobQueue);

        // Wait for all workers to finish
        for each worker slot {
            if (slot->active) {
                events[numEvents++] = slot->readyEvent;
            }
        }
        Event::WaitMultiple(numEvents, events, true, INFINITE);

        setJobSystemBusy(0);
    }

    self->jobCount = 0;                              // +0x98
}
```

---

## Delay Job Thread System

A **separate thread pool** for time-deferred work (graphics asset loading,
texture streaming, cleanup tasks). Runs independently of the frame timing.

### Creation (`sMain::createDelayJobThread` @ `0x1405202c0`)

```c
void sMain__createDelayJobThread(sMain* self, uint count) {
    if (count > 8) count = 8;

    for (int i = 0; i < count; i++) {
        slot = &self->delaySlots[i];          // stride 0x38
        slot->exitFlag = 1;
        slot->exitFlag = 0;

        slot->thread = Context::Schedule(0, 0,
            sMain__delayJobloop, slot_ctx, 4, slot_param);
        slot->threadId = i;

        Context::Block(slot->thread);
        Event::Wait(slot->workEvent, INFINITE);
        Context::Unsubscribe();
    }

    // Resume all threads
    for each slot {
        Context::Block(slot->thread);
    }
}
```

### Delay Job Loop (`sMain::delayJobloop` @ `0x1405210e0`)

```c
int sMain__delayJobloop(sMain* self, DelaySlot* slot) {
    while (slot->exitFlag == 0) {
        // Wait for work
        while (Event::Wait(self->delayEvent, INFINITE) != 0);  // +0xc0

        // Guard with critical section
        if (self->exitFlag || g_criticalSectionEnabled)
            EnterCriticalSection(&self->cs);                    // +0x8

        uint64 readPt = self->delayReadPt;                     // +0xb8
        uint64 writePt = self->delayWritePt;                   // +0xb0

        if (readPt == writePt) {
            // Ring buffer empty
            LeaveCriticalSectionIfNeeded();
            Sleep(1);
            continue;
        }

        // Claim a job from the ring buffer
        if (readPt < writePt) {
            self->delayReadPt = readPt + 1;
            job = &self->delayRing[readPt & 0xFFF];            // 4096 entries
        }

        LeaveCriticalSectionIfNeeded();

        if (job == NULL) { Sleep(1); continue; }

        // Execute based on type
        if (job->type == 0 || job->type == 2) {
            void (*func)(void*) = job->func;
            job->func = NULL;
            if (func) func(job->arg);
            job->arg = 0;
        } else {
            job->arg = 0;
        }
    }
    return 0;
}
```

### Ring Buffer Layout

At `sMain + 0x1fac8`:

- **4096 entries** (12-bit mask)
- Each entry **0x20 bytes**:
  - `+0x00`: argument pointer
  - `+0x08`: function pointer
  - `+0x0c`: type field (0 = one-shot, 2 = repeating)
  - `+0x10-0x1f`: padding

Protected by:
- A Win32 event handle at `sMain + 0xc0` (wakeup signaling)
- A critical section at `sMain + 0x08` (ring buffer access)

---

## Critical Section System

### `cSystem` Interface

The game's base class `cSystem` provides conditional critical sections:

```c
// cSystem + 0x30: byte — enable flag (0 = skip CS, single-threaded fast path)
// Global byte at 0x140e178f0: global CS enable override

void cSystem::enterCriticalSection(void* self) {
    if (*(char*)(self + 0x30) == '\0' && BYTE_140e178f0 == 0)
        return;                          // skip — single-threaded mode
    EnterCriticalSection((CRITICAL_SECTION*)(self + 8));
}

void cSystem::leaveCriticalSection(void* self) {
    if (*(char*)(self + 0x30) == '\0' && BYTE_140e178f0 == 0)
        return;                          // skip — single-threaded mode
    LeaveCriticalSection((CRITICAL_SECTION*)(self + 8));
}
```

| Address | Function |
|---------|----------|
| `0x140006cf0` | `EnterCriticalSection` (kernel32 wrapper) |
| `0x140006d00` | `cSystem::enterCriticalSection` |
| `0x14000b850` | `LeaveCriticalSection` (kernel32 wrapper) |
| `0x14000b860` | `cSystem::leaveCriticalSection` |
| `0x140054fd0` | `DeleteCriticalSection` (kernel32 wrapper) |

This allows subsystems to dynamically switch between single-threaded and
multi-threaded access patterns — when the byte at `+0x30` is zero and the
global disable flag is clear, the critical section calls are skipped entirely
(no atomic overhead).

### `MtCriticalSection`

A memory-allocator-level locking mechanism:

```c
void MtCriticalSection__enter(void* param_1, uint param_2) {
    Allocator* alloc = MtMemoryAllocator::getAllocator(
        &nDraw__Texture__AllocDTI);
    alloc->vtable[0x38](alloc, param_1, param_2, DWORD_140e1bf7c);
}
```

| Address | Function |
|---------|----------|
| `0x140781c40` | `MtCriticalSection::enter` |
| `0x140792a40` | `MtCriticalSection::getOwner` |

The texture allocator's vtable slot at offset `0x38` is a lock method. Each
`MtMemoryAllocator` instance has its own critical section, allowing
allocator-specific locking strategies (different allocators for textures,
audio, physics, etc. can be locked independently).

---

## MtThread Class

The `MtThread` class (vtable at `0x1405bd880`) is a thread wrapper with 19+
virtual functions. It likely wraps a ConcRT context with game-specific
metadata (name, priority, owning allocator).

| Address | Function |
|---------|----------|
| `0x1405bd880` | `MtThread::vfn12` |
| `0x1405bd950` | `MtThread::vfn6` |
| `0x1405bddb0` | `MtThread::dtor` |
| `0x1405c1e20` | `MtThread::vfn11` |
| `0x1405c1e60` | `MtThread::vfn5` |
| `0x1405c7540` | `MtThread::getDTI` |
| `0x1405c9b70` | `MtThread::vfn16` |
| `0x1405c9e90` | `MtThread::vfn13` |
| `0x1405cb620` | `MtThread::vfn17` |
| `0x1405cbb40` | `MtThread::vfn14` |
| `0x1405cc340` | `MtThread::vfn19` |

DTI: `MtThread__getDTI` returns `&QWORD_140e22c60`.

---

## CPU Cap Detection (`sMain::checkCPUCaps` @ `0x14051ffc0`)

At startup, the game runs a comprehensive CPUID-based capability check:

```
1. Read processor name from registry
   HKLM\HARDWARE\DESCRIPTION\System\CentralProcessor\0
   → ProcessorNameString (stripped of spaces → sMain + 0x4008d)

2. GetSystemInfo → logical processor count → sMain + 0x40088 (max thread cap)

3. CPUID leaf 0 → vendor string (GenuineIntel / AuthenticAMD)

4. CPUID leaf 1 → feature bits → sMain + 0x40080 bitmask:
   bit 0: SSE3
   bit 2: SSSE3
   bit 3: SSE4.1
   bit 4: SSE4.2

5. CPUID leaf 4 (Intel) → cores from cache topology → sMain + 0x40084
   CPUID leaf 0x80000008 (AMD) → cores from ECX[7:0] → sMain + 0x40084

6. Hyper-threading detection:
   CPUID.01h.EBX[23:16] (Intel) = logical cores per physical
   If HT is enabled AND the per-package core count = logical count,
   effectiveCores = physicalCores * cacheCoefficient
```

Output stored in `sMain`:

| Offset | Field | Description |
|--------|-------|-------------|
| `+0x40080` | capsBitmask | Feature flags (SSE3, SSSE3, SSE4.1, SSE4.2) |
| `+0x40084` | effectiveCores | Computed physical core count |
| `+0x40088` | logicalCores | GetSystemInfo dwNumberOfProcessors |
| `+0x4008c` | htFlag | Hyper-threading enabled (byte) |
| `+0x4008d` | cpuNameStr | Processor name string |
| `+0x40090+` | (string buf) | Raw processor name buffer |

---

## Thread Import Table

From the PE import table (kernel32), the game links:

### Thread Management
- `CreateThread` / `GetExitCodeThread` / `ResumeThread` — raw OS threads
- `SetThreadIdealProcessor` / `SetThreadPriority` — affinity & priority
- `GetCurrentThreadId` — identification
- `Sleep` / `SleepEx` — yielding

### Synchronization
- `InitializeCriticalSection` / `InitializeCriticalSectionAndSpinCount`
- `EnterCriticalSection` / `LeaveCriticalSection` / `TryEnterCriticalSection`
- `DeleteCriticalSection`
- `CreateEventA` / `SetEvent` / `ResetEvent`
- `CreateSemaphoreA` / `ReleaseSemaphore`
- `CreateMutexA` / `ReleaseMutex`
- `WaitForSingleObject` / `WaitForMultipleObjects`

### Thread-Local Storage
- `TlsAlloc` / `TlsSetValue` / `TlsGetValue` / `TlsFree`

**Note:** `CreateThread` is imported but **only used by ConcRT internally**.
The game never calls it directly. `SetThreadIdealProcessor` is likewise a
ConcRT internal for pinning workers to specific cores.

---

## Thread Inventory

### Game Threads

| Thread | Origin | Entry Point | Max Count | Purpose |
|--------|--------|-------------|-----------|---------|
| **Main thread** | OS loader | `WinMain` / `sApp::execute` | 1 | Window proc, message pump, frame timing, game simulation, rendering |
| **Job workers** | ConcRT | `sMain::jobloop` | 15 | Parallel work (physics, particles, AI) dispatched by `sMain::executeJob` |
| **Delay workers** | ConcRT | `sMain::delayJobloop` | 8 | Time-deferred work (asset loading, texture streaming) |

**Max native threads:** 1 + 15 + 8 = **up to 24 threads**.

### Hook Threads

The hook (`UMVC3Hook`) adds three threads via `CreateThread` in `DllMain`:

| Thread | Entry | Purpose | Sync |
|--------|-------|---------|------|
| **MainThread** | `MainThread` @ `dllmain.cpp:334` | Wait for kiero/D3D9 init, bind EndScene hook | Event-driven (kiero init) |
| **HookUpdate** | `HookUpdate` @ `dllmain.cpp:351` | Infinite loop: `HookProcessStuff()` + `Sleep(1)` | Uncoordinated (polling) |
| **Netplay Service** | `Netplay_ServiceThread` @ `netplay.cpp:58` | Owns all ENet I/O | 3× `CRITICAL_SECTION` |

**Hook thread usage:**

```
DLL_PROCESS_ATTACH:
  CreateThread → MainThread     ← kiero init + EndScene hook
  CreateThread → HookUpdate     ← ImGui menu + framestepper processing

Netplay_Init:
  CreateThread → ServiceThread  ← ENet send/recv (netplay.cpp:217)
```

### Thread Safety Analysis

**Game side:** The job system's data is partitioned — each job operates on its
own data. The delay ring buffer is protected by a critical section. The
conditional CS pattern in `cSystem` means subsystems can run unlocked when no
worker threads are active (which is the common case during gameplay — workers
are idle between `executeJob` calls).

**Hook side:** The `HookedMove` vtable hook replaces `sMvc3Main::move` on the
**main thread**. The hook update thread runs `HookProcessStuff()` which polls
frame state — not thread-safe in theory, but in practice the main thread is
the only one modifying game state. The ImGui rendering runs on the D3D9
`EndScene` hook, which fires during the game's present call (main thread).

The netplay service thread is the only truly concurrent hook thread. It
communicates with the game thread via three Win32 critical sections:

- `g_outboundCS` — guards the outbound packet buffer (game thread writes,
  service thread reads)
- `g_inboundCS` — guards the inbound packet buffer (service thread writes,
  game thread reads)
- `g_delayCS` — guards delay control values (bidirectional)

The game thread's frame loop and the netplay service thread also interact
through the `std::atomic<bool> g_running` flag for shutdown coordination.

```
                        Game Main Thread
                              │
                    HookedMove (vtable hook)
                     ┌────────┴────────┐
                     │                 │
              Controller_         oMove(sMvc3Main)
             OnFrameStart()          │
                     │          Game Simulation
              [spin-wait           │
               for remote     Controller_
               input]         OnFrameEnd()
                     │                 │
                     └────────┬────────┘
                              │
                    ring buffers (frame N)
                    ┌─────────┴─────────┐
                    │                   │
              Service Thread       Service Thread
              (reads inbound)     (writes outbound)
                    │                   │
               ENet recv             ENet send
```

---

## Frame Pipeline

The full per-frame call chain:

```
sMvc3Main::move (main thread, called by sApp::execute message loop)
  ├─ sMain::move               ← Frame timing (see main.md)
  │   ├─ Sleep() / busy-wait   ← Pacer: yield until next frame
  │   └─ sMain::executeJob     ← Optional: dispatch parallel work
  │       └─ worker threads     ← Job system (if multi-threaded)
  │           └─ sMain::jobloop
  ├─ sPad polling              ← Device input read
  ├─ sMvc3Manager::move        ← Game simulation
  │   └─ cChrInput::processFrameInput  ← Per-character input
  │       └─ GetsCurrentInput_withSOCD ← Pad/keyboard read
  ├─ sRender / sPrimitive       ← Rendering
  └─ Present                   ← D3D9 swap chain
      └─ hkEndScene             ← Hook: ImGui overlay
```

The `sMain::move` call (frame timing) runs `executeJob` which can dispatch
work to the job thread pool. After the workers finish, the main thread
continues with simulation and rendering. The delay job threads run
independently on their own schedule.

### How This Affects the Hook

Since `HookedMove` intercepts `sMvc3Main::move` (the game simulation call,
not `sMain::move` the timing call), the hook's `Controller_OnFrameStart`/`End`
run **after** frame timing and **inside** the simulation boundary:

```
sMain::move (timing + job dispatch)
  │
  ┌─ job threads may run here ─┐
  │                            │
  └────────────────────────────┘
  │
sMvc3Main::move (simulation):
  ├─ HookedMove (our hook)
  │   ├─ Controller_OnFrameStart   ← drain inbound, spin-wait
  │   ├─ oMove (original)          ← game simulation
  │   └─ Controller_OnFrameEnd     ← send outbound
```

This means:
1. The frame timing (sleep/busy-wait) is **not** intercepted — network input
   is NOT drained during the sleep period (see main.md for the problem).
2. Job threads run between `sMain::move` and `sMvc3Main::move` — the hook
   doesn't interact with them.
3. The delay job threads run asynchronously — they don't affect the hook's
   frame-level netcode.

---

## Synchronization Summary

### Per-Frame (Main Thread + Service Thread)

```
Frame N:
  Game Thread                         Service Thread
  ───────────                         ─────────────
  Controller_OnFrameStart()           [idle or processing]
    enter g_inboundCS
    drain all packets from buffer
    leave g_inboundCS
    [spin-wait if needed]
                                       ENet host service:
                                       ENET_EVENT_TYPE_RECEIVE:
                                         enter g_inboundCS
                                         write to inbound buffer
                                         leave g_inboundCS
                                       ENET_EVENT_TYPE_DISCONNECT:
                                         set g_running = false

  oMove(sMvc3Main)

  Controller_OnFrameEnd()
    enter g_outboundCS
    write outbound packet
    leave g_outboundCS
                                       enter g_outboundCS
                                       read and send packet
                                       leave g_outboundCS
```

### Lock Hierarchy

```
Game thread:
  1. g_inboundCS   (drain)
  2. g_delayCS     (read remote delay)
  3. g_outboundCS  (queue outbound)

Service thread:
  1. g_outboundCS  (send)
  2. g_inboundCS   (receive)
  3. g_delayCS     (receive delay)

No nested lock acquisition in either path → no deadlock risk.
```
