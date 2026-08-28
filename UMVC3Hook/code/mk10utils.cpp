#include "../pch.h"
#include "mk10utils.h"
#include "../sigscan.h"

uintptr_t g_umvc3Base = 0;

void InitializeGameAddressBase()
{
	g_umvc3Base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
}

unsigned long long start = 0;
unsigned long long _adjust(unsigned long long addr) {
	if (start == 0) {
		start = sigscan::get().start;
	}
	return start + addr;
}
