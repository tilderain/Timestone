# Native Mouse Disable

`mouse_disable.cpp` replaces `sMouse::move` at `0x1406AA990` through a
gameplay-owned detour, independent of backend timing. It does not call the
original, so native frame-time cursor visibility/clipping APIs and DirectInput
Poll/Acquire/GetDeviceState are no longer reached from mouse movement.

The typed Ghidra decompilation of `sMouse::move` establishes these fields:

- `+0x38/+0x3C`: coordinates, set to -1/-1 rather than freezing the last position.
- `+0x44..+0x4F`: relative axes and wheel, cleared.
- `+0x50..+0x67`: held/previous/pressed/released/changed/repeat masks, cleared.
- `+0x68..+0xA7`: eight repeat timers, cleared.

The vtable/base, visibility/control bytes at +0x40..+0x43, configuration at
+0xA8 and above, and COM pointers at +0xC0/+0xC8 are untouched. Construction
and destruction remain native; this disables periodic processing, not device
creation. Existing game mouse interactions are intentionally unavailable.

The former conditional Poll bypass and its delayed acquisition handoff are
removed. ImGui's separate Win32 handling is unchanged. Normal Launch disables
the detour and resumes native movement. No global cursor visibility or clipping
API is called by the replacement.

Expected captures no longer contain native `mouse_move`, `mouse_state`, or
mouse DirectInput API timings from this movement path. Overlay mouse metrics
remain. Test keyboard/controller menu navigation, overlay mouse interaction,
and Normal Launch. This does not eliminate independent scheduling, network,
or rollback stalls.
