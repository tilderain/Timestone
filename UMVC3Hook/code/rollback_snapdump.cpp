#include "../pch.h"

#include "rollback_snapdump.h"

#if ROLLBACK_SNAPDUMP_ENABLED

#include "mk10utils.h"
#include "netplay.h"
#include "rollback_diagnostics.h"
#include "rollback_snapshot.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <new>
#include <vector>

extern void HookLog(const char* fmt, ...);

namespace {

constexpr uint32_t DUMP_MAGIC = 0x50445355; // USDP
constexpr uint32_t DUMP_VERSION = 2;
constexpr int HISTORY_SIZE = 255;
constexpr uint8_t ENCODING_RAW = 0;
constexpr uint8_t ENCODING_XOR_RLE = 1;

#pragma pack(push, 1)
struct DumpHeader {
    uint32_t magic;
    uint32_t version;
    uint32_t headerSize;
    uint32_t frameCount;
    int32_t triggerFrame;
    uint32_t role;
    uint64_t imageBase;
    uint64_t imageSize;
    uint64_t timestampUnix;
    char gitCommit[41];
    uint8_t gitDirty;
    uint8_t reserved[6];
};

struct FrameHeader {
    int32_t frame;
    uint32_t localInput;
    uint32_t remoteInput;
    uint32_t regionCount;
    uint32_t shotCount;
    uint32_t effectCount;
    uint32_t collisionCloneCount;
    uint32_t modelRenderStateCount;
    uint64_t snapshotHash;
    uint64_t manifestHash;
    uint64_t uncompressedSize;
    uint64_t encodedSize;
    uint8_t encoding;
    uint8_t reserved[7];
};

struct RegionHeader {
    uint64_t address;
    uint64_t size;
    uint64_t ownerSlot;
    uint64_t expectedVtable;
    uint64_t hash;
    uint8_t skipVtable;
    uint8_t reserved[7];
    char name[64];
};
#pragma pack(pop)

static_assert(DUMP_VERSION == 2);
static_assert(sizeof(RegionHeader::name) == 64);

struct HistoryFrame {
    int32_t frame = -1;
    uint32_t localInput = 0;
    uint32_t remoteInput = 0;
    uint32_t shotCount = 0;
    uint32_t effectCount = 0;
    uint32_t collisionCloneCount = 0;
    uint32_t modelRenderStateCount = 0;
    uint32_t regionCount = 0;
    size_t byteSize = 0;
    size_t regionOffset = 0;
    size_t capacity = 0;
    uint8_t* storage = nullptr;
};

struct CapturedRegion {
    uint64_t address;
    uint64_t size;
    uint64_t ownerSlot;
    uint64_t expectedVtable;
    const char* name;
    bool skipVtable;
};

HistoryFrame g_history[HISTORY_SIZE];
bool g_dumpWritten = false;

uint64_t HashManifest(const RegionHeader* regions, size_t count)
{
    uint64_t hash = 1469598103934665603ull;
    for (size_t index = 0; index < count; ++index) {
        const RegionHeader& region = regions[index];
        const uint8_t* bytes = reinterpret_cast<const uint8_t*>(&region.size);
        for (size_t i = 0; i < sizeof(region.size); ++i) {
            hash ^= bytes[i];
            hash *= 1099511628211ull;
        }
        hash ^= region.skipVtable;
        hash *= 1099511628211ull;
    }
    return hash;
}

std::vector<uint8_t> EncodeXorRle(const uint8_t* current, size_t currentSize,
                                  const uint8_t* previous, size_t previousSize)
{
    std::vector<uint8_t> encoded;
    if (!previous || previousSize != currentSize) return encoded;
    size_t offset = 0;
    while (offset < currentSize) {
        const bool zero = (current[offset] ^ previous[offset]) == 0;
        size_t run = 1;
        while (offset + run < currentSize && run < 0x7fffffffu &&
               (((current[offset + run] ^ previous[offset + run]) == 0) == zero)) {
            ++run;
        }
        uint32_t tag = ((uint32_t)run << 1) | (zero ? 0u : 1u);
        do {
            uint8_t byte = (uint8_t)(tag & 0x7f);
            tag >>= 7;
            encoded.push_back(tag ? (uint8_t)(byte | 0x80) : byte);
        } while (tag);
        if (!zero) {
            for (size_t i = 0; i < run; ++i)
                encoded.push_back(current[offset + i] ^ previous[offset + i]);
        }
        offset += run;
    }
    return encoded;
}

bool WriteAll(FILE* file, const void* data, size_t size)
{
    return size == 0 || fwrite(data, 1, size, file) == size;
}

bool CreateParentDirectories(char* path)
{
    for (char* separator = path; *separator; ++separator) {
        if (*separator != '\\' && *separator != '/')
            continue;
        char saved = *separator;
        *separator = '\0';
        bool validRoot = separator - path > 2;
        bool created = !validRoot || CreateDirectoryA(path, nullptr) ||
                       GetLastError() == ERROR_ALREADY_EXISTS;
        *separator = saved;
        if (!created)
            return false;
    }
    return true;
}

} // namespace

void SnapDump_CaptureSnapshot(const RollbackSnapshotSlot& slot)
{
    if (slot.frame < 0 || !slot.data || slot.size == 0 || slot.regionCount <= 0) return;
    HistoryFrame& destination = g_history[(uint32_t)slot.frame % HISTORY_SIZE];
    const size_t regionOffset = (slot.size + alignof(CapturedRegion) - 1) &
                                ~(alignof(CapturedRegion) - 1);
    const size_t required = regionOffset +
                            (size_t)slot.regionCount * sizeof(CapturedRegion);
    if (destination.capacity < required) {
        const size_t megabyte = 1024 * 1024;
        const size_t newCapacity = (required + megabyte - 1) & ~(megabyte - 1);
        uint8_t* storage = new (std::nothrow) uint8_t[newCapacity];
        if (!storage) {
            destination.frame = -1;
            HookLog("SnapDump: failed to allocate %zu bytes for frame %d\n",
                    newCapacity, slot.frame);
            return;
        }
        delete[] destination.storage;
        destination.storage = storage;
        destination.capacity = newCapacity;
    }

    std::memcpy(destination.storage, slot.data, slot.size);
    CapturedRegion* captured = reinterpret_cast<CapturedRegion*>(
        destination.storage + regionOffset);
    for (int i = 0; i < slot.regionCount; ++i) {
        const RollbackRegion& source = slot.regions[i];
        captured[i] = {
            (uint64_t)source.ptr,
            source.size,
            source.ownerSlot,
            source.expectedVtable,
            source.snapDumpName,
            source.skipVtable,
        };
    }

    destination.frame = slot.frame;
    destination.localInput = slot.localInput;
    destination.remoteInput = slot.remoteInput;
    destination.shotCount = (uint32_t)slot.shotRecipes.size();
    destination.effectCount = (uint32_t)slot.effectRecipes.size();
    destination.collisionCloneCount = (uint32_t)slot.collisionNodeClones.size();
    destination.modelRenderStateCount = (uint32_t)slot.modelRenderStates.size();
    destination.regionCount = (uint32_t)slot.regionCount;
    destination.byteSize = slot.size;
    destination.regionOffset = regionOffset;
    g_dumpWritten = false;
}

void SnapDump_UpdateInputs(int32_t frame, uint32_t localInput, uint32_t remoteInput)
{
    if (frame < 0) return;
    HistoryFrame& history = g_history[(uint32_t)frame % HISTORY_SIZE];
    if (history.frame == frame) {
        history.localInput = localInput;
        history.remoteInput = remoteInput;
    }
}

bool SnapDump_WriteHistory(int32_t triggerFrame, const char* relativePath)
{
    if (g_dumpWritten) return true;
    std::vector<const HistoryFrame*> frames;
    for (const HistoryFrame& frame : g_history) {
        if (frame.frame >= 0 && (triggerFrame < 0 || frame.frame <= triggerFrame))
            frames.push_back(&frame);
    }
    std::sort(frames.begin(), frames.end(), [](const HistoryFrame* a, const HistoryFrame* b) {
        return a->frame < b->frame;
    });
    if (frames.empty()) return false;

    SYSTEMTIME time;
    GetLocalTime(&time);
    const char* role = Netplay_IsActive() ? (Netplay_IsHost() ? "host" : "client") : "offline";
    char defaultRelative[128];
    if (!relativePath || !relativePath[0]) {
        snprintf(defaultRelative, sizeof(defaultRelative),
                 "desync_%04d%02d%02d_%02d%02d%02d_%s_f%d.usdp",
                 time.wYear, time.wMonth, time.wDay, time.wHour, time.wMinute,
                 time.wSecond, role, triggerFrame);
        relativePath = defaultRelative;
    }
    char path[512];
    if (!Rollback_ResolveDllPath(path, sizeof(path), relativePath) ||
        !CreateParentDirectories(path))
        return false;
    FILE* file = fopen(path, "wb");
    if (!file) return false;

    DumpHeader header = {};
    header.magic = DUMP_MAGIC;
    header.version = DUMP_VERSION;
    header.headerSize = sizeof(header);
    header.frameCount = (uint32_t)frames.size();
    header.triggerFrame = triggerFrame;
    header.role = Netplay_IsActive() ? (Netplay_IsHost() ? 1u : 2u) : 0u;
    header.imageBase = (uint64_t)GetMKXEntryPoint();
    const IMAGE_DOS_HEADER* dos = (const IMAGE_DOS_HEADER*)header.imageBase;
    if (dos && dos->e_magic == IMAGE_DOS_SIGNATURE) {
        const IMAGE_NT_HEADERS64* nt = (const IMAGE_NT_HEADERS64*)(header.imageBase + dos->e_lfanew);
        if (nt->Signature == IMAGE_NT_SIGNATURE)
            header.imageSize = nt->OptionalHeader.SizeOfImage;
    }
    header.timestampUnix = (uint64_t)std::time(nullptr);
#ifdef UMVC3HOOK_GIT_COMMIT
    memcpy(header.gitCommit, UMVC3HOOK_GIT_COMMIT,
           std::min(sizeof(header.gitCommit) - 1, strlen(UMVC3HOOK_GIT_COMMIT)));
#else
    strcpy(header.gitCommit, "unknown");
#endif
#ifdef UMVC3HOOK_GIT_DIRTY
    header.gitDirty = UMVC3HOOK_GIT_DIRTY ? 1 : 0;
#endif

    bool success = WriteAll(file, &header, sizeof(header));
    const HistoryFrame* previous = nullptr;
    std::vector<RegionHeader> regions;
    for (const HistoryFrame* frame : frames) {
        const CapturedRegion* captured = reinterpret_cast<const CapturedRegion*>(
            frame->storage + frame->regionOffset);
        regions.resize(frame->regionCount);
        size_t regionDataOffset = 0;
        for (uint32_t i = 0; i < frame->regionCount; ++i) {
            const CapturedRegion& source = captured[i];
            RegionHeader& region = regions[i];
            region = {};
            region.address = source.address;
            region.size = source.size;
            region.ownerSlot = source.ownerSlot;
            region.expectedVtable = source.expectedVtable;
            region.hash = regionDataOffset + source.size <= frame->byteSize
                ? Rollback_Fnv1a64(frame->storage + regionDataOffset, source.size) : 0;
            region.skipVtable = source.skipVtable ? 1 : 0;
            if (source.name)
                strncpy(region.name, source.name, sizeof(region.name) - 1);
            regionDataOffset += source.size;
        }

        std::vector<uint8_t> encoded = EncodeXorRle(
            frame->storage, frame->byteSize,
            previous ? previous->storage : nullptr,
            previous ? previous->byteSize : 0);
        const bool useEncoded = !encoded.empty() && encoded.size() < frame->byteSize;
        FrameHeader frameHeader = {};
        frameHeader.frame = frame->frame;
        frameHeader.localInput = frame->localInput;
        frameHeader.remoteInput = frame->remoteInput;
        frameHeader.regionCount = frame->regionCount;
        frameHeader.shotCount = frame->shotCount;
        frameHeader.effectCount = frame->effectCount;
        frameHeader.collisionCloneCount = frame->collisionCloneCount;
        frameHeader.modelRenderStateCount = frame->modelRenderStateCount;
        frameHeader.snapshotHash = Rollback_Fnv1a64(frame->storage, frame->byteSize);
        frameHeader.manifestHash = HashManifest(regions.data(), regions.size());
        frameHeader.uncompressedSize = frame->byteSize;
        frameHeader.encodedSize = useEncoded ? encoded.size() : frame->byteSize;
        frameHeader.encoding = useEncoded ? ENCODING_XOR_RLE : ENCODING_RAW;
        success = success && WriteAll(file, &frameHeader, sizeof(frameHeader));
        success = success && WriteAll(file, regions.data(),
                                      regions.size() * sizeof(RegionHeader));
        success = success && WriteAll(file, useEncoded ? encoded.data() : frame->storage,
                                      (size_t)frameHeader.encodedSize);
        previous = frame;
        if (!success) break;
    }
    success = success && fflush(file) == 0;
    fclose(file);
    if (!success) {
        DeleteFileA(path);
        HookLog("SnapDump: failed writing %s\n", path);
        return false;
    }
    g_dumpWritten = true;
    HookLog("SnapDump: wrote %u snapshots through frame %d -> %s\n",
            (uint32_t)frames.size(), triggerFrame, path);
    return true;
}

void SnapDump_Reset()
{
    for (HistoryFrame& frame : g_history) {
        delete[] frame.storage;
        frame = {};
    }
    g_dumpWritten = false;
}

#endif
