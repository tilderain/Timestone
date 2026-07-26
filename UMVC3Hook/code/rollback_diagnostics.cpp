#include "../pch.h"

#include "rollback_diagnostics.h"

#include <cstdio>
#include <cstring>

EXTERN_C IMAGE_DOS_HEADER __ImageBase;

int Rollback_ResolveDllPath(char* destination, size_t destinationLength,
                            const char* relativePath)
{
    if (!destination || destinationLength == 0) return -1;

    char modulePath[260];
    DWORD result = GetModuleFileNameA((HINSTANCE)&__ImageBase, modulePath,
                                      sizeof(modulePath));
    if (result == 0 || result >= sizeof(modulePath)) {
        destination[0] = '\0';
        return -1;
    }

    char* separator = strrchr(modulePath, '\\');
    if (separator)
        *(separator + 1) = '\0';
    else
        modulePath[0] = '\0';
    return snprintf(destination, destinationLength, "%s%s", modulePath, relativePath);
}

uint64_t Rollback_Fnv1a64(const void* data, size_t length)
{
    uint64_t hash = 0xCBF29CE484222325ULL;
    const uint8_t* bytes = (const uint8_t*)data;
    for (size_t i = 0; i < length; ++i) {
        hash ^= bytes[i];
        hash *= 0x100000001B3ULL;
    }
    return hash;
}
