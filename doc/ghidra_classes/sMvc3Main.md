# sMvc3Main

**Ghidra type:** `sMvc3Main`
**Known size:** `0x402E0`
**Base class:** `sMain` (`0x40190` bytes)

## Confirmed Fields

| Offset | Size | Type | Name | Access | Evidence |
|---:|---:|---|---|---|---|
| `0x40288` | `8` | `byte[8]` | `renderState_40288` | read/write | `sMvc3Main__updateGraphicsSettings` tests byte 0 as a pending flag; `sMvc3Main__setGraphicsSettingsPending` writes it. The dword at `+0x4028C` is the pending resolution selection. |
| `0x40294` | `0x20` | `byte[32]` | `runtimeState_40294` | read/write | `sMvc3Main__syncGraphicsSettings` consumes graphics-option values beginning at `+0x40294`; the UI reads and writes the surrounding option slots through `sMvc3Main__getPendingGraphicsOption` and `sMvc3Main__setPendingGraphicsOption`. |
| `0x402A0` | `0x14` | `uint32_t[5]` | runtime graphics options | read | `sMvc3Main__getRuntimeGraphicsOption` reads five dwords here; `uUiAnmSprite__classFunc` saves them while opening the graphics-options UI. |

## Candidate Fields

| Offset | Size | Proposed type/name | Confidence | Evidence needed |
|---:|---:|---|---|---|
| `0x40290` | `4` | `uint32_t` graphics option selection 1 | medium | Identify the corresponding `sApp` option name. |
| `0x40294` | `4` | `uint32_t` graphics option selection 2 | medium | Confirm the option name from the `PTR_aResolution` string table. |
| `0x40298` | `4` | `uint32_t` graphics option selection 3 | medium | Confirm the option name from the `PTR_aResolution` string table. |
| `0x4029C` | `4` | `uint32_t` graphics option selection 4 | medium | Confirm the option name from the `PTR_aResolution` string table. |

## Relevant Methods

| Address | Name | Relationship |
|---|---|---|
| `0x140003170` | `sMvc3Main__getRuntimeGraphicsOption` | Reads one runtime graphics-option snapshot value at `+0x402A0 + index * 4`. |
| `0x140003180` | `sMvc3Main__getPendingGraphicsOption` | Reads one pending graphics-option value beginning at `+0x4028C`. |
| `0x1400037B0` | `sMvc3Main__setPendingGraphicsOption` | Writes one pending graphics-option value beginning at `+0x4028C`. |
| `0x1400037C0` | `sMvc3Main__setGraphicsSettingsPending` | Sets the pending-update flag at `+0x40288`. |
| `0x140258FA0` | `sMvc3Main__resetGraphicsOptionSelection` | Resolves the current display resolution and initializes graphics-option selections. |
| `0x140259BC0` | `sMvc3Main__syncGraphicsSettings` | Applies pending fullscreen, resolution, anti-aliasing, texture, and effect-volume settings. |
| `0x140259EA0` | `sMvc3Main__updateGraphicsSettings` | Runs synchronization while the pending flag is set. |

## Open Questions

- Resolve the exact `sApp` option names for the five pending/runtime slots.
- Confirm whether the `+0x402A0` values are applied settings, a UI snapshot, or
  another runtime copy; current evidence supports runtime graphics-option state.
- `sMain__getInstance` returns the base singleton pointer. In the normal game
  construction path, `sMvc3Main__sMvc3Main` calls `sMain__sMain` on
  `this->base_sMain`, and `sMain__sMain` stores that pointer globally. The
  `sMvc3Main` cast at these call sites therefore reflects the concrete object,
  not a separate standalone `sMain` allocation.
