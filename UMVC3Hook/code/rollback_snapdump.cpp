//Snapshut dumping for inspection with ai or visually in the ndjson-viewer-2.html
#include "../pch.h"
#include "../IniSafe.h"

#include "rollback_snapdump.h"
#include "autotest.h"

extern void HookLog(const char* fmt, ...);

namespace {

int32_t g_pendingDesyncTriggerFrame = -1;
int32_t g_pendingDesyncCaptureFrame = -1;

} // namespace

#if ROLLBACK_SNAPDUMP_ENABLED

#include "mk10utils.h"
#include "netplay.h"
#include "rollback_diagnostics.h"
#include "rollback_snapshot.h"
#include "rollback_unit.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <new>
#include <vector>

namespace {

constexpr uint32_t DUMP_MAGIC = 0x50445355; // USDP
constexpr uint32_t DUMP_VERSION = 5;
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

#pragma pack(pop)

static_assert(DUMP_VERSION == 5);
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
    std::shared_ptr<RollbackUnitSnapshot> lifetime;
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

uint8_t* SnapDump_AcquireSnapshotStorage(int32_t frame, size_t byteSize,
    int regionCount, size_t* capacity, std::shared_ptr<RollbackUnitSnapshot>* lifetime)
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
        SnapshotReuse_ForgetBuffer(destination.storage);
        delete[] destination.storage;
        destination.storage = storage;
        destination.capacity = newCapacity;
    }

    if (capacity) *capacity = destination.capacity;
    if (lifetime) *lifetime = destination.lifetime;
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

    if (destination.storage != slot.data) {
        SnapshotReuse_ForgetBuffer(destination.storage); // Not written by the save.
        std::memcpy(destination.storage, slot.data, slot.size);
    }
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
    // Share the same lifetime capture used for rollback, without a second copy
    // or extending any native-object/graph leases beyond the rollback window.
    destination.lifetime = slot.lifetime;
    destination.shotCount = 0;
    destination.effectCount = 0;
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

bool SnapDump_CaptureEnabled()
{
    static const bool enabled = []() {
        char iniPath[MAX_PATH] = {};
        if (Rollback_ResolveDllPath(
                iniPath, sizeof(iniPath), "Timestone.ini") < 0) {
            return false;
        }

        char value[16] = {};
        GetPrivateProfileStringA(
            "Rollback", "EnableSnapDumpWrites", "", value, sizeof(value),
            iniPath);
        if (!value[0]) {
            if (!IniSafe_WriteString(
                    "Rollback", "EnableSnapDumpWrites", "0", iniPath)) {
                HookLog("SnapDump: failed to add EnableSnapDumpWrites=0 to %s\n",
                        iniPath);
            }
            return false;
        }
        return std::strtol(value, nullptr, 10) != 0;
    }();
    return enabled;
}

bool SnapDump_WritesEnabled()
{
    return SnapDump_CaptureEnabled();
}

bool SnapDump_WriteHistory(int32_t triggerFrame, const char* relativePath)
{
    if (!SnapDump_WritesEnabled()) {
        HookLog("SnapDump: write skipped; set [Rollback] EnableSnapDumpWrites=1 in Timestone.ini to enable\n");
        return true;
    }
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
    char logRelative[512];
    const bool isAutoTestPath = strncmp(relativePath, "autotest/", 9) == 0 ||
                                strncmp(relativePath, "autotest\\", 9) == 0;
    int relativeLength = snprintf(logRelative, sizeof(logRelative), "%s%s",
                                  isAutoTestPath ? "" : "rollback_logs/",
                                  relativePath);
    if (relativeLength < 0 || (size_t)relativeLength >= sizeof(logRelative)) {
        HookLog("SnapDump: output path is too long: %s\n", relativePath);
        return false;
    }
    char path[512];
    if (Rollback_ResolveDllPath(path, sizeof(path), logRelative) < 0) {
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
    std::vector<uint8_t> previous, data;
    std::vector<RegionHeader> regions;
    for (const HistoryFrame* frame : frames) {
        const CapturedRegion* captured = reinterpret_cast<const CapturedRegion*>(
            frame->storage + frame->regionOffset);
        // Assemble diagnostics only on write. Never move shared history backing
        // or extend leases just to keep diagnostic history alive.
        data.assign(frame->storage, frame->storage + frame->byteSize);
        regions.resize(frame->regionCount);
        size_t regionDataOffset = 0;
        for (size_t i = 0; i < regions.size(); ++i) {
            const CapturedRegion& source = captured[i];
            RegionHeader& region = regions[i];
            region = {};
            region.address = source.address;
            region.size = source.size;
            region.ownerSlot = source.ownerSlot;
            region.expectedVtable = source.expectedVtable;
            region.hash = regionDataOffset + source.size <= data.size()
                ? Rollback_Fnv1a64(data.data() + regionDataOffset, source.size) : 0;
            region.skipVtable = source.skipVtable ? 1 : 0;
            if (source.name)
                strncpy(region.name, source.name, sizeof(region.name) - 1);
            regionDataOffset += source.size;
        }

        struct Output {
            std::vector<uint8_t>& data;
            std::vector<RegionHeader>& regions;
        } output{data, regions};
        const auto counts = RollbackUnit_VisitSnapshot(frame->frame, frame->lifetime.get(), frame == frames.back(),
            [](void* context, const char* name, const void* address, uint64_t ownerSlot,
               uint64_t vtable, const void* bytes, size_t size) {
                auto& out = *static_cast<Output*>(context);
                RegionHeader region{};
                region.address = uint64_t(address);
                region.size = size;
                region.ownerSlot = ownerSlot;
                region.expectedVtable = vtable;
                region.hash = Rollback_Fnv1a64(static_cast<const uint8_t*>(bytes), size);
                std::strncpy(region.name, name, sizeof(region.name) - 1);
                out.regions.push_back(region);
                const auto* data = static_cast<const uint8_t*>(bytes);
                out.data.insert(out.data.end(), data, data + size);
            }, &output);

        std::vector<uint8_t> encoded = EncodeXorRle(
            data.data(), data.size(), previous.empty() ? nullptr : previous.data(), previous.size());
        const bool useEncoded = !encoded.empty() && encoded.size() < data.size();
        FrameHeader frameHeader = {};
        frameHeader.frame = frame->frame;
        frameHeader.localInput = frame->localInput;
        frameHeader.remoteInput = frame->remoteInput;
        frameHeader.regionCount = (uint32_t)regions.size();
        frameHeader.shotCount = counts.shots;
        frameHeader.effectCount = counts.effects;
        frameHeader.collisionCloneCount = frame->collisionCloneCount;
        frameHeader.modelRenderStateCount = frame->modelRenderStateCount;
        frameHeader.snapshotHash = Rollback_Fnv1a64(data.data(), data.size());
        frameHeader.manifestHash = HashManifest(regions.data(), regions.size());
        frameHeader.uncompressedSize = data.size();
        frameHeader.encodedSize = useEncoded ? encoded.size() : data.size();
        frameHeader.encoding = useEncoded ? ENCODING_XOR_RLE : ENCODING_RAW;
        success = success && WriteAll(file, &frameHeader, sizeof(frameHeader));
        success = success && WriteAll(file, regions.data(),
                                      regions.size() * sizeof(RegionHeader));
        success = success && WriteAll(file, useEncoded ? encoded.data() : data.data(),
                                      (size_t)frameHeader.encodedSize);
        previous.swap(data);
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
    if (Netplay_IsActive()) {
        char outputDirectory[MAX_PATH] = {};
        Rollback_GetNetplayOutputDirectory(outputDirectory,
                                           sizeof(outputDirectory));
    }
    HookLog("SnapDump: wrote %u snapshots through frame %d -> %s\n",
            (uint32_t)frames.size(), triggerFrame, path);
    return true;
}

bool SnapDump_WriteManualHistory(int32_t frame, const char* timestamp)
{
    if (!timestamp || strlen(timestamp) != 15 || timestamp[8] != '_') {
        HookLog("SnapDump: rejected invalid manual timestamp\n");
        return false;
    }
    for (size_t i = 0; i < 15; ++i) {
        if (i == 8) continue;
        if (timestamp[i] < '0' || timestamp[i] > '9') {
            HookLog("SnapDump: rejected invalid manual timestamp %s\n", timestamp);
            return false;
        }
    }

    const char* role = Netplay_IsActive()
        ? (Netplay_IsHost() ? "host" : "client") : "offline";
    char relativePath[128] = {};
    snprintf(relativePath, sizeof(relativePath), "desync_%s_%s_f%d.usdp",
             timestamp, role, frame);
    g_dumpWritten = false;
    return SnapDump_WriteHistory(frame, relativePath);
}

void SnapDump_Reset()
{
    for (HistoryFrame& frame : g_history) {
        SnapshotReuse_ForgetBuffer(frame.storage);
        delete[] frame.storage;
        frame = {};
    }
    g_dumpWritten = false;
}

#endif

extern "C" void Rollback_NotifyDesyncTriggered(int32_t frame)
{
    if (g_pendingDesyncTriggerFrame >= 0) return;
    g_pendingDesyncTriggerFrame = frame;
    g_pendingDesyncCaptureFrame = frame + 15;
    HookLog("Desync: delaying pause and snapdump through frame %d\n",
            g_pendingDesyncCaptureFrame);
}

extern "C" void Rollback_ClearPendingDesyncCapture()
{
    g_pendingDesyncTriggerFrame = -1;
    g_pendingDesyncCaptureFrame = -1;
}

extern "C" bool Rollback_ProcessDesyncDelay(int32_t currentFrame)
{
    if (g_pendingDesyncTriggerFrame < 0) return false;
    const int32_t captureFrame = g_pendingDesyncCaptureFrame;
    if (currentFrame < captureFrame) return true;

#if ROLLBACK_SNAPDUMP_ENABLED
    char autoTestPath[MAX_PATH] = {};
    if (!SnapDump_WriteHistory(captureFrame,
            AutoTest_GetSnapDumpRelativePath(autoTestPath, sizeof(autoTestPath))
                ? autoTestPath : nullptr)) {
        HookLog("Desync: snapdump failed; retaining capture through frame %d for retry\n",
                captureFrame);
        return true;
    }
#endif

    g_pendingDesyncTriggerFrame = -1;
    g_pendingDesyncCaptureFrame = -1;
    return false;
}
