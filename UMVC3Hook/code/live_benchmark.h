#pragma once

#include <cstdint>

enum class LiveBenchmarkMetric : uint32_t {
    Save, Restore, Headless, Correction, Live, Count
};

// Game-thread timing only. A zero timestamp means this operation is not sampled.
//
// A frame is: BeginFrame at the top of the main hook (decides whether this
// frame is sampled), any correction and snapshot work, then BeginNative /
// EndFrame around the ordinary tick. "Live" is that tick alone. A sampled
// correction is paired with the tick that follows it, so the Rollback figure
// is a measured frame (correction + tick), not an estimate.
void LiveBenchmark_BeginFrame(int32_t frame);
uint64_t LiveBenchmark_BeginNative();
// `frames` replayed up to `endFrame`, the frame the game is on.
uint64_t LiveBenchmark_BeginResim(int32_t frames, int32_t endFrame);
void LiveBenchmark_EndResim(uint64_t start);
uint64_t LiveBenchmark_Now();
void LiveBenchmark_Record(LiveBenchmarkMetric metric, uint64_t start);
void LiveBenchmark_EndPacing(uint64_t start);
void LiveBenchmark_EndFrame(uint64_t start, int32_t frame);
void LiveBenchmark_Update();
void LiveBenchmark_Draw();
