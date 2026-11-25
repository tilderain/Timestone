// Crash log fit for mvc3's reflected classes.

#include "../pch.h"
#include "exception.h"
#include "framestepper.h"
#include "LaunchMenu.h"
#include "netplay.h"
#include "rollback.h"
#include "rollback_diagnostics.h"
#include "rollback_unit.h"
#include "umvc3.h"

#include <cstdint>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <shellapi.h>
#include <tlhelp32.h>
#include <winver.h>

extern void HookLog(const char* fmt, ...);
extern void HookLogAppendFile(const char* filename, long offset);
extern void HookLogWriteRecent(FILE* file);
extern void HookLogTryFlush();

#ifndef UMVC3HOOK_GIT_COMMIT
#define UMVC3HOOK_GIT_COMMIT "unknown"
#endif

#ifndef UMVC3HOOK_GIT_DIRTY
#define UMVC3HOOK_GIT_DIRTY 0
#endif

namespace {

PVOID g_exceptionHandler = nullptr;
LPTOP_LEVEL_EXCEPTION_FILTER g_previousUnhandledExceptionFilter = nullptr;
bool g_unhandledExceptionFilterInstalled = false;
using SignalHandler = void (*)(int);
SignalHandler g_previousAbortHandler = SIG_DFL;
bool g_abortHandlerInstalled = false;
volatile LONG g_crashInFlight = 0;

// Exception_RunGuarded: the slot holds the running thread's jump buffer while
// a guarded call is in progress. A plain TLS slot, not thread_local: the
// handler reads it on every first-chance exception of every thread, and
// thread_local here allocates on a thread's first use.
DWORD g_guardSlot = TLS_OUT_OF_INDEXES;

[[noreturn]] __attribute__((noinline)) void GuardLanding(void** buffer)
{
    __builtin_longjmp(buffer, 1);
}

// A fault inside a guarded call resumes the thread in GuardLanding, which
// jumps back out of the call. True when the exception was one of those.
bool ResumeGuardedCall(PEXCEPTION_POINTERS info)
{
    if (g_guardSlot == TLS_OUT_OF_INDEXES || !info || !info->ExceptionRecord ||
        !info->ContextRecord)
        return false;
    const DWORD code = info->ExceptionRecord->ExceptionCode;
    if (code != EXCEPTION_ACCESS_VIOLATION && code != EXCEPTION_IN_PAGE_ERROR &&
        code != EXCEPTION_DATATYPE_MISALIGNMENT && code != EXCEPTION_ILLEGAL_INSTRUCTION &&
        code != EXCEPTION_PRIV_INSTRUCTION)
        return false;
    void* buffer = TlsGetValue(g_guardSlot);
    if (!buffer) return false;
    TlsSetValue(g_guardSlot, nullptr);
    CONTEXT* context = info->ContextRecord;
    // As at a function's first instruction: below everything the faulting
    // code had on its stack, 8 off a 16-byte boundary, home space above.
    context->Rsp = (context->Rsp & ~static_cast<DWORD64>(15)) - 0x38;
    context->Rcx = reinterpret_cast<DWORD64>(buffer);
    context->Rip = reinterpret_cast<DWORD64>(&GuardLanding);
    return true;
}

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
    case EXCEPTION_BREAKPOINT:
    case EXCEPTION_ACCESS_VIOLATION:
    case EXCEPTION_IN_PAGE_ERROR:
    case EXCEPTION_ILLEGAL_INSTRUCTION:
    case EXCEPTION_PRIV_INSTRUCTION:
    case EXCEPTION_STACK_OVERFLOW:
    case EXCEPTION_INT_DIVIDE_BY_ZERO:
    case EXCEPTION_INT_OVERFLOW:
    case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:
    case STATUS_HEAP_CORRUPTION:
    case 0xC0000409: // STATUS_STACK_BUFFER_OVERRUN
    case 0xC0000417: // STATUS_INVALID_CRUNTIME_PARAMETER
    case 0xC0000420: // STATUS_ASSERTION_FAILURE
    case 0xC0000602: // STATUS_FAIL_FAST_EXCEPTION
    case 0x40000015: // STATUS_FATAL_APP_EXIT
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
         std::strstr(commandLine, "-autotest-client") ||
         std::strstr(commandLine, "-training-cpus"));
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

void WriteRawThreadState(FILE* file, const CONTEXT* context)
{
#if defined(_M_X64) || defined(__x86_64__)
    std::fprintf(file, "EFlags=0x%08lX\n",
                 static_cast<unsigned long>(context->EFlags));
    if ((context->ContextFlags & CONTEXT_FLOATING_POINT) == CONTEXT_FLOATING_POINT) {
        std::fprintf(file, "MXCSR=0x%08lX\n",
                     static_cast<unsigned long>(context->MxCsr));
        const M128A* xmm[] = {
            &context->Xmm0, &context->Xmm1, &context->Xmm2, &context->Xmm3,
            &context->Xmm4, &context->Xmm5, &context->Xmm6, &context->Xmm7,
            &context->Xmm8, &context->Xmm9, &context->Xmm10, &context->Xmm11,
            &context->Xmm12, &context->Xmm13, &context->Xmm14, &context->Xmm15,
        };
        std::fprintf(file, "XMM registers (128-bit hex, high half first):\n");
        for (size_t i = 0; i < sizeof(xmm) / sizeof(xmm[0]); ++i) {
            std::fprintf(file, "  XMM%02u=0x%016llX%016llX\n",
                         static_cast<unsigned>(i),
                         static_cast<unsigned long long>(xmm[i]->High),
                         static_cast<unsigned long long>(xmm[i]->Low));
        }
    } else {
        std::fprintf(file, "MXCSR/XMM: <not present in exception context>\n");
    }

    const void* teb = NtCurrentTeb();
    NT_TIB tib{};
    SIZE_T bytesRead = 0;
    std::fprintf(file, "thread stack: TEB=0x%016llX",
                 static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(teb)));
    if (ReadProcessMemory(GetCurrentProcess(), teb, &tib, sizeof(tib), &bytesRead) &&
        bytesRead == sizeof(tib)) {
        const uintptr_t base = reinterpret_cast<uintptr_t>(tib.StackBase);
        const uintptr_t limit = reinterpret_cast<uintptr_t>(tib.StackLimit);
        std::fprintf(file, " StackBase=0x%016llX StackLimit=0x%016llX",
                     static_cast<unsigned long long>(base),
                     static_cast<unsigned long long>(limit));
        if (limit < base) {
            std::fprintf(file, " RSP_in_stack=%d RBP_in_stack=%d\n",
                         context->Rsp >= limit && context->Rsp < base,
                         context->Rbp >= limit && context->Rbp < base);
        } else {
            std::fprintf(file, " <invalid stack bounds>\n");
        }
    } else {
        std::fprintf(file, " <stack bounds unreadable>\n");
    }

    // Keep invalid return targets and non-pointer data that the named scans omit.
    std::fprintf(file, "raw stack (128 unfiltered 8-byte words):\n");
    for (size_t i = 0; i < 128; ++i) {
        const uintptr_t offset = i * sizeof(uint64_t);
        std::fprintf(file, "  RSP+0x%03llX%s: ",
                     static_cast<unsigned long long>(offset), i == 0 ? " [RSP]" : "");
        if (context->Rsp > UINTPTR_MAX - offset) {
            std::fprintf(file, "<address overflow>\n");
            continue;
        }
        const uintptr_t address = context->Rsp + offset;
        uint64_t value = 0;
        bytesRead = 0;
        if (IsReadableRange(address, sizeof(value)) &&
            ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<LPCVOID>(address),
                              &value, sizeof(value), &bytesRead) &&
            bytesRead == sizeof(value)) {
            std::fprintf(file, "0x%016llX\n", static_cast<unsigned long long>(value));
        } else {
            std::fprintf(file, "<unreadable>\n");
        }
    }
#else
    (void)context;
    std::fprintf(file, "raw thread state: <unsupported architecture>\n");
#endif
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

// A stack pointer outside the thread's stack means the frames that led here are
// not at RSP. Every code address still on the real stack is listed instead, so
// the call chain can be read back. Entries below the frame that was running
// are leftovers of earlier, deeper calls.
void WriteRealStackWhenPivoted(FILE* file, const CONTEXT* context)
{
    NT_TIB tib{};
    SIZE_T bytesRead = 0;
    if (!ReadProcessMemory(GetCurrentProcess(), NtCurrentTeb(), &tib, sizeof(tib), &bytesRead) ||
        bytesRead != sizeof(tib))
        return;
    const uintptr_t base = reinterpret_cast<uintptr_t>(tib.StackBase);
    const uintptr_t limit = reinterpret_cast<uintptr_t>(tib.StackLimit);
    if (limit >= base || (context->Rsp >= limit && context->Rsp < base))
        return;

    std::fprintf(file, "RSP is outside the thread stack; registers that still point into it:");
    const struct { const char* name; DWORD64 value; } registers[] = {
        {"RBP", context->Rbp}, {"RBX", context->Rbx}, {"RSI", context->Rsi}, {"RDI", context->Rdi},
        {"R12", context->R12}, {"R13", context->R13}, {"R14", context->R14}, {"R15", context->R15},
    };
    bool any = false;
    for (const auto& entry : registers) {
        if (entry.value < limit || entry.value >= base) continue;
        std::fprintf(file, " %s=0x%016llX", entry.name, static_cast<unsigned long long>(entry.value));
        any = true;
    }
    std::fprintf(file, "%s\n", any ? "" : " none");

    std::fprintf(file, "real stack scan (code addresses, lowest address first):\n");
    constexpr size_t kMaxEntries = 512;
    size_t entries = 0;
    uint64_t page[512];
    for (uintptr_t address = limit & ~uintptr_t(0xFFF); address < base && entries < kMaxEntries;
         address += sizeof(page)) {
        bytesRead = 0;
        if (!IsReadableRange(address, sizeof(page)) ||
            !ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<LPCVOID>(address), page,
                               sizeof(page), &bytesRead) || bytesRead != sizeof(page))
            continue;
        for (size_t i = 0; i < 512 && entries < kMaxEntries; ++i) {
            const uintptr_t slot = address + i * sizeof(uint64_t);
            const uint64_t value = page[i];
            if (slot < limit || slot >= base || value < 0x10000 || value > 0x7FFFFFFFFFFFull ||
                !IsExecutableAddress(static_cast<uintptr_t>(value)))
                continue;
            char tag[40] = {};
            std::snprintf(tag, sizeof(tag), "  [0x%016llX]: ", static_cast<unsigned long long>(slot));
            ResolveAddress(file, tag, static_cast<uintptr_t>(value));
            ++entries;
        }
    }
    if (entries == kMaxEntries) std::fprintf(file, "  <more entries not shown>\n");
}

// True if a 64-bit base relocation starts at this RVA of the loaded image.
bool HasBaseRelocationAt(const uint8_t* image, const IMAGE_DATA_DIRECTORY& directory, uint32_t rva)
{
    for (uint32_t offset = 0; offset + sizeof(IMAGE_BASE_RELOCATION) <= directory.Size;) {
        const auto* block = reinterpret_cast<const IMAGE_BASE_RELOCATION*>(
            image + directory.VirtualAddress + offset);
        if (block->SizeOfBlock < sizeof(IMAGE_BASE_RELOCATION) ||
            block->SizeOfBlock > directory.Size - offset)
            return false;
        if (rva >= block->VirtualAddress && rva - block->VirtualAddress < 0x1000) {
            const auto* items = reinterpret_cast<const uint16_t*>(block + 1);
            const size_t count = (block->SizeOfBlock - sizeof(IMAGE_BASE_RELOCATION)) / sizeof(uint16_t);
            for (size_t i = 0; i < count; ++i)
                if ((items[i] >> 12) == IMAGE_REL_BASED_DIR64 &&
                    block->VirtualAddress + (items[i] & 0xFFFu) == rva)
                    return true;
        }
        offset += block->SizeOfBlock;
    }
    return false;
}

// Compares this plugin's code in memory with its file on disk. A register state
// that no instruction sequence in the file can produce is either code that was
// overwritten while running or a processor that executed it wrongly; this tells
// the two apart. Uses no CRT heap: the crash may have happened inside it.
void WriteOwnCodeIntegrity(FILE* file)
{
    std::fprintf(file, "plugin code check (memory against the file on disk):\n");
    HMODULE module = nullptr;
    char path[MAX_PATH] = {};
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCSTR>(&WriteOwnCodeIntegrity), &module) ||
        !module || !GetModuleFileNameA(module, path, MAX_PATH)) {
        std::fprintf(file, "  <own module not found>\n");
        return;
    }
    const auto* image = reinterpret_cast<const uint8_t*>(module);
    if (!IsReadableRange(reinterpret_cast<uintptr_t>(image), 0x1000)) {
        std::fprintf(file, "  <image headers unreadable>\n");
        return;
    }
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(image);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew < 0 ||
        size_t(dos->e_lfanew) + sizeof(IMAGE_NT_HEADERS64) > 0x1000) {
        std::fprintf(file, "  <image headers invalid>\n");
        return;
    }
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(image + dos->e_lfanew);
    const auto* sections = IMAGE_FIRST_SECTION(nt);
    const size_t sectionCount = nt->FileHeader.NumberOfSections;
    if (reinterpret_cast<const uint8_t*>(sections + sectionCount) > image + 0x1000) {
        std::fprintf(file, "  <section table invalid>\n");
        return;
    }
    IMAGE_DATA_DIRECTORY relocations = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC];
    if (!relocations.VirtualAddress || !relocations.Size ||
        !IsReadableRange(reinterpret_cast<uintptr_t>(image) + relocations.VirtualAddress, relocations.Size))
        relocations = {};

    HANDLE disk = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (disk == INVALID_HANDLE_VALUE) {
        std::fprintf(file, "  <cannot open %s (error %lu)>\n", BaseName(path), GetLastError());
        return;
    }
    // The file can have been replaced by an update since it was loaded, and the
    // build timestamp is a fixed value, so the on-disk headers are compared whole.
    uint8_t diskHeaders[0x1000] = {};
    DWORD got = 0;
    uint64_t preferredBase = 0;
    bool sameFile = ReadFile(disk, diskHeaders, sizeof(diskHeaders), &got, nullptr) &&
        got >= size_t(dos->e_lfanew) + sizeof(IMAGE_NT_HEADERS64);
    if (sameFile) {
        const auto* diskNt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(diskHeaders + dos->e_lfanew);
        preferredBase = diskNt->OptionalHeader.ImageBase;
        sameFile = diskNt->Signature == IMAGE_NT_SIGNATURE &&
            diskNt->FileHeader.NumberOfSections == sectionCount &&
            diskNt->OptionalHeader.SizeOfImage == nt->OptionalHeader.SizeOfImage &&
            diskNt->OptionalHeader.CheckSum == nt->OptionalHeader.CheckSum &&
            size_t(dos->e_lfanew) + sizeof(IMAGE_NT_HEADERS64) + sectionCount * sizeof(IMAGE_SECTION_HEADER) <= got &&
            std::memcmp(IMAGE_FIRST_SECTION(diskNt), sections, sectionCount * sizeof(IMAGE_SECTION_HEADER)) == 0;
    }
    if (!sameFile) {
        std::fprintf(file, "  <%s on disk is not the image that is loaded; not compared>\n", BaseName(path));
        CloseHandle(disk);
        return;
    }
    const uint64_t delta = reinterpret_cast<uintptr_t>(image) - preferredBase;

    constexpr size_t kMaxRuns = 24, kRunBytes = 16;
    for (size_t s = 0; s < sectionCount; ++s) {
        const IMAGE_SECTION_HEADER& section = sections[s];
        if (!(section.Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;
        const uint32_t size = std::min(section.Misc.VirtualSize, section.SizeOfRawData);
        char name[9] = {};
        std::memcpy(name, section.Name, 8);
        auto* fromDisk = static_cast<uint8_t*>(
            VirtualAlloc(nullptr, size_t(size) * 2, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
        if (!fromDisk) {
            std::fprintf(file, "  %s: <no memory for the comparison>\n", name);
            continue;
        }
        uint8_t* fromMemory = fromDisk + size;
        SIZE_T copied = 0;
        LARGE_INTEGER position{};
        position.QuadPart = section.PointerToRawData;
        got = 0;
        if (!SetFilePointerEx(disk, position, nullptr, FILE_BEGIN) ||
            !ReadFile(disk, fromDisk, size, &got, nullptr) || got != size) {
            std::fprintf(file, "  %s: <file read failed (error %lu)>\n", name, GetLastError());
        } else if (!ReadProcessMemory(GetCurrentProcess(), image + section.VirtualAddress, fromMemory,
                                      size, &copied) || copied != size) {
            std::fprintf(file, "  %s: <loaded code unreadable>\n", name);
        } else {
            size_t runs = 0, changedBytes = 0;
            for (uint32_t i = 0; i < size;) {
                if (fromDisk[i] == fromMemory[i]) { ++i; continue; }
                // A relocated address differs by exactly the load offset.
                bool relocated = false;
                for (uint32_t start = i >= 7 ? i - 7 : 0; start <= i && start + 8 <= size; ++start) {
                    uint64_t before = 0, after = 0;
                    std::memcpy(&before, fromDisk + start, 8);
                    std::memcpy(&after, fromMemory + start, 8);
                    if (before + delta == after && relocations.Size &&
                        HasBaseRelocationAt(image, relocations, section.VirtualAddress + start)) {
                        i = start + 8;
                        relocated = true;
                        break;
                    }
                }
                if (relocated) continue;
                uint32_t end = i;
                while (end < size && end - i < kRunBytes && fromDisk[end] != fromMemory[end]) ++end;
                changedBytes += end - i;
                if (runs++ < kMaxRuns) {
                    // Shown with the bytes before it, enough to find the instruction.
                    const uint32_t from = i >= 8 ? i - 8 : 0, to = std::min(size, end + 8);
                    std::fprintf(file, "  %s+0x%X (module+0x%X) changed bytes %u, shown from -0x%X\n    file  :",
                                 name, i, unsigned(section.VirtualAddress + i), end - i, i - from);
                    for (uint32_t b = from; b < to; ++b) std::fprintf(file, " %02X", fromDisk[b]);
                    std::fprintf(file, "\n    memory:");
                    for (uint32_t b = from; b < to; ++b) std::fprintf(file, " %02X", fromMemory[b]);
                    std::fprintf(file, "\n");
                }
                i = end;
            }
            if (!runs) std::fprintf(file, "  %s: 0x%X bytes identical\n", name, size);
            else std::fprintf(file, "  %s: %llu changed bytes in %llu places (%llu shown)\n", name,
                              static_cast<unsigned long long>(changedBytes),
                              static_cast<unsigned long long>(runs),
                              static_cast<unsigned long long>(std::min(runs, kMaxRuns)));
        }
        VirtualFree(fromDisk, 0, MEM_RELEASE);
    }
    CloseHandle(disk);
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

void WriteObjectVirtualSlot(FILE* file, uintptr_t object, uintptr_t offset)
{
    uintptr_t vtable = 0;
    uintptr_t target = 0;
    if (!SafeReadPointer(object, &vtable) ||
        vtable > UINTPTR_MAX - offset ||
        !SafeReadPointer(vtable + offset, &target)) {
        return;
    }

    std::fprintf(file, " vtable=0x%016llX vtable+0x%llX=0x%016llX executable=%d",
        static_cast<unsigned long long>(vtable),
        static_cast<unsigned long long>(offset),
        static_cast<unsigned long long>(target),
        IsExecutableAddress(target) ? 1 : 0);
}

void WriteNameEqualsFaultDetails(FILE* file, const CONTEXT* context)
{
#if defined(_M_X64) || defined(__x86_64__)
    const uintptr_t module = reinterpret_cast<uintptr_t>(GetModuleHandleA(nullptr));
    if (!module || module > UINTPTR_MAX - (0x14000418C - kPreferredImageBase) ||
        context->Rip != module + (0x14000418C - kPreferredImageBase)) {
        return;
    }

    char comparedName[128] = {};
    const bool hasComparedName = SafeCopyString(
        context->Rdx, comparedName, sizeof(comparedName));
    std::fprintf(file, "name_equals_fault: this=0x%016llX",
        static_cast<unsigned long long>(context->Rcx));
    if (hasComparedName)
        std::fprintf(file, " compare=\"%s\"", comparedName);
    else
        std::fprintf(file, " compare_ptr=0x%016llX",
            static_cast<unsigned long long>(context->Rdx));
    WriteObjectVirtualSlot(file, context->Rcx, 0x68);
    std::fprintf(file, "\n");
#else
    (void)file;
    (void)context;
#endif
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
        if (hasDtiName)
            WriteObjectVirtualSlot(file, values[i].value, 0x68);
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

void WriteStackObjectCandidates(FILE* file, const CONTEXT* context,
                                const CrashCharacter* characters,
                                size_t characterCount)
{
#if defined(_M_X64) || defined(__x86_64__)
    constexpr size_t stackEntries = 96;
    constexpr size_t maxObjects = 32;
    std::fprintf(file, "stack scan (named MT objects):\n");
    if (!IsReadableRange(context->Rsp, stackEntries * sizeof(uintptr_t))) {
        std::fprintf(file, "  <stack unreadable>\n");
        return;
    }

    const auto* stack = reinterpret_cast<const uintptr_t*>(context->Rsp);
    uintptr_t reported[maxObjects] = {};
    size_t reportedCount = 0;
    for (size_t i = 0; i < stackEntries && reportedCount < maxObjects; ++i) {
        const uintptr_t value = stack[i];
        if (value < 0x10000 ||
            !IsReadableRange(value, sizeof(uintptr_t))) {
            continue;
        }

        bool duplicate = false;
        for (size_t previous = 0; previous < reportedCount; ++previous)
            duplicate |= reported[previous] == value;
        if (duplicate)
            continue;

        char dtiName[96] = {};
        const bool hasDtiName = TryGetObjectDtiName(
            value, dtiName, sizeof(dtiName));
        uintptr_t interiorOffset = 0;
        const int characterIndex = FindLoadedCharacter(
            characters, characterCount, value, &interiorOffset);
        if (!hasDtiName && characterIndex < 0)
            continue;

        reported[reportedCount++] = value;
        std::fprintf(file, "  RSP+0x%03llX value=0x%016llX",
            static_cast<unsigned long long>(i * sizeof(uintptr_t)),
            static_cast<unsigned long long>(value));
        if (hasDtiName)
            std::fprintf(file, " dti=\"%s\"", dtiName);
        if (hasDtiName)
            WriteObjectVirtualSlot(file, value, 0x68);
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
    }
    if (reportedCount == 0)
        std::fprintf(file, "  <no named MT object found>\n");
#else
    (void)context;
    (void)characters;
    (void)characterCount;
    std::fprintf(file, "stack scan (named MT objects): <unsupported architecture>\n");
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

    // An indirect call through null records RIP=0, but RSP still points at the
    // caller's return address. Seed unwinding from that address instead of
    // emitting an empty backtrace for one of the most useful crash signatures.
    if (!context.Rip) {
        if (!IsReadableRange(context.Rsp, sizeof(uintptr_t)))
            return;
        const uintptr_t caller =
            *reinterpret_cast<const uintptr_t*>(context.Rsp);
        if (!IsExecutableAddress(caller))
            return;
        context.Rip = caller;
        context.Rsp += sizeof(uintptr_t);
    }

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

// Runs on the crashing thread after the user dismisses the crash dialog, so
// the launch must finish before the handler returns and the process dies.
void OpenCrashLog(const char* crashLogPath)
{
    char path[MAX_PATH] = {};
    const DWORD length = GetFullPathNameA(crashLogPath, MAX_PATH, path, nullptr);
    if (length == 0 || length >= MAX_PATH)
        return;

    SHELLEXECUTEINFOA open = {};
    open.cbSize = sizeof(open);
    open.fMask = SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI;
    open.lpVerb = "open";
    open.lpFile = path;
    open.nShow = SW_SHOWNORMAL;
    if (ShellExecuteExA(&open))
        return;

    // No usable .log association: fall back to Notepad directly.
    char command[MAX_PATH + 32] = {};
    std::snprintf(command, sizeof(command), "notepad.exe \"%s\"", path);
    STARTUPINFOA startup = {};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process = {};
    if (CreateProcessA(nullptr, command, nullptr, nullptr, FALSE, 0, nullptr,
                       nullptr, &startup, &process)) {
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
    }
}

LONG HandleException(PEXCEPTION_POINTERS info, bool catastrophicOnly, const char* reason = nullptr)
{
    if (!info || !info->ExceptionRecord || !info->ContextRecord)
        return EXCEPTION_CONTINUE_SEARCH;

    const DWORD code = info->ExceptionRecord->ExceptionCode;
    if (catastrophicOnly && !IsCatastrophicException(code))
        return EXCEPTION_CONTINUE_SEARCH;
    if (IsNetGalleryTeardownFault(info->ExceptionRecord, info->ContextRecord))
        return EXCEPTION_CONTINUE_SEARCH;
    if (InterlockedExchange(&g_crashInFlight, 1) != 0)
        return EXCEPTION_CONTINUE_SEARCH;

    const EXCEPTION_RECORD* exception = info->ExceptionRecord;
    const CONTEXT* context = info->ContextRecord;
    char crashLogPath[MAX_PATH] = {};
    char outputDirectory[MAX_PATH] = {};
    if (Netplay_IsActive() &&
        Rollback_GetNetplayOutputDirectory(outputDirectory,
                                           sizeof(outputDirectory))) {
        snprintf(crashLogPath, sizeof(crashLogPath), "%s/umvc3_crash.log",
                 outputDirectory);
    } else if (Rollback_ResolveDllPath(crashLogPath, sizeof(crashLogPath),
                                      "umvc3_crash.log") < 0) {
        GetFullPathNameA("umvc3_crash.log", MAX_PATH, crashLogPath, nullptr);
    }

    // Commit a minimal artifact before calling rollback, CRT-heavy, or unwind
    // diagnostics. If reporting itself faults, this still identifies the
    // original exception and makes automated runs classify it as a crash.
    long crashLogStart = -1;
    FILE* preamble = std::fopen(crashLogPath, "a");
    if (preamble) {
        if (std::fseek(preamble, 0, SEEK_END) == 0)
            crashLogStart = std::ftell(preamble);
        std::fprintf(preamble,
            "\n=== UMVC3Hook crash capture started ===\n"
            "TIMESTONE_VERSION=%s\n"
            "reason=%s\n"
            "commit=%s%s code=0x%08lX exception=%p thread=%lu "
            "rip=0x%016llX rsp=0x%016llX\n",
            UMVC3HOOK_VERSION,
            reason ? reason : "Windows exception",
            UMVC3HOOK_GIT_COMMIT, UMVC3HOOK_GIT_DIRTY ? " (dirty)" : "",
            static_cast<unsigned long>(code),
            info->ExceptionRecord->ExceptionAddress,
            static_cast<unsigned long>(GetCurrentThreadId()),
            static_cast<unsigned long long>(info->ContextRecord->Rip),
            static_cast<unsigned long long>(info->ContextRecord->Rsp));
        std::fflush(preamble);
        std::fclose(preamble);
    }

    FILE* file = std::fopen(crashLogPath, "a");
    if (file) {
        // Preserve completed writes if a later diagnostic faults before fclose.
        std::setvbuf(file, nullptr, _IONBF, 0);
        SYSTEMTIME time{};
        GetLocalTime(&time);
        std::fprintf(file,
            "\n=== UMVC3Hook crash %04u-%02u-%02u %02u:%02u:%02u.%03u ===\n"
            "TIMESTONE_VERSION=%s\n"
            "commit=%s%s\n"
            "reason=%s\n"
            "code=0x%08lX exception=%p thread=%lu\n",
            time.wYear, time.wMonth, time.wDay, time.wHour, time.wMinute,
            time.wSecond, time.wMilliseconds, UMVC3HOOK_VERSION, UMVC3HOOK_GIT_COMMIT,
            UMVC3HOOK_GIT_DIRTY ? " (dirty)" : "",
            reason ? reason : "Windows exception",
            static_cast<unsigned long>(code),
            exception->ExceptionAddress, static_cast<unsigned long>(GetCurrentThreadId()));
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
        WriteNameEqualsFaultDetails(file, context);
        CrashCharacter loadedCharacters[kMaxLoadedCharacters] = {};
        const size_t loadedCharacterCount = CollectLoadedCharacters(
            loadedCharacters, kMaxLoadedCharacters);
        WriteLoadedCharacters(file, loadedCharacters, loadedCharacterCount);
        WriteCrashObjectCandidates(file, context, loadedCharacters,
                                    loadedCharacterCount);
        WriteUnwindBacktrace(file, context);
        WriteStackObjectCandidates(file, context, loadedCharacters,
                                   loadedCharacterCount);
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
        WriteRawThreadState(file, context);
        WriteRealStackWhenPivoted(file, context);
        WriteOwnCodeIntegrity(file);
        HookLogWriteRecent(file);
        RollbackUnit_WritePending(file);
        WriteLoadedModules(file);
        std::fprintf(file, "=== end crash ===\n");
        std::fflush(file);
        std::fclose(file);
    }

    HookLogAppendFile(crashLogPath, crashLogStart);
    HookLogTryFlush();

    if (!IsAutoTestProcess()) {
        char message[MAX_PATH + 96] = {};
        std::snprintf(message, sizeof(message),
            "The game just freaking crashed! Crash log available at %s", crashLogPath);
        MessageBoxA(nullptr, message, "UMVC3Hook Crash",
                    MB_OK | MB_ICONERROR | MB_SYSTEMMODAL);
        OpenCrashLog(crashLogPath);
    } else if (!reason) {
        // An abort may originate while HookLog owns its lock. The report has
        // already been written and appended using the nonblocking path above.
        HookLog("UMVC3Hook: automated crash recorded at %s; message box suppressed\n",
                crashLogPath);
    }

    return EXCEPTION_CONTINUE_SEARCH;
}

void AbortSignalHandler(int signal)
{
    CONTEXT context{};
    RtlCaptureContext(&context);
    EXCEPTION_RECORD exception{};
    exception.ExceptionCode = 0x40000015; // STATUS_FATAL_APP_EXIT
    exception.ExceptionFlags = EXCEPTION_NONCONTINUABLE;
    exception.ExceptionAddress = reinterpret_cast<void*>(context.Rip);
    exception.NumberParameters = 1;
    exception.ExceptionInformation[0] = static_cast<ULONG_PTR>(signal);
    EXCEPTION_POINTERS info{&exception, &context};
    HandleException(&info, false, "SIGABRT (std::abort/CRT termination)");
    // Returning to abort still terminates. Do not run game/rollback teardown:
    // the failed invariant can hold locks or leave an incomplete transaction.
}

LONG CALLBACK VectoredExceptionHandler(PEXCEPTION_POINTERS info)
{
    if (ResumeGuardedCall(info)) return EXCEPTION_CONTINUE_EXECUTION;
    return HandleException(info, true);
}

LONG WINAPI UnhandledExceptionHandler(PEXCEPTION_POINTERS info)
{
    // Reaching the top-level filter proves this exception is fatal even when
    // its code is not in the first-chance catastrophic allowlist.
    const LONG result = HandleException(info, false);
    PrepareForGameExit();
    return result;
}

} // namespace

__attribute__((noinline)) bool Exception_RunGuarded(void (*function)(void*), void* argument)
{
    // Without the handler (or nested in another guarded call) a fault is an
    // ordinary crash, as before.
    if (!g_exceptionHandler || g_guardSlot == TLS_OUT_OF_INDEXES ||
        TlsGetValue(g_guardSlot)) {
        function(argument);
        return true;
    }
    void* buffer[5];
    if (__builtin_setjmp(buffer)) return false;
    TlsSetValue(g_guardSlot, buffer);
    function(argument);
    TlsSetValue(g_guardSlot, nullptr);
    return true;
}

void Exception_InstallHandler()
{
    if (g_exceptionHandler || g_unhandledExceptionFilterInstalled || g_abortHandlerInstalled)
        return;

    g_crashInFlight = 0;
    if (g_guardSlot == TLS_OUT_OF_INDEXES) g_guardSlot = TlsAlloc();
    g_exceptionHandler = AddVectoredExceptionHandler(1, VectoredExceptionHandler);
    g_previousUnhandledExceptionFilter =
        SetUnhandledExceptionFilter(UnhandledExceptionHandler);
    g_unhandledExceptionFilterInstalled = true;
    g_previousAbortHandler = std::signal(SIGABRT, AbortSignalHandler);
    g_abortHandlerInstalled = g_previousAbortHandler != SIG_ERR;
    if (!g_exceptionHandler || !g_abortHandlerInstalled)
        HookLog("UMVC3Hook: crash handlers vectored=%s unhandled=installed abort=%s\n",
            g_exceptionHandler ? "installed" : "FAILED",
            g_abortHandlerInstalled ? "installed" : "FAILED");
}

void Exception_RemoveHandler()
{
    if (g_abortHandlerInstalled) {
        std::signal(SIGABRT, g_previousAbortHandler);
        g_previousAbortHandler = SIG_DFL;
        g_abortHandlerInstalled = false;
    }
    if (g_exceptionHandler) {
        RemoveVectoredExceptionHandler(g_exceptionHandler);
        g_exceptionHandler = nullptr;
    }
    if (g_unhandledExceptionFilterInstalled) {
        SetUnhandledExceptionFilter(g_previousUnhandledExceptionFilter);
        g_previousUnhandledExceptionFilter = nullptr;
        g_unhandledExceptionFilterInstalled = false;
    }
}

void Exception_UpdateTestHotkey()
{
    if (KeyPressed(VK_F4)) {
        const ULONG_PTR details[] = {
            1, // Write access.
            0  // Address that would have been written.
        };
        RaiseException(EXCEPTION_ACCESS_VIOLATION, EXCEPTION_NONCONTINUABLE,
            static_cast<DWORD>(sizeof(details) / sizeof(details[0])), details);
    }
}
