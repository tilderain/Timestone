#pragma once
// Whether frames come out evenly, and whether the display showed each once.
//
// Two measurements, both taken where the game presents a frame:
//  - pacing: the time between one Present and the next, against 60 fps;
//  - display: on the D3D9Ex flip swapchain, the present statistics say which
//    display refresh each frame appeared on, so a frame held for an extra
//    refresh (repeated) or never shown (dropped) can be counted. No other
//    backend reports this.

#include <cstdint>

struct IDirect3DDevice9;

// After every Present on any device, from the thread that presented.
void FramePacing_OnPresent(IDirect3DDevice9* device);

struct FramePacingStats {
    bool valid;            // enough frames seen
    float averageMs;       // mean frame time over the last two seconds
    float worstMs;         // largest distance from 16.67 ms in that time
    uint32_t late;         // frames over 17.67 ms in that time
    bool displayValid;     // the swapchain reports present statistics
    float refreshHz;       // measured from those statistics
    uint32_t repeated;     // extra refreshes a frame was held for, last 10 s
    uint32_t dropped;      // frames that were never shown, last 10 s
};
FramePacingStats FramePacing_Get();
