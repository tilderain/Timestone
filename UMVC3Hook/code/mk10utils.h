#pragma once
#include <cstdint>
#include <cstddef>
typedef int64_t int64;


int64 GetMKXEntryPoint();
int64 _addr(int64_t addr);
unsigned long long baseaddr(unsigned long long addr);
unsigned long long _adjust(unsigned long long addr);