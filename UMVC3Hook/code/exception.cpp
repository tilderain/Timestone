#include "../pch.h"
#include "exception.h"
#include "netplay.h"
#include "rollback.h"
#include "rollback_effect.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <tlhelp32.h>
#include <winver.h>

extern void HookLog(const char* fmt, ...);

#ifndef UMVC3HOOK_GIT_COMMIT
#define UMVC3HOOK_GIT_COMMIT "unknown"
#endif

#ifndef UMVC3HOOK_GIT_DIRTY
#define UMVC3HOOK_GIT_DIRTY 0
#endif

namespace {

PVOID g_exceptionHandler = nullptr;
volatile LONG g_crashInFlight = 0;

constexpr uintptr_t kPreferredImageBase = 0x140000000;
constexpr uintptr_t kSCharacterPointer = 0x140D44A70;
constexpr uintptr_t kTeam0Offset = 0x40;
constexpr uintptr_t kTeam1Offset = 0x300;
// cChrTeam__getNextNode starts at the embedded sentinel (+0x08) and reads
// its intrusive link at +0x10, making the live head pointer team+0x18.
constexpr uintptr_t kTeamFirstNodeOffset = 0x18;
constexpr uintptr_t kNodeCharacterOffset = 0x08;
constexpr uintptr_t kNodeNextOffset = 0x10;
// This is only a corruption/cycle guard, not a roster-size assumption. Normal
// traversal continues until the team list terminates.
constexpr size_t kMaxCharacterNodesPerTeam = 64;
constexpr size_t kMaxLoadedCharacters = kMaxCharacterNodesPerTeam * 2;

struct CrashCharacter {
    uintptr_t object = 0;
    int team = -1;
    int slot = -1;
    char dtiName[96] = {};
};

bool IsCatastrophicException(DWORD code)
{
    switch (code) {
    case EXCEPTION_ACCESS_VIOLATION:
    case EXCEPTION_IN_PAGE_ERROR:
    case EXCEPTION_ILLEGAL_INSTRUCTION:
    case EXCEPTION_PRIV_INSTRUCTION:
    case EXCEPTION_STACK_OVERFLOW:
    case EXCEPTION_INT_DIVIDE_BY_ZERO:
    case EXCEPTION_INT_OVERFLOW:
    case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:
    case STATUS_HEAP_CORRUPTION:
        return true;
    default:
        return false;
    }
}

bool IsAutoTestProcess()
{
    const char* commandLine = GetCommandLineA();
    return commandLine &&
        (std::strstr(commandLine, "-autotest=") ||
         std::strstr(commandLine, "-autotest-client"));
}

const char* BaseName(const char* path)
{
    const char* slash = std::strrchr(path, '\\');
    return slash ? slash + 1 : path;
}

void ResolveAddress(FILE* file, const char* tag, uintptr_t address)
{
    HMODULE module = nullptr;
    if (GetModuleHandleExA(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCSTR>(address), &module) && module) {
        char path[MAX_PATH] = {};
        GetModuleFileNameA(module, path, MAX_PATH);
        std::fprintf(file, "%s0x%016llX  %s+0x%llX\n", tag,
            static_cast<unsigned long long>(address), BaseName(path),
            static_cast<unsigned long long>(address - reinterpret_cast<uintptr_t>(module)));
        return;
    }

    std::fprintf(file, "%s0x%016llX  <no module>\n", tag,
        static_cast<unsigned long long>(address));
}

bool IsReadableRange(uintptr_t address, size_t size)
{
    MEMORY_BASIC_INFORMATION memory{};
    if (!address || VirtualQuery(reinterpret_cast<LPCVOID>(address), &memory, sizeof(memory)) != sizeof(memory))
        return false;
    if (memory.State != MEM_COMMIT || (memory.Protect & (PAGE_GUARD | PAGE_NOACCESS)))
        return false;

    const DWORD readable = PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY |
        PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    if (!(memory.Protect & readable))
        return false;

    const uintptr_t regionEnd = reinterpret_cast<uintptr_t>(memory.BaseAddress) + memory.RegionSize;
    return size <= regionEnd - address;
}

bool IsExecutableAddress(uintptr_t address)
{
    MEMORY_BASIC_INFORMATION memory{};
    if (VirtualQuery(reinterpret_cast<LPCVOID>(address), &memory, sizeof(memory)) != sizeof(memory))
        return false;
    if (memory.State != MEM_COMMIT || (memory.Protect & (PAGE_GUARD | PAGE_NOACCESS)))
        return false;

    const DWORD executable = PAGE_EXECUTE | PAGE_EXECUTE_READ |
        PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    return (memory.Protect & executable) != 0;
}

bool IsMainModuleAddress(uintptr_t address)
{
    HMODULE module = nullptr;
    HMODULE mainModule = GetModuleHandleA(nullptr);
    if (!mainModule || !GetModuleHandleExA(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCSTR>(address), &module)) {
        return false;
    }
    return module == mainModule;
}

bool SafeReadPointer(uintptr_t address, uintptr_t* value)
{
    if (!value || !IsReadableRange(address, sizeof(*value)))
        return false;
    *value = *reinterpret_cast<const uintptr_t*>(address);
    return true;
}

bool SafeCopyString(uintptr_t address, char* output, size_t outputSize)
{
    if (!output || outputSize == 0 || !address)
        return false;

    for (size_t i = 0; i + 1 < outputSize; ++i) {
        if (address > UINTPTR_MAX - i || !IsReadableRange(address + i, 1)) {
            output[0] = '\0';
            return false;
        }
        const unsigned char character =
            *reinterpret_cast<const unsigned char*>(address + i);
        if (character == '\0') {
            output[i] = '\0';
            return i != 0;
        }
        output[i] = character >= 0x20 && character <= 0x7E
            ? static_cast<char>(character) : '?';
    }
    output[outputSize - 1] = '\0';
    return false;
}

bool TryGetObjectDtiName(uintptr_t object, char* output, size_t outputSize)
{
    if (!output || outputSize == 0 || !IsReadableRange(object, sizeof(uintptr_t)))
        return false;

    uintptr_t vtable = 0;
    if (!SafeReadPointer(object, &vtable) ||
        !IsReadableRange(vtable, 0x28)) {
        return false;
    }

    uintptr_t getDti = 0;
    if (vtable > UINTPTR_MAX - 0x20 ||
        !SafeReadPointer(vtable + 0x20, &getDti) ||
        !IsExecutableAddress(getDti) || !IsMainModuleAddress(getDti)) {
        return false;
    }

    // MtObject's DTI virtual is the least invasive way to identify a live
    // object. Only dispatch after validating both the vtable and its target.
    using GetDtiFunction = uintptr_t(__fastcall*)(uintptr_t);
    const uintptr_t dti = reinterpret_cast<GetDtiFunction>(getDti)(object);
    uintptr_t className = 0;
    if (dti > UINTPTR_MAX - 0x08 ||
        !SafeReadPointer(dti + 0x08, &className))
        return false;
    return SafeCopyString(className, output, outputSize);
}

int FindLoadedCharacter(const CrashCharacter* characters, size_t count,
                        uintptr_t candidate, uintptr_t* interiorOffset)
{
    for (size_t i = 0; i < count; ++i) {
        if (candidate == characters[i].object) {
            if (interiorOffset) *interiorOffset = 0;
            return static_cast<int>(i);
        }

        // Register values can point at a field inside cChr instead of its
        // base. The nominal object extent is enough for crash-time labeling.
        if (candidate >= characters[i].object &&
            candidate - characters[i].object < 0x6A60) {
            if (interiorOffset)
                *interiorOffset = candidate - characters[i].object;
            return static_cast<int>(i);
        }
    }
    return -1;
}

size_t CollectLoadedCharacters(CrashCharacter* characters, size_t capacity)
{
    if (!characters || capacity == 0)
        return 0;

    const uintptr_t module = reinterpret_cast<uintptr_t>(GetModuleHandleA(nullptr));
    if (!module || module > UINTPTR_MAX -
        (kSCharacterPointer - kPreferredImageBase))
        return 0;

    uintptr_t sCharacter = 0;
    if (!SafeReadPointer(module + (kSCharacterPointer - kPreferredImageBase),
                         &sCharacter) || !sCharacter) {
        return 0;
    }

    const uintptr_t teamOffsets[] = {kTeam0Offset, kTeam1Offset};
    size_t count = 0;
    for (int team = 0; team < 2 && count < capacity; ++team) {
        uintptr_t node = 0;
        if (sCharacter > UINTPTR_MAX - teamOffsets[team] - kTeamFirstNodeOffset ||
            !SafeReadPointer(sCharacter + teamOffsets[team] + kTeamFirstNodeOffset,
                             &node)) {
            continue;
        }

        uintptr_t visited[kMaxCharacterNodesPerTeam] = {};
        size_t visitedCount = 0;
        for (size_t slot = 0; node && slot < kMaxCharacterNodesPerTeam &&
             count < capacity; ++slot) {
            bool repeated = false;
            for (size_t i = 0; i < visitedCount; ++i) {
                if (visited[i] == node) {
                    repeated = true;
                    break;
                }
            }
            if (repeated || visitedCount >= kMaxCharacterNodesPerTeam)
                break;
            visited[visitedCount++] = node;

            uintptr_t character = 0;
            uintptr_t next = 0;
            if (node > UINTPTR_MAX - kNodeCharacterOffset ||
                !SafeReadPointer(node + kNodeCharacterOffset, &character)) {
                break;
            }
            if (node <= UINTPTR_MAX - kNodeNextOffset)
                SafeReadPointer(node + kNodeNextOffset, &next);

            if (character) {
                CrashCharacter& entry = characters[count++];
                entry.object = character;
                entry.team = team;
                entry.slot = static_cast<int>(slot);
                TryGetObjectDtiName(character, entry.dtiName,
                                    sizeof(entry.dtiName));
            }
            node = next;
        }
    }
    return count;
}

void WriteLoadedCharacters(FILE* file, const CrashCharacter* characters, size_t count)
{
    std::fprintf(file, "loaded characters:\n");
    if (count == 0) {
        std::fprintf(file, "  <unavailable>\n");
        return;
    }

    for (size_t i = 0; i < count; ++i) {
        const CrashCharacter& entry = characters[i];
        std::fprintf(file,
            "  team=%d slot=%d object=0x%016llX",
            entry.team, entry.slot,
            static_cast<unsigned long long>(entry.object));
        if (entry.dtiName[0])
            std::fprintf(file, " dti=\"%s\"", entry.dtiName);
        else
            std::fprintf(file, " dti=<unavailable>");
        std::fprintf(file, "\n");
    }
}

void WriteCrashObjectCandidates(FILE* file, const CONTEXT* context,
                                const CrashCharacter* characters,
                                size_t characterCount)
{
#if defined(_M_X64) || defined(__x86_64__)
    struct RegisterValue {
        const char* name;
        uintptr_t value;
    };
    const RegisterValue values[] = {
        {"RAX", context->Rax}, {"RBX", context->Rbx},
        {"RCX", context->Rcx}, {"RDX", context->Rdx},
        {"RSI", context->Rsi}, {"RDI", context->Rdi},
        {"R8", context->R8}, {"R9", context->R9},
        {"R10", context->R10}, {"R11", context->R11},
        {"R12", context->R12}, {"R13", context->R13},
        {"R14", context->R14}, {"R15", context->R15},
    };

    std::fprintf(file, "crashing object candidates (best effort):\n");
    bool found = false;
    for (size_t i = 0; i < sizeof(values) / sizeof(values[0]); ++i) {
        if (values[i].value < 0x10000 ||
            !IsReadableRange(values[i].value, sizeof(uintptr_t))) {
            continue;
        }

        bool duplicate = false;
        for (size_t previous = 0; previous < i; ++previous)
            duplicate |= values[previous].value == values[i].value;
        if (duplicate)
            continue;

        char dtiName[96] = {};
        const bool hasDtiName = TryGetObjectDtiName(
            values[i].value, dtiName, sizeof(dtiName));
        uintptr_t interiorOffset = 0;
        const int characterIndex = FindLoadedCharacter(
            characters, characterCount, values[i].value, &interiorOffset);
        if (!hasDtiName && characterIndex < 0)
            continue;

        std::fprintf(file, "  %s=0x%016llX",
            values[i].name, static_cast<unsigned long long>(values[i].value));
        if (hasDtiName)
            std::fprintf(file, " dti=\"%s\"", dtiName);
        if (characterIndex >= 0) {
            const CrashCharacter& character = characters[characterIndex];
            std::fprintf(file, " character_dti=\"%s\" team=%d slot=%d",
                character.dtiName[0] ? character.dtiName : "<unavailable>",
                character.team, character.slot);
            if (interiorOffset)
                std::fprintf(file, " object_offset=0x%llX",
                    static_cast<unsigned long long>(interiorOffset));
        }
        std::fprintf(file, "\n");
        found = true;
    }
    if (!found)
        std::fprintf(file, "  <no named MT Framework object in registers>\n");
#else
    (void)context;
    (void)characters;
    (void)characterCount;
    std::fprintf(file, "crashing object candidates: <unsupported architecture>\n");
#endif
}

bool IsNetGalleryTeardownFault(const EXCEPTION_RECORD* exception, const CONTEXT* context)
{
#if defined(_M_X64) || defined(__x86_64__)
    if (!exception || !context || exception->ExceptionCode != EXCEPTION_ACCESS_VIOLATION ||
        exception->NumberParameters < 2 || exception->ExceptionInformation[0] != 0 ||
        exception->ExceptionInformation[1] != 0x30)
        return false;

    const uintptr_t gameBase = reinterpret_cast<uintptr_t>(GetModuleHandleA(nullptr));
    if (!gameBase || context->Rip != gameBase + 0x6D00 ||
        !IsReadableRange(context->Rsp, sizeof(uintptr_t)))
        return false;

    // uBattleUiPauseNetGallery's destructor can post after sGameUI is gone.
    // Match its leaf call into cSystem__enterCriticalSection, not other null locks.
    return *reinterpret_cast<const uintptr_t*>(context->Rsp) == gameBase + 0x2BDB35;
#else
    (void)exception;
    (void)context;
    return false;
#endif
}

bool ReadModuleImageInfo(HMODULE module, DWORD* timestamp, DWORD* imageSize)
{
    if (!module || !timestamp || !imageSize ||
        !IsReadableRange(reinterpret_cast<uintptr_t>(module), sizeof(IMAGE_DOS_HEADER)))
        return false;

    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(module);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew < 0)
        return false;

    const uintptr_t ntAddress = reinterpret_cast<uintptr_t>(module) +
        static_cast<uintptr_t>(dos->e_lfanew);
    if (!IsReadableRange(ntAddress, sizeof(IMAGE_NT_HEADERS)))
        return false;

    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(ntAddress);
    if (nt->Signature != IMAGE_NT_SIGNATURE)
        return false;

    *timestamp = nt->FileHeader.TimeDateStamp;
    *imageSize = nt->OptionalHeader.SizeOfImage;
    return true;
}

void WriteModuleVersion(FILE* file, const char* path)
{
    DWORD ignored = 0;
    const DWORD versionSize = GetFileVersionInfoSizeA(path, &ignored);
    if (!versionSize)
        return;

    void* versionData = std::malloc(versionSize);
    if (!versionData || !GetFileVersionInfoA(path, 0, versionSize, versionData)) {
        std::free(versionData);
        return;
    }

    VS_FIXEDFILEINFO* fixed = nullptr;
    UINT fixedSize = 0;
    if (VerQueryValueA(versionData, "\\", reinterpret_cast<void**>(&fixed), &fixedSize) &&
        fixed && fixedSize >= sizeof(VS_FIXEDFILEINFO)) {
        std::fprintf(file, " version=%u.%u.%u.%u",
            HIWORD(fixed->dwFileVersionMS), LOWORD(fixed->dwFileVersionMS),
            HIWORD(fixed->dwFileVersionLS), LOWORD(fixed->dwFileVersionLS));
    }
    std::free(versionData);
}

void WriteLoadedModules(FILE* file)
{
    std::fprintf(file, "loaded modules:\n");
    HANDLE snapshot = CreateToolhelp32Snapshot(
        TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, GetCurrentProcessId());
    if (snapshot == INVALID_HANDLE_VALUE) {
        std::fprintf(file, "  <unavailable error=%lu>\n",
            static_cast<unsigned long>(GetLastError()));
        return;
    }

    MODULEENTRY32 entry{};
    entry.dwSize = sizeof(entry);
    BOOL hasModule = Module32First(snapshot, &entry);
    while (hasModule) {
        DWORD timestamp = 0;
        DWORD imageSize = 0;
        const bool hasImageInfo = ReadModuleImageInfo(
            entry.hModule, &timestamp, &imageSize);

        std::fprintf(file, "  %s base=0x%016llX size=0x%lX timestamp=0x%08lX",
            entry.szExePath,
            static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(entry.modBaseAddr)),
            static_cast<unsigned long>(hasImageInfo ? imageSize : 0),
            static_cast<unsigned long>(hasImageInfo ? timestamp : 0));
        WriteModuleVersion(file, entry.szExePath);
        if (!hasImageInfo)
            std::fprintf(file, " image_info=<unavailable>");
        std::fprintf(file, "\n");

        hasModule = Module32Next(snapshot, &entry);
    }
    if (!hasModule && GetLastError() != ERROR_NO_MORE_FILES)
        std::fprintf(file, "  <enumeration stopped error=%lu>\n",
            static_cast<unsigned long>(GetLastError()));
    CloseHandle(snapshot);
}

void WriteUnwindBacktrace(FILE* file, const CONTEXT* exceptionContext)
{
#if defined(_M_X64) || defined(__x86_64__)
    CONTEXT context = *exceptionContext;
    std::fprintf(file, "unwind backtrace:\n");

    constexpr size_t maxFrames = 64;
    for (size_t frame = 0; frame < maxFrames && context.Rip; ++frame) {
        char tag[24] = {};
        std::snprintf(tag, sizeof(tag), "  #%02llu: ",
            static_cast<unsigned long long>(frame));
        ResolveAddress(file, tag, context.Rip);

        const DWORD64 previousRip = context.Rip;
        const DWORD64 previousRsp = context.Rsp;
        DWORD64 imageBase = 0;
        PRUNTIME_FUNCTION function = RtlLookupFunctionEntry(
            context.Rip, &imageBase, nullptr);

        if (function) {
            PVOID handlerData = nullptr;
            DWORD64 establisherFrame = 0;
            RtlVirtualUnwind(
                UNW_FLAG_NHANDLER,
                imageBase,
                context.Rip,
                function,
                &context,
                &handlerData,
                &establisherFrame,
                nullptr);
        } else {
            // Leaf functions have no unwind metadata. Their return address is
            // the qword at RSP, as long as the stack location remains readable.
            if (!IsReadableRange(context.Rsp, sizeof(uintptr_t)))
                break;
            context.Rip = *reinterpret_cast<const uintptr_t*>(context.Rsp);
            context.Rsp += sizeof(uintptr_t);
        }

        if ((context.Rip == previousRip && context.Rsp == previousRsp) ||
            context.Rsp <= previousRsp ||
            (context.Rip != 0 && !IsExecutableAddress(context.Rip))) {
            break;
        }
    }
#else
    (void)exceptionContext;
    std::fprintf(file, "unwind backtrace: <unsupported architecture>\n");
#endif
}

LONG WINAPI ExceptionHandler(PEXCEPTION_POINTERS info)
{
    if (!info || !info->ExceptionRecord || !info->ContextRecord)
        return EXCEPTION_CONTINUE_SEARCH;

    const DWORD code = info->ExceptionRecord->ExceptionCode;
    if (!IsCatastrophicException(code))
        return EXCEPTION_CONTINUE_SEARCH;
    if (IsNetGalleryTeardownFault(info->ExceptionRecord, info->ContextRecord))
        return EXCEPTION_CONTINUE_SEARCH;
    if (InterlockedExchange(&g_crashInFlight, 1) != 0)
        return EXCEPTION_CONTINUE_SEARCH;

    const EXCEPTION_RECORD* exception = info->ExceptionRecord;
    const CONTEXT* context = info->ContextRecord;
    char crashLogPath[MAX_PATH] = "umvc3_crash.log";
    char netplayDirectory[MAX_PATH] = {};
    if (Netplay_IsActive() &&
        Rollback_GetNetplayLiveDirectory(netplayDirectory, sizeof(netplayDirectory))) {
        std::snprintf(crashLogPath, sizeof(crashLogPath),
            "%s/umvc3_crash.log", netplayDirectory);
    } else {
        GetFullPathNameA("umvc3_crash.log", MAX_PATH, crashLogPath, nullptr);
    }

    const bool ndjsonFlushed = Rollback_FlushNDJSONOnCrash();

    FILE* file = std::fopen(crashLogPath, "a");
    if (file) {
        SYSTEMTIME time{};
        GetLocalTime(&time);
        std::fprintf(file,
            "\n=== UMVC3Hook crash %04u-%02u-%02u %02u:%02u:%02u.%03u ===\n"
            "commit=%s%s\n"
            "code=0x%08lX exception=%p thread=%lu\n",
            time.wYear, time.wMonth, time.wDay, time.wHour, time.wMinute,
            time.wSecond, time.wMilliseconds, UMVC3HOOK_GIT_COMMIT,
            UMVC3HOOK_GIT_DIRTY ? " (dirty)" : "",
            static_cast<unsigned long>(code),
            exception->ExceptionAddress, static_cast<unsigned long>(GetCurrentThreadId()));
        std::fprintf(file, "ndjson_flush=%s\n", ndjsonFlushed ? "completed" : "unavailable");

        std::fprintf(file,
            "RIP=0x%016llX RSP=0x%016llX RBP=0x%016llX\n"
            "RAX=0x%016llX RBX=0x%016llX RCX=0x%016llX RDX=0x%016llX\n"
            "RSI=0x%016llX RDI=0x%016llX R8 =0x%016llX R9 =0x%016llX\n"
            "R10=0x%016llX R11=0x%016llX R12=0x%016llX R13=0x%016llX\n"
            "R14=0x%016llX R15=0x%016llX\n",
            context->Rip, context->Rsp, context->Rbp,
            context->Rax, context->Rbx, context->Rcx, context->Rdx,
            context->Rsi, context->Rdi, context->R8, context->R9,
            context->R10, context->R11, context->R12, context->R13,
            context->R14, context->R15);

        if ((code == EXCEPTION_ACCESS_VIOLATION || code == EXCEPTION_IN_PAGE_ERROR) &&
            exception->NumberParameters >= 2) {
            const ULONG_PTR operation = exception->ExceptionInformation[0];
            const char* operationName = operation == 0 ? "read" : operation == 1 ? "write" : "execute";
            std::fprintf(file, "access=%s address=0x%016llX\n", operationName,
                static_cast<unsigned long long>(exception->ExceptionInformation[1]));
        }

        ResolveAddress(file, "fault: ", context->Rip);
        CrashCharacter loadedCharacters[kMaxLoadedCharacters] = {};
        const size_t loadedCharacterCount = CollectLoadedCharacters(
            loadedCharacters, kMaxLoadedCharacters);
        WriteLoadedCharacters(file, loadedCharacters, loadedCharacterCount);
        WriteCrashObjectCandidates(file, context, loadedCharacters,
                                    loadedCharacterCount);
        if constexpr (EFFECT_LIFECYCLE_TRACE_ENABLED) {
            const uint64_t effectCandidates[] = {
                context->Rdi, context->Rcx, context->Rdx,
                context->Rbx, context->R14,
            };
            constexpr size_t effectCandidateCount =
                sizeof(effectCandidates) / sizeof(effectCandidates[0]);
            for (size_t i = 0; i < effectCandidateCount; ++i) {
                bool duplicate = false;
                for (size_t previous = 0; previous < i; ++previous) {
                    duplicate |= effectCandidates[i] == effectCandidates[previous];
                }
                if (!duplicate)
                    Effect_DumpCrashTrace(file, effectCandidates[i]);
            }
        }
        WriteUnwindBacktrace(file, context);
        std::fprintf(file, "stack scan (executable addresses):\n");
        constexpr size_t stackEntries = 96;
        if (IsReadableRange(context->Rsp, stackEntries * sizeof(uintptr_t))) {
            const auto* stack = reinterpret_cast<const uintptr_t*>(context->Rsp);
            for (size_t i = 0; i < stackEntries; ++i) {
                const uintptr_t address = stack[i];
                if (address < 0x10000 || !IsExecutableAddress(address))
                    continue;

                char tag[24] = {};
                std::snprintf(tag, sizeof(tag), "  RSP+0x%03llX: ",
                    static_cast<unsigned long long>(i * sizeof(uintptr_t)));
                ResolveAddress(file, tag, address);
            }
        } else {
            std::fprintf(file, "  <stack unreadable>\n");
        }
        WriteLoadedModules(file);
        std::fprintf(file, "=== end crash ===\n");
        std::fflush(file);
        std::fclose(file);
    }

    if (!IsAutoTestProcess()) {
        char message[MAX_PATH + 96] = {};
        std::snprintf(message, sizeof(message),
            "The game just freaking crashed! Crash log available at %s", crashLogPath);
        MessageBoxA(nullptr, message, "UMVC3Hook Crash", MB_OK | MB_ICONERROR | MB_SYSTEMMODAL);
    } else {
        HookLog("UMVC3Hook: autotest crash recorded at %s; message box suppressed\n",
            crashLogPath);
    }

    return EXCEPTION_CONTINUE_SEARCH;
}

} // namespace

void Exception_InstallHandler()
{
    if (g_exceptionHandler)
        return;

    g_crashInFlight = 0;
    g_exceptionHandler = AddVectoredExceptionHandler(1, ExceptionHandler);
    HookLog("UMVC3Hook: crash exception handler %s\n",
        g_exceptionHandler ? "installed" : "FAILED to install");
}

void Exception_RemoveHandler()
{
    if (!g_exceptionHandler)
        return;

    RemoveVectoredExceptionHandler(g_exceptionHandler);
    g_exceptionHandler = nullptr;
}

void Exception_UpdateTestHotkey()
{
    static bool wasDown = false;
    const bool isDown = (GetAsyncKeyState(VK_F4) & 0x8000) != 0;
    if (isDown && !wasDown) {
        const ULONG_PTR details[] = {
            1, // Write access.
            0  // Address that would have been written.
        };
        RaiseException(EXCEPTION_ACCESS_VIOLATION, EXCEPTION_NONCONTINUABLE,
            static_cast<DWORD>(sizeof(details) / sizeof(details[0])), details);
    }
    wasDown = isDown;
}
