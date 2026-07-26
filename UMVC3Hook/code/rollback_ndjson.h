#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

struct NDJSON_Region {
    const void* data;
    size_t size;
};

struct NDJSON_ShotRecipe {
    uint32_t slotId;
    uint64_t initParam;
    uint64_t ownerChr;
    uint64_t shotElement;
    bool extended;
};

void NDJSON_GetRegions(std::vector<NDJSON_Region>& regions);
bool NDJSON_GetShotRecipe(void* shot, NDJSON_ShotRecipe& recipe);

std::string NDJSON_GenerateNetplayLive(int32_t frame, bool isResim,
                                       const char* phase = nullptr,
                                       int32_t rollbackStart = -1,
                                       int32_t rollbackEnd = -1);
void NDJSON_WriteNetplayLive(const std::string& json);
void NDJSON_DumpNetplayLive(int32_t frame);
void NDJSON_CaptureFrame(int32_t frame, bool isResim, bool postOnly = false);
void NDJSON_DumpFrame(int32_t frame, uint8_t side);
void NDJSON_DiscardPendingNetplayLive(int32_t frame);
void NDJSON_SendResimNotify(int32_t startFrame, int32_t endFrame);
int NDJSON_BeginSession(bool useCurrentSession = false);
void NDJSON_QueuePendingResimDump(int32_t frame);
void NDJSON_LogResimNotify(const struct ResimNotifyPacket& packet);
void NDJSON_SendGroundTruth(int32_t startFrame, int32_t endFrame);
void NDJSON_ProcessInboundGroundTruth();
void NDJSON_NotifyDesyncTriggered(int32_t frame);
void NDJSON_FlushNetplayLive(int32_t frame);
void NDJSON_DumpNetplayHistory(int32_t frame);
bool NDJSON_FlushOnCrash();
void NDJSON_ResetNetplayLiveLog();
bool NDJSON_GetNetplayLiveDirectory(char* path, size_t pathSize);
void NDJSON_CommitPendingNetplayLive(int32_t frame);
