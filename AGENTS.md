# Project Overview: UMVC3Rollback

**Purpose:** A Windows DLL plugin (`.asi`) for Ultimate Marvel vs. Capcom 3 that adds rollback netcode, replay system, training mode enhancements, and a debug menu.

**Build:** `make -j8` using MinGW-w64 cross-compiler on Linux. The ASI is already symlinked and deployed, please don't check the location or attempt to install/copy it to the game folder.

**Ghidra:** The Ghidra MCP is available, and should be used after consulting the docs on reverse engineering notes. If you are decompiling functions, please try to apply structs to the unknown and unnamed structs inside a function (if you have determined the struct such as cChrInput, sAction to be a suitable match).

**Subagents** Please never under any circumstance use subagents to explore/search the codebase. You may use subagents to parallel Ghidra rename, or parallel Ghidra research / analyzing (for renaming).

## Key Dependencies (all vendored)
- **ENet 1.3.18** - reliable UDP networking
- **Dear ImGui** - GUI overlay
- **Kiero + MinHook** - DirectX hooking and function detouring
- **Ultimate ASI Loader** (`dinput8.dll`) - plugin injection into game process

## Runtime Config
- `Base/Timestone.ini` - hotkeys (F1 menu), camera binds, settings
- Plugin loads via `dinput8.dll` proxy → `UMVC3Hook.asi`
