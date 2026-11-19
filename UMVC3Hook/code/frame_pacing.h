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

// Waits until the performance counter reaches `targetQpc`, to the tick: a
// timer sleep to a millisecond short of it, then a busy spin.
void FramePacing_WaitUntil(int64_t targetQpc);

// The game thread, once the frame's pacing wait is over and its work begins.
void FramePacing_OnFrameStart();

// Around every Present on any device, from the thread that presents:
// `token = FramePacing_BeforePresent(device); Present(...); FramePacing_OnPresent(device, token);`
//
// BeforePresent is also where even pacing happens. With VSync off a frame is
// shown the moment its work is done, so frames that take a different time
// land unevenly. So with VSync off the game's frame waits here until a
// fixed time after its start (the longest recent work plus a little): frames
// then land one frame apart, each shown that much later than it could be.
int64_t FramePacing_BeforePresent(IDirect3DDevice9* device);
void FramePacing_OnPresent(IDirect3DDevice9* device, int64_t beforeToken);

struct FramePacingStats {
    bool valid;            // enough frames seen
    float averageMs;       // mean frame time over the last two seconds
    float worstMs;         // largest distance from 16.67 ms in that time
    uint32_t late;         // frames over 17.67 ms in that time
    // Where unevenness comes from, over the same two seconds:
    float startWorstMs;    // largest distance from 16.67 ms between frame starts
    float workMinMs;       // frame start to the Present call: the frame's work
    float workMaxMs;
    float presentMaxMs;    // longest the Present call itself took
    float holdMs;          // even pacing: frame start to Present, 0 when not holding
    bool displayValid;     // the swapchain reports present statistics
    float refreshHz;       // measured from those statistics
    uint32_t repeated;     // extra refreshes a frame was held for, last 10 s
    uint32_t dropped;      // frames that were never shown, last 10 s
};
FramePacingStats FramePacing_Get();
