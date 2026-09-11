#pragma once
#include <cstdint>
#include <cstddef>
typedef int64_t int64;

inline constexpr uintptr_t UMVC3_PREFERRED_BASE = 0x140000000ull;
extern uintptr_t g_umvc3Base;

void InitializeGameAddressBase();

inline int64 GetMKXEntryPoint()
{
	return static_cast<int64>(g_umvc3Base);
}

// Supported game executables have a fixed image base and stripped relocations.
inline constexpr int64 _addr(int64_t addr)
{
	return addr;
}

inline unsigned long long baseaddr(unsigned long long addr)
{
	return g_umvc3Base + addr;
}

unsigned long long _adjust(unsigned long long addr);
