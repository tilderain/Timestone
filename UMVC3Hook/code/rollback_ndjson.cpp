#include "../pch.h"

#include "rollback_ndjson.h"
#include "autotest.h"
#include "controller.h"
#include "rollback_snapdump.h"
#include "diag.h"
#include "mk10utils.h"
#include "netplay.h"
#include "rollback.h"
#include "rollback_diagnostics.h"
#include "rollback_memory.h"
#include "rollback_shot.h"
#include "rollback_snapshot.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

extern void HookLog(const char* fmt, ...);
extern void HookLogSetFile(const char* filename);

static_assert(sizeof(RollbackRegion) <= 64);

bool g_ndjsonLoggingEnabled = false;
bool g_ndjsonIncludeDirHistory = false;

namespace {

int32_t g_pendingDesyncTriggerFrame = -1;
int32_t g_pendingDesyncCaptureFrame = -1;
bool g_pendingDesyncCaptureWritten = false;

constexpr uintptr_t PTR_SCHARACTER = 0x140d44a70;
constexpr uintptr_t PTR_SMAIN = 0x140e177e8;
constexpr uintptr_t TEAM_OFFSETS[] = {0x40, 0x300};
constexpr uintptr_t TEAM_FIRST_NODE_OFFSET = 0x08;
constexpr uintptr_t NODE_CHR_OFFSET = 0x08;
constexpr uintptr_t NODE_NEXT_OFFSET = 0x10;
constexpr int SHOT_LIST_COUNT = 10;
constexpr size_t SHOT_NODE_OFFSET = 0x1450;
constexpr size_t SHOT_NODE_NEXT = 0x08;
constexpr int DYNAMIC_SAFETY = 128;
constexpr int NETPLAY_LIVE_HISTORY = 512;
constexpr int PENDING_HISTORY = 256;

bool g_liveInit = false;
char g_livePath[260] = {};
int g_liveReset = 0;
std::string g_pendingJson[PENDING_HISTORY];
int32_t g_pendingFrame[PENDING_HISTORY] = {};
std::string g_history[NETPLAY_LIVE_HISTORY];
int32_t g_historyFrame[NETPLAY_LIVE_HISTORY] = {};
bool g_desyncFlushed = false;
int g_sessionId = 0;
int g_diagWriteCount = 0;
char g_diagDir[260] = {};
FILE* g_diagFile = nullptr;
DWORD g_diagFlush = 0;

struct PendingResimDump {
    int32_t frame;
    ResimCharState p1Post;
    ResimCharState p2Post;
};
PendingResimDump g_resimDumps[PENDING_HISTORY] = {};
bool g_resimDumpValid[PENDING_HISTORY] = {};

void FillState(ResimCharState& dst, const CharDiagSnapshot& src)
{
    dst.actionId = src.actionId;
    dst.posX = src.posX;
    dst.posY = src.posY;
    dst.posZ = src.posZ;
    dst.flags = src.flags14F0;
    dst.mHeld = *(const uint32_t*)(src.cchr + 0x14);
    dst.frameCnt = *(const uint64_t*)(src.cchr + 0x18);
    for (int i = 0; i < 4; ++i) dst.rng[i] = src.xorShift[i];
}

void FillState(ResimCharState& dst, const LiveFrameRecord& src)
{
    dst.actionId = src.actionId;
    dst.posX = src.posX;
    dst.posY = src.posY;
    dst.posZ = src.posZ;
    dst.flags = src.flags14F0;
    dst.mHeld = src.mHeld;
    dst.frameCnt = src.frameCnt;
    for (int i = 0; i < 4; ++i) dst.rng[i] = src.rng[i];
}

bool FindPointCharacters(const FrameDiagSnapshot& snapshot,
                         const CharDiagSnapshot*& p1,
                         const CharDiagSnapshot*& p2)
{
    p1 = nullptr;
    p2 = nullptr;
    for (int i = 0; i < snapshot.charCount && i < 6; ++i) {
        const CharDiagSnapshot& character = snapshot.chars[i].post;
        if (character.teamIdx == 0 && character.charSlot == 0) p1 = &character;
        if (character.teamIdx == 1 && character.charSlot == 0) p2 = &character;
    }
    return p1 && p2;
}

const FrameDiagSnapshot* GetResimSnapshot(int32_t frame)
{
    uint32_t index = (uint32_t)(frame >= 0 ? frame : 0) % ROLLBACK_WINDOW_DIAG;
    const FrameDiagSnapshot& snapshot = g_resimFrameSnap[index];
    if (snapshot.frame != frame) {
        HookLog("Rollback: stale resim diag slot for frame %d (slot holds frame %d) - refusing read\n",
                frame, snapshot.frame);
        return nullptr;
    }
    return &snapshot;
}

void InitDiag()
{
    return;
    if (!g_ndjsonLoggingEnabled || g_diagDir[0]) return;

    SYSTEMTIME time;
    GetLocalTime(&time);
    const char* suffix = Netplay_IsActive()
        ? (Netplay_IsHost() ? "_host" : "_client") : "_offline";
    char relative[80];
    snprintf(relative, sizeof(relative), "rdiag_%04d%02d%02d_%02d%02d%02d%s",
             time.wYear, time.wMonth, time.wDay, time.wHour, time.wMinute,
             time.wSecond, suffix);

    Rollback_ResolveDllPath(g_diagDir, sizeof(g_diagDir), relative);
    CreateDirectoryA(g_diagDir, nullptr);
    HookLog("Rollback: diag dir: %s\n", g_diagDir);

    char path[260];
    snprintf(path, sizeof(path), "%s/rdiag.ndjson", g_diagDir);
    g_diagFile = fopen(path, "a");
    if (g_diagFile) g_diagFlush = GetTickCount();
}

void FlushDiagPeriodically()
{
    if (!g_diagFile) return;
    DWORD now = GetTickCount();
    if (now - g_diagFlush >= 1000) {
        fflush(g_diagFile);
        g_diagFlush = now;
    }
}

void WriteCharacterState(FILE* file, int team, const ResimCharState& state)
{
    fprintf(file,
            "%s{\"team\":%d,\"slot\":0,\"pos\":[%.4f,%.4f,%.4f],"
            "\"act\":\"0x%08X\",\"mChrF\":\"0x%08X\","
            "\"mHeld\":\"0x%08X\",\"fcnt\":%llu,"
            "\"rng\":[\"0x%08X\",\"0x%08X\",\"0x%08X\",\"0x%08X\"]}",
            team ? "," : "", team, state.posX, state.posY, state.posZ,
            state.actionId, state.flags, state.mHeld,
            (unsigned long long)state.frameCnt, state.rng[0], state.rng[1],
            state.rng[2], state.rng[3]);
}

void WriteComparisonEntry(FILE* file, const char* timestamp, int32_t frame,
                          const char* side, const ResimCharState& p1,
                          const ResimCharState& p2)
{
    fprintf(file, "{\"v\":1,\"ts\":\"%s\",\"frame\":%d,\"side\":\"%s\",\"chars\":[",
            timestamp, frame, side);
    WriteCharacterState(file, 0, p1);
    WriteCharacterState(file, 1, p2);
    fputs("]}\n", file);
}

std::string GenerateCharacterHitboxes(uint64_t character)
{
    static constexpr uint32_t capsuleBase = 0x4210;
    static constexpr uint32_t capsuleStride = 0xB0;
    static constexpr uint32_t sphereBases[] = {0x4E20, 0x5530, 0x5C40};
    static constexpr uint32_t sphereStride = 0x70;

    std::string out = "[";
    bool first = true;
    char entry[384];
    for (uint32_t index = 0; index < 16; ++index) {
        uint64_t primitive = character + capsuleBase + index * capsuleStride;
        if (*(uint8_t*)(primitive + 0x08) == 0) continue;
        float x0 = *(float*)(primitive + 0x20);
        float y0 = *(float*)(primitive + 0x24);
        float z0 = *(float*)(primitive + 0x28);
        float x1 = *(float*)(primitive + 0x30);
        float y1 = *(float*)(primitive + 0x34);
        float z1 = *(float*)(primitive + 0x38);
        float radius = *(float*)(primitive + 0x40);
        float dx = x1 - x0;
        float dy = y1 - y0;
        float dz = z1 - z0;
        float segmentLength = std::sqrt(dx * dx + dy * dy + dz * dz);
        if (!std::isfinite(x0) || !std::isfinite(y0) || !std::isfinite(z0) ||
            !std::isfinite(x1) || !std::isfinite(y1) || !std::isfinite(z1) ||
            !std::isfinite(radius) || !std::isfinite(segmentLength)) continue;
        snprintf(entry, sizeof(entry),
                 "%s{\"bank\":\"capsule\",\"index\":%u,\"shape\":\"capsule\",\"damaging\":true,"
                 "\"pos\":[%.9g,%.9g,%.9g],\"size\":[%.9g,%.9g,%.9g],"
                 "\"radius\":%.9g,\"length\":%.9g}",
                 first ? "" : ",", index,
                 (x0 + x1) * 0.5f, (y0 + y1) * 0.5f, (z0 + z1) * 0.5f,
                 std::fabs(dx) + radius * 2.0f, std::fabs(dy) + radius * 2.0f,
                 std::fabs(dz) + radius * 2.0f, radius,
                 segmentLength + radius * 2.0f);
        out += entry;
        first = false;
    }
    for (uint32_t bank = 0; bank < 3; ++bank) {
        for (uint32_t index = 0; index < 16; ++index) {
            uint64_t primitive = character + sphereBases[bank] + index * sphereStride;
            if (*(uint8_t*)(primitive + 0x08) == 0) continue;
            float x = *(float*)(primitive + 0x20);
            float y = *(float*)(primitive + 0x24);
            float z = *(float*)(primitive + 0x28);
            float radius = *(float*)(primitive + 0x2C);
            int32_t parent = *(int32_t*)(primitive + 0x40);
            float localX = *(float*)(primitive + 0x50);
            float localY = *(float*)(primitive + 0x54);
            float localZ = *(float*)(primitive + 0x58);
            if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z) ||
                !std::isfinite(radius) || !std::isfinite(localX) ||
                !std::isfinite(localY) || !std::isfinite(localZ)) continue;
            snprintf(entry, sizeof(entry),
                     "%s{\"bank\":\"sphere%u\",\"index\":%u,\"shape\":\"sphere\","
                     "\"damaging\":false,\"pos\":[%.9g,%.9g,%.9g],"
                     "\"size\":[%.9g,%.9g,%.9g],\"radius\":%.9g,"
                     "\"parent\":%d,\"local\":[%.9g,%.9g,%.9g]}",
                     first ? "" : ",", bank, index, x, y, z,
                     radius * 2.0f, radius * 2.0f, radius * 2.0f, radius,
                     parent, localX, localY, localZ);
            out += entry;
            first = false;
        }
    }
    return out + "]";
}

void InitLiveLog()
{
    g_liveInit = false;
    g_livePath[0] = '\0';

    SYSTEMTIME time;
    GetLocalTime(&time);
    const char* suffix = Netplay_IsActive()
        ? (Netplay_IsHost() ? "_host" : "_client") : "_offline";
    char relative[80];
    if (g_liveReset > 0) {
        snprintf(relative, sizeof(relative), "nplive_%04d%02d%02d_%02d%02d%02d%s_r%d",
                 time.wYear, time.wMonth, time.wDay, time.wHour, time.wMinute,
                 time.wSecond, suffix, g_liveReset);
    } else {
        snprintf(relative, sizeof(relative), "nplive_%04d%02d%02d_%02d%02d%02d%s",
                 time.wYear, time.wMonth, time.wDay, time.wHour, time.wMinute,
                 time.wSecond, suffix);
    }

    char directory[260];
    Rollback_ResolveDllPath(directory, sizeof(directory), relative);
    CreateDirectoryA(directory, nullptr);
    snprintf(g_livePath, sizeof(g_livePath), "%s/frames.ndjson", directory);
    if (g_ndjsonLoggingEnabled) {
        if (FILE* file = fopen(g_livePath, "w")) fclose(file);
    }

    char logPath[260];
    snprintf(logPath, sizeof(logPath), "%s/%s", directory,
             Netplay_IsHost() ? "UMVC3Hook_host.log" : "UMVC3Hook_client.log");
    HookLogSetFile(logPath);

    for (int i = 0; i < PENDING_HISTORY; ++i) {
        g_pendingFrame[i] = -1;
        g_pendingJson[i].clear();
    }
    for (int i = 0; i < NETPLAY_LIVE_HISTORY; ++i) {
        g_historyFrame[i] = -1;
        g_history[i].clear();
    }
    g_desyncFlushed = false;
    g_liveInit = true;
    if (g_ndjsonLoggingEnabled)
        HookLog("NetplayLive: NDJSON log -> %s\n", g_livePath);
    else
        HookLog("NetplayLive: profile session -> %s\n", directory);
}

void AppendShotCollision(std::string& json, void* shot)
{
    uint64_t address = (uint64_t)shot;
    uint64_t vtable = address ? *(uint64_t*)address : 0;
    if (!vtable || !IsAccessiblePointer(vtable) || !IsAccessibleAddress(vtable + 0xA0)) return;
    using GetMatrixFn = uint64_t(__fastcall*)(uint64_t, int32_t);
    GetMatrixFn getMatrix = *(GetMatrixFn*)(vtable + 0xA0);
    if (!getMatrix || !IsAccessibleAddress((uint64_t)getMatrix)) return;
    uint64_t matrix = getMatrix(address, -1);
    if (!matrix || !IsAccessiblePointer(matrix)) return;
    float values[16];
    for (int i = 0; i < 16; ++i) {
        values[i] = *(float*)(matrix + 0x20 + i * sizeof(float));
        if (!std::isfinite(values[i])) return;
    }
    char text[640];
    snprintf(text, sizeof(text),
             ",\"collisionMatrix\":[%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,"
             "%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g]",
             values[0], values[1], values[2], values[3], values[4], values[5],
             values[6], values[7], values[8], values[9], values[10], values[11],
             values[12], values[13], values[14], values[15]);
    json += text;
}

void AppendShotHitboxes(std::string& json, void* shot)
{
    json += ",\"hitboxes\":[";
    bool first = true;
    using GetPrimitiveFn = uint64_t(__fastcall*)(uint64_t, uint32_t);
    auto getMetadata = (uint64_t(__fastcall*)(uint64_t, uint32_t))_addr(0x1405AA900);
    uint32_t hitboxIndex = 0;
    for (uint32_t slotIndex = 0; slotIndex < 4; ++slotIndex) {
        uint64_t slot = *(uint64_t*)((uint8_t*)shot + 0x2058 + slotIndex * 8);
        if (!slot || !IsAccessiblePointer(slot)) continue;
        uint64_t vtable = *(uint64_t*)slot;
        if (!vtable || !IsAccessiblePointer(vtable) ||
            !IsAccessibleAddress(vtable + 0x88)) continue;
        GetPrimitiveFn getPrimitive = *(GetPrimitiveFn*)(vtable + 0x88);
        if (!getPrimitive || !IsAccessibleAddress((uint64_t)getPrimitive)) continue;
        uint32_t count = *(uint32_t*)(slot + 0x20);
        if (count > 128) count = 128;
        for (uint32_t i = 0; i < count; ++i) {
            uint64_t primitive = getPrimitive(slot, i);
            uint64_t metadata = getMetadata(slot, i);
            if (!primitive || !metadata || !IsAccessiblePointer(primitive) ||
                !IsAccessiblePointer(metadata)) continue;
            uint32_t type = *(uint32_t*)(metadata + 0x14);
            if (type > 1) continue;
            float x = *(float*)(primitive + 0x20);
            float y = *(float*)(primitive + 0x24);
            float z = *(float*)(primitive + 0x28);
            float radius = *(float*)(primitive + (type ? 0x40 : 0x2C));
            float x2 = 0.0f;
            float y2 = 0.0f;
            float z2 = 0.0f;
            if (type == 1) {
                x2 = *(float*)(primitive + 0x30);
                y2 = *(float*)(primitive + 0x34);
                z2 = *(float*)(primitive + 0x38);
            }
            if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z) ||
                !std::isfinite(x2) || !std::isfinite(y2) || !std::isfinite(z2) ||
                !std::isfinite(radius) || radius <= 0.0f || radius > 1000.0f) continue;
            char entry[512];
            if (type == 1) {
                snprintf(entry, sizeof(entry),
                         "%s{\"bank\":\"shot\",\"index\":%u,\"slot\":%u,\"shape\":\"capsule\","
                         "\"pos\":[%.9g,%.9g,%.9g],\"endPos\":[%.9g,%.9g,%.9g],"
                         "\"size\":[%.9g,%.9g,%.9g],\"radius\":%.9g}",
                         first ? "" : ",", hitboxIndex++, slotIndex,
                         x, y, z, x2, y2, z2,
                         radius * 2.0f, radius * 2.0f, radius * 2.0f, radius);
            } else {
                snprintf(entry, sizeof(entry),
                         "%s{\"bank\":\"shot\",\"index\":%u,\"slot\":%u,\"shape\":\"sphere\","
                         "\"pos\":[%.9g,%.9g,%.9g],\"size\":[%.9g,%.9g,%.9g],"
                         "\"radius\":%.9g}",
                         first ? "" : ",", hitboxIndex++, slotIndex,
                         x, y, z, radius * 2.0f, radius * 2.0f, radius * 2.0f, radius);
            }
            json += entry;
            first = false;
        }
    }
    json += "]";
}

std::string GenerateShots()
{
    std::string json = "[";
    bool first = true;
    for (int list = 0; list < SHOT_LIST_COUNT; ++list) {
        uint64_t sentinel = (uint64_t)_addr(0x140d47f90) + list * 0x28;
        uint64_t sentinelRef = sentinel + 0x08;
        uint64_t nodeRef = *(uint64_t*)(sentinel + 0x10);
        for (int safety = DYNAMIC_SAFETY;
             nodeRef && nodeRef != sentinelRef && safety-- > 0;
             nodeRef = *(uint64_t*)(nodeRef + SHOT_NODE_NEXT)) {
            void* shot = (void*)(nodeRef - SHOT_NODE_OFFSET);
            uint8_t* bytes = (uint8_t*)shot;
            NDJSON_ShotRecipe recipe = {};
            bool hasRecipe = NDJSON_GetShotRecipe(shot, recipe);
            char entry[1024];
            snprintf(entry, sizeof(entry),
                     "%s{\"list\":%d,\"addr\":\"0x%llX\",\"slot\":%u,"
                     "\"init\":%llu,\"kind\":\"%s\",\"owner\":\"0x%llX\","
                     "\"element\":\"0x%llX\",\"state\":\"0x%08X\","
                     "\"pos\":[%.9g,%.9g,%.9g],\"runtimePos\":[%.9g,%.9g,%.9g],"
                     "\"lifeFrame\":%.9g,\"velocity\":[%.9g,%.9g,%.9g],"
                     "\"speed\":%.9g,\"step\":%.9g,\"timer\":%.9g",
                     first ? "" : ",", list, (uint64_t)shot, recipe.slotId,
                     (unsigned long long)recipe.initParam,
                     hasRecipe ? (recipe.extended ? "extended" : "standard") : "unknown",
                     recipe.ownerChr, recipe.shotElement, *(uint32_t*)(bytes + 0x10),
                     *(float*)(bytes + 0x50), *(float*)(bytes + 0x54), *(float*)(bytes + 0x58),
                     *(float*)(bytes + 0x418), *(float*)(bytes + 0x41C), *(float*)(bytes + 0x420),
                     *(float*)(bytes + 0x2078), *(float*)(bytes + 0x20C0),
                     *(float*)(bytes + 0x20C4), *(float*)(bytes + 0x20C8),
                     *(float*)(bytes + 0x20D0), *(float*)(bytes + 0x212C),
                     *(float*)(bytes + 0x1F60));
            std::string shotJson(entry);
            AppendShotCollision(shotJson, shot);
            AppendShotHitboxes(shotJson, shot);
            shotJson += "}";
            json += shotJson;
            first = false;
        }
    }
    return json + "]";
}

void DumpBinary(int session, int32_t frame, uint8_t side)
{
    if (!g_diagDir[0]) return;
    std::vector<NDJSON_Region> regions;
    NDJSON_GetRegions(regions);
    if (regions.empty()) return;

    char path[260];
    snprintf(path, sizeof(path), "%s/session_%d.bin", g_diagDir, session);
    bool isNew = false;
    FILE* file = fopen(path, "rb");
    if (!file) {
        file = fopen(path, "wb");
        if (!file) return;
        isNew = true;
        const uint32_t header[] = {0x44534252, 2, (uint32_t)session, 0};
        fwrite(header, sizeof(header), 1, file);
    } else {
        fclose(file);
        file = fopen(path, "ab");
        if (!file) return;
    }

    ++g_diagWriteCount;
    int32_t storedFrame = side == 0 ? frame : frame + 1;
    fwrite(&storedFrame, sizeof(storedFrame), 1, file);
    fwrite(&side, sizeof(side), 1, file);
    uint32_t count = (uint32_t)regions.size();
    fwrite(&count, sizeof(count), 1, file);
    uint32_t totalSize = 0;
    for (const NDJSON_Region& region : regions) totalSize += (uint32_t)region.size;
    fwrite(&totalSize, sizeof(totalSize), 1, file);
    for (const NDJSON_Region& region : regions) {
        uint32_t size = (uint32_t)region.size;
        uint64_t hash = Rollback_Fnv1a64(region.data, region.size);
        fwrite(&size, sizeof(size), 1, file);
        fwrite(&hash, sizeof(hash), 1, file);
        fwrite(region.data, 1, size, file);
    }
    fclose(file);

    if (isNew) {
        file = fopen(path, "r+b");
        if (file) {
            uint32_t entryCount = (uint32_t)g_diagWriteCount;
            fseek(file, 12, SEEK_SET);
            fwrite(&entryCount, sizeof(entryCount), 1, file);
            fclose(file);
        }
    }
}

void DumpCharacterText(int session, int32_t frame, uint8_t side)
{
    if (!g_diagDir[0]) return;
    char path[260];
    snprintf(path, sizeof(path), "%s/session_%d.txt", g_diagDir, session);
    FILE* file = fopen(path, "a");
    if (!file) return;

    int32_t displayFrame = side == 0 ? frame : frame + 1;
    fprintf(file, "=== %s frame=%d ===\n", side == 0 ? "LIVE" : "RESIM", displayFrame);
    if (side == 0) {
        fprintf(file, "# NOTE: LIVE frame=%d labels the resim window start, actual state is at current live frame\n",
                frame);
    }
    uint64_t characters = *(uint64_t*)_addr(PTR_SCHARACTER);
    if (!characters) {
        fputs("  (no sCharacter)\n\n", file);
        fclose(file);
        return;
    }
    for (int team = 0; team < 2; ++team) {
        uint64_t node = characters + TEAM_OFFSETS[team] + TEAM_FIRST_NODE_OFFSET;
        int slot = 0;
        while (node) {
            uint64_t character = *(uint64_t*)(node + NODE_CHR_OFFSET);
            if (character) {
                CharDiagSnapshot snapshot;
                CaptureCharSnapshot(character, team, slot, snapshot);
                uint64_t frameCount = *(uint64_t*)(snapshot.cchr + 0x18);
                uint32_t directionSlot = (uint32_t)(frameCount & 0x3F);
                fprintf(file, "  Team%d Char%d @ 0x%llX\n", team, slot, character);
                fprintf(file, "    pos=(%+.4f,%+.4f,%+.4f) act=0x%08X fl=0x%08X\n",
                        snapshot.posX, snapshot.posY, snapshot.posZ,
                        snapshot.actionId, snapshot.flags14F0);
                fprintf(file, "    cci: mOld=0x%08X mHeld=0x%08X remap=0x%08X mask=0x%08X proc=0x%08X\n",
                        *(uint32_t*)(snapshot.cchr + 0x10), *(uint32_t*)(snapshot.cchr + 0x14),
                        *(uint32_t*)(snapshot.cchr + 0x560), *(uint32_t*)(snapshot.cchr + 0x568),
                        *(uint32_t*)(snapshot.cchr + 0x56C));
                fprintf(file, "    cci: cntr=%llu h590=0x%08X h594=0x%08X\n",
                        (unsigned long long)frameCount, *(uint32_t*)(snapshot.cchr + 0x590),
                        *(uint32_t*)(snapshot.cchr + 0x594));
                fprintf(file, "    cci: heldR0=0x%016llX relHead=0x%08X relR0=0x%016llX prsR0=0x%016llX\n",
                        *(uint64_t*)(snapshot.cchr + 0x28), *(uint32_t*)(snapshot.cchr + 0x0A8),
                        *(uint64_t*)(snapshot.cchr + 0x0AC), *(uint64_t*)(snapshot.cchr + 0x130));
                fprintf(file, "    cci: dir0=0x%08X dir4=0x%08X dirRing[0]=0x%016llX [%u]=0x%016llX\n",
                        *(uint32_t*)(snapshot.cchr + 0x1B0), *(uint32_t*)(snapshot.cchr + 0x1B4),
                        *(uint64_t*)(snapshot.cchr + 0x360), directionSlot,
                        *(uint64_t*)(snapshot.cchr + 0x360 + directionSlot * 8));
                fprintf(file, "    rng: 0x%08X 0x%08X 0x%08X 0x%08X\n",
                        snapshot.xorShift[0], snapshot.xorShift[1],
                        snapshot.xorShift[2], snapshot.xorShift[3]);
                ++slot;
            }
            node = *(uint64_t*)(node + NODE_NEXT_OFFSET);
        }
    }
    fputc('\n', file);
    fclose(file);
}

void DumpManifest(int session, int32_t frame, uint8_t side)
{
    if (!g_diagDir[0]) return;
    std::vector<NDJSON_Region> regions;
    NDJSON_GetRegions(regions);
    if (regions.empty()) return;
    char path[260];
    snprintf(path, sizeof(path), "%s/session_%d.txt", g_diagDir, session);
    FILE* file = fopen(path, "a");
    if (!file) return;
    fprintf(file, "=== MANIFEST %s frame=%d regions=%llu ===\n",
            side == 0 ? "LIVE" : "RESIM", side == 0 ? frame : frame + 1,
            (unsigned long long)regions.size());
    for (size_t i = 0; i < regions.size(); ++i) {
        fprintf(file, "  [%3llu] addr=0x%016llX size=%-8u hash=0x%016llX\n",
                (unsigned long long)i, (uint64_t)regions[i].data,
                (uint32_t)regions[i].size,
                Rollback_Fnv1a64(regions[i].data, regions[i].size));
    }
    fputc('\n', file);
    fclose(file);
}

} // namespace

void NDJSON_GetRegions(std::vector<NDJSON_Region>& regions)
{
    RollbackRegion snapshotRegions[ROLLBACK_MAX_REGIONS];
    int count = Rollback_BuildRegions(snapshotRegions, ROLLBACK_MAX_REGIONS);
    regions.clear();
    regions.reserve(count);
    for (int i = 0; i < count; ++i)
        regions.push_back({snapshotRegions[i].ptr, snapshotRegions[i].size});
}

bool NDJSON_GetShotRecipe(void* shot, NDJSON_ShotRecipe& recipe)
{
    RollbackShotRecipe found;
    if (!Shot_GetRecipe(shot, found)) return false;
    recipe.slotId = found.slotId;
    recipe.initParam = found.initParam;
    recipe.ownerChr = (uint64_t)found.ownerChr;
    recipe.shotElement = (uint64_t)found.shotElement;
    recipe.extended = found.kind == RollbackShotSpawnKind::Extended;
    return true;
}

std::string NDJSON_GenerateNetplayLive(int32_t frame, bool isResim,
                                       const char* phase, int32_t rollbackStart,
                                       int32_t rollbackEnd)
{
    if (!g_ndjsonLoggingEnabled || frame == 0) return {};
    SYSTEMTIME time;
    GetLocalTime(&time);
    char timestamp[32];
    snprintf(timestamp, sizeof(timestamp), "%04d-%02d-%02dT%02d:%02d:%02d",
             time.wYear, time.wMonth, time.wDay, time.wHour, time.wMinute, time.wSecond);
    std::string side = Netplay_IsHost() ? "host" : "client";
    if (phase) side += std::string("_") + phase;
    else if (isResim) side += "_resim";

    float deltaTime = 0.0f;
    uint64_t main = *(uint64_t*)_addr(PTR_SMAIN);
    if (main) deltaTime = *(float*)(main + 0x80);
    char header[512];
    if (rollbackStart >= 0 && rollbackEnd >= rollbackStart) {
        snprintf(header, sizeof(header),
                 "{\"v\":1,\"ts\":\"%s\",\"frame\":%d,\"side\":\"%s\","
                 "\"rollback_start\":%d,\"rollback_end\":%d,\"dt\":%.9g,\"chars\":[",
                 timestamp, frame, side.c_str(), rollbackStart, rollbackEnd, deltaTime);
    } else {
        snprintf(header, sizeof(header),
                 "{\"v\":1,\"ts\":\"%s\",\"frame\":%d,\"side\":\"%s\","
                 "\"dt\":%.9g,\"chars\":[",
                 timestamp, frame, side.c_str(), deltaTime);
    }

    std::string json = header;
    bool first = true;
    uint64_t characters = *(uint64_t*)_addr(PTR_SCHARACTER);
    if (characters) {
        for (int team = 0; team < 2; ++team) {
            uint64_t node = characters + TEAM_OFFSETS[team] + TEAM_FIRST_NODE_OFFSET;
            for (int slot = 0; node; node = *(uint64_t*)(node + NODE_NEXT_OFFSET)) {
                uint64_t character = *(uint64_t*)(node + NODE_CHR_OFFSET);
                if (!character) continue;
                CharDiagSnapshot snapshot;
                CaptureCharSnapshot(character, team, slot++, snapshot);
                if (!first) json += ",";
                first = false;

                std::string dashState;
                char temporary[128];
                dashState = "\"acc260\":{";
                bool firstDash = true;
                for (int bit = 0; bit < 32; ++bit) {
                    float value = *(float*)(snapshot.cchr + 0x260 + bit * 4);
                    if (value != 0.0f) {
                        if (!firstDash) dashState += ",";
                        snprintf(temporary, sizeof(temporary), "\"%d\":%.9g", bit, value);
                        dashState += temporary;
                        firstDash = false;
                    }
                }
                dashState += "},\"tap2e0\":{";
                firstDash = true;
                for (int bit = 0; bit < 32; ++bit) {
                    int32_t value = *(int32_t*)(snapshot.cchr + 0x2e0 + bit * 4);
                    if (value != 0) {
                        if (!firstDash) dashState += ",";
                        snprintf(temporary, sizeof(temporary), "\"%d\":%d", bit, value);
                        dashState += temporary;
                        firstDash = false;
                    }
                }
                dashState += "},\"acc130\":{";
                firstDash = true;
                for (int bit = 0; bit < 32; ++bit) {
                    float value = *(float*)(snapshot.cchr + 0x130 + bit * 4);
                    if (value != 0.0f) {
                        if (!firstDash) dashState += ",";
                        snprintf(temporary, sizeof(temporary), "\"%d\":%.9g", bit, value);
                        dashState += temporary;
                        firstDash = false;
                    }
                }
                snprintf(temporary, sizeof(temporary),
                         "},\"m12c\":\"0x%08X\",\"m230\":\"0x%08X\","
                         "\"m238\":\"0x%08X\",\"m250\":\"0x%08X\"",
                         *(uint32_t*)(snapshot.cchr + 0x12c),
                         *(uint32_t*)(snapshot.cchr + 0x230),
                         *(uint32_t*)(snapshot.cchr + 0x238),
                         *(uint32_t*)(snapshot.cchr + 0x250));
                dashState += temporary;

                char entry[4096];
                if (g_ndjsonIncludeDirHistory) {
                    uint64_t dirFrameCount = *(uint64_t*)(snapshot.cchr + 0x18);
                    uint32_t dirSlot = dirFrameCount & 0x3F;
                    snprintf(entry, sizeof(entry),
                             "{\"team\":%d,\"slot\":%d,\"mid\":%d,\"plr\":%d,\"ast\":%d,"
                             "\"pos\":[%.9g,%.9g,%.9g],\"act\":\"0x%08X\","
                             "\"mChrF\":\"0x%08X\",\"mAnmF\":\"0x%08X\","
                             "\"mPrgF\":\"0x%08X\",\"mHeld\":\"0x%08X\",\"fcnt\":%llu,"
                             "\"mOld\":\"0x%08X\",\"mPressed\":\"0x%08X\","
                             "\"mReleased\":\"0x%08X\",\"mMask\":\"0x%08X\","
                             "\"mProcessed\":\"0x%08X\",\"dirSlot\":%u,"
                             "\"dir0\":\"0x%08X\",\"dir4\":\"0x%08X\","
                             "\"h590\":\"0x%08X\",\"h594\":\"0x%08X\","
                             "\"rng\":[\"0x%08X\",\"0x%08X\",\"0x%08X\",\"0x%08X\"]",
                             team, slot - 1, *(int32_t*)(character + 0x2DB0),
                             *(int32_t*)(character + 0x2DA0), *(int32_t*)(character + 0x2DBC),
                             snapshot.posX, snapshot.posY, snapshot.posZ, snapshot.actionId,
                             snapshot.flags14F0, snapshot.flags14F4, snapshot.flags14F8,
                             *(uint32_t*)(snapshot.cchr + 0x14),
                             (unsigned long long)dirFrameCount,
                             *(uint32_t*)(snapshot.cchr + 0x10),
                             *(uint32_t*)(snapshot.cchr + 0x20),
                             *(uint32_t*)(snapshot.cchr + 0x24),
                             *(uint32_t*)(snapshot.cchr + 0x568),
                             *(uint32_t*)(snapshot.cchr + 0x56C), dirSlot,
                             *(uint32_t*)(snapshot.cchr + 0x1B0),
                             *(uint32_t*)(snapshot.cchr + 0x1B4),
                             *(uint32_t*)(snapshot.cchr + 0x590),
                             *(uint32_t*)(snapshot.cchr + 0x594),
                             snapshot.xorShift[0], snapshot.xorShift[1],
                             snapshot.xorShift[2], snapshot.xorShift[3]);
                } else {
                snprintf(entry, sizeof(entry),
                         "{\"team\":%d,\"slot\":%d,\"mid\":%d,\"plr\":%d,\"ast\":%d,"
                         "\"pos\":[%.9g,%.9g,%.9g],\"act\":\"0x%08X\","
                         "\"mChrF\":\"0x%08X\",\"mAnmF\":\"0x%08X\","
                         "\"mPrgF\":\"0x%08X\",\"mHeld\":\"0x%08X\",\"fcnt\":%llu,"
                         "\"mOld\":\"0x%08X\",\"mPressed\":\"0x%08X\","
                         "\"mReleased\":\"0x%08X\",\"mMask\":\"0x%08X\","
                         "\"mProcessed\":\"0x%08X\","
                         "\"rng\":[\"0x%08X\",\"0x%08X\",\"0x%08X\",\"0x%08X\"]",
                         team, slot - 1,
                         *(int32_t*)(character + 0x2DB0), *(int32_t*)(character + 0x2DA0),
                         *(int32_t*)(character + 0x2DBC), snapshot.posX, snapshot.posY,
                         snapshot.posZ, snapshot.actionId, snapshot.flags14F0,
                         snapshot.flags14F4, snapshot.flags14F8,
                         *(uint32_t*)(snapshot.cchr + 0x14),
                         (unsigned long long)*(uint64_t*)(snapshot.cchr + 0x18),
                         *(uint32_t*)(snapshot.cchr + 0x10),
                         *(uint32_t*)(snapshot.cchr + 0x20),
                         *(uint32_t*)(snapshot.cchr + 0x24),
                         *(uint32_t*)(snapshot.cchr + 0x568),
                         *(uint32_t*)(snapshot.cchr + 0x56C), snapshot.xorShift[0],
                          snapshot.xorShift[1], snapshot.xorShift[2], snapshot.xorShift[3]);
                }
                json += entry;
                json += ",";
                json += dashState;
                json += ",\"hitboxes\":";
                json += GenerateCharacterHitboxes(character);
                json += "}";
            }
        }
    }
    json += "],\"shots\":" + GenerateShots() + "}\n";
    return json;
}

void NDJSON_WriteNetplayLive(const std::string& json)
{
    if (!g_ndjsonLoggingEnabled) return;
    if (!g_liveInit) InitLiveLog();
    if (!g_livePath[0] || g_desyncFlushed || json.empty()) return;
    const char* key = std::strstr(json.c_str(), "\"frame\":");
    int32_t frame = key ? (int32_t)std::strtol(key + 8, nullptr, 10) : -1;
    if (frame < 0) return;
    int index = (uint32_t)frame % NETPLAY_LIVE_HISTORY;
    if (g_historyFrame[index] != frame) {
        g_historyFrame[index] = frame;
        g_history[index].clear();
    }
    g_history[index] += json;
}

void NDJSON_DumpNetplayLive(int32_t frame)
{
    if (!g_ndjsonLoggingEnabled) return;
    if (!g_liveInit) InitLiveLog();
    std::string json = NDJSON_GenerateNetplayLive(frame, false);
    if (json.empty()) return;
    int index = (uint32_t)frame % PENDING_HISTORY;
    if (frame < 12 || Controller_IsFrameConfirmed(frame)) {
        NDJSON_WriteNetplayLive(json);
        g_pendingFrame[index] = -1;
    } else {
        g_pendingJson[index] = std::move(json);
        g_pendingFrame[index] = frame;
    }
}

void NDJSON_CaptureFrame(int32_t frame, bool isResim, bool postOnly)
{
    if (!g_ndjsonLoggingEnabled) return;
    FrameDiagSnapshot& destination = isResim
        ? g_resimFrameSnap[(uint32_t)(frame >= 0 ? frame : 0) % ROLLBACK_WINDOW_DIAG]
        : g_liveFrameSnap[(uint32_t)(frame >= 0 ? frame : 0) % ROLLBACK_WINDOW_DIAG];
    if (!postOnly) {
        destination.charCount = 0;
        destination.frame = frame;
    }
    uint64_t characters = *(uint64_t*)_addr(PTR_SCHARACTER);
    if (!characters) return;
    int index = 0;
    for (int team = 0; team < 2; ++team) {
        uint64_t node = characters + TEAM_OFFSETS[team] + TEAM_FIRST_NODE_OFFSET;
        int slot = 0;
        while (node && (postOnly ? index < destination.charCount : destination.charCount < 6)) {
            uint64_t character = *(uint64_t*)(node + NODE_CHR_OFFSET);
            if (character) {
                CharDiagSnapshot snapshot;
                CaptureCharSnapshot(character, team, slot++, snapshot);
                if (postOnly) destination.chars[index].post = snapshot;
                else {
                    destination.chars[destination.charCount].pre = snapshot;
                    destination.chars[destination.charCount++].post = snapshot;
                }
                ++index;
            }
            node = *(uint64_t*)(node + NODE_NEXT_OFFSET);
        }
    }
}

void NDJSON_DumpFrame(int32_t frame, uint8_t side)
{
    if (!g_ndjsonLoggingEnabled) return;
    InitDiag();
    if (!g_diagFile) return;

    std::vector<NDJSON_Region> regions;
    NDJSON_GetRegions(regions);
    if (regions.empty()) return;
    SYSTEMTIME time;
    GetLocalTime(&time);
    char timestamp[32];
    snprintf(timestamp, sizeof(timestamp), "%04d-%02d-%02dT%02d:%02d:%02d",
             time.wYear, time.wMonth, time.wDay, time.wHour, time.wMinute, time.wSecond);
    int32_t outputFrame = side == 0 ? frame : frame + 1;
    fprintf(g_diagFile, "{\"v\":1,\"ts\":\"%s\",\"frame\":%d,\"side\":\"%s\",\"regions\":[",
            timestamp, outputFrame, side == 0 ? "ground_truth" : "resim");
    for (size_t i = 0; i < regions.size(); ++i) {
        if (i) fputc(',', g_diagFile);
        fprintf(g_diagFile, "{\"idx\":%llu,\"addr\":\"0x%llX\",\"size\":%llu,\"hash\":\"0x%016llX\"}",
                (unsigned long long)i, (uint64_t)regions[i].data,
                (unsigned long long)regions[i].size,
                Rollback_Fnv1a64(regions[i].data, regions[i].size));
    }

    fputs("],\"chars\":[", g_diagFile);
    bool firstCharacter = true;
    uint64_t characters = *(uint64_t*)_addr(PTR_SCHARACTER);
    if (characters) {
        for (int team = 0; team < 2; ++team) {
            uint64_t node = characters + TEAM_OFFSETS[team] + TEAM_FIRST_NODE_OFFSET;
            int slot = 0;
            while (node) {
                uint64_t character = *(uint64_t*)(node + NODE_CHR_OFFSET);
                if (character) {
                    CharDiagSnapshot snapshot;
                    CaptureCharSnapshot(character, team, slot, snapshot);
                    if (!firstCharacter) fputc(',', g_diagFile);
                    firstCharacter = false;
                    fprintf(g_diagFile,
                            "{\"team\":%d,\"slot\":%d,\"pos\":[%.4f,%.4f,%.4f],"
                            "\"act\":\"0x%08X\",\"mChrF\":\"0x%08X\","
                            "\"mAnmF\":\"0x%08X\",\"mPrgF\":\"0x%08X\","
                            "\"mHeld\":\"0x%08X\",\"fcnt\":%llu,"
                            "\"mOld\":\"0x%08X\",\"mPressed\":\"0x%08X\","
                            "\"mReleased\":\"0x%08X\",\"mMask\":\"0x%08X\","
                            "\"mProcessed\":\"0x%08X\","
                            "\"rng\":[\"0x%08X\",\"0x%08X\",\"0x%08X\",\"0x%08X\"]}",
                            team, slot, snapshot.posX, snapshot.posY, snapshot.posZ,
                            snapshot.actionId, snapshot.flags14F0, snapshot.flags14F4,
                            snapshot.flags14F8, *(uint32_t*)(snapshot.cchr + 0x14),
                            (unsigned long long)*(uint64_t*)(snapshot.cchr + 0x18),
                            *(uint32_t*)(snapshot.cchr + 0x10),
                            *(uint32_t*)(snapshot.cchr + 0x20),
                            *(uint32_t*)(snapshot.cchr + 0x24),
                            *(uint32_t*)(snapshot.cchr + 0x568),
                            *(uint32_t*)(snapshot.cchr + 0x56C),
                            snapshot.xorShift[0], snapshot.xorShift[1],
                            snapshot.xorShift[2], snapshot.xorShift[3]);
                    ++slot;
                }
                node = *(uint64_t*)(node + NODE_NEXT_OFFSET);
            }
        }
    }
    fputs("],\"shots\":", g_diagFile);
    std::string shots = GenerateShots();
    fputs(shots.c_str(), g_diagFile);
    fputs("}\n", g_diagFile);
    FlushDiagPeriodically();
}

void NDJSON_DiscardPendingNetplayLive(int32_t frame)
{
    if (!g_ndjsonLoggingEnabled) return;
    int index = (uint32_t)frame % PENDING_HISTORY;
    if (g_pendingFrame[index] == frame) g_pendingFrame[index] = -1;
}

void NDJSON_CommitPendingNetplayLive(int32_t frame)
{
    if (!g_ndjsonLoggingEnabled) return;
    int index = (uint32_t)frame % PENDING_HISTORY;
    if (g_pendingFrame[index] == frame) {
        NDJSON_WriteNetplayLive(g_pendingJson[index]);
        g_pendingFrame[index] = -1;
    }
}

void NDJSON_QueuePendingResimDump(int32_t frame)
{
    if (!g_ndjsonLoggingEnabled || !Netplay_IsActive()) return;
    const FrameDiagSnapshot* snapshot = GetResimSnapshot(frame);
    if (!snapshot) return;
    const CharDiagSnapshot* p1;
    const CharDiagSnapshot* p2;
    if (!FindPointCharacters(*snapshot, p1, p2)) return;
    int index = (uint32_t)frame % PENDING_HISTORY;
    g_resimDumps[index].frame = frame;
    FillState(g_resimDumps[index].p1Post, *p1);
    FillState(g_resimDumps[index].p2Post, *p2);
    g_resimDumpValid[index] = true;
}

void NDJSON_SendResimNotify(int32_t startFrame, int32_t endFrame)
{
    if (!g_ndjsonLoggingEnabled) return;
    ResimNotifyPacket packet = {};
    packet.type = PACKET_RESIM_NOTIFY;
    packet.resimStartFrame = startFrame;
    packet.resimEndFrame = endFrame;
    packet.correctedTeam = Netplay_IsHost() ? 1 : 0;
    int32_t first = startFrame > 1 ? startFrame - 2 : 0;
    int count = 0;
    for (int32_t frame = first; frame < endFrame + 2 && count < MAX_RESIM_FRAMES; ++frame) {
        ResimFrameResult& result = packet.frames[count++];
        result.frame = frame;
        uint32_t liveIndex = (uint32_t)frame % 256;
        FillState(result.p1Pre, g_liveRef[0][liveIndex]);
        FillState(result.p2Pre, g_liveRef[1][liveIndex]);
        int pendingIndex = (uint32_t)frame % PENDING_HISTORY;
        if (frame >= startFrame && frame < endFrame && g_resimDumpValid[pendingIndex] &&
            g_resimDumps[pendingIndex].frame == frame) {
            result.p1Post = g_resimDumps[pendingIndex].p1Post;
            result.p2Post = g_resimDumps[pendingIndex].p2Post;
        } else {
            result.p1Post = result.p1Pre;
            result.p2Post = result.p2Pre;
        }
        result.usedInput = Controller_GetUsedRemoteInput(frame);
        result.recvInput = Controller_GetReceivedRemoteInput(frame);
    }
    packet.frameCount = count;
    Netplay_SendResimNotify(packet);
}

int NDJSON_BeginSession(bool useCurrentSession)
{
    InitDiag();
    if (useCurrentSession) return g_sessionId++;
    g_diagWriteCount = 0;
    return ++g_sessionId;
}

void NDJSON_DumpNetplayHistory(int32_t frame)
{
    if (!g_ndjsonLoggingEnabled) return;
    if (!g_liveInit) InitLiveLog();
    if (!g_livePath[0]) return;
    for (int i = 0; i < PENDING_HISTORY; ++i) {
        if (g_pendingFrame[i] >= 0) {
            NDJSON_WriteNetplayLive(g_pendingJson[i]);
            g_pendingFrame[i] = -1;
        }
    }
    int32_t newest = -1;
    for (int i = 0; i < NETPLAY_LIVE_HISTORY; ++i)
        if (g_historyFrame[i] > newest && (frame < 0 || g_historyFrame[i] <= frame))
            newest = g_historyFrame[i];
    if (newest < 0) return;
    FILE* file = fopen(g_livePath, "w");
    if (!file) return;
    for (int32_t current = newest - NETPLAY_LIVE_HISTORY + 1; current <= newest; ++current) {
        int index = (uint32_t)current % NETPLAY_LIVE_HISTORY;
        if (g_historyFrame[index] == current) fputs(g_history[index].c_str(), file);
    }
    fclose(file);
    HookLog("NetplayLive: manual dump of last %d frames through frame %d\n",
            NETPLAY_LIVE_HISTORY, newest);
}

bool NDJSON_ExportNetplayHistory(int32_t frame, const char* path)
{
    if (!g_ndjsonLoggingEnabled || !path || !path[0]) return false;
    if (!g_liveInit) InitLiveLog();
    int32_t newest = -1;
    for (int i = 0; i < NETPLAY_LIVE_HISTORY; ++i) {
        if (g_historyFrame[i] > newest &&
            (frame < 0 || g_historyFrame[i] <= frame))
            newest = g_historyFrame[i];
    }
    if (newest < 0) return false;

    FILE* file = fopen(path, "w");
    if (!file) return false;
    for (int32_t current = newest - NETPLAY_LIVE_HISTORY + 1;
         current <= newest; ++current) {
        int historyIndex = (uint32_t)current % NETPLAY_LIVE_HISTORY;
        if (g_historyFrame[historyIndex] == current)
            fputs(g_history[historyIndex].c_str(), file);
        int pendingIndex = (uint32_t)current % PENDING_HISTORY;
        if (g_pendingFrame[pendingIndex] == current)
            fputs(g_pendingJson[pendingIndex].c_str(), file);
    }
    bool success = fflush(file) == 0;
    fclose(file);
    HookLog("NetplayLive: exported autotest history through frame %d -> %s\n",
            newest, path);
    return success;
}

void NDJSON_ResetNetplayHistoryBuffers()
{
    for (int i = 0; i < PENDING_HISTORY; ++i) {
        g_pendingFrame[i] = -1;
        g_pendingJson[i].clear();
    }
    for (int i = 0; i < NETPLAY_LIVE_HISTORY; ++i) {
        g_historyFrame[i] = -1;
        g_history[i].clear();
    }
    g_desyncFlushed = false;
}

void NDJSON_NotifyDesyncTriggered(int32_t frame)
{
    if (!g_ndjsonLoggingEnabled) return;
    if (g_desyncFlushed) return;
    if (!g_liveInit) InitLiveLog();
    if (!g_livePath[0]) return;
    for (int i = 0; i < PENDING_HISTORY; ++i) {
        if (g_pendingFrame[i] >= 0) {
            NDJSON_WriteNetplayLive(g_pendingJson[i]);
            g_pendingFrame[i] = -1;
        }
    }
    FILE* file = fopen(g_livePath, "a");
    if (!file) return;
    for (int32_t current = frame - NETPLAY_LIVE_HISTORY + 1; current <= frame; ++current) {
        int index = (uint32_t)current % NETPLAY_LIVE_HISTORY;
        if (g_historyFrame[index] == current) fputs(g_history[index].c_str(), file);
    }
    fclose(file);
    g_desyncFlushed = true;
    HookLog("NetplayLive: flushed last %d frames through desync trigger frame %d\n",
            NETPLAY_LIVE_HISTORY, frame);
}

void NDJSON_FlushNetplayLive(int32_t frame)
{
    if (!g_ndjsonLoggingEnabled) return;
    if (!g_liveInit) InitLiveLog();
    if (!g_livePath[0] || g_desyncFlushed) return;
    for (int i = 0; i < PENDING_HISTORY; ++i) {
        if (g_pendingFrame[i] >= 0) {
            NDJSON_WriteNetplayLive(g_pendingJson[i]);
            g_pendingFrame[i] = -1;
        }
    }
    int32_t newest = frame;
    if (newest < 0) {
        for (int i = 0; i < NETPLAY_LIVE_HISTORY; ++i)
            if (g_historyFrame[i] > newest) newest = g_historyFrame[i];
    }
    if (newest < 0) return;
    FILE* file = fopen(g_livePath, "a");
    if (!file) return;
    for (int32_t current = newest - NETPLAY_LIVE_HISTORY + 1; current <= newest; ++current) {
        int index = (uint32_t)current % NETPLAY_LIVE_HISTORY;
        if (g_historyFrame[index] == current) fputs(g_history[index].c_str(), file);
    }
    fclose(file);
    if (g_desyncFlushed) return;
    g_desyncFlushed = true;
    HookLog("NetplayLive: final flush of last %d frames through frame %d\n",
            NETPLAY_LIVE_HISTORY, newest);
}

bool NDJSON_FlushOnCrash()
{
    bool flushed = false;
    if (g_diagFile && fflush(g_diagFile) == 0) flushed = true;
    if (!g_ndjsonLoggingEnabled || !g_liveInit || !g_livePath[0] || g_desyncFlushed)
        return flushed;
    int32_t newest = -1;
    for (int i = 0; i < NETPLAY_LIVE_HISTORY; ++i)
        if (g_historyFrame[i] > newest) newest = g_historyFrame[i];
    for (int i = 0; i < PENDING_HISTORY; ++i)
        if (g_pendingFrame[i] > newest) newest = g_pendingFrame[i];
    if (newest < 0) return flushed;
    FILE* file = fopen(g_livePath, "a");
    if (!file) return flushed;
    for (int32_t frame = newest - NETPLAY_LIVE_HISTORY + 1; frame <= newest; ++frame) {
        int historyIndex = (uint32_t)frame % NETPLAY_LIVE_HISTORY;
        if (g_historyFrame[historyIndex] == frame) {
            const std::string& json = g_history[historyIndex];
            fwrite(json.data(), 1, json.size(), file);
        }
        int pendingIndex = (uint32_t)frame % PENDING_HISTORY;
        if (g_pendingFrame[pendingIndex] == frame) {
            const std::string& json = g_pendingJson[pendingIndex];
            fwrite(json.data(), 1, json.size(), file);
        }
    }
    flushed = fflush(file) == 0 || flushed;
    fclose(file);
    return flushed;
}

void NDJSON_ResetNetplayLiveLog()
{
    if (!g_ndjsonLoggingEnabled) return;
    ++g_liveReset;
    InitLiveLog();
}

bool NDJSON_GetNetplayLiveDirectory(char* path, size_t pathSize)
{
    if (!path || pathSize == 0) return false;
    if (!g_liveInit) InitLiveLog();
    if (!g_liveInit || !g_livePath[0]) return false;
    const char* slash = strrchr(g_livePath, '/');
    const char* backslash = strrchr(g_livePath, '\\');
    if (!slash || (backslash && backslash > slash)) slash = backslash;
    if (!slash) return false;
    size_t length = (size_t)(slash - g_livePath);
    if (length + 1 > pathSize) return false;
    memcpy(path, g_livePath, length);
    path[length] = '\0';
    return true;
}

void NDJSON_LogResimNotify(const ResimNotifyPacket& packet)
{
    if (!g_ndjsonLoggingEnabled) return;
    InitDiag();
    if (!g_diagFile) return;

    SYSTEMTIME time;
    GetLocalTime(&time);
    char timestamp[32];
    snprintf(timestamp, sizeof(timestamp), "%04d-%02d-%02dT%02d:%02d:%02d",
             time.wYear, time.wMonth, time.wDay, time.wHour, time.wMinute, time.wSecond);
    const char* senderSide = Netplay_IsHost() ? "host" : "client";
    uint32_t frameCount = packet.frameCount;
    if (frameCount > MAX_RESIM_FRAMES) frameCount = MAX_RESIM_FRAMES;
    for (uint32_t i = 0; i < frameCount; ++i) {
        const ResimFrameResult& result = packet.frames[i];
        auto writeEntry = [&](const char* side, const ResimCharState& p1,
                              const ResimCharState& p2) {
            fprintf(g_diagFile,
                    "{\"v\":1,\"ts\":\"%s\",\"frame\":%d,\"side\":\"%s\","
                    "\"resim_notify\":true,\"senderSide\":\"%s\","
                    "\"correctedTeam\":%u,\"corrected_frames\":[%d,%d],\"chars\":[",
                    timestamp, result.frame, side, senderSide, packet.correctedTeam,
                    packet.resimStartFrame, packet.resimEndFrame);
            WriteCharacterState(g_diagFile, 0, p1);
            WriteCharacterState(g_diagFile, 1, p2);
            fputs("]}\n", g_diagFile);
        };
        writeEntry("ground_truth", result.p1Pre, result.p2Pre);
        writeEntry("resim", result.p1Post, result.p2Post);
    }
    FlushDiagPeriodically();
}

void NDJSON_SendGroundTruth(int32_t startFrame, int32_t endFrame)
{
    if (!g_ndjsonLoggingEnabled || startFrame >= endFrame) return;
    GroundTruthPacket packet = {};
    packet.type = PACKET_GROUND_TRUTH;
    packet.startFrame = startFrame;
    packet.endFrame = endFrame;
    for (int32_t frame = startFrame;
         frame < endFrame && packet.frameCount < MAX_RESIM_FRAMES; ++frame) {
        GroundTruthFrame& result = packet.frames[packet.frameCount++];
        result.frame = frame;
        uint32_t index = (uint32_t)frame % 256;
        FillState(result.p1Live, g_liveRef[0][index]);
        FillState(result.p2Live, g_liveRef[1][index]);
    }
    Netplay_SendGroundTruth(packet);
}

void NDJSON_ProcessInboundGroundTruth()
{
    GroundTruthPacket packet;
    while (Netplay_GetInboundGroundTruth(packet)) {
        if (!g_ndjsonLoggingEnabled) continue;
        InitDiag();
        if (!g_diagFile) continue;

        SYSTEMTIME time;
        GetLocalTime(&time);
        char timestamp[32];
        snprintf(timestamp, sizeof(timestamp), "%04d-%02d-%02dT%02d:%02d:%02d",
                 time.wYear, time.wMonth, time.wDay, time.wHour, time.wMinute, time.wSecond);
        uint32_t frameCount = packet.frameCount;
        if (frameCount > MAX_RESIM_FRAMES) frameCount = MAX_RESIM_FRAMES;
        for (uint32_t i = 0; i < frameCount; ++i) {
            const GroundTruthFrame& result = packet.frames[i];
            int index = (uint32_t)(result.frame >= 0 ? result.frame : 0) % PENDING_HISTORY;
            if (!g_resimDumpValid[index] || g_resimDumps[index].frame != result.frame)
                continue;

            PendingResimDump resim = g_resimDumps[index];
            g_resimDumpValid[index] = false;
            WriteComparisonEntry(g_diagFile, timestamp, result.frame, "ground_truth",
                                 result.p1Live, result.p2Live);
            WriteComparisonEntry(g_diagFile, timestamp, result.frame, "resim",
                                 resim.p1Post, resim.p2Post);
        }
        FlushDiagPeriodically();
    }
}

extern "C" void Rollback_NotifyDesyncTriggered(int32_t frame)
{
    if (g_pendingDesyncTriggerFrame >= 0) return;
    g_pendingDesyncTriggerFrame = frame;
    g_pendingDesyncCaptureFrame = frame + 2;
    g_pendingDesyncCaptureWritten = false;
    HookLog("Desync: delaying pause and snapdump through frame %d\n",
            frame + 2);
}

extern "C" bool Rollback_ProcessDesyncDelay(int32_t currentFrame)
{
    if (g_pendingDesyncTriggerFrame < 0) return false;
    const int32_t captureFrame = g_pendingDesyncCaptureFrame;
    if (currentFrame < captureFrame) return true;
    if (!g_pendingDesyncCaptureWritten) {
#if ROLLBACK_SNAPDUMP_ENABLED
        char autoTestPath[MAX_PATH] = {};
        // Capture after the two-frame grace period so the dump contains the
        // state immediately after the checksum mismatch, not only the stale
        // history ending at the trigger frame.
        SnapDump_WriteHistory(captureFrame,
            AutoTest_GetSnapDumpRelativePath(autoTestPath, sizeof(autoTestPath))
                ? autoTestPath : nullptr);
#endif
        NDJSON_NotifyDesyncTriggered(g_pendingDesyncTriggerFrame);
        g_pendingDesyncCaptureWritten = true;
    }
    g_pendingDesyncTriggerFrame = -1;
    g_pendingDesyncCaptureFrame = -1;
    return false;
}

extern "C" void Rollback_FlushNetplayLive(int32_t frame)
{
    NDJSON_FlushNetplayLive(frame);
}

void Rollback_DumpNetplayHistory(int32_t frame)
{
    NDJSON_DumpNetplayHistory(frame);
}

bool Rollback_ExportNetplayHistory(int32_t frame, const char* path)
{
    return NDJSON_ExportNetplayHistory(frame, path);
}

void Rollback_ResetNetplayHistoryBuffers()
{
    NDJSON_ResetNetplayHistoryBuffers();
}

bool Rollback_FlushNDJSONOnCrash()
{
    return NDJSON_FlushOnCrash();
}

void Rollback_ResetNetplayLiveLog()
{
    NDJSON_ResetNetplayLiveLog();
}

bool Rollback_GetNetplayLiveDirectory(char* path, size_t pathSize)
{
    return NDJSON_GetNetplayLiveDirectory(path, pathSize);
}

void Rollback_LogResimNotifyNDJSON(const ResimNotifyPacket& packet)
{
    NDJSON_LogResimNotify(packet);
}

void Rollback_QueuePendingResimDump(int32_t frame)
{
    NDJSON_QueuePendingResimDump(frame);
}

extern "C" void Rollback_SendGroundTruth(int32_t startFrame, int32_t endFrame)
{
    NDJSON_SendGroundTruth(startFrame, endFrame);
}

extern "C" void Rollback_ProcessInboundGroundTruth()
{
    NDJSON_ProcessInboundGroundTruth();
}

extern "C" void Rollback_CommitPendingNetplayLive(int32_t frame)
{
    NDJSON_CommitPendingNetplayLive(frame);
}
