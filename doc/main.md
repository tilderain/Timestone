# Frame Timing Architecture (`sMain`)

## Overview

The game's frame timing is managed by the `sMain` singleton. It controls when
the next frame runs, how long to wait, and manages job threads for parallel
work. This document covers the three core functions:

- **`sMain__move`** — the main frame timing / wait loop
- **`sMain__jobloop`** — the parallel job dispatch system
- **`sMain__delayJobloop`** — a separate thread pool for deferred/"delay" jobs

---

## sMain Object Layout

From `UndefinedFunction_1405205bb` (the `createProperty` registration):

| Offset | Type | Name | Description |
|--------|------|------|-------------|
| +0x38 | uint64 | `mTimer` | Frame timer value (milliseconds since start) |
| +0x48 | int32 | `mFrameTimer` | Frame counter (increments each frame) |
| +0x50 | float | `mFps` | Target FPS (e.g. 60.0) |
| +0x54 | float | `mMaxFps` | Maximum FPS cap |
| +0x58 | byte | `mFrameWait` | **Frame wait mode**: 0 = sleep-based, 1 = vsync-based |
| +0x5c | float | `mActualFps` | Measured/actual FPS (updated by helper at +0x5215e0) |
| +0x60 | int64 | *(accumulator)* | Accumulated time in "tick" units (FPS-scaled QPC time) |
| +0x68 | int64 | *(accumulator base)* | Reference point for mDeltaTime calculation |
| +0x70 | int64 | *(last sample)* | Previous QPC value (for computing deltas) |
| +0x78 | int32 | *(pause counter)* | Remaining pause frames |
| +0x7c | byte | `mPause` | Pause flag (non-zero = game is paused) |
| +0x7d | byte | *(pause edge)* | Set to 1 when pause just ended this frame |
| +0x80 | float | `mDeltaTime` | **Frame delta time** (0.0 to 1.0, represents fraction of a frame) |
| +0x84 | float | `mDeltaTimeBorder` | Threshold: if mDeltaTime exceeds this, it's clamped to 1.0 and overflow → mDelayFrame |
| +0x88 | float | `mDeltaTimeLimite` | Absolute cap on mDeltaTime |
| +0x8c | float | `mGlobalSpeed` | Global time scale multiplier |
| +0x90 | float | `mDelayFrame` | Overflow accumulator (when mDeltaTime exceeds mDeltaTimeBorder, excess goes here) |
| +0x98 | int32 | *(job count)* | Number of jobs queued for execution |
| +0x9c | int32 | *(job index)* | Current read index into job queue |
| +0xa0 | int32 | *(job mode)* | Job dispatch mode (controls switch in jobloop) |
| +0xa4 | int32 | `mJobThreadNum` | Active job thread count (copied from +0xa8 each frame) |
| +0xa8 | int32 | *(desired threads)* | Desired job thread count (set by `setJobThreadNum`) |
| +0xac | int32 | `mDelayJobThreadNum` | Number of delay job threads |
| +0xb0 | uint64 | `mDelayJobWritePt` | Delay job ring buffer write pointer |
| +0xb8 | uint64 | `mDelayJobReadPt` | Delay job ring buffer read pointer |
| +0xc0 | handle | *(event)* | Win32 event handle for delay job thread synchronization |
| +0x1fac8 | 0x20×4096 | *(delay job buffer)* | Ring buffer for delay jobs (4096 entries × 0x20 bytes each) |
| +0x3fac8 | — | *(job queue)* | Job queue structure (referenced via global `0x140e177e8`) |
| +0x40048 | int64 | *(QPC now)* | Current `QueryPerformanceCounter` value |
| +0x40050 | int64 | *(QPC prev)* | Previous frame's QPC value |
| +0x40058 | int32 | *(QPC delta)* | Delta between current and previous QPC (low 32 bits) |
| +0x40070 | double | *(QPC inv freq)* | `1.0 / QPC_frequency` (for converting ticks → seconds) |
| +0x40078 | int64 | *(QPC freq)* | `QueryPerformanceFrequency` result (ticks per second) |

---

## sMain__move (`0x140521df0`)

This is the heart of frame timing. Called once per frame from `sMvc3Main__move`.

### Pseudocode

```c
void sMain__move(sMain* self) {
    // --- Phase 1: Pause handling ---
    self->mJobThreadNum = desired_job_threads;  // from +0xa8
    if (!self->mPause || self->pause_counter == 0)
        self->pause_counter = 0;
    else
        self->pause_counter--;
    self->pause_edge = (self->pause_counter == 0 && self->mPause);

    // --- Phase 2: Sample time ---
    sampleTime(self);  // FUN_1405215e0 — updates QPC, FPS measurement

    int64 qpcFreq = QueryPerformanceFrequency();
    self->qpc_freq = qpcFreq;
    self->qpc_invFreq = 1.0 / (double)qpcFreq;

    int64 qpcNow = QueryPerformanceCounter();
    int64 qpcDelta = qpcNow - self->qpc_prev;
    self->qpc_now = qpcNow;
    self->qpc_prev = qpcNow;
    self->qpc_delta = (int32)qpcDelta;

    double timeSec = (double)qpcNow * self->qpc_invFreq;
    double dtSec = timeSec - self->prevTimeSec;
    self->timeSec = timeSec;
    self->prevTimeSec = timeSec;
    self->dtSec = (float)dtSec;

    // --- Phase 3: Frame timer math ---
    if (self->last_sample == 0)
        self->last_sample = qpcNow;
    if (qpcNow < self->last_sample)  // handle QPC wraparound
        self->last_sample = qpcNow;

    // mTimer = elapsed ms since mTimerBase (+0x40)
    uint64 elapsed = ((qpcNow - self->mTimerBase) * 1000) / qpcFreq;
    self->mTimer = elapsed & 0xFFFFFFF;  // clamped to 28-bit

    // mFrameTimer = fps * mTimer / 1000 (fixed-point frame count)
    float fps = self->mFps;
    self->mFrameTimer = (int32)((fps * self->mTimer) / 1000);

    // Accumulator advance (tick units)
    int64 dtTicks = qpcNow - self->last_sample;
    self->last_sample = qpcNow;
    self->accumulator += (int64)((float)dtTicks * fps);

    // --- Phase 4: ADVANCE / WAIT ---
    if (self->mFrameWait == 0) {
        // ===== SLEEP-BASED MODE (default, "uncapped" / adaptive) =====
        float ratio = fps / self->mMaxFps;
        double invFreq = 1.0 / (double)qpcFreq;
        float delta = (float)((double)(self->accumulator - self->accumBase) * invFreq);
        self->mDeltaTime = delta;

        // Sleep if ahead of schedule
        float sleepTime = (ratio - delta) * (1000.0 / fps);
        if (sleepTime >= 2.0)
            Sleep((int)sleepTime - 1);

        // Busy-wait until it's time for the next frame
        if (self->mDeltaTime <= ratio && ratio != self->mDeltaTime) {
            do {
                qpcNow = QueryPerformanceCounter();
                self->qpc_now = qpcNow;
                self->qpc_prev = qpcNow;
                // ... update timing ...
                self->accumulator += (int64)((float)(qpcNow - self->last_sample) * fps);
                self->last_sample = qpcNow;
                delta = (float)((double)(self->accumulator - self->accumBase) * invFreq);
                self->mDeltaTime = delta;
            } while (delta < ratio);
        }

        // Clamp negative delta to 0
        if (self->mDeltaTime <= 0.0 && self->mDeltaTime != 0.0)
            self->mDeltaTime = 0.0f;

        // Overflow handling: if delta exceeds border, clamp to 1.0, excess → mDelayFrame
        if (self->mDeltaTimeBorder <= self->mDeltaTime && self->mDeltaTime != self->mDeltaTimeBorder) {
            self->mDeltaTime = 1.0f;
            self->accumBase = self->accumulator;
            self->mDelayFrame += (delta - 1.0f);
        }

        // Absolute cap
        if (self->mDeltaTimeLimite < self->mDeltaTime)
            self->mDeltaTime = self->mDeltaTimeLimite;

        self->accumBase += (int64)(self->mDeltaTime * (double)qpcFreq);

    } else {
        // ===== V-SYNC-BASED MODE (mFrameWait != 0) =====
        while (self->accumulator < self->accumBase) {
            Sleep(1);
            qpcNow = QueryPerformanceCounter();
            self->qpc_now = qpcNow;
            // ... update timing ...
            self->accumulator += (int64)((float)(qpcNow - self->last_sample) * fps);
            self->last_sample = qpcNow;
        }
        self->accumBase += qpcFreq;            // advance by exactly 1 QPC second
        self->mDeltaTime = 1.0f;               // exactly one frame
        if (self->accumBase < self->accumulator)
            self->accumulator = self->accumBase;  // don't let accumulator get ahead
    }

    // --- Phase 5: Apply global speed ---
    self->mDeltaTime = self->mGlobalSpeed * self->mDeltaTime;
}
```

### Key Insight: Two Frame Wait Modes

**Mode 0 (sleep-based, `mFrameWait == 0`):**
The game computes how far ahead of schedule it is and uses `Sleep()` to yield
the CPU. This is the normal mode. But critically: **the network sync runs on
the same thread**. The `Sleep()` + busy-wait loop means the entire netcode
pipeline (reading Steam P2P sockets, processing voice chat, dispatching input)
pauses when the thread sleeps.

**Mode 1 (vsync-based, `mFrameWait != 0`):**
The game busy-waits until the accumulator catches up, using `Sleep(1)` in the
loop. This is the "frame wait" / vsync mode. Same problem: the main thread is
blocked, so network processing is blocked too.

### Frame Time Accounting

The `mDeltaTime` field (at +0x80) is the **fraction of a frame** that has elapsed.
Normally this is 1.0 (one full frame), but under load it can go higher. The
`mDeltaTimeBorder` (at +0x84) controls when overflow spills into `mDelayFrame`
(at +0x90), which is an accumulator for slow frames — basically the game's
measure of how far behind schedule it is.

---

## sMain__jobloop (`0x140521780`)

Job thread main function. Each worker thread runs this loop.

### Pseudocode

```c
void sMain__jobloop(void* param_1, JobThreadContext* ctx) {
    SetEvent(ctx->readyEvent);           // signal that thread is initialized
    while (ctx->exitFlag == 0) {
        // Wait for work (semaphore)
        WaitForSingleObject(ctx->semaphore, INFINITE);
        if (ctx->exitFlag) break;

        ReleaseMutex(ctx->mutex);

        // Read job from shared global queue at 0x140e177e8
        int mode = *(int32*)(0x140e177e8 + 0xa0);
        switch (mode) {
        case 0:  // 1 thread, 0x10-byte jobs
        case 1:  // 1 thread, 0x10-byte jobs
        case 2:  // multi-thread, 0x20-byte jobs
        case 3:
        case 4:  // multi-thread, 0x20-byte jobs
        case 5:  // multi-thread, 0x20-byte jobs with 3rd arg
            // Atomically claim next job index:
            LOCK();
            idx = jobIndex;
            jobIndex++;
            UNLOCK();
            if (idx < jobCount) {
                void* func = jobs[idx].func;
                void* arg  = jobs[idx].arg;
                func(arg);
            }
            break;
        }

        SetEvent(ctx->readyEvent);
    }
    return 0;
}
```

### Architecture

- **Global job queue** at `0x140e177e8` (not part of `sMain` — it's a separate global)
- The queue contains a count (`+0x98`), an index (`+0x9c`), and a mode (`+0xa0`)
- Jobs are stored as an array starting at `+0xc8` (200 bytes into the struct)
- **Mutex + event-based** synchronization: each thread holds a mutex while
  processing the job queue, and signals a ready event when done
- The number of job threads is set via `sMain__setJobThreadNum`/`sMain__createDelayJobThread`

### Dispatch

`FUN_140521a40` (`sMain::executeJob` equivalent) sets up the job queue and
dispatches work. It handles two paths:
1. **Single-threaded** (`mJobThreadNum == 1`): runs the job queue inline
2. **Multi-threaded**: signals worker threads, then waits for all to complete

---

## sMain__delayJobloop (`0x1405210e0`)

A **separate thread pool** for "delay jobs" — deferred work that runs on its
own threads, independent of the main frame timing.

### Pseudocode

```c
int sMain__delayJobloop(sMain* self, DelayThreadContext* ctx) {
    if (ctx->exitFlag) return 0;

    while (ctx->exitFlag == 0) {
        // Wait for signal (semaphore/event at self->eventHandle)
        while (WaitForSingleObject(self->eventHandle, INFINITE) != 0);

        // Critical section enter
        if (self->exitFlag || some_global_flag)
            EnterCriticalSection(&self->cs);

        uint64 readPt = self->mDelayJobReadPt;    // +0xb8
        uint64 writePt = self->mDelayJobWritePt;  // +0xb0

        if (readPt == writePt) {
            // Ring buffer is empty — no work to do
            if (self->exitFlag || some_global_flag)
                LeaveCriticalSection(&self->cs);
            Sleep(1);  // yield
            continue;
        }

        // Claim a job from the ring buffer
        if (readPt < writePt) {
            self->mDelayJobReadPt = readPt + 1;
            job = &self->delayJobRing[(uint32)readPt & 0xFFF];  // 4096-entry ring
        }

        if (self->exitFlag || some_global_flag)
            LeaveCriticalSection(&self->cs);

        if (job == NULL) {
            Sleep(1);
            continue;
        }

        // Execute the job based on type:
        if (job->type == 0) {
            // One-shot job
            func = job->func;
            job->func = NULL;
            if (func) func(job->arg);
            job->arg = 0;
        } else if (job->type == 2) {
            // Persistent/repeating job
            func = job->func;
            job->func = NULL;
            if (func) func(job->arg);
            job->arg = 0;
        } else {
            // Other types: just clear
            job->arg = 0;
        }
    }
    return 0;
}
```

### Ring Buffer Layout

The delay job ring buffer at `self + 0x1fac8`:
- **4096 entries** (0xFFF mask = 4095)
- Each entry is **0x20 bytes**:
  - `+0x00`: argument pointer
  - `+0x08`: function pointer
  - `+0x0c`: type field (0 = one-shot, 2 = repeating)
  - `+0x10-0x1f`: reserved/padding

### Thread Lifecycle

Created by `sMain__createDelayJobThread` (`0x1405202c0`):
- Up to **8 delay job threads**
- Each thread gets its own context (0x38 bytes) within `sMain` at offset
  `0x3fe48 + idx * 0x38`
- Uses a shared event handle (`self + 0xc0`) for wakeup signaling
- A critical section at `self + 8` protects the ring buffer

---

## Interaction with Netcode

### Frame Pipeline

The full call chain each frame is:

```
sMvc3Main__move
  ├─ sMain__move            ← Frame timing: sleep/busy-wait until next frame
  │   └─ Sleep() / busy-wait loop
  ├─ FUN_140289c30           ← sMvc3SyncPad processing (network input)
  │   ├─ network_sync_buffer_read
  │   ├─ sMvc3Manager::processNetworkSync
  │   └─ Input dispatch to game
  ├─ sRender / sPrimitive    ← Rendering
  └─ sMvc3Manager::getAction ← Game simulation
```

### The Problem

Everything runs on the **main thread** — frame timing, network sync, and game
simulation are all sequential. When `sMain__move` decides to `Sleep()`, the
entire pipeline stops:

1. Network packets arrive in the Steam P2P socket buffer
2. Voice chat data is received but **not processed** until the main thread wakes
3. Input packets accumulate latency equal to the sleep duration
4. The dynamic delay adjustment (`FUN_14028c090`) reads stale metrics because
   the ring buffers weren't updated while sleeping

### Delay Jobs vs. Netcode

The delay job thread pool (`sMain__delayJobloop`) is used for **graphics/asset
loading** work, not netcode. Jobs like texture streaming and asset decompression
run here to avoid stalling the render thread. The netcode does **not** use this
pool — it runs entirely inline on the main thread.

### Summary

The game's frame timing in `sMain__move` treats network sync as just another
inline operation. There's no dedicated network thread, no async I/O for Steam
P2P packets (beyond what Steam's internal threading provides), and the
Sleep-based frame pacing introduces **variable latency** directly into the
input processing pipeline. This is a fundamental architectural limitation
of the delay-based netcode — the frame timing has no awareness of network
timing requirements.
