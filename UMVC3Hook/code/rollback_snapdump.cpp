#include "../pch.h"

#include "rollback_snapdump.h"

#if ROLLBACK_SNAPDUMP_ENABLED

#include "mk10utils.h"
#include "netplay.h"
#include "rollback_diagnostics.h"
#include "rollback_snapshot.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <new>
#include <vector>

extern void HookLog(const char* fmt, ...);

namespace {

constexpr uint32_t DUMP_MAGIC = 0x50445355; // USDP
constexpr uint32_t DUMP_VERSION = 4;
constexpr int HISTORY_SIZE = 255;
constexpr uint8_t ENCODING_RAW = 0;
constexpr uint8_t ENCODING_XOR_RLE = 1;
static_assert(HISTORY_SIZE > ROLLBACK_WINDOW);

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

struct ShotHeader {
    uint32_t recordSize;
    uint32_t slotId;
    uint8_t kind;
    uint8_t hasPreSpawnRng;
    uint8_t hasZireitouState;
    uint8_t reserved0;
    uint64_t spawnSequence;
    uint64_t savedPtr;
    uint64_t dti;
    uint64_t vtable;
    uint64_t initParam;
    uint64_t posData;
    uint64_t ownerChr;
    uint64_t shotElement;
    uint64_t objectRef1;
    uint64_t objectRef1Dti;
    uint64_t objectRef1ShotSequence;
    uint8_t objectRef1Kind;
    uint8_t reserved1[7];
    uint64_t objectRef2;
    uint64_t objectRef2Dti;
    uint64_t objectRef2ShotSequence;
    uint8_t objectRef2Kind;
    uint8_t reserved2[7];
    int32_t exA9;
    uint64_t exA10;
    int32_t exA11;
    uint64_t exA12;
    uint32_t flightSize;
    uint32_t bodySize;
    uint32_t primitiveCount;
    uint32_t reserved3;
};

struct PrimitiveHeader {
    uint32_t slot;
    uint32_t index;
    uint32_t metadataType;
    uint8_t geometryType;
    uint8_t hasSourceGeometry;
    uint8_t reserved[2];
    uint32_t collisionKind;
    uint32_t flags;
    uint64_t parent0;
    uint64_t parent1;
    int32_t parent0ShotOffset;
    int32_t parent1ShotOffset;
};
#pragma pack(pop)

static_assert(DUMP_VERSION == 4);
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
    std::vector<RollbackShotRecipe> shots;
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

template <typename T>
void AppendValue(std::vector<uint8_t>& output, const T& value)
{
    const uint8_t* bytes = reinterpret_cast<const uint8_t*>(&value);
    output.insert(output.end(), bytes, bytes + sizeof(value));
}

void AppendRange(std::vector<uint8_t>& output, const void* data, size_t size)
{
    if (size == 0) return;
    const uint8_t* bytes = reinterpret_cast<const uint8_t*>(data);
    output.insert(output.end(), bytes, bytes + size);
}

std::vector<uint8_t> SerializeShot(const RollbackShotRecipe& recipe)
{
    ShotHeader header = {};
    header.slotId = recipe.slotId;
    header.kind = (uint8_t)recipe.kind;
    header.hasPreSpawnRng = recipe.hasPreSpawnRng ? 1 : 0;
    header.hasZireitouState = recipe.flight.hasZireitouState ? 1 : 0;
    header.spawnSequence = recipe.spawnSequence;
    header.savedPtr = (uint64_t)recipe.savedPtr;
    header.dti = recipe.concrete.dti;
    header.vtable = recipe.concrete.vtable;
    header.initParam = recipe.initParam;
    header.posData = (uint64_t)recipe.posData;
    header.ownerChr = (uint64_t)recipe.ownerChr;
    header.shotElement = (uint64_t)recipe.shotElement;
    header.objectRef1 = (uint64_t)recipe.objectRef1.savedPtr;
    header.objectRef1Dti = recipe.objectRef1.expectedDti;
    header.objectRef1ShotSequence = recipe.objectRef1.shotSpawnSequence;
    header.objectRef1Kind = (uint8_t)recipe.objectRef1.kind;
    header.objectRef2 = (uint64_t)recipe.objectRef2.savedPtr;
    header.objectRef2Dti = recipe.objectRef2.expectedDti;
    header.objectRef2ShotSequence = recipe.objectRef2.shotSpawnSequence;
    header.objectRef2Kind = (uint8_t)recipe.objectRef2.kind;
    header.exA9 = recipe.exA9;
    header.exA10 = (uint64_t)recipe.exA10;
    header.exA11 = recipe.exA11;
    header.exA12 = (uint64_t)recipe.exA12;
    header.bodySize = (uint32_t)recipe.concrete.body.size();
    header.primitiveCount =
        (uint32_t)recipe.concrete.collisionPrimitives.size();

    std::vector<uint8_t> output;
    output.reserve(sizeof(header) + header.bodySize +
                   header.primitiveCount * (sizeof(PrimitiveHeader) + 0x70) +
                   (header.hasPreSpawnRng ? 48 : 0));
    AppendValue(output, header);
    if (recipe.hasPreSpawnRng) {
        AppendRange(output, recipe.preSpawnGlobalRng,
                    sizeof(recipe.preSpawnGlobalRng));
        AppendRange(output, recipe.preSpawnEffectRng,
                    sizeof(recipe.preSpawnEffectRng));
        AppendRange(output, recipe.preSpawnOwnerRng,
                    sizeof(recipe.preSpawnOwnerRng));
    }
    AppendRange(output, recipe.concrete.body.data(), recipe.concrete.body.size());
    for (const auto& primitive : recipe.concrete.collisionPrimitives) {
        PrimitiveHeader primitiveHeader = {};
        primitiveHeader.slot = primitive.slot;
        primitiveHeader.index = primitive.index;
        primitiveHeader.metadataType = primitive.metadataType;
        primitiveHeader.geometryType = primitive.geometryType;
        primitiveHeader.hasSourceGeometry = primitive.hasSourceGeometry ? 1 : 0;
        primitiveHeader.collisionKind = primitive.collisionKind;
        primitiveHeader.flags = primitive.flags;
        primitiveHeader.parent0 = primitive.parent0;
        primitiveHeader.parent1 = primitive.parent1;
        primitiveHeader.parent0ShotOffset = primitive.parent0ShotOffset;
        primitiveHeader.parent1ShotOffset = primitive.parent1ShotOffset;
        AppendValue(output, primitiveHeader);
        AppendRange(output, primitive.geometry, sizeof(primitive.geometry));
        AppendRange(output, primitive.parentState, sizeof(primitive.parentState));
        AppendRange(output, primitive.localGeometry,
                    sizeof(primitive.localGeometry));
        AppendRange(output, primitive.sourceGeometry,
                    sizeof(primitive.sourceGeometry));
    }
    header.recordSize = (uint32_t)output.size();
    std::memcpy(output.data(), &header, sizeof(header));
    return output;
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

uint8_t* SnapDump_AcquireSnapshotStorage(int32_t frame, size_t byteSize,
                                         int regionCount, size_t* capacity)
{
    if (capacity) *capacity = 0;
    if (frame < 0 || byteSize == 0 || regionCount <= 0) return nullptr;

    HistoryFrame& destination = g_history[(uint32_t)frame % HISTORY_SIZE];
    const size_t regionOffset = (byteSize + alignof(CapturedRegion) - 1) &
                                ~(alignof(CapturedRegion) - 1);
    const size_t required = regionOffset +
                            (size_t)regionCount * sizeof(CapturedRegion);
    if (destination.capacity < required) {
        const size_t megabyte = 1024 * 1024;
        const size_t newCapacity = (required + megabyte - 1) & ~(megabyte - 1);
        uint8_t* storage = new (std::nothrow) uint8_t[newCapacity];
        if (!storage) {
            HookLog("SnapDump: failed to allocate %zu bytes for frame %d\n",
                    newCapacity, frame);
            return nullptr;
        }
        delete[] destination.storage;
        destination.storage = storage;
        destination.capacity = newCapacity;
    }

    if (capacity) *capacity = destination.capacity;
    return destination.storage;
}

void SnapDump_CaptureSnapshot(const RollbackSnapshotSlot& slot)
{
    if (slot.frame < 0 || !slot.data || slot.size == 0 || slot.regionCount <= 0) return;
    HistoryFrame& destination = g_history[(uint32_t)slot.frame % HISTORY_SIZE];
    const size_t regionOffset = (slot.size + alignof(CapturedRegion) - 1) &
                                ~(alignof(CapturedRegion) - 1);
    if (!SnapDump_AcquireSnapshotStorage(slot.frame, slot.size,
                                         slot.regionCount, nullptr)) {
        destination.frame = -1;
        return;
    }

    if (destination.storage != slot.data)
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
    destination.shots = slot.shotRecipes;
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
    if (frames.empty()) {
        HookLog("SnapDump: no snapshots available through frame %d\n", triggerFrame);
        return false;
    }

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
    if (!Rollback_ResolveDllPath(path, sizeof(path), relativePath)) {
        HookLog("SnapDump: failed to resolve output path %s\n", relativePath);
        return false;
    }
    if (!CreateParentDirectories(path)) {
        HookLog("SnapDump: failed to create output directories for %s error=%lu\n",
                path, GetLastError());
        return false;
    }
    FILE* file = fopen(path, "wb");
    if (!file) {
        HookLog("SnapDump: failed to open %s errno=%d\n", path, errno);
        return false;
    }

    HookLog("SnapDump: writing %u snapshots through frame %d -> %s\n",
            (uint32_t)frames.size(), triggerFrame, path);

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
        for (const RollbackShotRecipe& shot : frame->shots) {
            std::vector<uint8_t> serialized = SerializeShot(shot);
            success = success && WriteAll(file, serialized.data(), serialized.size());
        }
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
