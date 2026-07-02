# Project Overview: UMVC3Rollback

**Purpose:** A Windows DLL plugin (`.asi`) for Ultimate Marvel vs. Capcom 3 that adds rollback netcode, replay system, training mode enhancements, and a debug menu.

**Build:** `make -j8` using MinGW-w64 cross-compiler on Linux. 

**Ghidra:** The Ghidra MCP is available, and should be used after consulting the docs on reverse engineering notes.

## Directory Structure

| Path | Description |
|---|---|
| `UMVC3Hook/` | Main source: `dllmain.cpp` (entry point), `code/` (core modules) |
| `UMVC3Hook/code/` | Core game hook modules (see detailed descriptions below) |
| `UMVC3Hook/code/rollback.cpp/h` | **Rollback netcode engine**: snapshot/restore ring buffer (16 frames), headless re-simulation pipeline, deferred resim requests (F7 test), remote input correction on late packets |
| `UMVC3Hook/code/netplay.cpp/h` | **ENet networking layer**: input packet protocol (sliding history window of 16 frames), delay/rollback sync, ground truth checksums for desync detection, dedicated background I/O thread, ResimNotify packets |
| `UMVC3Hook/code/controller.cpp/h` | **Input system**: hooks XInput polling, converts gamepad→game bitmask, SOCD cleaning, input history ring buffers for rollback replay, keyboard disable hook |
| `UMVC3Hook/code/umvc3menu.cpp/h` | **ImGui debug menu**: main UI overlay (toggled by F1), settings, training options, netplay controls, camera controls, replay recording/playback UI |
| `UMVC3Hook/code/framestepper.cpp/h` | **Training frame stepper**: pause game, advance frame-by-frame, D3D frame capture, snapshot save/restore (F6), re-simulation test trigger, slowdown |
| `UMVC3Hook/code/LaunchMenu.cpp/h` | **Launch dialog**: Win32 pre-init dialog for selecting Normal/Training/Dual Training mode, character/stage selection, ENet host/connect configuration |
| `UMVC3Hook/code/trainingboot.cpp/h` | **Training mode boot**: bypasses title screen logos, directly configures a training match with chosen characters/stage, hooks game init sequence |
| `UMVC3Hook/code/dtieditor.cpp/h` | **DTI property editor**: reverse-engineering tool that browses MT Framework's runtime type hierarchy (MtDTI) and captures live property values |
| `UMVC3Hook/code/diag.cpp/h` | **Diagnostics**: per-frame character state snapshots (position, animation, RNG, flags), input history logging, NDJSON dump for debugging rollback |
| `UMVC3Hook/code/eSettingsManager.cpp/h` | **Settings manager**: reads `UMVC3Hook.ini` at startup, exposes hotkey bindings (menu toggle, camera controls) and feature toggles |
| `UMVC3Hook/code/eNotifManager.cpp/h` | **OSD notifications**: on-screen text messages with fade timers, push-based API for alerting the user |
| `UMVC3Hook/code/UMVC3Camera.cpp/h` | **Free camera**: overrides game camera position/rotation/FOV, keyboard-controlled movement, screenshot utility |
| `UMVC3Hook/code/mk10utils.cpp/h` | **Address utilities**: base address relocation helpers (`_addr()`), converts hardcoded offsets to runtime addresses |
| `UMVC3Hook/code/umvc3.cpp/h` | **Base hooks**: version string, camera info struct, `HookProcessStuff()` and `HookCamera()` trampolines |
| `UMVC3Hook/code/defaults.h` | **Legacy types & helpers**: `Fighter` struct, `ScriptableFighter`, `SaveLoadState()` for old snapshot approach, replay buffer (pre-rollback), `hook()` template wrappers |
| `UMVC3Hook/code/generated.h` | **Auto-generated code** (7577 lines): decompiled character tick functions from Ghidra, used for headless re-simulation |
| `UMVC3Hook/code/propertytable.h` | **Property table** (1318 lines): auto-generated mapping of function addresses → DTI class names for the DTI editor |
| `UMVC3Hook/imgui/` | Dear ImGui library (vendored, DX9/DX11/Win32 backends) |
| `UMVC3Hook/kiero/` | Kiero graphics hook + MinHook (vendored, x86/x64 hooking) |
| `UMVC3Hook/utils/` | `Patterns.cpp/h` - memory pattern matching; `Trampoline.h`, `HookInit.hpp` |
| `enet-1.3.18/` | ENet networking library (vendored, C source) |
| `Base/` | Deployment files: `dinput8.dll` (ASI Loader), `UMVC3Hook.ini` (runtime config), compiled `UMVC3Hook.asi` |
| `build/` | Build artifacts (object files, include fix symlinks) |
| `doc/` | Design docs: rollback, netplay, re-simulation, threading, input, replay |
| `addrs/` | Memory address dumps for reverse engineering |
| `gen_proptable.py` | Python script to auto-generate property tables |

## Key Dependencies (all vendored)
- **ENet 1.3.18** - reliable UDP networking
- **Dear ImGui** - GUI overlay
- **Kiero + MinHook** - DirectX hooking and function detouring
- **Ultimate ASI Loader** (`dinput8.dll`) - plugin injection into game process

## Runtime Config
- `Base/UMVC3Hook.ini` - hotkeys (F1 menu), camera binds, settings
- Plugin loads via `dinput8.dll` proxy → `UMVC3Hook.asi`
