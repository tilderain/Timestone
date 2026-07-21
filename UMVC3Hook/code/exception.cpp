#include "../pch.h"
#include "exception.h"
#include "netplay.h"
#include "rollback.h"

#include <cstdint>
#include <cstdio>
#include <cstring>

extern void HookLog(const char* fmt, ...);

#ifndef UMVC3HOOK_GIT_COMMIT
#define UMVC3HOOK_GIT_COMMIT "unknown"
#endif

namespace {

PVOID g_exceptionHandler = nullptr;
volatile LONG g_crashInFlight = 0;

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

LONG WINAPI ExceptionHandler(PEXCEPTION_POINTERS info)
{
    if (!info || !info->ExceptionRecord || !info->ContextRecord)
        return EXCEPTION_CONTINUE_SEARCH;

    const DWORD code = info->ExceptionRecord->ExceptionCode;
    if (!IsCatastrophicException(code))
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

    FILE* file = std::fopen(crashLogPath, "a");
    if (file) {
        SYSTEMTIME time{};
        GetLocalTime(&time);
        std::fprintf(file,
            "\n=== UMVC3Hook crash %04u-%02u-%02u %02u:%02u:%02u.%03u ===\n"
            "commit=%s\n"
            "code=0x%08lX exception=%p thread=%lu\n",
            time.wYear, time.wMonth, time.wDay, time.wHour, time.wMinute,
            time.wSecond, time.wMilliseconds, UMVC3HOOK_GIT_COMMIT,
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
        std::fprintf(file, "=== end crash ===\n");
        std::fflush(file);
        std::fclose(file);
    }

    char message[MAX_PATH + 96] = {};
    std::snprintf(message, sizeof(message),
        "The game just freaking crashed! Crash log available at %s", crashLogPath);
    MessageBoxA(nullptr, message, "UMVC3Hook Crash", MB_OK | MB_ICONERROR | MB_SYSTEMMODAL);

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
